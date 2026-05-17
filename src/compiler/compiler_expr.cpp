// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 表达式编译实现
// 
// 本文件包含所有表达式类型的编译逻辑:
// - 字面量表达式 (整数、浮点数、布尔值、字符串)
// - 算术表达式 (加减乘除取模)
// - 位运算表达式 (与或异或左移右移)
// - 比较表达式 (相等、不等、大小比较)
// - 括号表达式
// - 函数调用表达式
// - 成员访问表达式
// - if-else 表达式
// - 数组表达式
// - 一元表达式 (取负、取反、取引用)

#include "compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/enum_node.h"
#include "compiler_runtime.h"
#include "ast/mangler.h"
#include "ast/yux.h"
#include "analyzer/symbol_suggest.h"
#include "analyzer/draft_impl_checker.h"
#include "analyzer/draft_registry.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <set>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <cassert>

// ==================== 辅助函数 ====================

// Phase 2.4 Sema/Codegen 拆分：codegen 读类型的统一入口。详见 compiler.h 注释。
// 已切换的调用点（先窄后宽）：
//   - compileExpr 入口 dispatch 后的 recordTemp / typeNeedsDestructor 三处用例
//     （call / array literal / enum ctor 分支）。这三个分支的 compile<Foo>Expr
//     已按 2.2 在入口写过 resolvedType，回到主 switch 时一定可读。
// 其余 compile<Foo>Expr 内部对 node->getType() 的现地复读保持原样，留待 Phase 3
// 按子系统迁移到 SemaPass 时统一切换。
TypeInfo Compiler::resolvedOrInferredType(p<ExprNode> node) const {
    if (node->hasResolvedType()) {
#ifndef NDEBUG
        const auto& resolved = node->resolvedType();
        auto inferred = node->getType();
        // 透明 alias (spec §3.9.1.2 / §3.9.3.1): SemaPass 缓存的 resolvedType 与 codegen
        // 阶段 getType() 重新计算的结果, 在字面上可能一侧是别名名 (`IPair`), 另一侧已被
        // 解开 (`(i32,i32)`). 两者按 alias 归一后应一致; 仅当归一后仍不等才视为真冲突.
        assert(resolveAlias(resolved) == resolveAlias(inferred) && "resolvedType / getType inconsistent");
#endif
        return node->resolvedType();
    }
    return node->getType();
}

// Phase 3.4.f.2: parseIntLiteral 已整体迁到 `sema::parseIntLiteral`
// (src/sema/call_resolve.cpp). E3103 由 sema 抢先抛 (kMigratedCodes 内,
// SemaPass.visitExpr ExprLiteralNode 分支主动调用), Compiler 端调用是幂等防御性双跑.

// ==================== 数组初始化表达式编译 ====================

// 编译数组填充表达式 ([value ... Type] 语法)
// 使用指定值填充整个数组
llvm::Value* Compiler::compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType, llvm::Value* destPtr) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto literal = node->value();
    auto literalType = literal->getType();
    auto text = literal->getValue().getText();

    // 确定元素类型
    TypeInfo elementType;
    if (node->explicitType()) {
        elementType = node->explicitType()->getType();
        // Phase 3.4.f.1: E3009 已在 ExprArrayInitNode::getType 抛 (kMigratedCodes
        // 命中, SemaPass 顶部 setResolvedType 自动重抛), 此处不可达; 保留作幂等
        // 防御性双跑.
        if (literalType != elementType) {
            throw YuxError(node->getLineNumber(), node->getColumn(),
                ErrorCode::E3009, literalType.name, elementType.name);
        }
    } else {
        elementType = literalType;
    }

    // 验证元素类型与目标数组类型匹配
    if (targetType.elementType && *targetType.elementType != elementType) {
        throw YuxError(node->getLineNumber(), node->getColumn(),
            ErrorCode::E3010, targetType.elementType->name, elementType.name);
    }

    DEBUG_LOG_VAL("    Expr: ArrayInit", targetType.name);

    auto llvmArrayType = getLLVMType(targetType);
    bool needLoad = (destPtr == nullptr);
    if (needLoad) {
        destPtr = _builder.CreateAlloca(llvmArrayType, nullptr, "array.init");
    }

    // 解析填充值
    llvm::Value* fillValue;
    bool isZeroFill = false;
    i64 intFillVal = 0;
    f64 floatFillVal = 0.0;

    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        intFillVal = sema::parseIntLiteral(text, node->getLineNumber(), node->getColumn());
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), intFillVal, true);
        isZeroFill = (intFillVal == 0);  // 零值优化
    } else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        // FLOAT 词法可能含科学计数法 (e[-]?\d+) 与类型后缀 f32/f64
        // 仅剥掉后缀，其余交给 stod
        string numStr = text;
        if (numStr.size() >= 3) {
            string suf = numStr.substr(numStr.size() - 3);
            if (suf == "f32" || suf == "f64") {
                numStr = numStr.substr(0, numStr.size() - 3);
            }
        }
        floatFillVal = stod(numStr);
        fillValue = llvm::ConstantFP::get(getLLVMType(elementType), floatFillVal);
        isZeroFill = (floatFillVal == 0.0);  // 零值优化
    } else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        intFillVal = boolVal ? 1 : 0;
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), intFillVal, false);
        isZeroFill = !boolVal;  // false 值优化
    } else {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3081);
    }

    // 填充数组
    if (isZeroFill) {
        // 零值优化: 使用 LLVM 的零初始化
        auto zeroInit = llvm::ConstantAggregateZero::get(llvmArrayType);
        _builder.CreateStore(zeroInit, destPtr);
    } else if (elementType.name == "i8" || elementType.name == "u8" || elementType.name == "bool") {
        // 字节类型优化: 使用 memset
        auto size = llvm::ConstantInt::get(_builder.getInt64Ty(), targetType.arraySize);
        auto fillByte = _builder.getInt8(static_cast<u8>(intFillVal));
        _builder.CreateMemSetInline(destPtr, llvm::MaybeAlign(1), fillByte, size);
    } else {
        // 通用情况: 逐元素填充
        for (u64 i = 0; i < targetType.arraySize; ++i) {
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
            llvm::Value* indices[] = {zero, index};
            auto elemPtr = _builder.CreateGEP(llvmArrayType, destPtr, indices, "array.elem.ptr");
            _builder.CreateStore(fillValue, elemPtr);
        }
    }

    if (needLoad) {
        return _builder.CreateLoad(llvmArrayType, destPtr, "array.load");
    }
    return nullptr;
}

// ==================== 类型转换 ====================

// 创建类型转换
// 处理整数、浮点数、指针、引用等类型之间的转换
llvm::Value* Compiler::createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType) {
    // 相同类型无需转换
    if (srcType == dstType) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType.name);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType.name << " -> " << dstType.name);

    // 指针类型转换
    if (dstType.isPtr()) {
        if (srcType.isRef()) {
            DEBUG_LOG("      Ref -> Ptr");
            return _builder.CreateBitCast(val, llvm::PointerType::get(_context, 0), "ref_to_ptr");
        }
        if (srcType.isRc()) {
            DEBUG_LOG("      Rc -> Ptr");
            // Rc -> Ptr：取 payload 首地址（DRAFT §9.4 跳过 RC 头）
            // payload = handle + 8
            auto rcStructType = getLLVMType(srcType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, val, {zero, zero}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }
    }

    // 数值类型转换
    auto dstLLVMType = getLLVMType(dstType);
    bool srcIsFloat = srcType.startsWith('f');
    bool dstIsFloat = dstType.startsWith('f');
    bool srcIsUnsigned = srcType.startsWith('u');
    bool dstIsUnsigned = dstType.startsWith('u');

    if (srcIsFloat && dstIsFloat) {
        // 浮点数之间的转换
        if (srcType.name == "f64" && dstType.name == "f32") {
            DEBUG_LOG("      FPTrunc (f64 -> f32)");
            return _builder.CreateFPTrunc(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPExt (f32 -> f64)");
            return _builder.CreateFPExt(val, dstLLVMType);
        }
    } else if (!srcIsFloat && !dstIsFloat) {
        // 整数之间的转换
        auto srcLLVMType = getLLVMType(srcType);
        if (dstLLVMType->getIntegerBitWidth() > srcLLVMType->getIntegerBitWidth()) {
            // 扩展
            if (srcIsUnsigned) {
                DEBUG_LOG("      ZExt (unsigned int extension)");
                return _builder.CreateZExt(val, dstLLVMType);
            } else {
                DEBUG_LOG("      SExt (signed int extension)");
                return _builder.CreateSExt(val, dstLLVMType);
            }
        } else {
            // 截断
            DEBUG_LOG("      Trunc (int truncation)");
            return _builder.CreateTrunc(val, dstLLVMType);
        }
    } else if (!srcIsFloat && dstIsFloat) {
        // 整数转浮点数
        if (srcIsUnsigned) {
            DEBUG_LOG("      UIToFP (unsigned int to float)");
            return _builder.CreateUIToFP(val, dstLLVMType);
        } else {
            DEBUG_LOG("      SIToFP (signed int to float)");
            return _builder.CreateSIToFP(val, dstLLVMType);
        }
    } else {
        // 浮点数转整数
        if (dstIsUnsigned) {
            DEBUG_LOG("      FPToUI (float to unsigned int)");
            return _builder.CreateFPToUI(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPToSI (float to signed int)");
            return _builder.CreateFPToSI(val, dstLLVMType);
        }
    }
}

// ==================== 字面量表达式编译 ====================

// 编译字面量表达式
// 处理整数、浮点数、布尔值、对象名等
llvm::Value* Compiler::compileLiteralExpr(p<ExprLiteralNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto literal = node->literal();
    auto type = literal->getType();
    auto text = literal->getValue().getText();

    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        i64 numVal = sema::parseIntLiteral(text, node->getLineNumber(), node->getColumn());
        DEBUG_LOG_VAL("    Expr: IntLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), numVal, true);
    } else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        // FLOAT 词法可能含科学计数法 (e[-]?\d+) 与类型后缀 f32/f64
        // 仅剥掉后缀，其余交给 stod
        string numStr = text;
        if (numStr.size() >= 3) {
            string suf = numStr.substr(numStr.size() - 3);
            if (suf == "f32" || suf == "f64") {
                numStr = numStr.substr(0, numStr.size() - 3);
            }
        }
        f64 numVal = stod(numStr);
        DEBUG_LOG_VAL("    Expr: FloatLiteral", text << " : " << type.name);
        return llvm::ConstantFP::get(getLLVMType(type), numVal);
    } else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        DEBUG_LOG_VAL("    Expr: BoolLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), boolVal ? 1 : 0, false);
    } else if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal)) {
        auto varName = text;
        // Phase 2b：lambda body 编译期 _currentFnNode 为 nullptr，但 body 节点的 parent
        // 链能经 bodyScope 找到 lambda 形参；fall back 到 findNearestScope 让 lambda 形参
        // 与外层局部都能查到（外层情况下两者等价）
        SymbolInfo* sym = nullptr;
        if (_currentFnNode) {
            sym = _currentFnNode->lookupSymbol(varName);
        }
        if (!sym) {
            if (auto sc = node->findNearestScope()) {
                sym = sc->lookupSymbol(varName);
            }
        }
        // Phase 2.3：解析到的变量符号挂回 AST，供后续 pass（codegen/LSP）复用，
        // 避免 2.4 切读路径前再发生一次 lookupSymbol。
        if (sym) {
            node->setResolvedVar(sym);
        }

        if (sym && _localVarPtrs.contains(varName)) {
            DEBUG_LOG_VAL("    Expr: VariableLoad", varName << " : " << sym->type.name);
            // Phase 4a: T& 借用 — _localVarPtrs[name] 是底层 T 的地址（参数/局部统一），自动解引用
            if (sym->type.isRef()) {
                auto innerType = sym->type.refElementType();
                return _builder.CreateLoad(getLLVMType(*innerType), _localVarPtrs[varName]);
            }
            return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[varName]);
        }

        string ownerMod = (sym && !sym->moduleName.empty()) ? sym->moduleName : _file->moduleName();
        bool globPriv = !varName.empty() && varName[0] == '_';
        string mangledName = Mangler::global(ownerMod, varName, globPriv);
        auto globalVar = _module->getGlobalVariable(mangledName, true);
        if (globalVar) {
            DEBUG_LOG_VAL("    Expr: GlobalConstLoad", varName << " : " << (sym ? sym->type.name : "unknown"));
            return _builder.CreateLoad(globalVar->getValueType(), globalVar, "global.load");
        }

        if (_currentFnNode) {
            SymbolSuggest::throwSymbolNotFound(_currentFnNode,
                node->getLineNumber(), node->getColumn(), ErrorCode::E3030, varName);
        }
        // lambda body 内引用外层 local：Phase 4a / 4a-2 闭包识别（spec §6.1 / §6.2）
        // - 找到 sym 但不在 _localVarPtrs 也无 globalVar → 外层 local
        // - 4a 接收 builtin 标量；4a-2 接收 8-byte 堆句柄包装（Rc / Weak / Array<T> / String）
        // - 其余（struct / enum / Fn fat-ptr / T&）仍报 E2029（4c+ 接入）
        // - 命中：addCapture（首次出现）+ 生成 GEP 读 captures buffer
        // captures Rc payload 布局：[0..8] dtor fn ptr，[8..] capture 字段（4a-2 引入 dtor 槽）
        if (_currentLambdaForCapture && _currentLambdaBodyScope
            && sym && sym->kind == SymbolKind::Variable) {
            const auto& t = sym->type;
            bool isScalar = t.isNormal() && isBuiltinType(t.name);
            bool isHandle = t.isRc() || t.isWeak() || t.isArrayGeneric()
                            || (t.isNormal() && t.name == "String");
            bool isRef = t.isRef();
            if (!isScalar && !isHandle && !isRef) {
                throw YuxError(node->getLineNumber(), node->getColumn(),
                               ErrorCode::E2029, varName, t.name);
            }
            // 已捕获 → 复用槽位；首次 → 追加（每 capture 固定 8 字节槽位）
            int idx = _currentLambdaForCapture->findCapture(varName);
            if (idx < 0) {
                u64 offset = _currentLambdaForCapture->capturesTotalSize();
                idx = _currentLambdaForCapture->addCapture(varName, t, offset, offset + 8);
                if (isRef) {
                    // Phase 4c：标记 lambda 含 T& 捕获，触发栈嵌入路径 + 不可逃逸约束
                    _currentLambdaForCapture->setHasRefCapture(true);
                }
            }
            const auto& cap = _currentLambdaForCapture->captures()[idx];
            // captures arg = block / alloca 句柄；layout 统一保留 16 字节前缀
            // （Rc 形态：[strong/weak 8 字节][dtor 8 字节]；Stack 形态：16 字节占位浪费），
            // capture 字段从 handle+16 起。这样 GEP offset 不依赖运行时 layout 选择。
            auto i8Ty = _builder.getInt8Ty();
            auto payloadOffset = _builder.getInt64(16 + (i64)cap.byteOffset);
            auto capAddr = _builder.CreateGEP(i8Ty, _currentLambdaCapturesArg,
                                              {payloadOffset}, "cap.addr");
            if (isRef) {
                // T& slot 存的是 ptr to inner T；先 load ptr，再 load inner T 实现 auto-deref
                // （与外层 T& 局部读语义对齐：compiler_expr.cpp:296 同源路径）
                auto innerType = t.refElementType();
                auto ptrTy = llvm::PointerType::get(_context, 0);
                auto refPtr = _builder.CreateLoad(ptrTy, capAddr, "cap.refptr");
                return _builder.CreateLoad(getLLVMType(*innerType), refPtr, "cap.load");
            }
            return _builder.CreateLoad(getLLVMType(t), capAddr, "cap.load");
        }
        // 兜底：lambda body 命中 sym 但禁用捕获（_currentLambdaForCapture 未启） → 旧 E2028
        if (_currentLambdaBodyScope && sym && sym->kind == SymbolKind::Variable) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E2028, varName);
        }
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3030, varName);
    } else if (auto cpLiteral = dynamic_cast<LiteralCodePointNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: CodePointLiteral", text << " : u32");
        return llvm::ConstantInt::get(getLLVMType(type), cpLiteral->codePoint(), false);
    } else if (auto nullLiteral = dynamic_cast<LiteralNullNode*>(literal)) {
        DEBUG_LOG("    Expr: NullLiteral");
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0));
    } else if (auto stringLiteral = dynamic_cast<LiteralStringNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: StringLiteral", text);
        return emitStringLiteralValue(stringLiteral->codePoints());
    } else if (auto tplLiteral = dynamic_cast<StringTemplateNode*>(literal)) {
        return compileStringTemplate(tplLiteral);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3080);
}

// 由码点向量发射 .rodata 哨兵 String 值
//
// Phase 1c.1：字面量走 .rodata 哨兵 Block，零启动开销。
// Block 字节布局匹配 Array<T>（compiler_runtime.cpp）：
//   { i32 strong=0xFFFFFFFF, i32 weak=0, i64 len, i64 cap, ptr data }
// strong = 0xFFFFFFFF 让 _array_retain / _array_release 直接跳过；
// String layout 仍是 { data: Array<u32> } = { { ptr handle } }，handle = &block。
//
// LiteralStringNode 与 StringTemplateNode（template parts）共用此发射路径。
llvm::Value* Compiler::emitStringLiteralValue(const vector<u32>& codePoints) {
    size_t len = codePoints.size();

    auto stringType = getLLVMType(TypeInfo("String"));
    auto alloca = _builder.CreateAlloca(stringType, nullptr, "str_tmp");

    auto i32Ty = _builder.getInt32Ty();
    auto i64Ty = _builder.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
    auto i32Zero = llvm::ConstantInt::get(i32Ty, 0);

    // 数据缓冲：len > 0 时铺常量 u32 数组，否则用 null 指针（Block.data）。
    llvm::Constant* dataConst = llvm::ConstantPointerNull::get(ptrTy);
    if (len > 0) {
        auto arrType = llvm::ArrayType::get(i32Ty, len);
        vector<llvm::Constant*> elements;
        elements.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            elements.push_back(llvm::ConstantInt::get(i32Ty, codePoints[i]));
        }
        auto arrInit = llvm::ConstantArray::get(arrType, elements);

        static int strDataCounter = 0;
        string dataName = ".str.data." + to_string(strDataCounter++);
        dataConst = new llvm::GlobalVariable(
            *_module, arrType, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, arrInit, dataName);
    }

    // .rodata Block：32 字节精确匹配 Array Block layout。
    auto blockTy = llvm::StructType::get(_context, {i32Ty, i32Ty, i64Ty, i64Ty, ptrTy});
    auto lenC = llvm::ConstantInt::get(i64Ty, len);
    auto blockInit = llvm::ConstantStruct::get(
        blockTy, {sentinel, i32Zero, lenC, lenC, dataConst});

    // 空字面量共享同一全局，省 .rodata 体积。
    llvm::GlobalVariable* blockGlobal = nullptr;
    if (len == 0) {
        const char* sharedName = ".str.empty.block";
        blockGlobal = _module->getNamedGlobal(sharedName);
        if (!blockGlobal) {
            blockGlobal = new llvm::GlobalVariable(
                *_module, blockTy, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, blockInit, sharedName);
        }
    } else {
        static int strBlockCounter = 0;
        string blockName = ".str.block." + to_string(strBlockCounter++);
        blockGlobal = new llvm::GlobalVariable(
            *_module, blockTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, blockInit, blockName);
    }

    storeArrayHandle(alloca, blockGlobal);
    return _builder.CreateLoad(stringType, alloca, "str_val");
}

// 编译字符串模板（v0.6 Phase 2a / 2b）
// 把 StringTemplateNode lower 为：
//   sb StringBuilder = StringBuilder()
//   for each non-empty part: sb.append(part_literal)
//   for each interp:         sb.append(interp_value)
//   result = sb.build()
//   ~sb                     ; 释放 build() 后留下的空 Array<u32>
//
// Phase 2b：插值非 String 时合成 `interp.to_string()` 走现有方法分发，
// 类型未实现 ToString 时报 E3026。合成节点用临时 unique_ptr 持有，
// compileExpr 返回后立即释放。
llvm::Value* Compiler::compileStringTemplate(StringTemplateNode* node) {
    DEBUG_LOG("    Expr: StringTemplate -> StringBuilder lowering");
    const auto& parts = node->parts();
    const auto& interps = node->interps();

    // Phase 2b 类型校验：插值类型必须是 String 或在 SDK / 当前文件中可解析到
    // `<Type>.to_string()` —— 等价于实现了 ToString。
    auto canToString = [&](const TypeInfo& t) -> bool {
        if (t.name == "String") return true;
        string fullName = t.name + ".to_string";
        if (_yux && _yux->sdkFile()
            && _yux->sdkFile()->lookupFnSymbol(fullName)) {
            return true;
        }
        if (_file && _file->lookupFnSymbol(fullName)) return true;
        return false;
    };
    for (size_t i = 0; i < interps.size(); ++i) {
        auto t = interps[i]->getType();
        if (!canToString(t)) {
            throw YuxError(
                interps[i]->getLineNumber(), interps[i]->getColumn(),
                ErrorCode::E3026, t.name);
        }
    }

    // UTF-8 → u32 码点解码（parts 在 ast_builder 已展开转义，仅含原始 UTF-8 字节）
    auto decodeUtf8 = [](const string& s) -> vector<u32> {
        vector<u32> out;
        for (size_t i = 0; i < s.size(); ) {
            u8 c = static_cast<u8>(s[i]);
            u32 cp = 0;
            if (c < 0x80) {
                cp = c; i += 1;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
                cp = ((c & 0x1F) << 6) | (static_cast<u8>(s[i + 1]) & 0x3F);
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
                cp = ((c & 0x0F) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 6)
                   | (static_cast<u8>(s[i + 2]) & 0x3F);
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
                cp = ((c & 0x07) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 12)
                   | ((static_cast<u8>(s[i + 2]) & 0x3F) << 6) | (static_cast<u8>(s[i + 3]) & 0x3F);
                i += 4;
            } else {
                cp = c; i += 1;
            }
            out.push_back(cp);
        }
        return out;
    };

    // 1. alloca StringBuilder + 调构造
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "tpl_sb");
    {
        vector<TypeInfo> noArgs;
        auto ctorFn = getMethodFunction("StringBuilder", "StringBuilder", noArgs, TypeInfo());
        _builder.CreateCall(ctorFn, {sbPtr});
    }

    // 2. emit sb.append(String) 帮手：普通 struct String 走 by-value 调用约定
    auto emitAppendString = [&](llvm::Value* strVal) {
        vector<TypeInfo> appendParams = {TypeInfo("String")};
        auto appendFn = getMethodFunction("StringBuilder", "append", appendParams, TypeInfo());
        _builder.CreateCall(appendFn, {sbPtr, strVal});
    };

    // 3. 交错追加 parts[i]、interps[i]
    //    Phase 2b：interp 非 String 时合成 `interp.to_string()` 节点，复用方法分发。
    //    合成节点的 _parent 取自 interp（其 parent 是 ScopeNode），保证 findNearestScope 可走到。
    vector<std::unique_ptr<Node>> synthHolder;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].empty()) {
            auto cps = decodeUtf8(parts[i]);
            emitAppendString(emitStringLiteralValue(cps));
        }
        if (i < interps.size()) {
            auto interpExpr = interps[i];
            llvm::Value* strVal;
            if (interpExpr->getType().name == "String") {
                strVal = compileExpr(interpExpr);
            } else {
                Token memberTok("to_string", static_cast<size_t>(interpExpr->getLineNumber()));
                p<Node> synthParent = interpExpr->parent();
                auto dotNode = new ExprDotNode(synthParent, interpExpr, memberTok);
                synthHolder.emplace_back(dotNode);
                auto callNode = new ExprCallNode(synthParent, dotNode);
                synthHolder.emplace_back(callNode);
                strVal = compileExpr(callNode);
            }
            emitAppendString(strVal);
        }
    }

    // 4. sb.build() → String
    vector<TypeInfo> noArgs;
    auto buildFn = getMethodFunction("StringBuilder", "build", noArgs, TypeInfo("String"));
    auto result = _builder.CreateCall(buildFn, {sbPtr}, "tpl_built");

    // 5. SB 析构：释放 build() 后留下的空 Array<u32>（沿用作用域析构口径）
    releaseAtPtr(sbPtr, TypeInfo("StringBuilder"));

    return result;
}

// v0.6 Phase 2c：连续 String `+` 链整链 lower。
//
// 把左结合 `+` 树扁平化为叶子序列（自顶 right 先压、再沿 left 下钻直到 left 不再是
// 「结果为 String 的 Add」），然后用单条 StringBuilder 累加：
//   sb StringBuilder = StringBuilder()
//   for each leaf:
//     if leaf : String  -> sb.append(leaf)
//     else if leaf : ToString -> sb.append(leaf.to_string())
//     else -> E3026
//   result = sb.build()
//   ~sb
//
// 设计要点：
// - 仅扁平化 left spine。`a + (b + c)` 中 `(b + c)` 作为单个右叶子，由 compileExpr 递归
//   处理（若结果仍是 String，会再次进入本函数，独立开一条 SB；可接受，常见写法是左结合）。
// - 扁平化判据：内部节点必须是 ExprAddSubNode + Op::Add + getType().name == "String"。
//   只要任一操作数是 String，ExprAddSubNode::getType() 会返回 String（spec §4.4.1.4）。
llvm::Value* Compiler::compileStringPlusChain(ExprAddSubNode* node) {
    DEBUG_LOG("    Expr: String + chain -> StringBuilder lowering");

    // 1. 扁平化左脊：得到从左到右的叶子序列（shared_ptr，复用 AST 持有的所有权）
    vector<p<ExprNode>> leaves;
    {
        ExprAddSubNode* cur = node;
        while (true) {
            leaves.push_back(cur->right());
            auto leftExpr = cur->left();
            auto* innerAdd = dynamic_cast<ExprAddSubNode*>(leftExpr);
            bool isStringAdd = innerAdd != nullptr
                && innerAdd->op() == ExprAddSubNode::Op::Add
                && innerAdd->getType().name == "String";
            if (isStringAdd) {
                cur = innerAdd;
                continue;
            }
            leaves.push_back(leftExpr);
            break;
        }
        std::reverse(leaves.begin(), leaves.end());
    }

    // 2. 类型校验：每个叶子必须是 String 或实现 ToString
    auto canToString = [&](const TypeInfo& t) -> bool {
        if (t.name == "String") return true;
        string fullName = t.name + ".to_string";
        if (_yux && _yux->sdkFile()
            && _yux->sdkFile()->lookupFnSymbol(fullName)) {
            return true;
        }
        if (_file && _file->lookupFnSymbol(fullName)) return true;
        return false;
    };
    for (const auto& leaf : leaves) {
        auto t = leaf->getType();
        if (!canToString(t)) {
            throw YuxError(
                leaf->getLineNumber(), leaf->getColumn(),
                ErrorCode::E3026, t.name);
        }
    }

    // 3. alloca StringBuilder + 调构造
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "plus_sb");
    {
        vector<TypeInfo> noArgs;
        auto ctorFn = getMethodFunction("StringBuilder", "StringBuilder", noArgs, TypeInfo());
        _builder.CreateCall(ctorFn, {sbPtr});
    }

    // 4. emit sb.append(String) 帮手
    vector<TypeInfo> appendParams = {TypeInfo("String")};
    auto appendFn = getMethodFunction("StringBuilder", "append", appendParams, TypeInfo());

    // 5. 逐叶 append；非 String 合成 `leaf.to_string()`（同 Phase 2b 模板）
    vector<std::unique_ptr<Node>> synthHolder;
    for (const auto& leaf : leaves) {
        llvm::Value* strVal;
        if (leaf->getType().name == "String") {
            strVal = compileExpr(leaf);
        } else {
            Token memberTok("to_string", static_cast<size_t>(leaf->getLineNumber()));
            p<Node> synthParent = leaf->parent();
            auto dotNode = new ExprDotNode(synthParent, leaf, memberTok);
            synthHolder.emplace_back(dotNode);
            auto callNode = new ExprCallNode(synthParent, dotNode);
            synthHolder.emplace_back(callNode);
            strVal = compileExpr(callNode);
        }
        _builder.CreateCall(appendFn, {sbPtr, strVal});
    }

    // 6. sb.build() → String
    vector<TypeInfo> noArgs;
    auto buildFn = getMethodFunction("StringBuilder", "build", noArgs, TypeInfo("String"));
    auto result = _builder.CreateCall(buildFn, {sbPtr}, "plus_built");

    // 7. SB 析构（同模板路径）
    releaseAtPtr(sbPtr, TypeInfo("StringBuilder"));

    return result;
}

// ==================== 自定义类型运算符方法调用 ====================

// 编译自定义类型的二元运算符方法调用
// 将运算符表达式转换为方法调用，如 a + b -> a.plus(b)
llvm::Value* Compiler::compileCustomTypeBinaryOp(
    p<ExprNode> leftExpr, p<ExprNode> rightExpr, const TypeInfo& leftType,
    const string& methodName, int lineNum) {
    
    DEBUG_LOG_VAL("    Expr: CustomTypeBinaryOp", leftType.name << "." << methodName);
    
    // 获取左操作数的指针
    llvm::Value* leftPtr = nullptr;
    if (auto leftLiteral = dynamic_cast<ExprLiteralNode*>(leftExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(leftLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                leftPtr = it->second;
            }
        }
    }
    
    if (!leftPtr) {
        auto leftVal = compileExpr(leftExpr);
        auto structType = getLLVMType(leftType);
        if (!structType) {
            throw YuxError(lineNum, ErrorCode::E3096, leftType.name);
        }
        auto alloca = _builder.CreateAlloca(structType, nullptr, "op_lhs_tmp");
        _builder.CreateStore(leftVal, alloca);
        leftPtr = alloca;
    }
    
    auto rightType = rightExpr->getType();
    // Phase 4b: 右操作数是 T& 字面变量时，从 _localVarPtrs 直接取裸 ptr，
    // 避免 compileExpr 对 ref 自动 load 出 struct 值后又 alloca 写回 —— 写回会
    // 把 struct 看成是 ptr 类型，触发 LLVM 签名校验失败。
    llvm::Value* rightVal = nullptr;
    if (rightType.isRef()) {
        if (auto rl = dynamic_cast<ExprLiteralNode*>(rightExpr)) {
            if (auto ol = dynamic_cast<LiteralObjNode*>(rl->literal())) {
                auto vn = ol->getValue().getText();
                auto rit = _localVarPtrs.find(vn);
                if (rit != _localVarPtrs.end()) rightVal = rit->second;
            }
        }
    }
    if (!rightVal) {
        rightVal = compileExpr(rightExpr);
    }

    // 查找方法（spec §7.2.3.3）：二元运算符方法形参强制 Self&，
    // 因此查表使用 [leftType, Ref<rightType>]，原 eq(other Self) 形态不再被运算符触发。
    // 运算符位置自动取址（spec §7.2.3.6）：右操作数自动包成 Ref，无需用户写 &。
    // Phase 4b: 操作数本身是 T& 时（如 fn 形参 `actual String&`），剥掉一层 Ref
    // 与方法注册的 [Self, Self&] 对齐；不剥则 lookup 失败导致调用方编译期崩溃。
    // v0.6 Phase 2b: 透明类型别名解析，使 `A = i32` 这类别名走到运算符方法时
    // 仍能匹配到 `i32.plus` 等内置方法。
    TypeInfo effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;
    TypeInfo effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;
    effLeftType = applySubst(effLeftType);
    effRightType = applySubst(effRightType);
    string methodFullName = effLeftType.name + "." + methodName;
    TypeInfo rightRefType;
    rightRefType.kind = TypeKind::Generic;
    rightRefType.name = "Ref";
    rightRefType.genericArgs.push_back(make_shared<TypeInfo>(effRightType));
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(effLeftType);
    methodParamTypes.push_back(rightRefType);

    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (!methodSymbol) {
        // 二次探测：用户可能把形参写成按值 Self（与文档表格历史措辞一致），运算符不会被触发。
        // 命中即附 hint，告诉用户改为 Self&。
        vector<TypeInfo> byvalParamTypes;
        byvalParamTypes.push_back(effLeftType);
        byvalParamTypes.push_back(effRightType);
        auto byvalSym = _file->lookupFnSymbolWithParams(methodFullName, byvalParamTypes);
        if (!byvalSym && _yux && _yux->sdkFile()) {
            byvalSym = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, byvalParamTypes);
        }
        const char* opSym =
            methodName == "plus" ? "+" :
            methodName == "minus" ? "-" :
            methodName == "mul" ? "*" :
            methodName == "div" ? "/" :
            methodName == "mod" ? "%" :
            methodName == "and" ? "&" :
            methodName == "or" ? "|" :
            methodName == "xor" ? "^" :
            methodName == "shl" ? "<<" :
            methodName == "shr" ? ">>" :
            methodName == "eq" ? "==" :
            methodName == "ne" ? "!=" :
            methodName == "lt" ? "<" :
            methodName == "le" ? "<=" :
            methodName == "gt" ? ">" :
            methodName == "ge" ? ">=" : methodName.c_str();
        auto err = YuxError(lineNum, ErrorCode::E3073, leftType.name, opSym, methodName);
        if (byvalSym) {
            err.withHint("找到同名方法 `" + methodFullName + "(" + effRightType.name
                         + ")` 但形参按值；运算符重载要求形参类型为 `" + effRightType.name
                         + "&`（见 docs/结构体.md「运算符重载」注意事项 #2）");
        }
        throw err;
    }
    
    // 准备方法参数
    vector<llvm::Value*> methodArgs;
    methodArgs.push_back(leftPtr);
    
    // 检查右操作数是否需要通过指针传递
    auto rightStructDecl = _file->getStructDecl(effRightType.name);
    if (!rightStructDecl && _yux && _yux->sdkFile()) {
        rightStructDecl = _yux->sdkFile()->getStructDecl(effRightType.name);
    }
    if (rightStructDecl && !isBuiltinType(effRightType.name)) {
        // 当右操作数本身是 T&（即 rightVal 已是 ptr）时，直接传 ptr，避免错误的
        // alloca-then-store-into-Struct 路径（structType=Struct，但 rightVal=ptr，类型不匹配）
        if (rightType.isRef()) {
            methodArgs.push_back(rightVal);
        } else {
            auto structType = getLLVMType(effRightType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_rhs_tmp");
            _builder.CreateStore(rightVal, alloca);
            methodArgs.push_back(alloca);
        }
    } else {
        methodArgs.push_back(rightVal);
    }
    
    // 获取或创建方法函数
    // mangle 与 LLVM 签名都按 methodSymbol 实际声明的形参类型走（spec §7.2.3.3 后是 Ref<T>），
    // 与 SDK / 用户代码侧定义的方法符号一一对应。
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    TypeInfo declaredRhsType = methodSymbol->params.size() >= 2 ? methodSymbol->params[1] : rightType;
    vector<TypeInfo> argTypes;
    argTypes.push_back(declaredRhsType);
    string mangledName = Mangler::method(ownerMod, effLeftType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        // 形参为 Ref<T> / 非 builtin struct 时按 ptr 传，其余按值
        bool rhsByPtr = declaredRhsType.isRef()
                        || (rightStructDecl && !isBuiltinType(declaredRhsType.name));
        if (rhsByPtr) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            paramTypes.push_back(getLLVMType(declaredRhsType));
        }
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }
    
    return _builder.CreateCall(fn, methodArgs);
}

// 编译自定义类型的一元运算符方法调用
// 将运算符表达式转换为方法调用，如 -a -> a.neg()
llvm::Value* Compiler::compileCustomTypeUnaryOp(
    p<ExprNode> expr, const TypeInfo& type, const string& methodName, int lineNum) {
    
    DEBUG_LOG_VAL("    Expr: CustomTypeUnaryOp", type.name << "." << methodName);
    
    // 获取操作数的指针
    llvm::Value* ptr = nullptr;
    if (auto literal = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                ptr = it->second;
            }
        }
    }
    
    if (!ptr) {
        auto val = compileExpr(expr);
        auto structType = getLLVMType(type);
        auto alloca = _builder.CreateAlloca(structType, nullptr, "op_tmp");
        _builder.CreateStore(val, alloca);
        ptr = alloca;
    }
    
    // 查找方法
    string methodFullName = type.name + "." + methodName;
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(type);
    
    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (!methodSymbol) {
        throw YuxError(lineNum, ErrorCode::E3074,
                       type.name, methodName == "neg" ? "-" :
                       methodName == "inv" ? "~" :
                       methodName == "not" ? "!" : methodName, methodName);
    }
    
    // 获取或创建方法函数
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    vector<TypeInfo> argTypes;
    string mangledName = Mangler::method(ownerMod, type.name, methodName, argTypes, methPriv);
    
    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }
    
    return _builder.CreateCall(fn, {ptr});
}

llvm::Value* Compiler::compileAddSubExpr(p<ExprAddSubNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // v0.6 Phase 2b: 透明别名解析，使 `A = i32` 后 `A + A` 仍走内置算子路径
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr = (node->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
    DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type.name);

    // v0.6 Phase 2c：`+` 表达式整体类型为 String 时，
    // 走 StringBuilder 整链 lower（多段连续 `+` 合并为单次 builder）。
    if (node->op() == ExprAddSubNode::Op::Add && type.name == "String") {
        return compileStringPlusChain(node);
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName = (node->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }
    
    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');

    if (node->op() == ExprAddSubNode::Op::Add) {
        if (isFloat) {
            return _builder.CreateFAdd(left, right);
        }
        return _builder.CreateAdd(left, right);
    } else {
        if (isFloat) {
            return _builder.CreateFSub(left, right);
        }
        return _builder.CreateSub(left, right);
    }
}

llvm::Value* Compiler::compileMulDivModExpr(p<ExprMulDivModNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul: opStr = "*";
        break;
    case ExprMulDivModNode::Op::Div: opStr = "/";
        break;
    case ExprMulDivModNode::Op::Mod: opStr = "%";
        break;
    }
    DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul: methodName = "mul"; break;
        case ExprMulDivModNode::Op::Div: methodName = "div"; break;
        case ExprMulDivModNode::Op::Mod: methodName = "mod"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isUnsigned = type.startsWith('u');

    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul:
        if (isFloat) {
            return _builder.CreateFMul(left, right);
        }
        return _builder.CreateMul(left, right);
    case ExprMulDivModNode::Op::Div:
        if (isFloat) {
            return _builder.CreateFDiv(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateUDiv(left, right);
        }
        return _builder.CreateSDiv(left, right);
    case ExprMulDivModNode::Op::Mod:
        if (isFloat) {
            return _builder.CreateFRem(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateURem(left, right);
        }
        return _builder.CreateSRem(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3077);
}

llvm::Value* Compiler::compileBinOpExpr(p<ExprBinOpNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprBinOpNode::Op::And: opStr = "&";
        break;
    case ExprBinOpNode::Op::Or: opStr = "|";
        break;
    case ExprBinOpNode::Op::Xor: opStr = "^";
        break;
    case ExprBinOpNode::Op::Shl: opStr = "<<";
        break;
    case ExprBinOpNode::Op::Shr: opStr = ">>";
        break;
    }
    DEBUG_LOG_VAL("    Expr: BinOp", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprBinOpNode::Op::And: methodName = "and"; break;
        case ExprBinOpNode::Op::Or: methodName = "or"; break;
        case ExprBinOpNode::Op::Xor: methodName = "xor"; break;
        case ExprBinOpNode::Op::Shl: methodName = "shl"; break;
        case ExprBinOpNode::Op::Shr: methodName = "shr"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());

    switch (node->op()) {
    case ExprBinOpNode::Op::And:
        return _builder.CreateAnd(left, right);
    case ExprBinOpNode::Op::Or:
        return _builder.CreateOr(left, right);
    case ExprBinOpNode::Op::Xor:
        return _builder.CreateXor(left, right);
    case ExprBinOpNode::Op::Shl:
        return _builder.CreateShl(left, right);
    case ExprBinOpNode::Op::Shr:
        if (type.startsWith('u')) {
            return _builder.CreateLShr(left, right);
        }
        return _builder.CreateAShr(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3075);
}

llvm::Value* Compiler::compileParenExpr(p<ExprParenNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    DEBUG_LOG("    Expr: Paren");
    return compileExpr(node->expr());
}

llvm::Value* Compiler::compileCompareExpr(p<ExprCompareNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    (void)node->getType();
    auto leftType = applySubst(node->left()->getType());
    auto rightType = applySubst(node->right()->getType());

    if (leftType != rightType) {
        // Phase 3.4.g: ExprCompareNode::getType 已抛 E3004 (kMigratedCodes 命中,
        // SemaPass 顶部 setResolvedType 自动重抛), 此处不可达; 保留作幂等防御性双跑。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3004, leftType.name, rightType.name);
    }

    // Phase 1d.3：禁 Weak == / !=（DRAFT §5：v1 不暴露 handle 比较语义）
    if (leftType.isWeak()) {
        if (node->op() == ExprCompareNode::Op::Eq || node->op() == ExprCompareNode::Op::Ne) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3078)
                .withHint("先 `upgrade(weak)` 取得 Rc<T>?，再用 `?.` / `??` / 相等比较判定目标对象");
        }
    }

    // Ptr：内置 == / !=（用于 `p == null` 等场景）；ordering 不开放
    if (leftType.isPtr()) {
        if (node->op() == ExprCompareNode::Op::Eq || node->op() == ExprCompareNode::Op::Ne) {
            auto left = compileExpr(node->left());
            auto right = compileExpr(node->right());
            if (node->op() == ExprCompareNode::Op::Eq) {
                return _builder.CreateICmpEQ(left, right);
            }
            return _builder.CreateICmpNE(left, right);
        }
        if (node->op() != ExprCompareNode::Op::AndAnd && node->op() != ExprCompareNode::Op::OrOr) {
            const char* opSym =
                node->op() == ExprCompareNode::Op::Lt ? "<" :
                node->op() == ExprCompareNode::Op::Le ? "<=" :
                node->op() == ExprCompareNode::Op::Gt ? ">" : ">=";
            const char* mname =
                node->op() == ExprCompareNode::Op::Lt ? "lt" :
                node->op() == ExprCompareNode::Op::Le ? "le" :
                node->op() == ExprCompareNode::Op::Gt ? "gt" : "ge";
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3073, "Ptr", opSym, mname)
                .withHint("Ptr 只支持 == / != 比较（与 null 或另一 Ptr）");
        }
    }

    string opStr;
    switch (node->op()) {
    case ExprCompareNode::Op::Eq: opStr = "==";
        break;
    case ExprCompareNode::Op::Ne: opStr = "!=";
        break;
    case ExprCompareNode::Op::Lt: opStr = "<";
        break;
    case ExprCompareNode::Op::Le: opStr = "<=";
        break;
    case ExprCompareNode::Op::Gt: opStr = ">";
        break;
    case ExprCompareNode::Op::Ge: opStr = ">=";
        break;
    case ExprCompareNode::Op::AndAnd: opStr = "&&";
        break;
    case ExprCompareNode::Op::OrOr: opStr = "||";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << leftType.name);

    // && 和 || 是逻辑运算符，不转换为方法调用
    if (node->op() == ExprCompareNode::Op::AndAnd || node->op() == ExprCompareNode::Op::OrOr) {
        auto left = compileExpr(node->left());
        auto right = compileExpr(node->right());
        if (node->op() == ExprCompareNode::Op::AndAnd) {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.rhs");
            return _builder.CreateAnd(leftBool, rightBool, "and");
        } else {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.rhs");
            return _builder.CreateOr(leftBool, rightBool, "or");
        }
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprCompareNode::Op::Eq: methodName = "eq"; break;
        case ExprCompareNode::Op::Ne: methodName = "ne"; break;
        case ExprCompareNode::Op::Lt: methodName = "lt"; break;
        case ExprCompareNode::Op::Le: methodName = "le"; break;
        case ExprCompareNode::Op::Gt: methodName = "gt"; break;
        case ExprCompareNode::Op::Ge: methodName = "ge"; break;
        default: break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = leftType.startsWith('f');
    bool isUnsigned = leftType.startsWith('u');

    switch (node->op()) {
    case ExprCompareNode::Op::Eq:
        if (isFloat) {
            return _builder.CreateFCmpOEQ(left, right);
        }
        return _builder.CreateICmpEQ(left, right);
    case ExprCompareNode::Op::Ne:
        if (isFloat) {
            return _builder.CreateFCmpONE(left, right);
        }
        return _builder.CreateICmpNE(left, right);
    case ExprCompareNode::Op::Lt:
        if (isFloat) {
            return _builder.CreateFCmpOLT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULT(left, right);
        }
        return _builder.CreateICmpSLT(left, right);
    case ExprCompareNode::Op::Le:
        if (isFloat) {
            return _builder.CreateFCmpOLE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULE(left, right);
        }
        return _builder.CreateICmpSLE(left, right);
    case ExprCompareNode::Op::Gt:
        if (isFloat) {
            return _builder.CreateFCmpOGT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGT(left, right);
        }
        return _builder.CreateICmpSGT(left, right);
    case ExprCompareNode::Op::Ge:
        if (isFloat) {
            return _builder.CreateFCmpOGE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGE(left, right);
        }
        return _builder.CreateICmpSGE(left, right);
    default:
        break;
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3076);
}

llvm::Value* Compiler::compileIfElseExpr(p<ExprIfElseNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = node->getType();
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL(
        "    Expr: IfElse", "hasResult=" << hasResult << ", type=" << (resultType.empty() ? "void" : resultType.name));

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    DEBUG_LOG("      Created basic blocks: if.then, if.else, if.merge");
    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);

    llvm::PHINode* phi = nullptr;
    if (hasResult) {
        phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    }

    DEBUG_LOG("      Compiling then block");
    compileStatementBlockWithResult(node->thenBlock(), mergeBB, phi, resultType);

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);

    auto& elifs = node->elifs();
    auto elseBlock = node->elseBlock();
    DEBUG_LOG_VAL("      elifs count", elifs.size());

    if (!elifs.empty()) {
        for (size_t i = 0; i < elifs.size(); ++i) {
            auto& elif = elifs[i];
            DEBUG_LOG_VAL("        Compiling elif", i);
            auto elifCond = compileExpr(elif->condition());
            auto elifCondBool = _builder.CreateICmpNE(
                elifCond, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "elif.cond");

            llvm::BasicBlock* elifThenBB = llvm::BasicBlock::Create(_context, "elif.then", func);
            llvm::BasicBlock* elifElseBB = llvm::BasicBlock::Create(_context, "elif.else");

            _builder.CreateCondBr(elifCondBool, elifThenBB, elifElseBB);

            _builder.SetInsertPoint(elifThenBB);
            compileStatementBlockWithResult(elif->block(), mergeBB, phi, resultType);

            func->insert(func->end(), elifElseBB);
            _builder.SetInsertPoint(elifElseBB);
        }
    }

    if (elseBlock) {
        DEBUG_LOG("      Compiling else block");
        compileStatementBlockWithResult(elseBlock, mergeBB, phi, resultType);
    } else {
        DEBUG_LOG("      No else block");
        if (hasResult) {
            phi->addIncoming(llvm::UndefValue::get(getLLVMType(resultType)), _builder.GetInsertBlock());
        }
        _builder.CreateBr(mergeBB);
    }

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    if (hasResult) {
        DEBUG_LOG("      Returning phi node");
        // Phase 8d.3: 各分支已归一为 +1，phi 整体作为 fresh 句柄交给外层 statement frame
        if (resultType.isRc() || resultType.isArrayGeneric() || resultType.isWeak()) {
            recordTemp(phi, resultType);
        }
        return phi;
    }
    return nullptr;
}

llvm::Value* Compiler::compileOneLineIfElseExpr(p<ExprOneLineIfElseNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = node->getType();

    DEBUG_LOG_VAL("    Expr: OneLineIfElse", "type=" << resultType.name);

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);
    auto trueVal = compileBranchResultNormalized(node->trueValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto thenEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto falseVal = compileBranchResultNormalized(node->falseValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto elseEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    auto phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    phi->addIncoming(trueVal, thenEndBB);
    phi->addIncoming(falseVal, elseEndBB);

    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (resultType.isRc() || resultType.isArrayGeneric() || resultType.isWeak()) {
        recordTemp(phi, resultType);
    }
    return phi;
}

llvm::Value* Compiler::compileIfElsePreValueExpr(p<ExprIfElsePreValueNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = node->getType();

    DEBUG_LOG_VAL("    Expr: IfElsePreValue", "type=" << resultType.name);

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);
    auto trueVal = compileBranchResultNormalized(node->trueValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto thenEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto falseVal = compileBranchResultNormalized(node->falseValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto elseEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    auto phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    phi->addIncoming(trueVal, thenEndBB);
    phi->addIncoming(falseVal, elseEndBB);

    if (resultType.isRc() || resultType.isArrayGeneric() || resultType.isWeak()) {
        recordTemp(phi, resultType);
    }
    return phi;
}

llvm::Value* Compiler::compileArrayGetExpr(p<ExprGetNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        // Phase 3.4.g: yux*.g4 强制 indices >= 1, 该分支不可达; 保留作 dead 防御。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3060);
    }

    DEBUG_LOG_VAL("    Expr: ArrayGet", arrayType.name);

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3033, varName);
            }
            currentPtr = it->second;
        }
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(arrayExpr)) {
        auto fieldPtr = compileExpr(dotNode);
        auto baseType = dotNode->baseExpr()->getType();
        auto member = dotNode->member();

        auto structDecl = _file->getStructDecl(baseType.name);
        if (structDecl) {
            int fieldIndex = structDecl->fieldIndex(member);
            if (fieldIndex >= 0) {
                auto alloca = _builder.CreateAlloca(getLLVMType(arrayType), nullptr, "array_field_tmp");
                _builder.CreateStore(fieldPtr, alloca);
                currentPtr = alloca;
            }
        }
    }

    if (!currentPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3061);
    }

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            // Phase 3.4.g: ExprGetNode::getType 已抛 E3057 (同条件, kMigratedCodes 命中);
            // 这里的 E3055 在 sema 跑过后不可达, 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3055);
        }

        auto elemLLVMType = getLLVMType(*elemType);
        auto handle = loadArrayHandle(currentPtr);
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayBlockDataFieldPtr(handle), "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        return _builder.CreateLoad(elemLLVMType, elemPtr, "array.elem.load");
    }

    if (!arrayType.isArray()) {
        // Phase 3.4.g: ExprGetNode::getType 已抛 E3062 (kMigratedCodes 命中,
        // SemaPass 自动重抛), 此处不可达; 保留作幂等防御性双跑。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3062, arrayType.name);
    }

    for (auto& indexExpr : indices) {
        auto indexVal = compileExpr(indexExpr);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* gepIndices[] = {zero, indexVal};

        auto llvmArrayType = getLLVMType(currentType);
        currentPtr = _builder.CreateGEP(llvmArrayType, currentPtr, gepIndices, "array.element");

        if (currentType.elementType) {
            currentType = *currentType.elementType;
        }
    }

    return _builder.CreateLoad(getLLVMType(currentType), currentPtr, "array.load");
}

// 编译元组构造表达式 (e1, e2, ...)
// Phase 3：透明 layout，按声明顺序构造一个匿名 struct 值；元素递归编译
// 实现：从 undef 起，逐个 CreateInsertValue 写入；返回 struct 值（非指针）
llvm::Value* Compiler::compileTupleExpr(p<ExprTupleNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto tupleType = node->getType();
    auto llvmTy = getLLVMType(tupleType);
    if (!llvmTy) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3098,
                       tupleType.name, string("(tuple)"), string("(tuple)"));
    }
    DEBUG_LOG_VAL("    Expr: Tuple", tupleType.name);

    llvm::Value* aggr = llvm::UndefValue::get(llvmTy);
    auto& elems = node->elements();
    for (size_t i = 0; i < elems.size(); ++i) {
        auto elemVal = compileExpr(elems[i]);
        if (!elemVal) {
            throw YuxError(elems[i]->getLineNumber(), elems[i]->getColumn(), ErrorCode::E3091);
        }
        aggr = _builder.CreateInsertValue(aggr, elemVal, {static_cast<unsigned>(i)}, "tuple.ins");
    }
    return aggr;
}

// 把 ExprArrayNode 按 Array<elemType> 字面量编译，分配 Block 并写入元素，返回 Block* 句柄。
// 详见声明处注释；嵌套 Array<Array<U>> 字面量的内层走自递归，避免被自身 getType()
// 推断为 [N x U] 固定数组后被外层 store 越界踩坏后续槽。
llvm::Value* Compiler::buildArrayLiteralBlock(ExprArrayNode* arrayNode, const TypeInfo& elemType) {
    auto& elements = arrayNode->elements();
    auto count = elements.size();
    auto elemLLVMType = getLLVMType(elemType);
    auto countVal = _builder.getInt64(count);
    auto block = allocArrayBlock(elemLLVMType, countVal, countVal);
    if (count == 0) return block;

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(block), "lit.data");
    bool elemIsArrayGeneric = elemType.isArrayGeneric();
    sp<TypeInfo> innerElemType = elemIsArrayGeneric ? elemType.arrayGenericElementType() : nullptr;

    for (size_t i = 0; i < count; ++i) {
        llvm::Value* elemVal = nullptr;
        // 嵌套：内层数组字面量按外层期望的 Array<U> 编译（递归），结果是 Block* 句柄，
        // 包成 { ptr } 句柄值再写入外层槽。否则会按 getType() 自报的 [N x U] 固定数组
        // 编译，CreateStore 写 sizeof([N x U]) 字节到 8 字节槽 → 越界。
        if (elemIsArrayGeneric && innerElemType) {
            if (auto innerArr = dynamic_cast<ExprArrayNode*>(elements[i])) {
                auto innerBlock = buildArrayLiteralBlock(innerArr, *innerElemType);
                auto tmp = _builder.CreateAlloca(elemLLVMType, nullptr, "lit.nested.handle");
                storeArrayHandle(tmp, innerBlock);
                elemVal = _builder.CreateLoad(elemLLVMType, tmp, "lit.nested.handle.load");
            }
        }
        if (!elemVal) elemVal = compileExpr(elements[i]);

        auto idx = _builder.getInt64(i);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "lit.elem.ptr");
        // RC 元素：fresh 来源（call/构造/数组字面量）已 +1，跳过 retain，并尝试从临时帧消费；
        // 非 fresh（已有 var/field 读出）走复制 retain。
        if (typeNeedsDestructor(elemType)) {
            if (!isFreshHandleExpr(elements[i])) {
                retainHandleAtCallSite(elemVal, elemType);
            } else {
                consumeTemp(elemVal);
            }
        }
        _builder.CreateStore(elemVal, elemPtr);
    }
    return block;
}

llvm::Value* Compiler::compileArrayLiteralExpr(p<ExprArrayNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto& elements = node->elements();
    auto arrayType = node->getType();
    auto llvmArrayType = getLLVMType(arrayType);

    DEBUG_LOG_VAL("    Expr: ArrayLiteral", arrayType.name);

    // Array<T> 字面量（动态数组）：走统一 helper，结果包装为 { handle } 结构体值返回
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        auto block = buildArrayLiteralBlock(node, elemType ? *elemType : TypeInfo("i8"));
        auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.literal");
        storeArrayHandle(alloca, block);
        return _builder.CreateLoad(llvmArrayType, alloca, "array.literal.load");
    }

    // 固定大小数组 [N]T 字面量
    if (elements.empty()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3063);
    }

    auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.literal");

    for (size_t i = 0; i < elements.size(); ++i) {
        auto elemVal = compileExpr(elements[i]);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, index};
        auto elemPtr = _builder.CreateGEP(llvmArrayType, alloca, indices, "array.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
    }

    return _builder.CreateLoad(llvmArrayType, alloca, "array.load");
}

llvm::Value* Compiler::compileGetRefExpr(p<ExprGetRefNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto objName = node->obj().getText();
    auto& subs = node->subs();

    DEBUG_LOG_VAL("    Expr: GetRef", objName);

    auto it = _localVarPtrs.find(objName);
    if (it == _localVarPtrs.end()) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode,
            node->getLineNumber(), node->getColumn(), ErrorCode::E3031, objName);
    }

    llvm::Value* currentPtr = it->second;
    auto sym = _currentFnNode->lookupSymbol(objName);
    if (!sym) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode,
            node->getLineNumber(), node->getColumn(), ErrorCode::E3030, objName);
    }

    TypeInfo currentType = sym->type;
    // Phase 4a: 若源是 T&（参数 / 局部 ref），currentPtr 已经是底层 T 的地址；剥到 T
    if (currentType.isRef()) {
        if (auto inner = currentType.refElementType()) currentType = *inner;
    }

    for (auto& sub : subs) {
        auto memberName = sub.getText();

        // Phase 4c: Rc<T>.field —— 自动 deref：load handle，payload = handle + 8
        if (currentType.isRc()) {
            auto rcStructType = getLLVMType(currentType);
            auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, currentPtr, {zero32, zero32}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            currentPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            if (auto inner = currentType.rcElementType()) currentType = *inner;
        }

        auto structDecl = _file->getStructDecl(currentType.name);
        if (!structDecl && _yux && _yux->sdkFile()) {
            structDecl = _yux->sdkFile()->getStructDecl(currentType.name);
        }
        if (!structDecl) {
            // Phase 3.4.d.1: SemaPass.visitExpr ExprGetRefNode 顶部
            // setResolvedType(getType) 已通过 ExprGetRefNode::getType 抛 E3041
            // (kMigratedCodes 命中, 自动重抛), 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3041, currentType.name);
        }

        int fieldIndex = structDecl->fieldIndex(memberName);
        if (fieldIndex < 0) {
            // Phase 3.4.d.1: 同上, getType 已抛 E3040, 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3040, currentType.name, memberName);
        }

        // Phase 3.4.d.2: E3042 私有字段可见性 整体抠到 sema::validatePrivateFieldAccess.
        // SemaPass.visitExpr ExprGetRefNode 分支调用 validateGetRefPrivacy 已沿同链路抢先抛;
        // 这里保留作幂等防御性双跑.
        sema::validatePrivateFieldAccess(structDecl, memberName, currentType.name,
                                         _currentStructName,
                                         node->getLineNumber(), node->getColumn());

        auto field = structDecl->fields()[fieldIndex];
        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        llvm::Value* indices[] = {zero, idx};

        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        TypeInfo fieldType = field->getType();
        if (currentType.isGeneric() && structDecl->isGeneric()
            && currentType.genericArgs.size() == structDecl->typeParams().size()) {
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    currentType.genericArgs[i] ? *currentType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        currentType = fieldType;
    }

    return currentPtr;
}

llvm::Value* Compiler::compileUnaryExpr(p<ExprUnaryNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = node->getType();
    auto rightType = node->right()->getType();

    string opStr;
    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        opStr = "-";
        break;
    case ExprUnaryNode::Op::Rev:
        opStr = "~";
        break;
    case ExprUnaryNode::Op::Not:
        opStr = "!";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(rightType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprUnaryNode::Op::Neg: methodName = "neg"; break;
        case ExprUnaryNode::Op::Rev: methodName = "inv"; break;
        case ExprUnaryNode::Op::Not: methodName = "not"; break;
        }
        return compileCustomTypeUnaryOp(node->right(), rightType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isBool = type.name == "bool";

    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        if (isFloat) {
            return _builder.CreateFNeg(right, "neg");
        }
        return _builder.CreateNeg(right, "neg");
    case ExprUnaryNode::Op::Rev:
        if (isFloat) {
            // Phase 3.4.h: ExprUnaryNode::getType 已抛 E3070 (kMigratedCodes 命中,
            // SemaPass 自动重抛), 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3070, type.name);
        }
        return _builder.CreateNot(right, "not");
    case ExprUnaryNode::Op::Not:
        if (!isBool) {
            // Phase 3.4.h: 同上, getType 已抛 E3071, 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3071, type.name);
        }
        return _builder.CreateNot(right, "lnot");
    }

    // Phase 3.4.h: switch default unreachable, 上面三个 case 已覆盖全部 Op; 兜底。
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3072);
}

llvm::Value* Compiler::compileDotExpr(p<ExprDotNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // 安全访问 a?.b：单独走分支
    if (node->isSafe()) {
        return compileSafeDotExpr(node);
    }
    auto baseExpr = node->baseExpr();
    auto member = node->member();

    // 元组成员访问 a.N：member 为纯数字，base 解析后必须是 Tuple
    // 透明 alias 由 applySubst 兜底（顶层 Normal alias、泛型 alias 实例化均能展开）
    if (!member.empty() && std::all_of(member.begin(), member.end(),
                                       [](char c) { return c >= '0' && c <= '9'; })) {
        auto baseTypeRaw = baseExpr->getType();
        auto baseTypeResolved = applySubst(baseTypeRaw);
        if (baseTypeResolved.isTuple()) {
            auto& elems = baseTypeResolved.tupleElements();
            size_t idx = static_cast<size_t>(std::stoul(member));
            if (idx >= elems.size()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3100,
                               member, baseTypeRaw.getFullName(),
                               std::to_string(elems.size()));
            }
            DEBUG_LOG_VAL("    Expr: TupleMemberAccess", baseTypeResolved.name << "." << member);
            auto baseVal = compileExpr(baseExpr);
            if (!baseVal) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
            }
            return _builder.CreateExtractValue(baseVal, {static_cast<unsigned>(idx)}, "tuple.elem");
        }
        // 非 Tuple 的 .N 留给后续逻辑（当前会落到 E3090 报"Unsupported dot expression"）
    }

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("    Expr: DotCast", member);
        auto baseVal = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();

        string castFnName = "__cast_" + to_string(_castCounter++);
        _castFunctions[castFnName] = {baseVal, srcType, TypeInfo(dstType)};

        return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    }

    auto baseType = baseExpr->getType();
    TypeInfo actualType = baseType;
    llvm::Value* structPtr = nullptr;

    if (baseType.isRef()) {
        auto refElemType = baseType.refElementType();
        if (refElemType) {
            actualType = *refElemType;
        }
    }

    if (baseType.isRc()) {
        auto rcElemType = baseType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
        }
    }

    auto structDecl = _file->getStructDecl(actualType.name);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(actualType.name);
    }

    if (structDecl) {
        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            // Phase 3.4.d.2: E3042 私有字段可见性 整体抠到 sema::validatePrivateFieldAccess.
            // SemaPass.visitExpr ExprDotNode 分支调用 validateDotFieldPrivacy 已抢先抛;
            // 这里保留作幂等防御性双跑.
            sema::validatePrivateFieldAccess(structDecl, member, actualType.name,
                                             _currentStructName,
                                             node->getLineNumber(), node->getColumn());
            auto field = structDecl->fields()[fieldIndex];

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        structPtr = it->second;
                    }
                }
            }

            if (!structPtr) {
                auto baseVal = compileExpr(baseExpr);
                auto tmpAlloca = _builder.CreateAlloca(getLLVMType(baseType), nullptr, "struct_field_tmp");
                _builder.CreateStore(baseVal, tmpAlloca);
                structPtr = tmpAlloca;
            }

            llvm::Value* dataPtr = structPtr;

            if (baseType.isRc()) {
                // Rc.field：load handle，payload = handle + 8
                auto rcStructType = getLLVMType(baseType);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto handleField = _builder.CreateGEP(rcStructType, structPtr, {zero, zero}, "rc.handle_field");
                auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
                dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            llvm::Value* indices[] = {zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, dataPtr, indices, "struct.field");
            auto fieldType = field->getType();

            if (actualType.isGeneric() && structDecl->isGeneric()
                && actualType.genericArgs.size() == structDecl->typeParams().size()) {
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                    subst[structDecl->typeParams()[i]] =
                        actualType.genericArgs[i] ? *actualType.genericArgs[i] : TypeInfo();
                }
                fieldType = fieldType.substitute(subst);
            }

            return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "field.load");
        }
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto sym = _currentFnNode->lookupSymbol(objName);
            if (sym && _localVarPtrs.contains(objName)) {
                DEBUG_LOG_VAL("    Expr: DotMemberLoad", objName << "." << member);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName]);
            }
        }
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3090);
}

// 编译 a?.b 安全成员访问
// 语义：a 是 Nullable<T>，T 是结构体，b 是 T 的字段
//   - a 持值 → Nullable<U>{ has=true, value=a._value.b }
//   - a 不持值 → Nullable<U>{ has=false, value=zero }
// 当前实现仅覆盖字段访问；方法调用形式 a?.foo() 不在本阶段
// 链式 a?.b?.c 自然递归（每层 base 类型为 Nullable<X>，仍走同分支）
llvm::Value* Compiler::compileSafeDotExpr(p<ExprDotNode> node) {
    auto baseExpr = node->baseExpr();
    auto member = node->member();
    auto baseType = baseExpr->getType();

    if (!baseType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3025, baseType.name);
    }
    auto innerType = baseType.nullableInnerType();
    // Phase 5: Rc<T>? 自动 deref —— 把 Rc<U> 视为 U 进字段查
    bool innerIsRc = innerType->isRc();
    auto rawInnerType = innerType;  // Rc<U>（用于 LLVM 类型 = { ptr handle }）
    if (innerIsRc) {
        auto rcInner = innerType->rcElementType();
        if (!rcInner) {
            throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3050);
        }
        innerType = rcInner;
    }
    auto innerStructDecl = _file->getStructDecl(innerType->name);
    if (!innerStructDecl && _yux && _yux->sdkFile()) {
        innerStructDecl = _yux->sdkFile()->getStructDecl(innerType->name);
    }
    if (!innerStructDecl) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3044, innerType->name);
    }
    int fieldIdx = innerStructDecl->fieldIndex(member);
    if (fieldIdx < 0) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3040, innerType->name, member);
    }
    auto fieldType = innerStructDecl->fields()[fieldIdx]->getType();
    if (innerType->isGeneric() && innerStructDecl->isGeneric()
        && innerType->genericArgs.size() == innerStructDecl->typeParams().size()) {
        map<string, TypeInfo> subst;
        for (size_t i = 0; i < innerStructDecl->typeParams().size(); ++i) {
            subst[innerStructDecl->typeParams()[i]] =
                innerType->genericArgs[i] ? *innerType->genericArgs[i] : TypeInfo();
        }
        fieldType = fieldType.substitute(subst);
    }

    // 结果类型 Nullable<U>
    vector<sp<TypeInfo>> nullArgs;
    nullArgs.push_back(make_shared<TypeInfo>(fieldType));
    TypeInfo resultType("Nullable", nullArgs);

    auto baseLLVMType = getLLVMType(baseType);     // Nullable<T> struct
    auto innerLLVMType = getLLVMType(*innerType);  // T struct
    auto fieldLLVMType = getLLVMType(fieldType);   // U
    auto resultLLVMType = getLLVMType(resultType); // Nullable<U> struct

    auto baseVal = compileExpr(baseExpr);
    auto hasVal = _builder.CreateExtractValue(baseVal, {0}, "sd.base.has");
    auto innerVal = _builder.CreateExtractValue(baseVal, {1}, "sd.base.inner");

    auto resultAlloca = _builder.CreateAlloca(resultLLVMType, nullptr, "sd.result");
    auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    auto resHasField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, zero32}, "sd.res.has");
    auto resValueField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, one32}, "sd.res.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "sd.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "sd.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "sd.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    // then: 取出 inner 的字段，包装到 result
    _builder.SetInsertPoint(thenBB);
    llvm::Value* fieldPtr = nullptr;
    auto fieldIdxConst = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIdx);
    if (innerIsRc) {
        // Rc<U>: 提取 handle，payload = handle + 8，GEP 到字段
        auto rawInnerLLVMTy = getLLVMType(*rawInnerType);  // Rc struct { ptr handle }
        auto rcAlloca = _builder.CreateAlloca(rawInnerLLVMTy, nullptr, "sd.rc.tmp");
        _builder.CreateStore(innerVal, rcAlloca);
        auto handleField = _builder.CreateGEP(rawInnerLLVMTy, rcAlloca, {zero32, zero32}, "sd.rc.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "sd.rc.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "sd.rc.payload");
        fieldPtr = _builder.CreateGEP(innerLLVMType, payload, {zero32, fieldIdxConst}, "sd.field");
    } else {
        auto innerAlloca = _builder.CreateAlloca(innerLLVMType, nullptr, "sd.inner.tmp");
        _builder.CreateStore(innerVal, innerAlloca);
        fieldPtr = _builder.CreateGEP(innerLLVMType, innerAlloca, {zero32, fieldIdxConst}, "sd.field");
    }
    auto fieldVal = _builder.CreateLoad(fieldLLVMType, fieldPtr, "sd.field.load");
    _builder.CreateStore(_builder.getInt1(true), resHasField);
    _builder.CreateStore(fieldVal, resValueField);
    _builder.CreateBr(mergeBB);

    // else: 空 Nullable<U>
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    _builder.CreateStore(_builder.getInt1(false), resHasField);
    _builder.CreateStore(llvm::Constant::getNullValue(fieldLLVMType), resValueField);
    _builder.CreateBr(mergeBB);

    // merge: load result 作为 ssa 值
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    return _builder.CreateLoad(resultLLVMType, resultAlloca, "sd.result.load");
}

// 编译 a ?? b 表达式
// 语义：a 是 Nullable<T>。a 持值则结果取 a._value，否则取 b（b 必须可转 T）
// IR 形态：
//   %has = extractvalue %a, 0
//   %v   = extractvalue %a, 1
//   br %has, then, else
//   then: br merge (carry %v)
//   else: %r = compile(b); br merge (carry %r)
//   merge: phi T [%v, then] [%r, else]
llvm::Value* Compiler::compileNullElseExpr(p<ExprNullElseNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto leftType = node->left()->getType();
    if (!leftType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3024, leftType.name);
    }
    auto innerType = leftType.nullableInnerType();
    if (!innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3051);
    }
    auto innerLLVMType = getLLVMType(*innerType);

    // flexible int 推断：右侧无后缀整数 → 推断为 T
    if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->right())) {
        tryInferIntType(node->right(), *innerType);
    }

    // 计算左侧（Nullable 结构体值）
    auto leftVal = compileExpr(node->left());
    auto hasVal = _builder.CreateExtractValue(leftVal, {0}, "ne.has");
    auto valueVal = _builder.CreateExtractValue(leftVal, {1}, "ne.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "ne.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "ne.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "ne.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    bool isRcHandle = innerType->isRc() || innerType->isArrayGeneric() || innerType->isWeak();

    // then: 持值，直接用 _value（这是从 Nullable struct 抽出的借用，需 retain 归一为 +1）
    _builder.SetInsertPoint(thenBB);
    if (isRcHandle) {
        emitRetainOnHandleValue(valueVal, *innerType);
    }
    auto thenEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // else: 取右侧默认值（用子帧 + 归一）
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto rightType = node->right()->getType();
    auto rightVal = compileBranchResultNormalized(node->right(), *innerType);
    if (rightType != *innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3023, rightType.name, innerType->name);
    }
    auto elseEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // merge: phi 合并
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    auto phi = _builder.CreatePHI(innerLLVMType, 2, "ne.result");
    phi->addIncoming(valueVal, thenEndBB);
    phi->addIncoming(rightVal, elseEndBB);
    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (isRcHandle) {
        recordTemp(phi, *innerType);
    }
    return phi;
}

llvm::Value* Compiler::compileExpr(p<ExprNode> node) {
    auto type = node->getType();
    DEBUG_LOG_VAL("  compileExpr", "type=" << (type.empty() ? "void" : type.name));
    // Phase 2.2 Sema/Codegen 拆分：resolvedType 由各 compile<Foo>Expr 自行写入。
    // Phase 2.4：本入口 dispatch 后的 recordTemp / typeNeedsDestructor 改读
    // resolvedOrInferredType(node)；本地 type 仅给 DEBUG_LOG 用，等 Phase 3
    // SemaPass 落地后即可整体删除。

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(node)) {
        return compileLiteralExpr(literalNode);
    } else if (auto addSubNode = dynamic_cast<ExprAddSubNode*>(node)) {
        return compileAddSubExpr(addSubNode);
    } else if (auto mulDivModNode = dynamic_cast<ExprMulDivModNode*>(node)) {
        return compileMulDivModExpr(mulDivModNode);
    } else if (auto binOpNode = dynamic_cast<ExprBinOpNode*>(node)) {
        return compileBinOpExpr(binOpNode);
    } else if (auto parenNode = dynamic_cast<ExprParenNode*>(node)) {
        return compileParenExpr(parenNode);
    } else if (auto callNode = dynamic_cast<ExprCallNode*>(node)) {
        // Phase 8d.1: 调用结果若为 fresh RC 句柄（Rc/Array/Weak），登记到当前语句临时帧
        auto val = compileCallExpr(callNode);
        if (val) {
            // Phase 2.4: compileCallExpr 入口已写 resolvedType，优先读它
            recordTemp(val, resolvedOrInferredType(node));
        }
        return val;
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(node)) {
        return compileDotExpr(dotNode);
    } else if (auto compareNode = dynamic_cast<ExprCompareNode*>(node)) {
        return compileCompareExpr(compareNode);
    } else if (auto ifElseNode = dynamic_cast<ExprIfElseNode*>(node)) {
        return compileIfElseExpr(ifElseNode);
    } else if (auto oneLineIfElseNode = dynamic_cast<ExprOneLineIfElseNode*>(node)) {
        return compileOneLineIfElseExpr(oneLineIfElseNode);
    } else if (auto ifElsePreValueNode = dynamic_cast<ExprIfElsePreValueNode*>(node)) {
        return compileIfElsePreValueExpr(ifElsePreValueNode);
    } else if (auto getNode = dynamic_cast<ExprGetNode*>(node)) {
        return compileArrayGetExpr(getNode);
    } else if (auto arrayNode = dynamic_cast<ExprArrayNode*>(node)) {
        // Phase 8d.1: 数组字面量 _array_alloc 给 strong=1，登记为 fresh 临时
        auto val = compileArrayLiteralExpr(arrayNode);
        if (val) {
            // Phase 2.4: 同上，优先读 resolvedType
            recordTemp(val, resolvedOrInferredType(node));
        }
        return val;
    } else if (auto tupleNode = dynamic_cast<ExprTupleNode*>(node)) {
        return compileTupleExpr(tupleNode);
    } else if (auto dynCtorNode = dynamic_cast<ExprDynCtorNode*>(node)) {
        // Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
        // Phase 1c：仅 emit 占位 fat ptr { vtable=null, data=src.handle }；
        // 真 vtable 与 dtor 路由留 Phase 3，对象安全 / E1133 类型检查留 Phase 2。
        return compileDynCtorExpr(dynCtorNode);
    } else if (auto heapCtorNode = dynamic_cast<ExprHeapCtorNode*>(node)) {
        // Heap:<T>(x) 构造（DRAFT-heap-types §8.3a）—— Phase 2.4 codegen
        return compileHeapCtorExpr(heapCtorNode);
    } else if (auto structLitNode = dynamic_cast<ExprStructLitNode*>(node)) {
        // Phase 3b 构造模型重构: `Self { .field = value ... }` codegen.
        // 仅在 #Static fn 体内合法 (sema Phase 2d 已校验). 流程:
        //   alloca Self -> 按 fieldIndex 依次 GEP + store -> Load 返回值.
        // Phase 4a (BUGS #4): 句柄字段 (Rc / Array / Weak / fn-fat-ptr /
        //   含 RC 字段的非平凡 struct) 在 store 前对非 fresh 源 retain,
        //   与 `$.field = value` assign 路径行为对齐. fresh 源 (call/ctor/array-lit
        //   等) 已自带 +1 所有权, 直接 move-in 不再 retain.
        int line = structLitNode->resolveLineNumber();
        int col = structLitNode->resolveColumn();
        const string& structName = _currentStructName;
        if (structName.empty()) {
            throw YuxError(line, col, ErrorCode::E0000,
                           "`Self { ... }` codegen 找不到所属结构体 (sema 应已拦截)");
        }
        auto* decl = _file ? _file->getStructDecl(structName) : nullptr;
        if (!decl && _yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
            decl = _yux->sdkFile()->getStructDecl(structName);
        }
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E0000,
                           "`Self { ... }` codegen 找不到 struct decl: " + structName);
        }
        auto llvmStructType = getLLVMType(TypeInfo(structName));
        if (!llvmStructType) {
            throw YuxError(line, col, ErrorCode::E3096, structName);
        }
        auto alloca = _builder.CreateAlloca(llvmStructType, nullptr, structName + ".lit");
        // 零初始化, 与 ctor 入口保持一致, 避免遗漏字段 (实际上 sema 已强制全列)
        auto& dl = _module->getDataLayout();
        auto sizeBytes = dl.getTypeAllocSize(llvmStructType).getFixedValue();
        _builder.CreateMemSetInline(alloca, llvm::MaybeAlign(1), _builder.getInt8(0),
                                    _builder.getInt64(sizeBytes));
        for (auto& fi : structLitNode->fields()) {
            string fname = fi->name().getText();
            int idx = decl->fieldIndex(fname);
            auto fieldPtr = _builder.CreateStructGEP(llvmStructType, alloca,
                                                     static_cast<unsigned>(idx),
                                                     structName + "." + fname);
            const auto* fdecl = decl->field(fname);
            const auto fieldType = fdecl ? fdecl->getType() : TypeInfo();
            // Phase 4a: Array<T> 字段 + 数组字面量 RHS, 直接走 buildArrayLiteralBlock,
            // 把字段的 element type 透传给 literal, 避免无目标类型语境下默认成定长 [N]T
            // (与 compileDeclareAssignStatement 的 isArrayGeneric 分支对齐, BUGS #4)
            if (fieldType.isArrayGeneric()) {
                if (auto arrayNode = dynamic_cast<ExprArrayNode*>(fi->value())) {
                    auto elemType = fieldType.arrayGenericElementType();
                    if (!elemType) {
                        throw YuxError(line, col, ErrorCode::E3055);
                    }
                    auto block = buildArrayLiteralBlock(arrayNode, *elemType);
                    storeArrayHandle(fieldPtr, block);
                    continue;
                }
            }
            auto val = compileExpr(fi->value());
            // Phase 4a: 句柄字段所有权转移 (与 declare-assign 路径对齐, BUGS #4)
            //   - fresh 源 (call / ctor / array-lit): 已 +1, 直接 consume 临时帧, 不重复 retain
            //   - 非 fresh 源 (let / 字段读取等): retain 一次, 让源句柄与字段都各持 +1
            if (fdecl && val && typeNeedsDestructor(fieldType)) {
                if (isFreshHandleExpr(fi->value())) {
                    consumeTemp(val);
                } else {
                    retainHandleAtCallSite(val, fieldType);
                }
            }
            _builder.CreateStore(val, fieldPtr);
        }
        return _builder.CreateLoad(llvmStructType, alloca, structName + ".lit.load");
    } else if (auto enumCtorNode = dynamic_cast<ExprPathCallNode*>(node)) {
        // Phase 5: enum ctor 是 +1 fresh：构造时把实参（含 RC payload）写入 enum 槽，
        // enum 值随后承担释放责任。仅当类型需要析构时才登记到临时帧
        auto val = compileEnumCtorExpr(enumCtorNode);
        // Phase 2.4: 同上，优先读 resolvedType
        auto resType = resolvedOrInferredType(node);
        if (val && typeNeedsDestructor(resType)) {
            recordTemp(val, resType);
        }
        return val;
    } else if (auto matchNode = dynamic_cast<ExprMatchNode*>(node)) {
        // Phase 6: match 表达式 — switch on tag + 绑定 + arm 体
        return compileMatchExpr(matchNode);
    } else if (auto tryCatchNode = dynamic_cast<ExprTryCatchNode*>(node)) {
        // Phase 10f: try-catch 表达式 — 仅占位 + 语义校验，IR 路由推 10g
        return compileTryCatchExpr(tryCatchNode);
    } else if (auto lambdaNode = dynamic_cast<LambdaExprNode*>(node)) {
        // Phase 2b: lambda 字面量 → 16 字节 fat-ptr 值 { fn_ptr, captures=null }
        return compileLambdaExpr(lambdaNode);
    } else if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(node)) {
        return compileGetRefExpr(getRefNode);
    } else if (auto unaryNode = dynamic_cast<ExprUnaryNode*>(node)) {
        return compileUnaryExpr(unaryNode);
    } else if (auto nullElseNode = dynamic_cast<ExprNullElseNode*>(node)) {
        return compileNullElseExpr(nullElseNode);
    } else if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(node)) {
        // 数组填充表达式需要类型注解，这里返回 nullptr
        // 实际处理在 compileDeclareAssignStatement 中
        return nullptr;
    } else {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
}

void Compiler::compileStatementBlock(p<StatementBlockNode> block) {
    DEBUG_LOG_VAL(
        "  compileStatementBlock", block->statements().size() << " statements, hasResult=" << block->hasResult());
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }
    if (block->hasResult()) {
        DEBUG_LOG("    Compiling result expression");
        compileExpr(block->resultExpr());
    }
}

llvm::Value* Compiler::compileStatementBlockWithResult(
    p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }

    if (_builder.GetInsertBlock()->getTerminator()) {
        return nullptr;
    }

    if (block->hasResult()) {
        // Phase 8d.3: RC 句柄分支结果走子帧 + 归一 retain；非 RC 沿用旧行为
        auto resultVal = compileBranchResultNormalized(block->resultExpr(), resultType);
        if (phi && !resultType.empty()) {
            phi->addIncoming(resultVal, _builder.GetInsertBlock());
        }
    }

    _builder.CreateBr(continueBlock);
    return nullptr;
}

// 编译枚举构造表达式 E::V / E::V() / E::V(args)
// Phase 5: 支持零参 + tuple-payload variant
//
// 步骤：
// 1. 通过 ExprPathCallNode::getType() 解析后的 enum 名（已透传别名）查 EnumDecl
// 2. 验证 variant 存在 / arity 匹配
// 3. 取 enum 的 LLVM 类型（{ i32 tag } 或 { i32, [N x i8] }）
// 4. 在栈上 alloca，写入 tag = variant index
// 5. tuple-payload variant：把 payload buffer 重解释为 variant 的 tuple struct，
//    逐元素 store 实参值（实参类型与 payload 元素类型严格匹配；callee-clean
//    入参规则下，Rc/Array/Weak 已是 +1 fresh 句柄，直接交付给 enum 拥有）
// 6. 零参 variant 不动 payload buffer（spec §6.5）
// 7. 加载整体 struct value 作为表达式结果返回
llvm::Value* Compiler::compileEnumCtorExpr(p<ExprPathCallNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    string enumName = node->getType().name;     // 经别名解析后的真实 enum 名
    string variantName = node->variantName().getText();
    int line = node->getLineNumber();
    int col = node->getColumn();

    // Phase 3c 构造模型重构: 若 LHS 是 struct, 走 #Static fn 调用路径.
    // sema 已先做形态校验 (#Static 命中 / 缺失 / 实例方法误用), 这里直接 emit call.
    {
        string lhsRaw = node->enumName().getText();
        auto* structImpl = _file ? _file->getStructImpl(lhsRaw) : nullptr;
        FileNode* sdk = _yux ? _yux->sdkFile() : nullptr;
        if (!structImpl && sdk && sdk != _file) {
            structImpl = sdk->getStructImpl(lhsRaw);
        }
        if (structImpl) {
            string methodName = node->variantName().getText();
            p<FnHeaderNode> methodHeader = nullptr;
            for (auto& m : structImpl->methods()) {
                if (m->header()->name().getText() == methodName) {
                    methodHeader = m->header(); break;
                }
            }
            // sema Phase 2c 已拦 E3120/E3121; 这里幂等防御性兜底
            if (!methodHeader || !methodHeader->isStatic()) {
                throw YuxError(line, col, ErrorCode::E3121, lhsRaw, methodName);
            }
            vector<TypeInfo> paramTypes;
            for (auto p : methodHeader->params()) {
                if (p->type()) paramTypes.push_back(p->type()->getType());
            }
            TypeInfo retType;
            if (methodHeader->retType()) retType = methodHeader->retType()->getType();
            string mFallibleErr;
            if (auto e = methodHeader->getAnnoArg("Fallible")) mFallibleErr = *e;
            auto fn = getMethodFunction(lhsRaw, methodName, paramTypes, retType,
                                        mFallibleErr, /*isStatic=*/true);
            vector<llvm::Value*> argVals;
            argVals.reserve(node->args().size());
            for (auto& a : node->args()) {
                argVals.push_back(compileExpr(a));
            }
            // Phase 4c: 静态 fn 调用点的句柄实参所有权转移，与 ExprCallNode 路径对齐
            // (compiler_call.cpp:667-670). 不做这步会让 callee 拿到 caller 唯一 +1,
            // callee 析构释放后 caller 的 alloca 变成 use-after-free.
            for (size_t i = 0; i < argVals.size() && i < paramTypes.size(); ++i) {
                if (!typeNeedsDestructor(paramTypes[i])) continue;
                if (!isFreshHandleExpr(node->args()[i])) {
                    retainHandleAtCallSite(argVals[i], paramTypes[i]);
                } else {
                    consumeTemp(argVals[i]);
                }
            }
            return _builder.CreateCall(fn, argVals,
                                       retType.empty() ? "" : methodName + ".ret");
        }
    }

    // Phase 3.4.a: enum ctor 形态校验 (E2019/E2020/E2021/E2032) 整体抠到 sema.
    // SemaPass 已先抛出; 这里是幂等防御性双跑.
    sema::validateEnumCtorShape(_file, _yux ? _yux->sdkFile() : nullptr, node);

    p<FileNode> owner = nullptr;
    auto enumDecl = lookupEnumDecl(enumName, owner);
    auto variant = enumDecl->variant(variantName);

    size_t givenArity = node->args().size();
    size_t declArity = variant->payloadArity();
    (void)givenArity; // arity 已由 sema::validateEnumCtorShape 校验

    int tagIndex = enumDecl->variantIndex(variantName);
    auto enumLLVMType = getLLVMType(TypeInfo(enumName));
    if (!enumLLVMType) {
        throw YuxError(line, col, ErrorCode::E3096, enumName);
    }

    // 在栈上 alloca、写入 tag
    auto alloca = _builder.CreateAlloca(enumLLVMType, nullptr, "enum.ctor");
    auto tagPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 0, "enum.tag.ptr");
    _builder.CreateStore(_builder.getInt32(tagIndex), tagPtr);

    // tuple-payload variant：把 payload buffer 重解释为 variant 自身的 tuple struct，
    // 按位置写入每个实参；payload 字段（即 enum LLVM type 的第 1 个字段）在 enum 类型有
    // payload 时一定存在，layout 形如 { i32 tag, [N x i8] payload }
    if (declArity > 0) {
        // 收集 variant 的 payload 元素 LLVM 类型，构造 variant tuple struct 类型
        vector<llvm::Type*> elemTys;
        elemTys.reserve(declArity);
        for (auto t : variant->payloadTypes()) {
            auto ll = getLLVMType(t->getType());
            if (!ll) {
                throw YuxError(line, col, ErrorCode::E3096,
                    enumName + "::" + variantName + " payload");
            }
            elemTys.push_back(ll);
        }
        auto payloadStruct = llvm::StructType::get(_context, elemTys);

        // payload buffer 字段地址（enum struct 的字段 1）
        auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 1, "enum.payload.ptr");

        // 编译实参并按 variant tuple struct 的字段位置 store.
        // 实参类型与 variant payload 类型的严格匹配 (E2032) 已由 sema::validateEnumCtorShape
        // 接管, 这里只走 IR emit.
        for (size_t i = 0; i < declArity; ++i) {
            auto argExpr = node->args()[i];
            auto argVal = compileExpr(argExpr);
            if (!argVal) {
                throw YuxError(line, col, ErrorCode::E3096,
                    enumName + "::" + variantName + " arg#" + std::to_string(i));
            }
            auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr,
                static_cast<unsigned>(i), "enum.payload.elem");
            _builder.CreateStore(argVal, fieldPtr);
            // 实参作为 fresh 临时若已入帧，需消费掉：所有权随构造转交给 enum 值，
            // 否则帧弹出时会 release 一次导致 use-after-free
            consumeTemp(argVal);
        }
    }

    auto loaded = _builder.CreateLoad(enumLLVMType, alloca, "enum.val");

    DEBUG_LOG_VAL("    Expr: EnumCtor",
        enumName << "::" << variantName << " tag=" << tagIndex << " arity=" << declArity);
    return loaded;
}

// 编译 Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
//
// Phase 1c：仅占位 codegen。目标是让 `Dyn<D>(x)` 在 parse + 类型推导 + IR 生成
// 全链路走通，落到 16 字节 fat pointer 值；真实 vtable 槽与对象安全检查留 Phase 2/3：
//   - Phase 2：E1131..E1134 静态检查（draft 名 / 嵌套 / 类型不满足 / 非对象安全）
//   - Phase 3：vtable 全局发射 + 槽 0 dtor wrapper + 构造时写真 vtable_ptr / 句柄消费
//
// 当前 emit 策略：
//   - vtable 槽：constant null（占位；Phase 3 替换为 `&__yux_vtable_<U>_<D>`）
//   - data 槽：
//       * arg.type = Rc<U>：从 Rc struct 中抽 handle（第 0 字段，指向 [RC head | U]）
//       * arg.type = U&    ：直接用借用 ptr（Phase 1c 不处理借用所有权，留 Phase 3c）
//       * 其它形态：暂用 null 占位，留 Phase 2 报 E1133
//
// 注：未做 retain / RC 转移。owned 形态意味着接管 Rc 的 +1，本应消费临时帧或 retain；
// 真路由（构造消费 Rc）随 vtable 落地一起补，所以这里 Rc 句柄"裸抽"——Phase 1c
// 的 smoke 只看编译能否过、IR 是否成型，不验运行时所有权。
llvm::Value* Compiler::compileDynCtorExpr(p<ExprDynCtorNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = node->getType();
    int line = node->getLineNumber();
    int col = node->getColumn();

    // ── Phase 2b: Dyn<D>(x) 构造静态检查 ─────────────────────────────────
    // 顺序: E1131 (D 必须是 draft) → E1132 (嵌套 Dyn) → E1134 (对象安全)
    // → E1133 (参数形态 Rc<U> / U& + U:D)。
    auto draftInner = resultType.dynDraftType();
    std::string draftBareName = draftInner ? draftInner->name : std::string();

    // 走 parent() 链而不是 parentScope()：struct 方法的 FnNode 在 AST 构造时
    // 不一定挂上 parentScope，但 parent() 链一定连到 FileNode
    // （参考 expr_node.cpp::lookupDynMethodRetType）。
    Node* cur = node->parent();
    FileNode* file = nullptr;
    while (cur) {
        if (auto f = dynamic_cast<FileNode*>(cur)) { file = f; break; }
        cur = cur->parent();
    }

    DraftDeclNode* draftDecl = nullptr;
    std::string draftQualified;
    if (_yux && file && !draftBareName.empty()) {
        auto& reg = _yux->draftRegistry();
        if (auto resolved = reg.resolve(draftBareName, file)) {
            draftDecl = resolved->decl;
            draftQualified = resolved->qualifiedName;
        }
    }
    if (!draftDecl) {
        // E1131: 内层不是已知 draft 名 (可能是结构体 / 类型别名 / 不存在符号)
        throw YuxError(line, col, ErrorCode::E1131,
            draftBareName.empty() ? std::string("?") : draftBareName);
    }

    // E1132: Dyn<Dyn<...>> — 内层 draft 位置不能再是 Dyn
    if (draftInner && draftInner->isDyn()) {
        throw YuxError(line, col, ErrorCode::E1132, resultType.getFullName());
    }

    // E1134: 对象安全
    if (_yux) {
        auto& checker = _yux->draftImplChecker();
        if (!checker.draftIsObjectSafe(draftDecl)) {
            throw YuxError(line, col, ErrorCode::E1134,
                draftQualified, draftQualified, draftQualified);
        }
    }

    // E1133: 参数形态 + U:D 满足
    auto argExpr = node->arg();
    auto argType = argExpr->getType();
    bool isBorrow = node->isBorrow();
    std::string concreteBare;
    if (isBorrow) {
        // Dyn<D&>(x): 接受 U& 或 Rc<U>
        if (argType.isRef()) {
            auto inner = argType.refElementType();
            if (inner) concreteBare = inner->name;
        } else if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    } else {
        // Dyn<D>(x): 仅接受 Rc<U>
        if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    }
    if (concreteBare.empty()) {
        throw YuxError(line, col, ErrorCode::E1133,
            draftQualified, argType.getFullName(), draftQualified);
    }
    if (_yux) {
        auto& checker = _yux->draftImplChecker();
        TypeInfo concreteTI(concreteBare);
        // boundSatisfied 同时覆盖显式 impl (_seen) 与 #DraftLike 结构匹配
        std::vector<TypeInfo> draftTypeArgs;
        if (!checker.boundSatisfied(concreteTI, draftDecl, draftQualified, draftTypeArgs)) {
            throw YuxError(line, col, ErrorCode::E1133,
                draftQualified, argType.getFullName(), draftQualified);
        }
    }

    // ── codegen ──────────────────────────────────────────────────────
    // Phase 3a/3c：vtable 由 getOrEmitDynVTable 合成；data 槽按 Rc<U> / U& 形态抽取。
    // 注：Rc 的 +1 / 借用 RC 半权交接留 Phase 3c.2（消费临时帧 / retain 抵消），
    // 本 Phase 仅落 vtable 真值，临时帧路径与原占位等价。
    auto llvmDynTy = getLLVMType(resultType);

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    auto argVal = compileExpr(argExpr);

    // 抽取 data 槽：Rc<U> 取 handle 字段；U& 直接用
    // Phase 3e RC 交接：
    //   owned (Rc<U>) 形态：源 Rc 若是 fresh 临时（G() 直构），consumeTemp 偷取 +1；
    //     否则（命名变量 / 字段读出）调 _box_retain 拷一份 +1，源 Rc 自己照常 release。
    //     Dyn 在自身 scope 退出时走 _dyn_release 抵消。
    //   borrow (U&) 形态：data_ptr 借用，不动 RC（由源 owner 维持）。
    llvm::Value* dataPtr = nullPtr;
    if (argType.isRc()) {
        // Rc layout = { ptr handle }；handle 指向 [RC head | payload]
        auto rcLLVMTy = getLLVMType(argType);
        auto tmp = _builder.CreateAlloca(rcLLVMTy, nullptr, "dyn.src.rc.tmp");
        _builder.CreateStore(argVal, tmp);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(rcLLVMTy, tmp, {zero, zero}, "dyn.src.handle.ptr");
        dataPtr = _builder.CreateLoad(ptrTy, handleField, "dyn.src.handle");

        // 偷取 fresh Rc 的 +1，否则 retain
        bool consumed = consumeTemp(argVal);
        if (!consumed) {
            _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {dataPtr});
        }
    } else if (argType.isRef()) {
        // U& 已是裸指针类型，直接用
        dataPtr = argVal;
    }

    // vtable 槽：Phase 3a 真值（按 (U, D) 合成 linkonce_odr 全局）
    TypeInfo concreteTI(concreteBare);
    llvm::Value* vtablePtr = getOrEmitDynVTable(concreteTI, draftQualified, draftDecl);
    if (!vtablePtr) vtablePtr = nullPtr;

    // 组装 fat pointer struct value { vtable, data }
    llvm::Value* fatPtr = llvm::UndefValue::get(llvmDynTy);
    fatPtr = _builder.CreateInsertValue(fatPtr, vtablePtr, {0}, "dyn.vtable");
    fatPtr = _builder.CreateInsertValue(fatPtr, dataPtr, {1}, "dyn.fat");

    DEBUG_LOG_VAL("    Expr: DynCtor",
        resultType.getFullName() << " <- " << argType.getFullName());
    return fatPtr;
}

// 编译 Heap:<T>(x) 构造表达式（DRAFT-heap-types §8.3a）
// 形态：单参；arg 求值为 T 值。
// 流程：
// 1. 求值 arg → T 值
// 2. arg 类型必须等于 turbofish 内 T（E3014）
// 3. __yux_heap_alloc(sizeof T) → ptr
// 4. store T 值到 ptr
// 5. 返回 ptr（Heap<T> LLVM 表示 = 裸 T*）
// 注：作用域析构 / 字段析构 / as_ref / 借用检查留 Phase 2.5+
llvm::Value* Compiler::compileHeapCtorExpr(p<ExprHeapCtorNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    int line = node->getLineNumber();
    int col = node->getColumn();

    auto resultType = node->getType();
    auto innerSp = resultType.heapElementType();
    if (!innerSp) {
        throw YuxError(line, col, "Heap:<T>(x) 缺少类型实参");
    }
    auto innerType = *innerSp;

    auto argExpr = node->arg();
    if (isIntTypeName(innerType.name) && isFlexibleIntExpr(argExpr)) {
        tryInferIntType(argExpr, innerType);
    }
    auto argType = argExpr->getType();
    if (!(argType == innerType)) {
        // Sema 已在 visitExpr(ExprHeapCtorNode) 内 shadow 抛 E3028；保留作幂等防御性双跑
        throw YuxError(line, col, ErrorCode::E3028,
            innerType.name, innerType.name, argType.name);
    }

    auto innerLLVMType = getLLVMType(innerType);
    auto argVal = compileExpr(argExpr);

    auto sizeVal = _builder.getInt64(
        _module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
    auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
    auto rawPtr = _builder.CreateCall(allocFn, {sizeVal}, "heap_payload");
    _builder.CreateStore(argVal, rawPtr);
    // fresh 临时 arg 的 +1 / RC 字段所有权转交给 Heap payload；
    // 否则帧弹出时 dtor 会与作用域尾 __yux_heap_free 前的 inner dtor 双释放（含 RC 字段时 use-after-free）
    consumeTemp(argVal);

    DEBUG_LOG_VAL("    Expr: HeapCtor",
        resultType.getFullName() << " <- " << argType.getFullName());
    return rawPtr;
}

// 编译 match 表达式 (Phase 6)
// 形态：match scrutinee { (E::V[(b1,..)] | else) => body ... }
//
// 流程：
// 1. 求值 scrutinee，落 alloca；判定是否拥有所有权（fresh 临时）
// 2. 校验：scrutinee 必须是 enum；arms 穷尽（或带 else）；variant 不重复；
//    arity 匹配；绑定名 arm 内不重复；else 必须最后；arm 体类型一致
// 3. 对每个 arm：开 BB，按位置加载 payload 元素到独立 alloca，注册到 _localVarPtrs
//    + FnNode 符号表 + _scopeVars，编译 body，cleanup（解注册）后跳到 merge
// 4. switch on tag 把入口块路由到各 arm；缺省走 else（或不可达）
// 5. 合并块用 phi 取共同结果（若 hasResult）
// 6. 末了若拥有 scrutinee，调 releaseAtPtr（含 RC 时按 tag dispatch）
//
// v1 限制：
// - 绑定按"借用"语义：不 retain，仅在 scrutinee 存活期间安全使用。要求 scrutinee
//   在整个 match 期间不被覆盖；arm body 不应让绑定逃逸（赋给变量等需要 +1 时
//   依赖普通赋值路径自身的 retain，仅 Rc/Array/Weak 走 compileBranchResultNormalized
//   归一）
// - arm body 仅单表达式（grammar 已限定）；多语句体押后
llvm::Value* Compiler::compileMatchExpr(p<ExprMatchNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto scrutinee = node->scrutinee();
    auto rawScrutType = scrutinee->getType();
    auto scrutType = resolveAlias(rawScrutType);
    int line = node->getLineNumber();
    int col = node->getColumn();

    // Rc<E> match：自动 deref。仅支持借用语义（不接管 Rc 所有权），
    // 因此要求 scrutinee 不是 fresh 来源（避免 Rc 临时立即释放后 enum 悬挂）。
    bool rcDeref = false;
    TypeInfo rcOuterType;
    if (scrutType.isRc()) {
        auto inner = scrutType.rcElementType();
        if (inner) {
            p<FileNode> tmpOwner = nullptr;
            if (lookupEnumDecl(inner->name, tmpOwner)) {
                if (isFreshHandleExpr(scrutinee)) {
                    throw YuxError(line, col, ErrorCode::E2022, scrutType.name)
                        .withHint("不支持对临时 Rc<E> 直接 match；先 `var b Rc<E> = ...` 落地再 match b");
                }
                rcDeref = true;
                rcOuterType = scrutType;
                scrutType = *inner;
            }
        }
    }

    // 1. 必须是 enum
    p<FileNode> enumOwner = nullptr;
    auto enumDecl = lookupEnumDecl(scrutType.name, enumOwner);
    if (!enumDecl) {
        throw YuxError(line, col, ErrorCode::E2022, scrutType.name);
    }
    string enumName = scrutType.name;

    auto& arms = node->arms();

    // Phase 3.4.b: arm 静态校验 (E2019/E2020/E2023/E2024/E2025/E2026/E2027) 整体抠到 sema.
    // SemaPass 在 scrut 直接是 enum 名 (非 Rc/非 alias) 时已先抛; 这里是幂等防御性双跑.
    sema::validateMatchArms(enumDecl, enumName, node, _file);

    // 重新收集 codegen 需要的状态 (helper 已校验合法性, 这里只做记录)
    set<string> seenVariants;
    bool hasElse = false;
    for (auto& arm : arms) {
        auto pat = arm->pattern();
        if (pat->isElse()) {
            hasElse = true;
            continue;
        }
        seenVariants.insert(pat->variantName().getText());
    }

    // 3. 结果类型一致性
    TypeInfo resultType;
    bool firstSet = false;
    for (auto& arm : arms) {
        auto t = arm->body()->getType();
        if (!firstSet) {
            resultType = t;
            firstSet = true;
            continue;
        }
        if (t != resultType) {
            throw YuxError(arm->body()->resolveLineNumber(), arm->body()->resolveColumn(),
                ErrorCode::E3027, resultType.name, t.name);
        }
    }
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL("    Expr: Match",
        "enum=" << enumName << " arms=" << arms.size() << " hasResult=" << hasResult
                << " result=" << (hasResult ? resultType.name : "void"));

    // 4. 求值 scrutinee 并落 alloca；fresh 时 consume 拿走所有权
    auto enumLLVMType = getLLVMType(scrutType);
    if (!enumLLVMType) {
        throw YuxError(line, col, ErrorCode::E3096, enumName);
    }

    auto scrutVal = compileExpr(scrutinee);
    if (!scrutVal) {
        throw YuxError(line, col, ErrorCode::E3091);
    }
    auto scrutAlloca = _builder.CreateAlloca(enumLLVMType, nullptr, "match.scrut");
    if (rcDeref) {
        // scrutVal 是 Rc<E>（{ ptr handle }）：取 handle → +8 跳过 RC 头 → 读 enum 值
        auto rcLLVMType = getLLVMType(rcOuterType);
        auto rcAlloca = _builder.CreateAlloca(rcLLVMType, nullptr, "match.rc");
        _builder.CreateStore(scrutVal, rcAlloca);
        auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(
            rcLLVMType, rcAlloca, {zero32, zero32}, "match.rc.handle_field");
        auto handle = _builder.CreateLoad(
            llvm::PointerType::get(_context, 0), handleField, "match.rc.handle");
        auto payload = _builder.CreateGEP(
            _builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "match.rc.payload");
        auto enumVal = _builder.CreateLoad(enumLLVMType, payload, "match.rc.enum");
        _builder.CreateStore(enumVal, scrutAlloca);
    } else {
        _builder.CreateStore(scrutVal, scrutAlloca);
    }

    // 仅当 scrutinee 是 fresh（构造 / 函数返回 / 含 RC 的 enum 临时）我们才需要在 match 末 dtor
    // Rc deref 路径走借用语义，不接管 Rc 所有权，故不计 drop
    bool ownsScrut = !rcDeref && isFreshHandleExpr(scrutinee);
    if (ownsScrut) {
        consumeTemp(scrutVal);
    }
    bool needScrutDrop = ownsScrut && enumNeedsDestructor(enumName);

    // 5. 构造基本块
    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto mergeBB = llvm::BasicBlock::Create(_context, "match.merge");

    // 各 arm BB（与 arm 索引一一对应；else arm 也是其中之一）
    vector<llvm::BasicBlock*> armBBs;
    armBBs.reserve(arms.size());
    for (size_t i = 0; i < arms.size(); ++i) {
        auto bb = llvm::BasicBlock::Create(_context, "match.arm" + std::to_string(i));
        armBBs.push_back(bb);
    }

    // tag load + switch
    auto tagPtr = _builder.CreateStructGEP(enumLLVMType, scrutAlloca, 0, "match.tag.ptr");
    auto tag = _builder.CreateLoad(_builder.getInt32Ty(), tagPtr, "match.tag");

    // default 块：若有 else arm 则跳到它；否则跳到 unreachable（穷尽性已保证不会到这里）
    llvm::BasicBlock* defaultBB = nullptr;
    if (hasElse) {
        defaultBB = armBBs.back();
    } else {
        defaultBB = llvm::BasicBlock::Create(_context, "match.default");
    }

    auto sw = _builder.CreateSwitch(tag, defaultBB, static_cast<unsigned>(seenVariants.size()));

    // 把每条非-else arm 的 variant 接进 switch
    for (size_t i = 0; i < arms.size(); ++i) {
        auto pat = arms[i]->pattern();
        if (pat->isElse()) continue;
        int idx = enumDecl->variantIndex(pat->variantName().getText());
        sw->addCase(_builder.getInt32(idx), armBBs[i]);
    }

    // 6. 编译每个 arm
    llvm::PHINode* phi = nullptr;
    if (hasResult) {
        phi = llvm::PHINode::Create(getLLVMType(resultType),
            static_cast<unsigned>(arms.size()), "match.result", mergeBB);
    }

    auto resultLLVMType = hasResult ? getLLVMType(resultType) : nullptr;

    for (size_t i = 0; i < arms.size(); ++i) {
        auto arm = arms[i];
        auto pat = arm->pattern();
        func->insert(func->end(), armBBs[i]);
        _builder.SetInsertPoint(armBBs[i]);

        // 准备绑定（仅非-else arm）
        struct BindSnap {
            string name;
            bool hadSym;
            SymbolInfo prevSym;
            bool hadPtr;
            llvm::Value* prevPtr;
        };
        vector<BindSnap> snaps;

        if (!pat->isElse() && !pat->binds().empty()) {
            string vName = pat->variantName().getText();
            auto* variant = enumDecl->variant(vName);
            // 重建 variant payload tuple struct
            vector<llvm::Type*> elemTys;
            elemTys.reserve(variant->payloadTypes().size());
            for (auto t : variant->payloadTypes()) {
                elemTys.push_back(getLLVMType(t->getType()));
            }
            auto payloadStruct = llvm::StructType::get(_context, elemTys);
            auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, scrutAlloca, 1,
                "match.payload.ptr");

            for (size_t k = 0; k < pat->binds().size(); ++k) {
                const string& bn = pat->binds()[k].getText();
                auto bindType = variant->payloadTypes()[k]->getType();
                auto bindLLVMType = getLLVMType(bindType);

                auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr,
                    static_cast<unsigned>(k), "match.bind.field");
                auto loaded = _builder.CreateLoad(bindLLVMType, fieldPtr,
                    ("match.bind." + bn).c_str());

                // 独立 alloca，便于 compileLiteralExpr 通过 _localVarPtrs 取出
                auto bindAlloca = _builder.CreateAlloca(bindLLVMType, nullptr,
                    ("bind." + bn).c_str());
                _builder.CreateStore(loaded, bindAlloca);

                BindSnap snap;
                snap.name = bn;
                auto* prev = _currentFnNode->lookupSymbol(bn);
                snap.hadSym = (prev != nullptr);
                if (prev) snap.prevSym = *prev;
                auto pit = _localVarPtrs.find(bn);
                snap.hadPtr = (pit != _localVarPtrs.end());
                snap.prevPtr = snap.hadPtr ? pit->second : nullptr;

                _currentFnNode->registerSymbol(bn,
                    {SymbolKind::Variable, bn, bindType, false});
                _localVarPtrs[bn] = bindAlloca;

                snaps.push_back(snap);
            }
        }

        // 编译 body：RC 句柄需归一到 +1 的部分由 compileBranchResultNormalized 处理
        llvm::Value* bodyVal = nullptr;
        if (hasResult) {
            bodyVal = compileBranchResultNormalized(arm->body(), resultType);
        } else {
            // 作为语句：仍走 compileExpr，吃掉中间 fresh 临时
            pushTempFrame();
            (void)compileExpr(arm->body());
            popAndReleaseTempFrame();
        }

        auto armEndBB = _builder.GetInsertBlock();

        // cleanup 绑定（先恢复 _currentFnNode 符号 / _localVarPtrs；
        // 绑定按借用语义，不在 alloca 上 release）
        for (auto it = snaps.rbegin(); it != snaps.rend(); ++it) {
            if (it->hadSym) {
                _currentFnNode->registerSymbol(it->name, it->prevSym);
            } else {
                _currentFnNode->eraseSymbol(it->name);
            }
            if (it->hadPtr) {
                _localVarPtrs[it->name] = it->prevPtr;
            } else {
                _localVarPtrs.erase(it->name);
            }
        }

        if (hasResult && phi && bodyVal) {
            // 类型再校验（防御）
            phi->addIncoming(bodyVal, armEndBB);
        }

        // 跳到 merge（终结块若已有 terminator 则跳过）
        if (!_builder.GetInsertBlock()->getTerminator()) {
            _builder.CreateBr(mergeBB);
        }
    }

    // 7. 无 else 时 default 走 unreachable（穷尽性应保证不可达）
    if (!hasElse) {
        func->insert(func->end(), defaultBB);
        _builder.SetInsertPoint(defaultBB);
        _builder.CreateUnreachable();
    }

    // 8. merge
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    // 9. 拥有 scrutinee 且需要 dtor：在 merge 后释放
    if (needScrutDrop) {
        releaseAtPtr(scrutAlloca, scrutType);
    }

    if (hasResult) {
        // 若 phi 为空（理论不会，arms 至少 1）兜底 undef
        if (phi->getNumIncomingValues() == 0) {
            return llvm::UndefValue::get(resultLLVMType);
        }
        // RC 句柄结果由 compileBranchResultNormalized 已归一为 fresh +1，
        // 登记到外层 statement frame
        if (resultType.isRc() || resultType.isArrayGeneric() || resultType.isWeak()) {
            recordTemp(phi, resultType);
        }
        return phi;
    }
    return nullptr;
}

// Phase 10f / 10g：try-catch 表达式编译（DRAFT-错误.md [#4.H]）
//
// 10g-5/6 实施后，错误通道路由 + 表达式合并已落地：
//   - 进入 try 前为每个 catch arm 预分配 entry BB + e alloca（类型 = arm errType 的 enum）
//   - 把这些信息塞进 TryCatchCtx 推入 _tryCatchStack；compileCallExpr 内的
//     handleFallibleCallResult 在 callee 返回 isErr=1 时按 callee 错误类型查 catchTypes，
//     命中即 store ErrEnum 到 e alloca 后跳到对应 arm entry（不退出当前 fn）
//   - try 成功路径末尾跳到 join；每个 arm body 末尾跳到 join（流终止 arm 不参与 phi）
//   - 表达式结果通过 phi 在 join 合并
//
// 静态语义校验（[#4.H] 规则表 + [#5.B] 错误码）：
//   - E7011：catch 类型必须是已声明 enum；
//   - E7002：try block 内 callee 错误类型未被任一 catch 子句覆盖（穷尽性）；
//   - E7010：catch arm body 末表达式类型与 try block 一致（流终止 arm 不参与）；
//   - E7015 / E7016 / E7017 / E7018：警告类，TODO 接入诊断分级。
//
// 穷尽性收集策略（[#4.H] "穷尽性不下钻 lambda body"）：
//   - 编译 try block 时把当前 try ctx 压入 _tryCatchStack；
//   - compileCallExpr 检测到 #Fallible callee 时把 callee 错误类型 append 到 ctx.seenErrTypes；
//   - lambda body 在 emitLambdaFunction 内有独立编译流，与外层 _tryCatchStack 隔离 →
//     穷尽性自然不下钻 lambda 内部。
llvm::Value* Compiler::compileTryCatchExpr(p<ExprTryCatchNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    int line = node->getLineNumber();
    int col = node->getColumn();

    auto func = _builder.GetInsertBlock()->getParent();

    // 1) 校验所有 catch 子句的错误类型必须是已声明 enum（E7011）
    //    同时为每个 arm 预分配 entry BB + e alloca
    TryCatchCtx ctx;
    ctx.catchTypes.reserve(node->catches().size());
    ctx.armEntryBBs.reserve(node->catches().size());
    ctx.armEAllocas.reserve(node->catches().size());

    for (auto& arm : node->catches()) {
        const string& errType = arm->errType();
        p<FileNode> owner = nullptr;
        auto enumDecl = lookupEnumDecl(errType, owner);
        if (!enumDecl) {
            int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
            int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
            throw YuxError(aline, acol, ErrorCode::E7011,
                arm->errName().getText(), errType, errType);
        }
        ctx.catchTypes.push_back(errType);

        // 为 e 绑定分配 alloca（类型 = enum）；命名带 arm 错误名便于 IR 阅读
        const string& bn = arm->errName().getText();
        auto eAlloca = _builder.CreateAlloca(getLLVMType(TypeInfo(errType)),
            nullptr, ("catch.e." + bn).c_str());
        ctx.armEAllocas.push_back(eAlloca);

        // arm entry BB 暂不插入 func；编译 arm body 时再 insert
        auto armBB = llvm::BasicBlock::Create(_context, "catch.arm");
        ctx.armEntryBBs.push_back(armBB);
    }

    // join BB（try 成功路径 + 各 arm 末尾汇合）
    auto joinBB = llvm::BasicBlock::Create(_context, "trycatch.join");

    // 2) 编译 try block，_tryCatchStack 顶为本 try 的 ctx
    _tryCatchStack.push_back(ctx);
    auto& tryBlock = node->tryBlock();
    for (auto& stmt : tryBlock->statements()) {
        compileStatement(stmt);
    }

    bool hasResult = tryBlock->hasResult();
    TypeInfo resultType;
    llvm::Value* tryResult = nullptr;
    if (hasResult) {
        try { resultType = tryBlock->resultExpr()->getType(); } catch (...) {}
        tryResult = compileExpr(tryBlock->resultExpr());
    }

    auto trySuccessEndBB = _builder.GetInsertBlock();

    // 取出 seenErrTypes 后出栈
    TryCatchCtx finishedCtx = std::move(_tryCatchStack.back());
    _tryCatchStack.pop_back();

    // 3) 穷尽性 E7002
    for (auto& seen : finishedCtx.seenErrTypes) {
        bool covered = false;
        for (auto& ct : ctx.catchTypes) {
            if (ct == seen) { covered = true; break; }
        }
        if (!covered) {
            throw YuxError(line, col, ErrorCode::E7002, seen, string("<unknown>"), seen);
        }
    }

    // 4) E7015 / E7017：警告类，TODO

    // try 成功路径末尾跳 join（若未被流终止语句抢占 terminator）
    vector<std::pair<llvm::Value*, llvm::BasicBlock*>> phiIncoming;
    if (!trySuccessEndBB->getTerminator()) {
        if (hasResult && tryResult) {
            phiIncoming.emplace_back(tryResult, trySuccessEndBB);
        }
        _builder.SetInsertPoint(trySuccessEndBB);
        _builder.CreateBr(joinBB);
    }

    // 5) 编译每个 catch arm
    auto resultLLVMType = hasResult ? getLLVMType(resultType) : nullptr;

    for (size_t i = 0; i < node->catches().size(); ++i) {
        auto& arm = node->catches()[i];
        auto armBB = ctx.armEntryBBs[i];
        func->insert(func->end(), armBB);
        _builder.SetInsertPoint(armBB);

        // 注册 e 绑定到 _localVarPtrs（CatchArmNode 在 ast_builder 阶段已注册符号到 ScopeNode）
        const string& bn = arm->errName().getText();
        auto pit = _localVarPtrs.find(bn);
        bool hadPtr = (pit != _localVarPtrs.end());
        llvm::Value* prevPtr = hadPtr ? pit->second : nullptr;
        _localVarPtrs[bn] = ctx.armEAllocas[i];

        // 编译 arm body 语句序列
        for (auto& stmt : arm->body()->statements()) {
            compileStatement(stmt);
        }

        llvm::Value* armResult = nullptr;
        if (arm->body()->hasResult()) {
            // E7010：arm result 表达式类型 == try block result 类型
            try {
                auto armT = arm->body()->resultExpr()->getType();
                if (hasResult && armT != resultType) {
                    int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                    int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                    throw YuxError(aline, acol, ErrorCode::E7010,
                        armT.name, resultType.name);
                }
            } catch (const YuxError&) { throw; }
              catch (...) {}
            armResult = compileExpr(arm->body()->resultExpr());
        }

        auto armEndBB = _builder.GetInsertBlock();

        // 还原 _localVarPtrs（CatchArmNode 自身的符号 entry 由 ScopeNode 持有，无需手动撤）
        if (hadPtr) _localVarPtrs[bn] = prevPtr;
        else _localVarPtrs.erase(bn);

        // 跳 join（若未被流终止抢占 terminator）
        if (!armEndBB->getTerminator()) {
            if (hasResult && armResult) {
                phiIncoming.emplace_back(armResult, armEndBB);
            }
            _builder.CreateBr(joinBB);
        }
    }

    // 6) join BB
    func->insert(func->end(), joinBB);
    _builder.SetInsertPoint(joinBB);

    // 表达式合并：若有结果，phi；若所有路径都流终止，joinBB 不可达
    if (hasResult && !phiIncoming.empty()) {
        auto phi = _builder.CreatePHI(resultLLVMType,
            static_cast<unsigned>(phiIncoming.size()), "trycatch.result");
        for (auto& inc : phiIncoming) {
            phi->addIncoming(inc.first, inc.second);
        }
        return phi;
    }
    if (hasResult) {
        // 所有分支都流终止，joinBB 不可达；emit unreachable 防 verifier
        _builder.CreateUnreachable();
        return llvm::UndefValue::get(resultLLVMType);
    }
    return nullptr;
}

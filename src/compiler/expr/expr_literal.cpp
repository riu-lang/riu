// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 字面量 / 数组 / 元组表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/enum_node.h"
#include "../compiler_runtime.h"
#include "ast/mangler.h"
#include "ast/yux.h"
#include "analyzer/symbol_suggest.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <set>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <cassert>

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
            std::array<llvm::Value*, 2> indices{zero, index};
            auto elemPtr = _builder.CreateGEP(llvmArrayType, destPtr, indices, "array.elem.ptr");
            _builder.CreateStore(fillValue, elemPtr);
        }
    }

    if (needLoad) {
        return _builder.CreateLoad(llvmArrayType, destPtr, "array.load");
    }
    return nullptr;
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
            // Phase 3e: Heap<T>? 接受按所有权 move 捕获 (B 档复用); Heap<T> 非空
            // 按值捕获 → E4024 (与 §5.2/§5.3 一致, 引导用户声明为可空形态)
            bool isHeapNullable = false;
            if (t.isNullable()) {
                auto inner = t.nullableInnerType();
                if (inner && inner->isHeap()) isHeapNullable = true;
            }
            if (t.isHeap()) {
                auto elem = t.heapElementType();
                string elemName = elem ? elem->getFullName() : string("?");
                throw YuxError(node->getLineNumber(), node->getColumn(),
                               ErrorCode::E4024, elemName, varName, elemName);
            }
            if (!isScalar && !isHandle && !isRef && !isHeapNullable) {
                throw YuxError(node->getLineNumber(), node->getColumn(),
                               ErrorCode::E2029, varName, t.name);
            }
            // 已捕获 → 复用槽位；首次 → 追加
            // 槽位字节数: handle 形态 / 标量 / T& 都是 8; Heap<T>? = {i1, ptr} 实际 16
            // 按 DataLayout 取真实 allocSize, 至少 8 字节对齐, 防 Heap<T>? 与下一个
            // capture 槽位重叠覆盖.
            int idx = _currentLambdaForCapture->findCapture(varName);
            if (idx < 0) {
                u64 offset = _currentLambdaForCapture->capturesTotalSize();
                u64 slotSize = 8;
                if (isHeapNullable) {
                    auto llvmTy = getLLVMType(t);
                    auto rawSize = _module->getDataLayout()
                                       .getTypeAllocSize(llvmTy).getFixedValue();
                    slotSize = rawSize < 8 ? 8 : rawSize;
                }
                idx = _currentLambdaForCapture->addCapture(varName, t, offset, offset + slotSize);
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
            auto payloadOffset = _builder.getInt64(16 + static_cast<i64>(cap.byteOffset));
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

    // Phase 2b 类型校验：插值类型必须是 String 或实现 ToString.
    // Bucket 6 (CURRENT-check.md): 抠到 sema::validateStringTemplateInterps;
    // SemaPass 已接管 E3026 实际抛出点, 此处幂等防御性双跑.
    sema::validateStringTemplateInterps(_file,
                                         _yux ? _yux->sdkFile() : nullptr,
                                         node);

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

    // 1. alloca StringBuilder + 调静态工厂 (Phase 6: 砍 ctor, 走 StringBuilder::make())
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "tpl_sb");
    {
        vector<TypeInfo> noArgs;
        auto mkFn = getMethodFunction("StringBuilder", "make", noArgs, TypeInfo("StringBuilder"),
                                      /*fallibleErrType=*/"", /*isStatic=*/true);
        auto sbVal = _builder.CreateCall(mkFn, {}, "tpl_sb.init");
        _builder.CreateStore(sbVal, sbPtr);
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
        std::ranges::reverse(leaves);
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

    // 3. alloca StringBuilder + 调静态工厂 (Phase 6: 砍 ctor, 走 StringBuilder::make())
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "plus_sb");
    {
        vector<TypeInfo> noArgs;
        auto mkFn = getMethodFunction("StringBuilder", "make", noArgs, TypeInfo("StringBuilder"),
                                      /*fallibleErrType=*/"", /*isStatic=*/true);
        auto sbVal = _builder.CreateCall(mkFn, {}, "plus_sb.init");
        _builder.CreateStore(sbVal, sbPtr);
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
        std::array<llvm::Value*, 2> indices{zero, index};
        auto elemPtr = _builder.CreateGEP(llvmArrayType, alloca, indices, "array.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
    }

    return _builder.CreateLoad(llvmArrayType, alloca, "array.load");
}

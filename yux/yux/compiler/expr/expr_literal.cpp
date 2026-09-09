// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 字面量 / 数组 / 元组表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <set>

// ==================== 数组初始化表达式编译 ====================

// 编译数组填充表达式 ([value ... Type] 语法)
// 使用指定值填充整个数组
llvm::Value* Compiler::compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType,
                                            llvm::Value* destPtr) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto literal = node->value();
    auto literalType = literal->getType();
    auto text = literal->getValue().getText();

    // 确定元素类型。E3009 由 SemaPass 带 target-type 检查。
    TypeInfo elementType;
    if (node->explicitType()) {
        elementType = node->explicitType()->getType();
    } else {
        elementType = literalType;
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
        isZeroFill = (intFillVal == 0); // 零值优化
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
        isZeroFill = (floatFillVal == 0.0); // 零值优化
    } else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        intFillVal = boolVal ? 1 : 0;
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), intFillVal, false);
        isZeroFill = !boolVal; // false 值优化
    } else {
        // E3080 由 SemaPass checkArrayInit 先抛；此处防 IR 对 string / 变量 fill memset。
        throwSemaGap(node->getLineNumber(), node->getColumn());
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
        SymbolInfo* sym = nullptr;
        if (node->hasResolvedSymbol() && node->resolvedSymbol().isVar()) {
            sym = node->resolvedSymbol().var;
        } else {
            // Phase 2b：lambda body 编译期 _currentFnNode 为 nullptr，但 body 节点的 parent
            // 链能经 bodyScope 找到 lambda 形参；fall back 到 findNearestScope 让 lambda 形参
            // 与外层局部都能查到（外层情况下两者等价）
            sym = lookupVarSymbol(varName, node);
            if (sym) node->setResolvedVar(sym);
        }

        // Phase B-1: E4033 use-after-move 检查已迁入 SemaPass，Compiler 端不再重复。

        if (sym && _localVarPtrs.contains(varName)) {
            DEBUG_LOG_VAL("    Expr: VariableLoad", varName << " : " << sym->type.name);
            // Phase 4a: T& 借用 — _localVarPtrs[name] 是底层 T 的地址（参数/局部统一），自动解引用
            if (sym->type.isRef()) {
                auto innerType = sym->type.refElementType();
                return _builder.CreateLoad(getLLVMType(*innerType), _localVarPtrs[varName]);
            }
            return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[varName]);
        }

        // 函数名作为值使用（非调用）：构造 fat-ptr { fn_ptr, null }
        // 普通函数编译为 (P1..Pn)→R ABI（无 captures 参数），但 fn-value 调用期
        // 望 captures-leading ABI (ptr, P1..Pn)→R。此处按需合成 thunk 桥接二者。
        if (sym && sym->kind == SymbolKind::Function) {
            auto* fnSym = _currentFnNode ? _currentFnNode->lookupFnSymbol(varName) : nullptr;
            if (!fnSym) {
                auto sc = node->findNearestScope();
                if (sc) fnSym = sc->lookupFnSymbol(varName);
            }
            if (fnSym) {
                string ownerMod = fnSym->moduleName.empty() ? _file->moduleName() : fnSym->moduleName;
                bool isPriv = !varName.empty() && varName[0] == '_';
                string fnMangled =
                    mangleFunction(ownerMod, varName, fnSym->params, isPriv, fnSym->retType, fnSym->fallibleErrType);
                auto func = _module->getFunction(fnMangled);
                if (func) {
                    DEBUG_LOG_VAL("    Expr: FunctionValue (fat-ptr)", varName << " -> " << fnMangled);
                    auto ptrTy = llvm::PointerType::get(_context, 0);

                    // 检查是否需要 ABI 适配 thunk：普通函数的 LLVM 签名不含 captures
                    // 首参，参数个数 == fnSym->params.size()；captures-leading 则会多 1。
                    auto* funcTy = func->getFunctionType();
                    auto fnValuePtr = static_cast<llvm::Value*>(func);
                    if (funcTy->getNumParams() == fnSym->params.size()) {
                        string thunkName = "__fn_thunk_" + fnMangled;
                        auto thunk = _module->getFunction(thunkName);
                        if (!thunk) {
                            vector<llvm::Type*> thunkParams;
                            thunkParams.push_back(ptrTy); // captures
                            for (auto& p : fnSym->params)
                                thunkParams.push_back(getLLVMType(p));
                            auto* thunkTy = llvm::FunctionType::get(funcTy->getReturnType(), thunkParams, false);
                            thunk =
                                llvm::Function::Create(thunkTy, llvm::Function::InternalLinkage, thunkName, _module);
                            DEBUG_LOG_VAL("    -> generated thunk", thunkName);
                            // 生成 thunk body：忽略 captures，forward 其余实参到原函数
                            auto* savedBB = _builder.GetInsertBlock();
                            auto savedIP = _builder.GetInsertPoint();
                            auto* bb = llvm::BasicBlock::Create(_context, "entry", thunk);
                            _builder.SetInsertPoint(bb);
                            vector<llvm::Value*> fwdArgs;
                            for (unsigned i = 1; i < thunk->arg_size(); ++i)
                                fwdArgs.push_back(thunk->getArg(i));
                            auto* retVal = _builder.CreateCall(func, fwdArgs);
                            if (funcTy->getReturnType()->isVoidTy())
                                _builder.CreateRetVoid();
                            else
                                _builder.CreateRet(retVal);
                            if (savedBB) _builder.SetInsertPoint(savedBB, savedIP);
                        }
                        fnValuePtr = thunk;
                    }

                    auto fatStructTy = llvm::StructType::get(_context, {ptrTy, ptrTy});
                    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);
                    llvm::Value* fat = llvm::UndefValue::get(fatStructTy);
                    fat = _builder.CreateInsertValue(fat, fnValuePtr, {0}, "fn.ptr");
                    fat = _builder.CreateInsertValue(fat, nullPtr, {1}, "fn.captures");
                    return fat;
                }
            }
        }

        string ownerMod = (sym && !sym->moduleName.empty()) ? sym->moduleName : _file->moduleName();
        bool globPriv = !varName.empty() && varName[0] == '_';
        string mangledName = Mangler::global(ownerMod, varName, globPriv);

        // #Inline #Cval：优先查 _inlineConstantValues 表，命中则直接返回常量值（无 Load 指令），
        // 实现 C #define 风格的内联替换。该表由 compileGlobalConsts 在遇到 #Inline 标注的
        // #Cval 时填充（不创建 GlobalVariable）。
        auto inlineIt = _inlineConstantValues.find(mangledName);
        if (inlineIt != _inlineConstantValues.end()) {
            DEBUG_LOG_VAL("    Expr: InlineConst",
                          varName << " : " << (sym ? sym->type.name : "unknown") << " [direct constant, no load]");
            return inlineIt->second;
        }

        auto globalVar = _module->getGlobalVariable(mangledName, true);
        // 跨文件常量：当前模块中不存在 GlobalVariable 定义，但 lookupSymbol 已在
        // 父作用域（经 _sdkFile → wildcardImports）找到符号。仿照函数调用的跨文件
        // 模式创建外部声明（ExternalLinkage + nullptr initializer），由 LLD 链接时
        // 解析到定义所在 .obj。
        // 仅当不在 lambda body 捕获上下文时才创建——lambda 捕获的外层局部变量
        // 走下方 _currentLambdaForCapture 分支，不应误创为全局常量。
        if (!globalVar && sym && !(_currentLambdaForCapture && _currentLambdaBodyScope)) {
            auto llvmType = getLLVMType(sym->type);
            globalVar = new llvm::GlobalVariable(*_module, llvmType, true, llvm::GlobalValue::ExternalLinkage, nullptr,
                                                 mangledName);
            DEBUG_LOG_VAL("    Expr: GlobalConstDecl (cross-file)", varName << " : " << sym->type.name);
        }
        if (globalVar) {
            DEBUG_LOG_VAL("    Expr: GlobalConstLoad", varName << " : " << (sym ? sym->type.name : "unknown"));
            return _builder.CreateLoad(globalVar->getValueType(), globalVar, "global.load");
        }

        if (_currentFnNode) {
            // E3030 由 SemaPass getType 先抛。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        // lambda body 内引用外层 local：Phase 4a / 4a-2 闭包识别（spec §6.1 / §6.2）
        // - 找到 sym 但不在 _localVarPtrs 也无 globalVar → 外层 local
        // - 4a 接收 builtin 标量；4a-2 接收 8-byte 堆句柄包装（Rc / Weak / Array<T> / String）
        // - 其余（struct / enum / Fn fat-ptr / T&）仍报 E2029（4c+ 接入）
        // - 命中：addCapture（首次出现）+ 生成 GEP 读 captures buffer
        // captures Rc payload 布局：[0..8] dtor fn ptr，[8..] capture 字段（4a-2 引入 dtor 槽）
        if (_currentLambdaForCapture && _currentLambdaBodyScope && sym && sym->kind == SymbolKind::Variable) {
            const auto& t = sym->type;
            bool isScalar = t.isNormal() && isBuiltinType(t.name);
            bool isHandle = t.isRcHandle();
            bool isRef = t.isRef();
            bool isHeapNullable = false;
            if (t.isNullable()) {
                auto inner = t.nullableInnerType();
                if (inner && inner->isHeap()) isHeapNullable = true;
            }
            if (!isScalar && !isHandle && !isRef && !isHeapNullable) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            // 已捕获 → 复用槽位；首次 → 追加
            // 槽位字节数: handle 形态 / 标量 / T& 都是 8; Heap<T>? = {i1, ptr} 实际 16
            // 按 DataLayout 取真实 allocSize, 至少 8 字节对齐, 防 Heap<T>? 与下一个
            // capture 槽位重叠覆盖.
            int idx = _currentLambdaForCapture->findCapture(varName);
            if (idx < 0) {
                u64 offset = _currentLambdaForCapture->capturesTotalSize();
                // 槽位字节数按实际 LLVM allocSize 计算，而非硬编码 8：
                // Rc/Weak/String = 8 字节，但 Array<T> = 24 字节（{ptr,i64,i64}），
                // Heap<T>? = 16 字节（{i1,ptr}）。统一走 getTypeAllocSize 防溢出。
                auto llvmTy = getLLVMType(t);
                auto rawSize = _module->getDataLayout().getTypeAllocSize(llvmTy).getFixedValue();
                u64 slotSize = rawSize < 8 ? 8 : rawSize;
                // 注：isHeapNullable 同样走上述逻辑，不再另分支
                (void)isHeapNullable;
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
            auto capAddr = _builder.CreateGEP(i8Ty, _currentLambdaCapturesArg, {payloadOffset}, "cap.addr");
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
        // 兜底：lambda body 命中 sym 但捕获通路未启。E2028 为历史码；sema 报 E2029。
        if (_currentLambdaBodyScope && sym && sym->kind == SymbolKind::Variable) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        throwSemaGap(node->getLineNumber(), node->getColumn());
    } else if (auto cpLiteral = dynamic_cast<LiteralCodePointNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: CodePointLiteral", text << " : u32");
        return llvm::ConstantInt::get(getLLVMType(type), cpLiteral->codePoint(), false);
    } else if (auto nullLiteral = dynamic_cast<LiteralNullNode*>(literal)) {
        DEBUG_LOG("    Expr: NullLiteral");
        // 若已推断为 Nullable<T>，生成 { _has=false, _value=zeroinit } 结构体常量
        if (nullLiteral->hasInferredType() && nullLiteral->getType().isNullable()) {
            auto nullableType = nullLiteral->getType();
            auto innerType = nullableType.nullableInnerType();
            auto llvmStructType = getLLVMType(nullableType);
            auto innerLLVMType = innerType ? getLLVMType(*innerType) : nullptr;
            if (llvmStructType && innerLLVMType && llvmStructType->isStructTy()) {
                std::vector<llvm::Constant*> fields = {
                    llvm::ConstantInt::get(_builder.getInt1Ty(), 0), // _has = false
                    llvm::Constant::getNullValue(innerLLVMType)      // _value = zeroinit
                };
                DEBUG_LOG("    -> Inferred Nullable<T>: emitting zeroinit struct constant");
                return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvmStructType), fields);
            }
        }
        // 未推断或非 Nullable 目标：保持原有 Ptr 行为
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0));
    } else if (auto stringLiteral = dynamic_cast<LiteralStringNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: StringLiteral", text);
        return emitStringLiteralValue(stringLiteral->codePoints());
    } else if (auto tplLiteral = dynamic_cast<StringTemplateNode*>(literal)) {
        return compileStringTemplate(tplLiteral);
    }
    throwSemaGap(node->getLineNumber(), node->getColumn());
}

// B-4: 由码点向量发射 sentinel RC Block 全局常量。
// Block layout: { u32 strong(0xFFFFFFFF), u32 weak(0), Array<u32> payload }
// Array<u32> payload: { ptr _data, i64 _len, i64 _cap }
// Block LLVM 类型: {i32, i32, ptr, i64, i64}（扁平化，与运行时 GEP 字节偏移兼容）
// 返回 GlobalVariable*，指向 sentinel block。不依赖 _builder 当前 BB。
llvm::GlobalVariable* Compiler::emitStringRcBlockConst(const vector<u32>& codePoints) {
    size_t len = codePoints.size();

    auto i32Ty = llvm::Type::getInt32Ty(_context);
    auto sizeTy = getSizeType();
    auto ptrTy = llvm::PointerType::get(_context, 0);

    // Sentinel RC Block: { u32 strong @0, u32 weak @4, Array<u32>={ptr _data,usize _len,usize _cap} @8 }
    // 与 compiler_runtime.cpp 内各 emit 函数的 block+4/block+8 字节偏移须保持同步。
    auto blockTy = llvm::StructType::get(_context, {i32Ty, i32Ty, ptrTy, sizeTy, sizeTy});

    // 数据缓冲：len > 0 时铺常量 u32 数组，否则用 null。
    llvm::Constant* dataConst = llvm::ConstantPointerNull::get(ptrTy);
    if (len > 0) {
        auto arrType = llvm::ArrayType::get(i32Ty, len);
        vector<llvm::Constant*> elements;
        elements.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            elements.push_back(llvm::ConstantInt::get(i32Ty, codePoints[i]));
        }
        auto arrInit = llvm::ConstantArray::get(arrType, elements);

        string dataName = ".str.data." + to_string(_strDataCounter++);
        auto* dataGV = new llvm::GlobalVariable(*_module, arrType, /*isConstant=*/true,
                                                llvm::GlobalValue::PrivateLinkage, arrInit, dataName);
        dataConst = dataGV;
    }

    auto sentinelStrong = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
    auto sentinelWeak = llvm::ConstantInt::get(i32Ty, 0);
    auto lenC = llvm::ConstantInt::get(sizeTy, len);
    auto blockInit = llvm::ConstantStruct::get(blockTy, {sentinelStrong, sentinelWeak, dataConst, lenC, lenC});

    // 空字面量：同一个空 sentinel block 全局复用（per-module）
    if (len == 0) {
        if (!_strEmptyBlock) {
            _strEmptyBlock = new llvm::GlobalVariable(*_module, blockTy, /*isConstant=*/true,
                                                      llvm::GlobalValue::PrivateLinkage, blockInit, ".str.empty_block");
        }
        return _strEmptyBlock;
    }

    string blockName = ".str.rc." + to_string(_strBlockCounter++);
    return new llvm::GlobalVariable(*_module, blockTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                    blockInit, blockName);
}

// 由码点向量发射 String 值。
// B-4: String layout = { _buf Rc<Array<u32>> } = { { ptr handle } }
// LiteralStringNode 与 StringTemplateNode（template parts）共用此发射路径。
llvm::Value* Compiler::emitStringLiteralValue(const vector<u32>& codePoints) {
    auto* blockGV = emitStringRcBlockConst(codePoints);

    // String = { Rc<Array<u32>> } = { { ptr handle } }
    auto stringType = getLLVMType(TypeInfo("String"));
    auto alloca = _builder.CreateAlloca(stringType, nullptr, "str_tmp");
    auto zero32 = _builder.getInt32(0);
    // GEP: String → field 0 (Rc<Array<u32>>) → field 0 (handle)
    auto handleField = _builder.CreateGEP(stringType, alloca, {zero32, zero32, zero32}, "str.handle");
    _builder.CreateStore(blockGV, handleField);
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

    // UTF-8 → u32 码点解码（parts 在 ast_builder 已展开转义，仅含原始 UTF-8 字节）
    auto decodeUtf8 = [](const string& s) -> vector<u32> {
        vector<u32> out;
        for (size_t i = 0; i < s.size();) {
            u8 c = static_cast<u8>(s[i]);
            u32 cp = 0;
            if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
                cp = ((c & 0x1F) << 6) | (static_cast<u8>(s[i + 1]) & 0x3F);
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
                cp =
                    ((c & 0x0F) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 6) | (static_cast<u8>(s[i + 2]) & 0x3F);
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
                cp = ((c & 0x07) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 12) |
                     ((static_cast<u8>(s[i + 2]) & 0x3F) << 6) | (static_cast<u8>(s[i + 3]) & 0x3F);
                i += 4;
            } else {
                // 单字节 ASCII 或非法 utf-8 起始字节兜底
                cp = c;
                i += 1;
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
    // B-4: String 含 Rc<Array<u32>> 字段，传参前必须 retain（callee-clean）
    auto emitAppendString = [&](llvm::Value* strVal) {
        retainHandleAtCallSite(strVal, TypeInfo("String"));
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
            auto interpTy = applySubst(interpExpr->getType());
            // E3026 由 SemaPass tryValidateToString 先抛；此处防 IR 合成不存在的 to_string。
            if (!sema::typeImplementsToString(_file, _yux ? _yux->sdkFile() : nullptr, interpTy)) {
                throwSemaGap(interpExpr->getLineNumber(), interpExpr->getColumn());
            }
            llvm::Value* strVal;
            if (interpTy.isString() || interpTy.name == "String") {
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

    // 4. sb.build() → String（fresh +1，由 caller 的 temp frame 管理所有权）
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
            bool isStringAdd = innerAdd != nullptr && innerAdd->op() == ExprAddSubNode::Op::Add &&
                               innerAdd->getType().name == "String";
            if (isStringAdd) {
                cur = innerAdd;
                continue;
            }
            leaves.push_back(leftExpr);
            break;
        }
        std::ranges::reverse(leaves);
    }

    // 2. 类型校验：每个叶子必须是 String 或实现 ToString。
    // applySubst：泛型体 T → i32 才能找到 i32.to_string。E3026 由 SemaPass 先抛。
    FileNode* sdk = _yux ? _yux->sdkFile() : nullptr;
    for (const auto& leaf : leaves) {
        auto t = applySubst(leaf->getType());
        if (!sema::typeImplementsToString(_file, sdk, t)) {
            throwSemaGap(leaf->getLineNumber(), leaf->getColumn());
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
    // B-4: String 含 Rc<Array<u32>> 字段，传参前必须 retain（callee-clean）
    vector<std::unique_ptr<Node>> synthHolder;
    for (const auto& leaf : leaves) {
        llvm::Value* strVal;
        if (applySubst(leaf->getType()).name == "String") {
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
        retainHandleAtCallSite(strVal, TypeInfo("String"));
        _builder.CreateCall(appendFn, {sbPtr, strVal});
    }

    // 6. sb.build() → String（fresh +1，由 caller 的 temp frame 管理所有权）
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
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3098, tupleType.name, string("(tuple)"),
                       string("(tuple)"));
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

// B-3: 把 ExprArrayNode 按 Array<elemType> 字面量编译，直接分配数据缓冲并填充元素，
// 返回 Array<T> struct 值（{ ptr _data, u64 _len, u64 _cap }）。
// 嵌套 Array<Array<U>> 字面量的内层走自递归。
llvm::Value* Compiler::buildArrayLiteralBlock(ExprArrayNode* arrayNode, const TypeInfo& elemType) {
    auto& elements = arrayNode->elements();
    auto count = elements.size();
    auto elemLLVMType = getLLVMType(elemType);
    auto sizeTy = getSizeType();
    auto countVal = llvm::ConstantInt::get(sizeTy, count);
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

    // 构造 Array<T> 类型
    auto elemSp = make_shared<TypeInfo>(elemType);
    TypeInfo arrayType("Array", {elemSp});
    auto arrayLLVMType = getLLVMType(arrayType);

    // Array<T> 临时 alloca
    auto arrayAlloca = _builder.CreateAlloca(arrayLLVMType, nullptr, "array.lit");

    // 初始化 _len 和 _cap
    _builder.CreateStore(countVal, arrayLenFieldPtr(arrayAlloca, "lit"));
    _builder.CreateStore(countVal, arrayCapFieldPtr(arrayAlloca, "lit"));

    if (count == 0) {
        // 空数组：_data = null
        _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), arrayDataFieldPtr(arrayAlloca, "lit"));
        return _builder.CreateLoad(arrayLLVMType, arrayAlloca, "array.lit.load");
    }

    // 分配数据缓冲：HeapAlloc(count * sizeof(T))
    auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
    auto elemSizeVal = llvm::ConstantInt::get(sizeTy, elemSize);
    auto byteSize = _builder.CreateMul(countVal, elemSizeVal, "byte_size");
    auto allocFn = runtime::getYuxrtAllocFn(_module, _builder);
    auto data = _builder.CreateCall(allocFn, {byteSize}, "lit.data");

    // 写入 _data 字段
    _builder.CreateStore(data, arrayDataFieldPtr(arrayAlloca, "lit"));

    bool elemIsArrayGeneric = elemType.isArrayGeneric();
    sp<TypeInfo> innerElemType = elemIsArrayGeneric ? elemType.arrayGenericElementType() : nullptr;

    for (size_t i = 0; i < count; ++i) {
        llvm::Value* elemVal = nullptr;
        // 嵌套：内层数组字面量按外层期望的 Array<U> 编译（递归），结果是 Array<U> struct 值
        if (elemIsArrayGeneric && innerElemType) {
            if (auto innerArr = dynamic_cast<ExprArrayNode*>(elements[i])) {
                elemVal = buildArrayLiteralBlock(innerArr, *innerElemType);
            }
        }
        if (!elemVal) elemVal = compileExpr(elements[i]);

        // Nullable<T> 元素包装：T 值 → {i1 true, T _value}，null 字面量 → {i1 false, T undef}
        if (elemType.isNullable()) {
            if (auto innerType = elemType.nullableInnerType()) {
                auto elemExprType = elements[i]->getType();
                if (isIntTypeName(innerType->name) && isFlexibleIntExpr(elements[i])) {
                    tryInferIntType(elements[i], *innerType);
                }
                bool isNullLit = isFlexibleNullExpr(elements[i]);
                // 仅在元素类型不是 Nullable<T> 时包装（已是 Nullable 的直接 store）
                if (isNullLit || !elemExprType.isNullable()) {
                    auto innerLLVMType = getLLVMType(*innerType);
                    auto tmp = _builder.CreateAlloca(elemLLVMType, nullptr, "nullable_wrap");
                    auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    auto o = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
                    auto hasPtr = _builder.CreateGEP(elemLLVMType, tmp, {z, z}, "nw.has");
                    auto valPtr = _builder.CreateGEP(elemLLVMType, tmp, {z, o}, "nw.val");
                    if (isNullLit) {
                        _builder.CreateStore(_builder.getInt1(false), hasPtr);
                        _builder.CreateStore(llvm::Constant::getNullValue(innerLLVMType), valPtr);
                    } else {
                        _builder.CreateStore(_builder.getInt1(true), hasPtr);
                        _builder.CreateStore(elemVal, valPtr);
                    }
                    elemVal = _builder.CreateLoad(elemLLVMType, tmp, "nw.load");
                }
            }
        }

        auto idx = llvm::ConstantInt::get(sizeTy, i);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, data, {idx}, "lit.elem.ptr");
        storeIntoSlot(elemPtr, elemVal, elemType, elements[i], SlotStore::Init);
    }
    return _builder.CreateLoad(arrayLLVMType, arrayAlloca, "array.lit.load");
}

llvm::Value* Compiler::compileArrayLiteralExpr(p<ExprArrayNode> node) {
    // 空 `[]` 的 getType 是 `[__empty * 0]`；有靶向时 SemaPass 已把 resolvedType 写成 Array<T>。
    // 必须读 resolved，否则 ret [] / 表达式位置的空字面量会走固定数组分支抛 E3091。
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto& elements = node->elements();
    auto arrayType = node->resolvedType();
    auto llvmArrayType = getLLVMType(arrayType);

    DEBUG_LOG_VAL("    Expr: ArrayLiteral", arrayType.name);

    // Array<T> 字面量（动态数组）：走统一 helper，返回 Array<T> struct 值
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        auto arrayVal = buildArrayLiteralBlock(node, elemType ? *elemType : TypeInfo("i8"));
        // B-3: buildArrayLiteralBlock 直接返回 Array<T> struct 值
        return arrayVal;
    }

    // 固定大小数组 [N]T 字面量
    if (elements.empty()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
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

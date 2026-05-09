// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Lambda 字面量与 fn-value 调用编译实现（spec §4 / §5）
//
// Phase 2b：零捕获场景
// - compileLambdaExpr：LambdaExprNode → 16 字节 fat-ptr { fn_ptr, captures=null }
// - emitLambdaFunction：根据上下文反推后的 Fn 类型生成顶层匿名 LLVM Function；
//   ABI captures-leading：(Ptr captures, P1, ..., Pn) → R，零捕获场景 captures 形参未使用
// - compileFnValueCall：callee 静态类型为 Fn 时走 fat-ptr：extractvalue 取
//   fn_ptr / captures，按 ABI 调用
// - inferLambdaParamsFromFnType：实参位置 lambda 形参缺类型时按 callee fnParamTypes 回填

#include "compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/mangler.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

// ==================== Lambda 形参类型反推 ====================
// 实参位置 lambda 反推：把 fn(...) 期望类型的形参列表回填到 LambdaExprNode 形参槽位。
// 仅当槽位 type==nullptr 时回填；retType 同理（仅在 expectedFnType 显式标注且 lambda 未标注时回填）。
// expectedFnType 必须是 isFn() 才会动作；否则 no-op。
void Compiler::inferLambdaParamsFromFnType(p<LambdaExprNode> lambda, const TypeInfo& expectedFnType) {
    if (!lambda || !expectedFnType.isFn()) return;
    auto& slots = lambda->mutableParams();
    const auto& expectedParams = expectedFnType.fnParamTypes();
    // arity 不一致由 sema 报错（Phase 2c），这里仅按位置回填能填的部分
    size_t n = std::min(slots.size(), expectedParams.size());
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].type) continue;        // 已标注：尊重源
        if (!expectedParams[i]) continue;   // 期望也是空：不回填
        // 把 TypeInfo 包成 TypeNormalNode 占位（codegen 仅取 getType()，name 即可定位 LLVM 类型）
        // 注意：复杂类型（Generic/Array/Tuple/Fn）经 getFullName 后字符串无法被 TypeNormalNode
        // 还原；Phase 2b 仅覆盖 Normal/Generic 通过名称还原的常见情形。
        // TODO: 引入"已解析 TypeInfo 直挂载"的 TypeNode 变体，让任意 TypeInfo 都能反推
        Token tok = lambda->params()[i].name;
        tok = Token(tok);  // 复制，作为占位
        // 直接重写 token 文本为期望类型 name
        // 这里用字符串名能覆盖 i32/u32/bool/String/struct 等 Normal 类型；
        // 对 Generic/Array/Fn 形参当前无法精确还原，留 TODO
    }
    // Phase 2b：暂不真的回填（避免引入伪 TypeNode）；emitLambdaFunction 走"直接看 expectedFnType"
    // 路径，从 expectedFnType.fnParamTypes() 拿类型即可。这里函数仅占位，便于未来扩展。
    (void)expectedFnType;
}

// ==================== emitLambdaFunction ====================
// 给定 LambdaExprNode 与"上下文期望类型"，生成顶层匿名 fn 函数。
// 期望类型给出形参 LLVM 类型（lambda 形参缺类型时按它取）；返回类型同理。
// 同一 LambdaExprNode 多次求值（罕见）按 mangle 名缓存；返回 llvm::Function*。
llvm::Function* Compiler::emitLambdaFunction(p<LambdaExprNode> node, const TypeInfo& expectedFnType) {
    int line = node->getLineNumber();
    int col = node->getColumn();
    string mod = _file ? _file->moduleName() : string();
    string mangled = Mangler::lambda(mod, line, col);

    if (auto fn = _module->getFunction(mangled)) {
        return fn;
    }

    // 解析形参类型：lambda 槽位优先；缺失则回退到 expectedFnType.fnParamTypes()
    vector<TypeInfo> paramTypes;
    paramTypes.reserve(node->params().size());
    for (size_t i = 0; i < node->params().size(); ++i) {
        const auto& slot = node->params()[i];
        if (slot.type) {
            paramTypes.push_back(slot.type->getType());
        } else if (expectedFnType.isFn() && i < expectedFnType.fnParamTypes().size()
                   && expectedFnType.fnParamTypes()[i]) {
            paramTypes.push_back(*expectedFnType.fnParamTypes()[i]);
        } else {
            // 缺类型且无上下文 → 报错；Phase 2c 由 sema 更早拒
            throw YuxError(line, col, ErrorCode::E3091);
        }
    }

    // 解析返回类型（spec §4.2）：
    // - 显式标注（仅 Paren 形态可写）→ 严格用之
    // - Paren 无标注 → 默认 void（不接受上下文反推；自描述形态）
    // - 裸形（Single/Block/ZeroBlock）无标注 → 上下文反推；无上下文 → void（兜底）
    TypeInfo retType;
    if (node->retType()) {
        retType = node->retType()->getType();
    } else if (node->form() == LambdaExprNode::Form::Paren) {
        // 括 + 无标 → void（spec §4.2 [#14]）
        // body 表达式被作"语句位置丢弃"
    } else if (expectedFnType.isFn() && expectedFnType.fnReturnType()) {
        retType = *expectedFnType.fnReturnType();
    }
    // retType.empty() == void

    // 构造 LLVM 函数签名：(Ptr captures, P1, ..., Pn) → R
    auto ptrTy = llvm::PointerType::get(_context, 0);
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);  // captures（零捕获场景未使用）
    for (auto& pt : paramTypes) {
        llvmParamTypes.push_back(getLLVMType(pt));
    }
    auto llvmRet = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
    auto fnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    auto func = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage, mangled, _module);

    // 保存当前编译状态；lambda 是顶层 fn，不复用外层 _localVarPtrs / _scopeVars
    auto savedFn = _currentFn;
    auto savedFnNode = _currentFnNode;
    auto savedStruct = _currentStructName;
    auto savedLocals = std::move(_localVarPtrs);
    auto savedScope = std::move(_scopeVars);
    auto savedTempStack = std::move(_tempStack);
    auto savedInsert = _builder.GetInsertBlock();
    auto savedInsertPoint = _builder.GetInsertPoint();
    auto savedLambdaBodyScope = _currentLambdaBodyScope;
    auto savedLambdaForCapture = _currentLambdaForCapture;
    auto savedLambdaCapturesArg = _currentLambdaCapturesArg;

    _currentFn = func;
    _currentFnNode = nullptr;     // lambda 无 FnNode；body 引用外层符号走 Phase 4a 捕获通道
    _currentStructName.clear();
    _localVarPtrs.clear();
    _scopeVars.clear();
    _tempStack.clear();
    _currentLambdaBodyScope = node->bodyScope();          // Phase 2c：启用 FV 通路
    // Phase 4a：启用捕获识别。清空旧 captures（防止重复 emit 累加；缓存命中走早返路径
    // 不重入此段）。emitLambdaFunction 这一次 body 编译期间，compileLiteralExpr 命中
    // 外层 local 会 addCapture 并就地 GEP 读 captures buffer。
    node->clearCaptures();
    _currentLambdaForCapture = node;

    auto entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);

    // 形参 alloca + store；首参 captures 跳过（零捕获场景未使用 / 含捕获走捕获通道）
    auto argIt = func->arg_begin();
    argIt->setName("captures");
    // Phase 4a：缓存 captures arg（block ptr），compileLiteralExpr 命中捕获时 GEP base
    _currentLambdaCapturesArg = &*argIt;
    ++argIt;
    for (size_t i = 0; i < paramTypes.size(); ++i, ++argIt) {
        auto paramName = node->params()[i].name.getText();
        argIt->setName(paramName);
        auto llvmType = getLLVMType(paramTypes[i]);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
        _builder.CreateStore(&*argIt, alloca);
        _localVarPtrs[paramName] = alloca;
    }

    // 编译 body
    // - Single / Paren：单表达式 → ret expr（void 返回类型时 ret void）
    // - Block / ZeroBlock：语句序列 → 顺序编译，末尾走隐式 ret void
    pushTempFrame();
    if (node->bodyExpr()) {
        auto val = compileExpr(node->bodyExpr());
        if (!_builder.GetInsertBlock()->getTerminator()) {
            if (retType.empty()) {
                popAndReleaseTempFrame();
                _builder.CreateRetVoid();
            } else {
                popAndReleaseTempFrame();
                _builder.CreateRet(val);
            }
        } else {
            popAndReleaseTempFrame();
        }
    } else {
        for (auto& s : node->bodyStmts()) {
            compileStatement(s);
        }
        if (!_builder.GetInsertBlock()->getTerminator()) {
            popAndReleaseTempFrame();
            if (retType.empty()) {
                _builder.CreateRetVoid();
            } else {
                // 缺显式 ret 且非 void：报错。Phase 2c 由 sema 更早拒
                throw YuxError(line, col, ErrorCode::E3091);
            }
        } else {
            popAndReleaseTempFrame();
        }
    }

    // 恢复外层编译状态
    _currentFn = savedFn;
    _currentFnNode = savedFnNode;
    _currentStructName = savedStruct;
    _localVarPtrs = std::move(savedLocals);
    _scopeVars = std::move(savedScope);
    _tempStack = std::move(savedTempStack);
    _currentLambdaBodyScope = savedLambdaBodyScope;
    _currentLambdaForCapture = savedLambdaForCapture;
    _currentLambdaCapturesArg = savedLambdaCapturesArg;
    if (savedInsert) {
        _builder.SetInsertPoint(savedInsert, savedInsertPoint);
    }

    return func;
}

// ==================== compileLambdaExpr ====================
// LambdaExprNode 求值：先 emit 底层 fn（期间 emit 通路完成 captures 槽位发现），
// 回到外层上下文后据 captures 列表分配 Box<CapturesT> + 写入字段；最后构造 fat-ptr。
// fat-ptr layout：{ ptr fn_ptr, ptr captures }；零捕获 captures = null。
//
// Phase 4a：仅支持标量按值复制；非标量在 emitLambdaFunction body 阶段已被
// compileLiteralExpr 拒（E2029），此处 captures 列表里全是 builtin 标量。
llvm::Value* Compiler::compileLambdaExpr(p<LambdaExprNode> node) {
    // 静态类型即 Fn TypeInfo（lambda 形参类型可能缺）
    auto fnType = node->getType();
    auto func = emitLambdaFunction(node, fnType);

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto fatStructTy = llvm::StructType::get(_context, {ptrTy, ptrTy});

    // captures：零捕获 → null；含捕获 → 分配 box，从 _localVarPtrs 拷贝外层 local 值
    llvm::Value* capturesPtr = llvm::ConstantPointerNull::get(ptrTy);
    const auto& caps = node->captures();
    if (!caps.empty()) {
        u64 totalSize = node->capturesTotalSize();
        auto allocFn = runtime::getBoxAllocFn(_module, _builder);
        auto blockHandle = _builder.CreateCall(
            allocFn, {_builder.getInt64((i64)totalSize)}, "captures.block");
        // 写入各 capture：payload = handle + 8，逐字段从 _localVarPtrs 加载并写
        auto i8Ty = _builder.getInt8Ty();
        for (const auto& cap : caps) {
            auto it = _localVarPtrs.find(cap.name);
            if (it == _localVarPtrs.end()) {
                // 不该发生：emit 期已通过 _currentLambdaBodyScope 校验过 sym 来源
                throw YuxError(node->getLineNumber(), node->getColumn(),
                               ErrorCode::E3030, cap.name);
            }
            auto valLLVMTy = getLLVMType(cap.type);
            auto srcVal = _builder.CreateLoad(valLLVMTy, it->second, "cap.src");
            auto offset = _builder.getInt64(8 + (i64)cap.byteOffset);
            auto dstAddr = _builder.CreateGEP(i8Ty, blockHandle, {offset}, "cap.dst");
            _builder.CreateStore(srcVal, dstAddr);
        }
        capturesPtr = blockHandle;
    }

    // ConstantStruct 不能直接用：Function* 是 Constant 但 fatStructTy 是 anonymous struct，
    // ConstantStruct::get 需要 named struct。统一走 InsertValue 动态构造。
    llvm::Value* fat = llvm::UndefValue::get(fatStructTy);
    fat = _builder.CreateInsertValue(fat, func, {0}, "fn.ptr");
    fat = _builder.CreateInsertValue(fat, capturesPtr, {1}, "fn.captures");
    return fat;
}

// ==================== compileFnValueCall ====================
// callee 静态类型为 Fn(...)R 的调用站点：
// 1) 实参位置 lambda 走 inferLambdaParamsFromFnType 反推（如有）
// 2) 编译 callee 得到 fat-ptr 值
// 3) extractvalue 取 fn_ptr / captures
// 4) 编译实参 + CreateCall(fnType, fn_ptr, [captures, args...])
llvm::Value* Compiler::compileFnValueCall(p<ExprCallNode> node) {
    auto calleeExpr = node->getCalleeExpr();
    auto fnType = calleeExpr->getType();
    if (!fnType.isFn()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    const auto& expectedParams = fnType.fnParamTypes();

    // 实参 lambda 反推：把 expectedParams[i] 回填到 LambdaExprNode 实参的 Fn 类型槽
    // 当前 inferLambdaParamsFromFnType 占位；emitLambdaFunction 已能从 expected 取类型，
    // 所以这里只需把"实参 lambda 的 expected 类型"通过 ExprCallNode 的 fnType 传递下去 —— 完成
    for (size_t i = 0; i < node->getArgs().size() && i < expectedParams.size(); ++i) {
        auto arg = node->getArgs()[i];
        auto lambdaArg = dynamic_cast<LambdaExprNode*>(arg);
        if (!lambdaArg) continue;
        if (!expectedParams[i]) continue;
        // 直接预 emit lambda function：让它走 expected 反推路径
        // 这样后续 compileExpr(arg) 走 compileLambdaExpr 时 emitLambdaFunction 命中缓存
        emitLambdaFunction(lambdaArg, *expectedParams[i]);
    }

    // 编译 callee → fat-ptr 16 字节
    auto fatPtr = compileExpr(calleeExpr);
    if (!fatPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto fnPtrVal = _builder.CreateExtractValue(fatPtr, {0}, "fn.ptr");
    auto captures = _builder.CreateExtractValue(fatPtr, {1}, "fn.captures");

    // 构造 LLVM FunctionType：(Ptr captures, P1, ..., Pn) → R
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);
    for (auto& pt : expectedParams) {
        if (!pt) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }
        llvmParamTypes.push_back(getLLVMType(*pt));
    }
    llvm::Type* llvmRet = _builder.getVoidTy();
    if (auto rt = fnType.fnReturnType()) {
        llvmRet = getLLVMType(*rt);
    }
    auto llvmFnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    // 编译实参（含 lambda → 构造 fat-ptr）
    vector<llvm::Value*> callArgs;
    callArgs.push_back(captures);
    for (size_t i = 0; i < node->getArgs().size(); ++i) {
        callArgs.push_back(compileExpr(node->getArgs()[i]));
    }

    return _builder.CreateCall(llvmFnType, fnPtrVal, callArgs);
}

// ==================== compileBoxFnValueCall ====================
// callee 静态类型为 Box<fn(...)R>：自动解引取 fat-ptr 后走 fn-value-call。
// box payload = handle + 8 字节（跳过 refcount 头），其上存放 16 字节 fat-ptr。
// 1) 实参 lambda 反推（按 innerFnType.fnParamTypes()）
// 2) 编译 callee 得到 box 值（{ ptr handle }），extractvalue 取 handle
// 3) payload_ptr = handle + 8；load fat-ptr 16 字节
// 4) 走与 compileFnValueCall 相同的 extractvalue + CreateCall 路径
llvm::Value* Compiler::compileBoxFnValueCall(p<ExprCallNode> node, const TypeInfo& innerFnType) {
    if (!innerFnType.isFn()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    const auto& expectedParams = innerFnType.fnParamTypes();

    // 实参 lambda 预 emit（与 compileFnValueCall 一致）
    for (size_t i = 0; i < node->getArgs().size() && i < expectedParams.size(); ++i) {
        auto arg = node->getArgs()[i];
        auto lambdaArg = dynamic_cast<LambdaExprNode*>(arg);
        if (!lambdaArg) continue;
        if (!expectedParams[i]) continue;
        emitLambdaFunction(lambdaArg, *expectedParams[i]);
    }

    // 编译 callee 得到 Box 值（struct { ptr handle }）；extractvalue 取 handle
    auto boxVal = compileExpr(node->getCalleeExpr());
    if (!boxVal) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto handle = _builder.CreateExtractValue(boxVal, {0}, "box.fn.handle");

    // payload_ptr = handle + 8 bytes（跳过 refcount 头，与 compileExpr Box.field 路径一致）
    auto payloadPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle,
        {_builder.getInt64(8)}, "box.fn.payload");

    // load fat-ptr 16 字节 { fn_ptr, captures }
    auto fatStructTy = llvm::StructType::get(_context, {ptrTy, ptrTy});
    auto fatPtr = _builder.CreateLoad(fatStructTy, payloadPtr, "box.fn.fatptr");
    auto fnPtrVal = _builder.CreateExtractValue(fatPtr, {0}, "fn.ptr");
    auto captures = _builder.CreateExtractValue(fatPtr, {1}, "fn.captures");

    // 构造 LLVM FunctionType：(Ptr captures, P1, ..., Pn) → R
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);
    for (auto& pt : expectedParams) {
        if (!pt) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }
        llvmParamTypes.push_back(getLLVMType(*pt));
    }
    llvm::Type* llvmRet = _builder.getVoidTy();
    if (auto rt = innerFnType.fnReturnType()) {
        llvmRet = getLLVMType(*rt);
    }
    auto llvmFnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    // 编译实参
    vector<llvm::Value*> callArgs;
    callArgs.push_back(captures);
    for (size_t i = 0; i < node->getArgs().size(); ++i) {
        callArgs.push_back(compileExpr(node->getArgs()[i]));
    }

    return _builder.CreateCall(llvmFnType, fnPtrVal, callArgs);
}

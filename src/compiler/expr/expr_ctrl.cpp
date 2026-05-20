// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 控制流表达式编译 (if-else / match / try-catch)：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
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
            // Bucket 6: SemaPass (sema_pass.cpp ExprTryCatchNode 分支) 已先抛出,
            // 此处保留作幂等防御性双跑 (sema getType 失败路径兜底).
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

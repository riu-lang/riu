// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 控制流表达式编译 (if-else / match / try-catch)：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
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

llvm::Value* Compiler::compileIfElseExpr(ExprIfElseNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = resolvedOrInferredType(node);
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL("    Expr: IfElse",
                  "hasResult=" << hasResult << ", type=" << (resultType.empty() ? "void" : resultType.name));

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
        auto ty = getLLVMType(resultType);
        if (!ty || ty->isVoidTy()) {
            hasResult = false;
        } else {
            phi = llvm::PHINode::Create(ty, 2, "if.result", mergeBB);
        }
    }

    // Phase B-1: 条件 move 保守追踪 — 保存 then 前的 moved 状态，
    // then 后收集，再恢复 for else 分支，最后取并集
    auto savedMoved = _movedVars;

    DEBUG_LOG("      Compiling then block");
    compileStatementBlockWithResult(node->thenBlock(), mergeBB, phi, resultType);

    // 收集 then 分支新增的 moved 变量
    auto afterThenMoved = _movedVars;
    _movedVars = savedMoved;

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
            auto elifCondBool =
                _builder.CreateICmpNE(elifCond, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "elif.cond");

            llvm::BasicBlock* elifThenBB = llvm::BasicBlock::Create(_context, "elif.then", func);
            llvm::BasicBlock* elifElseBB = llvm::BasicBlock::Create(_context, "elif.else");

            _builder.CreateCondBr(elifCondBool, elifThenBB, elifElseBB);

            _builder.SetInsertPoint(elifThenBB);
            // Phase B-1: elif 分支 move 追踪 — 快照 → 体 → 合并 → 恢复，与 SemaPass 对齐
            auto savedElif = _movedVars;
            compileStatementBlockWithResult(elif->block(), mergeBB, phi, resultType);
            for (auto& v : _movedVars)
                afterThenMoved.insert(v);
            _movedVars = savedElif;

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

    // Phase B-1: 汇合 — 取 then 分支和 else 分支 moved 变量的并集
    for (auto& v : afterThenMoved) {
        _movedVars.insert(v);
    }

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    if (hasResult) {
        DEBUG_LOG("      Returning phi node");
        // Phase 8d.3: 各分支已归一为 +1，phi 整体作为 fresh 句柄交给外层 statement frame
        if (typeNeedsDestructor(resultType)) {
            recordTemp(phi, resultType);
        }
        return phi;
    }
    return nullptr;
}

llvm::Value* Compiler::compileOneLineIfElseExpr(ExprOneLineIfElseNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = resolvedOrInferredType(node);
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL("    Expr: OneLineIfElse", "type=" << (hasResult ? resultType.name : "void"));

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);
    llvm::Value* trueVal = nullptr;
    bool thenToMerge = false;
    if (hasResult && !exprTerminatesFlow(node->findNearestScope(), node->trueValue())) {
        trueVal = compileBranchResultNormalized(node->trueValue(), resultType);
    } else {
        (void)compileExpr(node->trueValue());
    }
    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(mergeBB);
        thenToMerge = true;
    }
    auto thenEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    llvm::Value* falseVal = nullptr;
    bool elseToMerge = false;
    if (hasResult && !exprTerminatesFlow(node->findNearestScope(), node->falseValue())) {
        falseVal = compileBranchResultNormalized(node->falseValue(), resultType);
    } else {
        (void)compileExpr(node->falseValue());
    }
    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(mergeBB);
        elseToMerge = true;
    }
    auto elseEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    if (!hasResult) return nullptr;
    if (!thenToMerge && !elseToMerge) {
        _builder.CreateUnreachable();
        return llvm::UndefValue::get(getLLVMType(resultType));
    }

    auto phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    auto llvmTy = getLLVMType(resultType);
    if (thenToMerge) {
        phi->addIncoming(trueVal ? trueVal : llvm::UndefValue::get(llvmTy), thenEndBB);
    }
    if (elseToMerge) {
        phi->addIncoming(falseVal ? falseVal : llvm::UndefValue::get(llvmTy), elseEndBB);
    }

    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (typeNeedsDestructor(resultType)) {
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
// - arm body：`=> expr` 或 `=> { stmts }`（块末无 `;` 的表达式即块值，与 if 同）
llvm::Value* Compiler::compileMatchExpr(ExprMatchNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto scrutinee = node->scrutinee();
    auto rawScrutType = scrutinee->getType();
    // applySubst 含别名展开；泛型体实例化后 T → Color 才能按 enum 编译。
    auto scrutType = applySubst(rawScrutType);
    int line = node->getLineNumber();
    int col = node->getColumn();

    // Rc<E> match：自动 deref。仅支持借用语义（不接管 Rc 所有权），
    // 因此要求 scrutinee 不是 fresh 来源（避免 Rc 临时立即释放后 enum 悬挂）。
    bool rcDeref = false;
    TypeInfo rcOuterType;
    if (scrutType.isRc()) {
        auto inner = scrutType.rcElementType();
        if (inner) {
            FileNode* tmpOwner = nullptr;
            if (names().lookupEnum(*inner, &tmpOwner)) {
                if (isFreshHandleExpr(scrutinee)) {
                    throwSemaGap(line, col);
                }
                rcDeref = true;
                rcOuterType = scrutType;
                scrutType = *inner;
            }
        }
    }

    // Heap<E> match：自动 deref。Heap<T> 是裸 T*，load 即可得 enum 值。
    bool heapDeref = false;
    if (scrutType.isHeap()) {
        auto inner = scrutType.heapElementType();
        if (inner) {
            FileNode* tmpOwner = nullptr;
            if (names().lookupEnum(*inner, &tmpOwner)) {
                if (isFreshHandleExpr(scrutinee)) {
                    throwSemaGap(line, col);
                }
                heapDeref = true;
                scrutType = *inner;
            }
        }
    }

    // v0.16: T& match（如 match arr[i]）——自动 Load 引用以检查 enum discriminant。
    // arr[i] 返回 T&（指针），match 需要读枚举值来判断变体。
    bool refDeref = false;
    if (scrutType.isRef()) {
        auto inner = scrutType.refElementType();
        if (inner) {
            FileNode* tmpOwner = nullptr;
            if (names().lookupEnum(*inner, &tmpOwner)) {
                refDeref = true;
                scrutType = *inner;
            }
        }
    }

    // 1. 必须是 enum。E2022 由 SemaPass tryValidateMatchScrut 先抛；此处防 IR 走空路径。
    FileNode* enumOwner = nullptr;
    auto enumDecl = names().lookupEnum(scrutType, &enumOwner);
    if (!enumDecl) {
        throwSemaGap(line, col);
    }
    string enumName = scrutType.name;

    auto& arms = node->arms();

    // 重新收集 codegen 需要的状态
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

    // 3. 结果类型：优先 SemaPass 靶向后的 resolved（空 `[]` / `[T*N]` 字面量 → Array<T>）。
    TypeInfo resultType;
    bool firstSet = false;
    if (node->hasResolvedType() && !node->resolvedType().empty()) {
        resultType = node->resolvedType();
        firstSet = true;
    } else {
        for (auto& arm : arms) {
            if (arm->skipsTypeMerge()) continue;
            auto t = arm->resultType();
            if (!firstSet) {
                resultType = t;
                firstSet = true;
                continue;
            }
            if (t != resultType) {
                throwSemaGap(arm->resultLine(), arm->resultCol());
            }
        }
    }
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL("    Expr: Match", "enum=" << enumName << " arms=" << arms.size() << " hasResult=" << hasResult
                                             << " result=" << (hasResult ? resultType.name : "void"));

    // 4. 求值 scrutinee 并落 alloca；fresh 时 consume 拿走所有权
    auto enumLLVMType = getLLVMType(scrutType);
    if (!enumLLVMType) {
        throwSemaGap(line, col);
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
        auto handleField = _builder.CreateGEP(rcLLVMType, rcAlloca, {zero32, zero32}, "match.rc.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "match.rc.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "match.rc.payload");
        auto enumVal = _builder.CreateLoad(enumLLVMType, payload, "match.rc.enum");
        _builder.CreateStore(enumVal, scrutAlloca);
    } else if (heapDeref) {
        // scrutVal 是 E*（Heap<E> 的裸指针），Load 出枚举值再存入 alloca
        auto enumVal = _builder.CreateLoad(enumLLVMType, scrutVal, "match.heap.enum");
        _builder.CreateStore(enumVal, scrutAlloca);
    } else if (refDeref) {
        // v0.16: scrutVal 是 T& 指针（如 arr[i] 返回），Load 出枚举值再存入 alloca
        auto enumVal = _builder.CreateLoad(enumLLVMType, scrutVal, "match.ref.enum");
        _builder.CreateStore(enumVal, scrutAlloca);
    } else {
        _builder.CreateStore(scrutVal, scrutAlloca);
    }

    // 仅当 scrutinee 是 fresh（构造 / 函数返回 / 含 RC 的 enum 临时）我们才需要在 match 末 dtor
    // Rc deref / Heap deref / Ref deref 路径走借用语义，不接管所有权，故不计 drop
    bool ownsScrut = !rcDeref && !heapDeref && !refDeref && isFreshHandleExpr(scrutinee);
    if (ownsScrut) {
        consumeTemp(scrutVal);
    }
    bool needScrutDrop = ownsScrut && enumNeedsDestructor(scrutType);

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
        auto ty = getLLVMType(resultType);
        if (!ty || ty->isVoidTy()) {
            hasResult = false;
        } else {
            phi = llvm::PHINode::Create(ty, static_cast<unsigned>(arms.size()), "match.result", mergeBB);
        }
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
            auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, scrutAlloca, 1, "match.payload.ptr");

            for (size_t k = 0; k < pat->binds().size(); ++k) {
                const string& bn = pat->binds()[k].getText();
                auto bindType = variant->payloadTypes()[k]->getType();
                auto bindLLVMType = getLLVMType(bindType);

                auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr, static_cast<unsigned>(k),
                                                         "match.bind.field");
                auto loaded = _builder.CreateLoad(bindLLVMType, fieldPtr, ("match.bind." + bn).c_str());

                // 独立 alloca，便于 compileLiteralExpr 通过 _localVarPtrs 取出
                auto bindAlloca = _builder.CreateAlloca(bindLLVMType, nullptr, ("bind." + bn).c_str());
                _builder.CreateStore(loaded, bindAlloca);

                BindSnap snap;
                snap.name = bn;
                auto* prev = _currentFnNode->lookupSymbol(bn);
                snap.hadSym = (prev != nullptr);
                if (prev) snap.prevSym = *prev;
                auto pit = _localVarPtrs.find(bn);
                snap.hadPtr = (pit != _localVarPtrs.end());
                snap.prevPtr = snap.hadPtr ? pit->second : nullptr;

                _currentFnNode->registerSymbol(bn, {SymbolKind::Variable, bn, bindType, false});
                _localVarPtrs[bn] = bindAlloca;

                snaps.push_back(snap);
            }
        }

        // 编译 body：RC 句柄需归一到 +1 的部分由 compileBranchResultNormalized 处理
        llvm::Value* bodyVal = nullptr;
        if (arm->hasBlock()) {
            if (hasResult) {
                compileStatementBlockWithResult(arm->block(), mergeBB, phi, resultType);
            } else {
                compileStatementBlock(arm->block());
            }
        } else if (exprTerminatesFlow(arm->findNearestScope(), arm->body())) {
            (void)compileExpr(arm->body());
        } else if (hasResult) {
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
        if (typeNeedsDestructor(resultType)) {
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
llvm::Value* Compiler::compileTryCatchExpr(ExprTryCatchNode* node) {
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
        const auto& errTi = arm->errTypeInfo();
        FileNode* owner = nullptr;
        auto enumDecl = names().lookupEnum(errTi, &owner);
        if (!enumDecl) {
            int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
            int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
            // E7011 由 SemaPass try/catch 先抛。
            throwSemaGap(aline, acol);
        }
        ctx.catchTypes.push_back(arm->errType());

        // 为 e 绑定分配 alloca（类型 = enum）；命名带 arm 错误名便于 IR 阅读
        const string& bn = arm->errName().getText();
        const TypeInfo& catchErrTy = errTi;
        auto eAlloca = _builder.CreateAlloca(getLLVMType(catchErrTy), nullptr, ("catch.e." + bn).c_str());
        ctx.armEAllocas.push_back(eAlloca);

        // arm entry BB 暂不插入 func；编译 arm body 时再 insert
        auto armBB = llvm::BasicBlock::Create(_context, "catch.arm");
        ctx.armEntryBBs.push_back(armBB);
    }

    // join BB（try 成功路径 + 各 arm 末尾汇合）
    auto joinBB = llvm::BasicBlock::Create(_context, "trycatch.join");

    // 2) 编译 try block，_tryCatchStack 顶为本 try 的 ctx
    // 独立临时帧：catch 臂的 String 等 spill 不能在 join 上释放，否则成功路径
    // 析构未初始化槽（File& helper 里 try { m(a) } catch { _die("${e}") } 成功即 AV）。
    _tryCatchStack.push_back(ctx);
    pushTempFrame();
    auto* tryBlock = node->tryBlock();
    for (auto& stmt : tryBlock->statements()) {
        compileStatement(stmt);
    }

    // 与 if / match 对齐：有值当且仅当类型非空。语法 hasResult（末表达式无 `;`）
    // 在 void 调用上仍为 true，但不能 CreatePHI(void, ..., "name")（LLVM assert）。
    TypeInfo resultType;
    bool hasValue = false;
    llvm::Type* resultLLVMType = nullptr;
    llvm::Value* tryResult = nullptr;
    if (tryBlock->hasResult() && tryBlock->resultExpr()) {
        try {
            auto* re = tryBlock->resultExpr();
            resultType = resolvedOrInferredType(re);
            if (node->hasResolvedType() && !node->resolvedType().empty()) {
                resultType = node->resolvedType();
            }
        } catch (...) { // NOLINT(bugprone-empty-catch) — getType 失败: 仍编译 resultExpr，异常留上层
        }
        hasValue = !resultType.empty();
        if (hasValue) {
            resultLLVMType = getLLVMType(resultType);
            if (!resultLLVMType || resultLLVMType->isVoidTy()) {
                hasValue = false;
                resultLLVMType = nullptr;
            }
        }
        if (hasValue && !exprTerminatesFlow(tryBlock, tryBlock->resultExpr())) {
            tryResult = compileBranchResultNormalized(tryBlock->resultExpr(), resultType);
        } else {
            (void)compileExpr(tryBlock->resultExpr());
        }
    }

    auto trySuccessEndBB = _builder.GetInsertBlock();

    // 取出 seenErrTypes 后出栈
    TryCatchCtx finishedCtx = std::move(_tryCatchStack.back());
    _tryCatchStack.pop_back();

    // 3) 穷尽性 E7002
    for (auto& seen : finishedCtx.seenErrTypes) {
        bool covered = false;
        for (auto& ct : ctx.catchTypes) {
            if (ct == seen) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            // E7002 由 SemaPass try 穷尽性先抛。
            throwSemaGap(line, col);
        }
    }

    // 4) E7015 / E7017：警告类，TODO

    // try 成功路径：先释放本路径临时，再跳 join（若未被流终止语句抢占 terminator）
    vector<std::pair<llvm::Value*, llvm::BasicBlock*>> phiIncoming;
    if (!trySuccessEndBB->getTerminator()) {
        if (hasValue && tryResult) {
            phiIncoming.emplace_back(tryResult, trySuccessEndBB);
        }
        _builder.SetInsertPoint(trySuccessEndBB);
        popAndReleaseTempFrame();
        _builder.CreateBr(joinBB);
    } else {
        popAndReleaseTempFrame();
    }

    // 5) 编译每个 catch arm
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

        pushTempFrame();
        // 编译 arm body 语句序列
        for (auto& stmt : arm->body()->statements()) {
            compileStatement(stmt);
        }

        llvm::Value* armResult = nullptr;
        if (arm->body()->hasResult() && arm->body()->resultExpr()) {
            if (hasValue && !exprTerminatesFlow(arm->body(), arm->body()->resultExpr())) {
                armResult = compileBranchResultNormalized(arm->body()->resultExpr(), resultType);
            } else {
                (void)compileExpr(arm->body()->resultExpr());
            }
        }

        auto armEndBB = _builder.GetInsertBlock();

        // 还原 _localVarPtrs（CatchArmNode 自身的符号 entry 由 ScopeNode 持有，无需手动撤）
        if (hadPtr)
            _localVarPtrs[bn] = prevPtr;
        else
            _localVarPtrs.erase(bn);

        // 先释放本臂临时，再跳 join（若未被流终止抢占 terminator）
        if (!armEndBB->getTerminator()) {
            _builder.SetInsertPoint(armEndBB);
            popAndReleaseTempFrame();
            if (hasValue && armResult) {
                phiIncoming.emplace_back(armResult, _builder.GetInsertBlock());
                _builder.CreateBr(joinBB);
            } else if (hasValue) {
                // 有值 try 的无块值 arm 不参与 phi；未 ret 则视为不可达汇合
                _builder.CreateUnreachable();
            } else {
                _builder.CreateBr(joinBB);
            }
        } else {
            popAndReleaseTempFrame();
        }
    }

    // 6) join BB
    func->insert(func->end(), joinBB);
    _builder.SetInsertPoint(joinBB);

    // 表达式合并：若有结果，phi；若所有路径都流终止，joinBB 不可达
    if (hasValue && !phiIncoming.empty()) {
        auto phi = _builder.CreatePHI(resultLLVMType, static_cast<unsigned>(phiIncoming.size()), "trycatch.result");
        for (auto& inc : phiIncoming) {
            phi->addIncoming(inc.first, inc.second);
        }
        if (typeNeedsDestructor(resultType)) {
            recordTemp(phi, resultType);
        }
        return phi;
    }
    if (hasValue) {
        // 所有分支都流终止，joinBB 不可达；emit unreachable 防 verifier
        _builder.CreateUnreachable();
        return llvm::UndefValue::get(resultLLVMType);
    }
    return nullptr;
}

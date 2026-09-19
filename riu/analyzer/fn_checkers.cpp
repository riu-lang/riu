// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "fn_checkers.h"

#include "borrow_checker.h"
#include "const_mut_checker.h"
#include "flow_terminate_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"

namespace {

bool skipFn(FnNode* fn) {
    return !fn || !fn->header() || fn->header()->hasAnno("Builtin");
}

// Sema 之后单趟 walk：同一节点先 borrow 再 const-mut。#NoReturn 仍按需另走。
class FnCheckerWalk {
    BorrowChecker _borrow;
    ConstMutWalker _cm;

    void visitBlock(StatementBlockNode* blk) {
        if (!blk) return;
        _borrow.pushScope();
        visitBlockBody(blk);
        _borrow.popScope();
    }

    void visitBlockBody(StatementBlockNode* blk) {
        if (!blk) return;
        for (auto& s : blk->statements()) {
            visitStmt(s);
        }
        if (blk->hasResult() && blk->resultExpr()) {
            visitExpr(blk->resultExpr());
        }
    }

    void visitStmt(StatementNode* s) {
        if (!s) return;

        if (auto blk = dynamic_cast<StatementBlockNode*>(s)) {
            _cm.onStmt(s);
            visitBlock(blk);
            return;
        }

        if (auto loop = dynamic_cast<StatementLoopNode*>(s)) {
            // loop init 与 body 共享同一块作用域（borrow 原 walk）
            _borrow.pushScope();
            for (auto& n : loop->initNames()) {
                _borrow.declare(n.getText());
            }
            _cm.onStmt(s);
            visitBlockBody(loop->block());
            _borrow.popScope();
            return;
        }

        if (auto forin = dynamic_cast<StatementForInNode*>(s)) {
            _cm.onStmt(s);
            visitExpr(forin->expr());
            _borrow.pushScope();
            _borrow.declare(forin->item().getText());
            visitBlockBody(forin->block());
            _borrow.popScope();
            return;
        }

        if (auto decl = dynamic_cast<StatementDeclareNode*>(s)) {
            _borrow.declare(decl->name().getText());
            _cm.onStmt(s);
            return;
        }

        if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
            _borrow.declare(da->name().getText());
            _cm.onStmt(s);
            if (da->expr()) visitExpr(da->expr());
            _borrow.afterDeclareAssign(da, s->getLineNumber());
            return;
        }

        if (auto as = dynamic_cast<StatementAssignNode*>(s)) {
            _borrow.onAssign(as, s->getLineNumber());
            _cm.onStmt(s);
            if (as->expr()) visitExpr(as->expr());
            return;
        }

        if (auto sfs = dynamic_cast<StatementStaticFieldSetNode*>(s)) {
            _cm.onStmt(s);
            if (sfs->valueExpr()) visitExpr(sfs->valueExpr());
            return;
        }

        if (auto setN = dynamic_cast<StatementSetNode*>(s)) {
            _cm.onStmt(s);
            if (setN->arrayExpr()) visitExpr(setN->arrayExpr());
            for (auto& idx : setN->indices()) {
                if (idx) visitExpr(idx);
            }
            if (setN->valueExpr()) visitExpr(setN->valueExpr());
            return;
        }

        if (auto ret = dynamic_cast<StatementRetNode*>(s)) {
            _cm.onStmt(s);
            if (ret->expr()) visitExpr(ret->expr());
            _borrow.afterRet(ret, s->getLineNumber());
            return;
        }

        if (auto se = dynamic_cast<StatementExprNode*>(s)) {
            _cm.onStmt(s);
            if (se->expr()) visitExpr(se->expr());
            return;
        }

        _cm.onStmt(s);
    }

    void visitExpr(ExprNode* e) {
        if (!e) return;

        if (auto ie = dynamic_cast<ExprIfElseNode*>(e)) {
            if (ie->condition()) visitExpr(ie->condition());
            if (ie->thenBlock()) visitBlock(ie->thenBlock());
            for (auto& el : ie->elifs()) {
                if (!el) continue;
                if (el->condition()) visitExpr(el->condition());
                if (el->block()) visitBlock(el->block());
            }
            if (ie->elseBlock()) visitBlock(ie->elseBlock());
            return;
        }
        if (auto ol = dynamic_cast<ExprOneLineIfElseNode*>(e)) {
            if (ol->condition()) visitExpr(ol->condition());
            if (ol->trueValue()) visitExpr(ol->trueValue());
            if (ol->falseValue()) visitExpr(ol->falseValue());
            return;
        }
        if (auto call = dynamic_cast<ExprCallNode*>(e)) {
            _borrow.onCall(call);
            _cm.onExpr(e);
            if (call->getCalleeExpr()) visitExpr(call->getCalleeExpr());
            for (auto& a : call->getArgs()) {
                if (a) visitExpr(a);
            }
            return;
        }
        if (auto lam = dynamic_cast<LambdaExprNode*>(e)) {
            _borrow.enterLambda(lam);
            if (lam->bodyExpr()) {
                visitExpr(lam->bodyExpr());
                _borrow.afterLambdaBodyExpr(lam);
            } else {
                for (auto& st : lam->bodyStmts()) {
                    visitStmt(st);
                }
            }
            _borrow.leaveLambda();
            return;
        }
        if (auto m = dynamic_cast<ExprMatchNode*>(e)) {
            if (m->scrutinee()) visitExpr(m->scrutinee());
            for (auto& arm : m->arms()) {
                if (!arm) continue;
                if (arm->hasBlock())
                    visitBlock(arm->block());
                else if (arm->body())
                    visitExpr(arm->body());
            }
            return;
        }
        if (auto tc = dynamic_cast<ExprTryCatchNode*>(e)) {
            if (tc->tryBlock()) visitBlock(tc->tryBlock());
            for (auto& c : tc->catches()) {
                if (c && c->body()) visitBlock(c->body());
            }
            return;
        }
    }

public:
    void run(FnNode* fn, const std::string& selfStructName) {
        _borrow.begin(fn, selfStructName);
        _cm.begin(fn);
        for (auto& s : fn->body()) {
            visitStmt(s);
        }
        _borrow.finish();
    }
};

void checkOne(FnNode* fn, const std::string& selfStructName) {
    if (skipFn(fn)) return;
    FnCheckerWalk walk;
    walk.run(fn, selfStructName);
    checkFlowTerminate(fn);
}

} // namespace

void runFnCheckers(FileNode* file) {
    if (!file) return;
    for (auto* fn : file->getFunctions()) {
        checkOne(fn, "");
    }
    for (auto* impl : file->getStructImpls()) {
        if (!impl) continue;
        const std::string& name = impl->structName();
        for (auto* m : impl->methods()) {
            checkOne(m, name);
        }
        if (impl->hasDestructor()) checkOne(impl->destructor(), name);
    }
}

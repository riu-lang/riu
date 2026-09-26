// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `#If` 假分支在 Sema 前从 AST 摘掉；真分支剥掉 `#If` 留下目标。

#include "pass/cfg_strip_pass.h"

#include "ast/node/alias_node.h"
#include "ast/node/anno_call.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "error_code.h"
#include "sema/const_eval.h"

namespace {

enum class IfDecision : u8 { Keep, Drop };

vector<AnnoCall> withoutIf(const vector<AnnoCall>& calls) {
    vector<AnnoCall> out;
    out.reserve(calls.size());
    for (const auto& c : calls) {
        if (c.name != "If") out.push_back(c);
    }
    return out;
}

IfDecision decideIf(ConstEvaluator& ev, const vector<AnnoCall>& calls) {
    const AnnoCall* seen = nullptr;
    bool drop = false;
    for (const auto& c : calls) {
        if (c.name != "If") continue;
        if (seen) throw RiuError(c.line, c.col, ErrorCode::E3119, c.name);
        seen = &c;
        if (c.args.empty() || !c.args[0].expr) {
            throw RiuError(c.line, c.col, ErrorCode::E3163, c.name);
        }
        auto v = ev.eval(c.args[0].expr);
        if (!v || !v->isBool()) throw RiuError(c.line, c.col, ErrorCode::E3163, c.name);
        if (!v->boolVal) drop = true;
    }
    return drop ? IfDecision::Drop : IfDecision::Keep;
}

void stripExpr(ConstEvaluator& ev, ExprNode* expr);
void stripBlock(ConstEvaluator& ev, StatementBlockNode* blk);
void stripStmtList(ConstEvaluator& ev, ScopeNode* scope, vector<StatementNode*>& stmts);

void eraseStmtSymbols(ScopeNode* scope, StatementNode* stmt) {
    if (!scope || !stmt) return;
    if (auto* d = dynamic_cast<StatementDeclareNode*>(stmt)) {
        scope->eraseSymbol(d->name().getText());
        return;
    }
    if (auto* a = dynamic_cast<StatementDeclareAssignNode*>(stmt)) {
        scope->eraseSymbol(a->name().getText());
        return;
    }
    if (auto* t = dynamic_cast<StatementDeclareAssignTupleNode*>(stmt)) {
        for (const auto& n : t->names())
            scope->eraseSymbol(n.getText());
        return;
    }
    if (auto* al = dynamic_cast<AliasDeclNode*>(stmt)) {
        scope->eraseLocalAlias(al->name().getText());
    }
}

void stripExpr(ConstEvaluator& ev, ExprNode* expr) {
    if (!expr) return;
    if (auto* ife = dynamic_cast<ExprIfElseNode*>(expr)) {
        stripBlock(ev, ife->thenBlock());
        for (auto* elif : ife->elifs()) {
            if (elif) stripBlock(ev, elif->block());
        }
        stripBlock(ev, ife->elseBlock());
        return;
    }
    if (auto* m = dynamic_cast<ExprMatchNode*>(expr)) {
        stripExpr(ev, m->scrutinee());
        for (auto* arm : m->arms()) {
            if (!arm) continue;
            stripExpr(ev, arm->body());
            stripBlock(ev, arm->block());
        }
        return;
    }
    if (auto* tc = dynamic_cast<ExprTryCatchNode*>(expr)) {
        stripBlock(ev, tc->tryBlock());
        for (auto* c : tc->catches()) {
            if (c) stripBlock(ev, c->body());
        }
        return;
    }
    if (auto* lam = dynamic_cast<LambdaExprNode*>(expr)) {
        stripExpr(ev, lam->bodyExpr());
        auto stmts = lam->bodyStmts();
        stripStmtList(ev, lam->bodyScope(), stmts);
        lam->setBodyStmts(std::move(stmts));
        return;
    }
    if (auto* p = dynamic_cast<ExprParenNode*>(expr)) {
        stripExpr(ev, p->expr());
    }
}

void stripStmtNested(ConstEvaluator& ev, StatementNode* stmt) {
    if (!stmt) return;
    if (auto* lp = dynamic_cast<StatementLoopNode*>(stmt)) {
        stripBlock(ev, lp->block());
        stripExpr(ev, lp->initExpr());
        return;
    }
    if (auto* fi = dynamic_cast<StatementForInNode*>(stmt)) {
        stripExpr(ev, fi->expr());
        stripBlock(ev, fi->block());
        return;
    }
    if (auto* se = dynamic_cast<StatementExprNode*>(stmt)) {
        stripExpr(ev, se->expr());
        return;
    }
    if (auto* st = dynamic_cast<StatementSetNode*>(stmt)) {
        stripExpr(ev, st->arrayExpr());
        stripExpr(ev, st->valueExpr());
        for (auto* ix : st->indices())
            stripExpr(ev, ix);
        return;
    }
    if (auto* sf = dynamic_cast<StatementStaticFieldSetNode*>(stmt)) {
        stripExpr(ev, sf->valueExpr());
    }
}

void stripStmtList(ConstEvaluator& ev, ScopeNode* scope, vector<StatementNode*>& stmts) {
    vector<StatementNode*> keep;
    keep.reserve(stmts.size());
    for (auto* s : stmts) {
        if (!s) continue;
        if (decideIf(ev, s->prefixAnnos()) == IfDecision::Drop) {
            eraseStmtSymbols(scope, s);
            continue;
        }
        s->setPrefixAnnos(withoutIf(s->prefixAnnos()));
        stripStmtNested(ev, s);
        keep.push_back(s);
    }
    stmts = std::move(keep);
}

void stripBlock(ConstEvaluator& ev, StatementBlockNode* blk) {
    if (!blk) return;
    auto stmts = blk->statements();
    stripStmtList(ev, blk, stmts);
    blk->setStatements(std::move(stmts));
    if (blk->hasResult()) {
        if (decideIf(ev, blk->resultPrefixAnnos()) == IfDecision::Drop) {
            blk->clearResult();
            return;
        }
        blk->setResultPrefixAnnos(withoutIf(blk->resultPrefixAnnos()));
        stripExpr(ev, blk->resultExpr());
    }
}

void stripFnBody(ConstEvaluator& ev, FnNode* fn) {
    if (!fn) return;
    auto stmts = fn->body();
    stripStmtList(ev, fn, stmts);
    fn->setBody(std::move(stmts));
}

void stripHeaderIf(FnNode* fn) {
    if (!fn || !fn->header()) return;
    fn->header()->setAnnoCalls(withoutIf(fn->header()->annoCalls()));
}

} // namespace

void CfgStripPass::run(FileNode* file, Riu*) {
    if (!file) return;
    ConstEvaluator ev;
    ev.setFile(file);

    vector<GlobalConstNode*> keepConsts;
    for (auto* gc : file->getGlobalConsts()) {
        if (!gc) continue;
        if (decideIf(ev, gc->annoCalls()) == IfDecision::Drop) continue;
        gc->setAnnoCalls(withoutIf(gc->annoCalls()));
        if (gc->value()) {
            if (auto v = ev.eval(gc->value())) ev.setNamedConst(gc->name().getText(), *v);
        }
        keepConsts.push_back(gc);
    }
    file->retainGlobalConsts(std::move(keepConsts));

    vector<GlobalVarNode*> keepVars;
    for (auto* gv : file->getGlobalVars()) {
        if (!gv) continue;
        if (decideIf(ev, gv->annoCalls()) == IfDecision::Drop) continue;
        gv->setAnnoCalls(withoutIf(gv->annoCalls()));
        keepVars.push_back(gv);
    }
    file->retainGlobalVars(std::move(keepVars));

    vector<FnNode*> keepFns;
    for (auto* fn : file->getFunctions()) {
        if (!fn || !fn->header()) continue;
        if (decideIf(ev, fn->header()->annoCalls()) == IfDecision::Drop) continue;
        stripHeaderIf(fn);
        keepFns.push_back(fn);
    }
    file->retainFunctions(std::move(keepFns));

    vector<StructDeclNode*> keepStructs;
    for (auto* sd : file->getStructDecls()) {
        if (!sd) continue;
        if (decideIf(ev, sd->annoCalls()) == IfDecision::Drop) continue;
        sd->setAnnoCalls(withoutIf(sd->annoCalls()));
        keepStructs.push_back(sd);
    }
    file->retainStructDecls(std::move(keepStructs));

    vector<SpecDeclNode*> keepSpecs;
    for (auto* sp : file->getSpecDecls()) {
        if (!sp) continue;
        if (decideIf(ev, sp->annoCalls()) == IfDecision::Drop) continue;
        sp->setAnnoCalls(withoutIf(sp->annoCalls()));
        keepSpecs.push_back(sp);
    }
    file->retainSpecDecls(std::move(keepSpecs));

    vector<StructImplNode*> keepImpls;
    for (auto* impl : file->getStructImpls()) {
        if (!impl) continue;
        if (decideIf(ev, impl->annoCalls()) == IfDecision::Drop) continue;
        impl->setAnnoCalls(withoutIf(impl->annoCalls()));
        keepImpls.push_back(impl);
    }
    file->retainStructImpls(std::move(keepImpls));

    for (auto* sd : file->getStructDecls()) {
        if (!sd) continue;
        const string sname = sd->name().getText();
        vector<StructFieldNode*> keepFields;
        for (auto* f : sd->fields()) {
            if (!f) continue;
            if (decideIf(ev, f->annoCalls()) == IfDecision::Drop) {
                if (!f->isDiscard()) file->eraseSymbol(sname + "." + f->name().getText());
                continue;
            }
            f->setAnnoCalls(withoutIf(f->annoCalls()));
            keepFields.push_back(f);
        }
        sd->setFields(std::move(keepFields));

        vector<StructDeclNode::StaticFieldEntry> keepSf;
        for (const auto& sf : sd->staticFields()) {
            if (decideIf(ev, sf.annoCalls) == IfDecision::Drop) continue;
            auto copy = sf;
            copy.annoCalls = withoutIf(copy.annoCalls);
            keepSf.push_back(std::move(copy));
        }
        sd->setStaticFields(std::move(keepSf));
    }

    for (auto* impl : file->getStructImpls()) {
        if (!impl) continue;
        file->unbindImplMethodSymbols(impl);
        vector<FnNode*> keepMethods;
        for (auto* m : impl->methods()) {
            if (!m || !m->header()) continue;
            if (decideIf(ev, m->header()->annoCalls()) == IfDecision::Drop) continue;
            stripHeaderIf(m);
            keepMethods.push_back(m);
        }
        impl->setMethods(std::move(keepMethods));
        if (impl->destructor() && impl->destructor()->header()) {
            if (decideIf(ev, impl->destructor()->header()->annoCalls()) == IfDecision::Drop) {
                impl->setDestructor(nullptr);
            } else {
                stripHeaderIf(impl->destructor());
            }
        }
        file->rebindImplMethodSymbols(impl);
    }

    for (auto* fn : file->getFunctions())
        stripFnBody(ev, fn);
    for (auto* impl : file->getStructImpls()) {
        if (!impl) continue;
        for (auto* m : impl->methods())
            stripFnBody(ev, m);
        if (impl->destructor()) stripFnBody(ev, impl->destructor());
    }
    for (auto* sp : file->getSpecDecls()) {
        if (!sp) continue;
        for (auto* body : sp->defaultBodies())
            stripFnBody(ev, body);
    }
}

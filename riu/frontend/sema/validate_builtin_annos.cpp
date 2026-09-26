// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "validate_builtin_annos.h"

#include "ast/node/anno_call.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "builtin_annos.h"
#include "const_eval.h"
#include "error_code.h"

namespace {

void throwAnnoSite(const AnnoCall& call) {
    throw RiuError(call.line, call.col, ErrorCode::E2011, call.name);
}

void throwAnnoConst(const AnnoCall& call) {
    throw RiuError(call.line, call.col, ErrorCode::E3163, call.name);
}

void throwAnnoDup(const AnnoCall& call) {
    throw RiuError(call.line, call.col, ErrorCode::E3119, call.name);
}

void checkRepeatable(const vector<AnnoCall>& calls) {
    std::map<string, int> seen;
    for (const auto& c : calls) {
        const BuiltinAnnoSpec* spec = lookupBuiltinAnnoSpec(c.name);
        if (!spec || spec->repeatable) continue;
        if (++seen[c.name] > 1) throwAnnoDup(c);
    }
}

void evalIfBool(ConstEvaluator& ev, const AnnoCall& call) {
    if (call.args.empty() || !call.args[0].expr) throwAnnoConst(call);
    auto v = ev.eval(call.args[0].expr);
    if (!v || !v->isBool()) throwAnnoConst(call);
}

void evalCNameString(ConstEvaluator& ev, const AnnoCall& call) {
    if (call.args.empty() || !call.args[0].expr) throwAnnoConst(call);
    auto v = ev.eval(call.args[0].expr);
    if (!v || !v->isString()) throwAnnoConst(call);
}

void validateAnnoCalls(ConstEvaluator& ev, const vector<AnnoCall>& calls, AnnoAttachSite site) {
    checkRepeatable(calls);
    for (const auto& call : calls) {
        const BuiltinAnnoSpec* spec = lookupBuiltinAnnoSpec(call.name);
        if (!spec) continue;
        if (!builtinAnnoAllowsSite(*spec, site)) throwAnnoSite(call);
        if (call.name == "If") evalIfBool(ev, call);
        if (call.name == "CName") evalCNameString(ev, call);
    }
}

void validateStmtTree(ConstEvaluator& ev, StatementNode* stmt);
void validateBlock(ConstEvaluator& ev, StatementBlockNode* blk);

void validateBlock(ConstEvaluator& ev, StatementBlockNode* blk) {
    if (!blk) return;
    for (auto* s : blk->statements())
        validateStmtTree(ev, s);
}

void validateFnBody(ConstEvaluator& ev, const vector<StatementNode*>& body) {
    for (auto* s : body)
        validateStmtTree(ev, s);
}

void validateStmtTree(ConstEvaluator& ev, StatementNode* stmt) {
    if (!stmt) return;
    validateAnnoCalls(ev, stmt->prefixAnnos(), AnnoAttachSite::Code);
    if (auto* lp = dynamic_cast<StatementLoopNode*>(stmt)) {
        validateBlock(ev, lp->block());
        return;
    }
    if (auto* fi = dynamic_cast<StatementForInNode*>(stmt)) {
        validateBlock(ev, fi->block());
        return;
    }
    if (auto* se = dynamic_cast<StatementExprNode*>(stmt)) {
        if (auto* ife = dynamic_cast<ExprIfElseNode*>(se->expr())) {
            validateBlock(ev, ife->thenBlock());
            for (auto* elif : ife->elifs()) {
                if (elif) validateBlock(ev, elif->block());
            }
            validateBlock(ev, ife->elseBlock());
        }
    }
}

void validateFn(ConstEvaluator& ev, FnNode* fn, AnnoAttachSite site) {
    if (!fn || !fn->header()) return;
    validateAnnoCalls(ev, fn->header()->annoCalls(), site);
    validateFnBody(ev, fn->body());
}

} // namespace

namespace sema {

void validateBuiltinAnnos(FileNode* file, ConstEvaluator& cvalEv) {
    if (!file) return;

    for (auto& sd : file->getStructDecls()) {
        if (!sd) continue;
        validateAnnoCalls(cvalEv, sd->annoCalls(), AnnoAttachSite::StructDecl);
    }
    for (auto& impl : file->getStructImpls()) {
        if (!impl) continue;
        for (auto& m : impl->methods())
            validateFn(cvalEv, m, AnnoAttachSite::FnDecl);
        if (impl->destructor()) validateFn(cvalEv, impl->destructor(), AnnoAttachSite::FnDecl);
    }
    for (auto& fn : file->getFunctions()) {
        if (!fn || !fn->header()) continue;
        const bool externFn = fn->body().empty() && fn->header()->hasAnno("CName");
        validateAnnoCalls(cvalEv, fn->header()->annoCalls(),
                          externFn ? AnnoAttachSite::ExternFn : AnnoAttachSite::FnDecl);
        if (!externFn) validateFnBody(cvalEv, fn->body());
    }
    for (auto& ed : file->getEnumDecls()) {
        if (!ed) continue;
        validateAnnoCalls(cvalEv, ed->annoCalls(), AnnoAttachSite::EnumDecl);
    }
}

} // namespace sema

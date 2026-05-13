// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 实现 —— 详见 sema_pass.h
//
// Phase 3.1：骨架 only。walk file→fn→stmt→expr 全树, visitExpr 当前不写
// 任何标注 —— 待 3.2 接 `setResolvedType(node->getType())` 与
// compile<Foo>Expr 入口并存校验后, 再把 throw 按桶迁过来。

#include "sema/sema_pass.h"

#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"

SemaPass::SemaPass(p<FileNode> file) : _file(file) {
}

void SemaPass::run() {
    if (!_file) return;
    for (auto& fn : _file->getFunctions()) {
        // 泛型模板 / #CompilerInner 不走常规 codegen, 在 Compiler::compile 里也
        // 是被跳过的; SemaPass 这里同步跳过, 保持与 codegen 覆盖一致。
        if (fn->header()->isGeneric()) continue;
        if (fn->header()->hasAnno("CompilerInner")) continue;
        visitFn(fn);
    }
}

void SemaPass::visitFn(p<FnNode> fn) {
    if (!fn) return;
    for (auto& stmt : fn->body()) {
        visitStmt(stmt);
    }
}

void SemaPass::visitBlock(p<StatementBlockNode> block) {
    if (!block) return;
    for (auto& s : block->statements()) {
        visitStmt(s);
    }
    if (block->hasResult()) {
        visitExpr(block->resultExpr());
    }
}

void SemaPass::visitStmt(p<StatementNode> stmt) {
    if (!stmt) return;
    if (auto loop = dynamic_cast<p<StatementLoopNode>>(stmt)) {
        visitBlock(loop->block());
        return;
    }
    if (auto set = dynamic_cast<p<StatementSetNode>>(stmt)) {
        visitExpr(set->arrayExpr());
        for (auto& idx : set->indices()) visitExpr(idx);
        visitExpr(set->valueExpr());
        return;
    }
    if (dynamic_cast<p<StatementBreakNode>>(stmt)) return;
    if (dynamic_cast<p<StatementRetVoidNode>>(stmt)) return;
    if (dynamic_cast<p<StatementDeclareNode>>(stmt)) return; // 无表达式
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
        if (se->expr()) visitExpr(se->expr());
        return;
    }
    // 兜底：未识别的 stmt 直接跳过, 不抛错 —— SemaPass 当前是 no-op, 漏处理
    // 不应阻塞 codegen; 3.2 起开始有实际写入后再改成 assert(false)。
}

void SemaPass::visitExpr(p<ExprNode> expr) {
    if (!expr) return;
    // Phase 3.1：visitExpr 暂为空 —— 不写 resolvedType / resolvedSymbol。
    // 但递归继续往下走, 保证 3.2 接入写入逻辑后所有节点都被访问到。

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps()) visitExpr(e);
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr()); return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr());
        for (auto& a : n->getArgs()) visitExpr(a);
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        visitExpr(n->baseExpr()); return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitBlock(n->thenBlock());
        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            visitBlock(el->block());
        }
        if (n->elseBlock()) visitBlock(n->elseBlock());
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElsePreValueNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices()) visitExpr(i);
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // body 走 bodyExpr (Single/Paren) 或 bodyStmts (Block/ZeroBlock) 二选一
        if (n->bodyExpr()) visitExpr(n->bodyExpr());
        for (auto& s : n->bodyStmts()) visitStmt(s);
        return;
    }
    if (auto n = dynamic_cast<p<ExprEnumCtorNode>>(expr)) {
        for (auto& a : n->args()) visitExpr(a);
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms()) visitExpr(arm->body());
        return;
    }
    if (auto n = dynamic_cast<p<ExprTryCatchNode>>(expr)) {
        visitBlock(n->tryBlock());
        for (auto& c : n->catches()) visitBlock(c->body());
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg()); return;
    }
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    // ExprGetRefNode / ExprArrayInitNode 无子表达式 (ArrayInit 的 value 是
    // LiteralNode, 不递归)。其余未识别节点 3.2 起补 assert。
}

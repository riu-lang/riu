// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast_visitor.h"

#include "alias_node.h"
#include "expr_node.h"
#include "statement_node.h"

void ExprCallNode::accept(AstVisitor& v) {
    v.visitCall(*this);
}
void ExprLiteralNode::accept(AstVisitor& v) {
    v.visitLiteral(*this);
}
void ExprAddSubNode::accept(AstVisitor& v) {
    v.visitAddSub(*this);
}
void ExprMulDivModNode::accept(AstVisitor& v) {
    v.visitMulDivMod(*this);
}
void ExprBinOpNode::accept(AstVisitor& v) {
    v.visitBinOp(*this);
}
void ExprParenNode::accept(AstVisitor& v) {
    v.visitParen(*this);
}
void ExprDotNode::accept(AstVisitor& v) {
    v.visitDot(*this);
}
void ExprCompareNode::accept(AstVisitor& v) {
    v.visitCompare(*this);
}
void ExprIfElseNode::accept(AstVisitor& v) {
    v.visitIfElse(*this);
}
void ExprOneLineIfElseNode::accept(AstVisitor& v) {
    v.visitOneLineIfElse(*this);
}
void ExprGetNode::accept(AstVisitor& v) {
    v.visitGet(*this);
}
void ExprArrayNode::accept(AstVisitor& v) {
    v.visitArray(*this);
}
void ExprArrayInitNode::accept(AstVisitor& v) {
    v.visitArrayInit(*this);
}
void ExprGetRefNode::accept(AstVisitor& v) {
    v.visitGetRef(*this);
}
void ExprUnaryNode::accept(AstVisitor& v) {
    v.visitUnary(*this);
}
void LambdaExprNode::accept(AstVisitor& v) {
    v.visitLambda(*this);
}
void ExprTupleNode::accept(AstVisitor& v) {
    v.visitTuple(*this);
}
void ExprPathCallNode::accept(AstVisitor& v) {
    v.visitPathCall(*this);
}
void ExprStructLitNode::accept(AstVisitor& v) {
    v.visitStructLit(*this);
}
void ExprMatchNode::accept(AstVisitor& v) {
    v.visitMatch(*this);
}
void ExprTryCatchNode::accept(AstVisitor& v) {
    v.visitTryCatch(*this);
}
void ExprDynCtorNode::accept(AstVisitor& v) {
    v.visitDynCtor(*this);
}
void ExprMoveAssignNode::accept(AstVisitor& v) {
    v.visitMoveAssign(*this);
}
void ExprNullElseNode::accept(AstVisitor& v) {
    v.visitNullElse(*this);
}

void StatementBlockNode::accept(AstVisitor& v) {
    v.visitBlock(*this);
}

void StatementExprNode::accept(AstVisitor& v) {
    v.visitExprStmt(*this);
}
void StatementRetNode::accept(AstVisitor& v) {
    v.visitRet(*this);
}
void StatementRetVoidNode::accept(AstVisitor& v) {
    v.visitRetVoid(*this);
}
void StatementDeclareNode::accept(AstVisitor& v) {
    v.visitDeclare(*this);
}
void StatementDeclareAssignNode::accept(AstVisitor& v) {
    v.visitDeclareAssign(*this);
}
void StatementDeclareAssignTupleNode::accept(AstVisitor& v) {
    v.visitDeclareAssignTuple(*this);
}
void StatementAssignNode::accept(AstVisitor& v) {
    v.visitAssign(*this);
}
void StatementLoopNode::accept(AstVisitor& v) {
    v.visitLoop(*this);
}
void StatementBreakNode::accept(AstVisitor& v) {
    v.visitBreak(*this);
}
void StatementContinueNode::accept(AstVisitor& v) {
    v.visitContinue(*this);
}
void StatementForInNode::accept(AstVisitor& v) {
    v.visitForIn(*this);
}
void StatementStaticFieldSetNode::accept(AstVisitor& v) {
    v.visitStaticFieldSet(*this);
}
void StatementSetNode::accept(AstVisitor& v) {
    v.visitSet(*this);
}
void AliasDeclNode::accept(AstVisitor& v) {
    v.visitAlias(*this);
}

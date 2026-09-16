// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_AST_VISITOR_H
#define RIU_LANG_AST_VISITOR_H

// AST 双分派：每个表达式 / 语句节点一个 visitX = 0。
// 新节点 = 基类加纯虚 + 节点 accept；漏 override → 编不过
// （4.2 Sema / 4.3 codegen / 4.4 formatter）。
// 本头只前向声明节点，不断环。

class ExprCallNode;
class ExprLiteralNode;
class ExprAddSubNode;
class ExprMulDivModNode;
class ExprBinOpNode;
class ExprParenNode;
class ExprDotNode;
class ExprCompareNode;
class ExprIfElseNode;
class ExprOneLineIfElseNode;
class ExprGetNode;
class ExprArrayNode;
class ExprArrayInitNode;
class ExprGetRefNode;
class ExprUnaryNode;
class LambdaExprNode;
class ExprTupleNode;
class ExprPathCallNode;
class ExprStructLitNode;
class ExprMatchNode;
class ExprTryCatchNode;
class ExprDynCtorNode;
class ExprMoveAssignNode;
class ExprNullElseNode;
class StatementBlockNode;
class StatementExprNode;
class StatementRetNode;
class StatementRetVoidNode;
class StatementDeclareNode;
class StatementDeclareAssignNode;
class StatementDeclareAssignTupleNode;
class StatementAssignNode;
class StatementLoopNode;
class StatementBreakNode;
class StatementContinueNode;
class StatementForInNode;
class StatementStaticFieldSetNode;
class StatementSetNode;

class AstVisitor {
public:
    virtual ~AstVisitor() = default;

    virtual void visitCall(ExprCallNode&) = 0;
    virtual void visitLiteral(ExprLiteralNode&) = 0;
    virtual void visitAddSub(ExprAddSubNode&) = 0;
    virtual void visitMulDivMod(ExprMulDivModNode&) = 0;
    virtual void visitBinOp(ExprBinOpNode&) = 0;
    virtual void visitParen(ExprParenNode&) = 0;
    virtual void visitDot(ExprDotNode&) = 0;
    virtual void visitCompare(ExprCompareNode&) = 0;
    virtual void visitIfElse(ExprIfElseNode&) = 0;
    virtual void visitOneLineIfElse(ExprOneLineIfElseNode&) = 0;
    virtual void visitGet(ExprGetNode&) = 0;
    virtual void visitArray(ExprArrayNode&) = 0;
    virtual void visitArrayInit(ExprArrayInitNode&) = 0;
    virtual void visitGetRef(ExprGetRefNode&) = 0;
    virtual void visitUnary(ExprUnaryNode&) = 0;
    virtual void visitLambda(LambdaExprNode&) = 0;
    virtual void visitTuple(ExprTupleNode&) = 0;
    virtual void visitPathCall(ExprPathCallNode&) = 0;
    virtual void visitStructLit(ExprStructLitNode&) = 0;
    virtual void visitMatch(ExprMatchNode&) = 0;
    virtual void visitTryCatch(ExprTryCatchNode&) = 0;
    virtual void visitDynCtor(ExprDynCtorNode&) = 0;
    virtual void visitMoveAssign(ExprMoveAssignNode&) = 0;
    virtual void visitNullElse(ExprNullElseNode&) = 0;

    virtual void visitBlock(StatementBlockNode&) = 0;

    virtual void visitExprStmt(StatementExprNode&) = 0;
    virtual void visitRet(StatementRetNode&) = 0;
    virtual void visitRetVoid(StatementRetVoidNode&) = 0;
    virtual void visitDeclare(StatementDeclareNode&) = 0;
    virtual void visitDeclareAssign(StatementDeclareAssignNode&) = 0;
    virtual void visitDeclareAssignTuple(StatementDeclareAssignTupleNode&) = 0;
    virtual void visitAssign(StatementAssignNode&) = 0;
    virtual void visitLoop(StatementLoopNode&) = 0;
    virtual void visitBreak(StatementBreakNode&) = 0;
    virtual void visitContinue(StatementContinueNode&) = 0;
    virtual void visitForIn(StatementForInNode&) = 0;
    virtual void visitStaticFieldSet(StatementStaticFieldSetNode&) = 0;
    virtual void visitSet(StatementSetNode&) = 0;
};

#endif // RIU_LANG_AST_VISITOR_H

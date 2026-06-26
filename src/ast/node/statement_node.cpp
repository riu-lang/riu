// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "statement_node.h"
#include "expr_node.h"

const p<ExprNode>& StatementExprNode::expr() const {
    return _expr;
}

bool StatementExprNode::hasSemicolon() const {
    return _hasSemicolon;
}

Token StatementDeclareNode::name() const {
    return _name;
}

p<TypeNode> StatementDeclareNode::varType() const {
    return _type;
}

Token StatementDeclareAssignNode::name() const {
    return _name;
}

p<TypeNode> StatementDeclareAssignNode::varType() const {
    return _type;
}

StatementLoopNode::StatementLoopNode(const p<Node>& parent, p<StatementBlockNode> block, vector<Token> initNames,
                                     p<TypeNode> initType, p<ExprNode> initExpr)
    : StatementNode(parent), _block(block), _initNames(std::move(initNames)), _initType(initType), _initExpr(initExpr) {
}

const p<StatementBlockNode>& StatementLoopNode::block() const {
    return _block;
}

const p<ExprNode>& StatementSetNode::arrayExpr() const {
    return _arrayExpr;
}

const vector<p<ExprNode>>& StatementSetNode::indices() const {
    return _indices;
}

const p<ExprNode>& StatementSetNode::valueExpr() const {
    return _valueExpr;
}

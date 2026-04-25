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

DeclareType StatementDeclareAssignNode::declareType() const {
    return _declareType;
}

Token StatementDeclareAssignNode::name() const {
    return _name;
}

p<TypeNode> StatementDeclareAssignNode::varType() const {
    return _type;
}

StatementLoopNode::StatementLoopNode(const p<Node>& parent, p<StatementBlockNode> block) :
    StatementNode(parent),
    _block(std::move(block)) {
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

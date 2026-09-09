// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "statement_node.h"
#include "expr_node.h"

ExprNode* StatementExprNode::expr() const {
    return _expr;
}

bool StatementExprNode::hasSemicolon() const {
    return _hasSemicolon;
}

Token StatementDeclareNode::name() const {
    return _name;
}

TypeNode* StatementDeclareNode::varType() const {
    return _type;
}

Token StatementDeclareAssignNode::name() const {
    return _name;
}

TypeNode* StatementDeclareAssignNode::varType() const {
    return _type;
}

StatementLoopNode::StatementLoopNode(Node* parent, StatementBlockNode* block, Token label, vector<Token> initNames,
                                     TypeNode* initType, ExprNode* initExpr)
    : StatementNode(parent), _block(block), _label(std::move(label)), _initNames(std::move(initNames)),
      _initType(initType), _initExpr(initExpr) {}

StatementBlockNode* StatementLoopNode::block() const {
    return _block;
}

StatementForInNode::StatementForInNode(Node* parent, StatementBlockNode* block, Token item, ExprNode* expr, Token label)
    : StatementNode(parent), _block(block), _label(std::move(label)), _item(std::move(item)), _expr(expr) {}

StatementBlockNode* StatementForInNode::block() const {
    return _block;
}

ExprNode* StatementSetNode::arrayExpr() const {
    return _arrayExpr;
}

const vector<ExprNode*>& StatementSetNode::indices() const {
    return _indices;
}

ExprNode* StatementSetNode::valueExpr() const {
    return _valueExpr;
}

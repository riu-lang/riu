// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#include "statement_node.h"

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

Token StatementAssignNode::name() const {
    return _name;
}

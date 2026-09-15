// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "fn_node.h"

Token FnParamNode::name() const {
    return _name;
}

TypeNode* FnParamNode::type() const {
    return _type;
}

void FnHeaderNode::addParam(FnParamNode* param) {
    _params.push_back(param);
}

Token FnHeaderNode::name() const {
    return _name;
}

TypeNode* FnHeaderNode::retType() const {
    return _retType;
}

vector<FnParamNode*> FnHeaderNode::params() const {
    return _params;
}

TypeInfo FnHeaderNode::getType() const {
    if (_retType) {
        return _retType->getType();
    }
    return {};
}

string FnHeaderNode::resolvedFallibleErr() const {
    if (_fallibleErrType) return _fallibleErrType->getType().name;
    return {};
}

FnNode::FnNode(Node* parent, FnHeaderNode* header) : ScopeNode(parent), _header(header) {}

void FnNode::addStatement(StatementNode* stmt) {
    _body.push_back(stmt);
}

const vector<StatementNode*>& FnNode::body() const {
    return _body;
}

FnHeaderNode* FnNode::header() const {
    return _header;
}

TypeInfo FnNode::getType() const {
    return _header->getType();
}

string FnNode::getLocation() const {
    return _header->name().getText();
}

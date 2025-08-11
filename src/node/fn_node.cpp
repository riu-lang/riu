// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#include "fn_node.h"

Token FnParamNode::name() const {
    return _name;
}

Token FnParamNode::type() const {
    return _type;
}

void FnHeaderNode::addParam(p<FnParamNode> param) {
    _params.push_back(param);
}

Token FnHeaderNode::name() const {
    return _name;
}

Token FnHeaderNode::retType() const {
    return _retType;
}

vector<p<FnParamNode>> FnHeaderNode::params() const {
    return _params;
}

string FnHeaderNode::getType() const {
    string res = "fn()";
    return res;
}

FnNode::FnNode(const p<Node>& parent, p<FnHeaderNode> header) :
    ScopeNode(parent),
    _header(header) {
}

void FnNode::addStatement(p<StatementNode> stmt) {
    _body.push_back(std::move(stmt));
}

const vector<p<StatementNode>>& FnNode::body() const { return _body; }

const p<FnHeaderNode>& FnNode::header() const { return _header; }

string FnNode::getType() const {
    return _header->getType();
}

string FnNode::getLocation() const {
    return _header->name()->getText();
}

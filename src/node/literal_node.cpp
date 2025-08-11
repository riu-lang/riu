// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#include "literal_node.h"

#include <utility>
#include <regex>

LiteralNode::LiteralNode(Token value) :
    Node(nullptr), _value(std::move(value)) {
}

Token LiteralNode::getValue() const {
    return _value;
}

string LiteralNode::getLocation() const {
    return _value->getText();
}

LiteralNumberNode::LiteralNumberNode(Token value) : LiteralNode(std::move(value)) {
}

LiteralIntNode::LiteralIntNode(Token value) : LiteralNumberNode(std::move(value)) {
    const auto v = value->getText();
    // language=RegExp
    static const std::regex type_regex(R"([ui](\d+)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = match.str();
    } else
        _type = "i32";
}

string LiteralIntNode::getType() const {
    return _type;
}

LiteralFloatNode::LiteralFloatNode(const Token& value) : LiteralNumberNode(value) {
    const auto v = value->getText();
    // language=RegExp
    static const std::regex type_regex(R"(f(\d+)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = match.str();
    } else
        _type = "f64";
}

string LiteralFloatNode::getType() const {
    return _type;
}

LiteralBoolNode::LiteralBoolNode(Token value) : LiteralNode(std::move(value)) {
}

string LiteralBoolNode::getType() const {
    return "bool";
}

LiteralObjNode::LiteralObjNode(const p<Node>& parent, const Token& value) : LiteralNode(value) {
    _parent = parent;
}

string LiteralObjNode::getType() const {
    auto name = _value->getText();
    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(name);
        if (sym) {
            return sym->type;
        }
    }
    throw YuxError("Symbol {} not found", name);
}

string LiteralObjNode::getLocation() const {
    return _parent->getLocation() + "." + _value->getText();
}

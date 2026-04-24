// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

//
// Created by yjl_1 on 2026/3/29.
//

#ifndef YUX_LANG_FN_NODE_H
#define YUX_LANG_FN_NODE_H

#include "node.h"
#include "type_node.h"

#include <utility>

class FnHeaderNode;
class StatementNode;
class LiteralNode;
class ExprNode;

class FnParamNode : public Node {
protected:
    Token _name;
    p<TypeNode> _type;

public:
    explicit FnParamNode(const p<Node>& parent, Token name, p<TypeNode> type) :
        Node(parent), _name(name), _type(type) {
    }

    [[nodiscard]] Token name() const;
    [[nodiscard]] p<TypeNode> type() const;
};

class FnHeaderNode : public Node, public Named, public Typed, public Annotated {
protected:
    vector<p<FnParamNode>> _params;
    vector<string> _typeParams;
    p<TypeNode> _retType;

public:
    FnHeaderNode(const p<Node>& parent, Token name, p<TypeNode> retType) :
        Node(parent), Named(name), _retType(retType) {
    }

    void addParam(p<FnParamNode> param);

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    [[nodiscard]] Token name() const override;
    [[nodiscard]] p<TypeNode> retType() const;
    [[nodiscard]] vector<p<FnParamNode>> params() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class FnNode : public ScopeNode, public Typed {
    p<FnHeaderNode> _header;
    vector<p<StatementNode>> _body;

public:
    explicit FnNode(const p<Node>& parent, p<FnHeaderNode> header);

    void addStatement(p<StatementNode> stmt);

    [[nodiscard]] const vector<p<StatementNode>>& body() const;
    [[nodiscard]] const p<FnHeaderNode>& header() const;

    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] string getLocation() const override;
};

#endif //YUX_LANG_FN_NODE_H

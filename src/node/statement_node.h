// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#ifndef YUX_LANG_STATEMENT_NODE_H
#define YUX_LANG_STATEMENT_NODE_H

#include <utility>

#include "expr_node.h"
#include "type_node.h"


class StatementNode : public Node {
public:
    explicit StatementNode(const p<Node>& parent) : Node(parent) {
    }
};

class StatementExprNode : public StatementNode {
protected:
    p<ExprNode> _expr;
    bool _hasSemicolon;

public:
    explicit StatementExprNode(const p<Node>& parent, p<ExprNode> expr, bool hasSemicolon = true) :
        StatementNode(parent),
        _expr(std::move(expr)),
        _hasSemicolon(hasSemicolon) {
    }

    [[nodiscard]] const p<ExprNode>& expr() const;
    [[nodiscard]] bool hasSemicolon() const;
};

class StatementRetNode : public StatementExprNode {
public:
    explicit StatementRetNode(const p<Node>& parent, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)) {
    }
};

enum class DeclareType {
    Var,
    Val,
    CVal
};


class StatementDeclareAssignNode : public StatementExprNode {
protected:
    DeclareType _declareType;
    Token _name;
    p<TypeNode> _type;

public:
    explicit StatementDeclareAssignNode(const p<Node>& parent, DeclareType declType, Token name, p<TypeNode> type, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)), _declareType(declType), _name(name), _type(type) {
    }

    [[nodiscard]] DeclareType declareType() const;
    [[nodiscard]] Token name() const;
    [[nodiscard]] p<TypeNode> varType() const;
};

class StatementAssignNode : public StatementExprNode {
protected:
    Token _obj;
    vector<Token> _subs;

public:
    explicit StatementAssignNode(const p<Node>& parent, Token obj, vector<Token> subs, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)),
        _obj(obj),
        _subs(std::move(subs)) {
    }

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
};

#endif //YUX_LANG_STATEMENT_NODE_H

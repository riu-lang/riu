// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#ifndef YUX_LANG_STATEMENT_NODE_H
#define YUX_LANG_STATEMENT_NODE_H

#include <utility>

#include "expr_node.h"


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
    Token _type;

public:
    explicit StatementDeclareAssignNode(const p<Node>& parent, DeclareType declType, Token name, Token type, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)), _declareType(declType), _name(name), _type(type) {
    }

    [[nodiscard]] DeclareType declareType() const;
    [[nodiscard]] Token name() const;
    [[nodiscard]] Token varType() const;
};

class StatementAssignNode : public StatementExprNode {
protected:
    Token _name;

public:
    explicit StatementAssignNode(const p<Node>& parent, Token name, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)), _name(name) {
    }

    [[nodiscard]] Token name() const;
};

#endif //YUX_LANG_STATEMENT_NODE_H

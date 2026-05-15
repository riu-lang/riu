// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

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

class StatementRetVoidNode : public StatementNode {
public:
    explicit StatementRetVoidNode(const p<Node>& parent) : StatementNode(parent) {}
};

// DRAFT-let-unify §3：`let x` 默认浅不可变（isMut=false, isConst=false）；
// `#Mut let x` → isMut=true；`#Cval let x` → isConst=true（且不可重赋）。
class StatementDeclareNode : public StatementNode {
protected:
    bool _isMut;
    bool _isConst;
    Token _name;
    p<TypeNode> _type;

public:
    explicit StatementDeclareNode(const p<Node>& parent, bool isMut, bool isConst, Token name, p<TypeNode> type) :
        StatementNode(parent), _isMut(isMut), _isConst(isConst), _name(name), _type(type) {
    }

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    [[nodiscard]] Token name() const;
    [[nodiscard]] p<TypeNode> varType() const;
};

class StatementDeclareAssignNode : public StatementExprNode {
protected:
    bool _isMut;
    bool _isConst;
    Token _name;
    p<TypeNode> _type;

public:
    explicit StatementDeclareAssignNode(const p<Node>& parent, bool isMut, bool isConst, Token name, p<TypeNode> type, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)), _isMut(isMut), _isConst(isConst), _name(name), _type(type) {
    }

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    [[nodiscard]] Token name() const;
    [[nodiscard]] p<TypeNode> varType() const;
};

// 元组解构赋值声明：let (a, b, ...) = expr（默认浅不可变；`#Mut let (...)` → isMut=true）
// 平铺一层 ID，不支持嵌套与 _；可选总类型标注 typeWithRef，指代整个元组类型
class StatementDeclareAssignTupleNode : public StatementExprNode {
protected:
    bool _isMut;
    bool _isConst;
    vector<Token> _names;
    p<TypeNode> _type; // 可选，整体元组类型（含 typeWithRef）

public:
    explicit StatementDeclareAssignTupleNode(const p<Node>& parent, bool isMut, bool isConst,
                                             vector<Token> names, p<TypeNode> type, p<ExprNode> expr) :
        StatementExprNode(parent, std::move(expr)), _isMut(isMut), _isConst(isConst),
        _names(std::move(names)), _type(std::move(type)) {
    }

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    [[nodiscard]] const vector<Token>& names() const { return _names; }
    [[nodiscard]] p<TypeNode> varType() const { return _type; }
};

enum class AssignOp {
    Eq,
    AddEq,
    SubEq,
    MulEq,
    DivEq,
    ModEq,
    MtMtEq,
    LtLtEq
};

class StatementAssignNode : public StatementExprNode {
protected:
    Token _obj;
    vector<Token> _subs;
    AssignOp _op;

public:
    explicit StatementAssignNode(const p<Node>& parent, Token obj, vector<Token> subs, p<ExprNode> expr, AssignOp op = AssignOp::Eq) :
        StatementExprNode(parent, std::move(expr)),
        _obj(obj),
        _subs(std::move(subs)),
        _op(op) {
    }

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
    [[nodiscard]] AssignOp op() const { return _op; }
};

class StatementBlockNode;

class StatementLoopNode : public StatementNode {
protected:
    p<StatementBlockNode> _block;

public:
    explicit StatementLoopNode(const p<Node>& parent, p<StatementBlockNode> block);

    [[nodiscard]] const p<StatementBlockNode>& block() const;
};

class StatementBreakNode : public StatementNode {
public:
    explicit StatementBreakNode(const p<Node>& parent) : StatementNode(parent) {}
};

class StatementSetNode : public StatementNode {
    p<ExprNode> _arrayExpr;
    vector<p<ExprNode>> _indices;
    p<ExprNode> _valueExpr;

public:
    StatementSetNode(const p<Node>& parent, p<ExprNode> arrayExpr, vector<p<ExprNode>> indices, p<ExprNode> valueExpr) :
        StatementNode(parent),
        _arrayExpr(arrayExpr),
        _indices(std::move(indices)),
        _valueExpr(valueExpr) {
    }

    [[nodiscard]] const p<ExprNode>& arrayExpr() const;
    [[nodiscard]] const vector<p<ExprNode>>& indices() const;
    [[nodiscard]] const p<ExprNode>& valueExpr() const;
};

#endif //YUX_LANG_STATEMENT_NODE_H

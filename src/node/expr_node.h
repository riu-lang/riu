// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_EXPR_NODE_H
#define YUX_LANG_EXPR_NODE_H

#include "node.h"

#include <utility>
#include <vector>

#include "literal_node.h"

class StatementNode;

class ExprNode : public Node, public Typed {
public:
    ExprNode(const p<Node>& parent) : Node(parent) {
    }
};

class ExprCallNode : public ExprNode {
protected:
    p<ExprNode> _calleeExpr;
    vector<p<ExprNode>> _args;

public:
    ExprCallNode(const p<Node>& parent, p<ExprNode> callee) :
        ExprNode(parent),
        _calleeExpr(callee) {
    }

    void addArg(p<ExprNode> arg) {
        _args.push_back(arg);
    }

    [[nodiscard]] const p<ExprNode>& getCalleeExpr() const;
    [[nodiscard]] const std::vector<p<ExprNode>>& getArgs() const;

    [[nodiscard]] TypeInfo getType() const override;
};

class ExprLiteralNode : public ExprNode {
protected:
    p<LiteralNode> _literal;

public:
    explicit ExprLiteralNode(const p<Node>& parent, p<LiteralNode> literal) :
        ExprNode(parent),
        _literal(literal) {
    }

    [[nodiscard]] const p<LiteralNode>& literal() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprAddSubNode : public ExprNode {
public:
    enum class Op { Add, Sub };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprAddSubNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right) :
        ExprNode(parent),
        _op(op), _left(left),
        _right(right) {
    }

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprMulDivModNode : public ExprNode {
public:
    enum class Op { Mul, Div, Mod };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprMulDivModNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right) :
        ExprNode(parent),
        _op(op), _left(left),
        _right(right) {
    }

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprParenNode : public ExprNode {
    p<ExprNode> _inner;

public:
    ExprParenNode(const p<Node>& parent, p<ExprNode> inner) :
        ExprNode(parent),
        _inner(inner) {
    }

    [[nodiscard]] const p<ExprNode>& expr() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprDotNode : public ExprNode {
protected:
    p<ExprNode> _baseExpr;
    Token _member;

public:
    ExprDotNode(const p<Node>& parent, p<ExprNode> baseExpr, Token member) :
        ExprNode(parent),
        _baseExpr(baseExpr), _member(member) {
    }

    [[nodiscard]] const p<ExprNode>& baseExpr() const;
    [[nodiscard]] string member() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprCompareNode : public ExprNode {
public:
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprCompareNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right) :
        ExprNode(parent),
        _op(op), _left(left),
        _right(right) {
    }

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class StatementBlockNode : public ScopeNode {
    vector<p<StatementNode>> _statements;
    p<ExprNode> _resultExpr;
    bool _hasResult;

public:
    StatementBlockNode(const p<Node>& parent, vector<p<StatementNode>> statements, p<ExprNode> resultExpr, bool hasResult);

    [[nodiscard]] const vector<p<StatementNode>>& statements() const;
    [[nodiscard]] const p<ExprNode>& resultExpr() const;
    [[nodiscard]] bool hasResult() const;
};

class ExprElIfNode : public Node {
    p<ExprNode> _condition;
    p<StatementBlockNode> _block;

public:
    ExprElIfNode(const p<Node>& parent, p<ExprNode> condition, p<StatementBlockNode> block) :
        Node(parent),
        _condition(condition),
        _block(block) {
    }

    [[nodiscard]] const p<ExprNode>& condition() const;
    [[nodiscard]] const p<StatementBlockNode>& block() const;
};

class ExprIfElseNode : public ExprNode {
    p<ExprNode> _condition;
    p<StatementBlockNode> _thenBlock;
    vector<p<ExprElIfNode>> _elifs;
    p<StatementBlockNode> _elseBlock;

public:
    ExprIfElseNode(const p<Node>& parent, p<ExprNode> condition, p<StatementBlockNode> thenBlock,
                   vector<p<ExprElIfNode>> elifs, p<StatementBlockNode> elseBlock) :
        ExprNode(parent),
        _condition(condition),
        _thenBlock(thenBlock),
        _elifs(std::move(elifs)),
        _elseBlock(elseBlock) {
    }

    [[nodiscard]] const p<ExprNode>& condition() const;
    [[nodiscard]] const p<StatementBlockNode>& thenBlock() const;
    [[nodiscard]] const vector<p<ExprElIfNode>>& elifs() const;
    [[nodiscard]] const p<StatementBlockNode>& elseBlock() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprGetNode : public ExprNode {
    p<ExprNode> _arrayExpr;
    vector<p<ExprNode>> _indices;

public:
    ExprGetNode(const p<Node>& parent, p<ExprNode> arrayExpr, vector<p<ExprNode>> indices) :
        ExprNode(parent),
        _arrayExpr(arrayExpr),
        _indices(std::move(indices)) {
    }

    [[nodiscard]] const p<ExprNode>& arrayExpr() const;
    [[nodiscard]] const vector<p<ExprNode>>& indices() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprArrayNode : public ExprNode {
    vector<p<ExprNode>> _elements;

public:
    ExprArrayNode(const p<Node>& parent, vector<p<ExprNode>> elements) :
        ExprNode(parent),
        _elements(std::move(elements)) {
    }

    [[nodiscard]] const vector<p<ExprNode>>& elements() const;
    [[nodiscard]] TypeInfo getType() const override;
};

#endif //YUX_LANG_EXPR_NODE_H

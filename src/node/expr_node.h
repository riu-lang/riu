// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_EXPR_NODE_H
#define YUX_LANG_EXPR_NODE_H

#include "node.h"

#include <utility>
#include <vector>

#include "literal_node.h"

class StatementNode;
class TypeNode;

class ExprNode : public Node, public Typed {
public:
    ExprNode(const p<Node>& parent) : Node(parent) {
    }
};

// 如果表达式是无后缀的整数字面量且其类型可以推断，则返回 true。
// "灵活"子树只包含无类型后缀的整数字面量、括号、一元运算符和二元运算符。
bool isFlexibleIntExpr(p<ExprNode> expr);

// 尝试将子树中所有无类型后缀的整数字面量的类型设置为 `target`。
// 如果成功则返回 true（子树与 target 兼容——既可以是灵活类型并被传播，
// 也可以是其类型已经等于 target）。
bool tryInferIntType(p<ExprNode> expr, const TypeInfo& target);

inline bool isIntTypeName(const string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
           n == "u8" || n == "u16" || n == "u32" || n == "u64";
}

class ExprCallNode : public ExprNode {
protected:
    p<ExprNode> _calleeExpr;
    vector<p<ExprNode>> _args;
    vector<p<TypeNode>> _typeArgs;

public:
    ExprCallNode(const p<Node>& parent, p<ExprNode> callee) :
        ExprNode(parent),
        _calleeExpr(callee) {
    }

    void addArg(p<ExprNode> arg) {
        _args.push_back(arg);
    }

    void setTypeArgs(vector<p<TypeNode>> args) { _typeArgs = std::move(args); }
    [[nodiscard]] const vector<p<TypeNode>>& getTypeArgs() const { return _typeArgs; }

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
        _line = literal->getLineNumber();
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
    [[nodiscard]] int resolveLineNumber() const override;
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
    [[nodiscard]] int resolveLineNumber() const override;
};

class ExprBinOpNode : public ExprNode {
public:
    enum class Op { And, Or, Xor, Shl, Shr };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprBinOpNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right) :
        ExprNode(parent),
        _op(op), _left(left),
        _right(right) {
    }

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
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
    [[nodiscard]] int resolveLineNumber() const override;

    // 解析形如 `aliasLit.s1.s2...sN` 的 Dot 链。
    // 成功时：aliasName 置为根字面量；segments 按顺序存放 [s1, ..., sN]（不含 alias）。
    // 失败返回 false（链底不是字面量对象）。
    static bool parseChain(const ExprDotNode* top, string& aliasName, vector<string>& segments);
};

class ExprCompareNode : public ExprNode {
public:
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge, AndAnd, OrOr };

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
    [[nodiscard]] int resolveLineNumber() const override;
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
    [[nodiscard]] int resolveLineNumber() const override;
};

class ExprOneLineIfElseNode : public ExprNode {
    p<ExprNode> _condition;
    p<ExprNode> _trueValue;
    p<ExprNode> _falseValue;

public:
    ExprOneLineIfElseNode(const p<Node>& parent, p<ExprNode> condition, p<ExprNode> trueValue, p<ExprNode> falseValue) :
        ExprNode(parent),
        _condition(condition),
        _trueValue(trueValue),
        _falseValue(falseValue) {
    }

    [[nodiscard]] const p<ExprNode>& condition() const { return _condition; }
    [[nodiscard]] const p<ExprNode>& trueValue() const { return _trueValue; }
    [[nodiscard]] const p<ExprNode>& falseValue() const { return _falseValue; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
};

class ExprIfElsePreValueNode : public ExprNode {
    p<ExprNode> _condition;
    p<ExprNode> _trueValue;
    p<ExprNode> _falseValue;

public:
    ExprIfElsePreValueNode(const p<Node>& parent, p<ExprNode> condition, p<ExprNode> trueValue, p<ExprNode> falseValue) :
        ExprNode(parent),
        _condition(condition),
        _trueValue(trueValue),
        _falseValue(falseValue) {
    }

    [[nodiscard]] const p<ExprNode>& condition() const { return _condition; }
    [[nodiscard]] const p<ExprNode>& trueValue() const { return _trueValue; }
    [[nodiscard]] const p<ExprNode>& falseValue() const { return _falseValue; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
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
    [[nodiscard]] int resolveLineNumber() const override;
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
    [[nodiscard]] int resolveLineNumber() const override;
};

class ExprArrayInitNode : public ExprNode {
    p<LiteralNode> _value;
    p<TypeNode> _explicitType;

public:
    ExprArrayInitNode(const p<Node>& parent, p<LiteralNode> value, p<TypeNode> explicitType) :
        ExprNode(parent),
        _value(value),
        _explicitType(explicitType) {
    }

    [[nodiscard]] const p<LiteralNode>& value() const;
    [[nodiscard]] const p<TypeNode>& explicitType() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprGetRefNode : public ExprNode {
    Token _obj;
    vector<Token> _subs;

public:
    ExprGetRefNode(const p<Node>& parent, Token obj, vector<Token> subs) :
        ExprNode(parent),
        _obj(obj),
        _subs(std::move(subs)) {
    }

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
};

class ExprUnaryNode : public ExprNode {
public:
    enum class Op { Neg, Rev, Not };

protected:
    Op _op;
    p<ExprNode> _right;

public:
    ExprUnaryNode(const p<Node>& parent, Op op, p<ExprNode> right) :
        ExprNode(parent),
        _op(op),
        _right(right) {
    }

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
};

#endif //YUX_LANG_EXPR_NODE_H

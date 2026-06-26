// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_STATEMENT_NODE_H
#define YUX_LANG_STATEMENT_NODE_H

#include <utility>

#include "expr_node.h"
#include "type_node.h"

class StatementNode : public Node {
public:
    explicit StatementNode(const p<Node>& parent) : Node(parent) {}
};

class StatementExprNode : public StatementNode {
protected:
    p<ExprNode> _expr;
    bool _hasSemicolon;

public:
    explicit StatementExprNode(const p<Node>& parent, p<ExprNode> expr, bool hasSemicolon = true)
        : StatementNode(parent), _expr(expr), _hasSemicolon(hasSemicolon) {}

    [[nodiscard]] const p<ExprNode>& expr() const;
    [[nodiscard]] bool hasSemicolon() const;
};

class StatementRetNode : public StatementExprNode {
public:
    explicit StatementRetNode(const p<Node>& parent, p<ExprNode> expr) : StatementExprNode(parent, expr) {}
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
    explicit StatementDeclareNode(const p<Node>& parent, bool isMut, bool isConst, Token name, p<TypeNode> type)
        : StatementNode(parent), _isMut(isMut), _isConst(isConst), _name(std::move(name)), _type(type) {}

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
    explicit StatementDeclareAssignNode(const p<Node>& parent, bool isMut, bool isConst, Token name, p<TypeNode> type,
                                        p<ExprNode> expr)
        : StatementExprNode(parent, expr), _isMut(isMut), _isConst(isConst), _name(std::move(name)), _type(type) {}

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
    explicit StatementDeclareAssignTupleNode(const p<Node>& parent, bool isMut, bool isConst, vector<Token> names,
                                             p<TypeNode> type, p<ExprNode> expr)
        : StatementExprNode(parent, expr), _isMut(isMut), _isConst(isConst), _names(std::move(names)), _type(type) {}

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    [[nodiscard]] const vector<Token>& names() const { return _names; }
    [[nodiscard]] p<TypeNode> varType() const { return _type; }
};

enum class AssignOp : u8 { Eq, AddEq, SubEq, MulEq, DivEq, ModEq, MtMtEq, LtLtEq };

class StatementAssignNode : public StatementExprNode {
protected:
    Token _obj;
    vector<Token> _subs;
    AssignOp _op;

public:
    explicit StatementAssignNode(const p<Node>& parent, Token obj, vector<Token> subs, p<ExprNode> expr,
                                 AssignOp op = AssignOp::Eq)
        : StatementExprNode(parent, expr), _obj(std::move(obj)), _subs(std::move(subs)), _op(op) {}

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
    [[nodiscard]] AssignOp op() const { return _op; }
};

class StatementBlockNode;

class StatementLoopNode : public StatementNode {
protected:
    p<StatementBlockNode> _block;
    Token _label; // label 名称；空 Token = 无 label（label: loop { }）
    // loop init 子句（可选）：loop name = expr { } / loop (a, b) = expr { }
    vector<Token> _initNames; // 变量名；空 = 无 init
    p<TypeNode> _initType;    // 可选类型标注；nullptr = 从 expr 推断
    p<ExprNode> _initExpr;    // init 表达式；nullptr = 无 init

public:
    explicit StatementLoopNode(const p<Node>& parent, p<StatementBlockNode> block, Token label = {},
                               vector<Token> initNames = {}, p<TypeNode> initType = nullptr,
                               p<ExprNode> initExpr = nullptr);

    [[nodiscard]] const p<StatementBlockNode>& block() const;
    [[nodiscard]] const Token& label() const { return _label; }
    [[nodiscard]] const vector<Token>& initNames() const { return _initNames; }
    [[nodiscard]] const p<TypeNode>& initType() const { return _initType; }
    [[nodiscard]] const p<ExprNode>& initExpr() const { return _initExpr; }
    [[nodiscard]] bool hasInit() const { return !_initNames.empty(); }
};

class StatementBreakNode : public StatementNode {
protected:
    Token _label; // label 名称；空 Token = 无 label（break@label;）

public:
    explicit StatementBreakNode(const p<Node>& parent, Token label = {})
        : StatementNode(parent), _label(std::move(label)) {}
    [[nodiscard]] const Token& label() const { return _label; }
};

// DRAFT-static-vars Phase 5：静态字段写语句（Type::FIELD = expr）
// 语法形态 `TypeName::fieldName = value`，仅当字段为 #Mut 时合法；
// 非 #Mut 写由 codegen 抛 E3151。
class StatementStaticFieldSetNode : public StatementNode {
protected:
    Token _typeName;
    Token _fieldName;
    p<ExprNode> _valueExpr;

public:
    StatementStaticFieldSetNode(const p<Node>& parent, Token typeName, Token fieldName, p<ExprNode> valueExpr)
        : StatementNode(parent), _typeName(std::move(typeName)), _fieldName(std::move(fieldName)),
          _valueExpr(valueExpr) {}

    [[nodiscard]] Token typeName() const { return _typeName; }
    [[nodiscard]] Token fieldName() const { return _fieldName; }
    [[nodiscard]] const p<ExprNode>& valueExpr() const { return _valueExpr; }
};

class StatementSetNode : public StatementNode {
    p<ExprNode> _arrayExpr;
    vector<p<ExprNode>> _indices;
    p<ExprNode> _valueExpr;

public:
    StatementSetNode(const p<Node>& parent, p<ExprNode> arrayExpr, vector<p<ExprNode>> indices, p<ExprNode> valueExpr)
        : StatementNode(parent), _arrayExpr(arrayExpr), _indices(std::move(indices)), _valueExpr(valueExpr) {}

    [[nodiscard]] const p<ExprNode>& arrayExpr() const;
    [[nodiscard]] const vector<p<ExprNode>>& indices() const;
    [[nodiscard]] const p<ExprNode>& valueExpr() const;
};

#endif // YUX_LANG_STATEMENT_NODE_H

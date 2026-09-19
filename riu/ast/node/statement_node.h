// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_STATEMENT_NODE_H
#define RIU_LANG_STATEMENT_NODE_H

#include <utility>

#include "expr_node.h"
#include "type_node.h"

class StatementNode : public Node {
public:
    explicit StatementNode(Node* parent) : Node(parent) {}
    virtual void accept(AstVisitor& v) = 0;
};

class StatementExprNode : public StatementNode {
protected:
    ExprNode* _expr;
    bool _hasSemicolon;

public:
    explicit StatementExprNode(Node* parent, ExprNode* expr, bool hasSemicolon = true)
        : StatementNode(parent), _expr(expr), _hasSemicolon(hasSemicolon) {}

    [[nodiscard]] ExprNode* expr() const;
    [[nodiscard]] bool hasSemicolon() const;
    void accept(AstVisitor& v) override;
};

class StatementRetNode : public StatementExprNode {
public:
    explicit StatementRetNode(Node* parent, ExprNode* expr) : StatementExprNode(parent, expr) {}
    void accept(AstVisitor& v) override;
};

class StatementRetVoidNode : public StatementNode {
public:
    explicit StatementRetVoidNode(Node* parent) : StatementNode(parent) {}
    void accept(AstVisitor& v) override;
};

// DRAFT-let-unify §3：`let x` 默认浅不可变（isMut=false, isConst=false）；
// `#Mut let x` → isMut=true；`#Cval let x` → isConst=true（且不可重赋）。
class StatementDeclareNode : public StatementNode {
protected:
    bool _isMut;
    bool _isConst;
    bool _isFrozen = false;
    Token _name;
    TypeNode* _type;

public:
    explicit StatementDeclareNode(Node* parent, bool isMut, bool isConst, Token name, TypeNode* type)
        : StatementNode(parent), _isMut(isMut), _isConst(isConst), _name(std::move(name)), _type(type) {}

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    void setFrozen(bool v) { _isFrozen = v; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
    [[nodiscard]] const Token& name() const;
    [[nodiscard]] TypeNode* varType() const;
    void accept(AstVisitor& v) override;
};

class StatementDeclareAssignNode : public StatementExprNode {
protected:
    bool _isMut;
    bool _isConst;
    bool _isFrozen = false;
    Token _name;
    TypeNode* _type;

public:
    explicit StatementDeclareAssignNode(Node* parent, bool isMut, bool isConst, Token name, TypeNode* type,
                                        ExprNode* expr)
        : StatementExprNode(parent, expr), _isMut(isMut), _isConst(isConst), _name(std::move(name)), _type(type) {}

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    void setFrozen(bool v) { _isFrozen = v; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
    [[nodiscard]] const Token& name() const;
    [[nodiscard]] TypeNode* varType() const;
    void accept(AstVisitor& v) override;
};

// 元组解构赋值声明：let (a, b, ...) = expr（默认浅不可变；`#Mut let (...)` → isMut=true）
// 平铺一层 ID，不支持嵌套与 _；可选总类型标注 typeWithRef，指代整个元组类型
class StatementDeclareAssignTupleNode : public StatementExprNode {
protected:
    bool _isMut;
    bool _isConst;
    bool _isFrozen = false;
    vector<Token> _names;
    TypeNode* _type; // 可选，整体元组类型（含 typeWithRef）

public:
    explicit StatementDeclareAssignTupleNode(Node* parent, bool isMut, bool isConst, vector<Token> names,
                                             TypeNode* type, ExprNode* expr)
        : StatementExprNode(parent, expr), _isMut(isMut), _isConst(isConst), _names(std::move(names)), _type(type) {}

    [[nodiscard]] bool isMut() const { return _isMut; }
    [[nodiscard]] bool isConst() const { return _isConst; }
    void setFrozen(bool v) { _isFrozen = v; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
    [[nodiscard]] const vector<Token>& names() const { return _names; }
    [[nodiscard]] TypeNode* varType() const { return _type; }
    void accept(AstVisitor& v) override;
};

enum class AssignOp : u8 { Eq, AddEq, SubEq, MulEq, DivEq, ModEq };

class StatementAssignNode : public StatementExprNode {
protected:
    Token _obj;
    vector<Token> _subs;
    AssignOp _op;

public:
    explicit StatementAssignNode(Node* parent, Token obj, vector<Token> subs, ExprNode* expr,
                                 AssignOp op = AssignOp::Eq)
        : StatementExprNode(parent, expr), _obj(std::move(obj)), _subs(std::move(subs)), _op(op) {}

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
    [[nodiscard]] AssignOp op() const { return _op; }
    void accept(AstVisitor& v) override;
};

class StatementBlockNode;

class StatementLoopNode : public StatementNode {
protected:
    StatementBlockNode* _block;
    Token _label; // label 名称；空 Token = 无 label（label: loop { }）
    // loop init 子句（可选）：loop name = expr { } / loop (a, b) = expr { }
    vector<Token> _initNames; // 变量名；空 = 无 init
    TypeNode* _initType;      // 可选类型标注；nullptr = 从 expr 推断
    ExprNode* _initExpr;      // init 表达式；nullptr = 无 init

public:
    explicit StatementLoopNode(Node* parent, StatementBlockNode* block, Token label = {}, vector<Token> initNames = {},
                               TypeNode* initType = nullptr, ExprNode* initExpr = nullptr);

    [[nodiscard]] StatementBlockNode* block() const;
    [[nodiscard]] const Token& label() const { return _label; }
    [[nodiscard]] const vector<Token>& initNames() const { return _initNames; }
    [[nodiscard]] TypeNode* initType() const { return _initType; }
    [[nodiscard]] ExprNode* initExpr() const { return _initExpr; }
    [[nodiscard]] bool hasInit() const { return !_initNames.empty(); }
    void accept(AstVisitor& v) override;
};

class StatementBreakNode : public StatementNode {
protected:
    Token _label; // label 名称；空 Token = 无 label（break@label;）

public:
    explicit StatementBreakNode(Node* parent, Token label = {}) : StatementNode(parent), _label(std::move(label)) {}
    [[nodiscard]] const Token& label() const { return _label; }
    void accept(AstVisitor& v) override;
};

class StatementContinueNode : public StatementNode {
protected:
    Token _label; // 空 Token = 无 label（continue@label;）

public:
    explicit StatementContinueNode(Node* parent, Token label = {}) : StatementNode(parent), _label(std::move(label)) {}
    [[nodiscard]] const Token& label() const { return _label; }
    void accept(AstVisitor& v) override;
};

// `for item in expr { }`：Array / Indexed 时 item 为 T&；Iter 时 item 为 U 值。
// expr 须为 Array<T> / [T*N] / Indexed / Iter（可 peelRef）
class StatementForInNode : public StatementNode {
protected:
    StatementBlockNode* _block;
    Token _label;
    Token _item;
    ExprNode* _expr;

public:
    explicit StatementForInNode(Node* parent, StatementBlockNode* block, Token item, ExprNode* expr, Token label = {});

    [[nodiscard]] StatementBlockNode* block() const;
    [[nodiscard]] const Token& label() const { return _label; }
    [[nodiscard]] const Token& item() const { return _item; }
    [[nodiscard]] ExprNode* expr() const { return _expr; }
    void accept(AstVisitor& v) override;
};

// DRAFT-static-vars Phase 5：静态字段写语句（Type::FIELD = expr）
// 语法形态 `TypeName::fieldName = value`，仅当字段为 #Mut 时合法；
// 非 #Mut 写由 codegen 抛 E3151。
class StatementStaticFieldSetNode : public StatementNode {
protected:
    TypePath _typePath;
    Token _fieldName;
    ExprNode* _valueExpr;

public:
    StatementStaticFieldSetNode(Node* parent, Token typeName, Token fieldName, ExprNode* valueExpr)
        : StatementStaticFieldSetNode(parent, TypePath(std::move(typeName)), std::move(fieldName), valueExpr) {}
    StatementStaticFieldSetNode(Node* parent, TypePath typePath, Token fieldName, ExprNode* valueExpr)
        : StatementNode(parent), _typePath(std::move(typePath)), _fieldName(std::move(fieldName)),
          _valueExpr(valueExpr) {}

    [[nodiscard]] Token typeName() const { return _typePath.last(); }
    [[nodiscard]] const TypePath& typePath() const { return _typePath; }
    [[nodiscard]] Token fieldName() const { return _fieldName; }
    [[nodiscard]] ExprNode* valueExpr() const { return _valueExpr; }
    void accept(AstVisitor& v) override;
};

class StatementSetNode : public StatementNode {
    ExprNode* _arrayExpr;
    vector<ExprNode*> _indices;
    ExprNode* _valueExpr;

public:
    StatementSetNode(Node* parent, ExprNode* arrayExpr, vector<ExprNode*> indices, ExprNode* valueExpr)
        : StatementNode(parent), _arrayExpr(arrayExpr), _indices(std::move(indices)), _valueExpr(valueExpr) {}

    [[nodiscard]] ExprNode* arrayExpr() const;
    [[nodiscard]] const vector<ExprNode*>& indices() const;
    [[nodiscard]] ExprNode* valueExpr() const;
    void accept(AstVisitor& v) override;
};

#endif // RIU_LANG_STATEMENT_NODE_H

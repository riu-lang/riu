// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_FN_NODE_H
#define RIU_LANG_FN_NODE_H

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
    TypeNode* _type;
    // P1-3 const-mut §5：#Frozen 形参——体内不可重赋 / 不可写字段 / 不可传给可写形参；
    // 仅 copy_of 可作为脱 const 出口。
    bool _isFrozen = false;

public:
    explicit FnParamNode(Node* parent, Token name, TypeNode* type)
        : Node(parent), _name(std::move(name)), _type(type) {}

    [[nodiscard]] Token name() const;
    [[nodiscard]] TypeNode* type() const;

    void setFrozen(bool v) { _isFrozen = v; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
};

class FnHeaderNode : public Node, public Named, public Typed, public Annotated {
protected:
    vector<FnParamNode*> _params;
    vector<string> _typeParams;
    // 与 _typeParams 等长；每个槽位的 draft 边界名（如 ["ToString", "Eq"]）。
    // 空 vector 表示该类型形参无 bound。spec §12 / §6.4.4。
    vector<vector<string>> _typeParamBounds;
    TypeNode* _retType;
    TypeNode* _fallibleErrType;

public:
    FnHeaderNode(Node* parent, Token name, TypeNode* retType)
        : Node(parent), Named(std::move(name)), _retType(retType), _fallibleErrType(nullptr) {}

    void setFallibleErrType(TypeNode* errType) { _fallibleErrType = std::move(errType); }
    [[nodiscard]] TypeNode* fallibleErrTypeNode() const { return _fallibleErrType; }
    [[nodiscard]] string resolvedFallibleErr() const;

    void addParam(FnParamNode* param);

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    void setTypeParamBounds(vector<vector<string>> bounds) { _typeParamBounds = std::move(bounds); }
    [[nodiscard]] const vector<vector<string>>& typeParamBounds() const { return _typeParamBounds; }

    [[nodiscard]] Token name() const override;
    [[nodiscard]] TypeNode* retType() const;
    [[nodiscard]] vector<FnParamNode*> params() const;
    [[nodiscard]] TypeInfo getType() const override;

    // 构造模型重构：`#Static fn` 是关联函数（无 receiver $），调用形态 `Type::name(...)`
    // 仅当出现在 structImpl 体内时有意义；放在其它位置的合法性由 sema 校验（Phase 2）
    [[nodiscard]] bool isStatic() const { return hasAnno("Static"); }
};

class FnNode : public ScopeNode, public Typed {
    FnHeaderNode* _header;
    vector<StatementNode*> _body;
    string _sourceText; // 顶层 fn 的 ctx->getText()，供 .ud skeleton

public:
    explicit FnNode(Node* parent, FnHeaderNode* header);

    void addStatement(StatementNode* stmt);

    [[nodiscard]] const vector<StatementNode*>& body() const;
    [[nodiscard]] FnHeaderNode* header() const;

    void setSourceText(string s) { _sourceText = std::move(s); }
    [[nodiscard]] const string& sourceText() const { return _sourceText; }

    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] string getLocation() const override;
};

#endif // RIU_LANG_FN_NODE_H

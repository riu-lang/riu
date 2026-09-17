// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_ALIAS_NODE_H
#define RIU_LANG_ALIAS_NODE_H

#include "statement_node.h"

#include "type_node.h"
#include <utility>

// 透明类型别名 `type Name = T`
// 别名是编译期等价（非 newtype）；解析时透明替换为目标类型。
// 可出现在文件顶层、statementBlock、struct 字段段。左侧无 genericDef。
class AliasDeclNode : public StatementNode, public Named {
    vector<string> _typeParams;
    TypeNode* _target;

public:
    AliasDeclNode(Node* parent, Token name, TypeNode* target)
        : StatementNode(parent), Named(std::move(name)), _target(target) {}

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    void setTarget(TypeNode* target) { _target = target; }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }
    [[nodiscard]] TypeNode* target() const { return _target; }
    void accept(AstVisitor& v) override;
};

#endif // RIU_LANG_ALIAS_NODE_H

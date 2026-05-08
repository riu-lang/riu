// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_ALIAS_NODE_H
#define YUX_LANG_ALIAS_NODE_H

#include "node.h"
#include "type_node.h"

// 顶层透明类型别名 `A = T` / `Pair<T> = (T, T)`
// 别名是编译期等价（非 newtype）；解析时透明替换为目标类型
class AliasDeclNode : public ScopeNode, public Named {
    vector<string> _typeParams;
    p<TypeNode> _target;

public:
    AliasDeclNode(const p<Node>& parent, Token name, p<TypeNode> target) :
        ScopeNode(parent), Named(name), _target(target) {
    }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    void setTarget(p<TypeNode> target) { _target = target; }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }
    [[nodiscard]] p<TypeNode> target() const { return _target; }
};

#endif //YUX_LANG_ALIAS_NODE_H

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 枚举 AST 节点
//
// 本文件定义 enum + match 子系统的声明侧 AST 节点:
// - EnumVariantNode: 单个 variant，含短名 + 可选 tuple-style payload 类型列表
// - EnumDeclNode: 整个 enum 声明，含 variant 列表
//
// 设计要点（详见 docs/spec/draft/DRAFT-枚举.md）:
// - variant 永远以 E::V 全限定形式出现，variant 名不进顶层命名空间
// - 零参 variant 是 "tuple payload 元素数为 0" 的退化形式，构造侧
//   E::V 与 E::V() 等价；AST 上零参 variant 的 _payloadTypes 为空
// - enum 是封闭的，所有 variant 在声明处一次列全
// - v1 不支持泛型 enum / 独立 impl 块 / draft 实现 / discriminant 显式赋值

#ifndef YUX_LANG_ENUM_NODE_H
#define YUX_LANG_ENUM_NODE_H

#include "node.h"
#include "type_node.h"

// 单个 variant: 短名 + 可选 tuple-style payload
// payloadTypes 为空表示零参 variant
class EnumVariantNode : public Node, public Named {
    vector<p<TypeNode>> _payloadTypes;

public:
    EnumVariantNode(const p<Node>& parent, Token name) :
        Node(parent), Named(name) {
    }

    void addPayloadType(p<TypeNode> ty) { _payloadTypes.push_back(ty); }
    void setPayloadTypes(vector<p<TypeNode>> tys) { _payloadTypes = std::move(tys); }

    [[nodiscard]] const vector<p<TypeNode>>& payloadTypes() const { return _payloadTypes; }
    [[nodiscard]] bool hasPayload() const { return !_payloadTypes.empty(); }
    [[nodiscard]] size_t payloadArity() const { return _payloadTypes.size(); }
};

// enum 声明节点
// variant 顺序即 tag 编号顺序（0..N-1），用户不可观测
class EnumDeclNode : public ScopeNode, public Named, public Annotated {
    vector<p<EnumVariantNode>> _variants;
    map<string, size_t> _variantIndices;
    bool _isPrivate;

public:
    EnumDeclNode(const p<Node>& parent, Token name) :
        ScopeNode(parent), Named(name) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    // 返回 false 表示重名 variant，调用方负责报错
    bool addVariant(p<EnumVariantNode> variant) {
        const string& vn = variant->name().getText();
        if (_variantIndices.count(vn)) return false;
        _variantIndices[vn] = _variants.size();
        _variants.push_back(variant);
        return true;
    }

    [[nodiscard]] const vector<p<EnumVariantNode>>& variants() const { return _variants; }

    [[nodiscard]] int variantIndex(const string& name) const {
        auto it = _variantIndices.find(name);
        return it != _variantIndices.end() ? static_cast<int>(it->second) : -1;
    }

    [[nodiscard]] EnumVariantNode* variant(const string& name) const {
        int idx = variantIndex(name);
        return idx >= 0 ? _variants[idx] : nullptr;
    }

    [[nodiscard]] bool isPrivate() const { return _isPrivate; }
};

#endif //YUX_LANG_ENUM_NODE_H

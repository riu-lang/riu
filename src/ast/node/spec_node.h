// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SPEC_NODE_H
#define YUX_LANG_SPEC_NODE_H

#include "fn_node.h"
#include "node.h"

// spec 声明节点（spec §12.1.1）：承载方法签名集；签名可附带可选默认体
// （DRAFT-spec-default-body）。`_defaultBodies` 与 `_signatures` 等长并按 index
// 对齐：无默认体处放 nullptr。
class SpecDeclNode : public ScopeNode, public Named, public Annotated {
    vector<p<FnHeaderNode>> _signatures;
    vector<p<FnNode>> _defaultBodies;
    vector<string> _typeParams;
    bool _isPrivate;

public:
    SpecDeclNode(const p<Node>& parent, const Token& name) : ScopeNode(parent), Named(name) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    void addSignature(p<FnHeaderNode> sig, p<FnNode> defaultBody = nullptr) {
        _signatures.push_back(sig);
        _defaultBodies.push_back(defaultBody);
    }
    [[nodiscard]] const vector<p<FnHeaderNode>>& signatures() const { return _signatures; }
    [[nodiscard]] const vector<p<FnNode>>& defaultBodies() const { return _defaultBodies; }
    [[nodiscard]] p<FnNode> defaultBody(size_t idx) const {
        return idx < _defaultBodies.size() ? _defaultBodies[idx] : nullptr;
    }
    [[nodiscard]] bool hasDefaultBody(size_t idx) const {
        return idx < _defaultBodies.size() && _defaultBodies[idx] != nullptr;
    }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    [[nodiscard]] bool isPrivate() const { return _isPrivate; }
    [[nodiscard]] bool isDraftLike() const { return hasAnno("DraftLike"); }
};

// spec 引用：`#Impl(D1 + D2) struct X` 中每个 D 的解析结果（v0.5 仅按名 + 类型实参字串记录）。
struct SpecRef {
    string name;
    vector<TypeInfo> typeArgs;
    int line = 0;
    int col = 0;
};

#endif // YUX_LANG_SPEC_NODE_H

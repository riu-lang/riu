// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_DRAFT_NODE_H
#define YUX_LANG_DRAFT_NODE_H

#include "fn_node.h"
#include "node.h"

// draft 声明节点（spec §12.1.1）：仅承载方法签名集，无函数体。
class DraftDeclNode : public ScopeNode, public Named, public Annotated {
    vector<p<FnHeaderNode>> _signatures;
    vector<string> _typeParams;
    bool _isPrivate;

public:
    DraftDeclNode(const p<Node>& parent, Token name) :
        ScopeNode(parent), Named(name) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    void addSignature(p<FnHeaderNode> sig) { _signatures.push_back(sig); }
    [[nodiscard]] const vector<p<FnHeaderNode>>& signatures() const { return _signatures; }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    [[nodiscard]] bool isPrivate() const { return _isPrivate; }
    [[nodiscard]] bool isDraftLike() const { return hasAnno("DraftLike"); }
};

// draft 引用：实现块 `Type : D1 + D2` 中每个 D 的解析结果（v0.5 仅按名 + 类型实参字串记录）。
struct DraftRef {
    string name;
    vector<TypeInfo> typeArgs;
    int line = 0;
    int col = 0;
};

#endif //YUX_LANG_DRAFT_NODE_H

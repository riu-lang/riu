// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_STRUCT_NODE_H
#define YUX_LANG_STRUCT_NODE_H

#include "node.h"
#include "type_node.h"
#include "fn_node.h"
#include "spec_node.h"

class StructFieldNode : public Node {
    Token _name;
    p<TypeNode> _type;
    bool _isPrivate;
    // P1-4 DRAFT-const-mut §6.1：字段三档 (default var / #Val 浅 / #Frozen 深)。
    // 互斥；同时出现 → ast_builder 抛 E3105。
    bool _isVal = false;
    bool _isFrozen = false;

public:
    StructFieldNode(const p<Node>& parent, Token name, p<TypeNode> type) :
        Node(parent), _name(name), _type(type) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] Token name() const { return _name; }
    [[nodiscard]] p<TypeNode> type() const { return _type; }
    [[nodiscard]] TypeInfo getType() const { return _type->getType(); }
    [[nodiscard]] bool isPrivate() const { return _isPrivate; }

    void setVal(bool v) { _isVal = v; }
    void setFrozen(bool v) { _isFrozen = v; }
    [[nodiscard]] bool isVal() const { return _isVal; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
};

class StructDeclNode : public ScopeNode, public Named, public Annotated {
    vector<p<StructFieldNode>> _fields;
    map<string, size_t> _fieldIndices;
    vector<string> _typeParams;
    bool _isPrivate;

public:
    StructDeclNode(const p<Node>& parent, Token name) :
        ScopeNode(parent), Named(name) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    void addField(p<StructFieldNode> field) {
        _fieldIndices[field->name().getText()] = _fields.size();
        _fields.push_back(field);
    }

    [[nodiscard]] const vector<p<StructFieldNode>>& fields() const { return _fields; }
    [[nodiscard]] int fieldIndex(const string& name) const {
        auto it = _fieldIndices.find(name);
        return it != _fieldIndices.end() ? static_cast<int>(it->second) : -1;
    }
    [[nodiscard]] const StructFieldNode* field(const string& name) const {
        int idx = fieldIndex(name);
        return idx >= 0 ? _fields[idx] : nullptr;
    }
    [[nodiscard]] bool isPrivate() const { return _isPrivate; }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }
};

class StructImplNode : public ScopeNode, public Named, public Annotated {
    vector<p<FnNode>> _methods;
    p<FnNode> _destructor;
    vector<string> _typeParams;
    string _structName;

public:
    StructImplNode(const p<Node>& parent, Token structName) :
        ScopeNode(parent), Named(structName), _destructor(nullptr), _structName(structName.getText()) {
    }

    void addMethod(p<FnNode> method) {
        _methods.push_back(method);
    }

    void setDestructor(p<FnNode> destructor) {
        _destructor = destructor;
    }

    [[nodiscard]] const vector<p<FnNode>>& methods() const { return _methods; }
    [[nodiscard]] const p<FnNode>& destructor() const { return _destructor; }
    [[nodiscard]] bool hasDestructor() const { return _destructor != nullptr; }
    [[nodiscard]] const string& structName() const { return _structName; }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    // spec §12.2 draft 实现约束：`Type : D1 + D2 { ... }`。
    // v0.5 简化记录：只存 D 的解析名 + 类型实参（文本）+ 源位置。
    void setSpecRefs(vector<SpecRef> refs) { _specRefs = std::move(refs); }
    [[nodiscard]] const vector<SpecRef>& specRefs() const { return _specRefs; }

private:
    vector<SpecRef> _specRefs;
};

#endif //YUX_LANG_STRUCT_NODE_H

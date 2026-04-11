// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_STRUCT_NODE_H
#define YUX_LANG_STRUCT_NODE_H

#include "node.h"
#include "type_node.h"
#include "fn_node.h"

class StructFieldNode : public Node {
    Token _name;
    p<TypeNode> _type;

public:
    StructFieldNode(const p<Node>& parent, Token name, p<TypeNode> type) :
        Node(parent), _name(name), _type(type) {
    }

    [[nodiscard]] Token name() const { return _name; }
    [[nodiscard]] p<TypeNode> type() const { return _type; }
    [[nodiscard]] TypeInfo getType() const { return _type->getType(); }
};

class StructDeclNode : public ScopeNode, public Named {
    vector<p<StructFieldNode>> _fields;
    map<string, size_t> _fieldIndices;

public:
    StructDeclNode(const p<Node>& parent, Token name) :
        ScopeNode(parent), Named(name) {
    }

    void addField(p<StructFieldNode> field) {
        _fieldIndices[field->name()->getText()] = _fields.size();
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
};

class StructImplNode : public ScopeNode, public Named {
    vector<p<FnNode>> _methods;
    string _structName;

public:
    StructImplNode(const p<Node>& parent, Token structName) :
        ScopeNode(parent), Named(structName), _structName(structName->getText()) {
    }

    void addMethod(p<FnNode> method) {
        _methods.push_back(method);
    }

    [[nodiscard]] const vector<p<FnNode>>& methods() const { return _methods; }
    [[nodiscard]] const string& structName() const { return _structName; }
};

#endif //YUX_LANG_STRUCT_NODE_H

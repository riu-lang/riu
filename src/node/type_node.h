// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_NODE_H
#define YUX_LANG_TYPE_NODE_H

#include "node.h"

class TypeNode : public Node {
public:
    explicit TypeNode(const p<Node>& parent) : Node(parent) {}

    [[nodiscard]] virtual TypeInfo getType() const = 0;
    [[nodiscard]] virtual bool isArray() const { return false; }
};

class TypeNormalNode : public TypeNode {
    Token _typeName;

public:
    TypeNormalNode(const p<Node>& parent, Token typeName) :
        TypeNode(parent), _typeName(typeName) {
    }

    [[nodiscard]] TypeInfo getType() const override {
        return TypeInfo(_typeName.getText());
    }

    [[nodiscard]] Token typeNameToken() const {
        return _typeName;
    }
};

class TypeArrayNode : public TypeNode {
    p<TypeNode> _elementType;
    Token _count;

public:
    TypeArrayNode(const p<Node>& parent, p<TypeNode> elementType, Token count) :
        TypeNode(parent), _elementType(elementType), _count(count) {
    }

    [[nodiscard]] TypeInfo getType() const override {
        auto elemTypeInfo = _elementType->getType();
        auto elemShared = make_shared<TypeInfo>(elemTypeInfo);
        u64 size = stoull(_count.getText());
        return TypeInfo(elemShared, size);
    }

    [[nodiscard]] p<TypeNode> elementType() const {
        return _elementType;
    }

    [[nodiscard]] Token count() const {
        return _count;
    }

    [[nodiscard]] bool isArray() const override {
        return true;
    }
};

class TypeGenericNode : public TypeNode {
    Token _baseName;
    vector<p<TypeNode>> _typeArgs;

public:
    TypeGenericNode(const p<Node>& parent, Token baseName, vector<p<TypeNode>> typeArgs) :
        TypeNode(parent), _baseName(baseName), _typeArgs(std::move(typeArgs)) {
    }

    [[nodiscard]] TypeInfo getType() const override {
        vector<sp<TypeInfo>> args;
        for (auto& typeArg : _typeArgs) {
            args.push_back(make_shared<TypeInfo>(typeArg->getType()));
        }
        return TypeInfo(_baseName.getText(), args);
    }

    [[nodiscard]] Token baseName() const {
        return _baseName;
    }

    [[nodiscard]] const vector<p<TypeNode>>& typeArgs() const {
        return _typeArgs;
    }
};

// 元组类型节点 (T1, T2, ...)
class TypeTupleNode : public TypeNode {
    vector<p<TypeNode>> _elementTypes;

public:
    TypeTupleNode(const p<Node>& parent, vector<p<TypeNode>> elementTypes) :
        TypeNode(parent), _elementTypes(std::move(elementTypes)) {
    }

    [[nodiscard]] TypeInfo getType() const override {
        vector<sp<TypeInfo>> elems;
        elems.reserve(_elementTypes.size());
        for (auto& e : _elementTypes) {
            elems.push_back(make_shared<TypeInfo>(e->getType()));
        }
        return TypeInfo(TupleTag{}, std::move(elems));
    }

    [[nodiscard]] const vector<p<TypeNode>>& elementTypes() const {
        return _elementTypes;
    }
};

#endif //YUX_LANG_TYPE_NODE_H

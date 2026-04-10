// Copyright (c) 2026. Yin-Jinlong@github

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
        return TypeInfo(_typeName->getText());
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
        u64 size = stoull(_count->getText());
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

#endif //YUX_LANG_TYPE_NODE_H

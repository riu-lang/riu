// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#ifndef YUX_LANG_LITERAL_NODE_H
#define YUX_LANG_LITERAL_NODE_H

#include "node.h"

#include <utility>

class LiteralNode : public Node, public Typed {
protected:
    Token _value;

public:
    explicit LiteralNode(Token value);

    [[nodiscard]] Token getValue() const;

    [[nodiscard]] string getLocation() const override;
};

class LiteralNumberNode : public LiteralNode {
public:
    explicit LiteralNumberNode(Token value);
};

class LiteralIntNode : public LiteralNumberNode {
protected:
    TypeInfo _type;

public:
    explicit LiteralIntNode(Token value);
    [[nodiscard]] TypeInfo getType() const override;
};

class LiteralFloatNode : public LiteralNumberNode {
protected:
    TypeInfo _type;
public:
    explicit LiteralFloatNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
};

class LiteralBoolNode : public LiteralNode {
public:
    explicit LiteralBoolNode(Token value);
    [[nodiscard]] TypeInfo getType() const override;
};

class LiteralObjNode : public LiteralNode {
public:
    explicit LiteralObjNode(const p<Node>& parent,const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
   [[nodiscard]] string getLocation() const override;
};

class LiteralNullNode : public LiteralNode {
public:
    explicit LiteralNullNode(Token value);
    [[nodiscard]] TypeInfo getType() const override;
};

#endif //YUX_LANG_LITERAL_NODE_H

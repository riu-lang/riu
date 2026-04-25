// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

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
    bool _hasSuffix = false;

public:
    explicit LiteralIntNode(Token value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] bool hasSuffix() const { return _hasSuffix; }
    void setType(TypeInfo t) { _type = std::move(t); }
};

class LiteralFloatNode : public LiteralNumberNode {
protected:
    TypeInfo _type;
    bool _hasSuffix = false;
public:
    explicit LiteralFloatNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] bool hasSuffix() const { return _hasSuffix; }
    void setType(TypeInfo t) { _type = std::move(t); }
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

class LiteralCodePointNode : public LiteralNode {
    u32 _codePoint = 0;
public:
    explicit LiteralCodePointNode(Token value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] u32 codePoint() const { return _codePoint; }
};

class LiteralStringNode : public LiteralNode {
    vector<u32> _codePoints;
public:
    explicit LiteralStringNode(Token value, bool raw = false);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] const vector<u32>& codePoints() const { return _codePoints; }
};

#endif //YUX_LANG_LITERAL_NODE_H

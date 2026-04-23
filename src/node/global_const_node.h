// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_GLOBAL_CONST_NODE_H
#define YUX_LANG_GLOBAL_CONST_NODE_H

#include "node.h"
#include "type_node.h"
#include "literal_node.h"

class GlobalConstNode : public Node, public Named, public Typed {
    p<TypeNode> _type;
    p<LiteralNode> _value;
    bool _isPrivate;

public:
    GlobalConstNode(const p<Node>& parent, Token name, p<TypeNode> type, p<LiteralNode> value)
        : Node(parent), Named(name), _type(type), _value(value) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] p<TypeNode> typeNode() const;
    [[nodiscard]] p<LiteralNode> value() const;
    [[nodiscard]] bool isPrivate() const;

    [[nodiscard]] TypeInfo getType() const override;
};

#endif //YUX_LANG_GLOBAL_CONST_NODE_H

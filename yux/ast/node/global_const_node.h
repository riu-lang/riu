// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_GLOBAL_CONST_NODE_H
#define YUX_LANG_GLOBAL_CONST_NODE_H

#include "expr_node.h"
#include "node.h"
#include "type_node.h"

class GlobalConstNode : public Node, public Named, public Typed {
    p<TypeNode> _type;
    p<ExprNode> _value;
    bool _isPrivate;
    bool _isInline; // #Inline 注解：不产生 GlobalVariable，使用处直接替换常量值（类似 C #define）

public:
    GlobalConstNode(const p<Node>& parent, const Token& name, p<TypeNode> type, p<ExprNode> value,
                    bool isInline = false)
        : Node(parent), Named(name), _type(type), _value(value), _isInline(isInline) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] p<TypeNode> typeNode() const;
    [[nodiscard]] p<ExprNode> value() const;
    [[nodiscard]] bool isPrivate() const;
    [[nodiscard]] bool isInline() const { return _isInline; }

    [[nodiscard]] TypeInfo getType() const override;
};

#endif // YUX_LANG_GLOBAL_CONST_NODE_H

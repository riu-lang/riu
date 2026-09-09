// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_GLOBAL_CONST_NODE_H
#define YUX_LANG_GLOBAL_CONST_NODE_H

#include "expr_node.h"
#include "node.h"
#include "type_node.h"

class GlobalConstNode : public Node, public Named, public Typed {
    TypeNode* _type;
    ExprNode* _value;
    bool _isPrivate;
    bool _isInline;     // #Inline 注解：不产生 GlobalVariable，使用处直接替换常量值（类似 C #define）
    string _sourceText; // letGlobal 的 ctx->getText()，供 .decl skeleton

public:
    GlobalConstNode(Node* parent, const Token& name, TypeNode* type, ExprNode* value, bool isInline = false)
        : Node(parent), Named(name), _type(type), _value(value), _isInline(isInline) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] TypeNode* typeNode() const;
    [[nodiscard]] ExprNode* value() const;
    [[nodiscard]] bool isPrivate() const;
    [[nodiscard]] bool isInline() const { return _isInline; }

    void setSourceText(string s) { _sourceText = std::move(s); }
    [[nodiscard]] const string& sourceText() const { return _sourceText; }

    [[nodiscard]] TypeInfo getType() const override;
};

#endif // YUX_LANG_GLOBAL_CONST_NODE_H

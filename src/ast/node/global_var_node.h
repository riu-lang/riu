// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 全局变量 AST 节点（DRAFT-static-vars Phase 1）
//
// GlobalVarNode 表示运行期初始化的全局 let 变量（非 #Cval 档），
// 与 GlobalConstNode（编译期常量，#Cval 档）互补。
// 编译器为每个 GlobalVarNode 创建 LLVM GlobalVariable + 在
// _yux_global_init_<Mod>() 中 emit store 指令来运行期初始化。

#ifndef YUX_LANG_GLOBAL_VAR_NODE_H
#define YUX_LANG_GLOBAL_VAR_NODE_H

#include "expr_node.h"
#include "node.h"
#include "type_node.h"

class GlobalVarNode : public Node, public Named, public Typed {
    p<TypeNode> _type;
    p<ExprNode> _value;
    bool _isPrivate;
    bool _isMutable; // #Mut 档（Phase 2），Phase 1 始终 false

public:
    GlobalVarNode(const p<Node>& parent, Token name, p<TypeNode> type, p<ExprNode> value, bool isMutable = false)
        : Node(parent), Named(name), _type(type), _value(value), _isMutable(isMutable) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] p<TypeNode> typeNode() const { return _type; }
    [[nodiscard]] p<ExprNode> value() const { return _value; }
    [[nodiscard]] bool isPrivate() const { return _isPrivate; }
    [[nodiscard]] bool isMutable() const { return _isMutable; }

    [[nodiscard]] TypeInfo getType() const override;
};

#endif // YUX_LANG_GLOBAL_VAR_NODE_H

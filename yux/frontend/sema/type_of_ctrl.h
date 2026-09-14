// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_OF_CTRL_H
#define YUX_LANG_TYPE_OF_CTRL_H

// 1.7f：控制流族表达式的类型计算（IfElse / OneLineIfElse / Match / TryCatch / NullElse）。
// visitExpr 先下钻子树（块末 / 臂体带靶向）再写 resolved 槽；节点 getType() 有槽则读槽。
// 汇合 / 流终止跳过 / `??` 剥 Nullable 在节点 structuralType；泛型 codegen 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfIfElse(const ExprIfElseNode* n);
[[nodiscard]] TypeInfo typeOfOneLineIfElse(const ExprOneLineIfElseNode* n);
[[nodiscard]] TypeInfo typeOfMatch(const ExprMatchNode* n);
[[nodiscard]] TypeInfo typeOfTryCatch(const ExprTryCatchNode* n);
[[nodiscard]] TypeInfo typeOfNullElse(const ExprNullElseNode* n);

} // namespace sema

#endif // YUX_LANG_TYPE_OF_CTRL_H

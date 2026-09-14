// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_OF_DOT_H
#define YUX_LANG_TYPE_OF_DOT_H

// 1.7e：成员族表达式的类型计算（Dot）。
// visitExpr 先下钻基表达式再写 resolved 槽；节点 getType() 有槽则读槽。
// 方法点 E3095 仍由 Call 的 writeResolved 记下，Dot 分支校验 @Spec 后再决定重抛。
// 字段 / 方法 / `?.` / Field.value 解析在节点 structuralType；泛型 codegen 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfDot(const ExprDotNode* n);

} // namespace sema

#endif // YUX_LANG_TYPE_OF_DOT_H

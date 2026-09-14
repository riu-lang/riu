// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_OF_CALL_H
#define YUX_LANG_TYPE_OF_CALL_H

// 1.7d：调用族表达式的类型计算（Call / PathCall）。
// visitExpr 先下钻 callee / 实参再写 resolved 槽；节点 getType() 有槽则读槽。
// 重载 / 内建方法 / #Static fn 解析仍在 Sema 分支（call_resolve / NameResolver），
// 节点 structuralType 做无槽回退。泛型 codegen 用 structuralType() 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfCall(const ExprCallNode* n);
[[nodiscard]] TypeInfo typeOfPathCall(const ExprPathCallNode* n);

} // namespace sema

#endif // YUX_LANG_TYPE_OF_CALL_H

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_OF_STRUCT_H
#define YUX_LANG_TYPE_OF_STRUCT_H

// 1.7a：结构叶表达式的类型计算（Paren / Tuple / Array / 字面量）。
// visitExpr 先下钻子节点再写 resolved 槽；节点 getType() 有槽则读槽。
// ArrayInit 无靶向时仍走 SemaPass::checkArrayInit（E3009 / E3080）。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfLiteral(const ExprLiteralNode* n);
[[nodiscard]] TypeInfo typeOfParen(const ExprParenNode* n);
[[nodiscard]] TypeInfo typeOfTuple(const ExprTupleNode* n);
[[nodiscard]] TypeInfo typeOfArray(const ExprArrayNode* n);

} // namespace sema

#endif // YUX_LANG_TYPE_OF_STRUCT_H

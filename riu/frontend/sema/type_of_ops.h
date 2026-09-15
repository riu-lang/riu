// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_TYPE_OF_OPS_H
#define RIU_LANG_TYPE_OF_OPS_H

// 1.7c：运算族表达式的类型计算（Unary / AddSub / MulDiv / BinOp / Compare）。
// visitExpr 先下钻操作数再写 resolved 槽；节点 getType() 有槽则读槽。
// 运算符方法解析仍在 Sema tryValidate*（call_resolve），不在节点上。
// 泛型 codegen 用 structuralType() 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfUnary(const ExprUnaryNode* n);
[[nodiscard]] TypeInfo typeOfAddSub(const ExprAddSubNode* n);
[[nodiscard]] TypeInfo typeOfMulDiv(const ExprMulDivModNode* n);
[[nodiscard]] TypeInfo typeOfBinOp(const ExprBinOpNode* n);
[[nodiscard]] TypeInfo typeOfCompare(const ExprCompareNode* n);

} // namespace sema

#endif // RIU_LANG_TYPE_OF_OPS_H

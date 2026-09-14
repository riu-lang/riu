// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_OF_REST_H
#define YUX_LANG_TYPE_OF_REST_H

// 1.7g：其余表达式的类型计算（Lambda / StructLit / DynCtor / MoveAssign）。
// visitExpr 先下钻子树再写 resolved 槽；节点 getType() 有槽则读槽。
// Lambda 形参回填后的 Fn、Self/TypeName 字面量、Dyn<D> 包装、`<-` 取左侧类型
// 在节点 structuralType；泛型 codegen 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfLambda(const LambdaExprNode* n);
[[nodiscard]] TypeInfo typeOfStructLit(const ExprStructLitNode* n);
[[nodiscard]] TypeInfo typeOfDynCtor(const ExprDynCtorNode* n);
[[nodiscard]] TypeInfo typeOfMoveAssign(const ExprMoveAssignNode* n);

} // namespace sema

#endif // YUX_LANG_TYPE_OF_REST_H

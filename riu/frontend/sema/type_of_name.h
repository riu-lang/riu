// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_TYPE_OF_NAME_H
#define RIU_LANG_TYPE_OF_NAME_H

// 1.7b：名字族表达式的类型计算（Get 索引 / GetRef 取址）。
// Get：visitExpr 先下钻子节点再写 resolved 槽；GetRef 无子表达式，分支内写槽。
// 节点 getType() 有槽则读槽。泛型 codegen 用 structuralType() 无槽重算。

#include "ast/node/expr_node.h"

namespace sema {

[[nodiscard]] TypeInfo typeOfGet(const ExprGetNode* n);
[[nodiscard]] TypeInfo typeOfGetRef(const ExprGetRefNode* n);

} // namespace sema

#endif // RIU_LANG_TYPE_OF_NAME_H

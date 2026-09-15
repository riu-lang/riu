// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7e 成员族：节点 structuralType 做字段 / 方法 / `?.` / 元组 .N / Field.value。
// 方法点 callee 的 E3095 仍走 Call 的 writeResolved（getType 回落成基类型再抛）。

#include "sema/type_of_dot.h"

namespace sema {

TypeInfo typeOfDot(const ExprDotNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

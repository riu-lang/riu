// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7b 名字族：Get 无 call_resolve；GetRef 作用域查找在节点 structuralType。
// 计算即节点 structuralType（子节点槽 / 符号表）。

#include "sema/type_of_name.h"

namespace sema {

TypeInfo typeOfGet(const ExprGetNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfGetRef(const ExprGetRefNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

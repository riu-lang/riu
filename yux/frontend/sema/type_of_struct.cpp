// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7a 结构叶：无 call_resolve。计算即节点 structuralType（子节点槽 / 字面量）。

#include "sema/type_of_struct.h"

namespace sema {

TypeInfo typeOfLiteral(const ExprLiteralNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfParen(const ExprParenNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfTuple(const ExprTupleNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfArray(const ExprArrayNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

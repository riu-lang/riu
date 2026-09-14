// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7c 运算族：节点 structuralType 做类型比较 / 一元形态（E3001 / E3070 / E3071）。
// 自定义类型运算符方法走 Sema tryValidate* → call_resolve，不在此解析。

#include "sema/type_of_ops.h"

namespace sema {

TypeInfo typeOfUnary(const ExprUnaryNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfAddSub(const ExprAddSubNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfMulDiv(const ExprMulDivModNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfBinOp(const ExprBinOpNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfCompare(const ExprCompareNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

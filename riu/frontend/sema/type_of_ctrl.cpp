// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7f 控制流族：节点 structuralType 做汇合 / 流终止跳过 / `??` 剥 Nullable。
// E3005 / E7010 / E3024 仍由 Sema tryValidate* / 分支校验抛，不在此解析调用。

#include "sema/type_of_ctrl.h"

namespace sema {

TypeInfo typeOfIfElse(const ExprIfElseNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfOneLineIfElse(const ExprOneLineIfElseNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfMatch(const ExprMatchNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfTryCatch(const ExprTryCatchNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfNullElse(const ExprNullElseNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

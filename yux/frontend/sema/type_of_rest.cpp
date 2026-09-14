// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7g 其余族：节点 structuralType 做 Fn 字面量 / struct 字面量 / Dyn 包装 / `<-` 左侧。
// TypeName{...} 路径解析仍在节点 structuralType（resolveExprTypeLhs）；Sema 分支做
// E3124/E3125 等形态校验。

#include "sema/type_of_rest.h"

namespace sema {

TypeInfo typeOfLambda(const LambdaExprNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfStructLit(const ExprStructLitNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfDynCtor(const ExprDynCtorNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfMoveAssign(const ExprMoveAssignNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

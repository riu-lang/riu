// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 1.7d 调用族：节点 structuralType 做返回类型回退（Fn 返回 / 重载 / PathCall LHS）。
// 调用形态校验仍走 Sema 分支（err-propagate / 重载 / 内建方法 / #Static fn）。

#include "sema/type_of_call.h"

namespace sema {

TypeInfo typeOfCall(const ExprCallNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

TypeInfo typeOfPathCall(const ExprPathCallNode* n) {
    return n ? n->structuralType() : TypeInfo();
}

} // namespace sema

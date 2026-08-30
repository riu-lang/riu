// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 所有权三个入口：storeIntoSlot / passAsArg / returnValue
//
// 复制语义协议（takeOwnership）：
// - fresh 表达式（调用 / 字面量 / move / lambda / struct lit）：已持 +1，consumeTemp
// - 否则且 typeNeedsDestructor：retainHandleAtCallSite
// - 无源表达式（捕获拷贝等）：按非 fresh，需要析构则 retain
//
// 特例不走这三入口：Rc-from-T 构造（只转移 payload）、Weak-from-Rc（weak++）、
// Heap 可空 move-out、Array.clone / copy_of 深拷循环、match scrut。

#include "compiler.h"

void Compiler::takeOwnership(llvm::Value* val, const TypeInfo& type, p<ExprNode> expr) {
    if (!val) return;
    if (expr && isFreshHandleExpr(expr)) {
        consumeTemp(val);
        return;
    }
    if (typeNeedsDestructor(type)) {
        retainHandleAtCallSite(val, type);
    }
}

void Compiler::storeIntoSlot(llvm::Value* slotPtr, llvm::Value* val, const TypeInfo& type, p<ExprNode> expr,
                             SlotStore kind) {
    takeOwnership(val, type, expr);
    if (kind == SlotStore::Replace && typeNeedsDestructor(type)) {
        releaseAtPtr(slotPtr, type);
    }
    _builder.CreateStore(val, slotPtr);
}

void Compiler::passAsArg(llvm::Value* val, const TypeInfo& type, p<ExprNode> expr) {
    takeOwnership(val, type, expr);
}

bool Compiler::returnValue(llvm::Value* val, const TypeInfo& type, p<ExprNode> expr, bool retainHandle) {
    if (!val || !expr) return false;
    if (retainHandle) {
        takeOwnership(val, type, expr);
        return true;
    }
    if (typeNeedsDestructor(type) && isFreshHandleExpr(expr)) {
        consumeTemp(val);
    }
    return false;
}

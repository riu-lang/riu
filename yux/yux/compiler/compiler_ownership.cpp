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

#include "ast/node/literal_node.h"
#include "compiler.h"

void Compiler::takeOwnership(llvm::Value* val, const TypeInfo& type, ExprNode* expr) {
    if (!val) return;
    if (expr && isFreshHandleExpr(expr)) {
        consumeTemp(val);
        return;
    }
    if (typeNeedsDestructor(type)) {
        retainHandleAtCallSite(val, type);
    }
}

void Compiler::storeIntoSlot(llvm::Value* slotPtr, llvm::Value* val, const TypeInfo& type, ExprNode* expr,
                             SlotStore kind) {
    takeOwnership(val, type, expr);
    if (kind == SlotStore::Replace && typeNeedsDestructor(type)) {
        releaseAtPtr(slotPtr, type);
    }
    _builder.CreateStore(val, slotPtr);
}

void Compiler::passAsArg(llvm::Value* val, const TypeInfo& type, ExprNode* expr) {
    takeOwnership(val, type, expr);
}

llvm::Value* Compiler::pointerForRefParam(ExprNode* argExpr, llvm::Value* compiledVal) {
    // 标识符（含已是 T& 的形参 / 局部）：_localVarPtrs 就是底层 T 的地址。
    // 若走 compileExpr 的自解值再当 T& 传，callee 会把 struct 位型当成指针（成功路径 AV）。
    if (argExpr) {
        if (auto* lit = dynamic_cast<ExprLiteralNode*>(argExpr)) {
            if (auto* obj = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                auto it = _localVarPtrs.find(obj->getValue().getText());
                if (it != _localVarPtrs.end()) return it->second;
            }
        }
    }
    if (!compiledVal) return compiledVal;
    if (compiledVal->getType()->isPointerTy()) return compiledVal;
    auto* tmp = _builder.CreateAlloca(compiledVal->getType(), nullptr, "ref_arg_tmp");
    _builder.CreateStore(compiledVal, tmp);
    return tmp;
}

llvm::Value* Compiler::abiValueForParam(ExprNode* argExpr, llvm::Value* compiledVal, const TypeInfo& formal) {
    if (formal.isRef()) return pointerForRefParam(argExpr, compiledVal);
    if (structParamUsesPointer(formal)) {
        auto* ty = getLLVMType(formal);
        auto* slot = _builder.CreateAlloca(ty, nullptr, "struct_arg_tmp");
        _builder.CreateStore(compiledVal, slot);
        return slot;
    }
    return compiledVal;
}

bool Compiler::returnValue(llvm::Value* val, const TypeInfo& type, ExprNode* expr, bool retainHandle) {
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

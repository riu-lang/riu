// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// extern fn Win64 C ABI 降级（§8.7.4.2）：
// - bool 边界 i1 ↔ i8（C _Bool）
// - C-layout struct：store size 为 1/2/4/8 时按整数进寄存器；否则实参 byval、返回 sret
// 仅 isExternal；内部 yux 函数 ABI 不动。

#include "ast/node/node.h"
#include "compiler.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

Compiler::ExternAbiSlot Compiler::externAbiSlot(const TypeInfo& t) {
    ExternAbiSlot s;
    if (t.empty()) {
        s.abiTy = _builder.getVoidTy();
        return s;
    }
    if (t.isPtr() || t.isRef()) {
        s.abiTy = llvm::PointerType::get(_context, 0);
        s.valueTy = s.abiTy;
        return s;
    }
    if (t.name == "bool") {
        s.valueTy = _builder.getInt1Ty();
        s.abiTy = _builder.getInt8Ty();
        s.boolExt = true;
        return s;
    }
    s.valueTy = getLLVMType(t);
    if (!s.valueTy) {
        s.abiTy = llvm::PointerType::get(_context, 0);
        return s;
    }
    if (isBuiltinType(t.name)) {
        s.abiTy = s.valueTy;
        return s;
    }
    const uint64_t sz = _module->getDataLayout().getTypeStoreSize(s.valueTy).getFixedValue();
    if (sz == 1 || sz == 2 || sz == 4 || sz == 8) {
        s.abiTy = llvm::Type::getIntNTy(_context, static_cast<unsigned>(sz * 8));
        s.integerAgg = true;
        return s;
    }
    s.abiTy = llvm::PointerType::get(_context, 0);
    s.indirect = true;
    return s;
}

llvm::Value* Compiler::coerceToExternArg(llvm::Value* v, const ExternAbiSlot& slot) {
    if (!v) return v;
    if (slot.boolExt) {
        if (v->getType()->isIntegerTy(1)) {
            return _builder.CreateZExt(v, _builder.getInt8Ty(), "ffi.bool.zext");
        }
        return v;
    }
    if (slot.integerAgg && slot.valueTy && slot.abiTy) {
        auto* tmp = _builder.CreateAlloca(slot.valueTy, nullptr, "ffi.agg.arg");
        _builder.CreateStore(v, tmp);
        return _builder.CreateLoad(slot.abiTy, tmp, "ffi.agg.i");
    }
    if (slot.indirect && slot.valueTy) {
        auto* tmp = _builder.CreateAlloca(slot.valueTy, nullptr, "ffi.byval.arg");
        _builder.CreateStore(v, tmp);
        return tmp;
    }
    return v;
}

llvm::Value* Compiler::coerceFromExternRet(llvm::Value* v, const ExternAbiSlot& slot, llvm::Value* sretAlloca) {
    if (slot.indirect && sretAlloca && slot.valueTy) {
        return _builder.CreateLoad(slot.valueTy, sretAlloca, "ffi.sret.load");
    }
    if (slot.boolExt && v) {
        return _builder.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0), "ffi.bool.trunc");
    }
    if (slot.integerAgg && v && slot.valueTy && slot.abiTy) {
        auto* tmp = _builder.CreateAlloca(slot.valueTy, nullptr, "ffi.agg.ret");
        _builder.CreateStore(v, tmp);
        return _builder.CreateLoad(slot.valueTy, tmp, "ffi.agg.s");
    }
    return v;
}

llvm::Function* Compiler::getOrCreateExternFunction(const string& cName, const FnSymbolInfo& fnSymbol) {
    if (auto* existed = _module->getFunction(cName)) return existed;

    ExternAbiSlot retSlot = externAbiSlot(fnSymbol.retType);
    const bool retSret = retSlot.indirect;

    vector<llvm::Type*> paramTys;
    if (retSret) paramTys.push_back(llvm::PointerType::get(_context, 0));
    vector<ExternAbiSlot> argSlots;
    argSlots.reserve(fnSymbol.params.size());
    for (auto& p : fnSymbol.params) {
        auto slot = externAbiSlot(p);
        argSlots.push_back(slot);
        paramTys.push_back(slot.abiTy ? slot.abiTy : llvm::PointerType::get(_context, 0));
    }

    llvm::Type* llvmRet = _builder.getVoidTy();
    if (!retSret && !fnSymbol.retType.empty()) {
        llvmRet = retSlot.abiTy ? retSlot.abiTy : _builder.getVoidTy();
        if (fnSymbol.retType.isPtr()) {
            llvmRet = llvm::PointerType::get(_context, 0);
        }
    }

    auto* fnTy = llvm::FunctionType::get(llvmRet, paramTys, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, cName, _module);
    fn->setCallingConv(llvm::CallingConv::C);

    unsigned idx = 0;
    auto addAlign = [&](unsigned i, llvm::Type* ty) {
        if (!ty) return;
        auto align = _module->getDataLayout().getABITypeAlign(ty);
        fn->addParamAttr(i, llvm::Attribute::getWithAlignment(_context, align));
    };
    if (retSret && retSlot.valueTy) {
        fn->addParamAttr(0, llvm::Attribute::getWithStructRetType(_context, retSlot.valueTy));
        addAlign(0, retSlot.valueTy);
        idx = 1;
    }
    for (size_t i = 0; i < argSlots.size(); ++i) {
        if (argSlots[i].indirect && argSlots[i].valueTy) {
            fn->addParamAttr(idx + static_cast<unsigned>(i),
                             llvm::Attribute::getWithByValType(_context, argSlots[i].valueTy));
            addAlign(idx + static_cast<unsigned>(i), argSlots[i].valueTy);
        }
    }
    return fn;
}

void Compiler::applyExternCallAttrs(llvm::CallInst* ci, const FnSymbolInfo& fnSymbol) {
    if (!ci) return;
    ci->setCallingConv(llvm::CallingConv::C);
    ExternAbiSlot retSlot = externAbiSlot(fnSymbol.retType);
    unsigned idx = 0;
    auto addAlign = [&](unsigned i, llvm::Type* ty) {
        if (!ty) return;
        auto align = _module->getDataLayout().getABITypeAlign(ty);
        ci->addParamAttr(i, llvm::Attribute::getWithAlignment(_context, align));
    };
    if (retSlot.indirect && retSlot.valueTy) {
        ci->addParamAttr(0, llvm::Attribute::getWithStructRetType(_context, retSlot.valueTy));
        addAlign(0, retSlot.valueTy);
        idx = 1;
    }
    for (size_t i = 0; i < fnSymbol.params.size(); ++i) {
        auto slot = externAbiSlot(fnSymbol.params[i]);
        if (slot.indirect && slot.valueTy) {
            const unsigned p = idx + static_cast<unsigned>(i);
            ci->addParamAttr(p, llvm::Attribute::getWithByValType(_context, slot.valueTy));
            addAlign(p, slot.valueTy);
        }
    }
}

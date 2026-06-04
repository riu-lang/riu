// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 全局变量与运行期初始化实现（DRAFT-static-vars Phase 1）
//
// 本文件实现 Compiler::compileGlobalVars():
//   为每个 GlobalVarNode 创建 LLVM GlobalVariable，
//   并生成 _yux_global_init_<Mod>() 函数在运行期执行初始化。
//
// Phase 3: const-eval 优先分流 —— 若 GlobalVarNode 携带 constValue，
// 直接 emit LLVM ConstantInitializer（零运行期开销），不进 _yux_global_init；
// 非 const-evaluable 的 init 仍走 zero-init + runtime store。

#include "compiler.h"

#include "ast/mangler.h"
#include "sema/const_eval.h"

// ==================== ConstantValue → llvm::Constant 转换 ====================

// 将 sema 期求值的 ConstantValue 转换为 LLVM 常量（用于 GlobalVariable 初始值）。
// 仅在 GlobalVarNode 标记为 const-evaluated 时调用。
static llvm::Constant* llvmConstantFromValue(llvm::LLVMContext& ctx, const ConstantValue& cv, llvm::Type* llvmType) {
    switch (cv.kind) {
    case ConstantValue::Kind::Int: {
        // 按 type width 生成 ConstantInt；intBits 为 zero-extended 表示
        // yux 类型名：i* = signed, u* = unsigned
        bool isSigned = !cv.type.name.empty() && cv.type.name[0] == 'i';
        return llvm::ConstantInt::get(llvmType, cv.intBits, isSigned);
    }
    case ConstantValue::Kind::Float: {
        return llvm::ConstantFP::get(llvmType, cv.floatVal);
    }
    case ConstantValue::Kind::Bool: {
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), cv.boolVal ? 1 : 0);
    }
    case ConstantValue::Kind::Null: {
        return llvm::ConstantPointerNull::get(llvm::dyn_cast<llvm::PointerType>(llvmType));
    }
    case ConstantValue::Kind::Struct: {
        // 结构体常量：递归转换每个字段
        std::vector<llvm::Constant*> fieldConsts;
        fieldConsts.reserve(cv.structFields.size());
        auto* structTy = llvm::dyn_cast<llvm::StructType>(llvmType);
        for (size_t i = 0; i < cv.structFields.size() && structTy && i < structTy->getNumElements(); ++i) {
            fieldConsts.push_back(llvmConstantFromValue(ctx, cv.structFields[i], structTy->getElementType(i)));
        }
        return llvm::ConstantStruct::get(llvm::dyn_cast<llvm::StructType>(llvmType), fieldConsts);
    }
    }
    return llvm::Constant::getNullValue(llvmType);
}

// ==================== Compiler::compileGlobalVars ====================

void Compiler::compileGlobalVars() {
    auto& globalVars = _file->getGlobalVars();
    if (globalVars.empty()) return;

    DEBUG_LOG("Compiling global variables...");

    // 先创建所有 GlobalVariable
    struct Item {
        llvm::GlobalVariable* gv;
        p<GlobalVarNode> node;
    };
    std::vector<Item> items;
    items.reserve(globalVars.size());

    // Phase 3: 收集需要 runtime init 的项（非 const-evaluable）
    std::vector<Item> runtimeItems;
    runtimeItems.reserve(globalVars.size());

    for (auto& gvNode : globalVars) {
        auto name = gvNode->name().getText();
        bool isPriv = !name.empty() && name[0] == '_';
        auto mangledName = Mangler::global(_file->moduleName(), name, isPriv);
        auto type = gvNode->getType();
        auto llvmType = getLLVMType(type);

        llvm::Constant* init = nullptr;
        bool isConstEval = false;

        // Phase 3: const-eval 优先 —— 若已由 sema 期求值成功，直接转 LLVM Constant
        if (auto& cv = gvNode->constValue()) {
            init = llvmConstantFromValue(_context, *cv, llvmType);
            isConstEval = true;
            DEBUG_LOG_VAL("  GlobalVar (const-eval)", name << " : " << type.name << " -> " << mangledName);
        } else {
            // 非 const-evaluable：zero-initialized，由 _yux_global_init 在运行期赋值
            init = llvm::Constant::getNullValue(llvmType);
            DEBUG_LOG_VAL("  GlobalVar (runtime)", name << " : " << type.name << " -> " << mangledName);
        }

        // #Mut 全局的 isConstant 始终 false（即便 init 是常量）；val 在 const-eval 成功时可为 true
        bool isConst = isConstEval && !gvNode->isMutable();
        auto* gv = new llvm::GlobalVariable(*_module, llvmType, isConst, llvm::GlobalValue::InternalLinkage, init,
                                            mangledName);

        items.push_back({.gv = gv, .node = gvNode});
        if (!isConstEval) {
            runtimeItems.push_back({.gv = gv, .node = gvNode});
        }
    }

    // 无需 runtime init → 跳过 _yux_global_init 生成
    if (runtimeItems.empty()) {
        DEBUG_LOG("  All global vars const-evaluated, skipping _yux_global_init");
        return;
    }

    // 生成 _yux_global_init_<Mod>() 函数
    auto fnName = "_yux_global_init_" + _file->moduleName();
    for (auto& c : fnName) {
        if (c == '.') c = '_';
    }

    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
    auto func = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, fnName, _module);

    auto entryBB = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entryBB);

    // 设置最小 fn 上下文以支持 compileExpr
    auto savedFn = _currentFn;
    auto savedFnNode = _currentFnNode;
    _currentFn = func;
    _currentFnNode = nullptr; // 全局 init 不对应任何用户 fn

    pushTempFrame();

    // 按 lexical 序编译每个 runtime init 表达式 → store 到 GlobalVariable
    for (auto& item : runtimeItems) {
        auto* value = compileExpr(item.node->value());
        if (value) {
            _builder.CreateStore(value, item.gv);
        }
    }

    popAndReleaseTempFrame();

    _builder.CreateRetVoid();

    // 恢复编译器状态
    _currentFn = savedFn;
    _currentFnNode = savedFnNode;

    DEBUG_LOG_VAL("  Emitted global init function", fnName << " (" << runtimeItems.size() << " runtime items)");
}

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 全局变量与运行期初始化实现（DRAFT-static-vars Phase 1）
//
// 本文件实现 Compiler::compileGlobalVars():
//   为每个 GlobalVarNode 创建 LLVM GlobalVariable（zero-initialized），
//   并生成 _yux_global_init_<Mod>() 函数在运行期执行初始化。
//
// Phase 1: 所有 init 都走 runtime（即便 RHS 是字面量）。
// Phase 3 会将 const-evaluable 的 init 提升为 ConstantInitializer。

#include "compiler.h"

#include "ast/mangler.h"

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

    for (auto& gvNode : globalVars) {
        auto name = gvNode->name().getText();
        bool isPriv = !name.empty() && name[0] == '_';
        auto mangledName = Mangler::global(_file->moduleName(), name, isPriv);
        auto type = gvNode->getType();
        auto llvmType = getLLVMType(type);

        // Phase 1: 全部 zero-initialized（Phase 3 提升 const-evaluable init）
        auto* init = llvm::Constant::getNullValue(llvmType);
        auto* gv =
            new llvm::GlobalVariable(*_module, llvmType,
                                     /*isConstant=*/false, llvm::GlobalValue::InternalLinkage, init, mangledName);

        items.push_back({.gv = gv, .node = gvNode});
        DEBUG_LOG_VAL("  GlobalVar", name << " : " << type.name << " -> " << mangledName);
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

    // 按 lexical 序编译每个 init 表达式 → store 到 GlobalVariable
    for (auto& item : items) {
        auto* exprNode = item.node->value();
        auto* value = compileExpr(item.node->value());
        if (value) {
            _builder.CreateStore(value, item.gv);
        }
        (void)exprNode; // Phase 2+ 可能需要节点信息
    }

    popAndReleaseTempFrame();

    _builder.CreateRetVoid();

    // 恢复编译器状态
    _currentFn = savedFn;
    _currentFnNode = savedFnNode;

    DEBUG_LOG_VAL("  Emitted global init function", fnName);
}

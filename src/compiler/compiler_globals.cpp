// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 全局变量与结构体静态字段的运行期初始化实现（DRAFT-static-vars Phase 1-4）
//
// 本文件实现 Compiler::compileGlobalVars():
//   为 GlobalVarNode 与 StructDeclNode 静态字段创建 LLVM GlobalVariable，
//   并生成 _yux_global_init_<Mod>() 函数在运行期执行初始化。
//
// Phase 3: const-eval 优先分流 —— 若初始化器 const-evaluable，
// 直接 emit LLVM ConstantInitializer（零运行期开销），不进 _yux_global_init。
//
// Phase 4: 新增 struct 静态字段支持（#Static FIELD T = expr）。

#include "compiler.h"

#include "ast/mangler.h"
#include "ast/node/struct_node.h"
#include "sema/const_eval.h"

// ==================== ConstantValue → llvm::Constant 转换 ====================

// 将 sema 期求值的 ConstantValue 转换为 LLVM 常量（用于 GlobalVariable 初始值）。
static llvm::Constant* llvmConstantFromValue(llvm::LLVMContext& ctx, const ConstantValue& cv, llvm::Type* llvmType) {
    switch (cv.kind) {
    case ConstantValue::Kind::Int: {
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

// 统一表示一组需要 runtime init 的项（来自 global let 或 struct 静态字段）
struct RuntimeItem {
    llvm::GlobalVariable* gv;
    p<ExprNode> initExpr;
};

// ==================== Compiler::compileGlobalVars ====================

void Compiler::compileGlobalVars() {
    auto& globalVars = _file->getGlobalVars();
    auto& structDecls = _file->getStructDecls();

    // 先收集 struct 静态字段（Phase 4）
    struct StaticFieldSource {
        string structName;
        const StructDeclNode::StaticFieldEntry* entry;
    };
    std::vector<StaticFieldSource> staticFieldSources;
    for (auto& sd : structDecls) {
        for (auto& sf : sd->staticFields()) {
            staticFieldSources.push_back({.structName = sd->name().getText(), .entry = &sf});
        }
    }

    if (globalVars.empty() && staticFieldSources.empty()) return;

    DEBUG_LOG("Compiling global variables and static fields...");

    std::vector<RuntimeItem> runtimeItems;

    // —— 全局 let 变量 ——
    for (auto& gvNode : globalVars) {
        auto name = gvNode->name().getText();
        bool isPriv = !name.empty() && name[0] == '_';
        auto mangledName = Mangler::global(_file->moduleName(), name, isPriv);
        auto type = gvNode->getType();
        auto llvmType = getLLVMType(type);

        llvm::Constant* init = nullptr;
        bool isConstEval = false;

        if (auto& cv = gvNode->constValue()) {
            init = llvmConstantFromValue(_context, *cv, llvmType);
            isConstEval = true;
        } else {
            init = llvm::Constant::getNullValue(llvmType);
        }

        bool isConst = isConstEval && !gvNode->isMutable();
        auto* gv = new llvm::GlobalVariable(*_module, llvmType, isConst, llvm::GlobalValue::InternalLinkage, init,
                                            mangledName);
        DEBUG_LOG_VAL("  GlobalVar",
                      name << " : " << type.name << " -> " << mangledName << (isConstEval ? " [const]" : " [runtime]"));

        if (!isConstEval) {
            runtimeItems.push_back({.gv = gv, .initExpr = gvNode->value()});
        }
    }

    // —— struct 静态字段（Phase 4）——
    for (auto& sfs : staticFieldSources) {
        auto& sf = *sfs.entry;
        auto fieldName = sf.name.getText();
        auto mangledName = Mangler::staticField(_file->moduleName(), sfs.structName, fieldName);
        auto type = sf.type->getType();
        auto llvmType = getLLVMType(type);

        // 尝试 const-eval
        llvm::Constant* init = nullptr;
        bool isConstEval = false;

        ConstEvaluator ev;
        ev.setFile(_file);
        for (const auto& prior : _file->getGlobalConsts()) {
            auto v = ev.eval(prior->value());
            if (v) ev.setNamedConst(prior->name().getText(), *v);
        }
        if (auto cv = ev.eval(sf.init)) {
            init = llvmConstantFromValue(_context, *cv, llvmType);
            isConstEval = true;
        } else {
            init = llvm::Constant::getNullValue(llvmType);
        }

        bool isConst = isConstEval && !sf.isMutable;
        auto* gv = new llvm::GlobalVariable(*_module, llvmType, isConst, llvm::GlobalValue::InternalLinkage, init,
                                            mangledName);
        DEBUG_LOG_VAL("  StaticField", sfs.structName << "::" << fieldName << " : " << type.name << " -> "
                                                      << mangledName << (isConstEval ? " [const]" : " [runtime]"));

        if (!isConstEval) {
            runtimeItems.push_back({.gv = gv, .initExpr = sf.init});
        }
    }

    // 无需 runtime init → 跳过 _yux_global_init 生成
    if (runtimeItems.empty()) {
        DEBUG_LOG("  All items const-evaluated, skipping _yux_global_init");
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

    auto savedFn = _currentFn;
    auto savedFnNode = _currentFnNode;
    _currentFn = func;
    _currentFnNode = nullptr;

    pushTempFrame();

    for (auto& item : runtimeItems) {
        auto* value = compileExpr(item.initExpr);
        if (value) {
            _builder.CreateStore(value, item.gv);
        }
    }

    popAndReleaseTempFrame();

    _builder.CreateRetVoid();

    _currentFn = savedFn;
    _currentFnNode = savedFnNode;

    DEBUG_LOG_VAL("  Emitted global init function", fnName << " (" << runtimeItems.size() << " runtime items)");
}

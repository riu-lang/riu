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
    case ConstantValue::Kind::String: {
        // Phase 6: String ConstantValue 需要 Compiler 上下文（emitStringRcBlockConst）.
        // 此静态函数无法处理 String; 调用方应使用 Compiler::buildLLVMConstantFromValue.
        return nullptr;
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
    ExprNode* initExpr;
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

    // Phase 6: 始终生成 _yux_global_init_<Mod>() 桩 —— 即使本模块无全局/静态字段，
    // 跨模块 main shim 也会按 loadOrder 调用它。无字段时函数体仅一条 ret。
    bool hasItems = !globalVars.empty() || !staticFieldSources.empty();

    if (hasItems) {
        DEBUG_LOG("Compiling global variables and static fields...");
    }

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

        // #Cval #Inline：纯内联常量（类似 C #define），不产生 GlobalVariable / 符号。
        // 使用处由 compileEnumCtorExpr 直接求值替换，跨文件访问同样走 AST 内联。
        if (sf.isCval && sf.isInline) {
            DEBUG_LOG_VAL("  StaticField (inline)",
                          sfs.structName << "::" << fieldName << " : " << type.name << " [inline, no GlobalVariable]");
            continue;
        }

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
        // 非私有静态字段 ExternalLinkage：跨模块 `Mod.Struct::FIELD` 读/写要链到定义。
        // 私有（`_` 前缀）仍 Internal，与全局 #Cval 一致。
        bool isPriv = !fieldName.empty() && fieldName[0] == '_';
        auto linkage = isPriv ? llvm::GlobalValue::InternalLinkage : llvm::GlobalValue::ExternalLinkage;
        auto* gv = new llvm::GlobalVariable(*_module, llvmType, isConst, linkage, init, mangledName);
        DEBUG_LOG_VAL("  StaticField", sfs.structName << "::" << fieldName << " : " << type.name << " -> "
                                                      << mangledName << (isConstEval ? " [const]" : " [runtime]"));

        if (!isConstEval) {
            runtimeItems.push_back({.gv = gv, .initExpr = sf.init});
        }
    }

    // Phase 6: 始终生成 __yux_global_init.<Mod>() —— 即使无 runtime init item，
    // 跨模块 main shim 也会按拓扑序调用它；空函数开销为一条 ret。
    auto fnName = "__yux_global_init." + _file->moduleName();

    // Phase 6: ExternalLinkage 以便跨模块 main shim 调用（DRAFT-static-vars §5.5）
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
    auto func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);

    auto entryBB = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entryBB);

    auto savedFn = _currentFn;
    auto savedFnNode = _currentFnNode;
    _currentFn = func;
    _currentFnNode = nullptr;

    pushTempFrame();

    if (!runtimeItems.empty()) {
        for (auto& item : runtimeItems) {
            auto* value = compileExpr(item.initExpr);
            if (value) {
                _builder.CreateStore(value, item.gv);
            }
        }
    }

    popAndReleaseTempFrame();

    _builder.CreateRetVoid();

    _currentFn = savedFn;
    _currentFnNode = savedFnNode;

    DEBUG_LOG_VAL("  Emitted global init function", fnName << " (" << runtimeItems.size() << " runtime items)");
}

llvm::GlobalVariable* Compiler::getOrDeclareStaticFieldGV(const string& mangledName, llvm::Type* llvmType,
                                                          bool isConstant) {
    if (auto* gv = _module->getGlobalVariable(mangledName, true)) {
        return gv;
    }
    return new llvm::GlobalVariable(*_module, llvmType, isConstant, llvm::GlobalValue::ExternalLinkage, nullptr,
                                    mangledName);
}

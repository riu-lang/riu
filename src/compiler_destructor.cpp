// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 析构函数编译实现
// 
// 本文件包含析构函数相关的编译逻辑:
// - 自动生成默认析构函数
// - 调用结构体字段的析构函数
// - 调用作用域内所有变量的析构函数
// - 检查类型是否需要析构函数

#include "compiler.h"
#include "mangler.h"
#include <llvm/IR/Instructions.h>

// ==================== 析构函数调用 ====================

// Phase 3d: 在槽位地址上释放 RC 值
// 对 Box/Array/Weak: 从 { ptr handle } 槽 load handle 调对应 release
// 含 RC 字段 struct: 调其默认析构（字段逆序 release）；
// 平凡 / 内置 / 引用 / 指针: no-op
void Compiler::releaseAtPtr(llvm::Value* slotPtr, const TypeInfo& type) {
    if (!slotPtr) return;
    if (type.isRef() || type.isPtr()) return;
    if (isBuiltinType(type.name)) return;

    if (type.isBox()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.box.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "old.box.handle");
        _builder.CreateCall(runtime::getBoxReleaseFn(_module, _builder), {handle});
        return;
    }
    if (type.isWeak()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.weak.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "old.weak.handle");
        _builder.CreateCall(runtime::getWeakReleaseFn(_module, _builder), {handle});
        return;
    }
    if (type.isArrayGeneric()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.array.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "old.array.handle");
        _builder.CreateCall(runtime::getArrayReleaseFn(_module, _builder), {handle});
        return;
    }

    // 结构体：调其析构函数（默认析构按字段逆序 release）
    if (structNeedsDestructor(type.name)) {
        auto dtorFn = getDestructorFunction(type.name);
        if (dtorFn) {
            _builder.CreateCall(dtorFn, {slotPtr});
        }
    }
}

// 调用单个变量的析构函数
// 如果变量类型有析构函数，则调用它
void Compiler::callDestructor(const string& varName, const TypeInfo& varType) {
    // 内置 / 引用 / 指针类型不需要析构
    if (isBuiltinType(varType.name)) return;
    if (varType.isRef() || varType.isPtr()) return;

    auto it = _localVarPtrs.find(varName);
    if (it == _localVarPtrs.end()) return;

    DEBUG_LOG_VAL("  Calling destructor for", varName << " : " << varType.name);
    releaseAtPtr(it->second, varType);
}

// 调用当前作用域所有变量的析构函数
// 按照变量声明的逆序调用 (后进先出)
void Compiler::callDestructorsForScope() {
    // 逆序遍历作用域变量列表
    for (auto it = _scopeVars.rbegin(); it != _scopeVars.rend(); ++it) {
        auto varName = *it;
        auto sym = _currentFnNode->lookupSymbol(varName);
        if (sym) {
            callDestructor(varName, sym->type);
        }
    }
}

// 调用结构体字段的析构函数
// 用于结构体析构函数中，递归调用所有字段的析构函数
void Compiler::callFieldDestructor(llvm::Value* structPtr, const string& structName) {
    auto fieldTypes = resolveStructFieldTypes(structName);
    if (fieldTypes.empty()) return;

    auto structType = _structTypes.count(structName)
        ? _structTypes[structName]
        : llvm::cast_or_null<llvm::StructType>(getLLVMType(TypeInfo(structName)));
    if (!structType) return;
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

    // Phase 3d: 按声明逆序释放（DRAFT §7.4 "构造逆序对每个 RC 字段 release"）
    for (size_t k = fieldTypes.size(); k > 0; --k) {
        size_t i = k - 1;
        const auto& fieldType = fieldTypes[i];

        // 检查字段是否需要析构
        if (!typeNeedsDestructor(fieldType)) continue;

        // 获取字段指针
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, idx};
        auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "field.ptr");

        // 调用字段析构函数
        if (fieldType.isBox()) {
            // Box 字段：load handle，调用 _box_release(handle)
            auto boxStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(boxStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto boxReleaseFn = runtime::getBoxReleaseFn(_module, _builder);
            _builder.CreateCall(boxReleaseFn, {handle});
        } else if (fieldType.isWeak()) {
            // Weak 字段：load handle，调用 _weak_release(handle)
            auto weakStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(weakStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto weakReleaseFn = runtime::getWeakReleaseFn(_module, _builder);
            _builder.CreateCall(weakReleaseFn, {handle});
        } else if (fieldType.isArrayGeneric()) {
            // Array 字段：load handle，调用 _array_release(handle)
            auto arrayStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(arrayStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto arrayReleaseFn = runtime::getArrayReleaseFn(_module, _builder);
            _builder.CreateCall(arrayReleaseFn, {handle});
        } else if (!isBuiltinType(fieldType.name)) {
            // 结构体字段: 调用其析构函数
            auto fieldDtorsFn = getDestructorFunction(fieldType.name);
            if (fieldDtorsFn) {
                _builder.CreateCall(fieldDtorsFn, {fieldPtr});
            }
        }
    }
}

// ==================== 默认析构函数生成 ====================

// 为结构体生成默认析构函数
// 默认析构函数递归调用所有字段的析构函数
void Compiler::generateDefaultDestructor(const string& structName) {
    DEBUG_LOG_VAL("  Generating default destructor for", structName);

    // 获取或创建析构函数
    auto dtorFn = getDestructorFunction(structName);
    if (!dtorFn) return;

    // 创建入口基本块
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", dtorFn);
    _builder.SetInsertPoint(entry);

    // 获取当前实例参数（`$`）
    auto thisArg = &*dtorFn->arg_begin();

    // 调用字段析构函数
    callFieldDestructor(thisArg, structName);

    // 返回
    _builder.CreateRetVoid();
}

// ==================== Phase 3a: 调用点 retain ====================

// 给堆句柄实参在传入前 retain（callee-clean 调用约定，DRAFT §7.3）
// 非堆句柄类型 no-op；返回 true 表示已发出 retain
bool Compiler::retainHandleAtCallSite(llvm::Value* argVal, const TypeInfo& argType) {
    if (!argVal) return false;

    auto extractHandle = [&](const string& name) -> llvm::Value* {
        // argVal 是 { ptr handle } struct value（来自 compileExpr 的 load）
        return _builder.CreateExtractValue(argVal, {0}, name);
    };

    if (argType.isBox()) {
        auto handle = extractHandle("arg.box.handle");
        auto retainFn = runtime::getBoxRetainFn(_module, _builder);
        _builder.CreateCall(retainFn, {handle});
        return true;
    }
    if (argType.isArrayGeneric()) {
        auto handle = extractHandle("arg.array.handle");
        auto retainFn = runtime::getArrayRetainFn(_module, _builder);
        _builder.CreateCall(retainFn, {handle});
        return true;
    }
    if (argType.isWeak()) {
        auto handle = extractHandle("arg.weak.handle");
        auto retainFn = runtime::getWeakRetainFn(_module, _builder);
        _builder.CreateCall(retainFn, {handle});
        return true;
    }

    // Phase 3c.2.a: 含 RC 字段的非平凡 struct 按值传参，逐字段 retain；callee 析构释放
    if (!isBuiltinType(argType.name) && structNeedsDestructor(argType.name)) {
        retainStructFieldsAtCallSite(argVal, argType.name);
        return true;
    }
    return false;
}

// 递归把 struct value 中所有 RC 字段（含子 struct）retain
// argVal 为按值 struct LLVM aggregate
// 普通 struct / 泛型实例统一走 resolveStructFieldTypes
void Compiler::retainStructFieldsAtCallSite(llvm::Value* argVal, const string& structName) {
    auto fieldTypes = resolveStructFieldTypes(structName);
    for (size_t i = 0; i < fieldTypes.size(); ++i) {
        const auto& ft = fieldTypes[i];
        if (!typeNeedsDestructor(ft)) continue;

        if (ft.isBox() || ft.isArrayGeneric() || ft.isWeak()) {
            // 取字段值（{ ptr handle } struct），再取 handle
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.val");
            auto handle = _builder.CreateExtractValue(fieldVal, {0}, "field.handle");
            llvm::Function* retainFn = nullptr;
            if (ft.isBox()) retainFn = runtime::getBoxRetainFn(_module, _builder);
            else if (ft.isArrayGeneric()) retainFn = runtime::getArrayRetainFn(_module, _builder);
            else retainFn = runtime::getWeakRetainFn(_module, _builder);
            _builder.CreateCall(retainFn, {handle});
        } else if (!isBuiltinType(ft.name)) {
            // 嵌套 struct 字段：递归
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.struct");
            retainStructFieldsAtCallSite(fieldVal, ft.name);
        }
    }
}

// ==================== Phase 8d.1: per-statement 临时清单 ====================

// 在新语句入口 push 一个空帧
void Compiler::pushTempFrame() {
    _tempStack.emplace_back();
}

// 弹出顶帧；对其中未消费的 fresh RC 句柄发出 release（顺序无关，统一在帧末尾）
// 覆盖 Box/Array/Weak（单 handle by-value）+ 含 RC 字段 struct value（Phase 8d.4，靠 spillSlot dtor）。
// 调用前必须保证当前 IR 插入点能 dominate 帧内所有 Value*（线性控制流要求）。
void Compiler::popAndReleaseTempFrame() {
    if (_tempStack.empty()) return;
    auto frame = std::move(_tempStack.back());
    _tempStack.pop_back();
    if (frame.empty()) return;

    // 当前 BB 已被终结（如 ret 已 emit）就直接丢弃，避免在 unreachable 后插入指令
    auto* bb = _builder.GetInsertBlock();
    if (bb && bb->getTerminator()) return;

    for (auto& t : frame) {
        if (!t.val) continue;
        if (t.type.isBox()) {
            auto handle = _builder.CreateExtractValue(t.val, {0}, "temp.box.handle");
            _builder.CreateCall(runtime::getBoxReleaseFn(_module, _builder), {handle});
        } else if (t.type.isArrayGeneric()) {
            auto handle = _builder.CreateExtractValue(t.val, {0}, "temp.array.handle");
            _builder.CreateCall(runtime::getArrayReleaseFn(_module, _builder), {handle});
        } else if (t.type.isWeak()) {
            auto handle = _builder.CreateExtractValue(t.val, {0}, "temp.weak.handle");
            _builder.CreateCall(runtime::getWeakReleaseFn(_module, _builder), {handle});
        } else if (t.spillSlot) {
            // Phase 8d.4: 含 RC 字段 struct value：调其析构（按字段逆序 release）
            releaseAtPtr(t.spillSlot, t.type);
        }
    }
}

// 记录一个 fresh RC 临时到顶帧
// - Box/Array/Weak: 直接保存 by-value struct {ptr handle}，pop 时 extractValue 取 handle
// - 含 RC 字段 struct (e.g. String): 入 entry-block alloca 留 dtor 用，pop 时调 releaseAtPtr
void Compiler::recordTemp(llvm::Value* val, const TypeInfo& type) {
    if (!val) return;
    if (_tempStack.empty()) return;
    if (type.isBox() || type.isArrayGeneric() || type.isWeak()) {
        _tempStack.back().push_back({val, type, nullptr});
        return;
    }
    // Phase 8d.4: 含 RC 字段的 struct value（如 String）—— 落 entry 块 alloca，由 releaseAtPtr/dtor 释放
    if (type.isRef() || type.isPtr()) return;
    if (isBuiltinType(type.name)) return;
    if (!structNeedsDestructor(type.name)) return;

    auto* fn = _builder.GetInsertBlock()->getParent();
    auto& entryBB = fn->getEntryBlock();
    llvm::IRBuilder<> entryBuilder(&entryBB, entryBB.getFirstInsertionPt());
    auto slot = entryBuilder.CreateAlloca(getLLVMType(type), nullptr, "temp.struct.spill");
    _builder.CreateStore(val, slot);
    _tempStack.back().push_back({val, type, slot});
}

// 消费顶帧中匹配的 Value*（用于 declare-assign / assign / ret / fresh-arg-callsite 路径）
// 不存在则忽略（节点可能根本没产生 fresh，比如变量引用）
// 返回 true 表示找到并移除（调用方借此判断"是否为 fresh"）
bool Compiler::consumeTemp(llvm::Value* val) {
    if (!val) return false;
    if (_tempStack.empty()) return false;
    auto& frame = _tempStack.back();
    for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
        if (it->val == val) {
            frame.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

// Phase 8d.3: 编译分支体的结果表达式：用子帧吃掉中间 fresh 临时；非 fresh 结果发 retain 归一
llvm::Value* Compiler::compileBranchResultNormalized(p<ExprNode> expr, const TypeInfo& expectedType) {
    bool isRcHandle = expectedType.isBox() || expectedType.isArrayGeneric() || expectedType.isWeak();
    if (!isRcHandle) {
        return compileExpr(expr);
    }
    pushTempFrame();
    auto val = compileExpr(expr);
    bool wasFresh = consumeTemp(val);
    popAndReleaseTempFrame();
    if (!wasFresh && val) {
        emitRetainOnHandleValue(val, expectedType);
    }
    return val;
}

// Phase 8d.3: 对一个已存在的 RC 句柄 by-value（Box/Array/Weak struct value）发 retain。
// 调用前 IR 插入点必须 dominate val。用于分支汇合时把"借用结果"归一为"fresh +1"。
void Compiler::emitRetainOnHandleValue(llvm::Value* val, const TypeInfo& type) {
    if (!val) return;
    if (type.isBox()) {
        auto handle = _builder.CreateExtractValue(val, {0}, "merge.box.handle");
        _builder.CreateCall(runtime::getBoxRetainFn(_module, _builder), {handle});
    } else if (type.isArrayGeneric()) {
        auto handle = _builder.CreateExtractValue(val, {0}, "merge.array.handle");
        _builder.CreateCall(runtime::getArrayRetainFn(_module, _builder), {handle});
    } else if (type.isWeak()) {
        auto handle = _builder.CreateExtractValue(val, {0}, "merge.weak.handle");
        _builder.CreateCall(runtime::getWeakRetainFn(_module, _builder), {handle});
    }
}

// ==================== Phase 8b: fresh 表达式判定 ====================

// 识别 +1 所有权（fresh）表达式：调用结果（函数 / 方法 / 构造器）+ 数组字面量
// 用于在复制语义 retain 路径上跳过多余 retain，避免 leak（DRAFT §7.6 / §8）
bool Compiler::isFreshHandleExpr(p<ExprNode> expr) {
    if (!expr) return false;
    if (dynamic_cast<ExprCallNode*>(expr)) return true;
    if (dynamic_cast<ExprArrayNode*>(expr)) return true;
    return false;
}

// ==================== 析构函数需求检查 ====================

// 检查类型是否需要析构函数
bool Compiler::typeNeedsDestructor(const TypeInfo& type) {
    // 内置类型不需要析构
    if (isBuiltinType(type.name)) return false;

    // 引用类型不需要析构 (不拥有数据)
    if (type.isRef()) return false;

    // 指针类型不需要析构 (不拥有数据)
    if (type.isPtr()) return false;

    // Box / Weak / Array 需要析构
    if (type.isBox() || type.isWeak() || type.isArrayGeneric()) return true;

    // 检查结构体是否需要析构
    return structNeedsDestructor(type.name);
}

// Phase 3c.2.a/c: 用户 struct（普通 + 泛型实例）一律 by-value
// 仅 _structTypes 已注册但找不到声明的跨模块 struct：保守按指针
bool Compiler::structParamUsesPointer(const string& typeName) {
    if (isBuiltinType(typeName)) return false;

    // Ptr / 引用形参不是 struct，按值传递（原始 ptr）
    TypeInfo ti(typeName);
    if (ti.isPtr() || ti.isRef()) return false;

    // 普通 struct（当前文件 / SDK）→ by-value
    auto structDecl = _file->getStructDecl(typeName);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(typeName);
    }
    if (structDecl) return false;

    // 泛型实例 → by-value（3c.2.c）；fields 通过 resolveStructFieldTypes 套替换
    if (_structInstances.find(typeName) != _structInstances.end()) return false;

    // 仅在 LLVM 类型表中注册的（跨模块未通配导入等）保守按指针
    if (_structTypes.find(typeName) != _structTypes.end()) {
        return true;
    }

    return false;
}

// 检查结构体是否需要析构函数
// 如果结构体有任何需要析构的字段，则需要析构函数
// 普通 struct 走 fields()；泛型实例走 baseDecl + 实例 args 替换
bool Compiler::structNeedsDestructor(const string& structName) {
    auto fieldTypes = resolveStructFieldTypes(structName);
    for (const auto& ft : fieldTypes) {
        if (typeNeedsDestructor(ft)) return true;
    }
    return false;
}

// Phase 3c.2.c: 解析任何 struct（含泛型实例）的字段类型清单
// 普通 struct → fields() 直接取
// 泛型实例 → baseDecl 字段套实例 args 替换
// 找不到返回空（_structTypes-only 的跨模块 struct 等）
vector<TypeInfo> Compiler::resolveStructFieldTypes(const string& structName) {
    vector<TypeInfo> out;

    auto structDecl = _file->getStructDecl(structName);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(structName);
    }
    if (structDecl) {
        out.reserve(structDecl->fields().size());
        for (auto field : structDecl->fields()) {
            out.push_back(field->getType());
        }
        return out;
    }

    // 泛型实例：拼接 baseDecl typeParams → 实例 args 的替换表
    auto instIt = _structInstances.find(structName);
    if (instIt != _structInstances.end() && instIt->second.baseDecl) {
        const auto& inst = instIt->second;
        std::map<string, TypeInfo> subst;
        const auto& tparams = inst.baseDecl->typeParams();
        for (size_t i = 0; i < tparams.size() && i < inst.args.size(); ++i) {
            subst[tparams[i]] = inst.args[i];
        }
        out.reserve(inst.baseDecl->fields().size());
        for (auto field : inst.baseDecl->fields()) {
            out.push_back(field->getType().substitute(subst));
        }
    }
    return out;
}

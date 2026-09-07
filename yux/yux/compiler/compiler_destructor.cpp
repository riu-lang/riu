// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 析构函数编译实现
//
// 本文件包含析构函数相关的编译逻辑:
// - 自动生成默认析构函数
// - 调用结构体字段的析构函数
// - 调用作用域内所有变量的析构函数
// - 检查类型是否需要析构函数

#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "compiler.h"
#include <algorithm>
#include <llvm/IR/Instructions.h>

// ==================== 析构函数调用 ====================

// Phase 3d: 在槽位地址上释放 RC 值
// 对 Rc/Array/Weak: 从 { ptr handle } 槽 load handle 调对应 release
// 含 RC 字段 struct: 调其默认析构（字段逆序 release）；
// 平凡 / 内置 / 引用 / 指针: no-op
void Compiler::releaseAtPtr(llvm::Value* slotPtr, const TypeInfo& type) {
    if (!slotPtr) return;
    if (type.isRef() || type.isPtr()) return;
    if (isBuiltinType(type.name)) return;

    if (type.isRc()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.rc.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "old.rc.handle");
        // Phase B-2: 若内层 T 需析构，用 typed release（strong==0 时调 T::~()）
        auto releaseFn = getOrCreateRcTypedReleaseFn(type);
        _builder.CreateCall(releaseFn, {handle});
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
        // B-3: Array 析构 — 直接 free _data buffer（GEP 走三字段 layout）
        auto ptrTy = llvm::PointerType::get(_context, 0);
        auto dataField = arrayDataFieldPtr(slotPtr, "old.array");
        auto data = _builder.CreateLoad(ptrTy, dataField, "old.array.data");
        _builder.CreateCall(runtime::getArrayFreeDataFn(_module, _builder), {data});
        return;
    }

    // Heap<T>（DRAFT-heap-types §8.3a 单所有权堆作用域句柄）
    // 槽内 = 裸 T*；释放顺序：1) T 自身析构（按 T 槽=已加载 ptr） 2) __yux_heap_free
    // 注：__yux_heap_free 对 null 安全（emitHeapHandleHelpers 内 null-check）；
    // Heap 不参与 RC 计数，无 retain/release 计数语义
    if (type.isHeap()) {
        auto elemSp = type.heapElementType();
        auto ptr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), slotPtr, "old.heap.payload");
        if (elemSp && typeNeedsDestructor(*elemSp)) {
            releaseAtPtr(ptr, *elemSp);
        }
        _builder.CreateCall(runtime::getHeapHandleFreeFn(_module, _builder), {ptr});
        return;
    }

    // Phase 3d.2: Nullable<Heap<T>> 即 `Heap<T>?` —— 槽 = { i1 _has, ptr _value }.
    // _has=false 时直接跳过 (B 档 move 写回 null 后的状态); _has=true 走与 Heap<T>
    // 相同的释放序: 内层 T 析构 + __yux_heap_free.
    if (type.isNullable()) {
        auto inner = type.nullableInnerType();
        if (inner && inner->isHeap()) {
            auto ty = getLLVMType(type);
            auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto valueField = _builder.CreateGEP(ty, slotPtr, {z, one}, "old.heap_opt.value_field");
            auto ptr = _builder.CreateLoad(ptrTy, valueField, "old.heap_opt.payload");
            // __yux_heap_free 对 null 安全, 内层 T 若需析构则按非 null 才调.
            auto elemSp = inner->heapElementType();
            if (elemSp && typeNeedsDestructor(*elemSp)) {
                auto isNull = _builder.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy), "old.heap_opt.isnull");
                auto* pf = _builder.GetInsertBlock()->getParent();
                auto* dropBB = llvm::BasicBlock::Create(_context, "old.heap_opt.drop", pf);
                auto* contBB = llvm::BasicBlock::Create(_context, "old.heap_opt.cont", pf);
                _builder.CreateCondBr(isNull, contBB, dropBB);
                _builder.SetInsertPoint(dropBB);
                releaseAtPtr(ptr, *elemSp);
                _builder.CreateCall(runtime::getHeapHandleFreeFn(_module, _builder), {ptr});
                _builder.CreateBr(contBB);
                _builder.SetInsertPoint(contBB);
            } else {
                _builder.CreateCall(runtime::getHeapHandleFreeFn(_module, _builder), {ptr});
            }
            return;
        }
        // Nullable<Rc<T>> / Nullable<Weak<T>> / Nullable<含 RC 字段 struct> 等：
        // _has=true 时释放内层 T 的值
        if (inner && typeNeedsDestructor(*inner)) {
            auto ty = getLLVMType(type);
            auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto hasField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.nullable.has_field");
            auto hasVal = _builder.CreateLoad(_builder.getInt1Ty(), hasField, "old.nullable.has");
            auto valueField = _builder.CreateGEP(ty, slotPtr, {z, one}, "old.nullable.value_field");
            auto* pf = _builder.GetInsertBlock()->getParent();
            auto* dropBB = llvm::BasicBlock::Create(_context, "old.nullable.drop", pf);
            auto* contBB = llvm::BasicBlock::Create(_context, "old.nullable.cont", pf);
            _builder.CreateCondBr(hasVal, dropBB, contBB);
            _builder.SetInsertPoint(dropBB);
            releaseAtPtr(valueField, *inner);
            _builder.CreateBr(contBB);
            _builder.SetInsertPoint(contBB);
            return;
        }
    }

    // Phase 3e: owned Dyn<D> 释放
    // layout = { ptr vtable, ptr data }；data 指 [RC head | 实例]
    // 走 _dyn_release(data, vtable)：strong-- → if 0 then dtor=vtable[0] dispatch(data+8) → weak-- + free
    // Dyn<D&> 借用形态不动 RC，跳过
    if (type.isDynOwned()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
        auto vtableField = _builder.CreateGEP(ty, slotPtr, {z, z}, "old.dyn.vtable_field");
        auto vtable = _builder.CreateLoad(llvm::PointerType::get(_context, 0), vtableField, "old.dyn.vtable");
        auto dataField = _builder.CreateGEP(ty, slotPtr, {z, one}, "old.dyn.data_field");
        auto data = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataField, "old.dyn.data");
        _builder.CreateCall(runtime::getDynReleaseFn(_module, _builder), {data, vtable});
        return;
    }
    if (type.isDynBorrow()) {
        // 借用形态：不动 RC，源 owner 持有；release 等价 no-op
        return;
    }

    // Phase 3a / 4a-2 / 4c: fn(...)R fat-ptr { fn_ptr, captures Rc<CapturesT>? }
    // captures 字段在 offset 1；走 _box_release_dtor 让运行时在 strong 归零时 dispatch
    // payload[0..8] 处的 dtor fn ptr（4a-2）。零捕获 / 全标量场景 dtor 槽存 null，
    // 行为等价于纯 _box_release。
    // Phase 4c：栈嵌入 captures 把 LSB 标 1（spec §6.3 不可逃逸），release 站点检测后跳过。
    if (type.isFn()) {
        auto ty = getLLVMType(type);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
        auto capField = _builder.CreateGEP(ty, slotPtr, {z, one}, "old.fn.captures_field");
        auto cap = _builder.CreateLoad(llvm::PointerType::get(_context, 0), capField, "old.fn.captures");
        // 跳过条件：cap == null（零捕获）或 cap LSB == 1（栈嵌入 4c）
        auto i64Ty = _builder.getInt64Ty();
        auto capInt = _builder.CreatePtrToInt(cap, i64Ty, "old.fn.cap.asint");
        auto isStack = _builder.CreateICmpNE(_builder.CreateAnd(capInt, _builder.getInt64(1)), _builder.getInt64(0),
                                             "old.fn.cap.isstack");
        auto isNull = _builder.CreateICmpEQ(cap, llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)),
                                            "old.fn.cap.isnull");
        auto skip = _builder.CreateOr(isStack, isNull, "old.fn.cap.skip");
        auto* pf = _builder.GetInsertBlock()->getParent();
        auto* relBB = llvm::BasicBlock::Create(_context, "old.fn.cap.rel", pf);
        auto* contBB = llvm::BasicBlock::Create(_context, "old.fn.cap.cont", pf);
        _builder.CreateCondBr(skip, contBB, relBB);
        _builder.SetInsertPoint(relBB);
        _builder.CreateCall(runtime::getRcReleaseDtorFn(_module, _builder), {cap});
        _builder.CreateBr(contBB);
        _builder.SetInsertPoint(contBB);
        return;
    }

    // Phase 5: enum 类型 —— 走合成的 __enum_drop_<E> 按 tag dispatch
    if (enumNeedsDestructor(type)) {
        auto dtorFn = getEnumDestructorFunction(type.name, type.ownerModule);
        if (dtorFn) {
            _builder.CreateCall(dtorFn, {slotPtr});
        }
        return;
    }

    // 结构体：调其析构函数（默认析构按字段逆序 release）
    if (structNeedsDestructor(type)) {
        auto dtorFn = getDestructorFunction(type.isGeneric() ? type.getMangleName() : type.name, type.ownerModule);
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

void Compiler::pushScopeFrame() {
    _scopeFrames.emplace_back();
}

void Compiler::popScopeFrameAndDestroy() {
    if (_scopeFrames.empty()) return;
    auto& frame = _scopeFrames.back();
    for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
        if (it->needsDtor && !_movedVars.count(it->name)) {
            callDestructor(it->name, it->type);
        }
        if (it->prevPtr) {
            _localVarPtrs[it->name] = it->prevPtr;
        } else {
            _localVarPtrs.erase(it->name);
        }
        _movedVars.erase(it->name);
    }
    _scopeFrames.pop_back();
}

void Compiler::popScopeFrameNoDestroy() {
    if (_scopeFrames.empty()) return;
    for (auto& v : _scopeFrames.back()) {
        if (v.prevPtr) {
            _localVarPtrs[v.name] = v.prevPtr;
        } else {
            _localVarPtrs.erase(v.name);
        }
        _movedVars.erase(v.name);
    }
    _scopeFrames.pop_back();
}

void Compiler::emitDestructorsAbove(size_t depth) {
    // 仅发射析构 IR，不改编译期帧栈（供 break / ret；兄弟分支仍需同一帧结构）
    for (size_t i = _scopeFrames.size(); i > depth; --i) {
        auto& frame = _scopeFrames[i - 1];
        for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
            if (!it->needsDtor || _movedVars.count(it->name)) continue;
            callDestructor(it->name, it->type);
        }
    }
}

void Compiler::unwindScopeFramesTo(size_t depth) {
    while (_scopeFrames.size() > depth) {
        popScopeFrameNoDestroy();
    }
}

void Compiler::pushScopeVar(const string& name, const TypeInfo& type, llvm::Value* prevPtr, bool needsDtor) {
    if (_scopeFrames.empty()) pushScopeFrame();
    _scopeFrames.back().push_back({.name = name, .type = type, .prevPtr = prevPtr, .needsDtor = needsDtor});
}

void Compiler::registerLocalVar(const string& name, llvm::Value* alloca, const TypeInfo& type) {
    llvm::Value* prev = nullptr;
    auto it = _localVarPtrs.find(name);
    if (it != _localVarPtrs.end()) prev = it->second;
    _localVarPtrs[name] = alloca;
    auto resolved = resolveAlias(type);
    pushScopeVar(name, resolved, prev, typeNeedsDestructor(resolved));
}

void Compiler::eraseScopeVar(const string& name) {
    for (auto& frame : _scopeFrames) {
        std::erase_if(frame, [&](const ScopeVar& v) { return v.name == name; });
    }
}

SymbolInfo* Compiler::lookupVarSymbol(const string& name, p<Node> from) {
    if (from) {
        if (auto sc = from->findNearestScope()) {
            if (auto* s = sc->lookupSymbol(name)) return s;
        }
    }
    if (_currentFnNode) return _currentFnNode->lookupSymbol(name);
    return nullptr;
}

// ret 路径：对全部帧发射析构，不 pop（兄弟分支 / 后续编译仍依赖帧栈）
void Compiler::callDestructorsForScope() {
    emitDestructorsAbove(0);
}

// Phase 3d.3: 若 expr 是 `Heap<T>?` 的 lvalue, 返回其 slot ptr + slot llvm 类型.
// 覆盖两种形态:
//   (a) 局部 ID — `let h Heap<T>? = ...; f(h)` / `Self { .x = h }`
//   (b) 局部 struct 字段访问 — `let b Foo = ...; f(b.buf)` / `Self { .x = b.buf }`
// 不支持: Rc / ref base 的字段, 索引 lvalue, 跨函数链式 (后续切片).
// 用于 B 档 nullable move 把源槽写 {has=false, value=null}, 让作用域尾析构跳 free.
bool Compiler::tryHeapNullableLvalueSlot(ExprNode* expr, llvm::Value*& outSlot, llvm::Type*& outTy) {
    outSlot = nullptr;
    outTy = nullptr;
    if (!expr) return false;
    auto isHeapNullable = [](const TypeInfo& t) {
        if (!t.isNullable()) return false;
        auto inner = t.nullableInnerType();
        return inner && inner->isHeap();
    };
    // (a) 局部 ID
    if (auto litE = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(litE->literal())) {
            auto name = obj->getValue().getText();
            auto it = _localVarPtrs.find(name);
            if (it == _localVarPtrs.end()) return false;
            auto sym = lookupVarSymbol(name, litE);
            if (!sym) return false;
            auto ty = applySubst(sym->type);
            if (!isHeapNullable(ty)) return false;
            outSlot = it->second;
            outTy = getLLVMType(ty);
            return true;
        }
    }
    // (b) 局部 struct 字段访问 b.field
    if (auto dotE = dynamic_cast<ExprDotNode*>(expr)) {
        if (dotE->isSafe()) return false;
        auto baseE = dotE->baseExpr();
        auto baseLit = dynamic_cast<ExprLiteralNode*>(baseE);
        if (!baseLit) return false;
        auto baseObj = dynamic_cast<LiteralObjNode*>(baseLit->literal());
        if (!baseObj) return false;
        auto baseName = baseObj->getValue().getText();
        auto bit = _localVarPtrs.find(baseName);
        if (bit == _localVarPtrs.end()) return false;
        auto baseType = applySubst(baseE->getType());
        if (baseType.isRef() || baseType.isRc()) return false;
        StructDeclNode* sd = names().lookupStruct(baseType);
        if (!sd) {
            auto instIt = _structInstances.find(baseType.name);
            if (instIt != _structInstances.end()) sd = instIt->second.baseDecl;
        }
        if (!sd) return false;
        auto member = dotE->member();
        int idx = sd->fieldIndex(member);
        if (idx < 0) return false;
        const auto* fd = sd->field(member);
        if (!fd) return false;
        auto fieldTy = applySubst(fd->getType());
        if (!isHeapNullable(fieldTy)) return false;
        auto structLLVM = getLLVMType(baseType);
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto i = llvm::ConstantInt::get(_builder.getInt32Ty(), idx);
        outSlot = _builder.CreateGEP(structLLVM, bit->second, {z, i}, baseName + "." + member + ".slot");
        outTy = getLLVMType(fieldTy);
        return true;
    }
    return false;
}

// 调用结构体字段的析构函数
// 用于结构体析构函数中，递归调用所有字段的析构函数
void Compiler::callFieldDestructor(llvm::Value* structPtr, const string& structName) {
    auto fieldTypes = resolveStructFieldTypes(structName);
    if (fieldTypes.empty()) return;

    auto structType = _structTypes.count(structName)
                          ? _structTypes[structName]
                          : llvm::cast_or_null<llvm::StructType>(getLLVMType(typeInfoForNamedStruct(structName)));
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
        std::array<llvm::Value*, 2> indices{zero, idx};
        auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "field.ptr");

        // 调用字段析构函数
        if (fieldType.isRc()) {
            // Rc 字段：load handle，调用 _box_release(handle)
            // Phase B-2: 若内层 T 需析构，用 typed release
            auto rcStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(rcStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto rcReleaseFn = getOrCreateRcTypedReleaseFn(fieldType);
            _builder.CreateCall(rcReleaseFn, {handle});
        } else if (fieldType.isWeak()) {
            // Weak 字段：load handle，调用 _weak_release(handle)
            auto weakStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(weakStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto weakReleaseFn = runtime::getWeakReleaseFn(_module, _builder);
            _builder.CreateCall(weakReleaseFn, {handle});
        } else if (fieldType.isArrayGeneric()) {
            // B-3: Array 字段析构 — 调用 _array_free_data（GEP 走三字段 layout）
            auto dataField = arrayDataFieldPtr(fieldPtr, "fld.arr");
            auto data = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataField, "fld.arr.data");
            _builder.CreateCall(runtime::getArrayFreeDataFn(_module, _builder), {data});
        } else if (fieldType.isHeap()) {
            // Heap<T> 字段（DRAFT-heap-types §8.3a）：load 裸 T*，T 自身析构后 __yux_heap_free
            auto elemSp = fieldType.heapElementType();
            auto payload = _builder.CreateLoad(llvm::PointerType::get(_context, 0), fieldPtr, "field.heap.payload");
            if (elemSp && typeNeedsDestructor(*elemSp)) {
                releaseAtPtr(payload, *elemSp);
            }
            _builder.CreateCall(runtime::getHeapHandleFreeFn(_module, _builder), {payload});
        } else if (fieldType.isFn()) {
            // Phase 3a / 4a-2: fn 字段：fat-ptr 的 captures（offset 1）走 _box_release_dtor，
            // 让 strong 归零时 dispatch 到 lambda 自己的 captures 字段析构。
            auto fnStructType = getLLVMType(fieldType);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto capField = _builder.CreateGEP(fnStructType, fieldPtr, {zero, one});
            auto cap = _builder.CreateLoad(llvm::PointerType::get(_context, 0), capField);
            _builder.CreateCall(runtime::getRcReleaseDtorFn(_module, _builder), {cap});
        } else if (fieldType.isDynOwned()) {
            // Phase 4b: owned Dyn<D> 字段 —— { vtable, data } 走 _dyn_release，
            // 由 vtable[0] dispatch U 的 dtor；与 releaseAtPtr 同形（避免落到下面把 "Dyn" 当 struct 名查 dtor）。
            auto dynStructType = getLLVMType(fieldType);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto vtableField = _builder.CreateGEP(dynStructType, fieldPtr, {zero, zero});
            auto vtable = _builder.CreateLoad(llvm::PointerType::get(_context, 0), vtableField);
            auto dataField = _builder.CreateGEP(dynStructType, fieldPtr, {zero, one});
            auto data = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataField);
            _builder.CreateCall(runtime::getDynReleaseFn(_module, _builder), {data, vtable});
        } else if (fieldType.isDynBorrow()) {
            // 借用 Dyn<D&>：不动 RC，等价 no-op
        } else if (fieldType.isNullable()) {
            // Phase 3d.3: Nullable<T> 字段 — 走 releaseAtPtr 内联分支
            // (覆盖 Nullable<Heap<T>> / Nullable<Rc<T>> / Nullable<String> 等),
            // 避免 fallthrough 误查 bare `Nullable_~()` dtor.
            releaseAtPtr(fieldPtr, fieldType);
        } else if (!isBuiltinType(fieldType.name)) {
            // 结构体字段: 调用其析构函数
            const string fieldKey = fieldType.isGeneric() ? fieldType.getMangleName() : fieldType.name;
            auto fieldDtorsFn = getDestructorFunction(fieldKey, fieldType.ownerModule);
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

    if (argType.isRc()) {
        auto handle = extractHandle("arg.rc.handle");
        auto retainFn = runtime::getRcRetainFn(_module, _builder);
        _builder.CreateCall(retainFn, {handle});
        return true;
    }
    if (argType.isArrayGeneric()) {
        // B-3: Array 无 RC，无需 retain（浅拷贝共享 _data，生命周期由 #NoCopy 禁隐式复制保证）
        return false;
    }
    if (argType.isWeak()) {
        auto handle = extractHandle("arg.weak.handle");
        auto retainFn = runtime::getWeakRetainFn(_module, _builder);
        _builder.CreateCall(retainFn, {handle});
        return true;
    }

    // Dyn<D> owned：fat ptr { vtable, data }，data 指向 RC block，拷贝时 retain data
    // （与字段路径 retainStructFieldsAtCallSite 的 isDynOwned 分支对齐）
    if (argType.isDynOwned()) {
        auto data = _builder.CreateExtractValue(argVal, {1}, "arg.dyn.data");
        _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {data});
        return true;
    }
    if (argType.isDynBorrow()) {
        // 借用形态不动 RC
        return false;
    }

    // Phase 3a / 4c: fn(...)R fat-ptr：retain captures（offset 1）
    // 跳过条件：null（零捕获）或 LSB=1（4c 栈嵌入 T& 捕获）
    // _box_retain 不做 null 检查，需 IR 级 guard
    if (argType.isFn()) {
        auto cap = _builder.CreateExtractValue(argVal, {1}, "arg.fn.captures");
        auto ptrTy = llvm::PointerType::get(_context, 0);
        auto i64Ty = _builder.getInt64Ty();
        auto capInt = _builder.CreatePtrToInt(cap, i64Ty, "fn.cap.asint");
        auto isStack = _builder.CreateICmpNE(_builder.CreateAnd(capInt, _builder.getInt64(1)), _builder.getInt64(0),
                                             "fn.cap.isstack");
        auto isNull = _builder.CreateICmpEQ(cap, llvm::ConstantPointerNull::get(ptrTy), "fn.cap.isnull");
        auto skip = _builder.CreateOr(isStack, isNull, "fn.cap.skip");
        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* retainBB = llvm::BasicBlock::Create(_context, "fn.cap.retain", fn);
        auto* contBB = llvm::BasicBlock::Create(_context, "fn.cap.cont", fn);
        _builder.CreateCondBr(skip, contBB, retainBB);
        _builder.SetInsertPoint(retainBB);
        _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {cap});
        _builder.CreateBr(contBB);
        _builder.SetInsertPoint(contBB);
        return true;
    }

    // Phase 3c.2.a: 含 RC 字段的非平凡 struct 按值传参，逐字段 retain；callee 析构释放
    if (!isBuiltinType(argType.name) && structNeedsDestructor(argType)) {
        retainStructFieldsAtCallSite(argVal, argType.isGeneric() ? argType.getMangleName() : argType.name);
        return true;
    }

    // Phase 5: 含 RC payload 的 enum 按值传参 —— 落 alloca 后调 __enum_retain_<E>?
    // 当前简化：通过把 argVal 拷到 alloca、按 tag dispatch 出当前 variant 的 payload
    // 字段 retain。考虑到此路径需要重做一份 switch-on-tag IR，与 dtor 高度对称，
    // 直接合成 __enum_copy_<E> 比 inline 展开更省 IR；Phase 5 先用 inline 实现，
    // copy helper 押后到后续优化。
    if (!isBuiltinType(argType.name) && enumNeedsDestructor(argType)) {
        p<FileNode> owner = nullptr;
        auto decl = names().lookupEnum(argType.name, &owner);
        if (!decl) return false;

        auto enumLLVMType = getLLVMType(argType);
        if (!enumLLVMType) return false;

        // 把 argVal（by-value struct）spill 到 alloca，便于 GEP 取 payload
        auto* fn = _builder.GetInsertBlock()->getParent();
        auto& entryBB = fn->getEntryBlock();
        llvm::IRBuilder<> entryBuilder(&entryBB, entryBB.getFirstInsertionPt());
        auto slot = entryBuilder.CreateAlloca(enumLLVMType, nullptr, "arg.enum.spill");
        _builder.CreateStore(argVal, slot);

        auto tagPtr = _builder.CreateStructGEP(enumLLVMType, slot, 0, "arg.enum.tag.ptr");
        auto tag = _builder.CreateLoad(_builder.getInt32Ty(), tagPtr, "arg.enum.tag");

        auto* mergeBB = llvm::BasicBlock::Create(_context, "arg.enum.cont", fn);

        vector<int> dispatched;
        for (size_t i = 0; i < decl->variants().size(); ++i) {
            auto v = decl->variants()[i];
            if (!v->hasPayload()) continue;
            bool any = false;
            for (auto t : v->payloadTypes()) {
                if (typeNeedsDestructor(t->getType())) {
                    any = true;
                    break;
                }
            }
            if (any) dispatched.push_back(static_cast<int>(i));
        }
        auto* sw = _builder.CreateSwitch(tag, mergeBB, dispatched.size());

        for (int idx : dispatched) {
            auto v = decl->variants()[idx];
            auto* caseBB = llvm::BasicBlock::Create(_context, "arg.enum.case." + std::to_string(idx), fn);
            sw->addCase(_builder.getInt32(idx), caseBB);
            _builder.SetInsertPoint(caseBB);

            vector<llvm::Type*> elemTys;
            elemTys.reserve(v->payloadTypes().size());
            for (auto t : v->payloadTypes()) {
                elemTys.push_back(getLLVMType(t->getType()));
            }
            auto payloadStruct = llvm::StructType::get(_context, elemTys);
            auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, slot, 1, "arg.enum.payload.ptr");

            for (size_t i = 0; i < v->payloadTypes().size(); ++i) {
                auto fieldType = v->payloadTypes()[i]->getType();
                if (!typeNeedsDestructor(fieldType)) continue;
                auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr, static_cast<unsigned>(i),
                                                         "arg.enum.payload.elem");
                // 加载字段并 retain（按字段类型分派；handle 类直接 retain，含 RC 字段 struct 递归）
                if (fieldType.isRc() || fieldType.isArrayGeneric() || fieldType.isWeak()) {
                    auto ll = getLLVMType(fieldType);
                    auto handleField = _builder.CreateStructGEP(ll, fieldPtr, 0, "arg.enum.handle.ptr");
                    auto handle =
                        _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "arg.enum.handle");
                    llvm::Function* retainFn = nullptr;
                    if (fieldType.isRc())
                        retainFn = runtime::getRcRetainFn(_module, _builder);
                    else if (fieldType.isArrayGeneric())
                        continue; // B-3: Array 无 RC，跳过 retain
                    else
                        retainFn = runtime::getWeakRetainFn(_module, _builder);
                    _builder.CreateCall(retainFn, {handle});
                } else if (!isBuiltinType(fieldType.name) && structNeedsDestructor(fieldType)) {
                    auto ll = getLLVMType(fieldType);
                    auto fieldVal = _builder.CreateLoad(ll, fieldPtr, "arg.enum.struct.val");
                    retainStructFieldsAtCallSite(fieldVal,
                                                 fieldType.isGeneric() ? fieldType.getMangleName() : fieldType.name);
                } else if (!isBuiltinType(fieldType.name) && enumNeedsDestructor(fieldType)) {
                    // 嵌套 enum：递归（极少见但形态完整）
                    auto ll = getLLVMType(fieldType);
                    auto fieldVal = _builder.CreateLoad(ll, fieldPtr, "arg.enum.nested.val");
                    retainHandleAtCallSite(fieldVal, fieldType);
                } else if (fieldType.isDynOwned()) {
                    // Dyn<D> owned payload：{ vtable, data } fat ptr，retain data
                    auto dynStructType = getLLVMType(fieldType);
                    auto dataField = _builder.CreateStructGEP(dynStructType, fieldPtr, 1, "arg.enum.dyn.data_field");
                    auto data =
                        _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataField, "arg.enum.dyn.data");
                    _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {data});
                } else if (fieldType.isDynBorrow() || fieldType.isHeap()) {
                    // Dyn<D&> borrow / Heap<T> payload：不动 RC（借用不持有，Heap 所有权转移不深拷）
                }
            }
            _builder.CreateBr(mergeBB);
        }

        _builder.SetInsertPoint(mergeBB);
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

        if (ft.isRc() || ft.isArrayGeneric() || ft.isWeak()) {
            // 取字段值（{ ptr handle } struct），再取 handle
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.val");
            auto handle = _builder.CreateExtractValue(fieldVal, {0}, "field.handle");
            llvm::Function* retainFn = nullptr;
            if (ft.isRc())
                retainFn = runtime::getRcRetainFn(_module, _builder);
            else if (ft.isArrayGeneric())
                continue; // B-3: Array 无 RC，跳过 retain
            else
                retainFn = runtime::getWeakRetainFn(_module, _builder);
            _builder.CreateCall(retainFn, {handle});
        } else if (ft.isFn()) {
            // Phase 3a: fn 字段 fat-ptr，按 captures 字段 retain（null guard）
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.fn");
            auto cap = _builder.CreateExtractValue(fieldVal, {1}, "field.fn.captures");
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto isNull = _builder.CreateICmpEQ(cap, llvm::ConstantPointerNull::get(ptrTy), "field.fn.isnull");
            auto* fn = _builder.GetInsertBlock()->getParent();
            auto* retainBB = llvm::BasicBlock::Create(_context, "field.fn.retain", fn);
            auto* contBB = llvm::BasicBlock::Create(_context, "field.fn.cont", fn);
            _builder.CreateCondBr(isNull, contBB, retainBB);
            _builder.SetInsertPoint(retainBB);
            _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {cap});
            _builder.CreateBr(contBB);
            _builder.SetInsertPoint(contBB);
        } else if (ft.isDynOwned()) {
            // Dyn<D> owned 字段：{ vtable, data } fat ptr，data 指向 RC block，需 retain
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.dyn");
            auto data = _builder.CreateExtractValue(fieldVal, {1}, "field.dyn.data");
            _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {data});
        } else if (ft.isDynBorrow() || ft.isHeap()) {
            // Dyn<D&> 借用 / Heap<T> 字段：不动 RC（借用不持有，Heap 所有权转移不深拷）
        } else if (!isBuiltinType(ft.name)) {
            // 嵌套 struct 字段：递归
            auto fieldVal = _builder.CreateExtractValue(argVal, {static_cast<unsigned>(i)}, "field.struct");
            retainStructFieldsAtCallSite(fieldVal, ft.isGeneric() ? ft.getMangleName() : ft.name);
        }
    }
}

// copy_of 专用：深拷 struct 所有字段（含 Heap 新分配 + 替换指针）
// Rc/Weak/fn 字段的 retain 仍由 retainHandleAtCallSite 处理；
// 本函数只负责 Heap 字段的深拷（需要新分配内存，不能走 retain 路径）。
// 递归进入嵌套 struct/enum 处理其中的 Heap 字段。
llvm::Value* Compiler::copyOfStructFields(llvm::Value* structVal, const string& structName) {
    auto fieldTypes = resolveStructFieldTypes(structName);
    for (size_t i = 0; i < fieldTypes.size(); ++i) {
        const auto& ft = fieldTypes[i];
        if (!typeNeedsDestructor(ft)) continue;

        if (ft.isHeap()) {
            // Heap<T> 字段：新分配 + memcpy 旧值 + retain inner RC + 替换指针
            auto elemSp = ft.heapElementType();
            if (!elemSp) continue;
            const auto& innerType = *elemSp;
            auto innerLLVMType = getLLVMType(innerType);
            auto oldPayload = _builder.CreateExtractValue(structVal, {static_cast<unsigned>(i)}, "cof.heap.old");
            auto sizeVal = _builder.getInt64(_module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
            auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
            auto newPayload = _builder.CreateCall(allocFn, {sizeVal}, "cof.heap.new");
            auto oldInner = _builder.CreateLoad(innerLLVMType, oldPayload, "cof.heap.oldval");
            _builder.CreateStore(oldInner, newPayload);
            // 递归 retain inner 的 RC/Dyn 字段
            retainHandleAtCallSite(oldInner, innerType);
            // 替换 struct 中的 Heap 指针
            structVal =
                _builder.CreateInsertValue(structVal, newPayload, {static_cast<unsigned>(i)}, "cof.heap.inserted");
        } else if (!isBuiltinType(ft.name) && structNeedsDestructor(ft)) {
            // 嵌套 struct：递归处理其中的 Heap 字段
            auto fieldVal = _builder.CreateExtractValue(structVal, {static_cast<unsigned>(i)}, "cof.struct");
            auto newFieldVal = copyOfStructFields(fieldVal, ft.isGeneric() ? ft.getMangleName() : ft.name);
            if (newFieldVal != fieldVal) {
                structVal = _builder.CreateInsertValue(structVal, newFieldVal, {static_cast<unsigned>(i)},
                                                       "cof.struct.inserted");
            }
        }
        // Rc/Weak/fn/Dyn/Array：已由 retainHandleAtCallSite 处理，这里跳过
    }
    return structVal;
}

// ==================== Phase 8d.1: per-statement 临时清单 ====================

// 在新语句入口 push 一个空帧
void Compiler::pushTempFrame() {
    _tempStack.emplace_back();
}

// 弹出顶帧；对其中未消费的 fresh RC 句柄发出 release（顺序无关，统一在帧末尾）
// 覆盖 Rc/Array/Weak（单 handle by-value）+ 含 RC 字段 struct value（Phase 8d.4，靠 spillSlot dtor）。
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
        if (t.type.isRc()) {
            auto handle = _builder.CreateExtractValue(t.val, {0}, "temp.rc.handle");
            // Phase B-2: 若内层 T 需析构，用 typed release
            auto releaseFn = getOrCreateRcTypedReleaseFn(t.type);
            _builder.CreateCall(releaseFn, {handle});
        } else if (t.type.isArrayGeneric()) {
            // B-3: Array 临时值析构 = free _data buffer
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto arrAlloca = _builder.CreateAlloca(getLLVMType(t.type), nullptr, "temp.arr");
            _builder.CreateStore(t.val, arrAlloca);
            releaseAtPtr(arrAlloca, t.type);
        } else if (t.type.isWeak()) {
            auto handle = _builder.CreateExtractValue(t.val, {0}, "temp.weak.handle");
            _builder.CreateCall(runtime::getWeakReleaseFn(_module, _builder), {handle});
        } else if (t.type.isFn()) {
            // fn fat-ptr 的 captures（offset 1）走 _box_release_dtor；
            // 与 releaseAtPtr 的 fn 分支同款逻辑（null / stack-embedded 跳过）
            auto cap = _builder.CreateExtractValue(t.val, {1}, "temp.fn.captures");
            auto i64Ty = _builder.getInt64Ty();
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto capInt = _builder.CreatePtrToInt(cap, i64Ty, "temp.fn.cap.asint");
            auto isStack = _builder.CreateICmpNE(_builder.CreateAnd(capInt, _builder.getInt64(1)), _builder.getInt64(0),
                                                 "temp.fn.isstack");
            auto isNull = _builder.CreateICmpEQ(cap, llvm::ConstantPointerNull::get(ptrTy), "temp.fn.isnull");
            auto skip = _builder.CreateOr(isStack, isNull, "temp.fn.skip");
            auto* pf = _builder.GetInsertBlock()->getParent();
            auto* relBB = llvm::BasicBlock::Create(_context, "temp.fn.rel", pf);
            auto* contBB = llvm::BasicBlock::Create(_context, "temp.fn.cont", pf);
            _builder.CreateCondBr(skip, contBB, relBB);
            _builder.SetInsertPoint(relBB);
            _builder.CreateCall(runtime::getRcReleaseDtorFn(_module, _builder), {cap});
            _builder.CreateBr(contBB);
            _builder.SetInsertPoint(contBB);
        } else if (t.spillSlot) {
            // Phase 8d.4: 含 RC 字段 struct value：调其析构（按字段逆序 release）
            releaseAtPtr(t.spillSlot, t.type);
        }
    }
}

// 记录一个 fresh RC 临时到顶帧
// - Rc/Array/Weak/fn: 直接保存 by-value struct {ptr handle}，pop 时 extractValue 取 handle
// - 含 RC 字段 struct (e.g. String): 入 entry-block alloca 留 dtor 用，pop 时调 releaseAtPtr
void Compiler::recordTemp(llvm::Value* val, const TypeInfo& type) {
    if (!val) return;
    if (_tempStack.empty()) return;
    if (type.isRc() || type.isArrayGeneric() || type.isWeak() || type.isFn()) {
        _tempStack.back().push_back({.val = val, .type = type, .spillSlot = nullptr});
        return;
    }
    // Phase 8d.4: 含 RC 字段的 struct value（如 String）—— 落 entry 块 alloca，由 releaseAtPtr/dtor 释放
    // Phase 5 扩展：含 RC payload 的 enum 值同样按值持有 RC 句柄，按 tag dispatch dtor
    if (type.isRef() || type.isPtr()) return;
    if (isBuiltinType(type.name)) return;
    if (!structNeedsDestructor(type) && !enumNeedsDestructor(type)) return;

    auto* fn = _builder.GetInsertBlock()->getParent();
    auto& entryBB = fn->getEntryBlock();
    llvm::IRBuilder<> entryBuilder(&entryBB, entryBB.getFirstInsertionPt());
    auto slot = entryBuilder.CreateAlloca(getLLVMType(type), nullptr, "temp.struct.spill");
    _builder.CreateStore(val, slot);
    _tempStack.back().push_back({.val = val, .type = type, .spillSlot = slot});
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
    bool isRcHandle = expectedType.isRcHandle();
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

// Phase 8d.3: 对一个已存在的 RC 句柄 by-value（Rc/Array/Weak struct value）发 retain。
// 调用前 IR 插入点必须 dominate val。用于分支汇合时把"借用结果"归一为"fresh +1"。
void Compiler::emitRetainOnHandleValue(llvm::Value* val, const TypeInfo& type) {
    if (!val) return;
    if (type.isRc()) {
        auto handle = _builder.CreateExtractValue(val, {0}, "merge.rc.handle");
        _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {handle});
    } else if (type.isArrayGeneric()) {
        // B-3: Array 无 RC，跳过 retain
    } else if (type.isString()) {
        retainHandleAtCallSite(val, type);
    } else if (type.isWeak()) {
        auto handle = _builder.CreateExtractValue(val, {0}, "merge.weak.handle");
        _builder.CreateCall(runtime::getWeakRetainFn(_module, _builder), {handle});
    }
}

// ==================== Phase 8b: fresh 表达式判定 ====================

// 识别 +1 所有权（fresh）表达式：调用结果（函数 / 方法 / 构造器）+ 数组字面量 + move-assign + lambda
// 用于在复制语义 retain 路径上跳过多余 retain，避免 leak（DRAFT §7.6 / §8）
// !! 与 sema_pass_detail.cpp::isFreshHandleExpr 须保持同步 — 新增 case 需两边同时添加 !!
bool Compiler::isFreshHandleExpr(p<ExprNode> expr) {
    if (!expr) return false;
    if (dynamic_cast<ExprCallNode*>(expr)) return true;
    if (dynamic_cast<ExprArrayNode*>(expr)) return true;
    // Phase 5: enum 构造把实参 +1 句柄收纳到 enum 值，结果是 +1 fresh
    if (dynamic_cast<ExprPathCallNode*>(expr)) return true;
    // B-4: move-assign (a <- b) 移出旧值，返回 +1 fresh
    if (dynamic_cast<ExprMoveAssignNode*>(expr)) return true;
    // Phase 8f: lambda 字面量创建 captures Rc 块（strong=1），是 +1 fresh
    if (dynamic_cast<LambdaExprNode*>(expr)) return true;
    // B-1: struct 字面量 Self { ... } 创建新的 struct 值，是 +1 fresh
    if (dynamic_cast<ExprStructLitNode*>(expr)) return true;
    // if / match / try：phi 各值产生分支均 fresh 时整体 fresh（流终止臂不参与）。
    // Array 无 RC，不能靠 retain 归一；非 fresh 分支仍须 E4031。
    auto blockFresh = [this](p<StatementBlockNode> block) -> bool {
        if (!block || blockTerminatesFlow(block, block)) return true;
        if (!block->hasResult() || !block->resultExpr()) return true;
        return isFreshHandleExpr(block->resultExpr());
    };
    if (auto* ifn = dynamic_cast<ExprIfElseNode*>(expr)) {
        if (!blockFresh(ifn->thenBlock())) return false;
        for (auto& el : ifn->elifs()) {
            if (el && !blockFresh(el->block())) return false;
        }
        if (ifn->elseBlock() && !blockFresh(ifn->elseBlock())) return false;
        return true;
    }
    if (auto* ol = dynamic_cast<ExprOneLineIfElseNode*>(expr)) {
        p<ScopeNode> sc = ol->findNearestScope();
        bool tTerm = exprTerminatesFlow(sc, ol->trueValue());
        bool fTerm = exprTerminatesFlow(sc, ol->falseValue());
        if (tTerm && fTerm) return true;
        if (tTerm) return isFreshHandleExpr(ol->falseValue());
        if (fTerm) return isFreshHandleExpr(ol->trueValue());
        return isFreshHandleExpr(ol->trueValue()) && isFreshHandleExpr(ol->falseValue());
    }
    if (auto* mn = dynamic_cast<ExprMatchNode*>(expr)) {
        for (auto& arm : mn->arms()) {
            if (!arm || arm->skipsTypeMerge()) continue;
            if (arm->hasBlock()) {
                if (!blockFresh(arm->block())) return false;
            } else if (!isFreshHandleExpr(arm->body())) {
                return false;
            }
        }
        return true;
    }
    if (auto* tn = dynamic_cast<ExprTryCatchNode*>(expr)) {
        if (!blockFresh(tn->tryBlock())) return false;
        for (auto& arm : tn->catches()) {
            if (arm && !blockFresh(arm->body())) return false;
        }
        return true;
    }
    // `??`：两侧都 fresh 时整体 fresh（`give() ?? []`）；变量左侧仍是复制，E4031。
    if (auto* ne = dynamic_cast<ExprNullElseNode*>(expr)) {
        return isFreshHandleExpr(ne->left()) && isFreshHandleExpr(ne->right());
    }
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

    // Rc / Weak / Array 需要析构
    if (type.isRc() || type.isWeak() || type.isArrayGeneric()) return true;

    // Heap<T>：作用域尾走 __yux_heap_free（DRAFT-heap-types §8.3a）
    if (type.isHeap()) return true;

    // Nullable<T>：若内层 T 需析构（Rc/Weak/Array/Heap/String/含 RC 字段 struct/enum），
    // 则 Nullable<T> 也需析构——_has=true 时调内层 T 的析构释放
    if (type.isNullable()) {
        auto inner = type.nullableInnerType();
        if (inner && (inner->isHeap() || typeNeedsDestructor(*inner))) return true;
    }

    // Phase 3a: 函数类型 fn(...)R 的 captures 字段是 Rc<CapturesT>?，按 §7.4 字段级 RC
    // 即使零捕获场景下 captures 永远 null，IR 仍发出 retain/release（runtime null-safe）
    if (type.isFn()) return true;

    // Phase 3e (DRAFT-dyn-draft §12.9): owned Dyn<D> 走 _dyn_release 析构；
    // Dyn<D&> 借用形态不动 RC，但仍标记需要析构以便走 releaseAtPtr 的 no-op 分支统一帧管理
    if (type.isDyn()) return true;

    // Phase 5: enum 类型若任一 variant 含 RC payload 字段则需析构
    if (enumNeedsDestructor(type)) return true;

    // 检查结构体是否需要析构
    return structNeedsDestructor(type);
}

// Phase 3c.2.a/c: 用户 struct（普通 + 泛型实例）一律 by-value
// 仅 _structTypes 已注册但找不到声明的跨模块 struct：保守按指针
// 必须传完整 TypeInfo：从裸名构造会丢掉 genericArgs，Array/Rc 变成错误 kind。
bool Compiler::structParamUsesPointer(const TypeInfo& ti) {
    if (ti.isPtr() || ti.isRef()) return false;
    if (ti.isArrayGeneric()) return true;
    if (isBuiltinType(ti.name)) return false;

    // Decl 查找只用基名（Generic `Foo` / 防御 `Foo<i32>` 被塞进 name）；不作类型身份
    const string declName = ti.baseStructName();
    auto structDecl = _file->getStructDecl(declName);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(declName);
    }
    if (structDecl) return false;

    // 泛型实例 → by-value（3c.2.c）；实例 key 走 mangle，不走裸 name
    const string instKey = ti.getMangleName();
    if (_structInstances.find(instKey) != _structInstances.end()) return false;

    // 仅在 LLVM 类型表中注册的（跨模块未通配导入等）保守按指针
    if (_structTypes.find(instKey) != _structTypes.end()) {
        return true;
    }

    return false;
}

// 检查结构体是否需要析构函数
// 两种情况需要析构：
//   1. 有显式 fn ~() 析构函数 —— 即使所有字段都是标量
//   2. 任何字段需要析构 —— 默认析构或字段级 release
// 普通 struct 走 fields()；泛型实例走 baseDecl + 实例 args 替换
bool Compiler::structNeedsDestructor(const string& structName) {
    // 检查是否有显式析构函数 fn ~()
    auto* impl = _file->getStructImpl(structName);
    if (!impl && _yux && _yux->sdkFile()) {
        impl = _yux->sdkFile()->getStructImpl(structName);
    }
    if (impl && impl->hasDestructor()) return true;

    // 检查字段是否需要析构
    auto fieldTypes = resolveStructFieldTypes(structName);
    for (const auto& ft : fieldTypes) {
        if (typeNeedsDestructor(ft)) return true;
    }
    return false;
}

bool Compiler::structNeedsDestructor(const TypeInfo& type) {
    // Array<T> 有 _data 所有权，kind 即身份；不走裸名 "Array"
    if (type.isArrayGeneric()) return true;
    if (type.isGeneric()) {
        const string instKey = type.getMangleName();
        if (_structInstances.find(instKey) != _structInstances.end()) {
            return structNeedsDestructor(instKey);
        }
        return structNeedsDestructor(type.baseStructName());
    }
    return structNeedsDestructor(type.name);
}

// ==================== Phase B-2: Rc<T> 特化释放函数 ====================

// 获取或创建 Rc<T> 的 typed release 函数。
// 若 rcType 内层 T 无需析构，直接返回 generic _box_release。
// 否则生成特化版 _box_release_T：strong==0 时先调 T::~() 再走 weak/free。
llvm::Function* Compiler::getOrCreateRcTypedReleaseFn(const TypeInfo& rcType) {
    if (!rcType.isRc()) return runtime::getRcReleaseFn(_module, _builder);

    auto inner = rcType.rcElementType();
    if (!inner || !typeNeedsDestructor(*inner)) {
        // T 平凡：直接用 generic _box_release
        return runtime::getRcReleaseFn(_module, _builder);
    }

    // B-4: Array<T> 内层 → 生成 typed release，在 strong==0 时内联 _array_free_data
    if (inner->isArrayGeneric()) {
        // 构造 mangled name
        string arrInnerName = inner->arrayGenericElementType() ? inner->arrayGenericElementType()->name : "T";
        string mangledName = "__yux_box_release.Array." + arrInnerName;
        auto func = runtime::getRcReleaseTypedFn(_module, _builder, mangledName);
        if (func->empty()) {
            auto* savedBB = _builder.GetInsertBlock();
            auto savedIP = savedBB ? _builder.GetInsertPoint() : llvm::BasicBlock::iterator();
            runtime::emitRcReleaseForArrayFn(_context, _builder, _module, func);
            if (savedBB) {
                _builder.SetInsertPoint(savedBB, savedIP);
            }
        }
        return func;
    }

    // Rc<U> / Weak<U> / fn(...) 无独立 dtor 函数，由 typed release 内联生成 dtor IR
    if (inner->isRc()) {
        string mangledName = "__yux_box_release.Rc." + inner->rcElementType()->getMangleName();
        auto func = runtime::getRcReleaseTypedFn(_module, _builder, mangledName);
        if (func->empty()) {
            auto* savedBB = _builder.GetInsertBlock();
            auto savedIP = savedBB ? _builder.GetInsertPoint() : llvm::BasicBlock::iterator();
            // 内层可能仍是 Rc<...>，必须走 typed release，否则 Rc<Rc<Rc<T>>> 最内层泄漏
            auto innerReleaseFn = getOrCreateRcTypedReleaseFn(*inner);
            runtime::emitRcReleaseForInlineDtorFn(_context, _builder, _module, func, TypeKind::Rc, innerReleaseFn);
            if (savedBB) _builder.SetInsertPoint(savedBB, savedIP);
        }
        return func;
    }
    if (inner->isWeak()) {
        string mangledName = "__yux_box_release.Weak." + inner->weakElementType()->getMangleName();
        auto func = runtime::getRcReleaseTypedFn(_module, _builder, mangledName);
        if (func->empty()) {
            auto* savedBB = _builder.GetInsertBlock();
            auto savedIP = savedBB ? _builder.GetInsertPoint() : llvm::BasicBlock::iterator();
            runtime::emitRcReleaseForInlineDtorFn(_context, _builder, _module, func, TypeKind::Weak,
                                                  runtime::getWeakReleaseFn(_module, _builder));
            if (savedBB) _builder.SetInsertPoint(savedBB, savedIP);
        }
        return func;
    }
    if (inner->isFn()) {
        string mangledName = "__yux_box_release.Fn." + inner->getMangleName();
        auto func = runtime::getRcReleaseTypedFn(_module, _builder, mangledName);
        if (func->empty()) {
            auto* savedBB = _builder.GetInsertBlock();
            auto savedIP = savedBB ? _builder.GetInsertPoint() : llvm::BasicBlock::iterator();
            runtime::emitRcReleaseForInlineDtorFn(_context, _builder, _module, func, TypeKind::Fn,
                                                  runtime::getRcReleaseFn(_module, _builder));
            if (savedBB) _builder.SetInsertPoint(savedBB, savedIP);
        }
        return func;
    }

    // Rc<Heap<T>> / Rc<Dyn<D>> 已由 getLLVMType 拒绝（E4025 / E1132，含别名展开与
    // 泛型 subst、以及 Rc<Rc<Heap<T>>> 递归）。本函数不可达这两类内层。
    // 禁止回退 generic _box_release：那会跳过 HeapFree / Dyn vtable dtor，造成泄漏。
    if (inner->isHeap()) {
        // E4025 由 SemaPass / getLLVMType 先抛。
        throwSemaGap(1);
    }
    if (inner->isDyn()) {
        // E1132 由 SemaPass / getLLVMType 先抛。
        throwSemaGap(1);
    }

    // 构造 mangled name：__yux_box_release.T.<type>
    string mangledName = "__yux_box_release.T." + inner->getMangleName();

    auto func = runtime::getRcReleaseTypedFn(_module, _builder, mangledName);
    if (!func->empty()) return func;

    // 获取 T 的析构函数
    auto dtorFn = getDestructorFunction(inner->isGeneric() ? inner->getMangleName() : inner->name, inner->ownerModule);

    // 保存当前插入点
    auto* savedBB = _builder.GetInsertBlock();
    auto savedIP = savedBB ? _builder.GetInsertPoint() : llvm::BasicBlock::iterator();

    // 生成函数体
    runtime::emitRcReleaseTypedFn(_context, _builder, _module, func, dtorFn);

    // 恢复插入点
    if (savedBB) {
        _builder.SetInsertPoint(savedBB, savedIP);
    }

    return func;
}

// Phase B-1: 检查类型是否是 #NoCopy struct（含显式 #NoCopy 注解 + 隐含析构）
bool Compiler::isNoCopyType(const TypeInfo& type) const {
    if (isBuiltinType(type.name)) return false;
    if (type.isRc() || type.isWeak() || type.isHeap()) return false;
    if (type.isRef() || type.isPtr()) return false;

    // B-3: Array<T> 去 Builtin 后为 #NoCopy（有 fn ~()，含 _data 所有权）
    if (type.isArrayGeneric()) return true;

    // 查 local struct decl 的显式 #NoCopy 注解（按基名，与 SemaPass::isNoCopyTypeIn 对齐）
    const string baseName = type.baseStructName();
    auto* decl = _file ? _file->getStructDecl(baseName) : nullptr;
    if (!decl && _yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
        decl = _yux->sdkFile()->getStructDecl(baseName);
    }
    if (decl && decl->hasAnno("NoCopy")) return true;

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

// ==================== Phase 5: 枚举析构 ====================

// 任一 variant 的 payload 元素需析构则枚举需析构
bool Compiler::enumDeclNeedsDestructor(p<EnumDeclNode> decl) {
    if (!decl) return false;
    for (auto v : decl->variants()) {
        if (!v->hasPayload()) continue;
        for (auto t : v->payloadTypes()) {
            if (typeNeedsDestructor(t->getType())) return true;
        }
    }
    return false;
}

// 按名查 enum 决议是否需析构；非 enum 名返回 false
bool Compiler::enumNeedsDestructor(const string& enumName) {
    if (enumName.empty()) return false;
    p<FileNode> owner = nullptr;
    auto decl = names().lookupEnum(enumName, &owner);
    if (!decl) return false;
    return enumDeclNeedsDestructor(decl);
}

bool Compiler::enumNeedsDestructor(const TypeInfo& type) {
    if (!type.isNormal()) return false;
    p<FileNode> owner = nullptr;
    auto decl = names().lookupEnum(type, &owner);
    if (!decl) return false;
    return enumDeclNeedsDestructor(decl);
}

// 获取或创建 enum dtor 声明（mangled 含 owner 模块名）
// 与 struct dtor 同模型：`mod.Enum::~()`；有 owner 时不按短名找错模块
llvm::Function* Compiler::getEnumDestructorFunction(const string& enumName, string ownerModuleHint) {
    p<FileNode> owner = nullptr;
    EnumDeclNode* decl = nullptr;
    if (!ownerModuleHint.empty() && _yux) {
        owner = _yux->module(ownerModuleHint);
        if (owner) decl = owner->localEnumDecl(enumName);
    }
    if (!decl) decl = names().lookupEnum(enumName, &owner);
    if (!decl) return nullptr;

    string ownerModule =
        !ownerModuleHint.empty() ? ownerModuleHint : (owner ? owner->moduleName() : _file->moduleName());
    string mangled = Mangler::dtor(ownerModule, enumName);

    auto func = _module->getFunction(mangled);
    if (func) return func;

    vector<llvm::Type*> params;
    params.push_back(llvm::PointerType::get(_context, 0));
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), params, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangled, _module);
}

// 合成 __enum_drop_<E>(p*) 实现：switch on tag → 各 case 释放对应 variant 的 RC payload 字段
// 全 POD enum 不会进到这里（compileEnumDtors 提前过滤）
void Compiler::generateEnumDestructor(p<EnumDeclNode> decl, p<FileNode> owner) {
    if (!decl) return;
    string enumName = decl->name().getText();
    DEBUG_LOG_VAL("  Generating enum dtor for", enumName);

    auto fn = getEnumDestructorFunction(enumName, owner ? owner->moduleName() : "");
    if (!fn || !fn->empty()) return; // 已有定义则不重复

    TypeInfo enumTy(enumName);
    if (owner) enumTy.ownerModule = owner->moduleName();
    auto enumLLVMType = getLLVMType(enumTy);
    if (!enumLLVMType) return;

    // 保存当前插入点（compileEnumDtors 在主流水线中可能已设过）
    auto savedBB = _builder.GetInsertBlock();
    auto savedIP = _builder.GetInsertPoint();

    auto entry = llvm::BasicBlock::Create(_context, "entry", fn);
    auto exitBB = llvm::BasicBlock::Create(_context, "exit", fn);
    _builder.SetInsertPoint(entry);

    auto thisArg = &*fn->arg_begin();
    auto tagPtr = _builder.CreateStructGEP(enumLLVMType, thisArg, 0, "tag.ptr");
    auto tag = _builder.CreateLoad(_builder.getInt32Ty(), tagPtr, "tag");

    // 收集所有需析构的 variant；其余 variant（无 payload 或全 POD）不进 switch
    vector<int> dispatchedIndices;
    for (size_t i = 0; i < decl->variants().size(); ++i) {
        auto v = decl->variants()[i];
        if (!v->hasPayload()) continue;
        bool any = false;
        for (auto t : v->payloadTypes()) {
            if (typeNeedsDestructor(t->getType())) {
                any = true;
                break;
            }
        }
        if (any) dispatchedIndices.push_back(static_cast<int>(i));
    }

    auto sw = _builder.CreateSwitch(tag, exitBB, dispatchedIndices.size());

    for (int idx : dispatchedIndices) {
        auto v = decl->variants()[idx];
        auto caseBB = llvm::BasicBlock::Create(_context, "case." + std::to_string(idx), fn);
        sw->addCase(_builder.getInt32(idx), caseBB);
        _builder.SetInsertPoint(caseBB);

        // 重建 variant 的 tuple struct 类型（与 ctor 路径一致）
        vector<llvm::Type*> elemTys;
        elemTys.reserve(v->payloadTypes().size());
        for (auto t : v->payloadTypes()) {
            elemTys.push_back(getLLVMType(t->getType()));
        }
        auto payloadStruct = llvm::StructType::get(_context, elemTys);
        auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, thisArg, 1, "payload.ptr");

        // 按声明逆序释放（与 struct 字段释放约定一致）
        for (size_t k = v->payloadTypes().size(); k > 0; --k) {
            size_t i = k - 1;
            auto fieldType = v->payloadTypes()[i]->getType();
            if (!typeNeedsDestructor(fieldType)) continue;
            auto fieldPtr =
                _builder.CreateStructGEP(payloadStruct, payloadBufPtr, static_cast<unsigned>(i), "payload.elem");
            releaseAtPtr(fieldPtr, fieldType);
        }
        _builder.CreateBr(exitBB);
    }

    _builder.SetInsertPoint(exitBB);
    _builder.CreateRetVoid();

    // 恢复插入点（避免污染调用方上下文）
    if (savedBB && !savedBB->getTerminator()) {
        _builder.SetInsertPoint(savedBB, savedIP);
    } else if (savedBB) {
        _builder.SetInsertPoint(savedBB);
    }
}

// 主流水线：为本 file 的每个 enum 声明（若需析构）发射 dtor 定义
void Compiler::compileEnumDtors() {
    auto& enums = _file->getEnumDecls();
    DEBUG_LOG_VAL("  compileEnumDtors", enums.size() << " enums");
    for (auto decl : enums) {
        if (!enumDeclNeedsDestructor(decl)) continue;
        generateEnumDestructor(decl, _file);
    }
}

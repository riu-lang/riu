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
#include "ast/mangler.h"
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
        _builder.CreateCall(runtime::getRcReleaseFn(_module, _builder), {handle});
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
        auto isStack = _builder.CreateICmpNE(
            _builder.CreateAnd(capInt, _builder.getInt64(1)), _builder.getInt64(0), "old.fn.cap.isstack");
        auto isNull = _builder.CreateICmpEQ(cap, llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)), "old.fn.cap.isnull");
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
    if (enumNeedsDestructor(type.name)) {
        auto dtorFn = getEnumDestructorFunction(type.name);
        if (dtorFn) {
            _builder.CreateCall(dtorFn, {slotPtr});
        }
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
        if (fieldType.isRc()) {
            // Rc 字段：load handle，调用 _box_release(handle)
            auto rcStructType = getLLVMType(fieldType);
            auto handleField = _builder.CreateGEP(rcStructType, fieldPtr, {zero, zero});
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField);

            auto rcReleaseFn = runtime::getRcReleaseFn(_module, _builder);
            _builder.CreateCall(rcReleaseFn, {handle});
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

    if (argType.isRc()) {
        auto handle = extractHandle("arg.rc.handle");
        auto retainFn = runtime::getRcRetainFn(_module, _builder);
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

    // Phase 3a / 4c: fn(...)R fat-ptr：retain captures（offset 1）
    // 跳过条件：null（零捕获）或 LSB=1（4c 栈嵌入 T& 捕获）
    // _box_retain 不做 null 检查，需 IR 级 guard
    if (argType.isFn()) {
        auto cap = _builder.CreateExtractValue(argVal, {1}, "arg.fn.captures");
        auto ptrTy = llvm::PointerType::get(_context, 0);
        auto i64Ty = _builder.getInt64Ty();
        auto capInt = _builder.CreatePtrToInt(cap, i64Ty, "fn.cap.asint");
        auto isStack = _builder.CreateICmpNE(
            _builder.CreateAnd(capInt, _builder.getInt64(1)), _builder.getInt64(0), "fn.cap.isstack");
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
    if (!isBuiltinType(argType.name) && structNeedsDestructor(argType.name)) {
        retainStructFieldsAtCallSite(argVal, argType.name);
        return true;
    }

    // Phase 5: 含 RC payload 的 enum 按值传参 —— 落 alloca 后调 __enum_retain_<E>?
    // 当前简化：通过把 argVal 拷到 alloca、按 tag dispatch 出当前 variant 的 payload
    // 字段 retain。考虑到此路径需要重做一份 switch-on-tag IR，与 dtor 高度对称，
    // 直接合成 __enum_copy_<E> 比 inline 展开更省 IR；Phase 5 先用 inline 实现，
    // copy helper 押后到后续优化。
    if (!isBuiltinType(argType.name) && enumNeedsDestructor(argType.name)) {
        p<FileNode> owner = nullptr;
        auto decl = lookupEnumDecl(argType.name, owner);
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
                if (typeNeedsDestructor(t->getType())) { any = true; break; }
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
                auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr,
                    static_cast<unsigned>(i), "arg.enum.payload.elem");
                // 加载字段并 retain（按字段类型分派；handle 类直接 retain，含 RC 字段 struct 递归）
                if (fieldType.isRc() || fieldType.isArrayGeneric() || fieldType.isWeak()) {
                    auto ll = getLLVMType(fieldType);
                    auto handleField = _builder.CreateStructGEP(ll, fieldPtr, 0, "arg.enum.handle.ptr");
                    auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "arg.enum.handle");
                    llvm::Function* retainFn = nullptr;
                    if (fieldType.isRc()) retainFn = runtime::getRcRetainFn(_module, _builder);
                    else if (fieldType.isArrayGeneric()) retainFn = runtime::getArrayRetainFn(_module, _builder);
                    else retainFn = runtime::getWeakRetainFn(_module, _builder);
                    _builder.CreateCall(retainFn, {handle});
                } else if (!isBuiltinType(fieldType.name) && structNeedsDestructor(fieldType.name)) {
                    auto ll = getLLVMType(fieldType);
                    auto fieldVal = _builder.CreateLoad(ll, fieldPtr, "arg.enum.struct.val");
                    retainStructFieldsAtCallSite(fieldVal, fieldType.name);
                } else if (!isBuiltinType(fieldType.name) && enumNeedsDestructor(fieldType.name)) {
                    // 嵌套 enum：递归（极少见但形态完整）
                    auto ll = getLLVMType(fieldType);
                    auto fieldVal = _builder.CreateLoad(ll, fieldPtr, "arg.enum.nested.val");
                    retainHandleAtCallSite(fieldVal, fieldType);
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
            if (ft.isRc()) retainFn = runtime::getRcRetainFn(_module, _builder);
            else if (ft.isArrayGeneric()) retainFn = runtime::getArrayRetainFn(_module, _builder);
            else retainFn = runtime::getWeakRetainFn(_module, _builder);
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
            _builder.CreateCall(runtime::getRcReleaseFn(_module, _builder), {handle});
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
// - Rc/Array/Weak: 直接保存 by-value struct {ptr handle}，pop 时 extractValue 取 handle
// - 含 RC 字段 struct (e.g. String): 入 entry-block alloca 留 dtor 用，pop 时调 releaseAtPtr
void Compiler::recordTemp(llvm::Value* val, const TypeInfo& type) {
    if (!val) return;
    if (_tempStack.empty()) return;
    if (type.isRc() || type.isArrayGeneric() || type.isWeak()) {
        _tempStack.back().push_back({val, type, nullptr});
        return;
    }
    // Phase 8d.4: 含 RC 字段的 struct value（如 String）—— 落 entry 块 alloca，由 releaseAtPtr/dtor 释放
    // Phase 5 扩展：含 RC payload 的 enum 值同样按值持有 RC 句柄，按 tag dispatch dtor
    if (type.isRef() || type.isPtr()) return;
    if (isBuiltinType(type.name)) return;
    if (!structNeedsDestructor(type.name) && !enumNeedsDestructor(type.name)) return;

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
    bool isRcHandle = expectedType.isRc() || expectedType.isArrayGeneric() || expectedType.isWeak();
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
    // Phase 5: enum 构造把实参 +1 句柄收纳到 enum 值，结果是 +1 fresh
    if (dynamic_cast<ExprPathCallNode*>(expr)) return true;
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

    // Phase 3a: 函数类型 fn(...)R 的 captures 字段是 Rc<CapturesT>?，按 §7.4 字段级 RC
    // 即使零捕获场景下 captures 永远 null，IR 仍发出 retain/release（runtime null-safe）
    if (type.isFn()) return true;

    // Phase 3e (DRAFT-dyn-draft §12.9): owned Dyn<D> 走 _dyn_release 析构；
    // Dyn<D&> 借用形态不动 RC，但仍标记需要析构以便走 releaseAtPtr 的 no-op 分支统一帧管理
    if (type.isDyn()) return true;

    // Phase 5: enum 类型若任一 variant 含 RC payload 字段则需析构
    if (enumNeedsDestructor(type.name)) return true;

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
    auto decl = lookupEnumDecl(enumName, owner);
    if (!decl) return false;
    return enumDeclNeedsDestructor(decl);
}

// 获取或创建 __enum_drop_<E> 函数声明（mangled 含 owner 模块名）
// 与 struct dtor 同模型：定义只在 owner 模块发射，consumer 拿到 extern decl
llvm::Function* Compiler::getEnumDestructorFunction(const string& enumName) {
    p<FileNode> owner = nullptr;
    auto decl = lookupEnumDecl(enumName, owner);
    if (!decl) return nullptr;

    string ownerModule = owner ? owner->moduleName() : _file->moduleName();
    // mangling: Enum$<module>$<name>_~()  与 struct dtor 形态一致（仅替换前缀为 Enum$）
    string mangled = "Enum$" + ownerModule + "$" + enumName + "_~()";

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

    auto fn = getEnumDestructorFunction(enumName);
    if (!fn || !fn->empty()) return;     // 已有定义则不重复

    auto enumLLVMType = getLLVMType(TypeInfo(enumName));
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
            if (typeNeedsDestructor(t->getType())) { any = true; break; }
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
            auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr,
                static_cast<unsigned>(i), "payload.elem");
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

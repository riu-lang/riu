// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 函数调用编译：从 compiler_call.cpp 拆出 (P1 Phase 3)
// 覆盖 compileFunctionCall / compileGenericFunctionCall / compileKnownFunctionCall。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <functional>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

llvm::Value* Compiler::compileFunctionCall(p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args,
                                           vector<TypeInfo>& argTypes) {
    if (_castFunctions.contains(fnName)) {
        DEBUG_LOG_VAL("    Expr: CastFunction", fnName);
        auto& castInfo = _castFunctions[fnName];
        return createCast(castInfo.value, castInfo.srcType, castInfo.dstType);
    }

    // v0.6 Phase 2b: 透明类型别名解析，使 alias 名实参 / 形参在重载查找上视为同一类型
    // 函数符号表已在 validateAliases 中归一化；这里再把 argTypes 也走一遍，匹配两侧
    vector<TypeInfo> resolvedArgTypes;
    resolvedArgTypes.reserve(argTypes.size());
    for (auto& t : argTypes)
        resolvedArgTypes.push_back(applySubst(t));
    auto fnSymbol = _file->lookupFnSymbolWithParams(fnName, resolvedArgTypes);

    // Phase 4b: 当存在同名 generic + 非泛型重载时，参数严格匹配的非泛型优先；
    // 仅在 fnSymbol 没匹配到时才走泛型路径。这样 `assert_eq(s1 String, s2 String)`
    // 命中 SDK assert.yux 的 yux 重载，而不会跑到 #Builtin 的 compileTestAssertEq。
    // getGenericFunction 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
    auto [genericFn, fnOwner] = _file->getGenericFunction(fnName);
    if (!fnOwner) fnOwner = _file;

    // 泛型函数自身的 fnSymbol 注册项（参数含未解析类型形参如 T / Ref(T)）会在
    // lookupFnSymbolWithParams 中与调用方同名的未解析形参碰撞（典型场景：
    // Array<T>::get → T& 实参命中 print<T>(x T&) 的 fnSymbol）。此时 fnSymbol
    // 应让位给泛型消歧路径，而非当作普通函数调用（生成未实例化的泛型符号引用）。
    // 检查所有同名泛型重载，只要 fnSymbol 的 params 与任一泛型声明的 params 完全一致，
    // 说明 fnSymbol 就是该泛型自身的注册项 → 忽略它。
    // collectGenericFunctions 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
    if (fnSymbol && genericFn) {
        vector<pair<FnNode*, FileNode*>> allGenerics;
        _file->collectGenericFunctions(fnName, allGenerics, _file);
        for (auto& [gFn, _] : allGenerics) {
            auto gp = gFn->header()->params();
            if (fnSymbol->params.size() != gp.size()) continue;
            bool paramsMatch = true;
            for (size_t i = 0; i < gp.size(); ++i) {
                if (!gp[i]->type()) continue;
                if (fnSymbol->params[i] != gp[i]->type()->getType()) {
                    paramsMatch = false;
                    break;
                }
            }
            if (paramsMatch) {
                fnSymbol = nullptr;
                break;
            }
        }
    }

    if (genericFn && !fnSymbol) {
        // 多泛型重载消歧：当有多个同名泛型（如 print<T>(x T) + print<T>(x T&)）时，
        // 逐个试 inferGenericFnTypeArgs，按参数结构打分，选最匹配的
        // collectGenericFunctions 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
        vector<pair<FnNode*, FileNode*>> genericFns;
        _file->collectGenericFunctions(fnName, genericFns, _file);
        if (genericFns.size() > 1) {
            auto [best, bestOwner] = sema::resolveBestGenericOverload(genericFns, callNode, fnName, argTypes);
            if (best) {
                genericFn = best;
                fnOwner = bestOwner;
            }
        }
        return compileGenericFunctionCall(callNode, fnName, args, argTypes, genericFn, fnOwner);
    }
    // 0-参泛型 intrinsic（如 size_of<T>() / heap_null<T>()）的 fnSymbol 会与同名空参
    // 非泛型重载形态相同（params 都是 []），lookupFnSymbolWithParams 误命中泛型自身。
    // 调用点带显式 turbofish 时强制走泛型分派，避免 mangled extern call。
    if (genericFn && !callNode->getTypeArgs().empty()) {
        return compileGenericFunctionCall(callNode, fnName, args, argTypes, genericFn, fnOwner);
    }

    // Phase 3.3.2.b: 自由 intrinsic arity (E6027) 收口到 sema helper
    sema::validateFreeIntrinsicArity(fnName, args.size(), callNode->getLineNumber(), callNode->getColumn());

    if (fnName == "ptr_from_addr") {
        DEBUG_LOG("    Expr: PtrFromAddr");
        return _builder.CreateIntToPtr(args[0], llvm::PointerType::get(_context, 0), "ptr_from_addr");
    }

    if (fnName == "rc_leak_count") {
        DEBUG_LOG("    Expr: rc_leak_count");
        auto g = runtime::getRcBlockCountGlobal(_module, _builder);
        return _builder.CreateLoad(_builder.getInt64Ty(), g, "rc_leak");
    }

    if (fnName == "_ptr_offset") {
        DEBUG_LOG("    Expr: _ptr_offset");
        // E6023 (Phase 3.3.3.a): 跨模块私有, 迁至 sema::validatePtrOffsetVisibility.
        // 与 E6006 (validateFnSymbolVisibility) 重叠但错误码不同.
        sema::validatePtrOffsetVisibility(fnSymbol, _file->moduleName(), callNode->getLineNumber(),
                                          callNode->getColumn());
        auto i8Ty = _builder.getInt8Ty();
        return _builder.CreateGEP(i8Ty, args[0], args[1], "ptr_off");
    }

    // 测试断言内建（spec §11.3.5）：非泛型分支
    // assert_eq:<T> 走 compileGenericFunctionCall #Builtin 分支
    if (fnName == "assert_true") {
        DEBUG_LOG("    Expr: assert_true");
        return compileTestAssertTrue(callNode, args, argTypes);
    }
    if (fnName == "assert_false") {
        DEBUG_LOG("    Expr: assert_false");
        return compileTestAssertFalse(callNode, args, argTypes);
    }
    if (fnName == "fail") {
        DEBUG_LOG("    Expr: fail");
        return compileTestFail(callNode, args, argTypes);
    }

    if (fnSymbol) {
        // E6006 已迁至 sema::validateFnSymbolVisibility
        sema::validateFnSymbolVisibility(fnSymbol, _file->moduleName(), fnName, callNode->getLineNumber(),
                                         callNode->getColumn());
        return compileKnownFunctionCall(callNode, fnName, args, argTypes, fnSymbol);
    }

    DEBUG_LOG_VAL("    Expr: ExternalFunctionCall", fnName);
    auto retType = callNode->getType();
    bool retIsPtr = retType.isPtr();

    auto fn = _module->getFunction(fnName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (size_t i = 0; i < argTypes.size(); ++i) {
            if (argTypes[i].isPtr()) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(args[i]->getType());
            }
        }
        auto llvmRetType = retIsPtr ? llvm::PointerType::get(_context, 0) : _builder.getVoidTy();
        auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
    }

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        bool paramIsPtrInSignature =
            (i < fn->getFunctionType()->getNumParams()) && fn->getFunctionType()->getParamType(i)->isPointerTy();

        if (paramIsPtrInSignature && args[i]->getType()->isPointerTy()) {
            callArgs.push_back(_builder.CreateBitCast(args[i], llvm::PointerType::get(_context, 0), "ptr_cast"));
        } else {
            callArgs.push_back(args[i]);
        }
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    if (retIsPtr) {
        return callResult;
    }

    return callResult;
}

llvm::Value* Compiler::compileGenericFunctionCall(p<ExprCallNode> callNode, const string& fnName,
                                                  vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
                                                  p<FnNode> genericFn, p<FileNode> fnOwner) {

    const auto& typeParams = genericFn->header()->typeParams();
    vector<TypeInfo> typeArgs;

    const auto& explicitTypeArgs = callNode->getTypeArgs();
    if (!explicitTypeArgs.empty()) {
        // E6010 已迁至 sema::validateGenericTypeArgsArity
        sema::validateGenericTypeArgsArity(fnName, typeParams.size(), explicitTypeArgs.size(),
                                           callNode->getLineNumber(), callNode->getColumn());
        for (auto& tn : explicitTypeArgs) {
            typeArgs.push_back(applySubst(tn->getType()));
        }
    } else if (fnName == "as_ref" && !argTypes.empty() && argTypes[0].isHeap()) {
        // DRAFT-heap-types §8.3a.6 (Phase 2.7): as_ref(Heap<T>) 沿用 Rc 的 SDK 签名,
        // unify 不能从 Heap<T> arg 反推 T (Rc<T> ≠ Heap<T>), 在此前直接抽 Heap 内层.
        auto inner = argTypes[0].heapElementType();
        if (!inner) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                           argTypes[0].getFullName());
        }
        typeArgs.push_back(*inner);
    } else {
        // E6012 / E6013 已迁至 sema::inferGenericFnTypeArgs
        sema::inferGenericFnTypeArgs(callNode, genericFn, fnName, argTypes, typeArgs);
    }

    // 泛型类型推断完成后，按推断出的类型参数为灵活整数实参推断具体类型并重编译
    // 典型场景：assert_eq(a_i8, 42) — T 由 a 推断为 i8，42 默认 i32 需要按 T=i8 收束
    {
        auto params = genericFn->header()->params();
        for (size_t i = 0; i < params.size() && i < callNode->getArgs().size(); ++i) {
            if (!isFlexibleIntExpr(callNode->getArgs()[i])) continue;
            auto paramTypeNode = params[i]->type();
            if (!paramTypeNode) continue;
            TypeInfo pt = paramTypeNode->getType();
            for (size_t j = 0; j < typeParams.size() && j < typeArgs.size(); ++j) {
                if (pt.name == typeParams[j]) {
                    // 剥 Ref 壳后检查是否为整型（arr[0] 返 T&，T 被记为 i64& 时这里也能命中）
                    TypeInfo target = typeArgs[j];
                    if (target.isRef()) {
                        if (auto inner = target.refElementType()) target = *inner;
                    }
                    if (isIntTypeName(target.name)) {
                        tryInferIntType(callNode->getArgs()[i], target);
                        args[i] = compileExpr(callNode->getArgs()[i]);
                        argTypes[i] = callNode->getArgs()[i]->getType();
                        break;
                    }
                }
            }
        }
    }

    if (genericFn->header()->hasAnno("Builtin")) {
        // Phase 3.3.2.c: Builtin intrinsic typeArgs/args arity 校验
        // 同时覆盖 E6017 (未知 intrinsic) — helper 内部对清单外 fnName 直接抛.
        sema::validateBuiltinIntrinsicShape(fnName, typeArgs.size(), args.size(), callNode->getLineNumber(),
                                            callNode->getColumn());
        // Phase 3.3.2.d: Builtin intrinsic 类型形态校验
        // 覆盖 same_ref / ptr_of (E6028 AST 形态 + E6029 T 必须堆句柄) / as_ref / weak (E6029 argType)
        // / copy_of (E6032 深度 Ref 扫描).
        sema::validateBuiltinIntrinsicTypeShape(fnName, typeArgs, argTypes, callNode->getArgs(), _file,
                                                _yux ? _yux->sdkFile() : nullptr, callNode->getLineNumber(),
                                                callNode->getColumn());

        // 测试断言泛型分支（spec §11.3.5）：assert_eq:<T> T ∈ 数值/bool
        if (fnName == "assert_eq") {
            return compileTestAssertEq(callNode, args, argTypes, typeArgs[0]);
        }
        if (fnName == "size_of") {
            auto llvmType = getLLVMType(typeArgs[0]);
            if (!llvmType) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6019,
                               typeArgs[0].getFullName());
            }
            auto size = _module->getDataLayout().getTypeAllocSize(llvmType);
            return _builder.getInt64(size);
        }
        // DRAFT-spec-reflect Phase 3a (捷径 A): __yux_reflect_type:<T>() 拿 Type 反射节点.
        // lazy emit linkonce_odr rodata 全局 + load by value.
        if (fnName == "__yux_reflect_type") {
            auto* gv = ensureReflectTypeGlobal(typeArgs[0]);
            if (!gv) {
                // Phase 3a 仅放行 Normal 用户 / SDK / wildcard-imported struct;
                // 复用 E6019 (intrinsic 类型形态错) 占位, Phase 4+ 接 spec 链时换专属错码.
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6019,
                               typeArgs[0].getFullName());
            }
            auto typeStructTy = getLLVMType(TypeInfo("Type"));
            return _builder.CreateLoad(typeStructTy, gv, "reflect.type");
        }
        if (fnName == "upgrade") {
            // Phase 1d.2：Weak<T> → Rc<T>?
            // E6026 / E6027 已由 sema::validateBuiltinIntrinsicShape 校验
            auto& T = typeArgs[0];
            auto tShared = make_shared<TypeInfo>(T);
            TypeInfo weakTy("Weak", {tShared});
            TypeInfo rcTy("Rc", {tShared});
            auto rcShared = make_shared<TypeInfo>(rcTy);
            TypeInfo nullableRcTy("Nullable", {rcShared});

            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            // 取 Weak.handle
            auto weakStructTy = getLLVMType(weakTy);
            auto weakTmp = _builder.CreateAlloca(weakStructTy, nullptr, "upgrade.weak_tmp");
            _builder.CreateStore(args[0], weakTmp);
            auto handleField = _builder.CreateGEP(weakStructTy, weakTmp, {zero, zero}, "upgrade.handle_field");
            auto handle = _builder.CreateLoad(ptrTy, handleField, "upgrade.handle");

            // 调 _box_upgrade(handle) → handle_or_null
            auto upgradeFn = runtime::getRcUpgradeFn(_module, _builder);
            auto resultHandle = _builder.CreateCall(upgradeFn, {handle}, "upgrade.result");

            // 构造 Nullable<Rc<T>> = { i1 _has, { ptr handle } _value }
            auto nullableLLVMTy = getLLVMType(nullableRcTy);
            auto resultAlloca = _builder.CreateAlloca(nullableLLVMTy, nullptr, "upgrade.nullable");
            auto hasField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, zero}, "upgrade.has_field");
            auto valueField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, one}, "upgrade.value_field");

            auto isNotNull = _builder.CreateICmpNE(resultHandle, llvm::ConstantPointerNull::get(ptrTy), "upgrade.has");
            _builder.CreateStore(isNotNull, hasField);
            // Rc<T> = { ptr handle }；不论 has 与否都写 handle（null 时 _has=false 已表示无效）
            auto rcStructTy = getLLVMType(rcTy);
            auto innerHandleField = _builder.CreateGEP(rcStructTy, valueField, {zero, zero}, "upgrade.inner_handle");
            _builder.CreateStore(resultHandle, innerHandleField);

            return _builder.CreateLoad(nullableLLVMTy, resultAlloca, "upgrade.value");
        }
        if (fnName == "same_ref" || fnName == "ptr_of") {
            // Phase 7：地址相等 / 显式取裸指针 builtin
            // T 必须是堆句柄类型 (Rc / Weak / Array / String) 或 T&
            // E6028 AST 形态 / E6029 T 必须堆句柄 已由 sema::validateBuiltinIntrinsicTypeShape 校验 (3.3.2.d)
            // 下方 extractRawPtr 内残留的 E6028 / E6029 是兜底防御 (sema 抢先抛, 几乎不可达)
            // E6026 / E6027 已由 sema::validateBuiltinIntrinsicShape 校验
            auto& T = typeArgs[0];
            auto ptrTy = llvm::PointerType::get(_context, 0);

            // 工具：从第 i 个实参提取一个"裸指针"（handle / data / ref-ptr），按 T 的源类型决定如何抽
            // ptr_of 模式：当 T = Rc/Array/String 时，需要进一步跳过 RC 头或读 data 字段；same_ref 不跳头
            auto extractRawPtr = [&](size_t i, bool forPtrOf) -> llvm::Value* {
                if (T.isRc() || T.isWeak() || T.isArrayGeneric()) {
                    // args[i] 为 { ptr handle } 结构体值；ExtractValue 0 取 handle
                    auto handle = _builder.CreateExtractValue(args[i], {0}, "handle");
                    if (!forPtrOf || T.isRc()) {
                        if (forPtrOf && T.isRc()) {
                            // Rc payload 偏移 8（u32 strong + u32 weak）
                            return _builder.CreateInBoundsGEP(_builder.getInt8Ty(), handle,
                                                              {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                                                              "rc.payload");
                        }
                        return handle;
                    }
                    // B-3: Array 为 {ptr _data, i64 _len, i64 _cap} 内联 struct，
                    // field 0 即 _data，无需跳 RC 头
                    return handle;
                }
                if (T.name == "String" && T.kind == TypeKind::Normal) {
                    // B-4: String layout = { _buf: Rc<Array<u32>> } = { { ptr handle } }
                    auto handle = _builder.CreateExtractValue(args[i], {0, 0}, "string.handle");
                    if (!forPtrOf) return handle;
                    // RC Block: offset 8 = Array._data（跳过 u32 strong + u32 weak）
                    auto dataAddr = _builder.CreateInBoundsGEP(_builder.getInt8Ty(), handle,
                                                               {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                                                               "string.data.addr");
                    return _builder.CreateLoad(ptrTy, dataAddr, "string.data");
                }
                if (T.isRef()) {
                    // T& 路径：args[i] 是 compileExpr 自动 deref 后的 U 值，需要回溯 AST 拿原始指针
                    auto argNode = callNode->getArgs()[i];
                    if (auto litExpr = dynamic_cast<ExprLiteralNode*>(argNode)) {
                        if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                            auto name = objLit->getValue().getText();
                            auto it = _localVarPtrs.find(name);
                            if (it != _localVarPtrs.end()) return it->second;
                        }
                    }
                    if (auto refExpr = dynamic_cast<ExprGetRefNode*>(argNode)) {
                        return compileGetRefExpr(refExpr);
                    }
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6028, fnName);
                }
                // Phase 8a: ptr_of:<Heap<U>>(h) FFI handoff (DRAFT-heap-types §8.3a)
                // args[i] = Heap<U> = 裸 U* (无 wrapper), 直接作为 Ptr 返回; 调用点摘除 source slot.
                if (T.isHeap()) {
                    return args[i];
                }
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                               T.getFullName());
            };

            if (fnName == "same_ref") {
                auto p0 = extractRawPtr(0, false);
                auto p1 = extractRawPtr(1, false);
                return _builder.CreateICmpEQ(p0, p1, "same_ref");
            }
            // ptr_of
            auto raw = extractRawPtr(0, true);
            // Phase 8a: ptr_of:<Heap<U>>(h) move-out — 摘除 source slot 的 _scopeVars,
            // 并把 slot 写 null (防御性: 若变量名后续仍被引用, 至少不会 double-free).
            // sema 已强制 arg 是 ID-literal.
            if (T.isHeap()) {
                auto argNode = callNode->getArgs()[0];
                if (auto litE = dynamic_cast<ExprLiteralNode*>(argNode)) {
                    if (auto objLit = dynamic_cast<LiteralObjNode*>(litE->literal())) {
                        auto name = objLit->getValue().getText();
                        std::erase(_scopeVars, name);
                        auto it = _localVarPtrs.find(name);
                        if (it != _localVarPtrs.end()) {
                            _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), it->second);
                        }
                    }
                }
            }
            return raw;
        }
        if (fnName == "as_ref") {
            // spec §8.3.5.5：as_ref:<T>(box Rc<T>) T&
            // DRAFT-heap-types §8.3a.6 (Phase 2.7)：as_ref:<T>(h Heap<T>) T&
            //   - Rc<T>:  跳过 8 字节 RC 头 (u32 strong + u32 weak) → payload
            //   - Heap<T>: 句柄 = 裸 T*, 直接返回 (无头, GEP 偏移 0)
            // 寿命检查在 borrow_checker 处理（识别 ExprCallNode 形如 as_ref(x)）
            // E6026 / E6027 已由 sema::validateBuiltinIntrinsicShape 校验
            auto& T = typeArgs[0];
            auto argType = callNode->getArgs()[0]->getType();
            if (argType.isHeap()) {
                // args[0] 即裸 T*；直接作为 T& 返回
                return args[0];
            }
            // 实参必须是 Rc<T>（不接受 Rc<T>?、Array、String、Weak 等）
            if (!argType.isRc() || argType.isNullable()) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                               argType.getFullName());
            }
            // args[0] 为 Rc<T> = { ptr handle } 结构体值；ExtractValue 0 取 handle
            auto handle = _builder.CreateExtractValue(args[0], {0}, "as_ref.handle");
            // payload 偏移 8（u32 strong + u32 weak）
            return _builder.CreateInBoundsGEP(_builder.getInt8Ty(), handle,
                                              {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)}, "as_ref.payload");
        }
        if (fnName == "copy_of") {
            // spec §12.7.3 / DRAFT-const-mut [#1.I]：copy_of:<T>(x T&) T
            // 返回独立 owned T；值类型 memcpy，含 Rc / Array / String / Weak 字段时按字段 retain
            // 含 Ref<U> 字段 → 报 E6032（已由 sema::validateBuiltinIntrinsicTypeShape 校验, 3.3.2.d）
            // E6026 / E6027 已由 sema::validateBuiltinIntrinsicShape 校验
            auto& T = typeArgs[0];

            // Phase 3f / 6: Heap<U> 深拷 — 新分配 + 写入 inner U + 递归 retain U 的 RC 字段.
            // 不同于 Rc (shallow handle copy): Heap 是单 owner, 浅拷会 double-free.
            if (T.isHeap()) {
                auto innerSp = T.heapElementType();
                if (!innerSp) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                                   T.getFullName());
                }
                const auto& innerType = *innerSp;
                auto innerLLVMType = getLLVMType(innerType);
                // args[0] = Heap<U> = ptr (raw heap handle, 不是 wrapper struct)
                auto srcInner = _builder.CreateLoad(innerLLVMType, args[0], "copy_of.heap.src");
                auto sizeVal =
                    _builder.getInt64(_module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
                auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
                auto newPtr = _builder.CreateCall(allocFn, {sizeVal}, "copy_of.heap.new");
                _builder.CreateStore(srcInner, newPtr);
                // inner 的 RC 字段 +1 (deep copy 后两个 Heap 各持一份内嵌 Rc 句柄)
                retainHandleAtCallSite(srcInner, innerType);
                return newPtr;
            }

            // Phase 3f / 6: Heap<U>? 深拷 — null → null, some → 走 Heap 分支同款.
            if (T.isNullable()) {
                auto innerNullSp = T.nullableInnerType();
                if (innerNullSp && innerNullSp->isHeap()) {
                    const auto& innerHeapType = *innerNullSp;
                    auto innerElemSp = innerHeapType.heapElementType();
                    if (!innerElemSp) {
                        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                                       T.getFullName());
                    }
                    const auto& innerType = *innerElemSp;
                    auto innerLLVMType = getLLVMType(innerType);
                    auto ptrTy = llvm::PointerType::get(_context, 0);
                    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

                    auto hasFlag = _builder.CreateExtractValue(args[0], {0}, "copy_of.nh.has");
                    auto srcPtr = _builder.CreateExtractValue(args[0], {1}, "copy_of.nh.src");

                    auto* fn = _builder.GetInsertBlock()->getParent();
                    auto* startBB = _builder.GetInsertBlock();
                    auto* allocBB = llvm::BasicBlock::Create(_context, "copy_of.nh.alloc", fn);
                    auto* contBB = llvm::BasicBlock::Create(_context, "copy_of.nh.cont", fn);
                    _builder.CreateCondBr(hasFlag, allocBB, contBB);

                    _builder.SetInsertPoint(allocBB);
                    auto srcInner = _builder.CreateLoad(innerLLVMType, srcPtr, "copy_of.nh.inner");
                    auto sizeVal =
                        _builder.getInt64(_module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
                    auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
                    auto newPtr = _builder.CreateCall(allocFn, {sizeVal}, "copy_of.nh.new");
                    _builder.CreateStore(srcInner, newPtr);
                    retainHandleAtCallSite(srcInner, innerType);

                    auto* afterAllocBB = _builder.GetInsertBlock();
                    _builder.CreateBr(contBB);

                    _builder.SetInsertPoint(contBB);
                    auto phi = _builder.CreatePHI(ptrTy, 2, "copy_of.nh.ptr");
                    phi->addIncoming(nullPtr, startBB);
                    phi->addIncoming(newPtr, afterAllocBB);

                    auto resultTy = getLLVMType(T);
                    llvm::Value* result = llvm::UndefValue::get(resultTy);
                    result = _builder.CreateInsertValue(result, hasFlag, {0}, "copy_of.nh.res.has");
                    result = _builder.CreateInsertValue(result, phi, {1}, "copy_of.nh.res.val");

                    return result;
                }
            }

            // args[0] 是 T& (ptr) 或 T 值：若 LLVM 类型为 ptr 则 load 出 T 值
            llvm::Value* copied = args[0];
            bool isArray = T.isArrayGeneric();
            llvm::Type* copiedLLVMTy = getLLVMType(T);
            if (args[0]->getType()->isPointerTy()) {
                copied = _builder.CreateLoad(copiedLLVMTy, args[0], "copy_of.load");
            }

            // B-3: Array<T> — #NoCopy 类型，深拷贝待 Spec Clone 后实现
            // TODO(Spec Clone): 实现 Array 逐元素深拷贝（分配新缓冲 + 逐元素复制 + retain）
            if (isArray) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E4031, T.name, "copy_of",
                               T.name);
            }

            // 把所有 RC 子结构 +1：Rc/Array/Weak 抽 handle 调对应 retain；
            // struct 走 retainStructFieldsAtCallSite 递归；含 RC enum 走其分支。
            // 内置 / Ptr / 平凡 struct：no-op。
            retainHandleAtCallSite(copied, T);

            return copied;
        }
        if (fnName == "weak") {
            // spec §4.8.3.1 / §9：weak:<T>(box Rc<T>?) Weak<T>
            // 接受 Rc<T> 或 Rc<T>?；null/哨兵输入返回空 Weak（永远 upgrade 失败）
            // 复用 _weak_retain：复制 handle 指针 + weak 计数 +1
            // E6026 / E6027 已由 sema::validateBuiltinIntrinsicShape 校验
            auto& T = typeArgs[0];
            auto argType = callNode->getArgs()[0]->getType();

            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

            // 提取源 handle：
            //   Rc<T>      → args[0] = { ptr handle }，直接抽 field 0
            //   Rc<T>?     → args[0] = Nullable<Rc<T>> = { i1 _has, { ptr handle } _value }
            //                  按 _has 选 inner.handle / null
            llvm::Value* srcHandle = nullptr;
            if (argType.isRc() && !argType.isNullable()) {
                srcHandle = _builder.CreateExtractValue(args[0], {0}, "weak.src.handle");
            } else if (argType.isNullable()) {
                auto inner = argType.nullableInnerType();
                if (!inner || !inner->isRc()) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                                   argType.getFullName());
                }
                auto hasFlag = _builder.CreateExtractValue(args[0], {0}, "weak.has");
                auto innerHandle = _builder.CreateExtractValue(args[0], {1, 0}, "weak.inner.handle");
                srcHandle = _builder.CreateSelect(hasFlag, innerHandle, nullPtr, "weak.handle");
            } else {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6029, fnName,
                               argType.getFullName());
            }

            // _weak_retain(handle)：null / 哨兵跳过；否则 weak++
            auto weakRetainFn = runtime::getWeakRetainFn(_module, _builder);
            _builder.CreateCall(weakRetainFn, {srcHandle});

            // 构造 Weak<T> = { ptr handle }
            auto tShared = make_shared<TypeInfo>(T);
            TypeInfo weakTy("Weak", {tShared});
            auto weakStructTy = getLLVMType(weakTy);
            auto resultAlloca = _builder.CreateAlloca(weakStructTy, nullptr, "weak.result");
            auto handleField = _builder.CreateGEP(weakStructTy, resultAlloca, {zero, zero}, "weak.result.handle_field");
            _builder.CreateStore(srcHandle, handleField);
            auto result = _builder.CreateLoad(weakStructTy, resultAlloca, "weak.result.val");

            return result;
        }
        if (fnName == "move") {
            // Phase B-1: move:<T>(x T&) T — 所有权转移
            // 从 T& 参数 load 出 T 值，源变量标记为 moved（不可达、不析构）
            auto& T = typeArgs[0];
            auto argNode = callNode->getArgs()[0];

            // 回溯 AST 拿到源变量的 alloca 指针
            llvm::Value* srcAlloca = nullptr;
            string varName;
            if (auto lit = dynamic_cast<ExprLiteralNode*>(argNode)) {
                if (auto objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                    varName = objLit->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) srcAlloca = it->second;
                }
            }
            if (!srcAlloca) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6028, fnName);
            }

            // Load T 值（不 retain，所有权转移）
            auto llvmT = getLLVMType(T);
            auto loaded = _builder.CreateLoad(llvmT, srcAlloca, "move.load");

            // 标记 moved：destructor 跳过 + 后续访问报 E4033
            _movedVars.insert(varName);
            std::erase(_scopeVars, varName);

            // 防御性：堆句柄类型写 null 到源 slot 防 double-free
            if (T.isRc() || T.isArrayGeneric() || T.isWeak()) {
                _builder.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)), srcAlloca);
            }
            // TODO: 含 RC 字段的普通 struct move 后应清空 alloca，防字段级 double-release

            return loaded;
        }
        if (fnName == "_ptr_as_ref") {
            // B-3: _ptr_as_ref:<T>(p Ptr) T& — 裸指针 reinterpret 为 T&
            // LLVM opaque pointers 下 Ptr == T& (都是 ptr)，直接透传
            return args[0];
        }
        if (fnName == "_ptr_write") {
            // B-3: _ptr_write:<T>(p Ptr, v T) — 将 v 写入 p 指向的内存
            _builder.CreateStore(args[1], args[0]);
            return nullptr; // void
        }
        if (fnName == "heap_some" || fnName == "heap_null") {
            // DRAFT-heap-types §8.3a.4.2 (Phase 3d)：Heap<T>? 构造助手
            // heap_some<T>(v T) Heap<T>?  → {_has=true,  _value=__yux_heap_alloc + store v}
            // heap_null<T>()    Heap<T>?  → {_has=false, _value=null}
            auto& T = typeArgs[0];
            auto innerLLVMType = getLLVMType(T);
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            // 结果类型 = Nullable<Heap<T>> = { i1 _has, ptr _value }
            auto tShared = make_shared<TypeInfo>(T);
            TypeInfo heapTy("Heap", {tShared});
            auto heapShared = make_shared<TypeInfo>(heapTy);
            TypeInfo nullableHeapTy("Nullable", {heapShared});
            auto nullableLLVMTy = getLLVMType(nullableHeapTy);

            auto resultAlloca = _builder.CreateAlloca(nullableLLVMTy, nullptr,
                                                      fnName == "heap_some" ? "heap_some.result" : "heap_null.result");
            auto hasField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, zero}, "heap_opt.has_field");
            auto valueField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, one}, "heap_opt.value_field");

            if (fnName == "heap_null") {
                _builder.CreateStore(_builder.getInt1(false), hasField);
                _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), valueField);
            } else {
                // heap_some: 分配 + 写 inner，所有权由 arg 转交给 Heap payload
                auto sizeVal =
                    _builder.getInt64(_module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
                auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
                auto rawPtr = _builder.CreateCall(allocFn, {sizeVal}, "heap_some.payload");
                _builder.CreateStore(args[0], rawPtr);
                consumeTemp(args[0]);
                _builder.CreateStore(_builder.getInt1(true), hasField);
                _builder.CreateStore(rawPtr, valueField);
            }

            auto result = _builder.CreateLoad(nullableLLVMTy, resultAlloca, "heap_opt.val");
            return result;
        }
        // E6017 (未知 Builtin intrinsic) 已由 sema::validateBuiltinIntrinsicShape
        // 在分派前抛出, 不会到这里; 留 unreachable assert 防御.
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6017, fnName);
    }

    // §6.4.4.4 / §12.4 边界单态化校验 (Phase 3.3): 对每个 <T : D1 + D2>,
    // 解析每个 D 的限定名并校验 typeArgs[i] 是否满足 D (显式 impl 或
    // #DraftLike 结构匹配); 不满足报 E1106. 不影响 ensureFnInstance 的
    // mangle (单态化静态分发, 边界仅做静态检查).
    // E3032 / E1106 (Phase 3.3.3.c): 迁至 sema::validateGenericTypeArgsSpecBound.
    if (_yux) {
        sema::validateGenericTypeArgsSpecBound(&_yux->specRegistry(), &_yux->specImplChecker(), fnOwner,
                                               genericFn->header(), typeArgs, callNode->getLineNumber(),
                                               callNode->getColumn());
    }

    string mangledName = ensureFnInstance(genericFn, typeArgs, fnOwner, callNode->getLineNumber());

    map<string, TypeInfo> subst;
    for (size_t i = 0; i < typeParams.size(); ++i) {
        subst[typeParams[i]] = typeArgs[i];
    }
    _substStack.push_back(SubstFrame{.subst = subst, .baseStructName = "", .effStructName = ""});

    vector<TypeInfo> instParamTypes;
    for (auto param : genericFn->header()->params()) {
        if (param->type()) {
            instParamTypes.push_back(applySubst(param->type()->getType()));
        }
    }

    TypeInfo instRetType;
    if (genericFn->header()->retType()) {
        instRetType = applySubst(genericFn->header()->retType()->getType());
    }

    _substStack.pop_back();

    bool isPrivate = !fnName.empty() && fnName[0] == '_';
    // 泛型实例：使用消费方模块作为符号前缀（与 emitFnInstances 一致，每个使用方模块各自一份 IR）
    auto& fi = _fnInstances[mangledName];
    string ownerModForMangle = fi.consumerModule.empty() ? fnOwner->moduleName() : fi.consumerModule;
    string cName = Mangler::function(ownerModForMangle, mangledName, instParamTypes, isPrivate);
    DEBUG_LOG_VAL("    Expr: GenericFunctionCall", fnName << " -> " << cName);

    auto fn = _module->getFunction(cName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (auto& t : instParamTypes) {
            if (!t.isPtr() && !t.isRef() && structParamUsesPointer(t)) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(getLLVMType(t));
            }
        }
        string gFallibleErr;
        if (auto e = genericFn->header()->getAnnoArg("Fallible")) gFallibleErr = *e;
        auto retType = wrapFallibleRetType(instRetType, gFallibleErr);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        auto& at = instParamTypes[i];
        if (at.isPtr() || at.isRef()) {
            callArgs.push_back(args[i]);
            continue;
        }
        // Phase 3a: Rc/Array/Weak 实参传前 retain（callee-clean）
        // Phase 8c: fresh 实参（call/array literal）已自带 +1，跳过 retain
        // Phase 8d.1: fresh 实参的 +1 移交给 callee，从临时帧消费掉，避免帧末多余 release
        bool isFresh = isFreshHandleExpr(callNode->getArgs()[i]);
        // Phase B-1: E4031 #NoCopy 按值传参检查已迁入 SemaPass，Compiler 端不再重复。
        if (typeNeedsDestructor(at)) {
            if (!isFresh) {
                retainHandleAtCallSite(args[i], at);
            } else {
                consumeTemp(args[i]);
            }
            // B-4: Array<T> 等既需析构又需按指针传参的类型，做指针转换再 push
            if (structParamUsesPointer(at)) {
                auto structType = getLLVMType(at);
                auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
                _builder.CreateStore(args[i], alloca);
                callArgs.push_back(alloca);
            } else {
                callArgs.push_back(args[i]);
            }
            continue;
        }
        if (structParamUsesPointer(at)) {
            auto structType = getLLVMType(at);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
        } else {
            callArgs.push_back(args[i]);
        }
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    if (!instRetType.empty()) {
        if (instRetType.isPtr() || instRetType.isRef()) {
            return callResult;
        }
        auto sd = _file->getStructDecl(instRetType.name);
        if (!sd && _yux && _yux->sdkFile()) {
            sd = _yux->sdkFile()->getStructDecl(instRetType.name);
        }
        if (sd && !isBuiltinType(instRetType.name)) {
            auto structType = getLLVMType(instRetType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "ret_tmp");
            _builder.CreateStore(callResult, alloca);
            return _builder.CreateLoad(structType, alloca);
        }
    }

    return callResult;
}

llvm::Value* Compiler::compileKnownFunctionCall(p<ExprCallNode> callNode, const string& fnName,
                                                vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
                                                FnSymbolInfo* fnSymbol) {
    string cName;
    if (fnSymbol->isExternal) {
        cName = fnName;
    } else if (fnName == "main") {
        cName = "yux_main";
    } else {
        string ownerMod = fnSymbol->moduleName.empty() ? _file->moduleName() : fnSymbol->moduleName;
        bool isPriv = !fnName.empty() && fnName[0] == '_';
        cName = Mangler::function(ownerMod, fnName, fnSymbol->params, isPriv);
    }

    DEBUG_LOG_VAL("    Expr: FunctionCall", fnName << " -> " << cName);
    auto fn = _module->getFunction(cName);

    bool needPtrConversion = fnSymbol->isExternal;
    for (auto& param : fnSymbol->params) {
        if (param.isPtr()) {
            needPtrConversion = true;
            break;
        }
    }

    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (auto& param : fnSymbol->params) {
            if (param.isPtr() || param.isRef() || structParamUsesPointer(param.name)) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(getLLVMType(param));
            }
        }
        // extern fn 禁 #Fallible（[#7]）—— extern 路径走原 isPtr 分支不包装；
        // 用户 fn 走 wrapFallibleRetType，按 fnSymbol->fallibleErrType 决定是否包成 struct
        llvm::Type* retType;
        if (fnSymbol->isExternal && !fnSymbol->retType.empty() && TypeInfo(fnSymbol->retType).isPtr()) {
            retType = llvm::PointerType::get(_context, 0);
        } else {
            retType = wrapFallibleRetType(TypeInfo(fnSymbol->retType), fnSymbol->fallibleErrType);
        }
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }

    DEBUG_LOG_VAL("    Function signature check", "numParams=" << fn->getFunctionType()->getNumParams());
    for (size_t i = 0; i < fn->getFunctionType()->getNumParams() && i < args.size(); ++i) {
        auto expectedType = fn->getFunctionType()->getParamType(i);
        auto actualType = args[i]->getType();
        DEBUG_LOG_VAL("    Param type",
                      i << " expected=" << expectedType->getTypeID() << " actual=" << actualType->getTypeID());
        if (expectedType->isIntegerTy() && actualType->isIntegerTy()) {
            DEBUG_LOG_VAL("    Integer bit width", "expected=" << expectedType->getIntegerBitWidth()
                                                               << " actual=" << actualType->getIntegerBitWidth());
        }
        if (expectedType != actualType) {
            DEBUG_LOG_VAL("    TYPE MISMATCH", "need conversion");
        }
    }

    // Phase 3d.2 / 3d.3: B 档 nullable move 收集 —— 形参 `Heap<T>?` byval + 实参是
    // `Heap<T>?` 的 lvalue (局部 ID 或局部 struct 字段 `b.field`) 时, 记录调用方 slot,
    // 调用后写回 {has=false, value=null}. 索引 lvalue / lambda 捕获留后续切片.
    struct HeapBdangSlot {
        llvm::Value* slotPtr;
        llvm::Type* llvmTy;
    };
    vector<HeapBdangSlot> heapBdangSlots;
    auto recordBdangIfEligible = [&](size_t i) {
        if (i >= fnSymbol->params.size() || i >= callNode->getArgs().size()) return;
        const auto& p = fnSymbol->params[i];
        if (p.isRef() || p.isPtr()) return;
        if (!p.isNullable()) return;
        auto inner = p.nullableInnerType();
        if (!inner || !inner->isHeap()) return;
        llvm::Value* slot = nullptr;
        llvm::Type* ty = nullptr;
        if (!tryHeapNullableLvalueSlot(callNode->getArgs()[i], slot, ty)) return;
        heapBdangSlots.push_back({.slotPtr = slot, .llvmTy = ty});
    };

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size() && i < fnSymbol->params.size(); ++i) {
        DEBUG_LOG_VAL("    Param", i << " argType=" << argTypes[i].name << " paramType=" << fnSymbol->params[i].name);
        DEBUG_LOG_VAL("    Param isPtr", argTypes[i].isPtr() << " paramIsPtr=" << fnSymbol->params[i].isPtr());
        recordBdangIfEligible(i);
        if (fnSymbol->params[i].isRef() && !fnSymbol->isExternal) {
            if (auto literalNode = dynamic_cast<ExprLiteralNode*>(callNode->getArgs()[i])) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        callArgs.push_back(it->second);
                        continue;
                    }
                }
            }
            // Phase 4b: 非局部变量（字符串字面量 / 调用结果 / 字段访问等）传给 T& 形参时，
            // alloca 一个 T 临时存放，再把 alloca 的指针作为 T& 传入。
            // args[i] 已是 T 值（compileExpr 对 T& 形参会把 ref 自解；对 T 直接给值）。
            if (args[i]->getType()->isPointerTy()) {
                callArgs.push_back(args[i]);
            } else {
                auto tmpAlloca = _builder.CreateAlloca(args[i]->getType(), nullptr, "ref_arg_tmp");
                _builder.CreateStore(args[i], tmpAlloca);
                callArgs.push_back(tmpAlloca);
            }
            continue;
        }

        if (fnSymbol->params[i].isPtr()) {
            if (args[i]->getType()->isPointerTy()) {
                auto ptrVal = _builder.CreateBitCast(args[i], llvm::PointerType::get(_context, 0), "ptr_cast");
                callArgs.push_back(ptrVal);
                continue;
            }
            // Phase 7c (DRAFT §9.3): extern 边界自动转 Ptr
            // T& / Rc<T> / Weak<T> / Array<T> / String 作实参传给 Ptr 形参时自动转换
            // 转换规则与 ptr_of 一致：Rc → payload (跳 RC 头)；Array/String → data 区
            if (fnSymbol->isExternal) {
                auto& aType = argTypes[i];
                auto ptrTy = llvm::PointerType::get(_context, 0);
                if (aType.isRef()) {
                    // T& → 原始指针：从 AST 回溯（args[i] 已被 compileExpr 自动 deref 为 U 值）
                    auto argNode = callNode->getArgs()[i];
                    llvm::Value* refPtr = nullptr;
                    if (auto litExpr = dynamic_cast<ExprLiteralNode*>(argNode)) {
                        if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                            auto name = objLit->getValue().getText();
                            auto it = _localVarPtrs.find(name);
                            if (it != _localVarPtrs.end()) refPtr = it->second;
                        }
                    }
                    if (!refPtr) {
                        if (auto refExpr = dynamic_cast<ExprGetRefNode*>(argNode)) {
                            refPtr = compileGetRefExpr(refExpr);
                        }
                    }
                    if (refPtr) {
                        callArgs.push_back(refPtr);
                        continue;
                    }
                }
                if (aType.isRc() || aType.isWeak() || aType.isArrayGeneric()) {
                    auto handle = _builder.CreateExtractValue(args[i], {0}, "handle");
                    if (aType.isWeak()) {
                        callArgs.push_back(handle);
                        continue;
                    }
                    if (aType.isRc()) {
                        // Rc payload 偏移 8（跳过 RC 头）
                        auto payload = _builder.CreateInBoundsGEP(_builder.getInt8Ty(), handle,
                                                                  {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                                                                  "rc.payload");
                        callArgs.push_back(payload);
                        continue;
                    }
                    // B-3: Array 为 {ptr _data, i64 _len, i64 _cap} 内联 struct，
                    // field 0 即 _data，无需跳 RC 头
                    callArgs.push_back(handle);
                    continue;
                }
                if (aType.name == "String" && aType.kind == TypeKind::Normal) {
                    // B-4: String = { _buf Rc<Array<u32>> } = { { ptr handle } }
                    auto handle = _builder.CreateExtractValue(args[i], {0, 0}, "string.handle");
                    // RC Block: offset 8 = Array._data
                    auto dataAddr = _builder.CreateInBoundsGEP(_builder.getInt8Ty(), handle,
                                                               {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                                                               "string.data.addr");
                    auto dataPtr = _builder.CreateLoad(ptrTy, dataAddr, "string.data");
                    callArgs.push_back(dataPtr);
                    continue;
                }
            }
        }

        // callee-clean (DRAFT §7.3)：传参前 retain；callee 末尾析构 release 抵消
        // Phase 8c: fresh 实参（call/array literal）已自带 +1，跳过 retain
        // Phase 8d.1: fresh 实参的 +1 移交给 callee，从临时帧消费掉
        // Phase B-1: E4031 #NoCopy 按值传参检查已迁入 SemaPass，Compiler 端不再重复。
        bool paramNeedsPtr = structParamUsesPointer(fnSymbol->params[i]);
        if (typeNeedsDestructor(argTypes[i]) && !paramNeedsPtr) {
            if (i < callNode->getArgs().size() && !isFreshHandleExpr(callNode->getArgs()[i])) {
                retainHandleAtCallSite(args[i], argTypes[i]);
            } else if (i < callNode->getArgs().size()) {
                consumeTemp(args[i]);
            }
            callArgs.push_back(args[i]);
            continue;
        }

        if (paramNeedsPtr) {
            DEBUG_LOG_VAL("    Passing struct by pointer", "arg " << i << " : " << fnSymbol->params[i].name);
            // B-4: Array<T> 等 struct-by-pointer 实参也需要 consumeTemp（避免 temp frame
            // 末尾重复释放已在 callee 中移入 Rc 的 _data 缓冲）。
            // 对齐 compileMethodCall 的 typeNeedsDestructor + structParamUsesPointer 双检模式。
            if (typeNeedsDestructor(argTypes[i])) {
                if (i < callNode->getArgs().size() && !isFreshHandleExpr(callNode->getArgs()[i])) {
                    retainHandleAtCallSite(args[i], argTypes[i]);
                } else if (i < callNode->getArgs().size()) {
                    consumeTemp(args[i]);
                }
            }
            auto structType = getLLVMType(argTypes[i]);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
            continue;
        }

        callArgs.push_back(args[i]);
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    // Phase 3d.2: B 档 nullable move 写回 —— callee 接管 `Heap<T>?` byval 后,
    // 调用方 slot 写 {_has=false, _value=null}, 让作用域尾析构 / 后续读都视作 null.
    if (!heapBdangSlots.empty()) {
        auto z = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
        auto ptrTy = llvm::PointerType::get(_context, 0);
        for (auto& s : heapBdangSlots) {
            auto hasField = _builder.CreateGEP(s.llvmTy, s.slotPtr, {z, z}, "bdang.has");
            auto valField = _builder.CreateGEP(s.llvmTy, s.slotPtr, {z, one}, "bdang.value");
            _builder.CreateStore(_builder.getInt1(false), hasField);
            _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), valField);
        }
    }

    if (fnSymbol->isExternal && !fnSymbol->retType.empty() && TypeInfo(fnSymbol->retType).isPtr()) {
        return callResult;
    }

    // [#10.A] / [#10.C]：callee 标 #Fallible 时分流 isErr → 透传 / 提取 T_ok
    return handleFallibleCallResult(callResult, fnSymbol->fallibleErrType, TypeInfo(fnSymbol->retType), callNode);
}

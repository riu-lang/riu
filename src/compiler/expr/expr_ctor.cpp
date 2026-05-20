// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 构造表达式编译 (Enum / Dyn / Heap ctor)：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/enum_node.h"
#include "../compiler_runtime.h"
#include "ast/mangler.h"
#include "ast/yux.h"
#include "analyzer/symbol_suggest.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <set>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <cassert>


// 编译枚举构造表达式 E::V / E::V() / E::V(args)
// Phase 5: 支持零参 + tuple-payload variant
//
// 步骤：
// 1. 通过 ExprPathCallNode::getType() 解析后的 enum 名（已透传别名）查 EnumDecl
// 2. 验证 variant 存在 / arity 匹配
// 3. 取 enum 的 LLVM 类型（{ i32 tag } 或 { i32, [N x i8] }）
// 4. 在栈上 alloca，写入 tag = variant index
// 5. tuple-payload variant：把 payload buffer 重解释为 variant 的 tuple struct，
//    逐元素 store 实参值（实参类型与 payload 元素类型严格匹配；callee-clean
//    入参规则下，Rc/Array/Weak 已是 +1 fresh 句柄，直接交付给 enum 拥有）
// 6. 零参 variant 不动 payload buffer（spec §6.5）
// 7. 加载整体 struct value 作为表达式结果返回
llvm::Value* Compiler::compileEnumCtorExpr(p<ExprPathCallNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    string enumName = node->getType().name;     // 经别名解析后的真实 enum 名
    string variantName = node->variantName().getText();
    int line = node->getLineNumber();
    int col = node->getColumn();

    // Phase 3c 构造模型重构: 若 LHS 是 struct, 走 #Static fn 调用路径.
    // sema 已先做形态校验 (#Static 命中 / 缺失 / 实例方法误用), 这里直接 emit call.
    {
        string lhsRaw = node->enumName().getText();
        auto* structImpl = _file ? _file->getStructImpl(lhsRaw) : nullptr;
        FileNode* sdk = _yux ? _yux->sdkFile() : nullptr;
        if (!structImpl && sdk && sdk != _file) {
            structImpl = sdk->getStructImpl(lhsRaw);
        }
        if (structImpl) {
            string methodName = node->variantName().getText();
            p<FnHeaderNode> methodHeader = nullptr;
            for (auto& m : structImpl->methods()) {
                if (m->header()->name().getText() == methodName) {
                    methodHeader = m->header(); break;
                }
            }
            // sema Phase 2c 已拦 E3120/E3121; 这里幂等防御性兜底
            if (!methodHeader || !methodHeader->isStatic()) {
                throw YuxError(line, col, ErrorCode::E3121, lhsRaw, methodName);
            }
            // Phase 6E.4-B: 泛型 struct turbofish 形态 `Type:<T>::name(...)`
            // 消费 lhsTypeArgs, 触发 ensureStructInstance, 切到实例 mangled 名;
            // 同时压一帧 SubstFrame 让 paramTypes / retType 的 T / Self 替换生效.
            string effLhs = lhsRaw;
            bool pushedFrame = false;
            const auto& lhsTArgs = node->lhsTypeArgs();
            if (!lhsTArgs.empty()) {
                p<StructDeclNode> baseDecl = _file ? _file->getStructDecl(lhsRaw) : nullptr;
                p<FileNode> baseOwner = _file;
                if (!baseDecl && _yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                    baseDecl = _yux->sdkFile()->getStructDecl(lhsRaw);
                    baseOwner = _yux->sdkFile();
                }
                if (!baseDecl || !baseDecl->isGeneric()) {
                    throw YuxError(line, col, ErrorCode::E0000,
                                   "turbofish 形态需泛型 struct: " + lhsRaw);
                }
                if (lhsTArgs.size() != baseDecl->typeParams().size()) {
                    throw YuxError(line, col, ErrorCode::E6011,
                                   lhsRaw, baseDecl->typeParams().size(), lhsTArgs.size());
                }
                vector<sp<TypeInfo>> instArgs;
                instArgs.reserve(lhsTArgs.size());
                for (auto& ta : lhsTArgs) {
                    instArgs.push_back(std::make_shared<TypeInfo>(applySubst(ta->getType())));
                }
                effLhs = ensureStructInstance(baseDecl, instArgs, baseOwner, line);
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < instArgs.size(); ++i) {
                    subst[baseDecl->typeParams()[i]] = instArgs[i] ? *instArgs[i] : TypeInfo();
                }
                _substStack.push_back(SubstFrame{
                    .subst=std::move(subst), .baseStructName=lhsRaw, .effStructName=effLhs,
                    .sourceFile=_file ? _file->moduleName() : "", .sourceLine=line});
                pushedFrame = true;
            }
            // SubstFrame 在异常路径上必须 pop, 否则后续 applySubst 误用本帧 → 类型污染.
            try {
            vector<TypeInfo> paramTypes;
            for (auto p : methodHeader->params()) {
                if (p->type()) paramTypes.push_back(applySubst(p->type()->getType()));
            }
            TypeInfo retType;
            if (methodHeader->retType()) retType = applySubst(methodHeader->retType()->getType());
            string mFallibleErr;
            if (auto e = methodHeader->getAnnoArg("Fallible")) mFallibleErr = *e;
            auto fn = getMethodFunction(effLhs, methodName, paramTypes, retType,
                                        mFallibleErr, /*isStatic=*/true);
            vector<llvm::Value*> argVals;
            argVals.reserve(node->args().size());
            // Phase 6E: 灵活整数字面量按形参类型回填 (如 `S::make(1)` 推 1 为 i64)
            for (size_t i = 0; i < node->args().size() && i < paramTypes.size(); ++i) {
                tryInferIntType(node->args()[i], paramTypes[i]);
            }
            // Phase 6E: 调用站点实参 arity / 类型校验 (替代 6D 删除的 E6033).
            // 先抛 E3131 比让 LLVM signature-mismatch 断言崩好得多.
            if (node->args().size() != paramTypes.size()) {
                string expected, got;
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (i) expected += ", ";
                    expected += paramTypes[i].getFullName();
                }
                for (size_t i = 0; i < node->args().size(); ++i) {
                    if (i) got += ", ";
                    got += node->args()[i]->getType().getFullName();
                }
                throw YuxError(line, col, ErrorCode::E3131,
                               lhsRaw, methodName,
                               paramTypes.size(), expected,
                               node->args().size(), got);
            }
            for (size_t i = 0; i < node->args().size(); ++i) {
                auto actualTy = node->args()[i]->getType();
                if (!actualTy.empty() && !(actualTy == paramTypes[i])) {
                    string expected, got;
                    for (size_t j = 0; j < paramTypes.size(); ++j) {
                        if (j) expected += ", ";
                        expected += paramTypes[j].getFullName();
                    }
                    for (size_t j = 0; j < node->args().size(); ++j) {
                        if (j) got += ", ";
                        got += node->args()[j]->getType().getFullName();
                    }
                    throw YuxError(line, col, ErrorCode::E3131,
                                   lhsRaw, methodName,
                                   paramTypes.size(), expected,
                                   node->args().size(), got);
                }
            }
            for (auto& a : node->args()) {
                argVals.push_back(compileExpr(a));
            }
            // Phase 4c: 静态 fn 调用点的句柄实参所有权转移，与 ExprCallNode 路径对齐
            // (compiler_call.cpp:667-670). 不做这步会让 callee 拿到 caller 唯一 +1,
            // callee 析构释放后 caller 的 alloca 变成 use-after-free.
            for (size_t i = 0; i < argVals.size() && i < paramTypes.size(); ++i) {
                if (!typeNeedsDestructor(paramTypes[i])) continue;
                if (!isFreshHandleExpr(node->args()[i])) {
                    retainHandleAtCallSite(argVals[i], paramTypes[i]);
                } else {
                    consumeTemp(argVals[i]);
                }
            }
            auto callResult = _builder.CreateCall(fn, argVals,
                                       retType.empty() ? "" : methodName + ".ret");
            if (pushedFrame) {
                _substStack.pop_back();
            }
            return callResult;
            } catch (...) {
                if (pushedFrame) _substStack.pop_back();
                throw;
            }
        }
    }

    // Phase 3.4.a: enum ctor 形态校验 (E2019/E2020/E2021/E2032) 整体抠到 sema.
    // SemaPass 已先抛出; 这里是幂等防御性双跑.
    sema::validateEnumCtorShape(_file, _yux ? _yux->sdkFile() : nullptr, node);

    p<FileNode> owner = nullptr;
    auto enumDecl = lookupEnumDecl(enumName, owner);
    auto variant = enumDecl->variant(variantName);

    size_t givenArity = node->args().size();
    size_t declArity = variant->payloadArity();
    (void)givenArity; // arity 已由 sema::validateEnumCtorShape 校验

    int tagIndex = enumDecl->variantIndex(variantName);
    auto enumLLVMType = getLLVMType(TypeInfo(enumName));
    if (!enumLLVMType) {
        throw YuxError(line, col, ErrorCode::E3096, enumName);
    }

    // 在栈上 alloca、写入 tag
    auto alloca = _builder.CreateAlloca(enumLLVMType, nullptr, "enum.ctor");
    auto tagPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 0, "enum.tag.ptr");
    _builder.CreateStore(_builder.getInt32(tagIndex), tagPtr);

    // tuple-payload variant：把 payload buffer 重解释为 variant 自身的 tuple struct，
    // 按位置写入每个实参；payload 字段（即 enum LLVM type 的第 1 个字段）在 enum 类型有
    // payload 时一定存在，layout 形如 { i32 tag, [N x i8] payload }
    if (declArity > 0) {
        // 收集 variant 的 payload 元素 LLVM 类型，构造 variant tuple struct 类型
        vector<llvm::Type*> elemTys;
        elemTys.reserve(declArity);
        for (auto t : variant->payloadTypes()) {
            auto ll = getLLVMType(t->getType());
            if (!ll) {
                throw YuxError(line, col, ErrorCode::E3096,
                    enumName + "::" + variantName + " payload");
            }
            elemTys.push_back(ll);
        }
        auto payloadStruct = llvm::StructType::get(_context, elemTys);

        // payload buffer 字段地址（enum struct 的字段 1）
        auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 1, "enum.payload.ptr");

        // 编译实参并按 variant tuple struct 的字段位置 store.
        // 实参类型与 variant payload 类型的严格匹配 (E2032) 已由 sema::validateEnumCtorShape
        // 接管, 这里只走 IR emit.
        for (size_t i = 0; i < declArity; ++i) {
            auto argExpr = node->args()[i];
            auto argVal = compileExpr(argExpr);
            if (!argVal) {
                throw YuxError(line, col, ErrorCode::E3096,
                    enumName + "::" + variantName + " arg#" + std::to_string(i));
            }
            auto fieldPtr = _builder.CreateStructGEP(payloadStruct, payloadBufPtr,
                static_cast<unsigned>(i), "enum.payload.elem");
            _builder.CreateStore(argVal, fieldPtr);
            // 实参作为 fresh 临时若已入帧，需消费掉：所有权随构造转交给 enum 值，
            // 否则帧弹出时会 release 一次导致 use-after-free
            consumeTemp(argVal);
        }
    }

    auto loaded = _builder.CreateLoad(enumLLVMType, alloca, "enum.val");

    DEBUG_LOG_VAL("    Expr: EnumCtor",
        enumName << "::" << variantName << " tag=" << tagIndex << " arity=" << declArity);
    return loaded;
}


// 编译 Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
//
// Phase 1c：仅占位 codegen。目标是让 `Dyn<D>(x)` 在 parse + 类型推导 + IR 生成
// 全链路走通，落到 16 字节 fat pointer 值；真实 vtable 槽与对象安全检查留 Phase 2/3：
//   - Phase 2：E1131..E1134 静态检查（draft 名 / 嵌套 / 类型不满足 / 非对象安全）
//   - Phase 3：vtable 全局发射 + 槽 0 dtor wrapper + 构造时写真 vtable_ptr / 句柄消费
//
// 当前 emit 策略：
//   - vtable 槽：constant null（占位；Phase 3 替换为 `&__yux_vtable_<U>_<D>`）
//   - data 槽：
//       * arg.type = Rc<U>：从 Rc struct 中抽 handle（第 0 字段，指向 [RC head | U]）
//       * arg.type = U&    ：直接用借用 ptr（Phase 1c 不处理借用所有权，留 Phase 3c）
//       * 其它形态：暂用 null 占位，留 Phase 2 报 E1133
//
// 注：未做 retain / RC 转移。owned 形态意味着接管 Rc 的 +1，本应消费临时帧或 retain；
// 真路由（构造消费 Rc）随 vtable 落地一起补，所以这里 Rc 句柄"裸抽"——Phase 1c
// 的 smoke 只看编译能否过、IR 是否成型，不验运行时所有权。
llvm::Value* Compiler::compileDynCtorExpr(p<ExprDynCtorNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = node->getType();
    int line = node->getLineNumber();
    int col = node->getColumn();

    // ── Phase 2b: Dyn<D>(x) 构造静态检查 ─────────────────────────────────
    // 顺序: E1131 (D 必须是 draft) → E1132 (嵌套 Dyn) → E1134 (对象安全)
    // → E1133 (参数形态 Rc<U> / U& + U:D)。
    auto specInner = resultType.dynSpecType();
    std::string specBareName = specInner ? specInner->name : std::string();

    // 走 parent() 链而不是 parentScope()：struct 方法的 FnNode 在 AST 构造时
    // 不一定挂上 parentScope，但 parent() 链一定连到 FileNode
    // （参考 expr_node.cpp::lookupDynMethodRetType）。
    Node* cur = node->parent();
    FileNode* file = nullptr;
    while (cur) {
        if (auto f = dynamic_cast<FileNode*>(cur)) { file = f; break; }
        cur = cur->parent();
    }

    SpecDeclNode* specDecl = nullptr;
    std::string specQualified;
    if (_yux && file && !specBareName.empty()) {
        auto& reg = _yux->specRegistry();
        if (auto resolved = reg.resolve(specBareName, file)) {
            specDecl = resolved->decl;
            specQualified = resolved->qualifiedName;
        }
    }
    if (!specDecl) {
        // E1131: 内层不是已知 draft 名 (可能是结构体 / 类型别名 / 不存在符号)
        throw YuxError(line, col, ErrorCode::E1131,
            specBareName.empty() ? std::string("?") : specBareName);
    }

    // E1132: Dyn<Dyn<...>> — 内层 draft 位置不能再是 Dyn
    if (specInner && specInner->isDyn()) {
        throw YuxError(line, col, ErrorCode::E1132, resultType.getFullName());
    }

    // E1134: 对象安全
    if (_yux) {
        auto& checker = _yux->specImplChecker();
        if (!checker.specIsObjectSafe(specDecl)) {
            throw YuxError(line, col, ErrorCode::E1134,
                specQualified, specQualified, specQualified);
        }
    }

    // E1133: 参数形态 + U:D 满足
    auto argExpr = node->arg();
    auto argType = argExpr->getType();
    bool isBorrow = node->isBorrow();
    std::string concreteBare;
    if (isBorrow) {
        // Dyn<D&>(x): 接受 U& 或 Rc<U>
        if (argType.isRef()) {
            auto inner = argType.refElementType();
            if (inner) concreteBare = inner->name;
        } else if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    } else {
        // Dyn<D>(x): 仅接受 Rc<U>
        if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    }
    if (concreteBare.empty()) {
        throw YuxError(line, col, ErrorCode::E1133,
            specQualified, argType.getFullName(), specQualified);
    }
    if (_yux) {
        auto& checker = _yux->specImplChecker();
        TypeInfo concreteTI(concreteBare);
        // boundSatisfied 同时覆盖显式 impl (_seen) 与 #DraftLike 结构匹配
        std::vector<TypeInfo> specTypeArgs;
        if (!checker.boundSatisfied(concreteTI, specDecl, specQualified, specTypeArgs)) {
            throw YuxError(line, col, ErrorCode::E1133,
                specQualified, argType.getFullName(), specQualified);
        }
    }

    // ── codegen ──────────────────────────────────────────────────────
    // Phase 3a/3c：vtable 由 getOrEmitDynVTable 合成；data 槽按 Rc<U> / U& 形态抽取。
    // 注：Rc 的 +1 / 借用 RC 半权交接留 Phase 3c.2（消费临时帧 / retain 抵消），
    // 本 Phase 仅落 vtable 真值，临时帧路径与原占位等价。
    auto llvmDynTy = getLLVMType(resultType);

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    auto argVal = compileExpr(argExpr);

    // 抽取 data 槽：Rc<U> 取 handle 字段；U& 直接用
    // Phase 3e RC 交接：
    //   owned (Rc<U>) 形态：源 Rc 若是 fresh 临时（G() 直构），consumeTemp 偷取 +1；
    //     否则（命名变量 / 字段读出）调 _box_retain 拷一份 +1，源 Rc 自己照常 release。
    //     Dyn 在自身 scope 退出时走 _dyn_release 抵消。
    //   borrow (U&) 形态：data_ptr 借用，不动 RC（由源 owner 维持）。
    llvm::Value* dataPtr = nullPtr;
    if (argType.isRc()) {
        // Rc layout = { ptr handle }；handle 指向 [RC head | payload]
        auto rcLLVMTy = getLLVMType(argType);
        auto tmp = _builder.CreateAlloca(rcLLVMTy, nullptr, "dyn.src.rc.tmp");
        _builder.CreateStore(argVal, tmp);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(rcLLVMTy, tmp, {zero, zero}, "dyn.src.handle.ptr");
        dataPtr = _builder.CreateLoad(ptrTy, handleField, "dyn.src.handle");

        // 偷取 fresh Rc 的 +1，否则 retain
        bool consumed = consumeTemp(argVal);
        if (!consumed) {
            _builder.CreateCall(runtime::getRcRetainFn(_module, _builder), {dataPtr});
        }
    } else if (argType.isRef()) {
        // U& 已是裸指针类型，直接用
        dataPtr = argVal;
    }

    // vtable 槽：Phase 3a 真值（按 (U, D) 合成 linkonce_odr 全局）
    TypeInfo concreteTI(concreteBare);
    llvm::Value* vtablePtr = getOrEmitDynVTable(concreteTI, specQualified, specDecl);
    if (!vtablePtr) vtablePtr = nullPtr;

    // 组装 fat pointer struct value { vtable, data }
    llvm::Value* fatPtr = llvm::UndefValue::get(llvmDynTy);
    fatPtr = _builder.CreateInsertValue(fatPtr, vtablePtr, {0}, "dyn.vtable");
    fatPtr = _builder.CreateInsertValue(fatPtr, dataPtr, {1}, "dyn.fat");

    DEBUG_LOG_VAL("    Expr: DynCtor",
        resultType.getFullName() << " <- " << argType.getFullName());
    return fatPtr;
}


// 编译 Heap:<T>(x) 构造表达式（DRAFT-heap-types §8.3a）
// 形态：单参；arg 求值为 T 值。
// 流程：
// 1. 求值 arg → T 值
// 2. arg 类型必须等于 turbofish 内 T（E3014）
// 3. __yux_heap_alloc(sizeof T) → ptr
// 4. store T 值到 ptr
// 5. 返回 ptr（Heap<T> LLVM 表示 = 裸 T*）
// 注：作用域析构 / 字段析构 / as_ref / 借用检查留 Phase 2.5+
llvm::Value* Compiler::compileHeapCtorExpr(p<ExprHeapCtorNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    int line = node->getLineNumber();
    int col = node->getColumn();

    auto resultType = node->getType();
    auto innerSp = resultType.heapElementType();
    if (!innerSp) {
        throw YuxError(line, col, "Heap:<T>(x) 缺少类型实参");
    }
    auto innerType = *innerSp;

    auto argExpr = node->arg();
    if (isIntTypeName(innerType.name) && isFlexibleIntExpr(argExpr)) {
        tryInferIntType(argExpr, innerType);
    }
    auto argType = argExpr->getType();

    // Phase 8b: Heap:<T>(p Ptr) FFI take-over (DRAFT-heap-types §8.3a) —
    // T != Ptr 且 argType == Ptr 时, 直接把 Ptr 作为 Heap<T> 句柄接管,
    // 作用域尾走既有 Heap<T> dtor (releaseAtPtr(inner) + __yux_heap_free).
    // 责任方: 调用者必须保证 Ptr 指向 T-shape 有效内存且由 __yux_heap_alloc 分配.
    bool takeoverFromPtr = argType.isPtr() && innerType.name != "Ptr";
    if (!takeoverFromPtr && !(argType == innerType)) {
        // Sema 已在 visitExpr(ExprHeapCtorNode) 内 shadow 抛 E3028；保留作幂等防御性双跑
        throw YuxError(line, col, ErrorCode::E3028,
            innerType.name, innerType.name, argType.name);
    }

    auto argVal = compileExpr(argExpr);

    if (takeoverFromPtr) {
        DEBUG_LOG_VAL("    Expr: HeapCtor (FFI take-over)",
            resultType.getFullName() << " <- Ptr");
        return argVal;
    }

    auto innerLLVMType = getLLVMType(innerType);
    auto sizeVal = _builder.getInt64(
        _module->getDataLayout().getTypeAllocSize(innerLLVMType).getFixedValue());
    auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
    auto rawPtr = _builder.CreateCall(allocFn, {sizeVal}, "heap_payload");
    _builder.CreateStore(argVal, rawPtr);
    // fresh 临时 arg 的 +1 / RC 字段所有权转交给 Heap payload；
    // 否则帧弹出时 dtor 会与作用域尾 __yux_heap_free 前的 inner dtor 双释放（含 RC 字段时 use-after-free）
    consumeTemp(argVal);

    DEBUG_LOG_VAL("    Expr: HeapCtor",
        resultType.getFullName() << " <- " << argType.getFullName());
    return rawPtr;
}

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Lambda 字面量与 fn-value 调用编译实现（spec §4 / §5）
//
// Phase 2b：零捕获场景
// - compileLambdaExpr：LambdaExprNode → 16 字节 fat-ptr { fn_ptr, captures=null }
// - emitLambdaFunction：根据上下文反推后的 Fn 类型生成顶层匿名 LLVM Function；
//   ABI captures-leading：(Ptr captures, P1, ..., Pn) → R，零捕获场景 captures 形参未使用
// - compileFnValueCall：callee 静态类型为 Fn 时走 fat-ptr：extractvalue 取
//   fn_ptr / captures，按 ABI 调用
// - inferLambdaParamsFromFnType：实参位置 lambda 形参缺类型时按 callee fnParamTypes 回填

#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "compiler.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

// ==================== Lambda 形参类型反推 ====================
// 实参位置 lambda 反推：把 fn(...) 期望类型的形参列表回填到 LambdaExprNode 形参槽位。
// 仅当槽位 type==nullptr 时回填；retType 同理（仅在 expectedFnType 显式标注且 lambda 未标注时回填）。
// expectedFnType 必须是 isFn() 才会动作；否则 no-op。
void Compiler::inferLambdaParamsFromFnType(p<LambdaExprNode> lambda, const TypeInfo& expectedFnType) {
    if (!lambda || !expectedFnType.isFn()) return;
    auto& slots = lambda->mutableParams();
    const auto& expectedParams = expectedFnType.fnParamTypes();
    // arity 不一致由 sema 报错（Phase 2c），这里仅按位置回填能填的部分
    size_t n = std::min(slots.size(), expectedParams.size());
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].type) continue;      // 已标注：尊重源
        if (!expectedParams[i]) continue; // 期望也是空：不回填
        // 把 TypeInfo 包成 TypeNormalNode 占位（codegen 仅取 getType()，name 即可定位 LLVM 类型）
        // 注意：复杂类型（Generic/Array/Tuple/Fn）经 getFullName 后字符串无法被 TypeNormalNode
        // 还原；Phase 2b 仅覆盖 Normal/Generic 通过名称还原的常见情形。
        // TODO: 引入"已解析 TypeInfo 直挂载"的 TypeNode 变体，让任意 TypeInfo 都能反推
        Token tok = lambda->params()[i].name;
        tok = Token(tok); // 复制，作为占位
        // 直接重写 token 文本为期望类型 name
        // 这里用字符串名能覆盖 i32/u32/bool/String/struct 等 Normal 类型；
        // 对 Generic/Array/Fn 形参当前无法精确还原，留 TODO
    }
    // Phase 2b：暂不真的回填（避免引入伪 TypeNode）；emitLambdaFunction 走"直接看 expectedFnType"
    // 路径，从 expectedFnType.fnParamTypes() 拿类型即可。这里函数仅占位，便于未来扩展。
    (void)expectedFnType;
}

// ==================== emitLambdaFunction ====================
// 给定 LambdaExprNode 与"上下文期望类型"，生成顶层匿名 fn 函数。
// 期望类型给出形参 LLVM 类型（lambda 形参缺类型时按它取）；返回类型同理。
// 同一 LambdaExprNode 多次求值（罕见）按 mangle 名缓存；返回 llvm::Function*。
llvm::Function* Compiler::emitLambdaFunction(p<LambdaExprNode> node, const TypeInfo& expectedFnType) {
    int line = node->getLineNumber();
    int col = node->getColumn();
    string mod = _file ? _file->moduleName() : string();
    string mangled = Mangler::lambda(mod, line, col);

    if (auto fn = _module->getFunction(mangled)) {
        return fn;
    }

    // 解析形参类型：lambda 槽位优先；缺失则回退到 expectedFnType.fnParamTypes()
    vector<TypeInfo> paramTypes;
    paramTypes.reserve(node->params().size());
    for (size_t i = 0; i < node->params().size(); ++i) {
        const auto& slot = node->params()[i];
        if (slot.type) {
            paramTypes.push_back(slot.type->getType());
        } else if (expectedFnType.isFn() && i < expectedFnType.fnParamTypes().size() &&
                   expectedFnType.fnParamTypes()[i]) {
            paramTypes.push_back(*expectedFnType.fnParamTypes()[i]);
        } else {
            // 缺类型且无上下文 → 报错；Phase 2c 由 sema 更早拒
            throw YuxError(line, col, ErrorCode::E3091);
        }
    }

    // 解析返回类型（spec §4.2）：
    // - 显式标注（仅 Paren 形态可写）→ 严格用之
    // - Paren 无标注 → 默认 void（不接受上下文反推；自描述形态）
    // - 裸形（Single/Block/ZeroBlock）无标注 → 上下文反推；无上下文 → void（兜底）
    TypeInfo retType;
    if (node->retType()) {
        retType = node->retType()->getType();
    } else if (node->form() == LambdaExprNode::Form::Paren) {
        // 括 + 无标 → void（spec §4.2 [#14]）
        // body 表达式被作"语句位置丢弃"
    } else if (expectedFnType.isFn() && expectedFnType.fnReturnType()) {
        retType = *expectedFnType.fnReturnType();
    }
    // retType.empty() == void

    // 构造 LLVM 函数签名：(Ptr captures, P1, ..., Pn) → R
    auto ptrTy = llvm::PointerType::get(_context, 0);
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy); // captures（零捕获场景未使用）
    for (auto& pt : paramTypes) {
        llvmParamTypes.push_back(getLLVMType(pt));
    }
    auto llvmRet = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
    auto fnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    auto func = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, mangled, _module);

    // 保存当前编译状态；lambda 是顶层 fn，不复用外层 _localVarPtrs / _scopeVars
    auto savedFn = _currentFn;
    auto savedFnNode = _currentFnNode;
    auto savedStruct = _currentStructName;
    auto savedLocals = std::move(_localVarPtrs);
    auto savedScope = std::move(_scopeVars);
    auto savedTempStack = std::move(_tempStack);
    auto savedInsert = _builder.GetInsertBlock();
    auto savedInsertPoint = _builder.GetInsertPoint();
    auto savedLambdaBodyScope = _currentLambdaBodyScope;
    auto savedLambdaForCapture = _currentLambdaForCapture;
    auto savedLambdaCapturesArg = _currentLambdaCapturesArg;

    _currentFn = func;
    _currentFnNode = nullptr; // lambda 无 FnNode；body 引用外层符号走 Phase 4a 捕获通道
    _currentStructName.clear();
    _localVarPtrs.clear();
    _scopeVars.clear();
    _movedVars.clear(); // Phase B-1
    _tempStack.clear();
    _currentLambdaBodyScope = node->bodyScope(); // Phase 2c：启用 FV 通路
    // Phase 4a：启用捕获识别。清空旧 captures（防止重复 emit 累加；缓存命中走早返路径
    // 不重入此段）。emitLambdaFunction 这一次 body 编译期间，compileLiteralExpr 命中
    // 外层 local 会 addCapture 并就地 GEP 读 captures buffer。
    node->clearCaptures();
    _currentLambdaForCapture = node;

    auto entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);

    // 形参 alloca + store；首参 captures 跳过（零捕获场景未使用 / 含捕获走捕获通道）
    auto argIt = func->arg_begin();
    argIt->setName("captures");
    // Phase 4c：可能传入 LSB 标 1 的栈嵌入指针（compileLambdaExpr 在 hasRefCapture 时
    // 把 alloca | 1 写入 fat-ptr）。body 内 GEP 必须用未标记的指针；在入口处统一掩 LSB 一次：
    //   - heap 句柄（_box_alloc 返回）8 字节对齐，LSB=0，掩 LSB 不变；
    //   - 栈 alloca | 1，掩 LSB 还原为真实 alloca 指针；
    //   - null（零捕获）保持 null（不会被访问）。
    // 由此 _currentLambdaCapturesArg 始终是"可直接 GEP 的真实 payload 指针"。
    auto ptrTyMask = llvm::PointerType::get(_context, 0);
    auto i64Ty = _builder.getInt64Ty();
    auto rawAsInt = _builder.CreatePtrToInt(&*argIt, i64Ty, "captures.asint");
    auto maskedInt = _builder.CreateAnd(rawAsInt, _builder.getInt64(~static_cast<u64>(1)), "captures.untagged.asint");
    auto maskedPtr = _builder.CreateIntToPtr(maskedInt, ptrTyMask, "captures.untagged");
    _currentLambdaCapturesArg = maskedPtr;
    ++argIt;
    for (size_t i = 0; i < paramTypes.size(); ++i, ++argIt) {
        auto paramName = node->params()[i].name.getText();
        argIt->setName(paramName);
        auto llvmType = getLLVMType(paramTypes[i]);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
        _builder.CreateStore(&*argIt, alloca);
        _localVarPtrs[paramName] = alloca;
    }

    // 编译 body
    // - Single / Paren：单表达式 → ret expr（void 返回类型时 ret void）
    // - Block / ZeroBlock：语句序列 → 顺序编译；retType 非 void 时若末位是无 `;` 的
    //   ExprStmt，当 tail-expr 返回值（spec §4.0 "0 参块与 ≥1 参块同构"，BUG#0 修复）；
    //   否则末尾走隐式 ret void / 缺显式 ret 报错
    pushTempFrame();
    if (node->bodyExpr()) {
        auto val = compileExpr(node->bodyExpr());
        if (!_builder.GetInsertBlock()->getTerminator()) {
            if (retType.empty()) {
                popAndReleaseTempFrame();
                _builder.CreateRetVoid();
            } else {
                popAndReleaseTempFrame();
                _builder.CreateRet(val);
            }
        } else {
            popAndReleaseTempFrame();
        }
    } else {
        const auto& stmts = node->bodyStmts();
        // BUG#0：peel 末位无 `;` 的 ExprStmt 当 tail-expr return；仅在 retType 非 void 时启用
        p<ExprNode> tailExpr = nullptr;
        size_t nStmts = stmts.size();
        if (!retType.empty() && nStmts > 0) {
            if (auto exprStmt = dynamic_cast<StatementExprNode*>(stmts.back())) {
                if (!exprStmt->hasSemicolon() && !dynamic_cast<StatementRetNode*>(stmts.back())) {
                    tailExpr = exprStmt->expr();
                    --nStmts;
                }
            }
        }
        for (size_t i = 0; i < nStmts; ++i) {
            compileStatement(stmts[i]);
        }
        if (!_builder.GetInsertBlock()->getTerminator()) {
            if (tailExpr) {
                auto val = compileExpr(tailExpr);
                popAndReleaseTempFrame();
                _builder.CreateRet(val);
            } else if (retType.empty()) {
                popAndReleaseTempFrame();
                _builder.CreateRetVoid();
            } else {
                // 缺显式 ret 且非 void：报错。Phase 2c 由 sema 更早拒
                popAndReleaseTempFrame();
                throw YuxError(line, col, ErrorCode::E3091);
            }
        } else {
            popAndReleaseTempFrame();
        }
    }

    // 恢复外层编译状态
    _currentFn = savedFn;
    _currentFnNode = savedFnNode;
    _currentStructName = savedStruct;
    _localVarPtrs = std::move(savedLocals);
    _scopeVars = std::move(savedScope);
    _tempStack = std::move(savedTempStack);
    _currentLambdaBodyScope = savedLambdaBodyScope;
    _currentLambdaForCapture = savedLambdaForCapture;
    _currentLambdaCapturesArg = savedLambdaCapturesArg;
    if (savedInsert) {
        _builder.SetInsertPoint(savedInsert, savedInsertPoint);
    }

    return func;
}

// ==================== emitCapturesDtorFunction ====================
// Phase 4a-2：合成 lambda captures 的字段级析构。当且仅当 captures 列表里有
// 至少一个 needs-destructor 字段（堆句柄）时返回非 null Function；全标量场景
// 返回 nullptr（dtor 槽存 null，_box_release_dtor 跳过 dispatch）。
//
// 签名：void __captures_dtor_<lambdaMangle>(ptr fields_base)
// fields_base 指向 capture 字段区起点（= block handle + 16），逐 capture 调
// releaseAtPtr 释放槽内的句柄字段。
llvm::Function* Compiler::emitCapturesDtorFunction(p<LambdaExprNode> node, const string& lambdaMangled) {
    bool anyNeedsDtor = false;
    for (const auto& cap : node->captures()) {
        if (typeNeedsDestructor(cap.type)) {
            anyNeedsDtor = true;
            break;
        }
    }
    if (!anyNeedsDtor) return nullptr;

    string fnName = "__captures_dtor_" + lambdaMangled;
    if (auto existing = _module->getFunction(fnName)) return existing;

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto fnTy = llvm::FunctionType::get(_builder.getVoidTy(), {ptrTy}, false);
    auto func = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, fnName, _module);

    // 保存 / 恢复构建器状态
    auto savedInsert = _builder.GetInsertBlock();
    auto savedInsertPoint = _builder.GetInsertPoint();

    auto entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);

    auto fieldsBase = &*func->arg_begin();
    fieldsBase->setName("fields_base");

    auto i8Ty = _builder.getInt8Ty();
    for (const auto& cap : node->captures()) {
        if (!typeNeedsDestructor(cap.type)) continue;
        auto offset = _builder.getInt64(static_cast<i64>(cap.byteOffset));
        auto slotPtr = _builder.CreateGEP(i8Ty, fieldsBase, {offset}, "cap.slot");
        // captures 槽位存的就是 wrapper struct（{ptr handle} 等），releaseAtPtr 直接走
        releaseAtPtr(slotPtr, cap.type);
    }
    _builder.CreateRetVoid();

    if (savedInsert) {
        _builder.SetInsertPoint(savedInsert, savedInsertPoint);
    }
    return func;
}

// ==================== compileLambdaExpr ====================
// LambdaExprNode 求值：先 emit 底层 fn（期间 emit 通路完成 captures 槽位发现），
// 回到外层上下文后据 captures 列表分配 captures Rc + 写入字段；最后构造 fat-ptr。
// fat-ptr layout：{ ptr fn_ptr, ptr captures }；零捕获 captures = null。
//
// Phase 4a-2 captures Rc 布局（由 _box_release_dtor 配合）：
//   [handle+0..8]  RC 头（strong / weak）
//   [handle+8..16] dtor fn ptr（null 表示无字段需析构）
//   [handle+16..]  capture 字段区，每槽 8 字节，按 byteOffset 寻址
//
// 含堆句柄 captures：调用站点 retain 后写入槽位；strong 归零时 _box_release_dtor
// 调 dtor 释放每个堆句柄字段，再 free。多 fat-ptr 副本共享 Rc 时不会过早析构。
llvm::Value* Compiler::compileLambdaExpr(p<LambdaExprNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // 静态类型即 Fn TypeInfo（lambda 形参类型可能缺）
    auto fnType = node->getType();
    auto func = emitLambdaFunction(node, fnType);

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto fatStructTy = llvm::StructType::get(_context, {ptrTy, ptrTy});

    // captures：零捕获 → null；含捕获 → 分两条路径
    //   - hasRefCapture（spec §6.3）：栈嵌入 alloca + 不构造 Rc，captures 字段标 LSB=1
    //   - 否则：堆 _box_alloc + dtor，与 Phase 4a/4a-2 同
    // layout 统一保留 16 字节前缀（offset 0..16 给 RC 头 / dtor 槽；栈形态浪费），
    // capture 字段从 +16 起；body GEP base offset 不依赖路径选择。
    llvm::Value* capturesPtr = llvm::ConstantPointerNull::get(ptrTy);
    const auto& caps = node->captures();
    if (!caps.empty()) {
        auto i8Ty = _builder.getInt8Ty();
        auto i64Ty = _builder.getInt64Ty();
        u64 prefixSize = 16;
        u64 payloadSize = prefixSize + node->capturesTotalSize();
        bool stackEmbedded = node->hasRefCapture();

        // 4c 限制：栈嵌入路径下不允许混入堆句柄字段（混合释放路径未实现）
        if (stackEmbedded) {
            for (const auto& cap : caps) {
                if (typeNeedsDestructor(cap.type)) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E2029, cap.name,
                                   cap.type.getFullName());
                }
            }
        }

        llvm::Value* baseHandle;
        if (stackEmbedded) {
            // 栈分配 [16 前缀 + 字段]；前缀字节不被读，但保 layout 统一
            auto* parentFn = _builder.GetInsertBlock()->getParent();
            auto& entryBB = parentFn->getEntryBlock();
            llvm::IRBuilder<> entryBuilder(&entryBB, entryBB.getFirstInsertionPt());
            auto bytesTy = llvm::ArrayType::get(i8Ty, payloadSize);
            auto allocaPtr = entryBuilder.CreateAlloca(bytesTy, nullptr, "captures.stack");
            baseHandle = allocaPtr;
        } else {
            // payload_size 给 _box_alloc 是不含 RC 头的字节数；前缀里的 dtor 槽（8 字节）算 payload，
            // RC 头由 _box_alloc 自己加。即 payload = 8（dtor 槽）+ capturesTotalSize。
            u64 rcPayloadSize = 8 + node->capturesTotalSize();
            auto allocFn = runtime::getRcAllocFn(_module, _builder);
            baseHandle =
                _builder.CreateCall(allocFn, {_builder.getInt64(static_cast<i64>(rcPayloadSize))}, "captures.block");

            // 写 dtor 槽位 @ handle+8（_box_release_dtor 在 strong 归零时调用）
            string mod = _file ? _file->moduleName() : string();
            string mangled = Mangler::lambda(mod, node->getLineNumber(), node->getColumn());
            auto dtorFn = emitCapturesDtorFunction(node, mangled);
            auto dtorSlot = _builder.CreateGEP(i8Ty, baseHandle, {_builder.getInt64(8)}, "captures.dtor_slot");
            if (dtorFn) {
                _builder.CreateStore(dtorFn, dtorSlot);
            } else {
                _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), dtorSlot);
            }
        }

        // 写入各 capture：从 _localVarPtrs 加载值/取指针，存到 handle+16+byteOffset
        for (const auto& cap : caps) {
            auto it = _localVarPtrs.find(cap.name);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3030, cap.name);
            }
            auto offset = _builder.getInt64(16 + static_cast<i64>(cap.byteOffset));
            auto dstAddr = _builder.CreateGEP(i8Ty, baseHandle, {offset}, "cap.dst");
            if (cap.type.isRef()) {
                // T& 捕获：_localVarPtrs[name] 即 inner T 指针（参数/局部统一），
                // 写 8 字节指针；不 retain（借用语义，无所有权迁移）
                _builder.CreateStore(it->second, dstAddr);
            } else {
                // Phase 3e: Heap<T>? 捕获走 B 档 move 语义 (草案 §5.5 / Phase 5)
                // —— 不 retain (retainHandleAtCallSite 本就无 Heap 分支), 写入 env 后
                // 把 outer slot 写 {_has=false, _value=null}; 让 outer scope 尾 dtor
                // 看 _has=false 跳 free, env 独占所有权 (与 §5.3 字段 move-out 同款).
                bool isHeapNullableCap = false;
                if (cap.type.isNullable()) {
                    auto inner = cap.type.nullableInnerType();
                    if (inner && inner->isHeap()) isHeapNullableCap = true;
                }
                auto valLLVMTy = getLLVMType(cap.type);
                auto srcVal = _builder.CreateLoad(valLLVMTy, it->second, "cap.src");
                _builder.CreateStore(srcVal, dstAddr);
                // B-3: #NoCopy 类型（Array / 自定义 #NoCopy struct）捕获 → move 语义：
                // 值已移入 captures buffer，源 alloca 清零防外层析构 double-free。
                // 置于 retain 分支之前：NoCopy 无 RC，retain 路径对它们无意义。
                bool isNoCopyCap = isNoCopyType(cap.type);
                if (isHeapNullableCap) {
                    auto z0 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    auto z1 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
                    auto hasField = _builder.CreateGEP(valLLVMTy, it->second, {z0, z0}, "cap.bdang.has");
                    auto valField = _builder.CreateGEP(valLLVMTy, it->second, {z0, z1}, "cap.bdang.value");
                    _builder.CreateStore(_builder.getInt1(false), hasField);
                    _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), valField);
                } else if (isNoCopyCap) {
                    // #NoCopy 类型：值已移入 captures，源写零防 double-free
                    auto zeroVal = llvm::ConstantAggregateZero::get(valLLVMTy);
                    _builder.CreateStore(zeroVal, it->second);
                } else if (typeNeedsDestructor(cap.type)) {
                    // 堆句柄按 callee-clean 习惯 retain（与 retainHandleAtCallSite 同款逻辑）
                    // —— 仅 Rc 路径需要；栈嵌入路径已在前面拒绝了 needs-dtor 字段
                    retainHandleAtCallSite(srcVal, cap.type);
                }
            }
        }

        if (stackEmbedded) {
            // captures 标 LSB=1，标记"栈嵌入，跳过 RC 操作"。release / retain 站点检测此位即跳过。
            // body 入口已统一掩 LSB；其余调用路径（compileFnValueCall）只把 captures 透传给 fn，
            // 不解读其 RC 头，故对 LSB 标记透明。
            auto baseInt = _builder.CreatePtrToInt(baseHandle, i64Ty, "captures.stack.asint");
            auto taggedInt = _builder.CreateOr(baseInt, _builder.getInt64(1), "captures.tagged.asint");
            capturesPtr = _builder.CreateIntToPtr(taggedInt, ptrTy, "captures.tagged");
        } else {
            capturesPtr = baseHandle;
        }
    }

    // ConstantStruct 不能直接用：Function* 是 Constant 但 fatStructTy 是 anonymous struct，
    // ConstantStruct::get 需要 named struct。统一走 InsertValue 动态构造。
    llvm::Value* fat = llvm::UndefValue::get(fatStructTy);
    fat = _builder.CreateInsertValue(fat, func, {0}, "fn.ptr");
    fat = _builder.CreateInsertValue(fat, capturesPtr, {1}, "fn.captures");
    return fat;
}

// ==================== compileFnValueCall ====================
// callee 静态类型为 Fn(...)R 的调用站点：
// 1) 实参位置 lambda 走 inferLambdaParamsFromFnType 反推（如有）
// 2) 编译 callee 得到 fat-ptr 值
// 3) extractvalue 取 fn_ptr / captures
// 4) 编译实参 + CreateCall(fnType, fn_ptr, [captures, args...])
llvm::Value* Compiler::compileFnValueCall(p<ExprCallNode> node) {
    auto calleeExpr = node->getCalleeExpr();
    auto fnType = resolveAlias(calleeExpr->getType());
    if (!fnType.isFn()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    const auto& expectedParams = fnType.fnParamTypes();

    // 实参 lambda 反推：把 expectedParams[i] 回填到 LambdaExprNode 实参的 Fn 类型槽
    // 当前 inferLambdaParamsFromFnType 占位；emitLambdaFunction 已能从 expected 取类型，
    // 所以这里只需把"实参 lambda 的 expected 类型"通过 ExprCallNode 的 fnType 传递下去 —— 完成
    for (size_t i = 0; i < node->getArgs().size() && i < expectedParams.size(); ++i) {
        auto arg = node->getArgs()[i];
        auto lambdaArg = dynamic_cast<LambdaExprNode*>(arg);
        if (!lambdaArg) continue;
        if (!expectedParams[i]) continue;
        // 直接预 emit lambda function：让它走 expected 反推路径
        // 这样后续 compileExpr(arg) 走 compileLambdaExpr 时 emitLambdaFunction 命中缓存
        emitLambdaFunction(lambdaArg, *expectedParams[i]);
    }

    // 编译 callee → fat-ptr 16 字节
    auto fatPtr = compileExpr(calleeExpr);
    if (!fatPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto fnPtrVal = _builder.CreateExtractValue(fatPtr, {0}, "fn.ptr");
    auto captures = _builder.CreateExtractValue(fatPtr, {1}, "fn.captures");

    // 构造 LLVM FunctionType：(Ptr captures, P1, ..., Pn) → R
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);
    for (auto& pt : expectedParams) {
        if (!pt) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }
        llvmParamTypes.push_back(getLLVMType(*pt));
    }
    llvm::Type* llvmRet = _builder.getVoidTy();
    if (auto rt = fnType.fnReturnType()) {
        llvmRet = getLLVMType(*rt);
    }
    auto llvmFnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    // 实参类型校验：实参类型必须与形参类型严格匹配
    // Ref<T> 不能隐式转为 T，需手动 copy_of 或 let 暂存后取值
    for (size_t idx = 0; idx < node->getArgs().size() && idx < expectedParams.size(); ++idx) {
        try {
            auto argType = node->getArgs()[idx]->getType();
            if (expectedParams[idx] && !argType.name.empty() && argType.name != "Self" &&
                expectedParams[idx]->name != "Self") {
                // Ref<T> 实参传给值类型形参 T：类型不匹配
                if (argType.isRef() && !expectedParams[idx]->isRef()) {
                    auto inner = argType.refElementType();
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                              "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                              inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                              inner ? inner->name : "?", inner ? inner->name : "?"));
                }
                // 值类型实参与值类型形参不匹配
                if (!argType.isRef() && !expectedParams[idx]->isRef() && argType != *expectedParams[idx]) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", argType.getFullName(),
                                              expectedParams[idx]->getFullName()));
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 失败 (lambda 形参未推断等)：留后续 codegen 兜底
        }
    }

    // 编译实参（含 lambda → 构造 fat-ptr）
    vector<llvm::Value*> callArgs;
    callArgs.push_back(captures);
    for (auto i : node->getArgs()) {
        callArgs.push_back(compileExpr(i));
    }

    return _builder.CreateCall(llvmFnType, fnPtrVal, callArgs);
}

// ==================== compileRcFnValueCall ====================
// callee 静态类型为 Rc<fn(...)R>：自动解引取 fat-ptr 后走 fn-value-call。
// Rc payload = handle + 8 字节（跳过 refcount 头），其上存放 16 字节 fat-ptr。
// 1) 实参 lambda 反推（按 innerFnType.fnParamTypes()）
// 2) 编译 callee 得到 Rc 值（{ ptr handle }），extractvalue 取 handle
// 3) payload_ptr = handle + 8；load fat-ptr 16 字节
// 4) 走与 compileFnValueCall 相同的 extractvalue + CreateCall 路径
llvm::Value* Compiler::compileRcFnValueCall(p<ExprCallNode> node, const TypeInfo& innerFnType) {
    if (!innerFnType.isFn()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    const auto& expectedParams = innerFnType.fnParamTypes();

    // 实参 lambda 预 emit（与 compileFnValueCall 一致）
    for (size_t i = 0; i < node->getArgs().size() && i < expectedParams.size(); ++i) {
        auto arg = node->getArgs()[i];
        auto lambdaArg = dynamic_cast<LambdaExprNode*>(arg);
        if (!lambdaArg) continue;
        if (!expectedParams[i]) continue;
        emitLambdaFunction(lambdaArg, *expectedParams[i]);
    }

    // 编译 callee 得到 Rc 值（struct { ptr handle }）；extractvalue 取 handle
    auto rcVal = compileExpr(node->getCalleeExpr());
    if (!rcVal) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto handle = _builder.CreateExtractValue(rcVal, {0}, "rc.fn.handle");

    // payload_ptr = handle + 8 bytes（跳过 refcount 头，与 compileExpr Rc.field 路径一致）
    auto payloadPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.fn.payload");

    // load fat-ptr 16 字节 { fn_ptr, captures }
    auto fatStructTy = llvm::StructType::get(_context, {ptrTy, ptrTy});
    auto fatPtr = _builder.CreateLoad(fatStructTy, payloadPtr, "rc.fn.fatptr");
    auto fnPtrVal = _builder.CreateExtractValue(fatPtr, {0}, "fn.ptr");
    auto captures = _builder.CreateExtractValue(fatPtr, {1}, "fn.captures");

    // 构造 LLVM FunctionType：(Ptr captures, P1, ..., Pn) → R
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);
    for (auto& pt : expectedParams) {
        if (!pt) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }
        llvmParamTypes.push_back(getLLVMType(*pt));
    }
    llvm::Type* llvmRet = _builder.getVoidTy();
    if (auto rt = innerFnType.fnReturnType()) {
        llvmRet = getLLVMType(*rt);
    }
    auto llvmFnType = llvm::FunctionType::get(llvmRet, llvmParamTypes, false);

    // 实参类型校验（与 compileFnValueCall 同款检查）
    for (size_t idx = 0; idx < node->getArgs().size() && idx < expectedParams.size(); ++idx) {
        try {
            auto argType = node->getArgs()[idx]->getType();
            if (expectedParams[idx] && !argType.name.empty() && argType.name != "Self" &&
                expectedParams[idx]->name != "Self") {
                if (argType.isRef() && !expectedParams[idx]->isRef()) {
                    auto inner = argType.refElementType();
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                              "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                              inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                              inner ? inner->name : "?", inner ? inner->name : "?"));
                }
                if (!argType.isRef() && !expectedParams[idx]->isRef() && argType != *expectedParams[idx]) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", argType.getFullName(),
                                              expectedParams[idx]->getFullName()));
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 失败: 留后续 codegen 兜底
        }
    }

    // 编译实参
    vector<llvm::Value*> callArgs;
    callArgs.push_back(captures);
    for (auto i : node->getArgs()) {
        callArgs.push_back(compileExpr(i));
    }

    return _builder.CreateCall(llvmFnType, fnPtrVal, callArgs);
}

// v0.16: callee 为 Ref<fn(...)R>（如 arr[i] 返回 fn&）的调用站点：
// 1) 实参位置 lambda 走 inferLambdaParamsFromFnType 反推（如有）
// 2) compileExpr 得到 T& 指针（指向 fat-ptr）
// 3) Load fat-ptr 16 字节 { fn_ptr, captures }
// 4) extractvalue 取 fn_ptr / captures
// 5) 编译实参 + CreateCall(fnType, fn_ptr, [captures, args...])
llvm::Value* Compiler::compileRefFnValueCall(p<ExprCallNode> node, const TypeInfo& innerFnType) {
    if (!innerFnType.isFn()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    const auto& expectedParams = innerFnType.fnParamTypes();

    // 实参 lambda 预 emit（与 compileFnValueCall 一致）
    for (size_t i = 0; i < node->getArgs().size() && i < expectedParams.size(); ++i) {
        auto arg = node->getArgs()[i];
        auto lambdaArg = dynamic_cast<LambdaExprNode*>(arg);
        if (!lambdaArg) continue;
        if (!expectedParams[i]) continue;
        emitLambdaFunction(lambdaArg, *expectedParams[i]);
    }

    // compileExpr 得到 T& 指针（arr[i] 返回 Ref<fn> = 指向 fat-ptr 的指针）
    auto refPtr = compileExpr(node->getCalleeExpr());
    if (!refPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
    auto ptrTy = llvm::PointerType::get(_context, 0);

    // Load fat-ptr { fn_ptr, captures } 从 T& 指针
    auto fatStructTy2 = llvm::StructType::get(_context, {ptrTy, ptrTy});
    auto fatPtr2 = _builder.CreateLoad(fatStructTy2, refPtr, "ref.fn.fatptr");
    auto fnPtrVal2 = _builder.CreateExtractValue(fatPtr2, {0}, "fn.ptr");
    auto captures2 = _builder.CreateExtractValue(fatPtr2, {1}, "fn.captures");

    // 构造 LLVM FunctionType：(Ptr captures, P1, ..., Pn) → R
    vector<llvm::Type*> llvmParamTypes2;
    llvmParamTypes2.push_back(ptrTy);
    for (auto& pt : expectedParams) {
        if (!pt) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }
        llvmParamTypes2.push_back(getLLVMType(*pt));
    }
    llvm::Type* llvmRet2 = _builder.getVoidTy();
    if (auto rt = innerFnType.fnReturnType()) {
        llvmRet2 = getLLVMType(*rt);
    }
    auto llvmFnType2 = llvm::FunctionType::get(llvmRet2, llvmParamTypes2, false);

    // 实参类型校验（与 compileFnValueCall 同款检查）
    for (size_t idx = 0; idx < node->getArgs().size() && idx < expectedParams.size(); ++idx) {
        try {
            auto argType = node->getArgs()[idx]->getType();
            if (expectedParams[idx] && !argType.name.empty() && argType.name != "Self" &&
                expectedParams[idx]->name != "Self") {
                if (argType.isRef() && !expectedParams[idx]->isRef()) {
                    auto inner = argType.refElementType();
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                              "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                              inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                              inner ? inner->name : "?", inner ? inner->name : "?"));
                }
                if (!argType.isRef() && !expectedParams[idx]->isRef() && argType != *expectedParams[idx]) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3014,
                                   expectedParams[idx]->getFullName(), argType.getFullName())
                        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", argType.getFullName(),
                                              expectedParams[idx]->getFullName()));
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 失败: 留后续 codegen 兜底
        }
    }

    // 编译实参
    vector<llvm::Value*> callArgs2;
    callArgs2.push_back(captures2);
    for (auto i : node->getArgs()) {
        callArgs2.push_back(compileExpr(i));
    }

    return _builder.CreateCall(llvmFnType2, fnPtrVal2, callArgs2);
}

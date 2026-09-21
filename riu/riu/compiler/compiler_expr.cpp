// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 表达式编译实现
//
// compileExpr 经 AstVisitor::accept 分派到 visitX（4.3）。
// 本文件含 compileExpr 入口、visit* 与部分表达式 IR（字面量分发、struct lit、语句块、move-assign）。

#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/riu.h"
#include "compiler.h"
#include "compiler_runtime.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <memory>
#include <set>

// ==================== 辅助函数 ====================

// Phase 2.4 Sema/Codegen 拆分：codegen 读类型的统一入口。详见 compiler.h 注释。
// visitCall / visitArray / visitPathCall 在 accept 返回前 recordTemp，读 resolvedOrInferredType。
TypeInfo Compiler::resolvedOrInferredType(ExprNode* node) const {
    // 泛型 AST 会被多个具体实例复用，节点上的 resolvedType 只保存最近一次
    // SemaPass 复查结果。subst 帧里始终用 structuralType() 再替换，不读槽。
    if (!_substStack.empty()) {
        auto inferred = node->structuralType();
        if (node->hasResolvedType()) {
            const auto& resolved = node->resolvedType();
            // 数组字面量 structural 是 `[T * N]` / `[__empty * 0]`；Sema 按靶向写成 Array<T>。
            const bool arrayLitToGeneric = inferred.isArray() && resolved.isArrayGeneric();
            const bool ctrlArrayTarget =
                resolved.isArrayGeneric() && inferred.isArray() &&
                (dynamic_cast<const ExprIfElseNode*>(node) || dynamic_cast<const ExprOneLineIfElseNode*>(node) ||
                 dynamic_cast<const ExprMatchNode*>(node) || dynamic_cast<const ExprTryCatchNode*>(node));
            if (arrayLitToGeneric || ctrlArrayTarget) return applySubst(resolved);
        }
        return applySubst(inferred);
    }
    if (node->hasResolvedType()) {
#ifndef NDEBUG
        const auto& resolved = node->resolvedType();
        auto inferred = node->structuralType();
        // 数组字面量 getType 是 `[T * N]` / `[__empty * 0]`；SemaPass 按靶向可写成 Array<T>。
        const bool arrayLitToGeneric = inferred.isArray() && resolved.isArrayGeneric();
        // if / match / try 汇合：子节点已按靶向写成 Array，节点自身 getType 在写 resolved 前
        // 可能仍见字面量的 `[T * N]`；assert 放行，codegen 读 resolved。
        const bool ctrlArrayTarget =
            resolved.isArrayGeneric() && inferred.isArray() &&
            (dynamic_cast<const ExprIfElseNode*>(node) || dynamic_cast<const ExprOneLineIfElseNode*>(node) ||
             dynamic_cast<const ExprMatchNode*>(node) || dynamic_cast<const ExprTryCatchNode*>(node));
        // 透明 alias (spec §3.9.1.2 / §3.9.3.1): SemaPass 缓存的 resolvedType 与 codegen
        // 阶段 getType() 重新计算的结果, 在字面上可能一侧是别名名 (`IPair`), 另一侧已被
        // 解开 (`(i32,i32)`). 两者按 alias 归一后应一致; 仅当归一后仍不等才视为真冲突.
        const bool consistent =
            resolveAlias(resolved) == resolveAlias(inferred) || arrayLitToGeneric || ctrlArrayTarget;
        assert(consistent && "resolvedType / getType inconsistent");
#endif
        return node->resolvedType();
    }
    return node->getType();
}

// ==================== 类型转换 ====================

// 创建类型转换
// 处理整数、浮点数、指针、引用等类型之间的转换
// NOLINTBEGIN(bugprone-branch-clone)
llvm::Value* Compiler::createCast(llvm::Value* val, const TypeInfo& rawSrc, const TypeInfo& rawDst) {
    const TypeInfo& srcType = rawSrc.withoutFallible();
    const TypeInfo& dstType = rawDst.withoutFallible();
    // 相同类型无需转换（含透明别名：IntUnOp = Function<i32, i32>）
    if (srcType == dstType || resolveAlias(srcType) == resolveAlias(dstType)) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType.name);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType.name << " -> " << dstType.name);

    auto dstLLVMType = getLLVMType(dstType);
    auto srcLLVMType = getLLVMType(srcType);
    if (srcLLVMType == dstLLVMType) {
        DEBUG_LOG_VAL("    Cast: no-op (same LLVM type)", srcType.name << " -> " << dstType.name);
        return val;
    }

    // 指针类型转换
    if (dstType.isPtr()) {
        if (srcType.isRef()) {
            DEBUG_LOG("      Ref -> Ptr");
            return _builder.CreateBitCast(val, llvm::PointerType::get(_context, 0), "ref_to_ptr");
        }
        if (srcType.isRc()) {
            DEBUG_LOG("      Rc -> Ptr");
            // Rc -> Ptr：取 payload 首地址（DRAFT §9.4 跳过 RC 头）
            // payload = handle + 8
            auto rcStructType = getLLVMType(srcType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, val, {zero, zero}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }
    }

    // 数值类型转换
    bool srcIsFloat = srcType.isFloat();
    bool dstIsFloat = dstType.isFloat();
    bool srcIsUnsigned = srcType.isUnsigned();
    bool dstIsUnsigned = dstType.isUnsigned();

    if (srcIsFloat && dstIsFloat) {
        // 浮点数之间的转换
        if (srcType.name == "f64" && dstType.name == "f32") { // NOLINT(bugprone-branch-clone)
            DEBUG_LOG("      FPTrunc (f64 -> f32)");
            return _builder.CreateFPTrunc(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPExt (f32 -> f64)");
            return _builder.CreateFPExt(val, dstLLVMType);
        }
    } else if (!srcIsFloat && !dstIsFloat) {
        // 整数之间的转换

        // bool 与整数的语义转换：不能用 trunc/sext，必须 icmp ne 0 / zext
        if (dstType.name == "bool") {
            // int -> bool：非零为 true，零为 false
            DEBUG_LOG("      ICmpNE 0 (int -> bool)");
            return _builder.CreateICmpNE(val, llvm::ConstantInt::get(srcLLVMType, 0));
        }
        if (srcType.name == "bool") {
            // bool -> int：true=1, false=0，必须零扩展
            DEBUG_LOG("      ZExt (bool -> int)");
            return _builder.CreateZExt(val, dstLLVMType);
        }

        if (dstLLVMType->getIntegerBitWidth() > srcLLVMType->getIntegerBitWidth()) {
            // 扩展
            if (srcIsUnsigned) { // NOLINT(bugprone-branch-clone)
                DEBUG_LOG("      ZExt (unsigned int extension)");
                return _builder.CreateZExt(val, dstLLVMType);
            } else {
                DEBUG_LOG("      SExt (signed int extension)");
                return _builder.CreateSExt(val, dstLLVMType);
            }
        } else {
            // 截断
            DEBUG_LOG("      Trunc (int truncation)");
            return _builder.CreateTrunc(val, dstLLVMType);
        }
    } else if (!srcIsFloat && dstIsFloat) {
        // 整数转浮点数
        if (srcIsUnsigned) {
            DEBUG_LOG("      UIToFP (unsigned int to float)");
            return _builder.CreateUIToFP(val, dstLLVMType);
        } else {
            DEBUG_LOG("      SIToFP (signed int to float)");
            return _builder.CreateSIToFP(val, dstLLVMType);
        }
    } else {
        // 浮点数转 bool / 整数
        if (dstType.name == "bool") {
            // float -> bool：fcmp une x, 0.0（NaN 也判 true，与主流语言一致）
            DEBUG_LOG("      FCmpUNE 0.0 (float -> bool)");
            auto srcLLVMType = getLLVMType(srcType);
            return _builder.CreateFCmpUNE(val, llvm::ConstantFP::get(srcLLVMType, 0.0));
        }
        if (dstIsUnsigned) {
            DEBUG_LOG("      FPToUI (float to unsigned int)");
            return _builder.CreateFPToUI(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPToSI (float to signed int)");
            return _builder.CreateFPToSI(val, dstLLVMType);
        }
    }
}
// NOLINTEND(bugprone-branch-clone)

llvm::Value* Compiler::compileExpr(ExprNode* node) {
    auto type = node->getType();
    DEBUG_LOG_VAL("  compileExpr", "type=" << (type.empty() ? "void" : type.name));
    // Phase B：dispatch 后的 recordTemp 等读 resolvedOrInferredType；此处 type 仅 DEBUG_LOG。
    // 不能在入口对所有节点 assert resolvedType==getType：SemaPass 先写槽再跑
    // resolveFnOverload，灵活整数会被回填，槽与二次 getType 会暂时不一致。
    // 4.3：accept 分派；漏 override 编不过。嵌套 compileExpr 保存恢复结果槽。
    struct Restore {
        llvm::Value*& slot;
        llvm::Value* prev;
        ~Restore() { slot = prev; }
    } restore{.slot = _compileExprResult, .prev = _compileExprResult};
    _compileExprResult = nullptr;
    node->accept(*this);
    return _compileExprResult;
}

void Compiler::visitLiteral(ExprLiteralNode& node) {
    _compileExprResult = compileLiteralExpr(&node);
}
void Compiler::visitAddSub(ExprAddSubNode& node) {
    _compileExprResult = compileAddSubExpr(&node);
}
void Compiler::visitMulDivMod(ExprMulDivModNode& node) {
    _compileExprResult = compileMulDivModExpr(&node);
}
void Compiler::visitBinOp(ExprBinOpNode& node) {
    _compileExprResult = compileBinOpExpr(&node);
}
void Compiler::visitParen(ExprParenNode& node) {
    _compileExprResult = compileParenExpr(&node);
}
void Compiler::visitCall(ExprCallNode& node) {
    auto val = compileCallExpr(&node);
    if (val) recordTemp(val, resolvedOrInferredType(&node));
    _compileExprResult = val;
}
void Compiler::visitDot(ExprDotNode& node) {
    _compileExprResult = compileDotExpr(&node);
}
void Compiler::visitCompare(ExprCompareNode& node) {
    _compileExprResult = compileCompareExpr(&node);
}
void Compiler::visitIfElse(ExprIfElseNode& node) {
    _compileExprResult = compileIfElseExpr(&node);
}
void Compiler::visitOneLineIfElse(ExprOneLineIfElseNode& node) {
    _compileExprResult = compileOneLineIfElseExpr(&node);
}
void Compiler::visitGet(ExprGetNode& node) {
    _compileExprResult = compileArrayGetExpr(&node);
}
void Compiler::visitArray(ExprArrayNode& node) {
    auto val = compileArrayLiteralExpr(&node);
    if (val) recordTemp(val, resolvedOrInferredType(&node));
    _compileExprResult = val;
}
void Compiler::visitTuple(ExprTupleNode& node) {
    _compileExprResult = compileTupleExpr(&node);
}
void Compiler::visitDynCtor(ExprDynCtorNode& node) {
    _compileExprResult = compileDynCtorExpr(&node);
}
void Compiler::visitStructLit(ExprStructLitNode& node) {
    _compileExprResult = compileStructLitExpr(&node);
}
void Compiler::visitPathCall(ExprPathCallNode& node) {
    auto val = compileEnumCtorExpr(&node);
    auto resType = resolvedOrInferredType(&node);
    if (val && typeNeedsDestructor(resType)) recordTemp(val, resType);
    _compileExprResult = val;
}
void Compiler::visitMatch(ExprMatchNode& node) {
    _compileExprResult = compileMatchExpr(&node);
}
void Compiler::visitTryCatch(ExprTryCatchNode& node) {
    _compileExprResult = compileTryCatchExpr(&node);
}
void Compiler::visitLambda(LambdaExprNode& node) {
    _compileExprResult = compileLambdaExpr(&node);
}
void Compiler::visitGetRef(ExprGetRefNode& node) {
    _compileExprResult = compileGetRefExpr(&node);
}
void Compiler::visitUnary(ExprUnaryNode& node) {
    _compileExprResult = compileUnaryExpr(&node);
}
void Compiler::visitNullElse(ExprNullElseNode& node) {
    _compileExprResult = compileNullElseExpr(&node);
}
void Compiler::visitMoveAssign(ExprMoveAssignNode& node) {
    _compileExprResult = compileMoveAssignExpr(&node);
}
void Compiler::visitArrayInit(ExprArrayInitNode&) {
    // 数组填充表达式需要类型注解，实际处理在 compileDeclareAssignStatement
    _compileExprResult = nullptr;
}

// NOLINTBEGIN(bugprone-branch-clone)
llvm::Value* Compiler::compileStructLitExpr(ExprStructLitNode* node) {
    auto* structLitNode = node;
    // Phase 3b 构造模型重构: `Self { .field = value ... }` codegen.
    // 仅在 #Static fn 体内合法 (sema Phase 2d 已校验). 流程:
    //   alloca Self -> 按 fieldIndex 依次 GEP + store -> Load 返回值.
    // Phase 4a (BUGS #4): 句柄字段 (Rc / Array / Weak / fn-fat-ptr /
    //   含 RC 字段的非平凡 struct) 在 store 前对非 fresh 源 retain,
    //   与 `$.field = value` assign 路径行为对齐. fresh 源 (call/ctor/array-lit
    //   等) 已自带 +1 所有权, 直接 move-in 不再 retain.
    int line = structLitNode->resolveLineNumber();
    int col = structLitNode->resolveColumn();
    // DRAFT-const-eval Phase 5: TypeName{...} 形态从节点 structName() 取;
    // Self{...}：泛型实例方法里 AST 记的是模板名（`Map`），LLVM 类型在
    // `_generic.structs()` 里键为 mangled（`Map<i32,i32>`）。优先用当前单态名。
    string structName = structLitNode->structName();
    if (structLitNode->isSelfForm() && !_currentStructName.empty()) {
        const auto* inst = _generic.structs().find(_currentStructName);
        if (inst && inst->baseDecl && (structName.empty() || inst->baseDecl->name().getText() == structName)) {
            structName = _currentStructName;
        }
    }
    if (structName.empty()) {
        structName = _currentStructName;
    }
    if (structName.empty()) {
        // E3124 由 SemaPass Self/TypeName 字面量先抛。
        throwSemaGap(line, col);
    }
    // Phase 6E.4-C: 泛型 struct #Static fn 体内 `Self {...}` —
    // _currentStructName 是实例全限定名, getStructDecl 查不到; 走
    // _generic.structs() 拿 baseDecl, llvmStructType 仍按 mangled 名解析.
    TypeInfo litTy = typeInfoForNamedStruct(structName);
    StructDeclNode* decl = nullptr;
    if (!structLitNode->isSelfForm()) {
        auto r = sema::resolveExprTypeLhs(_file, _riu, structLitNode->typePath(), line, col);
        if (!r.type.empty()) {
            litTy = r.type;
            structName = r.type.name;
        }
        decl = r.structDecl;
        if (!decl) decl = names().lookupStruct(litTy);
    }
    if (!decl) {
        decl = names().lookupStruct(litTy);
    }
    if (!decl && _file) {
        decl = _file->getStructDecl(structName);
    }
    if (!decl && _riu && _riu->sdkFile() && _riu->sdkFile() != _file) {
        decl = _riu->sdkFile()->getStructDecl(structName);
    }
    if (!decl) {
        if (auto* inst = _generic.structs().find(structName)) {
            decl = inst->baseDecl;
        }
    }
    if (!decl) {
        // E3124 由 SemaPass 先抛；此处防 IR 无 decl 可 GEP。
        throwSemaGap(line, col);
    }
    auto llvmStructType = getLLVMType(litTy);
    if (!llvmStructType) {
        throwSemaGap(line, col);
    }
    auto alloca = createTypedAlloca(llvmStructType, litTy, structName + ".lit");
    // 零初始化, 与 ctor 入口保持一致, 避免遗漏字段 (实际上 sema 已强制全列)
    auto& dl = _module->getDataLayout();
    auto sizeBytes = dl.getTypeAllocSize(llvmStructType).getFixedValue();
    _builder.CreateMemSetInline(alloca, llvm::MaybeAlign(1), _builder.getInt8(0), _builder.getInt64(sizeBytes));
    std::unique_ptr<FieldInitNode> positionalInit;
    vector<FieldInitNode*> fieldInits = structLitNode->fields();
    if (auto* pos = structLitNode->positional()) {
        const int soleIdx = decl->soleNamedInstanceLayoutIndex();
        if (soleIdx < 0) {
            throwSemaGap(line, col);
        }
        positionalInit =
            std::make_unique<FieldInitNode>(structLitNode, decl->fields()[static_cast<size_t>(soleIdx)]->name(), pos);
        fieldInits = {positionalInit.get()};
    }
    for (auto& fi : fieldInits) {
        string fname = fi->name().getText();
        int idx = decl->fieldIndex(fname);
        string gepName = structName;
        gepName += '.';
        gepName += fname;
        auto fieldPtr = structFieldPtr(llvmStructType, alloca, static_cast<unsigned>(idx), gepName);
        const auto* fdecl = decl->field(fname);
        // Phase 6E.4-C: 泛型实例 Self {...} — 字段类型 (含 T) 透过当前
        // SubstFrame 替换为具体类型, 让 isArrayGeneric / typeNeedsDestructor
        // 识别本应是 Array<i32> 的字段而非 bare T.
        const auto fieldType = fdecl ? applySubst(fdecl->getType()) : TypeInfo();
        // Phase 4a: Array<T> 字段 + 数组字面量 RHS, 直接走 buildArrayLiteralBlock,
        // 把字段的 element type 透传给 literal, 避免无目标类型语境下默认成定长 [N]T
        // (与 compileDeclareAssignStatement 的 isArrayGeneric 分支对齐, BUGS #4)
        if (fieldType.isArrayGeneric()) {
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(fi->value())) {
                auto elemType = fieldType.arrayGenericElementType();
                if (!elemType) {
                    throwSemaGap(line, col);
                }
                auto block = buildArrayLiteralBlock(arrayNode, *elemType);
                _builder.CreateStore(block, fieldPtr);
                continue;
            }
        }
        // Phase 3d.3: Nullable<T> 字段处理
        // 三种情况：null 字面量 / Heap<T>? move-out / T→Nullable<T> 隐式包装
        llvm::Value* heapBdangSrcSlot = nullptr;
        llvm::Type* heapBdangSrcTy = nullptr;
        bool isHeapNullableField = false;
        bool isNullableWrapDone = false;
        if (fieldType.isNullable()) {
            auto inner = fieldType.nullableInnerType();
            if (inner && inner->isHeap()) {
                isHeapNullableField = true;
                tryHeapNullableLvalueSlot(fi->value(), heapBdangSrcSlot, heapBdangSrcTy);
            }
        }

        // Phase B-1: E4031 #NoCopy 字段初始化检查已迁入 SemaPass，Compiler 端不再重复。

        // Nullable<T> 字段：T → Nullable<T> 隐式包装 / null 字面量
        // （与 compileDeclareAssignStatement 的 nullable 路径对齐）
        if (fieldType.isNullable() && !isHeapNullableField) {
            auto innerType = fieldType.nullableInnerType();
            auto exprType = fi->value()->getType();
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto nullableLLVMTy = getLLVMType(fieldType);
            auto hasPtr = _builder.CreateGEP(nullableLLVMTy, fieldPtr, {zero, zero}, "nullable.has");
            auto valuePtr = _builder.CreateGEP(nullableLLVMTy, fieldPtr, {zero, one}, "nullable.value");

            if (innerType && exprType == *innerType) {
                // T → Nullable<T> 隐式包装：_has=true, 值写入 value 槽
                auto val = compileExpr(fi->value());
                takeOwnership(val, *innerType, fi->value());
                _builder.CreateStore(_builder.getInt1(true), hasPtr);
                _builder.CreateStore(val, valuePtr);
                isNullableWrapDone = true;
            } else if (isFlexibleNullExpr(fi->value())) {
                // null 字面量：zero-init 已给出 _has=false + _value=zero，跳过即可
                isNullableWrapDone = true;
            } else if (exprType.isNullable() && exprType == fieldType) {
                // 已是 Nullable<T> → 整体复制（走下方既有路径）
                // 不做 isNullableWrapDone，让 compileExpr + CreateStore 正常处理
            }
        }

        if (!isNullableWrapDone) {
            auto val = compileExpr(fi->value());
            // Phase 4a: 句柄字段所有权转移 (与 declare-assign 路径对齐, BUGS #4)
            //   - fresh 源 (call / ctor / array-lit): 已 +1, 直接 consume 临时帧, 不重复 retain
            //   - 非 fresh 源 (let / 字段读取等): retain 一次, 让源句柄与字段都各持 +1
            // Phase 3d.3: Heap<T>? 字段 + lvalue 源 = move-out, 跳过 retain.
            if (fdecl && val && typeNeedsDestructor(fieldType)) {
                if (isHeapNullableField && heapBdangSrcSlot) {
                    if (isFreshHandleExpr(fi->value())) consumeTemp(val);
                } else {
                    takeOwnership(val, fieldType, fi->value());
                }
            }
            _builder.CreateStore(val, fieldPtr);
        }
        if (heapBdangSrcSlot && heapBdangSrcTy) {
            auto z0 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto z1 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto hasField = _builder.CreateGEP(heapBdangSrcTy, heapBdangSrcSlot, {z0, z0}, "bdang.field.has");
            auto valField = _builder.CreateGEP(heapBdangSrcTy, heapBdangSrcSlot, {z0, z1}, "bdang.field.value");
            _builder.CreateStore(_builder.getInt1(false), hasField);
            _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), valField);
        }
    }
    return _builder.CreateLoad(llvmStructType, alloca, structName + ".lit.load");
}
// NOLINTEND(bugprone-branch-clone)

void Compiler::visitBlock(StatementBlockNode& node) {
    compileStatementBlock(&node);
}

void Compiler::compileStatementBlock(StatementBlockNode* block) {
    DEBUG_LOG_VAL("  compileStatementBlock", block->statements().size()
                                                 << " statements, hasResult=" << block->hasResult());
    pushScopeFrame();
    const size_t myDepth = scopeFrameDepth();
    for (auto& stmt : block->statements()) {
        if (_builder.GetInsertBlock()->getTerminator()) break;
        compileStatement(stmt);
    }
    if (!_builder.GetInsertBlock()->getTerminator() && block->hasResult()) {
        DEBUG_LOG("    Compiling result expression");
        // 与 compileBranchResultNormalized 同款：void 尾表达式的 String 临时必须在本块释放。
        // match 无值臂走本函数，两臂共用外层语句帧会在 merge 上析构未初始化 spill。
        (void)compileBranchResultNormalized(block->resultExpr(), TypeInfo());
    }
    if (scopeFrameDepth() == myDepth) {
        if (_builder.GetInsertBlock()->getTerminator()) {
            // break/ret 已发射析构；仅恢复编译期帧栈，供兄弟分支继续
            popScopeFrameNoDestroy();
        } else {
            popScopeFrameAndDestroy();
        }
    }
}

llvm::Value* Compiler::compileStatementBlockWithResult(StatementBlockNode* block, llvm::BasicBlock* continueBlock,
                                                       llvm::PHINode* phi, const TypeInfo& resultType) {
    pushScopeFrame();
    const size_t myDepth = scopeFrameDepth();
    for (auto& stmt : block->statements()) {
        if (_builder.GetInsertBlock()->getTerminator()) break;
        compileStatement(stmt);
    }

    if (_builder.GetInsertBlock()->getTerminator()) {
        if (scopeFrameDepth() == myDepth) {
            popScopeFrameNoDestroy();
        }
        return nullptr;
    }

    if (block->hasResult()) {
        // 尾表达式是 #NoReturn / 全分支终止时不当值：只编译调用（内部 emit unreachable），不进 phi。
        if (exprTerminatesFlow(block, block->resultExpr())) {
            (void)compileBranchResultNormalized(block->resultExpr(), TypeInfo());
            if (scopeFrameDepth() == myDepth) {
                popScopeFrameNoDestroy();
            }
            return nullptr;
        }
        // Phase 8d.3: 分支结果走子帧（void 也开，吃掉模板插值临时）；需析构时再 consume / retain
        auto resultVal = compileBranchResultNormalized(block->resultExpr(), resultType);
        if (_builder.GetInsertBlock()->getTerminator()) {
            if (scopeFrameDepth() == myDepth) {
                popScopeFrameNoDestroy();
            }
            return nullptr;
        }
        if (phi && !resultType.empty() && resultVal) {
            phi->addIncoming(resultVal, _builder.GetInsertBlock());
        }
    }

    if (scopeFrameDepth() == myDepth) {
        popScopeFrameAndDestroy();
    }
    _builder.CreateBr(continueBlock);
    return nullptr;
}

// ==================== a <- b : move-assign 表达式 ====================

// 取 lvalue 表达式的地址（alloca / GEP）。
// 仅支持简单变量、$、$.field / a.b 链式字段访问。
llvm::Value* Compiler::compileLvalueAddr(ExprNode* node) {
    auto line = node->resolveLineNumber();
    auto col = node->resolveColumn();

    // 简单变量引用: ExprLiteralNode(LiteralObjNode("name"))
    if (auto lit = dynamic_cast<ExprLiteralNode*>(node)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(lit->literal())) {
            auto name = obj->getValue().getText();
            // 当前实例 $ —— 存在 _localVarPtrs["$"] 中
            if (name == "$") {
                auto it = _localVarPtrs.find("$");
                if (it == _localVarPtrs.end()) {
                    throwSemaGap(line, col);
                }
                return it->second;
            }
            // 局部变量
            auto it = _localVarPtrs.find(name);
            if (it != _localVarPtrs.end()) {
                return it->second;
            }
            // 全局变量
            string ownerMod = _file ? _file->moduleName() : "";
            bool globPriv = !name.empty() && name[0] == '_';
            string mangledName = Mangler::global(ownerMod, name, globPriv);
            // #Inline #Cval：无存储地址，不可作为 lvalue
            if (_inlineConstantValues.contains(mangledName)) {
                // E3118 由 SemaPass GetRef / 读路径先抛。
                throwSemaGap(line, col);
            }
            if (auto gv = _module->getGlobalVariable(mangledName, true)) {
                return gv;
            }
            // E3030 由 SemaPass getType 先抛。
            throwSemaGap(line, col);
        }
        // E4036 由 SemaPass isMoveAssignLvalue 先抛；此处防 IR 把字面量当槽。
        throwSemaGap(line, col);
    }

    // 字段访问: ExprDotNode(base, member)
    if (auto dot = dynamic_cast<ExprDotNode*>(node)) {
        auto baseAddr = compileLvalueAddr(dot->baseExpr());
        auto baseType = dot->baseExpr()->getType();
        // 剥 Ref<T> → T
        bool wasRef = false;
        if (baseType.isRef()) {
            auto inner = baseType.refElementType();
            if (inner) {
                baseType = *inner;
                wasRef = true;
            }
        }
        // 剥 Rc<T> → T
        if (baseType.isRc()) {
            auto inner = baseType.rcElementType();
            if (inner) baseType = *inner;
        }
        string member = dot->member();

        // 元组成员访问: member 为纯数字，base 解析后为 Tuple
        // 透明 alias 由 applySubst 兜底（与 compileDotExpr 一致）
        if (!member.empty() && std::ranges::all_of(member, [](char c) { return c >= '0' && c <= '9'; })) {
            auto resolved = applySubst(baseType);
            if (resolved.isTuple()) {
                auto& elems = resolved.tupleElements();
                auto idx = static_cast<size_t>(std::stoul(member));
                if (idx >= elems.size()) {
                    throwSemaGap(line, col);
                }
                // Ref<T> 的 lvalue addr 存的是引用值（ptr），需先 Load 再 GEP 到元组元素
                if (wasRef) {
                    baseAddr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), baseAddr, "tuple.ref.deref");
                }
                auto llvmTupleType = getLLVMType(resolved);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto idxVal = llvm::ConstantInt::get(_builder.getInt32Ty(), static_cast<unsigned>(idx));
                return _builder.CreateGEP(llvmTupleType, baseAddr, {zero, idxVal}, "move.lhs.tuple.gep");
            }
            // 非 Tuple 的 .N 留给后续 struct 逻辑（会落到 E3040 报字段不存在）
        }

        auto structType = getLLVMType(baseType);
        if (!structType) {
            throwSemaGap(line, col);
        }
        // 找字段索引
        auto* decl = names().lookupStruct(baseType);
        if (!decl) {
            // E3041 / E3043 由 SemaPass 读路径先抛；此处防 IR 无 decl 可 GEP。
            throwSemaGap(line, col);
        }
        int idx = decl->fieldIndex(member);
        if (idx < 0) {
            throwSemaGap(line, col);
        }
        return structFieldPtr(structType, baseAddr, static_cast<unsigned>(idx), "move.lhs.gep");
    }

    // E4036 由 SemaPass isMoveAssignLvalue 先抛；此处防 IR 走进不支持的 LHS 形态。
    throwSemaGap(line, col);
}

// 是否为 ident / ident.field... 左值（与 isMoveAssignLvalue 形态对齐）。
static bool isMoveLvalueExpr(ExprNode* node) {
    while (auto* dot = dynamic_cast<ExprDotNode*>(node)) {
        node = dot->baseExpr();
    }
    auto* lit = dynamic_cast<ExprLiteralNode*>(node);
    if (!lit) return false;
    return dynamic_cast<LiteralObjNode*>(lit->literal()) != nullptr;
}

// 编译 a <- b：移出旧值、替换新值、返回旧值
// 1. 取 LHS 地址 → load 旧值
// 2. 编译 RHS (新值)
// 3. RC 所有权管理：旧值不移 retain（ownership 转给结果），新值 retain（多一个 owner）
// 4. Store 新值到 LHS 地址
// 5. Array / Heap / #NoCopy：非 fresh 左值 RHS 无 RC 可 retain，须把源槽写成零值，
//    否则与 LHS 共享缓冲 → 双重释放（§8.7.7.3 / §9.2.1.3 O(1) 转移）
// 6. 返回旧值；若含 RC 字段则 recordTemp（结果持 ownership +1）
llvm::Value* Compiler::compileMoveAssignExpr(ExprMoveAssignNode* node) {
    int line = node->resolveLineNumber();
    int col = node->resolveColumn();

    auto leftNode = node->left();
    auto rightNode = node->right();
    auto leftType = leftNode->getType();

    // 1. 取 LHS 地址 + 读取旧值
    auto addr = compileLvalueAddr(leftNode);
    auto llvmType = getLLVMType(leftType);
    if (!llvmType) {
        throwSemaGap(line, col);
    }
    auto oldVal = _builder.CreateLoad(llvmType, addr, "move.old");

    // 2. 类型推断 + 编译 RHS
    //    空数组 [] 需用 leftType 的 elem type 走 buildArrayLiteralBlock（否则 getLLVMType("__empty") 炸）
    if (isIntTypeName(leftType.name) && isFlexibleIntExpr(rightNode)) {
        tryInferIntType(rightNode, leftType);
    }
    llvm::Value* valToStore;
    if (leftType.isArrayGeneric() && dynamic_cast<ExprArrayNode*>(rightNode)) {
        auto arrNode = static_cast<ExprArrayNode*>(rightNode);
        auto elemType = leftType.arrayGenericElementType();
        valToStore = buildArrayLiteralBlock(arrNode, elemType ? *elemType : TypeInfo("i8"));
        // buildArrayLiteralBlock 已按 leftType 构建，无需 createCast
    } else {
        auto rightVal = compileExpr(rightNode);
        auto rightType = rightNode->getType();
        valToStore = createCast(rightVal, rightType, leftType);
    }

    // 3. RC 所有权管理
    //    RHS 新值即将存入 LHS 槽位 → LHS 成为一个新 owner → 需 retain（或 consume fresh temp）。
    //    空数组 [] 走 buildArrayLiteralBlock，返回值自带 +1，不在 temp frame 中，无需 consume。
    if (typeNeedsDestructor(leftType)) {
        bool isArrayLiteral = leftType.isArrayGeneric() && dynamic_cast<ExprArrayNode*>(rightNode);
        if (!isArrayLiteral) {
            takeOwnership(valToStore, leftType, rightNode);
        }
    }

    // 4. 写入新值
    _builder.CreateStore(valToStore, addr);

    // 5. 无 RC 的独占类型：非 fresh 左值 RHS 置空（含 a <- a），完成 O(1) 所有权转移
    if ((isNoCopyType(leftType) || leftType.isHeap()) && !isFreshHandleExpr(rightNode)) {
        if (isMoveLvalueExpr(rightNode)) {
            auto rhsAddr = compileLvalueAddr(rightNode);
            _builder.CreateStore(llvm::Constant::getNullValue(llvmType), rhsAddr);
        }
    }

    // 6. 旧值返回（不含 retain：ownership 从 LHS 移交给结果）
    if (typeNeedsDestructor(leftType)) {
        recordTemp(oldVal, leftType);
    }

    return oldVal;
}

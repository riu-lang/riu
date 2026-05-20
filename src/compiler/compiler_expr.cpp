// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 表达式编译实现
// 
// 本文件包含所有表达式类型的编译逻辑:
// - 字面量表达式 (整数、浮点数、布尔值、字符串)
// - 算术表达式 (加减乘除取模)
// - 位运算表达式 (与或异或左移右移)
// - 比较表达式 (相等、不等、大小比较)
// - 括号表达式
// - 函数调用表达式
// - 成员访问表达式
// - if-else 表达式
// - 数组表达式
// - 一元表达式 (取负、取反、取引用)

#include "compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/enum_node.h"
#include "compiler_runtime.h"
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

// ==================== 辅助函数 ====================

// Phase 2.4 Sema/Codegen 拆分：codegen 读类型的统一入口。详见 compiler.h 注释。
// 已切换的调用点（先窄后宽）：
//   - compileExpr 入口 dispatch 后的 recordTemp / typeNeedsDestructor 三处用例
//     （call / array literal / enum ctor 分支）。这三个分支的 compile<Foo>Expr
//     已按 2.2 在入口写过 resolvedType，回到主 switch 时一定可读。
// 其余 compile<Foo>Expr 内部对 node->getType() 的现地复读保持原样，留待 Phase 3
// 按子系统迁移到 SemaPass 时统一切换。
TypeInfo Compiler::resolvedOrInferredType(p<ExprNode> node) const {
    if (node->hasResolvedType()) {
#ifndef NDEBUG
        const auto& resolved = node->resolvedType();
        auto inferred = node->getType();
        // 透明 alias (spec §3.9.1.2 / §3.9.3.1): SemaPass 缓存的 resolvedType 与 codegen
        // 阶段 getType() 重新计算的结果, 在字面上可能一侧是别名名 (`IPair`), 另一侧已被
        // 解开 (`(i32,i32)`). 两者按 alias 归一后应一致; 仅当归一后仍不等才视为真冲突.
        assert(resolveAlias(resolved) == resolveAlias(inferred) && "resolvedType / getType inconsistent");
#endif
        return node->resolvedType();
    }
    return node->getType();
}


// ==================== 类型转换 ====================

// 创建类型转换
// 处理整数、浮点数、指针、引用等类型之间的转换
llvm::Value* Compiler::createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType) {
    // 相同类型无需转换
    if (srcType == dstType) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType.name);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType.name << " -> " << dstType.name);

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
    auto dstLLVMType = getLLVMType(dstType);
    bool srcIsFloat = srcType.startsWith('f');
    bool dstIsFloat = dstType.startsWith('f');
    bool srcIsUnsigned = srcType.startsWith('u');
    bool dstIsUnsigned = dstType.startsWith('u');

    if (srcIsFloat && dstIsFloat) {
        // 浮点数之间的转换
        if (srcType.name == "f64" && dstType.name == "f32") {
            DEBUG_LOG("      FPTrunc (f64 -> f32)");
            return _builder.CreateFPTrunc(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPExt (f32 -> f64)");
            return _builder.CreateFPExt(val, dstLLVMType);
        }
    } else if (!srcIsFloat && !dstIsFloat) {
        // 整数之间的转换
        auto srcLLVMType = getLLVMType(srcType);
        if (dstLLVMType->getIntegerBitWidth() > srcLLVMType->getIntegerBitWidth()) {
            // 扩展
            if (srcIsUnsigned) {
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
        // 浮点数转整数
        if (dstIsUnsigned) {
            DEBUG_LOG("      FPToUI (float to unsigned int)");
            return _builder.CreateFPToUI(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPToSI (float to signed int)");
            return _builder.CreateFPToSI(val, dstLLVMType);
        }
    }
}


llvm::Value* Compiler::compileExpr(p<ExprNode> node) {
    auto type = node->getType();
    DEBUG_LOG_VAL("  compileExpr", "type=" << (type.empty() ? "void" : type.name));
    // Phase 2.2 Sema/Codegen 拆分：resolvedType 由各 compile<Foo>Expr 自行写入。
    // Phase 2.4：本入口 dispatch 后的 recordTemp / typeNeedsDestructor 改读
    // resolvedOrInferredType(node)；本地 type 仅给 DEBUG_LOG 用，等 Phase 3
    // SemaPass 落地后即可整体删除。

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(node)) {
        return compileLiteralExpr(literalNode);
    } else if (auto addSubNode = dynamic_cast<ExprAddSubNode*>(node)) {
        return compileAddSubExpr(addSubNode);
    } else if (auto mulDivModNode = dynamic_cast<ExprMulDivModNode*>(node)) {
        return compileMulDivModExpr(mulDivModNode);
    } else if (auto binOpNode = dynamic_cast<ExprBinOpNode*>(node)) {
        return compileBinOpExpr(binOpNode);
    } else if (auto parenNode = dynamic_cast<ExprParenNode*>(node)) {
        return compileParenExpr(parenNode);
    } else if (auto callNode = dynamic_cast<ExprCallNode*>(node)) {
        // Phase 8d.1: 调用结果若为 fresh RC 句柄（Rc/Array/Weak），登记到当前语句临时帧
        auto val = compileCallExpr(callNode);
        if (val) {
            // Phase 2.4: compileCallExpr 入口已写 resolvedType，优先读它
            recordTemp(val, resolvedOrInferredType(node));
        }
        return val;
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(node)) {
        return compileDotExpr(dotNode);
    } else if (auto compareNode = dynamic_cast<ExprCompareNode*>(node)) {
        return compileCompareExpr(compareNode);
    } else if (auto ifElseNode = dynamic_cast<ExprIfElseNode*>(node)) {
        return compileIfElseExpr(ifElseNode);
    } else if (auto oneLineIfElseNode = dynamic_cast<ExprOneLineIfElseNode*>(node)) {
        return compileOneLineIfElseExpr(oneLineIfElseNode);
    } else if (auto ifElsePreValueNode = dynamic_cast<ExprIfElsePreValueNode*>(node)) {
        return compileIfElsePreValueExpr(ifElsePreValueNode);
    } else if (auto getNode = dynamic_cast<ExprGetNode*>(node)) {
        return compileArrayGetExpr(getNode);
    } else if (auto arrayNode = dynamic_cast<ExprArrayNode*>(node)) {
        // Phase 8d.1: 数组字面量 _array_alloc 给 strong=1，登记为 fresh 临时
        auto val = compileArrayLiteralExpr(arrayNode);
        if (val) {
            // Phase 2.4: 同上，优先读 resolvedType
            recordTemp(val, resolvedOrInferredType(node));
        }
        return val;
    } else if (auto tupleNode = dynamic_cast<ExprTupleNode*>(node)) {
        return compileTupleExpr(tupleNode);
    } else if (auto dynCtorNode = dynamic_cast<ExprDynCtorNode*>(node)) {
        // Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
        // Phase 1c：仅 emit 占位 fat ptr { vtable=null, data=src.handle }；
        // 真 vtable 与 dtor 路由留 Phase 3，对象安全 / E1133 类型检查留 Phase 2。
        return compileDynCtorExpr(dynCtorNode);
    } else if (auto heapCtorNode = dynamic_cast<ExprHeapCtorNode*>(node)) {
        // Heap:<T>(x) 构造（DRAFT-heap-types §8.3a）—— Phase 2.4 codegen
        return compileHeapCtorExpr(heapCtorNode);
    } else if (auto structLitNode = dynamic_cast<ExprStructLitNode*>(node)) {
        // Phase 3b 构造模型重构: `Self { .field = value ... }` codegen.
        // 仅在 #Static fn 体内合法 (sema Phase 2d 已校验). 流程:
        //   alloca Self -> 按 fieldIndex 依次 GEP + store -> Load 返回值.
        // Phase 4a (BUGS #4): 句柄字段 (Rc / Array / Weak / fn-fat-ptr /
        //   含 RC 字段的非平凡 struct) 在 store 前对非 fresh 源 retain,
        //   与 `$.field = value` assign 路径行为对齐. fresh 源 (call/ctor/array-lit
        //   等) 已自带 +1 所有权, 直接 move-in 不再 retain.
        int line = structLitNode->resolveLineNumber();
        int col = structLitNode->resolveColumn();
        const string& structName = _currentStructName;
        if (structName.empty()) {
            throw YuxError(line, col, ErrorCode::E0000,
                           "`Self { ... }` codegen 找不到所属结构体 (sema 应已拦截)");
        }
        // Phase 6E.4-C: 泛型 struct #Static fn 体内 `Self {...}` —
        // _currentStructName 是 mangled (`GH$i32`), getStructDecl 查不到; 走
        // _structInstances 拿 baseDecl, llvmStructType 仍按 mangled 名解析.
        auto* decl = _file ? _file->getStructDecl(structName) : nullptr;
        if (!decl && _yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
            decl = _yux->sdkFile()->getStructDecl(structName);
        }
        if (!decl) {
            auto instIt = _structInstances.find(structName);
            if (instIt != _structInstances.end()) {
                decl = instIt->second.baseDecl;
            }
        }
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E0000,
                           "`Self { ... }` codegen 找不到 struct decl: " + structName);
        }
        auto llvmStructType = getLLVMType(TypeInfo(structName));
        if (!llvmStructType) {
            throw YuxError(line, col, ErrorCode::E3096, structName);
        }
        auto alloca = _builder.CreateAlloca(llvmStructType, nullptr, structName + ".lit");
        // 零初始化, 与 ctor 入口保持一致, 避免遗漏字段 (实际上 sema 已强制全列)
        auto& dl = _module->getDataLayout();
        auto sizeBytes = dl.getTypeAllocSize(llvmStructType).getFixedValue();
        _builder.CreateMemSetInline(alloca, llvm::MaybeAlign(1), _builder.getInt8(0),
                                    _builder.getInt64(sizeBytes));
        for (auto& fi : structLitNode->fields()) {
            string fname = fi->name().getText();
            int idx = decl->fieldIndex(fname);
            auto fieldPtr = _builder.CreateStructGEP(llvmStructType, alloca,
                                                     static_cast<unsigned>(idx),
                                                     structName + "." + fname);
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
                        throw YuxError(line, col, ErrorCode::E3055);
                    }
                    auto block = buildArrayLiteralBlock(arrayNode, *elemType);
                    storeArrayHandle(fieldPtr, block);
                    continue;
                }
            }
            // Phase 3d.3: B 档 nullable move — 字段类型 `Heap<T>?` 且源是 lvalue
            // (局部 ID / 局部 struct 字段) 时, 走 move-out: 不 retain, 写入后把源槽
            // {_has=false, _value=null} 置空. 让源 owner 析构跳 free, 字段独占所有权.
            llvm::Value* heapBdangSrcSlot = nullptr;
            llvm::Type* heapBdangSrcTy = nullptr;
            bool isHeapNullableField = false;
            if (fieldType.isNullable()) {
                auto inner = fieldType.nullableInnerType();
                if (inner && inner->isHeap()) {
                    isHeapNullableField = true;
                    tryHeapNullableLvalueSlot(fi->value(), heapBdangSrcSlot, heapBdangSrcTy);
                }
            }

            auto val = compileExpr(fi->value());
            // Phase 4a: 句柄字段所有权转移 (与 declare-assign 路径对齐, BUGS #4)
            //   - fresh 源 (call / ctor / array-lit): 已 +1, 直接 consume 临时帧, 不重复 retain
            //   - 非 fresh 源 (let / 字段读取等): retain 一次, 让源句柄与字段都各持 +1
            // Phase 3d.3: Heap<T>? 字段 + lvalue 源 = move-out, 跳过 retain.
            if (fdecl && val && typeNeedsDestructor(fieldType)) {
                if (isFreshHandleExpr(fi->value())) {
                    consumeTemp(val);
                } else if (!(isHeapNullableField && heapBdangSrcSlot)) {
                    retainHandleAtCallSite(val, fieldType);
                }
            }
            _builder.CreateStore(val, fieldPtr);
            if (heapBdangSrcSlot && heapBdangSrcTy) {
                auto z0 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto z1 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
                auto ptrTy = llvm::PointerType::get(_context, 0);
                auto hasField = _builder.CreateGEP(heapBdangSrcTy, heapBdangSrcSlot,
                                                   {z0, z0}, "bdang.field.has");
                auto valField = _builder.CreateGEP(heapBdangSrcTy, heapBdangSrcSlot,
                                                   {z0, z1}, "bdang.field.value");
                _builder.CreateStore(_builder.getInt1(false), hasField);
                _builder.CreateStore(llvm::ConstantPointerNull::get(ptrTy), valField);
            }
        }
        return _builder.CreateLoad(llvmStructType, alloca, structName + ".lit.load");
    } else if (auto enumCtorNode = dynamic_cast<ExprPathCallNode*>(node)) {
        // Phase 5: enum ctor 是 +1 fresh：构造时把实参（含 RC payload）写入 enum 槽，
        // enum 值随后承担释放责任。仅当类型需要析构时才登记到临时帧
        auto val = compileEnumCtorExpr(enumCtorNode);
        // Phase 2.4: 同上，优先读 resolvedType
        auto resType = resolvedOrInferredType(node);
        if (val && typeNeedsDestructor(resType)) {
            recordTemp(val, resType);
        }
        return val;
    } else if (auto matchNode = dynamic_cast<ExprMatchNode*>(node)) {
        // Phase 6: match 表达式 — switch on tag + 绑定 + arm 体
        return compileMatchExpr(matchNode);
    } else if (auto tryCatchNode = dynamic_cast<ExprTryCatchNode*>(node)) {
        // Phase 10f: try-catch 表达式 — 仅占位 + 语义校验，IR 路由推 10g
        return compileTryCatchExpr(tryCatchNode);
    } else if (auto lambdaNode = dynamic_cast<LambdaExprNode*>(node)) {
        // Phase 2b: lambda 字面量 → 16 字节 fat-ptr 值 { fn_ptr, captures=null }
        return compileLambdaExpr(lambdaNode);
    } else if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(node)) {
        return compileGetRefExpr(getRefNode);
    } else if (auto unaryNode = dynamic_cast<ExprUnaryNode*>(node)) {
        return compileUnaryExpr(unaryNode);
    } else if (auto nullElseNode = dynamic_cast<ExprNullElseNode*>(node)) {
        return compileNullElseExpr(nullElseNode);
    } else if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(node)) {
        // 数组填充表达式需要类型注解，这里返回 nullptr
        // 实际处理在 compileDeclareAssignStatement 中
        return nullptr;
    } else {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
}

void Compiler::compileStatementBlock(p<StatementBlockNode> block) {
    DEBUG_LOG_VAL(
        "  compileStatementBlock", block->statements().size() << " statements, hasResult=" << block->hasResult());
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }
    if (block->hasResult()) {
        DEBUG_LOG("    Compiling result expression");
        compileExpr(block->resultExpr());
    }
}

llvm::Value* Compiler::compileStatementBlockWithResult(
    p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }

    if (_builder.GetInsertBlock()->getTerminator()) {
        return nullptr;
    }

    if (block->hasResult()) {
        // Phase 8d.3: RC 句柄分支结果走子帧 + 归一 retain；非 RC 沿用旧行为
        auto resultVal = compileBranchResultNormalized(block->resultExpr(), resultType);
        if (phi && !resultType.empty()) {
            phi->addIncoming(resultVal, _builder.GetInsertBlock());
        }
    }

    _builder.CreateBr(continueBlock);
    return nullptr;
}

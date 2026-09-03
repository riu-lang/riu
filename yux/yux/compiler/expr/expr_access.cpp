// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 成员 / 索引 / 安全访问表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "analyzer/symbol_suggest.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <set>

llvm::Value* Compiler::compileArrayGetExpr(p<ExprGetNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        // Phase 3.4.g: yux*.g4 强制 indices >= 1, 该分支不可达; 保留作 dead 防御。
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    DEBUG_LOG_VAL("    Expr: ArrayGet", arrayType.name);

    // Auto-deref: [T * N]& → [T * N] (Ref<Array<...>>)
    TypeInfo derefArrayType = arrayType;
    bool isRefArray = false;
    if (arrayType.isRef()) {
        auto inner = arrayType.refElementType();
        if (inner) {
            derefArrayType = *inner;
            isRefArray = true;
        }
    }

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = derefArrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3030, varName);
            }
            currentPtr = it->second;
        }
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(arrayExpr)) {
        auto fieldPtr = compileExpr(dotNode);
        auto baseType = dotNode->baseExpr()->getType();
        auto member = dotNode->member();

        auto structDecl = _file->getStructDecl(baseType.name);
        if (structDecl) {
            int fieldIndex = structDecl->fieldIndex(member);
            if (fieldIndex >= 0) {
                auto alloca = _builder.CreateAlloca(getLLVMType(arrayType), nullptr, "array_field_tmp");
                _builder.CreateStore(fieldPtr, alloca);
                currentPtr = alloca;
            }
        }
    }

    // General fallback: compile the base expression to get the array value/ref
    if (!currentPtr) {
        auto baseVal = compileExpr(arrayExpr);
        if (!baseVal) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        auto valTy = getLLVMType(arrayType);
        auto alloca = _builder.CreateAlloca(valTy, nullptr, "array.base");
        _builder.CreateStore(baseVal, alloca);
        currentPtr = alloca;
        // If the array was accessed via reference (e.g. [T * N]&), load the array pointer
        if (isRefArray) {
            currentPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), currentPtr, "array.ref.load");
        }
    }
    // Note: for variable lookups, currentPtr from _localVarPtrs is already usable directly:
    //   - T& variables: currentPtr is the reference value (ptr to array)
    //   - non-T& variables: currentPtr is the alloca
    // No isRefArray load needed in that path.

    if (derefArrayType.isArrayGeneric()) {
        auto elemType = derefArrayType.arrayGenericElementType();
        auto elemLLVMType = getLLVMType(*elemType);
        // B-3: _data 字段内联，直接 load，不再经过 Block 间接
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayDataFieldPtr(currentPtr, "arr"),
                                           "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        // v0.16: [] 返回 T&，与 .get() 一致；返回指针，不 Load
        return elemPtr;
    }

    for (auto& indexExpr : indices) {
        auto indexVal = compileExpr(indexExpr);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        std::array<llvm::Value*, 2> gepIndices{zero, indexVal};

        auto llvmArrayType = getLLVMType(currentType);
        currentPtr = _builder.CreateGEP(llvmArrayType, currentPtr, gepIndices, "array.element");

        if (currentType.elementType) {
            currentType = *currentType.elementType;
        }
    }

    // v0.16: [] 返回 T&，与 .get() 一致；返回指针，不 Load
    // 特例：[T& * N] 的元素已是 Ref，GEP 到的是引用槽（ptr-to-ptr），需 Load 取引用值
    if (currentType.isRef()) {
        return _builder.CreateLoad(getLLVMType(currentType), currentPtr, "array.elem.ref.load");
    }
    return currentPtr;
}

llvm::Value* Compiler::compileDotExpr(p<ExprDotNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // 安全访问 a?.b：单独走分支
    if (node->isSafe()) {
        return compileSafeDotExpr(node);
    }
    auto baseExpr = node->baseExpr();
    auto member = node->member();

    // 元组成员访问 a.N：member 为纯数字，base 解析后必须是 Tuple
    // 透明 alias 由 applySubst 兜底（顶层 Normal alias、泛型 alias 实例化均能展开）
    if (!member.empty() && std::ranges::all_of(member, [](char c) { return c >= '0' && c <= '9'; })) {
        auto baseTypeRaw = baseExpr->getType();
        auto baseTypeResolved = applySubst(baseTypeRaw);
        if (baseTypeResolved.isTuple()) {
            auto& elems = baseTypeResolved.tupleElements();
            auto idx = static_cast<size_t>(std::stoul(member));
            // E3100 由 SemaPass tryValidateFieldChain 先抛；此处防 IR ExtractValue 越界。
            if (idx >= elems.size()) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            DEBUG_LOG_VAL("    Expr: TupleMemberAccess", baseTypeResolved.name << "." << member);
            auto baseVal = compileExpr(baseExpr);
            if (!baseVal) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
            }
            return _builder.CreateExtractValue(baseVal, {static_cast<unsigned>(idx)}, "tuple.elem");
        }
        // 非 Tuple 的 .N 留给后续逻辑（当前会落到 E3090 报"Unsupported dot expression"）
    }

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("    Expr: DotCast", member);
        auto baseVal = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();

        string castFnName = "__cast_" + to_string(_castCounter++);
        _castFunctions[castFnName] = {.value = baseVal, .srcType = srcType, .dstType = TypeInfo(dstType)};

        return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    }

    // DRAFT-spec-reflect §6: Field.value → compile-time field name rewrite.
    // Resolution happens in ExprDotNode::getType() which caches {structDecl, fieldIndex}.
    // Here we just check the cached result and emit $.field_name.
    // Trigger getType() to populate the cached metadata (mirrors compileEnumCtorExpr L38).
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    if (node->isReflectFieldValue()) {
        auto* sd = node->reflectStructDecl();
        int fieldIdx = node->reflectFieldIndex();
        if (!sd || fieldIdx < 0) {
            // E3133 由 SemaPass tryValidateReflectFieldValueRead 先抛；此处防 IR 无字段可读。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        if (_currentStructName.empty()) {
            // E3134 由 SemaPass 先抛；此处防 IR 无 `$`。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        auto selfIt = _localVarPtrs.find("$");
        if (selfIt == _localVarPtrs.end()) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        auto structType = getLLVMType(typeInfoForNamedStruct(_currentStructName));
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto fIdx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIdx);
        std::array<llvm::Value*, 2> indices{zero, fIdx};
        auto fieldPtr = _builder.CreateGEP(structType, selfIt->second, indices, "reflect.field");
        auto fieldType = sd->fields()[fieldIdx]->getType();
        return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "reflect.field.load");
    }

    auto baseType = baseExpr->getType();
    TypeInfo actualType = baseType;
    llvm::Value* structPtr = nullptr;
    bool isHeapBase = baseType.isHeap();
    bool heapFromLocal = false;

    if (baseType.isRef()) {
        auto refElemType = baseType.refElementType();
        if (refElemType) {
            actualType = *refElemType;
        }
    }

    if (actualType.isHeap()) {
        auto heapElemType = actualType.heapElementType();
        if (heapElemType) {
            actualType = *heapElemType;
        }
    }

    if (actualType.isRc()) {
        auto rcElemType = actualType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
        }
    }

    auto structDecl = _file->getStructDecl(actualType.name, /*includeBuiltin=*/true);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(actualType.name, /*includeBuiltin=*/true);
    }

    if (structDecl) {

        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            auto field = structDecl->fields()[fieldIndex];

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        structPtr = it->second;
                        if (isHeapBase) heapFromLocal = true;
                    }
                }
            }

            if (!structPtr) {
                auto baseVal = compileExpr(baseExpr);
                if (isHeapBase) {
                    // Heap<T>: compileExpr 返回 T*，直接用作 struct 指针
                    structPtr = baseVal;
                } else {
                    auto tmpAlloca = _builder.CreateAlloca(getLLVMType(baseType), nullptr, "struct_field_tmp");
                    _builder.CreateStore(baseVal, tmpAlloca);
                    structPtr = tmpAlloca;
                }
            }

            llvm::Value* dataPtr = structPtr;

            if (isHeapBase) {
                // Heap<T>: 若来自 _localVarPtrs（alloca slot T**），load 出 T*
                // 若来自 compileExpr（已是 T*），无需额外 load — 但 _localVarPtrs
                // 和 compileExpr 都返回 ptr，靠 heapFromLocal 区分
                if (heapFromLocal) {
                    dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtr, "heap.ptr");
                }
            } else if ((baseType.isRc() ||
                        (baseType.isRef() && baseType.refElementType() && baseType.refElementType()->isRc()))) {
                // Ref<Rc<...>>：先 deref 拿到指向 Rc struct 的指针，再提取 handle
                if (baseType.isRef()) {
                    dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtr, "ref.deref");
                }
                // Rc.field：load handle，payload = handle + 8
                // 用剥掉 Ref 后的 Rc 类型取 struct layout（Ref 的 LLVM 类型是 ptr，不是 Rc struct）
                TypeInfo rcType = baseType.isRef() ? *baseType.refElementType() : baseType;
                auto rcStructType = getLLVMType(rcType);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto handleField = _builder.CreateGEP(rcStructType, dataPtr, {zero, zero}, "rc.handle_field");
                auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
                dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            } else if (baseType.isRef()) {
                // T& (non-Rc): baseVal 是指针, 被存到了 alloca 里 (structPtr 指向栈槽).
                // GEP 之前必须先 load 出指针值, 否则 GEP 会索引到栈槽上而非 struct 上.
                dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtr, "ref.deref");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            std::array<llvm::Value*, 2> indices{zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, dataPtr, indices, "struct.field");
            auto fieldType = field->getType();

            if (actualType.hasGenericArgs() && structDecl->isGeneric() &&
                actualType.genericArgs.size() == structDecl->typeParams().size()) {
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                    subst[structDecl->typeParams()[i]] =
                        actualType.genericArgs[i] ? *actualType.genericArgs[i] : TypeInfo();
                }
                fieldType = fieldType.substitute(subst);
            }
            // 泛型 struct 方法体内 `$` 的类型不含泛型实参时
            // （如 Ref<Box> 而非 Ref<Box<i32>>），仍需通过 _substStack 替换字段类型 T → i32
            fieldType = applySubst(fieldType);

            return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "field.load");
        }
        // E3152: 实例访问静态字段 (DRAFT-static-vars §4.4)
        // obj.FIELD 形态 —— FIELD 是 #Static 字段，不挂在实例上
        if (structDecl->staticField(member)) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto sym = lookupVarSymbol(objName, node);
            if (sym && _localVarPtrs.contains(objName)) {
                DEBUG_LOG_VAL("    Expr: DotMemberLoad", objName << "." << member);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName]);
            }
        }
    }
    // E3090 由 SemaPass 读路径先抛（方法当值 / 非字段点表达式）；此处防 IR 无成员可降。
    throwSemaGap(node->getLineNumber(), node->getColumn());
}

// 编译 a?.b 安全成员访问
// 语义：a 是 Nullable<T>，T 是结构体，b 是 T 的字段
//   - a 持值 → Nullable<U>{ has=true, value=a._value.b }
//   - a 不持值 → Nullable<U>{ has=false, value=zero }
// 当前实现覆盖字段访问；方法调用 a?.foo() 走 compileSafeDotMethodCall
// 链式 a?.b?.c 自然递归（每层 base 类型为 Nullable<X>，仍走同分支）
llvm::Value* Compiler::compileSafeDotExpr(p<ExprDotNode> node) {
    auto baseExpr = node->baseExpr();
    auto member = node->member();
    auto baseType = baseExpr->getType();

    // E3024 / E3044 / E3040 由 SemaPass tryValidateSafeDot 先抛；此处防 IR 走空路径。
    if (!baseType.isNullable()) {
        throwSemaGap(node->resolveLineNumber(), node->resolveColumn());
    }
    auto innerType = baseType.nullableInnerType();
    // Phase 5: Rc<T>? 自动 deref —— 把 Rc<U> 视为 U 进字段查
    bool innerIsRc = innerType->isRc();
    auto rawInnerType = innerType; // Rc<U>（用于 LLVM 类型 = { ptr handle }）
    if (innerIsRc) {
        auto rcInner = innerType->rcElementType();
        if (!rcInner) {
            throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3050);
        }
        innerType = rcInner;
    }
    auto innerStructDecl = _file->getStructDecl(innerType->name);
    if (!innerStructDecl && _yux && _yux->sdkFile()) {
        innerStructDecl = _yux->sdkFile()->getStructDecl(innerType->name, /*includeBuiltin=*/true);
    }
    if (!innerStructDecl) {
        throwSemaGap(node->resolveLineNumber(), node->resolveColumn());
    }
    int fieldIdx = innerStructDecl->fieldIndex(member);
    if (fieldIdx < 0) {
        throwSemaGap(node->resolveLineNumber(), node->resolveColumn());
    }
    auto fieldType = innerStructDecl->fields()[fieldIdx]->getType();
    if (innerType->isGeneric() && innerStructDecl->isGeneric() &&
        innerType->genericArgs.size() == innerStructDecl->typeParams().size()) {
        map<string, TypeInfo> subst;
        for (size_t i = 0; i < innerStructDecl->typeParams().size(); ++i) {
            subst[innerStructDecl->typeParams()[i]] =
                innerType->genericArgs[i] ? *innerType->genericArgs[i] : TypeInfo();
        }
        fieldType = fieldType.substitute(subst);
    }

    // 结果类型 Nullable<U>
    vector<sp<TypeInfo>> nullArgs;
    nullArgs.push_back(make_shared<TypeInfo>(fieldType));
    TypeInfo resultType("Nullable", nullArgs);

    auto baseLLVMType = getLLVMType(baseType);     // Nullable<T> struct
    auto innerLLVMType = getLLVMType(*innerType);  // T struct
    auto fieldLLVMType = getLLVMType(fieldType);   // U
    auto resultLLVMType = getLLVMType(resultType); // Nullable<U> struct

    auto baseVal = compileExpr(baseExpr);
    auto hasVal = _builder.CreateExtractValue(baseVal, {0}, "sd.base.has");
    auto innerVal = _builder.CreateExtractValue(baseVal, {1}, "sd.base.inner");

    auto resultAlloca = _builder.CreateAlloca(resultLLVMType, nullptr, "sd.result");
    auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    auto resHasField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, zero32}, "sd.res.has");
    auto resValueField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, one32}, "sd.res.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "sd.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "sd.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "sd.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    // then: 取出 inner 的字段，包装到 result
    _builder.SetInsertPoint(thenBB);
    llvm::Value* fieldPtr = nullptr;
    auto fieldIdxConst = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIdx);
    if (innerIsRc) {
        // Rc<U>: 提取 handle，payload = handle + 8，GEP 到字段
        auto rawInnerLLVMTy = getLLVMType(*rawInnerType); // Rc struct { ptr handle }
        auto rcAlloca = _builder.CreateAlloca(rawInnerLLVMTy, nullptr, "sd.rc.tmp");
        _builder.CreateStore(innerVal, rcAlloca);
        auto handleField = _builder.CreateGEP(rawInnerLLVMTy, rcAlloca, {zero32, zero32}, "sd.rc.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "sd.rc.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "sd.rc.payload");
        fieldPtr = _builder.CreateGEP(innerLLVMType, payload, {zero32, fieldIdxConst}, "sd.field");
    } else {
        auto innerAlloca = _builder.CreateAlloca(innerLLVMType, nullptr, "sd.inner.tmp");
        _builder.CreateStore(innerVal, innerAlloca);
        fieldPtr = _builder.CreateGEP(innerLLVMType, innerAlloca, {zero32, fieldIdxConst}, "sd.field");
    }
    auto fieldVal = _builder.CreateLoad(fieldLLVMType, fieldPtr, "sd.field.load");
    _builder.CreateStore(_builder.getInt1(true), resHasField);
    _builder.CreateStore(fieldVal, resValueField);
    _builder.CreateBr(mergeBB);

    // else: 空 Nullable<U>
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    _builder.CreateStore(_builder.getInt1(false), resHasField);
    _builder.CreateStore(llvm::Constant::getNullValue(fieldLLVMType), resValueField);
    _builder.CreateBr(mergeBB);

    // merge: load result 作为 ssa 值
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    return _builder.CreateLoad(resultLLVMType, resultAlloca, "sd.result.load");
}

// 编译 a ?? b 表达式
// 语义：a 是 Nullable<T>。a 持值则结果取 a._value，否则取 b（b 必须可转 T）
// IR 形态：
//   %has = extractvalue %a, 0
//   %v   = extractvalue %a, 1
//   br %has, then, else
//   then: br merge (carry %v)
//   else: %r = compile(b); br merge (carry %r)
//   merge: phi T [%v, then] [%r, else]
llvm::Value* Compiler::compileNullElseExpr(p<ExprNullElseNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto leftType = node->left()->getType();

    // Array<Nullable<T>> 下标返回 Ref<Nullable<T>>（T?&）→ 需先 load Nullable struct
    // 再走 extractvalue，与 compileCompareExpr 中 Nullable == null 的 Ref load 对齐
    bool needRefLoad = false;
    if (leftType.isRef()) {
        if (auto refInner = leftType.refElementType(); refInner && refInner->isNullable()) {
            leftType = *refInner;
            needRefLoad = true;
        }
    }

    // E3024 / E3014 由 SemaPass NullElse 先抛；此处防 IR extractvalue 走空路径。
    if (!leftType.isNullable()) {
        throwSemaGap(node->resolveLineNumber(), node->resolveColumn());
    }
    auto innerType = leftType.nullableInnerType();
    if (!innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3050);
    }
    auto innerLLVMType = getLLVMType(*innerType);

    // flexible int 推断：右侧无后缀整数 → 推断为 T
    if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->right())) {
        tryInferIntType(node->right(), *innerType);
    }
    // flexible null 推断：右侧 null 字面量 → 推断为 Nullable<T>（如 b: i32?? 的 ?? null）
    tryInferNullType(node->right(), *innerType);

    // 计算左侧（Nullable 结构体值）
    auto leftVal = compileExpr(node->left());
    // Ref<Nullable<T>>：compileExpr 返回指向 Nullable struct 的指针，需 load 出 struct 值
    if (needRefLoad) {
        leftVal = _builder.CreateLoad(getLLVMType(leftType), leftVal, "ne.ref.load");
    }
    auto hasVal = _builder.CreateExtractValue(leftVal, {0}, "ne.has");
    auto valueVal = _builder.CreateExtractValue(leftVal, {1}, "ne.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "ne.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "ne.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "ne.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    bool isRcHandle = innerType->isRc() || innerType->isArrayGeneric() || innerType->isWeak();

    // then: 持值，直接用 _value（这是从 Nullable struct 抽出的借用，需 retain 归一为 +1）
    _builder.SetInsertPoint(thenBB);
    if (isRcHandle) {
        emitRetainOnHandleValue(valueVal, *innerType);
    }
    auto thenEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // else: 取右侧默认值（用子帧 + 归一）
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto rightType = node->right()->getType();
    auto rightVal = compileBranchResultNormalized(node->right(), *innerType);
    if (rightType != *innerType) {
        throwSemaGap(node->resolveLineNumber(), node->resolveColumn());
    }
    auto elseEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // merge: phi 合并
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    auto phi = _builder.CreatePHI(innerLLVMType, 2, "ne.result");
    phi->addIncoming(valueVal, thenEndBB);
    phi->addIncoming(rightVal, elseEndBB);
    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (isRcHandle) {
        recordTemp(phi, *innerType);
    }
    return phi;
}

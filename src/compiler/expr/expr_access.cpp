// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 成员 / 索引 / 安全访问表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler_runtime.h"
#include "../compiler.h"
#include <algorithm>
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "analyzer/symbol_suggest.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include "sema/call_resolve.h"
#include <set>

llvm::Value* Compiler::compileArrayGetExpr(p<ExprGetNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        // Phase 3.4.g: yux*.g4 强制 indices >= 1, 该分支不可达; 保留作 dead 防御。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3060);
    }

    DEBUG_LOG_VAL("    Expr: ArrayGet", arrayType.name);

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3033, varName);
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

    if (!currentPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3061);
    }

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            // Phase 3.4.g: ExprGetNode::getType 已抛 E3057 (同条件, kMigratedCodes 命中);
            // 这里的 E3055 在 sema 跑过后不可达, 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3055);
        }

        auto elemLLVMType = getLLVMType(*elemType);
        auto handle = loadArrayHandle(currentPtr);
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayBlockDataFieldPtr(handle), "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        return _builder.CreateLoad(elemLLVMType, elemPtr, "array.elem.load");
    }

    if (!arrayType.isArray()) {
        // Phase 3.4.g: ExprGetNode::getType 已抛 E3062 (kMigratedCodes 命中,
        // SemaPass 自动重抛), 此处不可达; 保留作幂等防御性双跑。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3062, arrayType.name);
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

    return _builder.CreateLoad(getLLVMType(currentType), currentPtr, "array.load");
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
    if (!member.empty() && std::ranges::all_of(member,
                                       [](char c) { return c >= '0' && c <= '9'; })) {
        auto baseTypeRaw = baseExpr->getType();
        auto baseTypeResolved = applySubst(baseTypeRaw);
        if (baseTypeResolved.isTuple()) {
            auto& elems = baseTypeResolved.tupleElements();
            auto idx = static_cast<size_t>(std::stoul(member));
            if (idx >= elems.size()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3100,
                               member, baseTypeRaw.getFullName(),
                               std::to_string(elems.size()));
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
        _castFunctions[castFnName] = {.value=baseVal, .srcType=srcType, .dstType=TypeInfo(dstType)};

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
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3133);
        }
        if (_currentStructName.empty()) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3134);
        }
        auto selfIt = _localVarPtrs.find("$");
        if (selfIt == _localVarPtrs.end()) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3134);
        }
        auto structType = getLLVMType(TypeInfo(_currentStructName));
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

    if (baseType.isRef()) {
        auto refElemType = baseType.refElementType();
        if (refElemType) {
            actualType = *refElemType;
        }
    }

    if (baseType.isRc()) {
        auto rcElemType = baseType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
        }
    }

    auto structDecl = _file->getStructDecl(actualType.name);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(actualType.name);
    }

    if (structDecl) {
        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            // Phase 3.4.d.2: E3042 私有字段可见性 整体抠到 sema::validatePrivateFieldAccess.
            // SemaPass.visitExpr ExprDotNode 分支调用 validateDotFieldPrivacy 已抢先抛;
            // 这里保留作幂等防御性双跑.
            sema::validatePrivateFieldAccess(structDecl, member, actualType.name,
                                             _currentStructName,
                                             node->getLineNumber(), node->getColumn());
            auto field = structDecl->fields()[fieldIndex];

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        structPtr = it->second;
                    }
                }
            }

            if (!structPtr) {
                auto baseVal = compileExpr(baseExpr);
                auto tmpAlloca = _builder.CreateAlloca(getLLVMType(baseType), nullptr, "struct_field_tmp");
                _builder.CreateStore(baseVal, tmpAlloca);
                structPtr = tmpAlloca;
            }

            llvm::Value* dataPtr = structPtr;

            if (baseType.isRc()) {
                // Rc.field：load handle，payload = handle + 8
                auto rcStructType = getLLVMType(baseType);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto handleField = _builder.CreateGEP(rcStructType, structPtr, {zero, zero}, "rc.handle_field");
                auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
                dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            std::array<llvm::Value*, 2> indices{zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, dataPtr, indices, "struct.field");
            auto fieldType = field->getType();

            if (actualType.isGeneric() && structDecl->isGeneric()
                && actualType.genericArgs.size() == structDecl->typeParams().size()) {
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                    subst[structDecl->typeParams()[i]] =
                        actualType.genericArgs[i] ? *actualType.genericArgs[i] : TypeInfo();
                }
                fieldType = fieldType.substitute(subst);
            }

            return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "field.load");
        }
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto sym = _currentFnNode->lookupSymbol(objName);
            if (sym && _localVarPtrs.contains(objName)) {
                DEBUG_LOG_VAL("    Expr: DotMemberLoad", objName << "." << member);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName]);
            }
        }
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3090);
}

// 编译 a?.b 安全成员访问
// 语义：a 是 Nullable<T>，T 是结构体，b 是 T 的字段
//   - a 持值 → Nullable<U>{ has=true, value=a._value.b }
//   - a 不持值 → Nullable<U>{ has=false, value=zero }
// 当前实现仅覆盖字段访问；方法调用形式 a?.foo() 不在本阶段
// 链式 a?.b?.c 自然递归（每层 base 类型为 Nullable<X>，仍走同分支）
llvm::Value* Compiler::compileSafeDotExpr(p<ExprDotNode> node) {
    auto baseExpr = node->baseExpr();
    auto member = node->member();
    auto baseType = baseExpr->getType();

    if (!baseType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3025, baseType.name);
    }
    auto innerType = baseType.nullableInnerType();
    // Phase 5: Rc<T>? 自动 deref —— 把 Rc<U> 视为 U 进字段查
    bool innerIsRc = innerType->isRc();
    auto rawInnerType = innerType;  // Rc<U>（用于 LLVM 类型 = { ptr handle }）
    if (innerIsRc) {
        auto rcInner = innerType->rcElementType();
        if (!rcInner) {
            throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3050);
        }
        innerType = rcInner;
    }
    auto innerStructDecl = _file->getStructDecl(innerType->name);
    if (!innerStructDecl && _yux && _yux->sdkFile()) {
        innerStructDecl = _yux->sdkFile()->getStructDecl(innerType->name);
    }
    if (!innerStructDecl) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3044, innerType->name);
    }
    int fieldIdx = innerStructDecl->fieldIndex(member);
    if (fieldIdx < 0) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3040, innerType->name, member);
    }
    auto fieldType = innerStructDecl->fields()[fieldIdx]->getType();
    if (innerType->isGeneric() && innerStructDecl->isGeneric()
        && innerType->genericArgs.size() == innerStructDecl->typeParams().size()) {
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
        auto rawInnerLLVMTy = getLLVMType(*rawInnerType);  // Rc struct { ptr handle }
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
    if (!leftType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3024, leftType.name);
    }
    auto innerType = leftType.nullableInnerType();
    if (!innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3051);
    }
    auto innerLLVMType = getLLVMType(*innerType);

    // flexible int 推断：右侧无后缀整数 → 推断为 T
    if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->right())) {
        tryInferIntType(node->right(), *innerType);
    }

    // 计算左侧（Nullable 结构体值）
    auto leftVal = compileExpr(node->left());
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
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3023, rightType.name, innerType->name);
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

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 方法调用编译：从 compiler_call.cpp 拆出 (P1 Phase 3)
// 覆盖 compileMethodCall + 数组 / 内置 / 结构体 / Dyn 子分发。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "sema/call_resolve.h"
#include <functional>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace {

// 方法 receiver 是局部变量或字段链时，应直接传原存储地址。
// 调用结果等临时值仍走备用 alloca，不把其误当左值。
bool isAddressableMethodReceiver(ExprNode* node) {
    bool hasField = false;
    while (auto* dot = dynamic_cast<ExprDotNode*>(node)) {
        hasField = true;
        node = dot->baseExpr();
    }
    auto* literal = dynamic_cast<ExprLiteralNode*>(node);
    return hasField && literal && dynamic_cast<LiteralObjNode*>(literal->literal());
}

} // namespace

// ==================== 安全方法调用编译 (a?.foo()) ====================
// 编译 a?.foo(args) 安全方法调用表达式
// 语义：a 是 Nullable<T>，T 有方法 foo
//   - a 持值 → 求值实参并调用 a._value.foo(args)，结果包装为 Nullable<ret>
//   - a 不持值 → 不求值实参（§4.1.1.3 短路），Nullable<ret>{ has=false, value=zero }
// 模式与 compileSafeDotExpr 一致：extractvalue + br + then/else/merge BB
llvm::Value* Compiler::compileSafeDotMethodCall(ExprCallNode* callNode, ExprDotNode* dotNode,
                                                vector<TypeInfo>& argTypes) {
    auto baseExpr = dotNode->baseExpr();
    auto member = dotNode->member();
    auto baseType = resolvedOrInferredType(baseExpr);

    // E3024 由 SemaPass tryValidateSafeDot 先抛；此处防 IR 走空路径。
    if (!baseType.isNullable()) {
        throwSemaGap(dotNode->resolveLineNumber(), dotNode->resolveColumn());
    }
    auto innerType = baseType.nullableInnerType();
    if (!innerType) {
        throwSemaGap(dotNode->resolveLineNumber(), dotNode->resolveColumn());
    }
    // Phase 5: Rc<T>? 自动 deref
    bool innerIsRc = innerType->isRc();
    auto rawInnerType = innerType;
    if (innerIsRc) {
        auto rcInner = innerType->rcElementType();
        if (!rcInner) {
            throwSemaGap(dotNode->resolveLineNumber(), dotNode->resolveColumn());
        }
        innerType = rcInner;
    }

    // 去掉 Ref/Heap/Rc 包装，拿到实际方法查找类型
    TypeInfo actualType = *innerType;
    if (actualType.isRef()) {
        if (auto e = actualType.refElementType()) actualType = *e;
    } else if (actualType.isHeap()) {
        if (auto e = actualType.heapElementType()) actualType = *e;
    } else if (actualType.isRc()) {
        if (auto e = actualType.rcElementType()) actualType = *e;
    }

    // DRAFT-spec-disambig-at: 处理 @Spec 后缀
    if (dotNode->hasSpecQualifier()) {
        auto bt = dotNode->baseExpr()->getType();
        if (!bt.isDyn()) {
            member = member + "@" + dotNode->specQualifier();
        }
    }

    // 查找方法符号
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(actualType);
    methodParamTypes.insert(methodParamTypes.end(), argTypes.begin(), argTypes.end());

    FnSymbolInfo* methodSymbol = names().lookupMethodWithParams(actualType, member, methodParamTypes);

    // 泛型 struct 实例方法
    FnNode* genericMethodNode = nullptr;
    string genericEffName;
    map<string, TypeInfo> genericSubst;
    if (!methodSymbol && actualType.hasGenericArgs()) {
        FileNode* owner = _file;
        auto baseDecl = names().lookupStruct(actualType, /*includeBuiltin=*/false, &owner);
        if (!owner) owner = _file;
        if (baseDecl && baseDecl->isGeneric()) {
            genericEffName = genericStruct(baseDecl, actualType.genericArgs, owner);
            auto& inst = _generic.structs()[genericEffName];
            if (inst.baseImpl) {
                for (auto m : inst.baseImpl->methods()) {
                    if (m->header()->name().getText() != member) continue;
                    if (m->header()->params().size() != argTypes.size()) continue;
                    genericMethodNode = m;
                    genericSubst = inst.substMap();
                    break;
                }
            }
        }
    }

    if (!methodSymbol && !genericMethodNode) {
        return nullptr; // 方法未找到，由调用方处理错误
    }

    // 方法返回类型
    TypeInfo retType;
    if (methodSymbol) {
        retType = methodSymbol->retType;
        // 泛型替换
        if (actualType.hasGenericArgs() && !actualType.genericArgs.empty()) {
            StructDeclNode* structDecl = names().lookupStruct(actualType, /*includeBuiltin=*/true);
            if (structDecl && structDecl->isGeneric() &&
                structDecl->typeParams().size() == actualType.genericArgs.size()) {
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < actualType.genericArgs.size(); ++i) {
                    auto& a = actualType.genericArgs[i];
                    subst[structDecl->typeParams()[i]] = a ? *a : TypeInfo();
                }
                retType = retType.substitute(subst);
                if (structDecl) {
                    string eff = actualType.getMangleName();
                    retType = bindStructSelfType(retType, structDecl->name().getText(), eff);
                }
            }
        }
    } else if (genericMethodNode && genericMethodNode->header()->retType()) {
        retType = genericMethodNode->header()->retType()->getType().substitute(genericSubst);
        retType = bindStructSelfType(retType, actualType.name, genericEffName);
    }
    if (retType.empty()) retType = TypeInfo(TupleTag{}, {});

    // 结果类型 Nullable<ret>
    vector<sp<TypeInfo>> nullArgsInner;
    nullArgsInner.push_back(make_shared<TypeInfo>(retType));
    TypeInfo resultType("Nullable", nullArgsInner);

    // LLVM 类型
    auto baseLLVMType = getLLVMType(baseType);
    auto innerLLVMType = getLLVMType(*rawInnerType); // Rc struct 或 bare struct
    auto resultLLVMType = getLLVMType(resultType);

    // 编译 base 表达式 (Nullable<T>)
    auto baseVal = compileExpr(baseExpr);
    auto hasVal = _builder.CreateExtractValue(baseVal, {0}, "sd.m.has");
    auto innerVal = _builder.CreateExtractValue(baseVal, {1}, "sd.m.inner");

    // 结果 alloca
    auto resultAlloca = _builder.CreateAlloca(resultLLVMType, nullptr, "sd.m.result");
    auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    auto resHasField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, zero32}, "sd.m.res.has");
    auto resValueField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, one32}, "sd.m.res.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "sd.m.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "sd.m.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "sd.m.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    // ==== then: 调用方法，包装结果为 Nullable<ret> ====
    _builder.SetInsertPoint(thenBB);

    llvm::Value* receiverArg = nullptr;
    llvm::Value* dataPtr = nullptr;

    if (innerIsRc) {
        // Rc<T>?: 提取 handle → payload = handle + 8
        auto rcAlloca = _builder.CreateAlloca(innerLLVMType, nullptr, "sd.m.rc.tmp");
        _builder.CreateStore(innerVal, rcAlloca);
        auto handleField = _builder.CreateGEP(innerLLVMType, rcAlloca, {zero32, zero32}, "sd.m.rc.handle");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "sd.m.rc.handle");
        dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "sd.m.rc.payload");
    } else {
        // 局部 T?：直接用槽内 _value 指针，让 mutating 方法写回原变量
        llvm::Value* localSlot = nullptr;
        if (auto lit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                auto it = _localVarPtrs.find(obj->getValue().getText());
                if (it != _localVarPtrs.end()) {
                    localSlot = _builder.CreateGEP(baseLLVMType, it->second, {zero32, one32}, "sd.m.inner.slot");
                }
            }
        }
        if (localSlot) {
            dataPtr = localSlot;
        } else {
            auto innerAlloca = _builder.CreateAlloca(innerLLVMType, nullptr, "sd.m.inner.tmp");
            _builder.CreateStore(innerVal, innerAlloca);
            dataPtr = innerAlloca;
        }
    }

    bool receiverByValue = isBuiltinType(actualType.name);
    receiverArg = dataPtr;
    if (receiverByValue) {
        receiverArg = _builder.CreateLoad(getLLVMType(actualType), dataPtr, "sd.m.receiver.val");
    }

    // 构建方法调用参数（实参在 then 内求值，空 receiver 不触发副作用）
    vector<llvm::Value*> methodArgs;
    methodArgs.push_back(receiverArg);
    static const vector<TypeInfo> emptyParams;
    const auto& mparams = methodSymbol ? methodSymbol->params : emptyParams;
    const auto& callArgs = callNode->getArgs();
    for (size_t i = 0; i < callArgs.size(); ++i) {
        auto& at = argTypes[i];
        auto argVal = compileExpr(callArgs[i]);
        size_t mpi = i + 1;
        TypeInfo formal = mpi < mparams.size() ? mparams[mpi] : at;
        if (!formal.isRef()) passAsArg(argVal, at, callArgs[i]);
        methodArgs.push_back(abiValueForParam(callArgs[i], argVal, formal));
    }

    // 获取或创建 LLVM 函数
    llvm::Function* llvmFn = nullptr;
    if (genericMethodNode) {
        string ownerMod = _generic.structs()[genericEffName].ownerModule();
        bool methPriv = !member.empty() && member[0] == '_';
        TypeInfo genRetType;
        if (genericMethodNode->header()->retType()) {
            genRetType = genericMethodNode->header()->retType()->getType().substitute(genericSubst);
            genRetType = bindStructSelfType(genRetType, actualType.name, genericEffName);
        }
        string mFallibleErr;
        if (genericMethodNode->header()->fallibleErrTypeNode()) {
            mFallibleErr =
                fallibleErrKey(genericMethodNode->header()->fallibleErrTypeNode()->getType().substitute(genericSubst));
        }
        string mangledName =
            mangleMethod(ownerMod, genericEffName, member, argTypes, methPriv, genRetType, mFallibleErr);
        llvmFn = _module->getFunction(mangledName);
        if (!llvmFn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            for (auto& t : argTypes) {
                if (structParamUsesPointer(t)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto llvmRetType = wrapFallibleRetType(genRetType, mFallibleErr);
            auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
            llvmFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
    } else if (methodSymbol) {
        string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
        bool methPriv = !member.empty() && member[0] == '_';
        vector<TypeInfo> declaredParams(mparams.size() > 1 ? mparams.begin() + 1 : mparams.begin(), mparams.end());
        if (mparams.size() <= 1) declaredParams.clear();
        string mangledName = mangleMethod(ownerMod, actualType.name, member, declaredParams, methPriv,
                                          methodSymbol->retType, methodSymbol->fallibleErrType);
        llvmFn = _module->getFunction(mangledName);
        if (!llvmFn) {
            vector<llvm::Type*> paramTypes;
            if (receiverByValue) {
                paramTypes.push_back(getLLVMType(actualType));
            } else {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            }
            for (size_t i = 1; i < mparams.size(); ++i) {
                auto& t = mparams[i];
                if (structParamUsesPointer(t)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto rt = wrapFallibleRetType(methodSymbol->retType, methodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(rt, paramTypes, false);
            llvmFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
    }

    if (!llvmFn) {
        throwSemaGap(callNode->getLineNumber(), callNode->getColumn());
    }
    if (llvmFn->getReturnType()->isVoidTy()) {
        _builder.CreateCall(llvmFn, methodArgs);
        _builder.CreateStore(_builder.getInt1(true), resHasField);
        _builder.CreateStore(llvm::Constant::getNullValue(getLLVMType(retType)), resValueField);
    } else {
        auto callResult = _builder.CreateCall(llvmFn, methodArgs, "sd.m.call");
        _builder.CreateStore(_builder.getInt1(true), resHasField);
        _builder.CreateStore(callResult, resValueField);
    }
    _builder.CreateBr(mergeBB);

    // ==== else: 空 Nullable<ret> ====
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    _builder.CreateStore(_builder.getInt1(false), resHasField);
    _builder.CreateStore(llvm::Constant::getNullValue(getLLVMType(retType)), resValueField);
    _builder.CreateBr(mergeBB);

    // ==== merge: load result ====
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    return _builder.CreateLoad(resultLLVMType, resultAlloca, "sd.m.result.load");
}

// ==================== 方法调用编译 ====================
// 编译方法调用表达式 (obj.method(args))
// 处理多种情况: 包别名调用、模块别名调用、内置类型方法、数组方法、结构体方法
llvm::Value* Compiler::compileMethodCall(ExprCallNode* callNode, ExprDotNode* dotNode, vector<llvm::Value*>& args,
                                         vector<TypeInfo>& argTypes) {
    auto baseExpr = dotNode->baseExpr();
    auto member = dotNode->member();
    // DRAFT-spec-disambig-at: `$.m@SpecA()` / `obj.m@SpecA()` 把 member 重写为
    // `m@<specShort>`, 由 spec_impl_checker 预登记的 @-tagged fnSymbol 命中.
    // 例外: Dyn<D> 上 `d.m@D()` 走 vtable dispatch (sema 已校验 @D 匹配), 不重写.
    if (dotNode->hasSpecQualifier()) {
        auto bt = baseExpr->getType();
        if (!bt.isDyn()) {
            member = member + "@" + dotNode->specQualifier();
        }
    }

    // 处理包别名调用 / 模块别名调用 (E6001-E6005 已迁至 sema::resolveModuleFnCall)
    if (auto modCall = sema::resolveModuleFnCall(_file, _riu, callNode, dotNode, argTypes); modCall.matched) {
        // 泛型回退：模块限定调用目标为泛型函数时走 compileGenericFunctionCall
        if (modCall.genericFn) {
            return compileGenericFunctionCall(callNode, modCall.fnName, args, argTypes, modCall.genericFn,
                                              modCall.genericOwner);
        }
        return compileKnownFunctionCall(callNode, modCall.fnName, args, argTypes, modCall.fnSym);
    }

    auto baseType = resolvedOrInferredType(baseExpr);
    // §12.4 / §6.4.4：若 baseExpr 类型是当前替换栈中的泛型形参 T，
    // 应用替换得到具体类型（T -> i32 / Counter / ...），后续按具体类型分发
    // 边界 (E1106) 已在调用点 compileGenericFunctionCall 校验过。
    baseType = applySubst(baseType);

    // v0.16: [] 返回 T&——方法分派前剥 & 以便方法查找（效果等同二元运算的 effType）
    // Dyn<D&> 在剥 & 前先走 isDyn 分支；此处不干扰。
    if (baseType.isRef()) {
        auto inner = baseType.refElementType();
        if (inner) baseType = *inner;
    }

    // Heap<T> / Rc<T> 自动解引用：提前到 builtin/Array/Dyn 检查之前
    // 对方法调用应穿透 wrapper 作用在内部 T，与字段访问 / 运算符一致
    TypeInfo actualType = baseType;
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

    // Dyn<D> / Dyn<D&> 方法调用 (Phase 2d 静态检查 + Phase 3d vtable codegen)
    if (baseType.isDyn()) {
        return compileDynMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    // 处理内置类型方法（用 Rc 解引用后的 actualType 做分发）
    if (isBuiltinType(actualType.name)) {
        return compileBuiltinTypeMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    // 处理指针类型方法
    if (baseType.isPtr()) {
        if (member == "to_int") {
            DEBUG_LOG("    Expr: PtrMethod - to_int");
            auto selfVal = compileExpr(baseExpr);
            return _builder.CreatePtrToInt(selfVal, _builder.getInt64Ty(), "ptr_to_int");
        }
    }

    // 表驱动：类型谓词 × 方法名（kBuiltinMethods）
    if (auto* spec = sema::lookupInstanceBuiltin(actualType, member)) {
        if (spec->recv == sema::BuiltinRecv::Array && actualType.isArrayGeneric()) {
            auto result = compileArrayMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
            if (result) return result;
        }
    }

    // 处理结构体方法
    auto structMethodResult = compileStructMethodCall(callNode, baseExpr, baseType, actualType, member, args, argTypes);
    if (structMethodResult) return structMethodResult;

    // 处理内部函数调用 (obj.fn(args) 其中 obj 是函数名)
    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto fnSymbol = _file->lookupFnSymbolWithParams(objName, argTypes);
            if (fnSymbol) {
                string ownerMod = fnSymbol->moduleName.empty() ? _file->moduleName() : fnSymbol->moduleName;
                bool fnPriv = !objName.empty() && objName[0] == '_';
                auto cName =
                    mangleFunction(ownerMod, objName, argTypes, fnPriv, fnSymbol->retType, fnSymbol->fallibleErrType);
                DEBUG_LOG_VAL("    Expr: InnerFnCall (dot)", objName << " -> " << cName);
                auto fn = _module->getFunction(cName);
                if (!fn) {
                    vector<llvm::Type*> paramTypes;
                    paramTypes.reserve(argTypes.size());
                    for (auto& t : argTypes) {
                        paramTypes.push_back(getLLVMType(t));
                    }
                    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
                    fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
                }
                return _builder.CreateCall(fn, args);
            }
        }
    }
    return nullptr;
}

llvm::Value* Compiler::compileArrayMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                              const string& member, vector<llvm::Value*>& args,
                                              vector<TypeInfo>& argTypes) {

    // Heap<T> / Rc<T> 自动解引用：elemType / structType 用内层 Array<T> 而非 wrapper
    TypeInfo arrType = baseType;
    if (baseType.isHeap()) {
        auto heapElem = baseType.heapElementType();
        if (heapElem) arrType = *heapElem;
    }
    if (baseType.isRc()) {
        auto rcElem = baseType.rcElementType();
        if (rcElem) arrType = *rcElem;
    }
    auto elemType = arrType.arrayGenericElementType();
    auto arrayStructType = getLLVMType(arrType);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto sizeTy = getSizeType();
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    llvm::Value* arrayPtr = nullptr;
    if (auto baseLit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
            auto it = _localVarPtrs.find(obj->getValue().getText());
            if (it != _localVarPtrs.end()) {
                arrayPtr = it->second;
            }
        }
    } else if (auto dotBase = dynamic_cast<ExprDotNode*>(baseExpr)) {
        auto outerBase = dotBase->baseExpr();
        auto outerType = outerBase->getType();
        TypeInfo outerActual = outerType;
        if (outerType.isRef()) {
            auto t = outerType.refElementType();
            if (t) outerActual = *t;
        }
        if (outerType.isHeap()) {
            auto t = outerType.heapElementType();
            if (t) outerActual = *t;
        }
        if (outerType.isRc()) {
            auto t = outerType.rcElementType();
            if (t) outerActual = *t;
        }
        llvm::Value* outerPtr = nullptr;
        if (auto ol = dynamic_cast<ExprLiteralNode*>(outerBase)) {
            if (auto oobj = dynamic_cast<LiteralObjNode*>(ol->literal())) {
                auto it = _localVarPtrs.find(oobj->getValue().getText());
                if (it != _localVarPtrs.end()) {
                    outerPtr = it->second;
                }
            }
        }
        auto outerStructDecl = names().lookupStruct(outerActual);
        if (outerPtr && outerStructDecl) {
            int fi = outerStructDecl->fieldIndex(dotBase->member());
            if (fi >= 0) {
                llvm::Value* dataPtr = outerPtr;
                if (outerType.isHeap()) {
                    // Heap<T>: outerPtr 是 alloca slot (T**)，load 出 T* → dataPtr
                    dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), outerPtr, "heap.ptr");
                } else if (outerType.isRc()) {
                    // Rc.field：load handle，payload = handle + 8
                    auto rcStructType = getLLVMType(outerType);
                    auto handleField = _builder.CreateGEP(rcStructType, outerPtr, {zero, zero}, "rc.handle_field");
                    auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
                    dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
                }
                auto outerLLVM = getLLVMType(outerActual);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fi);
                std::array<llvm::Value*, 2> indices{zero, idx};
                arrayPtr = _builder.CreateGEP(outerLLVM, dataPtr, indices, "array.field.ptr");
            }
        }
    }

    // Heap<T> / Rc<T> 自动解引用：从 wrapper 取出 Array<T> 指针
    if (arrayPtr && baseType.isHeap()) {
        arrayPtr = _builder.CreateLoad(ptrTy, arrayPtr, "heap.arr.ptr");
    }
    if (arrayPtr && baseType.isRc()) {
        auto rcStructType = getLLVMType(baseType);
        auto handleField = _builder.CreateGEP(rcStructType, arrayPtr, {zero, zero}, "rc.handle_field");
        auto handle = _builder.CreateLoad(ptrTy, handleField, "rc.handle");
        arrayPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
    }

    auto* spec = sema::lookupInstanceBuiltin(arrType, member);
    if (!spec) return nullptr;

    // Phase 3.3.2.a: Array<T> 方法形态校验 (E3050/E6042/E6027)
    sema::validateArrayMethodCall(arrType, member, args.size(), arrayPtr != nullptr, callNode->getLineNumber(),
                                  callNode->getColumn());

    auto getReadPtr = [&]() -> llvm::Value* {
        if (arrayPtr) return arrayPtr;
        auto baseVal = compileExpr(baseExpr);
        // [] 返回 T&（指针），直接用作 struct 指针，无需 alloca 副本
        // 否则 compileExpr 返回的是 struct 值，需要 alloca 保存再取地址
        if (baseExpr->getType().isRef()) {
            // TODO: Rc<Array<T>>& 路径待支持（需先 load Rc struct 再 unwrap）
            return baseVal;
        }
        // Heap<Array<T>> 自动解引用：compileExpr 返回 Array<T>*，直接使用
        if (baseType.isHeap()) {
            return baseVal;
        }
        // Rc<Array<T>> 自动解引用（P2）：从 Rc handle 取出 Array<T> 指针
        if (baseType.isRc()) {
            auto rcStructType = getLLVMType(baseType);
            auto tmp = _builder.CreateAlloca(rcStructType, nullptr, "rc_tmp");
            _builder.CreateStore(baseVal, tmp);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, tmp, {zero, zero}, "rc.handle_field");
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto handle = _builder.CreateLoad(ptrTy, handleField, "rc.handle");
            return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }
        auto tmp = _builder.CreateAlloca(arrayStructType, nullptr, "array_tmp");
        _builder.CreateStore(baseVal, tmp);
        return tmp;
    };

    // B-3: _len 字段 load 辅助
    auto loadLen = [&](llvm::Value* ptr) -> llvm::Value* {
        return _builder.CreateLoad(sizeTy, arrayLenFieldPtr(ptr, "arr"), "array.len");
    };
    // B-3: _data 字段 load 辅助
    auto loadData = [&](llvm::Value* ptr) -> llvm::Value* {
        return _builder.CreateLoad(ptrTy, arrayDataFieldPtr(ptr, "arr"), "array.data");
    };

    llvm::Type* elemLLVMType = nullptr;
    if (spec->needsElemType && elemType) {
        elemLLVMType = getLLVMType(*elemType);
    }

    auto voidResult = [&]() -> llvm::Value* { return llvm::ConstantInt::get(_builder.getInt32Ty(), 0); };

    auto copyElem = [&](llvm::Value* v) -> llvm::Value* { return copyOwnedValue(v, *elemType); };

    auto wrapNullable = [&](llvm::Value* has, llvm::Value* val, llvm::Type* nty) -> llvm::Value* {
        llvm::Value* r = llvm::UndefValue::get(nty);
        r = _builder.CreateInsertValue(r, has, {0}, "null.has");
        r = _builder.CreateInsertValue(r, val, {1}, "null.val");
        return r;
    };

    auto loadNeedle = [&]() -> llvm::Value* {
        llvm::Value* needle = args[0];
        const bool needleIsPtr = needle->getType()->isPointerTy();
        const bool argIsRef = !argTypes.empty() && argTypes[0].isRef();
        if (needleIsPtr && (argIsRef || !elemLLVMType->isPointerTy())) {
            needle = _builder.CreateLoad(elemLLVMType, needle, "arr.needle");
        }
        return needle;
    };

    auto elemEq = [&](llvm::Value* elemVal, llvm::Value* needle, const char* tag) -> llvm::Value* {
        if (elemType->isFloat()) {
            return _builder.CreateFCmpOEQ(elemVal, needle, (string(tag) + ".eq").c_str());
        }
        if (isBuiltinType(elemType->name)) {
            return _builder.CreateICmpEQ(elemVal, needle, (string(tag) + ".eq").c_str());
        }
        if (elemType->isString()) {
            auto strTy = getLLVMType(*elemType);
            auto lhsA = _builder.CreateAlloca(strTy, nullptr, (string(tag) + ".lhs").c_str());
            auto rhsA = _builder.CreateAlloca(strTy, nullptr, (string(tag) + ".rhs").c_str());
            _builder.CreateStore(elemVal, lhsA);
            _builder.CreateStore(needle, rhsA);
            TypeInfo strRef("Ref", {make_shared<TypeInfo>("String")});
            auto eqFn = getMethodFunction("String", "eq", {strRef}, TypeInfo("bool"));
            return _builder.CreateCall(eqFn, {lhsA, rhsA}, (string(tag) + ".str.eq").c_str());
        }
        auto bytes = _module->getDataLayout().getTypeStoreSize(elemLLVMType);
        auto memPtrTy = llvm::PointerType::get(_context, 0);
        auto memcmpTy =
            llvm::FunctionType::get(_builder.getInt32Ty(), {memPtrTy, memPtrTy, _builder.getInt64Ty()}, false);
        auto memcmpFn = _module->getOrInsertFunction("riurt_memcmp", memcmpTy);
        auto lhsA = _builder.CreateAlloca(elemLLVMType, nullptr, (string(tag) + ".lhs").c_str());
        auto rhsA = _builder.CreateAlloca(elemLLVMType, nullptr, (string(tag) + ".rhs").c_str());
        _builder.CreateStore(elemVal, lhsA);
        _builder.CreateStore(needle, rhsA);
        auto cmp = _builder.CreateCall(memcmpFn, {lhsA, rhsA, llvm::ConstantInt::get(_builder.getInt64Ty(), bytes)},
                                       (string(tag) + ".memcmp").c_str());
        return _builder.CreateICmpEQ(cmp, _builder.getInt32(0), (string(tag) + ".eq").c_str());
    };

    auto emitOobAbort = [&](llvm::Value* oob, const char* okName) {
        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* dieBB = llvm::BasicBlock::Create(_context, "arr.oob", fn);
        auto* okBB = llvm::BasicBlock::Create(_context, okName, fn);
        _builder.CreateCondBr(oob, dieBB, okBB);
        _builder.SetInsertPoint(dieBB);
        auto abortFn = runtime::getRiurtAbortFn(_module, _builder);
        _builder.CreateCall(abortFn);
        _builder.CreateUnreachable();
        _builder.SetInsertPoint(okBB);
    };

    auto emitSlice = [&](llvm::Value* startRaw, llvm::Value* endRaw) -> llvm::Value* {
        auto ptr = getReadPtr();
        auto oldData = loadData(ptr);
        auto oldLen = loadLen(ptr);
        auto resultTy = arrayStructType;
        auto startV = _builder.CreateZExtOrTrunc(startRaw, sizeTy, "slice.start");
        auto endV = _builder.CreateZExtOrTrunc(endRaw, sizeTy, "slice.end");
        auto umin = [&](llvm::Value* a, llvm::Value* b, const char* nm) {
            return _builder.CreateSelect(_builder.CreateICmpULT(a, b, "slice.lt"), a, b, nm);
        };
        auto sClamped = umin(startV, oldLen, "slice.s");
        auto eClamped = umin(endV, oldLen, "slice.e");
        auto startIdx = umin(sClamped, eClamped, "slice.start_idx");
        auto count = _builder.CreateSub(eClamped, startIdx, "slice.n");

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        llvm::Value* emptyArr = llvm::UndefValue::get(resultTy);
        emptyArr = _builder.CreateInsertValue(emptyArr, nullPtr, {0}, "slice.empty.data");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {1}, "slice.empty.len");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {2}, "slice.empty.cap");

        auto* allocBB = llvm::BasicBlock::Create(_context, "slice.alloc", fn);
        auto* loopHdrBB = llvm::BasicBlock::Create(_context, "slice.loop.hdr", fn);
        auto* loopBodyBB = llvm::BasicBlock::Create(_context, "slice.loop.body", fn);
        auto* loopLatchBB = llvm::BasicBlock::Create(_context, "slice.loop.latch", fn);
        auto* loopExitBB = llvm::BasicBlock::Create(_context, "slice.loop.exit", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "slice.done", fn);

        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        auto nIsZero = _builder.CreateICmpEQ(count, zeroSize, "slice.is_empty");
        _builder.CreateCondBr(nIsZero, doneBB, allocBB);

        _builder.SetInsertPoint(allocBB);
        auto elemSizeVal = _builder.getInt64(_module->getDataLayout().getTypeAllocSize(elemLLVMType).getFixedValue());
        auto countI64 = _builder.CreateZExtOrTrunc(count, _builder.getInt64Ty(), "slice.n.i64");
        auto startI64 = _builder.CreateZExtOrTrunc(startIdx, _builder.getInt64Ty(), "slice.start.i64");
        auto newSize = _builder.CreateMul(countI64, elemSizeVal, "slice.new_size");
        auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
        auto newData = _builder.CreateCall(allocFn, {newSize}, "slice.new_data");
        _builder.CreateBr(loopHdrBB);

        _builder.SetInsertPoint(loopHdrBB);
        auto loopPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "slice.i");
        loopPhi->addIncoming(_builder.getInt64(0), allocBB);
        auto loopCond = _builder.CreateICmpULT(loopPhi, countI64, "slice.loop.cond");
        _builder.CreateCondBr(loopCond, loopBodyBB, loopExitBB);

        _builder.SetInsertPoint(loopBodyBB);
        auto srcIdx = _builder.CreateAdd(startI64, loopPhi, "slice.src.idx");
        auto oldElemPtr = _builder.CreateInBoundsGEP(elemLLVMType, oldData, {srcIdx}, "slice.old.ptr");
        llvm::Value* elemVal = copyElem(_builder.CreateLoad(elemLLVMType, oldElemPtr, "slice.elem"));
        auto newElemPtr = _builder.CreateInBoundsGEP(elemLLVMType, newData, {loopPhi}, "slice.new.ptr");
        _builder.CreateStore(elemVal, newElemPtr);
        _builder.CreateBr(loopLatchBB);

        _builder.SetInsertPoint(loopLatchBB);
        auto iNext = _builder.CreateAdd(loopPhi, _builder.getInt64(1), "slice.i.next");
        loopPhi->addIncoming(iNext, loopLatchBB);
        _builder.CreateBr(loopHdrBB);

        _builder.SetInsertPoint(loopExitBB);
        llvm::Value* newArr = llvm::UndefValue::get(resultTy);
        newArr = _builder.CreateInsertValue(newArr, newData, {0}, "slice.res.data");
        newArr = _builder.CreateInsertValue(newArr, count, {1}, "slice.res.len");
        newArr = _builder.CreateInsertValue(newArr, count, {2}, "slice.res.cap");
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto resultPhi = _builder.CreatePHI(resultTy, 2, "slice.result");
        resultPhi->addIncoming(emptyArr, startBB);
        resultPhi->addIncoming(newArr, loopExitBB);
        return resultPhi;
    };

    switch (spec->lower) {
    case sema::BuiltinLower::ArrayLen:
        DEBUG_LOG("    Expr: Array.len()");
        return loadLen(getReadPtr());
    case sema::BuiltinLower::ArrayCap: {
        DEBUG_LOG("    Expr: Array.cap()");
        auto ptr = getReadPtr();
        return _builder.CreateLoad(sizeTy, arrayCapFieldPtr(ptr, "arr"), "array.cap");
    }
    case sema::BuiltinLower::ArrayIsEmpty: {
        DEBUG_LOG("    Expr: Array.is_empty()");
        auto lenVal = loadLen(getReadPtr());
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        return _builder.CreateICmpEQ(lenVal, zeroSize, "array.is_empty");
    }
    case sema::BuiltinLower::ArrayGet: {
        DEBUG_LOG("    Expr: Array.get() → T&");
        auto ptr = getReadPtr();
        auto lenVal = loadLen(ptr);
        auto idx = _builder.CreateZExtOrTrunc(args[0], sizeTy, "get.i");
        emitOobAbort(_builder.CreateICmpUGE(idx, lenVal, "get.oob"), "get.ok");
        auto dataPtr = loadData(ptr);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "get.elem.ptr");
        if (elemType) {
            callNode->setResolvedType(TypeInfo("Ref", {elemType}));
        }
        return elemPtr;
    }
    case sema::BuiltinLower::ArrayFirst: {
        DEBUG_LOG("    Expr: Array.first() → T&");
        auto ptr = getReadPtr();
        auto lenVal = loadLen(ptr);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        emitOobAbort(_builder.CreateICmpEQ(lenVal, zeroSize, "first.empty"), "first.ok");
        auto dataPtr = loadData(ptr);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {zeroSize}, "first.elem.ptr");
        if (elemType) {
            callNode->setResolvedType(TypeInfo("Ref", {elemType}));
        }
        return elemPtr;
    }
    case sema::BuiltinLower::ArrayLast: {
        DEBUG_LOG("    Expr: Array.last() → T&");
        auto ptr = getReadPtr();
        auto lenVal = loadLen(ptr);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        emitOobAbort(_builder.CreateICmpEQ(lenVal, zeroSize, "last.empty"), "last.ok");
        auto oneSize = llvm::ConstantInt::get(sizeTy, 1);
        auto lastIdx = _builder.CreateSub(lenVal, oneSize, "last.idx");
        auto dataPtr = loadData(ptr);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "last.elem.ptr");
        if (elemType) {
            callNode->setResolvedType(TypeInfo("Ref", {elemType}));
        }
        return elemPtr;
    }
    case sema::BuiltinLower::ArrayPop: {
        DEBUG_LOG("    Expr: Array.pop()");
        auto lenFieldPtr = arrayLenFieldPtr(arrayPtr, "arr");
        auto lenVal = _builder.CreateLoad(sizeTy, lenFieldPtr, "a.len");
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        emitOobAbort(_builder.CreateICmpEQ(lenVal, zeroSize, "pop.empty"), "pop.ok");
        auto oneSize = llvm::ConstantInt::get(sizeTy, 1);
        auto lastIdx = _builder.CreateSub(lenVal, oneSize, "pop.idx");

        auto dataPtr = loadData(arrayPtr);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "pop.elem.ptr");
        auto elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "pop.elem");

        _builder.CreateStore(lastIdx, lenFieldPtr);
        return elemVal;
    }
    case sema::BuiltinLower::ArrayClear:
    case sema::BuiltinLower::ArraySetLen:
    case sema::BuiltinLower::ArrayReserve:
    case sema::BuiltinLower::ArrayPush: {
        auto lenFieldPtr = arrayLenFieldPtr(arrayPtr, "arr");
        auto capFieldPtr = arrayCapFieldPtr(arrayPtr, "arr");
        auto dataFieldPtr = arrayDataFieldPtr(arrayPtr, "arr");

        if (spec->lower == sema::BuiltinLower::ArrayClear) {
            DEBUG_LOG("    Expr: Array.clear()");
            auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
            auto oldLen = _builder.CreateLoad(sizeTy, lenFieldPtr, "clear.old_len");
            releaseArrayElements(arrayPtr, arrType, zeroSize, oldLen);
            _builder.CreateStore(zeroSize, lenFieldPtr);
            return voidResult();
        }
        if (spec->lower == sema::BuiltinLower::ArraySetLen) {
            DEBUG_LOG("    Expr: Array.set_len()");
            auto newLen = _builder.CreateZExtOrTrunc(args[0], sizeTy, "set_len.n");
            auto oldLen = _builder.CreateLoad(sizeTy, lenFieldPtr, "set_len.old_len");
            emitOobAbort(_builder.CreateICmpUGT(newLen, oldLen, "set_len.grow"), "set_len.ok");
            releaseArrayElements(arrayPtr, arrType, newLen, oldLen);
            _builder.CreateStore(newLen, lenFieldPtr);
            return voidResult();
        }
        if (spec->lower == sema::BuiltinLower::ArrayReserve) {
            DEBUG_LOG("    Expr: Array.reserve()");
            auto additional = args[0];
            auto lenVal = _builder.CreateLoad(sizeTy, lenFieldPtr, "a.len");
            auto capVal = _builder.CreateLoad(sizeTy, capFieldPtr, "a.cap");
            auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
            auto needCap = _builder.CreateAdd(lenVal, additional, "need.cap");
            // 检查是否上溢（needCap < lenVal → overflow → 转为 usize 最大）
            auto overflowed = _builder.CreateICmpULT(needCap, lenVal, "overflow");
            auto maxSize = llvm::ConstantInt::get(sizeTy, ~0ULL);
            auto safeNeedCap = _builder.CreateSelect(overflowed, maxSize, needCap, "safe.need");
            auto needGrow = _builder.CreateICmpUGT(safeNeedCap, capVal, "reserve.need_grow");

            auto doneBB = llvm::BasicBlock::Create(_context, "reserve.done", _currentFn);
            auto growBB = llvm::BasicBlock::Create(_context, "reserve.grow", _currentFn);
            _builder.CreateCondBr(needGrow, growBB, doneBB);

            // growBB: 扩容路径
            _builder.SetInsertPoint(growBB);
            auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
            auto elemSizeVal = llvm::ConstantInt::get(sizeTy, elemSize);
            auto newByteSize = _builder.CreateMul(safeNeedCap, elemSizeVal, "new.byte_size");
            auto oldData = _builder.CreateLoad(ptrTy, dataFieldPtr, "old.data");
            auto allocFn = runtime::getRiurtAllocFn(_module, _builder);
            auto reallocFn = runtime::getRiurtReallocFn(_module, _builder);

            auto dataIsNull = _builder.CreateICmpEQ(oldData, nullPtr, "data.is_null");
            auto allocBB = llvm::BasicBlock::Create(_context, "reserve.alloc", _currentFn);
            auto reallocBB = llvm::BasicBlock::Create(_context, "reserve.realloc", _currentFn);
            auto growDoneBB = llvm::BasicBlock::Create(_context, "reserve.grow_done", _currentFn);
            _builder.CreateCondBr(dataIsNull, allocBB, reallocBB);

            _builder.SetInsertPoint(allocBB);
            auto alloced = _builder.CreateCall(allocFn, {newByteSize}, "alloced.data");
            _builder.CreateBr(growDoneBB);

            _builder.SetInsertPoint(reallocBB);
            auto realloced = _builder.CreateCall(reallocFn, {oldData, newByteSize}, "realloced");
            _builder.CreateBr(growDoneBB);

            _builder.SetInsertPoint(growDoneBB);
            auto phi = _builder.CreatePHI(ptrTy, 2, "new.data");
            phi->addIncoming(alloced, allocBB);
            phi->addIncoming(realloced, reallocBB);
            _builder.CreateStore(phi, dataFieldPtr);
            _builder.CreateStore(safeNeedCap, capFieldPtr);
            _builder.CreateBr(doneBB);

            _builder.SetInsertPoint(doneBB);
            return voidResult();
        }
        DEBUG_LOG("    Expr: Array.push()");
        auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
        auto elemVal = args[0];
        // 与 arr[i]=expr 对齐：fresh 实参（move / 字面量）从临时帧摘走，避免
        // 语句末 popAndReleaseTempFrame 把已写入缓冲的 Array._data 再 free。
        if (elemType && !callNode->getArgs().empty()) {
            passAsArg(elemVal, *elemType, callNode->getArgs()[0]);
        }
        auto lenVal = _builder.CreateLoad(sizeTy, lenFieldPtr, "a.len");
        auto capVal = _builder.CreateLoad(sizeTy, capFieldPtr, "a.cap");

        auto needGrow = _builder.CreateICmpUGE(lenVal, capVal, "push.need_grow");
        auto growBB = llvm::BasicBlock::Create(_context, "push.grow", _currentFn);
        auto storeBB = llvm::BasicBlock::Create(_context, "push.store", _currentFn);
        _builder.CreateCondBr(needGrow, growBB, storeBB);

        // B-3 扩容路径：直接调 HeapAlloc/HeapReAlloc，更新 _data 和 _cap
        _builder.SetInsertPoint(growBB);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        auto oneSize = llvm::ConstantInt::get(sizeTy, 1);
        auto capIsZero = _builder.CreateICmpEQ(capVal, zeroSize, "cap.is_zero");
        auto doubled = _builder.CreateMul(capVal, llvm::ConstantInt::get(sizeTy, 2), "cap.dbl");
        auto newCap = _builder.CreateSelect(capIsZero, llvm::ConstantInt::get(sizeTy, 4), doubled, "new.cap");

        auto elemSizeVal = llvm::ConstantInt::get(sizeTy, elemSize);
        auto newByteSize = _builder.CreateMul(newCap, elemSizeVal, "new.byte_size");
        auto oldData = _builder.CreateLoad(ptrTy, dataFieldPtr, "old.data");
        auto allocFn = runtime::getRiurtAllocFn(_module, _builder);
        auto reallocFn = runtime::getRiurtReallocFn(_module, _builder);

        // 旧 data 为空 → riurt_alloc，否则 → riurt_realloc
        auto dataIsNull = _builder.CreateICmpEQ(oldData, nullPtr, "data.is_null");
        auto allocBB = llvm::BasicBlock::Create(_context, "push.alloc", _currentFn);
        auto reallocBB = llvm::BasicBlock::Create(_context, "push.realloc", _currentFn);
        auto growDoneBB = llvm::BasicBlock::Create(_context, "push.grow_done", _currentFn);
        _builder.CreateCondBr(dataIsNull, allocBB, reallocBB);

        _builder.SetInsertPoint(allocBB);
        auto alloced = _builder.CreateCall(allocFn, {newByteSize}, "alloced.data");
        _builder.CreateBr(growDoneBB);

        _builder.SetInsertPoint(reallocBB);
        auto realloced = _builder.CreateCall(reallocFn, {oldData, newByteSize}, "realloced");
        _builder.CreateBr(growDoneBB);

        _builder.SetInsertPoint(growDoneBB);
        auto phi = _builder.CreatePHI(ptrTy, 2, "new.data");
        phi->addIncoming(alloced, allocBB);
        phi->addIncoming(realloced, reallocBB);
        _builder.CreateStore(phi, dataFieldPtr);
        _builder.CreateStore(newCap, capFieldPtr);
        _builder.CreateBr(storeBB);

        // 写入新元素并 len++
        _builder.SetInsertPoint(storeBB);
        auto curData = _builder.CreateLoad(ptrTy, dataFieldPtr, "a.data.cur");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, curData, {lenVal}, "push.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
        auto newLen = _builder.CreateAdd(lenVal, oneSize, "new.len");
        _builder.CreateStore(newLen, lenFieldPtr);
        return voidResult();
    }

    case sema::BuiltinLower::ArrayClone: {
        DEBUG_LOG("    Expr: Array.clone() → Array<T> 深拷贝");
        return cloneArrayAtPtr(getReadPtr(), arrType);
    }

    case sema::BuiltinLower::ArraySlice: {
        DEBUG_LOG("    Expr: Array.slice() → Array<T>");
        return emitSlice(args[0], args[1]);
    }

    case sema::BuiltinLower::ArrayTake: {
        DEBUG_LOG("    Expr: Array.take() → Array<T>");
        return emitSlice(llvm::ConstantInt::get(sizeTy, 0), args[0]);
    }

    case sema::BuiltinLower::ArrayDrop: {
        DEBUG_LOG("    Expr: Array.drop() → Array<T>");
        return emitSlice(args[0], loadLen(getReadPtr()));
    }

    case sema::BuiltinLower::ArrayConcat: {
        DEBUG_LOG("    Expr: Array.concat() → Array<T>");
        auto ptr = getReadPtr();
        auto oldData = loadData(ptr);
        auto oldLen = loadLen(ptr);
        llvm::Value* otherPtr = args[0];
        if (!otherPtr->getType()->isPointerTy()) {
            auto tmp = _builder.CreateAlloca(arrayStructType, nullptr, "concat.other_tmp");
            _builder.CreateStore(args[0], tmp);
            otherPtr = tmp;
        }
        auto otherData = loadData(otherPtr);
        auto otherLen = loadLen(otherPtr);
        auto count = _builder.CreateAdd(oldLen, otherLen, "concat.n");
        auto resultTy = arrayStructType;

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        llvm::Value* emptyArr = llvm::UndefValue::get(resultTy);
        emptyArr = _builder.CreateInsertValue(emptyArr, nullPtr, {0}, "concat.empty.data");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {1}, "concat.empty.len");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {2}, "concat.empty.cap");

        auto* allocBB = llvm::BasicBlock::Create(_context, "concat.alloc", fn);
        auto* loop1HdrBB = llvm::BasicBlock::Create(_context, "concat.l1.hdr", fn);
        auto* loop1BodyBB = llvm::BasicBlock::Create(_context, "concat.l1.body", fn);
        auto* loop1LatchBB = llvm::BasicBlock::Create(_context, "concat.l1.latch", fn);
        auto* loop2HdrBB = llvm::BasicBlock::Create(_context, "concat.l2.hdr", fn);
        auto* loop2BodyBB = llvm::BasicBlock::Create(_context, "concat.l2.body", fn);
        auto* loop2LatchBB = llvm::BasicBlock::Create(_context, "concat.l2.latch", fn);
        auto* loopExitBB = llvm::BasicBlock::Create(_context, "concat.exit", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "concat.done", fn);

        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        auto nIsZero = _builder.CreateICmpEQ(count, zeroSize, "concat.is_empty");
        _builder.CreateCondBr(nIsZero, doneBB, allocBB);

        _builder.SetInsertPoint(allocBB);
        auto elemSizeVal = _builder.getInt64(_module->getDataLayout().getTypeAllocSize(elemLLVMType).getFixedValue());
        auto countI64 = _builder.CreateZExtOrTrunc(count, _builder.getInt64Ty(), "concat.n.i64");
        auto len1I64 = _builder.CreateZExtOrTrunc(oldLen, _builder.getInt64Ty(), "concat.len1.i64");
        auto len2I64 = _builder.CreateZExtOrTrunc(otherLen, _builder.getInt64Ty(), "concat.len2.i64");
        auto newSize = _builder.CreateMul(countI64, elemSizeVal, "concat.new_size");
        auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
        auto newData = _builder.CreateCall(allocFn, {newSize}, "concat.new_data");
        _builder.CreateBr(loop1HdrBB);

        auto copyElemAt = [&](llvm::Value* srcData, llvm::Value* srcIdx, llvm::Value* dstIdx, const char* tag) {
            auto oldElemPtr =
                _builder.CreateInBoundsGEP(elemLLVMType, srcData, {srcIdx}, (string(tag) + ".old.ptr").c_str());
            llvm::Value* loaded = _builder.CreateLoad(elemLLVMType, oldElemPtr, (string(tag) + ".elem").c_str());
            llvm::Value* elemVal = copyOwnedValue(loaded, *elemType);
            auto newElemPtr =
                _builder.CreateInBoundsGEP(elemLLVMType, newData, {dstIdx}, (string(tag) + ".new.ptr").c_str());
            _builder.CreateStore(elemVal, newElemPtr);
        };

        _builder.SetInsertPoint(loop1HdrBB);
        auto i1 = _builder.CreatePHI(_builder.getInt64Ty(), 2, "concat.i1");
        i1->addIncoming(_builder.getInt64(0), allocBB);
        auto c1 = _builder.CreateICmpULT(i1, len1I64, "concat.l1.cond");
        _builder.CreateCondBr(c1, loop1BodyBB, loop2HdrBB);

        _builder.SetInsertPoint(loop1BodyBB);
        copyElemAt(oldData, i1, i1, "concat.l1");
        _builder.CreateBr(loop1LatchBB);
        _builder.SetInsertPoint(loop1LatchBB);
        auto i1n = _builder.CreateAdd(i1, _builder.getInt64(1), "concat.i1.next");
        i1->addIncoming(i1n, loop1LatchBB);
        _builder.CreateBr(loop1HdrBB);

        _builder.SetInsertPoint(loop2HdrBB);
        auto i2 = _builder.CreatePHI(_builder.getInt64Ty(), 2, "concat.i2");
        i2->addIncoming(_builder.getInt64(0), loop1HdrBB);
        auto c2 = _builder.CreateICmpULT(i2, len2I64, "concat.l2.cond");
        _builder.CreateCondBr(c2, loop2BodyBB, loopExitBB);

        _builder.SetInsertPoint(loop2BodyBB);
        auto dst2 = _builder.CreateAdd(len1I64, i2, "concat.dst2");
        copyElemAt(otherData, i2, dst2, "concat.l2");
        _builder.CreateBr(loop2LatchBB);
        _builder.SetInsertPoint(loop2LatchBB);
        auto i2n = _builder.CreateAdd(i2, _builder.getInt64(1), "concat.i2.next");
        i2->addIncoming(i2n, loop2LatchBB);
        _builder.CreateBr(loop2HdrBB);

        _builder.SetInsertPoint(loopExitBB);
        llvm::Value* newArr = llvm::UndefValue::get(resultTy);
        newArr = _builder.CreateInsertValue(newArr, newData, {0}, "concat.res.data");
        newArr = _builder.CreateInsertValue(newArr, count, {1}, "concat.res.len");
        newArr = _builder.CreateInsertValue(newArr, count, {2}, "concat.res.cap");
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto resultPhi = _builder.CreatePHI(resultTy, 2, "concat.result");
        resultPhi->addIncoming(emptyArr, startBB);
        resultPhi->addIncoming(newArr, loopExitBB);
        return resultPhi;
    }

    case sema::BuiltinLower::ArrayAny:
    case sema::BuiltinLower::ArrayAll: {
        const bool isAny = spec->lower == sema::BuiltinLower::ArrayAny;
        if (isAny) {
            DEBUG_LOG("    Expr: Array.any()");
        } else {
            DEBUG_LOG("    Expr: Array.all()");
        }
        auto ptr = getReadPtr();
        auto dataPtr = loadData(ptr);
        auto lenVal = loadLen(ptr);
        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "higher.len.i64");
        auto predicate = args[0];
        passAsArg(predicate, argTypes[0], callNode->getArgs()[0]);
        auto predicateSlot = _builder.CreateAlloca(getLLVMType(argTypes[0]), nullptr, "higher.fn.arg");
        _builder.CreateStore(predicate, predicateSlot);
        auto predicatePtr = _builder.CreateExtractValue(predicate, {0}, "higher.fn.ptr");
        auto captures = _builder.CreateExtractValue(predicate, {1}, "higher.fn.captures");
        auto predicateType = llvm::FunctionType::get(_builder.getInt1Ty(), {ptrTy, ptrTy}, false);

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* hdrBB = llvm::BasicBlock::Create(_context, "higher.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "higher.body", fn);
        auto* latchBB = llvm::BasicBlock::Create(_context, "higher.latch", fn);
        auto* shortBB = llvm::BasicBlock::Create(_context, "higher.short", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "higher.done", fn);

        _builder.CreateBr(hdrBB);
        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "higher.i");
        iPhi->addIncoming(_builder.getInt64(0), startBB);
        auto more = _builder.CreateICmpULT(iPhi, lenI64, "higher.more");
        _builder.CreateCondBr(more, bodyBB, doneBB);

        _builder.SetInsertPoint(bodyBB);
        auto elemPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {iPhi}, "higher.elem.ptr");
        auto matched = _builder.CreateCall(predicateType, predicatePtr, {captures, elemPtr}, "higher.match");
        _builder.CreateCondBr(isAny ? matched : _builder.CreateNot(matched, "higher.failed"), shortBB, latchBB);

        _builder.SetInsertPoint(latchBB);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "higher.i.next");
        iPhi->addIncoming(iNext, latchBB);
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(shortBB);
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto result = _builder.CreatePHI(_builder.getInt1Ty(), 2, "higher.result");
        result->addIncoming(llvm::ConstantInt::get(_builder.getInt1Ty(), isAny ? 0 : 1), hdrBB);
        result->addIncoming(llvm::ConstantInt::get(_builder.getInt1Ty(), isAny ? 1 : 0), shortBB);
        releaseAtPtr(predicateSlot, argTypes[0]);
        return result;
    }

    case sema::BuiltinLower::ArrayFilter: {
        DEBUG_LOG("    Expr: Array.filter()");
        auto ptr = getReadPtr();
        auto oldData = loadData(ptr);
        auto oldLen = loadLen(ptr);
        auto lenI64 = _builder.CreateZExtOrTrunc(oldLen, _builder.getInt64Ty(), "filter.len.i64");
        auto predicate = args[0];
        passAsArg(predicate, argTypes[0], callNode->getArgs()[0]);
        auto predicateSlot = _builder.CreateAlloca(getLLVMType(argTypes[0]), nullptr, "filter.fn.arg");
        _builder.CreateStore(predicate, predicateSlot);
        auto predicatePtr = _builder.CreateExtractValue(predicate, {0}, "filter.fn.ptr");
        auto captures = _builder.CreateExtractValue(predicate, {1}, "filter.fn.captures");
        auto predicateType = llvm::FunctionType::get(_builder.getInt1Ty(), {ptrTy, ptrTy}, false);

        llvm::Value* emptyArr = llvm::UndefValue::get(arrayStructType);
        emptyArr = _builder.CreateInsertValue(emptyArr, nullPtr, {0}, "filter.empty.data");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {1}, "filter.empty.len");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {2}, "filter.empty.cap");

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* allocBB = llvm::BasicBlock::Create(_context, "filter.alloc", fn);
        auto* hdrBB = llvm::BasicBlock::Create(_context, "filter.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "filter.body", fn);
        auto* matchBB = llvm::BasicBlock::Create(_context, "filter.match", fn);
        auto* skipBB = llvm::BasicBlock::Create(_context, "filter.skip", fn);
        auto* latchBB = llvm::BasicBlock::Create(_context, "filter.latch", fn);
        auto* finishBB = llvm::BasicBlock::Create(_context, "filter.finish", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "filter.done", fn);

        auto isEmpty = _builder.CreateICmpEQ(oldLen, llvm::ConstantInt::get(sizeTy, 0), "filter.is_empty");
        _builder.CreateCondBr(isEmpty, doneBB, allocBB);

        _builder.SetInsertPoint(allocBB);
        auto elemBytes = _builder.getInt64(_module->getDataLayout().getTypeAllocSize(elemLLVMType).getFixedValue());
        auto totalBytes = _builder.CreateMul(lenI64, elemBytes, "filter.bytes");
        auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
        auto newData = _builder.CreateCall(allocFn, {totalBytes}, "filter.data");
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "filter.i");
        auto outPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "filter.out");
        iPhi->addIncoming(_builder.getInt64(0), allocBB);
        outPhi->addIncoming(_builder.getInt64(0), allocBB);
        auto more = _builder.CreateICmpULT(iPhi, lenI64, "filter.more");
        _builder.CreateCondBr(more, bodyBB, finishBB);

        _builder.SetInsertPoint(bodyBB);
        auto srcPtr = _builder.CreateInBoundsGEP(elemLLVMType, oldData, {iPhi}, "filter.src.ptr");
        auto matched = _builder.CreateCall(predicateType, predicatePtr, {captures, srcPtr}, "filter.keep");
        _builder.CreateCondBr(matched, matchBB, skipBB);

        _builder.SetInsertPoint(matchBB);
        auto elemVal = copyElem(_builder.CreateLoad(elemLLVMType, srcPtr, "filter.elem"));
        auto dstPtr = _builder.CreateInBoundsGEP(elemLLVMType, newData, {outPhi}, "filter.dst.ptr");
        _builder.CreateStore(elemVal, dstPtr);
        auto outNext = _builder.CreateAdd(outPhi, _builder.getInt64(1), "filter.out.next");
        auto* matchExitBB = _builder.GetInsertBlock();
        _builder.CreateBr(latchBB);

        _builder.SetInsertPoint(skipBB);
        _builder.CreateBr(latchBB);

        _builder.SetInsertPoint(latchBB);
        auto outAfter = _builder.CreatePHI(_builder.getInt64Ty(), 2, "filter.out.after");
        outAfter->addIncoming(outNext, matchExitBB);
        outAfter->addIncoming(outPhi, skipBB);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "filter.i.next");
        iPhi->addIncoming(iNext, latchBB);
        outPhi->addIncoming(outAfter, latchBB);
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(finishBB);
        auto outLen = _builder.CreateZExtOrTrunc(outPhi, sizeTy, "filter.out.len");
        llvm::Value* newArr = llvm::UndefValue::get(arrayStructType);
        newArr = _builder.CreateInsertValue(newArr, newData, {0}, "filter.res.data");
        newArr = _builder.CreateInsertValue(newArr, outLen, {1}, "filter.res.len");
        newArr = _builder.CreateInsertValue(newArr, oldLen, {2}, "filter.res.cap");
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto result = _builder.CreatePHI(arrayStructType, 2, "filter.result");
        result->addIncoming(emptyArr, startBB);
        result->addIncoming(newArr, finishBB);
        releaseAtPtr(predicateSlot, argTypes[0]);
        return result;
    }

    case sema::BuiltinLower::ArrayMap: {
        DEBUG_LOG("    Expr: Array.map()");
        TypeInfo mappedElem;
        if (!callNode->getTypeArgs().empty()) {
            mappedElem = applySubst(callNode->getTypeArgs()[0]->getType());
        } else if (!argTypes.empty() && argTypes[0].isFn()) {
            if (auto ret = argTypes[0].fnReturnType()) mappedElem = ret->withoutFallible();
        }
        if (mappedElem.empty()) {
            throwSemaGap(callNode->getLineNumber(), callNode->getColumn());
        }
        TypeInfo mappedArrayType("Array", {make_shared<TypeInfo>(mappedElem)});
        auto mappedArrayLLVM = getLLVMType(mappedArrayType);
        auto mappedElemLLVM = getLLVMType(mappedElem);
        callNode->setResolvedType(mappedArrayType);

        auto ptr = getReadPtr();
        auto oldData = loadData(ptr);
        auto oldLen = loadLen(ptr);
        auto lenI64 = _builder.CreateZExtOrTrunc(oldLen, _builder.getInt64Ty(), "map.len.i64");
        auto transform = args[0];
        passAsArg(transform, argTypes[0], callNode->getArgs()[0]);
        auto transformSlot = _builder.CreateAlloca(getLLVMType(argTypes[0]), nullptr, "map.fn.arg");
        _builder.CreateStore(transform, transformSlot);
        auto transformPtr = _builder.CreateExtractValue(transform, {0}, "map.fn.ptr");
        auto captures = _builder.CreateExtractValue(transform, {1}, "map.fn.captures");
        auto transformType = llvm::FunctionType::get(mappedElemLLVM, {ptrTy, ptrTy}, false);

        llvm::Value* emptyArr = llvm::UndefValue::get(mappedArrayLLVM);
        emptyArr = _builder.CreateInsertValue(emptyArr, nullPtr, {0}, "map.empty.data");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {1}, "map.empty.len");
        emptyArr = _builder.CreateInsertValue(emptyArr, llvm::ConstantInt::get(sizeTy, 0), {2}, "map.empty.cap");

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* allocBB = llvm::BasicBlock::Create(_context, "map.alloc", fn);
        auto* hdrBB = llvm::BasicBlock::Create(_context, "map.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "map.body", fn);
        auto* latchBB = llvm::BasicBlock::Create(_context, "map.latch", fn);
        auto* finishBB = llvm::BasicBlock::Create(_context, "map.finish", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "map.done", fn);

        auto isEmpty = _builder.CreateICmpEQ(oldLen, llvm::ConstantInt::get(sizeTy, 0), "map.is_empty");
        _builder.CreateCondBr(isEmpty, doneBB, allocBB);

        _builder.SetInsertPoint(allocBB);
        auto elemBytes = _builder.getInt64(_module->getDataLayout().getTypeAllocSize(mappedElemLLVM).getFixedValue());
        auto totalBytes = _builder.CreateMul(lenI64, elemBytes, "map.bytes");
        auto allocFn = runtime::getHeapHandleAllocFn(_module, _builder);
        auto newData = _builder.CreateCall(allocFn, {totalBytes}, "map.data");
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "map.i");
        iPhi->addIncoming(_builder.getInt64(0), allocBB);
        auto more = _builder.CreateICmpULT(iPhi, lenI64, "map.more");
        _builder.CreateCondBr(more, bodyBB, finishBB);

        _builder.SetInsertPoint(bodyBB);
        auto srcPtr = _builder.CreateInBoundsGEP(elemLLVMType, oldData, {iPhi}, "map.src.ptr");
        auto mapped = _builder.CreateCall(transformType, transformPtr, {captures, srcPtr}, "map.elem");
        auto dstPtr = _builder.CreateInBoundsGEP(mappedElemLLVM, newData, {iPhi}, "map.dst.ptr");
        _builder.CreateStore(mapped, dstPtr);
        _builder.CreateBr(latchBB);

        _builder.SetInsertPoint(latchBB);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "map.i.next");
        iPhi->addIncoming(iNext, latchBB);
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(finishBB);
        llvm::Value* newArr = llvm::UndefValue::get(mappedArrayLLVM);
        newArr = _builder.CreateInsertValue(newArr, newData, {0}, "map.res.data");
        newArr = _builder.CreateInsertValue(newArr, oldLen, {1}, "map.res.len");
        newArr = _builder.CreateInsertValue(newArr, oldLen, {2}, "map.res.cap");
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto result = _builder.CreatePHI(mappedArrayLLVM, 2, "map.result");
        result->addIncoming(emptyArr, startBB);
        result->addIncoming(newArr, finishBB);
        releaseAtPtr(transformSlot, argTypes[0]);
        return result;
    }

    case sema::BuiltinLower::ArrayContains: {
        DEBUG_LOG("    Expr: Array.contains()");
        auto ptr = getReadPtr();
        auto dataPtr = loadData(ptr);
        auto lenVal = loadLen(ptr);
        llvm::Value* needle = loadNeedle();

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* hdrBB = llvm::BasicBlock::Create(_context, "contains.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "contains.body", fn);
        auto* latchBB = llvm::BasicBlock::Create(_context, "contains.latch", fn);
        auto* foundBB = llvm::BasicBlock::Create(_context, "contains.found", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "contains.done", fn);

        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "contains.len.i64");
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "contains.i");
        iPhi->addIncoming(_builder.getInt64(0), startBB);
        auto more = _builder.CreateICmpULT(iPhi, lenI64, "contains.more");
        _builder.CreateCondBr(more, bodyBB, doneBB);

        _builder.SetInsertPoint(bodyBB);
        auto elemPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {iPhi}, "contains.elem.ptr");
        llvm::Value* elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "contains.elem");
        auto eqv = elemEq(elemVal, needle, "contains");
        _builder.CreateCondBr(eqv, foundBB, latchBB);

        _builder.SetInsertPoint(latchBB);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "contains.i.next");
        iPhi->addIncoming(iNext, latchBB);
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(foundBB);
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto resultPhi = _builder.CreatePHI(_builder.getInt1Ty(), 2, "contains.result");
        resultPhi->addIncoming(llvm::ConstantInt::getFalse(_context), hdrBB);
        resultPhi->addIncoming(llvm::ConstantInt::getTrue(_context), foundBB);
        return resultPhi;
    }

    case sema::BuiltinLower::ArrayIndexOf:
    case sema::BuiltinLower::ArrayLastIndexOf: {
        const bool last = spec->lower == sema::BuiltinLower::ArrayLastIndexOf;
        if (last) {
            DEBUG_LOG("    Expr: Array.last_index_of()");
        } else {
            DEBUG_LOG("    Expr: Array.index_of()");
        }
        auto ptr = getReadPtr();
        auto dataPtr = loadData(ptr);
        auto lenVal = loadLen(ptr);
        llvm::Value* needle = loadNeedle();
        TypeInfo usizeNullTy("Nullable", {make_shared<TypeInfo>("usize")});
        auto nty = getLLVMType(usizeNullTy);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        auto noneVal = wrapNullable(_builder.getInt1(false), zeroSize, nty);

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* hdrBB = llvm::BasicBlock::Create(_context, "idxof.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "idxof.body", fn);
        auto* latchBB = llvm::BasicBlock::Create(_context, "idxof.latch", fn);
        auto* foundBB = llvm::BasicBlock::Create(_context, "idxof.found", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "idxof.done", fn);

        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "idxof.len.i64");
        if (last) {
            auto empty = _builder.CreateICmpEQ(lenVal, zeroSize, "idxof.empty");
            auto* initBB = llvm::BasicBlock::Create(_context, "idxof.init", fn);
            _builder.CreateCondBr(empty, doneBB, initBB);
            _builder.SetInsertPoint(initBB);
            auto lastI = _builder.CreateSub(lenI64, _builder.getInt64(1), "idxof.last");
            _builder.CreateBr(hdrBB);

            _builder.SetInsertPoint(hdrBB);
            auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "idxof.i");
            iPhi->addIncoming(lastI, initBB);
            _builder.CreateBr(bodyBB);

            _builder.SetInsertPoint(bodyBB);
            auto elemPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {iPhi}, "idxof.elem.ptr");
            llvm::Value* elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "idxof.elem");
            auto eqv = elemEq(elemVal, needle, "idxof");
            _builder.CreateCondBr(eqv, foundBB, latchBB);

            _builder.SetInsertPoint(latchBB);
            auto atZero = _builder.CreateICmpEQ(iPhi, _builder.getInt64(0), "idxof.at0");
            auto iPrev = _builder.CreateSub(iPhi, _builder.getInt64(1), "idxof.i.prev");
            iPhi->addIncoming(iPrev, latchBB);
            _builder.CreateCondBr(atZero, doneBB, hdrBB);

            _builder.SetInsertPoint(foundBB);
            auto foundUsize = _builder.CreateZExtOrTrunc(iPhi, sizeTy, "idxof.found.usize");
            auto someVal = wrapNullable(_builder.getInt1(true), foundUsize, nty);
            _builder.CreateBr(doneBB);

            _builder.SetInsertPoint(doneBB);
            auto resultPhi = _builder.CreatePHI(nty, 3, "idxof.result");
            resultPhi->addIncoming(noneVal, startBB);
            resultPhi->addIncoming(noneVal, latchBB);
            resultPhi->addIncoming(someVal, foundBB);
            return resultPhi;
        }

        _builder.CreateBr(hdrBB);
        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "idxof.i");
        iPhi->addIncoming(_builder.getInt64(0), startBB);
        auto more = _builder.CreateICmpULT(iPhi, lenI64, "idxof.more");
        _builder.CreateCondBr(more, bodyBB, doneBB);

        _builder.SetInsertPoint(bodyBB);
        auto elemPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {iPhi}, "idxof.elem.ptr");
        llvm::Value* elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "idxof.elem");
        auto eqv = elemEq(elemVal, needle, "idxof");
        _builder.CreateCondBr(eqv, foundBB, latchBB);

        _builder.SetInsertPoint(latchBB);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "idxof.i.next");
        iPhi->addIncoming(iNext, latchBB);
        _builder.CreateBr(hdrBB);

        _builder.SetInsertPoint(foundBB);
        auto foundUsize = _builder.CreateZExtOrTrunc(iPhi, sizeTy, "idxof.found.usize");
        auto someVal = wrapNullable(_builder.getInt1(true), foundUsize, nty);
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto resultPhi = _builder.CreatePHI(nty, 2, "idxof.result");
        resultPhi->addIncoming(noneVal, hdrBB);
        resultPhi->addIncoming(someVal, foundBB);
        return resultPhi;
    }

    case sema::BuiltinLower::ArrayGetOrNull:
    case sema::BuiltinLower::ArrayFirstOrNull:
    case sema::BuiltinLower::ArrayLastOrNull: {
        DEBUG_LOG("    Expr: Array.*_or_null() → T?");
        auto ptr = getReadPtr();
        auto dataPtr = loadData(ptr);
        auto lenVal = loadLen(ptr);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        TypeInfo elemNullTy("Nullable", {elemType});
        auto nty = getLLVMType(elemNullTy);
        auto noneVal = wrapNullable(_builder.getInt1(false), llvm::Constant::getNullValue(elemLLVMType), nty);

        llvm::Value* miss = nullptr;
        llvm::Value* idx = nullptr;
        if (spec->lower == sema::BuiltinLower::ArrayGetOrNull) {
            idx = _builder.CreateZExtOrTrunc(args[0], sizeTy, "gon.i");
            miss = _builder.CreateICmpUGE(idx, lenVal, "gon.oob");
        } else if (spec->lower == sema::BuiltinLower::ArrayFirstOrNull) {
            idx = zeroSize;
            miss = _builder.CreateICmpEQ(lenVal, zeroSize, "fon.empty");
        } else {
            auto oneSize = llvm::ConstantInt::get(sizeTy, 1);
            idx = _builder.CreateSub(lenVal, oneSize, "lon.idx");
            miss = _builder.CreateICmpEQ(lenVal, zeroSize, "lon.empty");
        }

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* hitBB = llvm::BasicBlock::Create(_context, "ornull.hit", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "ornull.done", fn);
        _builder.CreateCondBr(miss, doneBB, hitBB);

        _builder.SetInsertPoint(hitBB);
        auto idxI64 = _builder.CreateZExtOrTrunc(idx, _builder.getInt64Ty(), "ornull.i64");
        auto elemPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {idxI64}, "ornull.ptr");
        llvm::Value* copied = copyElem(_builder.CreateLoad(elemLLVMType, elemPtr, "ornull.elem"));
        auto someVal = wrapNullable(_builder.getInt1(true), copied, nty);
        auto* hitExitBB = _builder.GetInsertBlock();
        _builder.CreateBr(doneBB);

        _builder.SetInsertPoint(doneBB);
        auto resultPhi = _builder.CreatePHI(nty, 2, "ornull.result");
        resultPhi->addIncoming(noneVal, startBB);
        resultPhi->addIncoming(someVal, hitExitBB);
        return resultPhi;
    }

    case sema::BuiltinLower::ArrayInsert: {
        DEBUG_LOG("    Expr: Array.insert()");
        auto lenFieldPtr = arrayLenFieldPtr(arrayPtr, "arr");
        auto capFieldPtr = arrayCapFieldPtr(arrayPtr, "arr");
        auto dataFieldPtr = arrayDataFieldPtr(arrayPtr, "arr");
        auto idx = _builder.CreateZExtOrTrunc(args[0], sizeTy, "ins.i");
        auto elemVal = args[1];
        if (elemType && callNode->getArgs().size() >= 2) {
            passAsArg(elemVal, *elemType, callNode->getArgs()[1]);
        }
        auto lenVal = _builder.CreateLoad(sizeTy, lenFieldPtr, "a.len");
        auto oob = _builder.CreateICmpUGT(idx, lenVal, "ins.oob");
        emitOobAbort(oob, "ins.ok");

        auto capVal = _builder.CreateLoad(sizeTy, capFieldPtr, "a.cap");
        auto needGrow = _builder.CreateICmpUGE(lenVal, capVal, "ins.need_grow");
        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* growBB = llvm::BasicBlock::Create(_context, "ins.grow", fn);
        auto* shiftBB = llvm::BasicBlock::Create(_context, "ins.shift", fn);
        _builder.CreateCondBr(needGrow, growBB, shiftBB);

        _builder.SetInsertPoint(growBB);
        auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);
        auto capIsZero = _builder.CreateICmpEQ(capVal, zeroSize, "ins.cap0");
        auto doubled = _builder.CreateMul(capVal, llvm::ConstantInt::get(sizeTy, 2), "ins.cap.dbl");
        auto newCap = _builder.CreateSelect(capIsZero, llvm::ConstantInt::get(sizeTy, 4), doubled, "ins.new.cap");
        auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
        auto elemSizeVal = llvm::ConstantInt::get(sizeTy, elemSize);
        auto newByteSize = _builder.CreateMul(newCap, elemSizeVal, "ins.new.bytes");
        auto oldData = _builder.CreateLoad(ptrTy, dataFieldPtr, "ins.old.data");
        auto allocFn = runtime::getRiurtAllocFn(_module, _builder);
        auto reallocFn = runtime::getRiurtReallocFn(_module, _builder);
        auto dataIsNull = _builder.CreateICmpEQ(oldData, nullPtr, "ins.data.null");
        auto* allocBB = llvm::BasicBlock::Create(_context, "ins.alloc", fn);
        auto* reallocBB = llvm::BasicBlock::Create(_context, "ins.realloc", fn);
        auto* growDoneBB = llvm::BasicBlock::Create(_context, "ins.grow_done", fn);
        _builder.CreateCondBr(dataIsNull, allocBB, reallocBB);
        _builder.SetInsertPoint(allocBB);
        auto alloced = _builder.CreateCall(allocFn, {newByteSize}, "ins.alloced");
        _builder.CreateBr(growDoneBB);
        _builder.SetInsertPoint(reallocBB);
        auto realloced = _builder.CreateCall(reallocFn, {oldData, newByteSize}, "ins.realloced");
        _builder.CreateBr(growDoneBB);
        _builder.SetInsertPoint(growDoneBB);
        auto dataPhi = _builder.CreatePHI(ptrTy, 2, "ins.new.data");
        dataPhi->addIncoming(alloced, allocBB);
        dataPhi->addIncoming(realloced, reallocBB);
        _builder.CreateStore(dataPhi, dataFieldPtr);
        _builder.CreateStore(newCap, capFieldPtr);
        _builder.CreateBr(shiftBB);

        _builder.SetInsertPoint(shiftBB);
        auto curData = _builder.CreateLoad(ptrTy, dataFieldPtr, "ins.data");
        auto idxI64 = _builder.CreateZExtOrTrunc(idx, _builder.getInt64Ty(), "ins.i64");
        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "ins.len.i64");
        auto* shdr = llvm::BasicBlock::Create(_context, "ins.sh.hdr", fn);
        auto* sbody = llvm::BasicBlock::Create(_context, "ins.sh.body", fn);
        auto* storeBB = llvm::BasicBlock::Create(_context, "ins.store", fn);
        _builder.CreateBr(shdr);
        _builder.SetInsertPoint(shdr);
        auto jPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "ins.j");
        jPhi->addIncoming(lenI64, shiftBB);
        auto needShift = _builder.CreateICmpUGT(jPhi, idxI64, "ins.need_shift");
        _builder.CreateCondBr(needShift, sbody, storeBB);
        _builder.SetInsertPoint(sbody);
        auto jPrev = _builder.CreateSub(jPhi, _builder.getInt64(1), "ins.j.prev");
        auto srcPtr = _builder.CreateInBoundsGEP(elemLLVMType, curData, {jPrev}, "ins.src");
        auto dstPtr = _builder.CreateInBoundsGEP(elemLLVMType, curData, {jPhi}, "ins.dst");
        _builder.CreateStore(_builder.CreateLoad(elemLLVMType, srcPtr, "ins.moved"), dstPtr);
        jPhi->addIncoming(jPrev, sbody);
        _builder.CreateBr(shdr);

        _builder.SetInsertPoint(storeBB);
        auto hole = _builder.CreateInBoundsGEP(elemLLVMType, curData, {idxI64}, "ins.hole");
        _builder.CreateStore(elemVal, hole);
        auto newLen = _builder.CreateAdd(lenVal, llvm::ConstantInt::get(sizeTy, 1), "ins.new.len");
        _builder.CreateStore(newLen, lenFieldPtr);
        return voidResult();
    }

    case sema::BuiltinLower::ArrayRemoveAt: {
        DEBUG_LOG("    Expr: Array.remove_at()");
        auto lenFieldPtr = arrayLenFieldPtr(arrayPtr, "arr");
        auto dataFieldPtr = arrayDataFieldPtr(arrayPtr, "arr");
        auto idx = _builder.CreateZExtOrTrunc(args[0], sizeTy, "rm.i");
        auto lenVal = _builder.CreateLoad(sizeTy, lenFieldPtr, "a.len");
        auto oob = _builder.CreateICmpUGE(idx, lenVal, "rm.oob");
        emitOobAbort(oob, "rm.ok");

        auto dataPtr = _builder.CreateLoad(ptrTy, dataFieldPtr, "rm.data");
        auto idxI64 = _builder.CreateZExtOrTrunc(idx, _builder.getInt64Ty(), "rm.i64");
        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "rm.len.i64");
        auto outPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {idxI64}, "rm.out.ptr");
        auto outVal = _builder.CreateLoad(elemLLVMType, outPtr, "rm.out");

        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* shiftStart = _builder.GetInsertBlock();
        auto* shdr = llvm::BasicBlock::Create(_context, "rm.sh.hdr", fn);
        auto* sbody = llvm::BasicBlock::Create(_context, "rm.sh.body", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "rm.done", fn);
        _builder.CreateBr(shdr);
        _builder.SetInsertPoint(shdr);
        auto jPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "rm.j");
        jPhi->addIncoming(idxI64, shiftStart);
        auto last = _builder.CreateSub(lenI64, _builder.getInt64(1), "rm.last");
        auto needShift = _builder.CreateICmpULT(jPhi, last, "rm.need_shift");
        _builder.CreateCondBr(needShift, sbody, doneBB);
        _builder.SetInsertPoint(sbody);
        auto jNext = _builder.CreateAdd(jPhi, _builder.getInt64(1), "rm.j.next");
        auto srcPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {jNext}, "rm.src");
        auto dstPtr = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {jPhi}, "rm.dst");
        _builder.CreateStore(_builder.CreateLoad(elemLLVMType, srcPtr, "rm.moved"), dstPtr);
        jPhi->addIncoming(jNext, sbody);
        _builder.CreateBr(shdr);

        _builder.SetInsertPoint(doneBB);
        _builder.CreateStore(last, lenFieldPtr);
        return outVal;
    }

    case sema::BuiltinLower::ArrayReverse: {
        DEBUG_LOG("    Expr: Array.reverse()");
        auto lenVal = _builder.CreateLoad(sizeTy, arrayLenFieldPtr(arrayPtr, "arr"), "a.len");
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayDataFieldPtr(arrayPtr, "arr"), "a.data");
        auto lenI64 = _builder.CreateZExtOrTrunc(lenVal, _builder.getInt64Ty(), "rev.len.i64");
        auto* fn = _builder.GetInsertBlock()->getParent();
        auto* startBB = _builder.GetInsertBlock();
        auto* hdrBB = llvm::BasicBlock::Create(_context, "rev.hdr", fn);
        auto* bodyBB = llvm::BasicBlock::Create(_context, "rev.body", fn);
        auto* doneBB = llvm::BasicBlock::Create(_context, "rev.done", fn);
        _builder.CreateBr(hdrBB);
        _builder.SetInsertPoint(hdrBB);
        auto iPhi = _builder.CreatePHI(_builder.getInt64Ty(), 2, "rev.i");
        iPhi->addIncoming(_builder.getInt64(0), startBB);
        auto twoI = _builder.CreateAdd(iPhi, iPhi, "rev.2i");
        auto cont = _builder.CreateICmpULT(twoI, lenI64, "rev.cont");
        _builder.CreateCondBr(cont, bodyBB, doneBB);
        _builder.SetInsertPoint(bodyBB);
        auto j = _builder.CreateSub(_builder.CreateSub(lenI64, _builder.getInt64(1), "rev.nm1"), iPhi, "rev.j");
        auto ip = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {iPhi}, "rev.ip");
        auto jp = _builder.CreateInBoundsGEP(elemLLVMType, dataPtr, {j}, "rev.jp");
        auto iv = _builder.CreateLoad(elemLLVMType, ip, "rev.iv");
        auto jv = _builder.CreateLoad(elemLLVMType, jp, "rev.jv");
        _builder.CreateStore(jv, ip);
        _builder.CreateStore(iv, jp);
        auto iNext = _builder.CreateAdd(iPhi, _builder.getInt64(1), "rev.i.next");
        iPhi->addIncoming(iNext, bodyBB);
        _builder.CreateBr(hdrBB);
        _builder.SetInsertPoint(doneBB);
        return voidResult();
    }

    case sema::BuiltinLower::None:
    case sema::BuiltinLower::ArrayWithCapacity:
    default:
        return nullptr;
    }
}

llvm::Value* Compiler::compileArrayWithCapacity(ExprPathCallNode* node) {
    auto* spec = sema::lookupStaticBuiltin("Array", "with_capacity");
    const size_t expectArity = spec ? static_cast<size_t>(spec->arity) : 1;
    const char* expectArg0 = spec && spec->arg0Type ? spec->arg0Type : "usize";
    int line = node->getLineNumber();
    int col = node->getColumn();
    const auto& lhsTArgs = node->lhsTypeArgs();
    if (lhsTArgs.size() != 1) {
        // E6011 由 SemaPass validateArrayWithCapacity 先抛。
        throwSemaGap(line, col);
    }
    if (node->args().size() != expectArity) {
        string got;
        for (size_t i = 0; i < node->args().size(); ++i) {
            if (i) got += ", ";
            got += node->args()[i]->getType().getFullName();
        }
        throwSemaGap(line, col);
    }

    TypeInfo elemType = lhsTArgs[0]->getType();
    TypeInfo arrType("Array", {make_shared<TypeInfo>(elemType)});
    TypeInfo expectTy(expectArg0);
    tryInferIntType(node->args()[0], expectTy);
    auto actualTy = node->args()[0]->getType();
    if (!actualTy.empty() && !(actualTy == expectTy)) {
        throwSemaGap(line, col);
    }

    auto capVal = compileExpr(node->args()[0]);
    auto sizeTy = getSizeType();
    capVal = _builder.CreateZExtOrTrunc(capVal, sizeTy, "with_cap.n");

    auto resultTy = getLLVMType(arrType);
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    auto zeroSize = llvm::ConstantInt::get(sizeTy, 0);

    auto* fn = _builder.GetInsertBlock()->getParent();
    auto* startBB = _builder.GetInsertBlock();
    auto* allocBB = llvm::BasicBlock::Create(_context, "with_cap.alloc", fn);
    auto* doneBB = llvm::BasicBlock::Create(_context, "with_cap.done", fn);

    llvm::Value* emptyArr = llvm::UndefValue::get(resultTy);
    emptyArr = _builder.CreateInsertValue(emptyArr, nullPtr, {0}, "with_cap.empty.data");
    emptyArr = _builder.CreateInsertValue(emptyArr, zeroSize, {1}, "with_cap.empty.len");
    emptyArr = _builder.CreateInsertValue(emptyArr, zeroSize, {2}, "with_cap.empty.cap");

    auto isZero = _builder.CreateICmpEQ(capVal, zeroSize, "with_cap.is_zero");
    _builder.CreateCondBr(isZero, doneBB, allocBB);

    _builder.SetInsertPoint(allocBB);
    auto elemLLVMType = getLLVMType(elemType);
    auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
    auto elemSizeVal = llvm::ConstantInt::get(sizeTy, elemSize);
    auto byteSize = _builder.CreateMul(capVal, elemSizeVal, "with_cap.bytes");
    auto allocFn = runtime::getRiurtAllocFn(_module, _builder);
    auto data = _builder.CreateCall(allocFn, {byteSize}, "with_cap.data");
    llvm::Value* filled = llvm::UndefValue::get(resultTy);
    filled = _builder.CreateInsertValue(filled, data, {0}, "with_cap.res.data");
    filled = _builder.CreateInsertValue(filled, zeroSize, {1}, "with_cap.res.len");
    filled = _builder.CreateInsertValue(filled, capVal, {2}, "with_cap.res.cap");
    _builder.CreateBr(doneBB);

    _builder.SetInsertPoint(doneBB);
    auto phi = _builder.CreatePHI(resultTy, 2, "with_cap.result");
    phi->addIncoming(emptyArr, startBB);
    phi->addIncoming(filled, allocBB);
    return phi;
}

llvm::Value* Compiler::compileBuiltinTypeMethodCall(ExprCallNode* callNode, ExprNode* baseExpr,
                                                    const TypeInfo& baseType, const string& member,
                                                    vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {

    // Heap<T> / Rc<T> 自动解引用：方法查找与类型判定用内层 T 而非 wrapper 类型
    TypeInfo lookupType = baseType;
    if (baseType.isHeap()) {
        auto heapElem = baseType.heapElementType();
        if (heapElem) lookupType = *heapElem;
    }
    if (baseType.isRc()) {
        auto rcElem = baseType.rcElementType();
        if (rcElem) lookupType = *rcElem;
    }

    // 编译 receiver 值：若 baseType 是 Heap/Rc，compileExpr 后自动 unwrap 到内层值
    // 同时处理 T& 自动 load（v0.16: [] 返回 Ref 时 compileExpr 返回指针）
    auto compileReceiver = [&]() -> llvm::Value* {
        auto val = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();
        if (srcType.isRef() && val->getType()->isPointerTy()) {
            auto inner = srcType.refElementType();
            if (inner) {
                val = _builder.CreateLoad(getLLVMType(*inner), val, "ref.load");
            }
        }
        if (baseType.isHeap()) {
            // Heap<T>: compileExpr 返回 T*，load 出 T 值
            return _builder.CreateLoad(getLLVMType(lookupType), val, "heap.val");
        }
        if (!baseType.isRc()) return val;
        // Rc<T> struct { ptr } → alloca → extract handle → +8 payload → load T 值
        auto rcStructType = getLLVMType(baseType);
        auto tmp = _builder.CreateAlloca(rcStructType, nullptr, "rc.unwrap");
        _builder.CreateStore(val, tmp);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(rcStructType, tmp, {zero, zero}, "rc.handle.field");
        auto ptrTy = llvm::PointerType::get(_context, 0);
        auto handle = _builder.CreateLoad(ptrTy, handleField, "rc.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        auto llvmTy = getLLVMType(lookupType);
        return _builder.CreateLoad(llvmTy, payload, "rc.payload.val");
    };

    // 处理 bit-cast：f64.to_bits()→u64 / f32.to_bits()→u32 / u64.as_f64()→f64 / u32.as_f32()→f32
    // alloca + store + bitcast ptr + load（CreateBitCast 只接受指针类型）
    if (member == "to_bits") {
        if (lookupType.name == "f64" || lookupType.name == "f32") {
            DEBUG_LOG_VAL("    Expr: BitCast", lookupType.name << ".to_bits()");
            auto baseVal = compileReceiver();
            auto srcLLVMTy = getLLVMType(lookupType);
            auto dstLLVMTy =
                lookupType.name == "f64" ? llvm::Type::getInt64Ty(_context) : llvm::Type::getInt32Ty(_context);
            auto alloca = _builder.CreateAlloca(srcLLVMTy, nullptr, "bitcast.tmp");
            _builder.CreateStore(baseVal, alloca);
            auto bitcastPtr = _builder.CreateBitCast(alloca, llvm::PointerType::get(_context, 0), "bitcast.ptr");
            return _builder.CreateLoad(dstLLVMTy, bitcastPtr, "bitcast.load");
        }
    }
    if (member == "as_f64" || member == "as_f32") {
        if ((member == "as_f64" && lookupType.name == "u64") || (member == "as_f32" && lookupType.name == "u32")) {
            DEBUG_LOG_VAL("    Expr: BitCast", lookupType.name << "." << member << "()");
            auto baseVal = compileReceiver();
            auto srcLLVMTy = getLLVMType(lookupType);
            auto dstLLVMTy = member == "as_f64" ? llvm::Type::getDoubleTy(_context) : llvm::Type::getFloatTy(_context);
            auto alloca = _builder.CreateAlloca(srcLLVMTy, nullptr, "bitcast.tmp");
            _builder.CreateStore(baseVal, alloca);
            auto bitcastPtr = _builder.CreateBitCast(alloca, llvm::PointerType::get(_context, 0), "bitcast.ptr");
            return _builder.CreateLoad(dstLLVMTy, bitcastPtr, "bitcast.load");
        }
    }

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        if (isBuiltinType(dstType)) {
            DEBUG_LOG_VAL("    Expr: CastCall (to_)", dstType);
            auto baseVal = compileReceiver();
            return createCast(baseVal, lookupType, TypeInfo(dstType));
        }
    }

    // 处理 #Builtin 运算符方法：直接生成 LLVM IR
    if (isBuiltinMethod(lookupType.name, member)) {
        // Phase 3.3.2.e: 操作符方法 arity + 类型域校验
        //   E6027 17 处二元 op arity != 1, E3070 inv-on-float 全部抠到 sema.
        sema::validateOperatorMethodCall(member, lookupType, args.size(), callNode->getLineNumber(),
                                         callNode->getColumn());

        auto baseVal = compileReceiver();
        bool isFloat = lookupType.isFloat();
        bool isUnsigned = lookupType.isUnsigned();

        // 一元先处理：无 args，不能 loadScalarArg(0)
        if (member == "neg") {
            DEBUG_LOG_VAL("    Expr: Builtin neg", baseType.name);
            if (isFloat) {
                return _builder.CreateFNeg(baseVal, "neg");
            }
            return _builder.CreateNeg(baseVal, "neg");
        }
        if (member == "inv") {
            DEBUG_LOG_VAL("    Expr: Builtin inv", baseType.name);
            return _builder.CreateNot(baseVal, "inv");
        }
        if (member == "not") {
            DEBUG_LOG_VAL("    Expr: Builtin not", baseType.name);
            return _builder.CreateNot(baseVal, "not");
        }

        // T& 实参自动 load：形参声明为 T& 时 args[0] 是指针，load 出值参与 LLVM 运算
        auto loadScalarArg = [&](size_t idx) -> llvm::Value* {
            if (idx < argTypes.size() && argTypes[idx].isRef()) {
                auto inner = argTypes[idx].refElementType();
                if (inner) {
                    return _builder.CreateLoad(getLLVMType(*inner), args[idx], "scalar.ref.load");
                }
            }
            return args[idx];
        };
        auto rhs = loadScalarArg(0);
        if (rhs->getType() != baseVal->getType() && rhs->getType()->isIntegerTy() &&
            baseVal->getType()->isIntegerTy()) {
            rhs = _builder.CreateIntCast(rhs, baseVal->getType(), !isUnsigned, "bit.arg.cast");
        }

        // 算术运算符
        if (member == "plus") {
            DEBUG_LOG_VAL("    Expr: Builtin plus", baseType.name);
            if (isFloat) {
                return _builder.CreateFAdd(baseVal, rhs, "add");
            }
            return _builder.CreateAdd(baseVal, rhs, "add");
        }
        if (member == "minus") {
            DEBUG_LOG_VAL("    Expr: Builtin minus", baseType.name);
            if (isFloat) {
                return _builder.CreateFSub(baseVal, rhs, "sub");
            }
            return _builder.CreateSub(baseVal, rhs, "sub");
        }
        if (member == "mul") {
            DEBUG_LOG_VAL("    Expr: Builtin mul", baseType.name);
            if (isFloat) {
                return _builder.CreateFMul(baseVal, rhs, "mul");
            }
            return _builder.CreateMul(baseVal, rhs, "mul");
        }
        if (member == "div") {
            DEBUG_LOG_VAL("    Expr: Builtin div", baseType.name);
            if (isFloat) {
                return _builder.CreateFDiv(baseVal, rhs, "div");
            }
            if (isUnsigned) {
                return _builder.CreateUDiv(baseVal, rhs, "div");
            }
            return _builder.CreateSDiv(baseVal, rhs, "div");
        }
        if (member == "mod") {
            DEBUG_LOG_VAL("    Expr: Builtin mod", baseType.name);
            if (isFloat) {
                return _builder.CreateFRem(baseVal, rhs, "mod");
            }
            if (isUnsigned) {
                return _builder.CreateURem(baseVal, rhs, "mod");
            }
            return _builder.CreateSRem(baseVal, rhs, "mod");
        }

        // 比较运算符
        if (member == "eq") {
            DEBUG_LOG_VAL("    Expr: Builtin eq", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOEQ(baseVal, rhs, "eq");
            }
            return _builder.CreateICmpEQ(baseVal, rhs, "eq");
        }
        if (member == "ne") {
            DEBUG_LOG_VAL("    Expr: Builtin ne", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpONE(baseVal, rhs, "ne");
            }
            return _builder.CreateICmpNE(baseVal, rhs, "ne");
        }
        if (member == "lt") {
            DEBUG_LOG_VAL("    Expr: Builtin lt", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOLT(baseVal, rhs, "lt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULT(baseVal, rhs, "lt");
            }
            return _builder.CreateICmpSLT(baseVal, rhs, "lt");
        }
        if (member == "le") {
            DEBUG_LOG_VAL("    Expr: Builtin le", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOLE(baseVal, rhs, "le");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULE(baseVal, rhs, "le");
            }
            return _builder.CreateICmpSLE(baseVal, rhs, "le");
        }
        if (member == "gt") {
            DEBUG_LOG_VAL("    Expr: Builtin gt", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOGT(baseVal, rhs, "gt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGT(baseVal, rhs, "gt");
            }
            return _builder.CreateICmpSGT(baseVal, rhs, "gt");
        }
        if (member == "ge") {
            DEBUG_LOG_VAL("    Expr: Builtin ge", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOGE(baseVal, rhs, "ge");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGE(baseVal, rhs, "ge");
            }
            return _builder.CreateICmpSGE(baseVal, rhs, "ge");
        }

        // 位运算符
        if (member == "and") {
            DEBUG_LOG_VAL("    Expr: Builtin and", baseType.name);
            return _builder.CreateAnd(baseVal, rhs, "and");
        }
        if (member == "or") {
            DEBUG_LOG_VAL("    Expr: Builtin or", baseType.name);
            return _builder.CreateOr(baseVal, rhs, "or");
        }
        if (member == "xor") {
            DEBUG_LOG_VAL("    Expr: Builtin xor", baseType.name);
            return _builder.CreateXor(baseVal, rhs, "xor");
        }
        if (member == "shl") {
            DEBUG_LOG_VAL("    Expr: Builtin shl", baseType.name);
            return _builder.CreateShl(baseVal, rhs, "shl");
        }
        if (member == "shr") {
            DEBUG_LOG_VAL("    Expr: Builtin shr", baseType.name);
            if (isUnsigned) {
                return _builder.CreateLShr(baseVal, rhs, "shr");
            }
            return _builder.CreateAShr(baseVal, rhs, "shr");
        }
    }

    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(lookupType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }

    string methodFullName = lookupType.name + "." + member;
    FnSymbolInfo* sdkMethodSymbol = nullptr;
    if (_riu && _riu->sdkFile()) {
        sdkMethodSymbol = _riu->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }

    if (sdkMethodSymbol) {
        DEBUG_LOG_VAL("    Expr: BuiltinTypeMethodCall (SDK)", methodFullName);

        auto baseVal = compileReceiver();

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(baseVal);
        for (auto& arg : args) {
            methodArgs.push_back(arg);
        }

        // SDK 平铺文件拆分后，方法实际所属模块通过 sdkMethodSymbol 获取（不再硬编码 riu.core）
        string ownerMod =
            sdkMethodSymbol->moduleName.empty() ? _riu->sdkFile()->moduleName() : sdkMethodSymbol->moduleName;
        bool methPriv = !member.empty() && member[0] == '_';
        string mangledName = mangleMethod(ownerMod, lookupType.name, member, argTypes, methPriv,
                                          sdkMethodSymbol->retType, sdkMethodSymbol->fallibleErrType);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(getLLVMType(lookupType));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = wrapFallibleRetType(sdkMethodSymbol->retType, sdkMethodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    // E6016 / E3095 由 SemaPass 方法分派先抛；此处防 IR 无 builtin 方法可降。
    throwSemaGap(callNode->getLineNumber(), callNode->getColumn());
}

llvm::Value* Compiler::compileStructMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                               const TypeInfo& actualType, const string& member,
                                               vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {

    if (actualType.hasGenericArgs()) {
        FileNode* owner = _file;
        auto baseDecl = names().lookupStruct(actualType, /*includeBuiltin=*/false, &owner);
        if (!owner) owner = _file;
        if (baseDecl && baseDecl->isGeneric()) {
            string effName = genericStruct(baseDecl, actualType.genericArgs, owner);
            auto& inst = _generic.structs()[effName];
            if (inst.baseImpl) {
                FnNode* chosen = nullptr;
                for (auto m : inst.baseImpl->methods()) {
                    if (m->header()->name().getText() != member) continue;
                    if (m->header()->params().size() != argTypes.size()) continue;
                    chosen = m;
                    break;
                }
                if (chosen) {
                    llvm::Value* basePtr = nullptr;
                    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                            auto varName = objLiteral->getValue().getText();
                            auto it = _localVarPtrs.find(varName);
                            if (it != _localVarPtrs.end()) basePtr = it->second;
                        }
                    }
                    if (!basePtr && isAddressableMethodReceiver(baseExpr)) {
                        basePtr = compileLvalueAddr(baseExpr);
                    }
                    if (!basePtr) {
                        auto baseVal = compileExpr(baseExpr);
                        // [] / get() / 调用返 T&：compileExpr 已是 T*，不能当 struct 值 store
                        if (baseExpr->getType().isRef()) {
                            basePtr = baseVal;
                        } else {
                            auto structType = _structTypes[effName];
                            auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
                            _builder.CreateStore(baseVal, alloca);
                            basePtr = alloca;
                        }
                    }

                    map<string, TypeInfo> subst = inst.substMap();
                    // 与 emitInstanceMethods / 非泛型路径一致：mangle 与 LLVM 签名用替换后的
                    // 形参类型，不用调用点实参（否则 K& 工厂被 mangle 成 `K`，T& 被调成 `i32`）。
                    vector<TypeInfo> formalTypes;
                    for (auto p : chosen->header()->params()) {
                        if (p->type()) {
                            formalTypes.push_back(p->type()->getType().substitute(subst));
                        }
                    }
                    TypeInfo genRetType;
                    if (chosen->header()->retType()) {
                        genRetType = chosen->header()->retType()->getType().substitute(subst);
                        genRetType = bindStructSelfType(genRetType, inst.baseDecl->name().getText(), effName);
                    }
                    string mFallibleErr;
                    if (chosen->header()->fallibleErrTypeNode()) {
                        mFallibleErr =
                            fallibleErrKey(chosen->header()->fallibleErrTypeNode()->getType().substitute(subst));
                    }

                    vector<llvm::Value*> methodArgs;
                    methodArgs.push_back(basePtr);
                    for (size_t i = 0; i < args.size(); ++i) {
                        TypeInfo at = i < argTypes.size() ? applySubst(argTypes[i]) : TypeInfo();
                        TypeInfo formal = i < formalTypes.size() ? formalTypes[i] : at;
                        ExprNode* argExpr = i < callNode->getArgs().size() ? callNode->getArgs()[i] : nullptr;
                        if (!formal.isRef() && argExpr) passAsArg(args[i], at, argExpr);
                        methodArgs.push_back(abiValueForParam(argExpr, args[i], formal));
                    }

                    auto fn = getMethodFunction(effName, member, formalTypes, genRetType, mFallibleErr,
                                                /*isStatic=*/false);
                    auto callResult = _builder.CreateCall(fn, methodArgs, genRetType.empty() ? "" : member + ".ret");
                    auto v = handleFallibleCallResult(callResult, mFallibleErr, genRetType, callNode);
                    // void / fallible-void 成功路径是 nullptr；compileMethodCall 用空指针表示未命中
                    return v ? v : llvm::UndefValue::get(_builder.getInt8Ty());
                }
            }
        }
    }

    string methodFullName = actualType.name + "." + member;

    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(actualType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }
    auto methodSymbol = names().lookupMethodWithParams(actualType, member, methodParamTypes);

    // 普通方法严格匹配失败后，尝试方法自身的泛型重载。方法模板不在
    // compileStructImpls 中直接发射，而是在这里取得类型实参后进入延迟单态化队列。
    FileNode* genericOwner = _file;
    vector<pair<FnNode*, FileNode*>> genericMethods;
    if (!methodSymbol) {
        FileNode* structOwner = _file;
        auto* structDecl = names().lookupStruct(actualType, /*includeBuiltin=*/false, &structOwner);
        if (structDecl && structOwner) {
            if (auto* impl = structOwner->getStructImpl(structDecl->name().getText())) {
                for (auto* method : impl->methods()) {
                    if (!method->header()->isGeneric() || method->header()->isStatic()) continue;
                    if (method->header()->name().getText() != member) continue;
                    if (method->header()->params().size() != argTypes.size()) continue;
                    genericMethods.emplace_back(method, structOwner);
                }
            }
        }
    }

    FnNode* genericMethod = nullptr;
    if (!genericMethods.empty()) {
        if (genericMethods.size() == 1) {
            genericMethod = genericMethods[0].first;
            genericOwner = genericMethods[0].second;
        } else {
            auto [best, owner] = sema::resolveBestGenericOverload(genericMethods, callNode, member, argTypes);
            genericMethod = best;
            genericOwner = owner;
        }
    }

    if (genericMethod) {
        const auto& typeParams = genericMethod->header()->typeParams();
        vector<TypeInfo> typeArgs;
        if (!callNode->getTypeArgs().empty()) {
            sema::validateGenericTypeArgsArity(member, typeParams.size(), callNode->getTypeArgs().size(),
                                               callNode->getLineNumber(), callNode->getColumn());
            for (auto& typeArg : callNode->getTypeArgs()) {
                typeArgs.push_back(applySubst(typeArg->getType()));
            }
        } else {
            sema::inferGenericFnTypeArgs(callNode, genericMethod, member, argTypes, typeArgs);
        }
        if (_riu) {
            sema::validateGenericTypeArgsSpecBound(&_riu->specRegistry(), &_riu->specImplChecker(), genericOwner,
                                                   genericMethod->header(), typeArgs, callNode->getLineNumber(),
                                                   callNode->getColumn());
        }

        map<string, TypeInfo> subst;
        for (size_t i = 0; i < typeParams.size(); ++i) {
            subst[typeParams[i]] = typeArgs[i];
        }
        vector<TypeInfo> formalTypes;
        for (auto* param : genericMethod->header()->params()) {
            if (param->type()) formalTypes.push_back(param->type()->getType().substitute(subst));
        }
        TypeInfo retType;
        if (genericMethod->header()->retType()) {
            retType = genericMethod->header()->retType()->getType().substitute(subst);
        }
        const string fallibleErr =
            genericMethod->header()->fallibleErrTypeNode()
                ? fallibleErrKey(genericMethod->header()->fallibleErrTypeNode()->getType().substitute(subst))
                : string();
        const string instanceKey =
            internGenericMethod(genericMethod, actualType.name, typeArgs, genericOwner, callNode->getLineNumber());
        const string& instanceName = _generic.fns()[instanceKey].mangledName;

        llvm::Value* basePtr = nullptr;
        if (auto* baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            if (auto* objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                auto it = _localVarPtrs.find(objLiteral->getValue().getText());
                if (it != _localVarPtrs.end()) basePtr = it->second;
            }
        }
        if (!basePtr && isAddressableMethodReceiver(baseExpr)) basePtr = compileLvalueAddr(baseExpr);
        if (!basePtr) {
            auto* baseVal = compileExpr(baseExpr);
            if (baseType.isHeap() || baseExpr->getType().isRef()) {
                basePtr = baseVal;
            } else {
                auto* structType = getLLVMType(actualType);
                basePtr = _builder.CreateAlloca(structType, nullptr, "method_tmp");
                _builder.CreateStore(baseVal, basePtr);
            }
        } else if (baseType.isHeap()) {
            basePtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), basePtr, "heap.ptr");
        } else if (baseType.isRc()) {
            auto* rcStructType = getLLVMType(baseType);
            auto* zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto* handleField = _builder.CreateGEP(rcStructType, basePtr, {zero, zero}, "rc.handle_field");
            auto* handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            basePtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(basePtr);
        for (size_t i = 0; i < args.size(); ++i) {
            TypeInfo actual = i < argTypes.size() ? applySubst(argTypes[i]) : TypeInfo();
            TypeInfo formal = i < formalTypes.size() ? formalTypes[i] : actual;
            ExprNode* argExpr = i < callNode->getArgs().size() ? callNode->getArgs()[i] : nullptr;
            if (!formal.isRef() && argExpr) passAsArg(args[i], actual, argExpr);
            methodArgs.push_back(abiValueForParam(argExpr, args[i], formal));
        }

        string ownerModule = genericOwner ? genericOwner->moduleName() : _file->moduleName();
        auto* fn = getMethodFunction(actualType.name, instanceName, formalTypes, retType, fallibleErr,
                                     /*isStatic=*/false, ownerModule);
        auto* callResult = _builder.CreateCall(fn, methodArgs, retType.empty() ? "" : member + ".ret");
        auto* value = handleFallibleCallResult(callResult, fallibleErr, retType, callNode);
        return value ? value : llvm::UndefValue::get(_builder.getInt8Ty());
    }

    if (methodSymbol) {
        DEBUG_LOG_VAL("    Expr: MethodCall", methodFullName);

        // E6007 (Phase 3.3.3.a): 跨可见性私有方法, 迁至 sema::validateStructMethodVisibility.
        sema::validateStructMethodVisibility(methodSymbol, _currentStructName, actualType.name, member,
                                             callNode->getLineNumber(), callNode->getColumn());

        llvm::Value* basePtr = nullptr;
        if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                auto varName = objLiteral->getValue().getText();
                auto it = _localVarPtrs.find(varName);
                if (it != _localVarPtrs.end()) {
                    basePtr = it->second;
                }
            }
        }
        if (!basePtr && isAddressableMethodReceiver(baseExpr)) {
            basePtr = compileLvalueAddr(baseExpr);
        }

        bool heapFromLocal = false;
        if (!basePtr) {
            auto baseVal = compileExpr(baseExpr);
            if (baseType.isHeap() || baseExpr->getType().isRef()) {
                // Heap<T>：compileExpr 返回 T*
                // [] / get() / 调用返 T&：compileExpr 已是 T*，直接当 Self&
                basePtr = baseVal;
            } else {
                auto structType = getLLVMType(actualType);
                auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
                _builder.CreateStore(baseVal, alloca);
                basePtr = alloca;
            }
        } else if (baseType.isHeap()) {
            heapFromLocal = true;
        }

        llvm::Value* dataPtr = basePtr;

        if (baseType.isHeap()) {
            // Heap<T>：若来自 _localVarPtrs（alloca slot T**），load 出 T*
            if (heapFromLocal) {
                dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtr, "heap.ptr");
            }
            // else: compileExpr 已返回 T*，无需额外 load
        } else if (baseType.isRc()) {
            // Rc 方法 receiver：load handle，payload = handle + 8
            auto rcStructType = getLLVMType(baseType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, basePtr, {zero, zero}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }

        // Rc<primitive> 方法调用：内置类型方法的 receiver 走 by-value ABI
        // （见 getMethodFunction line 291：isBuiltinType(structName) 时第 0 槽用
        // getLLVMType(structName)，对应 compileMethod line 672-678 把首参 alloca + store
        // 作为 `$`）。这里要把 payload load 出来按值传，否则与 callee 签名不一致：
        // - 单跑 b.to_string()：callee 把 ptr 当 i32 读 → 栈上残值；
        // - 与 42i32.to_string() 共存：fn 已被前者按 (i32)→T 声明，此处再按 (ptr)→T
        //   call 触发 LLVM "Calling a function with a bad signature!" assert。
        bool receiverByValue = isBuiltinType(actualType.name);
        llvm::Value* receiverArg = dataPtr;
        if (receiverByValue) {
            auto receiverTy = getLLVMType(actualType);
            receiverArg = _builder.CreateLoad(receiverTy, dataPtr, "rc.payload.val");
        }

        // 使用方法声明的形参类型（含 Ref<>）替代调用点实参类型，
        // 保证 T& 形参的 mangle 名 / LLVM 签名与定义侧一致。
        // 注意：methodSymbol->params 第 0 元素是 receiver 类型（如 String），
        // 真正的形参从下标 1 开始。
        auto& mparams = methodSymbol->params;

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(receiverArg);
        for (size_t i = 0; i < args.size(); ++i) {
            auto& at = argTypes[i];
            size_t mpi = i + 1; // 跳 receiver（mparams[0]）
            TypeInfo formal = mpi < mparams.size() ? mparams[mpi] : at;
            ExprNode* argExpr = i < callNode->getArgs().size() ? callNode->getArgs()[i] : nullptr;
            if (!formal.isRef() && argExpr) passAsArg(args[i], at, argExpr);
            methodArgs.push_back(abiValueForParam(argExpr, args[i], formal));
        }

        string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
        bool methPriv = !member.empty() && member[0] == '_';
        // 去掉 mparams[0]（receiver 类型），Mangler::method 已含 structName
        vector<TypeInfo> methodDeclaredParams(mparams.size() > 1 ? mparams.begin() + 1 : mparams.begin(),
                                              mparams.end());
        // 若 mparams 仅含 receiver（无参方法如 len()），methodDeclaredParams 为空
        if (mparams.size() <= 1) methodDeclaredParams.clear();
        string mangledName = mangleMethod(ownerMod, actualType.name, member, methodDeclaredParams, methPriv,
                                          methodSymbol->retType, methodSymbol->fallibleErrType);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            if (receiverByValue) {
                paramTypes.push_back(getLLVMType(actualType));
            } else {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            }
            for (size_t i = 1; i < mparams.size(); ++i) {
                auto& t = mparams[i];
                if (structParamUsesPointer(t)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto retType = wrapFallibleRetType(methodSymbol->retType, methodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        auto callResult = _builder.CreateCall(fn, methodArgs, methodSymbol->retType.empty() ? "" : member + ".ret");
        auto v = handleFallibleCallResult(callResult, methodSymbol->fallibleErrType, methodSymbol->retType, callNode);
        // void / fallible-void 成功路径是 nullptr；compileMethodCall 用空指针表示未命中
        return v ? v : llvm::UndefValue::get(_builder.getInt8Ty());
    }

    return nullptr;
}

// Phase 2d: Dyn<D> / Dyn<D&> 方法调用静态检查 (vtable 间接调用 codegen 推 Phase 3d).
//
// 流程:
//   1. 从 baseType (Dyn<D> / Dyn<D&>) 取 D, 在 draft 注册表按 _file 可见性解析.
//      解析失败 (理论上 Phase 2b/2c 已拦截) → 直接 throw E1131.
//   2. 在 D 的 signatures 中按 member 名查找; 失败 → 抛"未定义方法"风格诊断
//      (沿用 E6016 段位, type = baseType.getFullName(), 与 builtin 未定义方法一致).
//   3. arity 严格匹配 sig->params().size() 与 argTypes.size(); 不匹配 → E6012.
//   4. 参数类型按 D 签名 (不是具体实现签名) 逐位比对; 不匹配 → E3001 风格暂复用 E6015
//      (后续 4c 落 E3xxx 明确码; 此处先用通用 E6015 + hint, 保证 Phase 2d 闭环).
//   5. Phase 2d 不接 codegen: 命中合法调用统一抛 E6015 + hint「Phase 3d pending」.
//      Phase 3d 把第 5 步替换为 load vtable[i] + indirect call.
llvm::Value* Compiler::compileDynMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                            const string& member, vector<llvm::Value*>& args,
                                            vector<TypeInfo>& argTypes) {
    int line = callNode->getLineNumber();
    int col = callNode->getColumn();

    // 1. 取 D 名 (剥 Dyn<D&> 的内层 Ref); 解析为 draft decl.
    // E1131 (Phase 3.3.3.b): 迁至 sema::resolveDynCalleeSpec.
    const SpecRegistry* reg = (_riu && _file) ? &_riu->specRegistry() : nullptr;
    auto resolved = sema::resolveDynCalleeSpec(reg, _file, baseType, line, col);
    SpecDeclNode* specDecl = resolved.decl;
    const string& specQualified = resolved.qualified;

    // 2-4. sig 查找 / arity / 形参类型 抠到 sema (E6016 / E6012 / E6015).
    FnHeaderNode* sig = sema::resolveDynMethodSig(specDecl, specQualified, baseType, member, argTypes, line, col);

    // 5. Phase 3d: load fat_ptr.vtable → GEP slot[i+1] → load fn ptr → indirect call.
    //    receiver:
    //      - Dyn<D>  (owned)  : data + 8（跳过 Rc RC 头，与 compileStructMethodCall 的
    //                           Rc receiver 一致；layout 见 compileDynCtorExpr）
    //      - Dyn<D&> (借用)   : data 直接是实例指针（裸 ref）
    //    fn 签名按 D.sig 还原：(ptr receiver, P1, ..., Pn) -> R
    //    （对象安全确保 sig 不含 Self / 自身名，所以 D.sig 形参/返回类型与 U.impl 一致）

    // 找到方法在 D.signatures() 中的下标（vtable 槽 0 是 dtor，方法从 1 开始）
    size_t methodIdx = 0;
    for (size_t i = 0; i < specDecl->signatures().size(); ++i) {
        if (specDecl->signatures()[i] == sig) {
            methodIdx = i;
            break;
        }
    }

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto i32Ty = _builder.getInt32Ty();

    // 5.1 编译 baseExpr → 落到 alloca 以便 GEP 出 vtable / data 字段
    auto fatStructTy = getLLVMType(baseType); // { ptr, ptr }
    llvm::Value* fatAlloca = nullptr;
    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) fatAlloca = it->second;
        }
    }
    if (!fatAlloca) {
        auto baseVal = compileExpr(baseExpr);
        // v0.16: [] 返回 T&（指针）；若 base 是引用，先 load 出 struct 值再存到 alloca
        auto baseExprType = baseExpr->getType();
        if (baseExprType.isRef()) {
            auto inner = baseExprType.refElementType();
            if (inner) {
                baseVal = _builder.CreateLoad(fatStructTy, baseVal, "dyn.ref.load");
            }
        }
        fatAlloca = _builder.CreateAlloca(fatStructTy, nullptr, "dyn.tmp");
        _builder.CreateStore(baseVal, fatAlloca);
    }

    auto zero = llvm::ConstantInt::get(i32Ty, 0);
    auto one = llvm::ConstantInt::get(i32Ty, 1);
    auto vtableFieldPtr = _builder.CreateGEP(fatStructTy, fatAlloca, {zero, zero}, "dyn.vtable.field");
    auto vtablePtr = _builder.CreateLoad(ptrTy, vtableFieldPtr, "dyn.vtable.load");
    auto dataFieldPtr = _builder.CreateGEP(fatStructTy, fatAlloca, {zero, one}, "dyn.data.field");
    auto dataPtr = _builder.CreateLoad(ptrTy, dataFieldPtr, "dyn.data.load");

    // 5.2 GEP vtable[methodIdx + 1] → load fn ptr
    // vtable 是 i8* 数组，按 ptr 步长 GEP 即可
    auto slotIdx = llvm::ConstantInt::get(_builder.getInt64Ty(), static_cast<uint64_t>(methodIdx + 1));
    auto slotPtr = _builder.CreateGEP(ptrTy, vtablePtr, {slotIdx}, "dyn.slot.ptr");
    auto fnPtr = _builder.CreateLoad(ptrTy, slotPtr, "dyn.fn.ptr");

    // 5.3 receiver：owned → data + 8（跳 RC 头）；borrow → data 直接是实例指针
    llvm::Value* receiver = dataPtr;
    if (baseType.isDynOwned()) {
        receiver = _builder.CreateGEP(_builder.getInt8Ty(), dataPtr, {_builder.getInt64(8)}, "dyn.payload");
    }

    // 5.4 构建 indirect call 的 FunctionType（与 vtable 端 forward-declare 一致）
    std::vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy); // receiver
    for (auto& p : sig->params()) {
        if (!p || !p->type()) continue;
        auto pt = p->type()->getType();
        if (pt.isPtr() || pt.isRef()) {
            llvmParamTypes.push_back(ptrTy);
        } else {
            llvmParamTypes.push_back(getLLVMType(pt));
        }
    }
    llvm::Type* llvmRetType = _builder.getVoidTy();
    TypeInfo retType;
    if (sig->retType()) {
        retType = sig->retType()->getType();
        if (!retType.empty()) llvmRetType = getLLVMType(retType);
    }
    auto fnTy = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);

    // 5.5 组装实参并 indirect call
    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(receiver);
    for (size_t i = 0; i < args.size(); ++i) {
        TypeInfo formal = argTypes[i];
        if (i < sig->params().size() && sig->params()[i] && sig->params()[i]->type()) {
            formal = sig->params()[i]->type()->getType();
        }
        ExprNode* argExpr = i < callNode->getArgs().size() ? callNode->getArgs()[i] : nullptr;
        callArgs.push_back(abiValueForParam(argExpr, args[i], formal));
    }

    const char* callName = llvmRetType->isVoidTy() ? "" : "dyn.call";
    return _builder.CreateCall(fnTy, fnPtr, callArgs, callName);
}

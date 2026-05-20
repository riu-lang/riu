// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 方法调用编译：从 compiler_call.cpp 拆出 (P1 Phase 3)
// 覆盖 compileMethodCall + 数组 / 内置 / 结构体 / Dyn 子分发。

#include "../compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "sema/call_resolve.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <functional>

// ==================== 方法调用编译 ====================
// 编译方法调用表达式 (obj.method(args))
// 处理多种情况: 包别名调用、模块别名调用、内置类型方法、数组方法、结构体方法
llvm::Value* Compiler::compileMethodCall(
    p<ExprCallNode> callNode, p<ExprDotNode> dotNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    auto baseExpr = dotNode->baseExpr();
    auto member = dotNode->member();

    // 处理包别名调用 / 模块别名调用 (E6001-E6005 已迁至 sema::resolveModuleFnCall)
    if (auto modCall = sema::resolveModuleFnCall(_file, _yux, callNode, dotNode, argTypes);
        modCall.matched) {
        return compileKnownFunctionCall(callNode, modCall.fnName, args, argTypes, modCall.fnSym);
    }

    auto baseType = baseExpr->getType();
    // §12.4 / §6.4.4：若 baseExpr 类型是当前替换栈中的泛型形参 T，
    // 应用替换得到具体类型（T -> i32 / Counter / ...），后续按具体类型分发
    // 边界 (E1106) 已在调用点 compileGenericFunctionCall 校验过。
    baseType = applySubst(baseType);

    // Dyn<D> / Dyn<D&> 方法调用 (Phase 2d 静态检查 + Phase 3d vtable codegen)
    if (baseType.isDyn()) {
        return compileDynMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    // 处理内置类型方法
    if (isBuiltinType(baseType.name)) {
        return compileBuiltinTypeMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    TypeInfo actualType = baseType;

    // 处理指针类型方法
    if (baseType.isPtr()) {
        if (member == "to_int") {
            DEBUG_LOG("    Expr: PtrMethod - to_int");
            auto selfVal = compileExpr(baseExpr);
            return _builder.CreatePtrToInt(selfVal, _builder.getInt64Ty(), "ptr_to_int");
        }
    }

    // 处理数组方法
    if (baseType.isArrayGeneric()) {
        auto result = compileArrayMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
        if (result) return result;
    }

    // 处理 Rc 类型: 解包获取实际类型
    if (baseType.isRc()) {
        auto rcElemType = baseType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
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
                auto cName = Mangler::function(ownerMod, objName, argTypes, fnPriv);
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

llvm::Value* Compiler::compileArrayMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    auto elemType = baseType.arrayGenericElementType();
    auto arrayStructType = getLLVMType(baseType);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto i64Ty = _builder.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(_context, 0);

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
        auto outerStructDecl = _file->getStructDecl(outerActual.name);
        if (!outerStructDecl && _yux && _yux->sdkFile()) {
            outerStructDecl = _yux->sdkFile()->getStructDecl(outerActual.name);
        }
        if (outerPtr && outerStructDecl) {
            int fi = outerStructDecl->fieldIndex(dotBase->member());
            if (fi >= 0) {
                llvm::Value* dataPtr = outerPtr;
                if (outerType.isRc()) {
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

    // Phase 3.3.2.a: Array<T> 方法形态校验 (E3055/E6040-E6044)
    sema::validateArrayMethodCall(baseType, member, args.size(), arrayPtr != nullptr,
                                   callNode->getLineNumber(), callNode->getColumn());

    auto getReadPtr = [&]() -> llvm::Value* {
        if (arrayPtr) return arrayPtr;
        auto baseVal = compileExpr(baseExpr);
        auto tmp = _builder.CreateAlloca(arrayStructType, nullptr, "array_tmp");
        _builder.CreateStore(baseVal, tmp);
        return tmp;
    };

    if (member == "len") {
        DEBUG_LOG("    Expr: Array.len()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenField = arrayBlockLenPtr(handle);
        return _builder.CreateLoad(i64Ty, lenField, "array.len");
    }
    if (member == "cap") {
        DEBUG_LOG("    Expr: Array.cap()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto capField = arrayBlockCapPtr(handle);
        return _builder.CreateLoad(i64Ty, capField, "array.cap");
    }

    // E3055 已由 sema::validateArrayMethodCall 在函数顶部抛出 (顶部 helper 保证 elemType 非空)
    auto elemLLVMType = getLLVMType(*elemType);

    if (member == "is_empty") {
        DEBUG_LOG("    Expr: Array.is_empty()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenField = arrayBlockLenPtr(handle);
        auto lenVal = _builder.CreateLoad(i64Ty, lenField, "array.len");
        return _builder.CreateICmpEQ(lenVal, _builder.getInt64(0), "array.is_empty");
    }

    if (member == "at") {
        DEBUG_LOG("    Expr: Array.at()");
        // E6040 已由 sema::validateArrayMethodCall 保证 args.size() == 1
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {args[0]}, "at.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "at.elem");
    }

    if (member == "first") {
        DEBUG_LOG("    Expr: Array.first()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {_builder.getInt64(0)}, "first.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "first.elem");
    }

    if (member == "last") {
        DEBUG_LOG("    Expr: Array.last()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenVal = _builder.CreateLoad(i64Ty, arrayBlockLenPtr(handle), "array.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "last.idx");
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "last.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "last.elem");
    }

    if (member == "pop") {
        DEBUG_LOG("    Expr: Array.pop()");
        // E6041 已由 sema::validateArrayMethodCall 保证 arrayPtr != nullptr
        auto handle = loadArrayHandle(arrayPtr);
        auto lenFieldPtr = arrayBlockLenPtr(handle);
        auto lenVal = _builder.CreateLoad(i64Ty, lenFieldPtr, "a.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "pop.idx");

        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "pop.elem.ptr");
        auto elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "pop.elem");

        _builder.CreateStore(lastIdx, lenFieldPtr);
        return elemVal;
    }

    if (member == "push" || member == "set_len" || member == "clear") {
        // E6042 已由 sema::validateArrayMethodCall 保证 arrayPtr != nullptr
        auto handle = loadArrayHandle(arrayPtr);
        auto lenFieldPtr = arrayBlockLenPtr(handle);
        auto capFieldPtr = arrayBlockCapPtr(handle);

        auto voidResult = [&]() -> llvm::Value* {
            return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        };

        if (member == "clear") {
            DEBUG_LOG("    Expr: Array.clear()");
            _builder.CreateStore(_builder.getInt64(0), lenFieldPtr);
            return voidResult();
        }
        if (member == "set_len") {
            DEBUG_LOG("    Expr: Array.set_len()");
            // E6043 已由 sema::validateArrayMethodCall 保证 args.size() == 1
            _builder.CreateStore(args[0], lenFieldPtr);
            return voidResult();
        }
        DEBUG_LOG("    Expr: Array.push()");
        // E6044 已由 sema::validateArrayMethodCall 保证 args.size() == 1
        auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
        auto elemVal = args[0];
        auto lenVal = _builder.CreateLoad(i64Ty, lenFieldPtr, "a.len");
        auto capVal = _builder.CreateLoad(i64Ty, capFieldPtr, "a.cap");

        auto needGrow = _builder.CreateICmpUGE(lenVal, capVal, "push.need_grow");
        auto growBB = llvm::BasicBlock::Create(_context, "push.grow", _currentFn);
        auto storeBB = llvm::BasicBlock::Create(_context, "push.store", _currentFn);
        _builder.CreateCondBr(needGrow, growBB, storeBB);

        // 扩容路径：通过 _array_grow 在 Block 内原地更新 cap、data
        _builder.SetInsertPoint(growBB);
        auto capIsZero = _builder.CreateICmpEQ(capVal, _builder.getInt64(0), "cap.is_zero");
        auto doubled = _builder.CreateMul(capVal, _builder.getInt64(2), "cap.dbl");
        auto newCap = _builder.CreateSelect(capIsZero, _builder.getInt64(4), doubled, "new.cap");
        auto growFn = runtime::getArrayGrowFn(_module, _builder);
        _builder.CreateCall(growFn, {handle, _builder.getInt64(elemSize), newCap});
        _builder.CreateBr(storeBB);

        // 写入新元素并 len++
        _builder.SetInsertPoint(storeBB);
        auto curData = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data.cur");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, curData, {lenVal}, "push.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
        auto newLen = _builder.CreateAdd(lenVal, _builder.getInt64(1), "new.len");
        _builder.CreateStore(newLen, lenFieldPtr);
        return voidResult();
    }

    return nullptr;
}

llvm::Value* Compiler::compileBuiltinTypeMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        if (isBuiltinType(dstType)) {
            DEBUG_LOG_VAL("    Expr: CastCall (to_)", dstType);
            auto baseVal = compileExpr(baseExpr);
            auto srcType = baseExpr->getType();
            return createCast(baseVal, srcType, TypeInfo(dstType));
        }
    }
    
    // 处理 #CompilerInner 运算符方法：直接生成 LLVM IR
    if (isCompilerInnerMethod(baseType.name, member)) {
        // Phase 3.3.2.e: 操作符方法 arity + 类型域校验
        //   E6045 17 处二元 op arity != 1, E3070 inv-on-float 全部抠到 sema.
        sema::validateOperatorMethodCall(member, baseType, args.size(),
            callNode->getLineNumber(), callNode->getColumn());

        auto baseVal = compileExpr(baseExpr);
        bool isFloat = baseType.startsWith('f');
        bool isUnsigned = baseType.startsWith('u');

        // 算术运算符
        if (member == "plus") {
            DEBUG_LOG_VAL("    Expr: CompilerInner plus", baseType.name);
            if (isFloat) {
                return _builder.CreateFAdd(baseVal, args[0], "add");
            }
            return _builder.CreateAdd(baseVal, args[0], "add");
        }
        if (member == "minus") {
            DEBUG_LOG_VAL("    Expr: CompilerInner minus", baseType.name);
            if (isFloat) {
                return _builder.CreateFSub(baseVal, args[0], "sub");
            }
            return _builder.CreateSub(baseVal, args[0], "sub");
        }
        if (member == "mul") {
            DEBUG_LOG_VAL("    Expr: CompilerInner mul", baseType.name);
            if (isFloat) {
                return _builder.CreateFMul(baseVal, args[0], "mul");
            }
            return _builder.CreateMul(baseVal, args[0], "mul");
        }
        if (member == "div") {
            DEBUG_LOG_VAL("    Expr: CompilerInner div", baseType.name);
            if (isFloat) {
                return _builder.CreateFDiv(baseVal, args[0], "div");
            }
            if (isUnsigned) {
                return _builder.CreateUDiv(baseVal, args[0], "div");
            }
            return _builder.CreateSDiv(baseVal, args[0], "div");
        }
        if (member == "mod") {
            DEBUG_LOG_VAL("    Expr: CompilerInner mod", baseType.name);
            if (isFloat) {
                return _builder.CreateFRem(baseVal, args[0], "mod");
            }
            if (isUnsigned) {
                return _builder.CreateURem(baseVal, args[0], "mod");
            }
            return _builder.CreateSRem(baseVal, args[0], "mod");
        }

        // 比较运算符
        if (member == "eq") {
            DEBUG_LOG_VAL("    Expr: CompilerInner eq", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOEQ(baseVal, args[0], "eq");
            }
            return _builder.CreateICmpEQ(baseVal, args[0], "eq");
        }
        if (member == "ne") {
            DEBUG_LOG_VAL("    Expr: CompilerInner ne", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpONE(baseVal, args[0], "ne");
            }
            return _builder.CreateICmpNE(baseVal, args[0], "ne");
        }
        if (member == "lt") {
            DEBUG_LOG_VAL("    Expr: CompilerInner lt", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOLT(baseVal, args[0], "lt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULT(baseVal, args[0], "lt");
            }
            return _builder.CreateICmpSLT(baseVal, args[0], "lt");
        }
        if (member == "le") {
            DEBUG_LOG_VAL("    Expr: CompilerInner le", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOLE(baseVal, args[0], "le");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULE(baseVal, args[0], "le");
            }
            return _builder.CreateICmpSLE(baseVal, args[0], "le");
        }
        if (member == "gt") {
            DEBUG_LOG_VAL("    Expr: CompilerInner gt", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOGT(baseVal, args[0], "gt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGT(baseVal, args[0], "gt");
            }
            return _builder.CreateICmpSGT(baseVal, args[0], "gt");
        }
        if (member == "ge") {
            DEBUG_LOG_VAL("    Expr: CompilerInner ge", baseType.name);
            if (isFloat) {
                return _builder.CreateFCmpOGE(baseVal, args[0], "ge");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGE(baseVal, args[0], "ge");
            }
            return _builder.CreateICmpSGE(baseVal, args[0], "ge");
        }

        // 位运算符
        if (member == "and") {
            DEBUG_LOG_VAL("    Expr: CompilerInner and", baseType.name);
            return _builder.CreateAnd(baseVal, args[0], "and");
        }
        if (member == "or") {
            DEBUG_LOG_VAL("    Expr: CompilerInner or", baseType.name);
            return _builder.CreateOr(baseVal, args[0], "or");
        }
        if (member == "xor") {
            DEBUG_LOG_VAL("    Expr: CompilerInner xor", baseType.name);
            return _builder.CreateXor(baseVal, args[0], "xor");
        }
        if (member == "shl") {
            DEBUG_LOG_VAL("    Expr: CompilerInner shl", baseType.name);
            return _builder.CreateShl(baseVal, args[0], "shl");
        }
        if (member == "shr") {
            DEBUG_LOG_VAL("    Expr: CompilerInner shr", baseType.name);
            if (isUnsigned) {
                return _builder.CreateLShr(baseVal, args[0], "shr");
            }
            return _builder.CreateAShr(baseVal, args[0], "shr");
        }

        // 一元运算符
        if (member == "neg") {
            DEBUG_LOG_VAL("    Expr: CompilerInner neg", baseType.name);
            if (isFloat) {
                return _builder.CreateFNeg(baseVal, "neg");
            }
            return _builder.CreateNeg(baseVal, "neg");
        }
        if (member == "inv") {
            DEBUG_LOG_VAL("    Expr: CompilerInner inv", baseType.name);
            // E3070 (inv on float) 已由 sema::validateOperatorMethodCall 校验
            return _builder.CreateNot(baseVal, "inv");
        }
        if (member == "not") {
            DEBUG_LOG_VAL("    Expr: CompilerInner not", baseType.name);
            return _builder.CreateNot(baseVal, "not");
        }
    }
    
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(baseType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }
    
    string methodFullName = baseType.name + "." + member;
    FnSymbolInfo* sdkMethodSymbol = nullptr;
    if (_yux && _yux->sdkFile()) {
        sdkMethodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (sdkMethodSymbol) {
        DEBUG_LOG_VAL("    Expr: BuiltinTypeMethodCall (SDK)", methodFullName);
        
        auto baseVal = compileExpr(baseExpr);
        
        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(baseVal);
        for (auto& arg : args) {
            methodArgs.push_back(arg);
        }
        
        string ownerMod = _yux->sdkFile()->moduleName();
        bool methPriv = !member.empty() && member[0] == '_';
        string mangledName = Mangler::method(ownerMod, baseType.name, member, argTypes, methPriv);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(getLLVMType(baseType));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = wrapFallibleRetType(sdkMethodSymbol->retType, sdkMethodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6016, member, baseType.name);
}

llvm::Value* Compiler::compileStructMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType, const TypeInfo& actualType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    if (actualType.isGeneric()) {
        auto baseDecl = _file->getStructDecl(actualType.name);
        p<FileNode> owner = _file;
        if (!baseDecl && _yux && _yux->sdkFile()) {
            baseDecl = _yux->sdkFile()->getStructDecl(actualType.name);
            if (baseDecl) owner = _yux->sdkFile();
        }
        if (baseDecl && baseDecl->isGeneric()) {
            string effName = ensureStructInstance(baseDecl, actualType.genericArgs, owner);
            auto& inst = _structInstances[effName];
            if (inst.baseImpl) {
                p<FnNode> chosen = nullptr;
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
                    if (!basePtr) {
                        auto baseVal = compileExpr(baseExpr);
                        auto structType = _structTypes[effName];
                        auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
                        _builder.CreateStore(baseVal, alloca);
                        basePtr = alloca;
                    }

                    vector<llvm::Value*> methodArgs;
                    methodArgs.push_back(basePtr);
                    for (auto& a : args) methodArgs.push_back(a);

                    // 泛型实例方法：用消费方模块作为前缀（与 emit 端一致）
                    string ownerMod = inst.consumerModule;
                    bool methPriv = !member.empty() && member[0] == '_';
                    string mangledName = Mangler::method(ownerMod, effName, member, argTypes, methPriv);
                    auto fn = _module->getFunction(mangledName);
                    if (!fn) {
                        map<string, TypeInfo> subst;
                        for (size_t i = 0; i < inst.args.size(); ++i) {
                            subst[inst.baseDecl->typeParams()[i]] = inst.args[i];
                        }
                        vector<llvm::Type*> paramTypes;
                        paramTypes.push_back(llvm::PointerType::get(_context, 0));
                        for (auto& t : argTypes) {
                            if (structParamUsesPointer(t.name)) {
                                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                            } else {
                                paramTypes.push_back(getLLVMType(t));
                            }
                        }
                        TypeInfo retType;
                        if (chosen->header()->retType()) {
                            retType = chosen->header()->retType()->getType().substitute(subst);
                        }
                        string mFallibleErr;
                        if (auto e = chosen->header()->getAnnoArg("Fallible")) mFallibleErr = *e;
                        auto llvmRetType = wrapFallibleRetType(retType, mFallibleErr);
                        auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
                        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
                    }
                    return _builder.CreateCall(fn, methodArgs);
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
    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);

    if (methodSymbol) {
        DEBUG_LOG_VAL("    Expr: MethodCall", methodFullName);

        // E6007 (Phase 3.3.3.a): 跨可见性私有方法, 迁至 sema::validateStructMethodVisibility.
        sema::validateStructMethodVisibility(methodSymbol, _currentStructName,
                                              actualType.name, member,
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

        if (!basePtr) {
            auto baseVal = compileExpr(baseExpr);
            auto structType = getLLVMType(actualType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
            _builder.CreateStore(baseVal, alloca);
            basePtr = alloca;
        }

        llvm::Value* dataPtr = basePtr;

        if (baseType.isRc()) {
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

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(receiverArg);
        for (size_t i = 0; i < args.size(); ++i) {
            auto& at = argTypes[i];
            if (structParamUsesPointer(at.name)) {
                auto structType = getLLVMType(at);
                auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
                _builder.CreateStore(args[i], alloca);
                methodArgs.push_back(alloca);
            } else {
                methodArgs.push_back(args[i]);
            }
        }

        string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
        bool methPriv = !member.empty() && member[0] == '_';
        string mangledName = Mangler::method(ownerMod, actualType.name, member, argTypes, methPriv);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            if (receiverByValue) {
                paramTypes.push_back(getLLVMType(actualType));
            } else {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            }
            for (auto& t : argTypes) {
                if (structParamUsesPointer(t.name)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto retType = wrapFallibleRetType(methodSymbol->retType, methodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
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
llvm::Value* Compiler::compileDynMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    int line = callNode->getLineNumber();
    int col = callNode->getColumn();

    // 1. 取 D 名 (剥 Dyn<D&> 的内层 Ref); 解析为 draft decl.
    // E1131 (Phase 3.3.3.b): 迁至 sema::resolveDynCalleeSpec.
    const SpecRegistry* reg = (_yux && _file) ? &_yux->specRegistry() : nullptr;
    auto resolved = sema::resolveDynCalleeSpec(reg, _file, baseType, line, col);
    SpecDeclNode* specDecl = resolved.decl;
    const string& specQualified = resolved.qualified;

    // 2-4. sig 查找 / arity / 形参类型 抠到 sema (E6016 / E6012 / E6015).
    FnHeaderNode* sig = sema::resolveDynMethodSig(
        specDecl, specQualified, baseType, member, argTypes, line, col);

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
        if (specDecl->signatures()[i] == sig) { methodIdx = i; break; }
    }

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto i32Ty = _builder.getInt32Ty();

    // 5.1 编译 baseExpr → 落到 alloca 以便 GEP 出 vtable / data 字段
    auto fatStructTy = getLLVMType(baseType);  // { ptr, ptr }
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
        receiver = _builder.CreateGEP(_builder.getInt8Ty(), dataPtr,
                                       {_builder.getInt64(8)}, "dyn.payload");
    }

    // 5.4 构建 indirect call 的 FunctionType（与 vtable 端 forward-declare 一致）
    std::vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);  // receiver
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
        auto& at = argTypes[i];
        if (structParamUsesPointer(at.name)) {
            auto stTy = getLLVMType(at);
            auto alloca = _builder.CreateAlloca(stTy, nullptr, "dyn.arg.tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
        } else {
            callArgs.push_back(args[i]);
        }
    }

    const char* callName = llvmRetType->isVoidTy() ? "" : "dyn.call";
    return _builder.CreateCall(fnTy, fnPtr, callArgs, callName);
}

// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#include "compiler.h"
#include "node/fn_node.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "types.h"
#include <utility>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

llvm::Type* Compiler::getLLVMType(const TypeInfo& type) {
    if (type.isArray()) {
        if (type.elementType) {
            auto elementLLVMType = getLLVMType(*type.elementType);
            return llvm::ArrayType::get(elementLLVMType, type.arraySize);
        }
    }
    
    if (type.isRef()) {
        auto elemType = type.refElementType();
        if (elemType) {
            return llvm::PointerType::get(getLLVMType(*elemType), 0);
        }
        return llvm::PointerType::get(_builder.getInt8Ty(), 0);
    }
    
    if (type.isGeneric()) {
        string mangledName = _file->getMangledName(type.getFullName());
        auto it = _structTypes.find(type.getFullName());
        if (it != _structTypes.end()) {
            return it->second;
        }
        auto structIt = _structTypes.find(mangledName);
        if (structIt != _structTypes.end()) {
            return structIt->second;
        }
        return llvm::PointerType::get(_builder.getInt8Ty(), 0);
    }
    
    auto it = _structTypes.find(type.name);
    if (it != _structTypes.end()) {
        return it->second;
    }
    
    return _typeMap[type.name];
}

llvm::StructType* Compiler::getOrCreateStructType(p<StructDeclNode> structDecl) {
    string name = structDecl->name()->getText();
    string mangledName = _file->getMangledName(name);
    
    auto it = _structTypes.find(name);
    if (it != _structTypes.end()) {
        return it->second;
    }
    
    vector<llvm::Type*> fieldTypes;
    for (auto field : structDecl->fields()) {
        fieldTypes.push_back(getLLVMType(field->getType()));
    }
    
    auto structType = llvm::StructType::create(_context, fieldTypes, mangledName);
    _structTypes[name] = structType;
    
    DEBUG_LOG_VAL("Created struct type", mangledName);
    return structType;
}

llvm::FunctionType* Compiler::getLLVMFunctionType(p<FnHeaderNode> header) {
    vector<llvm::Type*> paramTypes;
    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        paramTypes.push_back(getLLVMType(paramType));
    }
    auto retType = header->retType();
    TypeInfo retTypeInfo = retType ? retType->getType() : TypeInfo();
    auto fnName = header->name()->getText();
    if (fnName == "main" && retTypeInfo.name.empty()) {
        return llvm::FunctionType::get(_builder.getInt32Ty(), paramTypes, false);
    }
    return llvm::FunctionType::get(getLLVMType(retTypeInfo), paramTypes, false);
}

Compiler::Compiler(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file, bool isSdk) :
    _context(context), _builder(builder), _module(mod), _file(file), _isSdk(isSdk) {
    _typeMap.insert({"", _builder.getVoidTy()});
    _typeMap.insert({"bool", _builder.getInt1Ty()});
    _typeMap.insert({"i8", _builder.getInt8Ty()});
    _typeMap.insert({"u8", _builder.getInt8Ty()});
    _typeMap.insert({"i16", _builder.getInt16Ty()});
    _typeMap.insert({"u16", _builder.getInt16Ty()});
    _typeMap.insert({"i32", _builder.getInt32Ty()});
    _typeMap.insert({"u32", _builder.getInt32Ty()});
    _typeMap.insert({"i64", _builder.getInt64Ty()});
    _typeMap.insert({"u64", _builder.getInt64Ty()});
    _typeMap.insert({"f32", _builder.getFloatTy()});
    _typeMap.insert({"f64", _builder.getDoubleTy()});
}

llvm::Function* Compiler::getFunction(p<FnHeaderNode> header) {
    auto name = header->name()->getText();
    if (name == "main") {
        name = "yux_main";
    } else if (name == "_stdout_write") {
        return getStdoutWriteFn();
    } else {
        vector<TypeInfo> paramTypes;
        for (auto param : header->params()) {
            if (param->type()) {
                paramTypes.push_back(param->type()->getType());
            }
        }
        name = _file->getMangledName(name, paramTypes);
    }
    
    auto fnType = getLLVMFunctionType(header);
    auto func = _module->getFunction(name);
    if (!func) {
        func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, _module);
    }
    return func;
}

llvm::Function* Compiler::getMethodFunction(const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes) {
    string mangledStructName = _file->getMangledName(structName);
    string mangledName = mangledStructName + "_" + methodName;
    
    auto func = _module->getFunction(mangledName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(llvm::PointerType::get(getLLVMType(TypeInfo(structName)), 0));
    
    for (auto& paramType : paramTypes) {
        llvmParamTypes.push_back(getLLVMType(paramType));
    }
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), llvmParamTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

llvm::Function* Compiler::getStdoutWriteFn() {
    string fnName = "_stdout_write";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
    paramTypes.push_back(_builder.getInt64Ty());
    paramTypes.push_back(_builder.getInt64Ty());
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

void Compiler::emitStdoutWrite() {
    auto func = _module->getFunction("_stdout_write");
    if (!func) {
        auto fnType = llvm::FunctionType::get(
            _builder.getVoidTy(),
            {
                llvm::PointerType::get(_builder.getInt8Ty(), 0),
                _builder.getInt64Ty(),
                _builder.getInt64Ty()
            },
            false
        );
        func = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "_stdout_write",
            _module
        );
    }
    
    if (!func->empty()) {
        return;
    }
    
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    
    auto args = func->args();
    auto argIt = args.begin();
    llvm::Value* bufPtr = &(*argIt);
    ++argIt;
    llvm::Value* offVal = &(*argIt);
    ++argIt;
    llvm::Value* lenVal = &(*argIt);
    
    auto getStdHandle = _module->getFunction("GetStdHandle");
    if (!getStdHandle) {
        auto getStdHandleType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {_builder.getInt32Ty()},
            false
        );
        getStdHandle = llvm::Function::Create(
            getStdHandleType,
            llvm::Function::ExternalLinkage,
            "GetStdHandle",
            _module
        );
    }
    
    auto writeFile = _module->getFunction("WriteFile");
    if (!writeFile) {
        auto writeFileType = llvm::FunctionType::get(
            _builder.getInt1Ty(),
            {
                llvm::PointerType::get(_builder.getInt8Ty(), 0),
                llvm::PointerType::get(_builder.getInt8Ty(), 0),
                _builder.getInt32Ty(),
                llvm::PointerType::get(_builder.getInt32Ty(), 0),
                llvm::PointerType::get(_builder.getInt8Ty(), 0)
            },
            false
        );
        writeFile = llvm::Function::Create(
            writeFileType,
            llvm::Function::ExternalLinkage,
            "WriteFile",
            _module
        );
    }
    
    auto STD_OUTPUT_HANDLE = llvm::ConstantInt::get(_builder.getInt32Ty(), -11, true);
    auto handle = _builder.CreateCall(getStdHandle, {STD_OUTPUT_HANDLE}, "stdout_handle");
    
    auto bufWithOff = _builder.CreateGEP(_builder.getInt8Ty(), bufPtr, offVal, "buf_with_off");
    auto len32 = _builder.CreateTrunc(lenVal, _builder.getInt32Ty(), "len32");
    
    auto writtenAlloca = _builder.CreateAlloca(_builder.getInt32Ty(), nullptr, "written");
    
    _builder.CreateCall(writeFile, {
        handle,
        bufWithOff,
        len32,
        writtenAlloca,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(_builder.getInt8Ty(), 0))
    });
    
    _builder.CreateRetVoid();
}

void Compiler::emitRuntimeHelpers() {
    auto fltused = new llvm::GlobalVariable(
        *_module,
        _builder.getInt32Ty(),
        true,
        llvm::GlobalValue::ExternalLinkage,
        _builder.getInt32(0),
        "_fltused"
    );
    
    auto chkstkFn = _module->getFunction("__chkstk");
    if (!chkstkFn) {
        auto chkstkType = llvm::FunctionType::get(
            _builder.getVoidTy(),
            {},
            false
        );
        chkstkFn = llvm::Function::Create(
            chkstkType,
            llvm::Function::ExternalLinkage,
            "__chkstk",
            _module
        );
        
        auto entry = llvm::BasicBlock::Create(_context, "entry", chkstkFn);
        _builder.SetInsertPoint(entry);
        _builder.CreateRetVoid();
    }
}

void Compiler::compile(p<FileNode> file) {
    compileStructDecls();
    compileStructImpls();
    
    if (_isSdk) {
        emitStdoutWrite();
        emitRuntimeHelpers();
    }
    
    auto functions = file->getFunctions();
    for (auto fn : functions) {
        auto func = getFunction(fn->header());
        compileFn(fn, func);
    }
}

void Compiler::compileStructDecls() {
    for (auto structDecl : _file->getStructDecls()) {
        getOrCreateStructType(structDecl);
    }
}

void Compiler::compileStructImpls() {
    for (auto structImpl : _file->getStructImpls()) {
        string structName = structImpl->structName();
        for (auto method : structImpl->methods()) {
            vector<TypeInfo> paramTypes;
            for (auto param : method->header()->params()) {
                if (param->type()) {
                    paramTypes.push_back(param->type()->getType());
                }
            }
            auto func = getMethodFunction(structName, method->header()->name()->getText(), paramTypes);
            compileMethod(method, func, structName);
        }
    }
}

void Compiler::compileFn(p<FnNode> node, llvm::Function* func) {
    _currentFn = func;
    _currentFnNode = node;
    _localVarPtrs.clear();

    DEBUG_LOG_VAL("Compiling function", node->header()->name()->getText());

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    DEBUG_LOG("Created entry basic block");

    for (auto& arg : func->args()) {
        auto paramName = node->header()->params()[arg.getArgNo()]->name()->getText();
        TypeInfo paramType = node->header()->params()[arg.getArgNo()]->type() 
            ? node->header()->params()[arg.getArgNo()]->type()->getType() 
            : TypeInfo();
        
        if (paramType.isRef()) {
            _localVarPtrs[paramName] = &arg;
            DEBUG_LOG_VAL("  Param (ref)", paramName << " : " << paramType.getFullName());
        } else {
            auto llvmType = getLLVMType(paramType);
            auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
            _builder.CreateStore(&arg, alloca);
            _localVarPtrs[paramName] = alloca;
            DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);
        }
    }

    for (auto s : node->body()) {
        compileStatement(s);
    }

    auto fnName = node->header()->name()->getText();
    if (fnName == "main" && func->getReturnType()->isIntegerTy(32)) {
        _builder.CreateRet(_builder.getInt32(0));
        DEBUG_LOG("  Added implicit return 0 for main");
    } else if (func->getReturnType()->isVoidTy()) {
        _builder.CreateRetVoid();
        DEBUG_LOG("  Added implicit void return");
    }
    DEBUG_LOG_VAL("Finished compiling function", node->header()->name()->getText());
}

void Compiler::compileMethod(p<FnNode> node, llvm::Function* func, const string& structName) {
    _currentFn = func;
    _currentFnNode = node;
    _localVarPtrs.clear();

    DEBUG_LOG_VAL("Compiling method", structName << "." << node->header()->name()->getText());

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    DEBUG_LOG("Created entry basic block");

    auto args = func->args();
    auto argIt = args.begin();
    
    if (argIt != args.end()) {
        string selfName = "self";
        _localVarPtrs[selfName] = &(*argIt);
        DEBUG_LOG_VAL("  Param (self)", selfName << " : " << structName << "*");
        ++argIt;
    }

    for (auto& param : node->header()->params()) {
        if (argIt == args.end()) break;
        
        auto paramName = param->name()->getText();
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        auto llvmType = getLLVMType(paramType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
        _builder.CreateStore(&(*argIt), alloca);
        _localVarPtrs[paramName] = alloca;

        DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);
        ++argIt;
    }

    for (auto s : node->body()) {
        compileStatement(s);
    }

    if (func->getReturnType()->isVoidTy()) {
        _builder.CreateRetVoid();
        DEBUG_LOG("  Added implicit void return");
    }
    DEBUG_LOG_VAL("Finished compiling method", structName << "." << node->header()->name()->getText());
}

void Compiler::compileRetStatement(p<StatementRetNode> node) {
    DEBUG_LOG("  Statement: Return");
    auto retType = node->expr()->getType();
    if (retType.empty()) {
        compileExpr(node->expr());
        DEBUG_LOG("    Expression compiled as void return");
    } else {
        auto retVal = compileExpr(node->expr());
        _builder.CreateRet(retVal);
        DEBUG_LOG("    Created return instruction");
    }
}

void Compiler::compileDeclareAssignStatement(p<StatementDeclareAssignNode> node) {
    auto expr = node->expr();
    auto varName = node->name()->getText();

    if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(expr)) {
        if (!node->varType()) {
            throw YuxError("Array fill expression requires array type annotation with size");
        }
        
        TypeInfo varType = node->varType()->getType();
        if (!varType.isArray()) {
            throw YuxError("Array fill expression requires array type annotation");
        }
        
        DEBUG_LOG_VAL("  Statement: Declare (ArrayFill)", varName << " : " << varType.name);
        
        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _localVarPtrs[varName] = alloca;
        
        auto exprVal = compileArrayInitExpr(arrayInitNode, varType);
        _builder.CreateStore(exprVal, alloca);
    } else {
        TypeInfo varType;
        if (node->varType()) {
            varType = node->varType()->getType();
        } else {
            varType = expr->getType();
        }

        DEBUG_LOG_VAL("  Statement: Declare", varName << " : " << varType.name);

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _localVarPtrs[varName] = alloca;
        
        auto exprVal = compileExpr(expr);
        auto exprType = expr->getType();
        
        if (varType.isArray() && exprType.isArray()) {
            if (varType.arraySize != exprType.arraySize) {
                throw YuxError("Array size mismatch: expected {}, got {}", varType.arraySize, exprType.arraySize);
            }
            if (varType.elementType && exprType.elementType) {
                if (*varType.elementType != *exprType.elementType) {
                    throw YuxError("Array element type mismatch: expected {}, got {}", varType.elementType->name, exprType.elementType->name);
                }
            }
        }
        
        _builder.CreateStore(exprVal, alloca);
    }
}

void Compiler::compileAssignStatement(p<StatementAssignNode> node) {
    auto objName = node->obj()->getText();
    auto expr = node->expr();
    auto exprVal = compileExpr(expr);
    auto& subs = node->subs();
    
    if (subs.empty()) {
        auto sym = _currentFnNode->lookupSymbol(objName);
        if (!sym) {
            throw YuxError("Undefined variable: {}", objName);
        }

        if (!sym->writeable) {
            throw YuxError("Cannot assign to immutable variable: {}", objName);
        }

        DEBUG_LOG_VAL("  Statement: Assign", objName << " : " << sym->type.name);

        auto exprType = expr->getType();
        auto valToStore = createCast(exprVal, exprType, sym->type);
        _builder.CreateStore(valToStore, _localVarPtrs[objName]);
    } else {
        auto sym = _currentFnNode->lookupSymbol(objName);
        if (!sym) {
            throw YuxError("Undefined variable: {}", objName);
        }
        
        TypeInfo actualType = sym->type;
        if (sym->type.isRef()) {
            auto refElemType = sym->type.refElementType();
            if (refElemType) {
                actualType = *refElemType;
            }
        }
        
        auto structDecl = _file->getStructDecl(actualType.name);
        if (!structDecl) {
            throw YuxError("Cannot access member on non-struct type: {}", actualType.name);
        }
        
        DEBUG_LOG_VAL("  Statement: MemberAssign", objName << "." << subs[0]->getText());
        
        auto it = _localVarPtrs.find(objName);
        if (it == _localVarPtrs.end()) {
            throw YuxError("Variable not found: {}", objName);
        }
        
        llvm::Value* structPtr = it->second;
        auto structType = getLLVMType(actualType);
        
        for (size_t i = 0; i < subs.size(); ++i) {
            auto memberName = subs[i]->getText();
            int fieldIndex = structDecl->fieldIndex(memberName);
            if (fieldIndex < 0) {
                throw YuxError("Struct {} has no field: {}", actualType.name, memberName);
            }
            
            auto field = structDecl->fields()[fieldIndex];
            
            if (i == subs.size() - 1) {
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
                llvm::Value* indices[] = {zero, idx};
                
                auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "struct.field");
                auto exprType = expr->getType();
                auto valToStore = createCast(exprVal, exprType, field->getType());
                _builder.CreateStore(valToStore, fieldPtr);
            } else {
                throw YuxError("Nested member access not yet supported");
            }
        }
    }
}

void Compiler::compileLoopStatement(p<StatementLoopNode> node) {
    DEBUG_LOG("  Statement: Loop");
    
    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(_context, "loop.cond");
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(_context, "loop.body");
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(_context, "loop.exit");
    
    _builder.CreateBr(condBB);
    
    func->insert(func->end(), condBB);
    _builder.SetInsertPoint(condBB);
    _builder.CreateBr(bodyBB);
    
    func->insert(func->end(), bodyBB);
    _builder.SetInsertPoint(bodyBB);
    
    _loopExitBlocks.push_back(exitBB);
    
    for (auto& stmt : node->block()->statements()) {
        compileStatement(stmt);
    }
    
    if (node->block()->hasResult()) {
        compileExpr(node->block()->resultExpr());
    }
    
    _loopExitBlocks.pop_back();
    
    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(condBB);
    }
    
    func->insert(func->end(), exitBB);
    _builder.SetInsertPoint(exitBB);
}

void Compiler::compileBreakStatement(p<StatementBreakNode> node) {
    DEBUG_LOG("  Statement: Break");
    
    if (_loopExitBlocks.empty()) {
        throw YuxError("break statement not within a loop");
    }
    
    llvm::BasicBlock* exitBB = _loopExitBlocks.back();
    _builder.CreateBr(exitBB);
    
    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* unreachableBB = llvm::BasicBlock::Create(_context, "unreachable", func);
    _builder.SetInsertPoint(unreachableBB);
}

void Compiler::compileArraySetStatement(p<StatementSetNode> node) {
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    if (!arrayType.isArray()) {
        throw YuxError("Cannot index non-array type: {}", arrayType.name);
    }

    auto& indices = node->indices();
    if (indices.empty()) {
        throw YuxError("Array assignment requires at least one index");
    }

    DEBUG_LOG_VAL("  Statement: ArraySet", arrayType.name);

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue()->getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError("Array variable not found: {}", varName);
            }
            currentPtr = it->second;
        }
    }

    if (!currentPtr) {
        throw YuxError("Array assignment requires a variable");
    }

    for (auto& indexExpr : indices) {
        auto indexVal = compileExpr(indexExpr);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* gepIndices[] = {zero, indexVal};

        auto llvmArrayType = getLLVMType(currentType);
        currentPtr = _builder.CreateGEP(llvmArrayType, currentPtr, gepIndices, "array.element");

        if (currentType.elementType) {
            currentType = *currentType.elementType;
        }
    }

    auto valueVal = compileExpr(node->valueExpr());
    _builder.CreateStore(valueVal, currentPtr);
}

void Compiler::compileStatement(p<StatementNode> node) {
    if (auto retNode = dynamic_cast<StatementRetNode*>(node)) {
        compileRetStatement(retNode);
    }
    else if (auto declareNode = dynamic_cast<StatementDeclareAssignNode*>(node)) {
        compileDeclareAssignStatement(declareNode);
    }
    else if (auto assignNode = dynamic_cast<StatementAssignNode*>(node)) {
        compileAssignStatement(assignNode);
    }
    else if (auto exprNode = dynamic_cast<StatementExprNode*>(node)) {
        DEBUG_LOG("  Statement: Expression");
        compileExpr(exprNode->expr());
    }
    else if (auto loopNode = dynamic_cast<StatementLoopNode*>(node)) {
        compileLoopStatement(loopNode);
    }
    else if (auto breakNode = dynamic_cast<StatementBreakNode*>(node)) {
        compileBreakStatement(breakNode);
    }
    else if (auto setNode = dynamic_cast<StatementSetNode*>(node)) {
        compileArraySetStatement(setNode);
    }
}

llvm::Value* Compiler::compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType) {
    auto literal = node->value();
    auto literalType = literal->getType();
    auto text = literal->getValue()->getText();
    
    TypeInfo elementType;
    if (node->explicitType()) {
        elementType = node->explicitType()->getType();
        if (literalType != elementType) {
            throw YuxError("Array fill literal type mismatch: literal is {}, but explicit type is {}", 
                          literalType.name, elementType.name);
        }
    } else {
        elementType = literalType;
    }
    
    if (targetType.elementType && *targetType.elementType != elementType) {
        throw YuxError("Array fill element type mismatch: expected {}, got {}", 
                      targetType.elementType->name, elementType.name);
    }
    
    DEBUG_LOG_VAL("    Expr: ArrayInit", targetType.name);
    
    auto llvmArrayType = getLLVMType(targetType);
    auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.init");
    
    llvm::Value* fillValue;
    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        string numStr;
        for (char c : text) {
            if (isdigit(c) || c == '-') {
                numStr += c;
            } else {
                break;
            }
        }
        i64 numVal = stoll(numStr);
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), numVal, true);
    }
    else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        string numStr;
        for (char c : text) {
            if (isdigit(c) || c == '.' || c == '-') {
                numStr += c;
            } else {
                break;
            }
        }
        f64 numVal = stod(numStr);
        fillValue = llvm::ConstantFP::get(getLLVMType(elementType), numVal);
    }
    else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), boolVal ? 1 : 0, false);
    }
    else {
        throw YuxError("Unsupported literal type for array fill");
    }
    
    for (u64 i = 0; i < targetType.arraySize; ++i) {
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, index};
        auto elemPtr = _builder.CreateGEP(llvmArrayType, alloca, indices, "array.elem.ptr");
        _builder.CreateStore(fillValue, elemPtr);
    }
    
    return _builder.CreateLoad(llvmArrayType, alloca, "array.load");
}

llvm::Value* Compiler::createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType) {
    if (srcType == dstType) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType.name);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType.name << " -> " << dstType.name);

    auto dstLLVMType = getLLVMType(dstType);
    bool srcIsFloat = srcType.startsWith('f');
    bool dstIsFloat = dstType.startsWith('f');
    bool srcIsUnsigned = srcType.startsWith('u');
    bool dstIsUnsigned = dstType.startsWith('u');

    if (srcIsFloat && dstIsFloat) {
        if (srcType.name == "f64" && dstType.name == "f32") {
            DEBUG_LOG("      FPTrunc (f64 -> f32)");
            return _builder.CreateFPTrunc(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPExt (f32 -> f64)");
            return _builder.CreateFPExt(val, dstLLVMType);
        }
    }
    else if (!srcIsFloat && !dstIsFloat) {
        auto srcLLVMType = getLLVMType(srcType);
        if (dstLLVMType->getIntegerBitWidth() > srcLLVMType->getIntegerBitWidth()) {
            if (srcIsUnsigned) {
                DEBUG_LOG("      ZExt (unsigned int extension)");
                return _builder.CreateZExt(val, dstLLVMType);
            } else {
                DEBUG_LOG("      SExt (signed int extension)");
                return _builder.CreateSExt(val, dstLLVMType);
            }
        } else {
            DEBUG_LOG("      Trunc (int truncation)");
            return _builder.CreateTrunc(val, dstLLVMType);
        }
    }
    else if (!srcIsFloat && dstIsFloat) {
        if (srcIsUnsigned) {
            DEBUG_LOG("      UIToFP (unsigned int to float)");
            return _builder.CreateUIToFP(val, dstLLVMType);
        } else {
            DEBUG_LOG("      SIToFP (signed int to float)");
            return _builder.CreateSIToFP(val, dstLLVMType);
        }
    }
    else {
        if (dstIsUnsigned) {
            DEBUG_LOG("      FPToUI (float to unsigned int)");
            return _builder.CreateFPToUI(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPToSI (float to signed int)");
            return _builder.CreateFPToSI(val, dstLLVMType);
        }
    }
}

llvm::Value* Compiler::compileLiteralExpr(p<ExprLiteralNode> node) {
    auto literal = node->literal();
    auto type = literal->getType();
    auto text = literal->getValue()->getText();

    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        string numStr;
        for (char c : text) {
            if (isdigit(c) || c == '-') {
                numStr += c;
            } else {
                break;
            }
        }
        i64 numVal = stoll(numStr);
        DEBUG_LOG_VAL("    Expr: IntLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), numVal, true);
    }
    else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        string numStr;
        for (char c : text) {
            if (isdigit(c) || c == '.' || c == '-') {
                numStr += c;
            } else {
                break;
            }
        }
        f64 numVal = stod(numStr);
        DEBUG_LOG_VAL("    Expr: FloatLiteral", text << " : " << type.name);
        return llvm::ConstantFP::get(getLLVMType(type), numVal);
    }
    else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        DEBUG_LOG_VAL("    Expr: BoolLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), boolVal ? 1 : 0, false);
    }
    else if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal)) {
        auto varName = text;
        auto sym = _currentFnNode->lookupSymbol(varName);
        if (sym && _localVarPtrs.contains(varName)) {
            DEBUG_LOG_VAL("    Expr: VariableLoad", varName << " : " << sym->type.name);
            return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[varName]);
        }
        throw YuxError("Undefined variable: {}", varName);
    }
    throw YuxError("Unsupported literal type");
}

llvm::Value* Compiler::compileAddSubExpr(p<ExprAddSubNode> node) {
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    auto type = node->getType();
    bool isFloat = type.startsWith('f');

    string opStr = (node->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
    DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type.name);

    if (node->op() == ExprAddSubNode::Op::Add) {
        if (isFloat) {
            return _builder.CreateFAdd(left, right);
        }
        return _builder.CreateAdd(left, right);
    } else {
        if (isFloat) {
            return _builder.CreateFSub(left, right);
        }
        return _builder.CreateSub(left, right);
    }
}

llvm::Value* Compiler::compileMulDivModExpr(p<ExprMulDivModNode> node) {
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    auto type = node->getType();
    bool isFloat = type.startsWith('f');
    bool isUnsigned = type.startsWith('u');

    string opStr;
    switch (node->op()) {
        case ExprMulDivModNode::Op::Mul: opStr = "*"; break;
        case ExprMulDivModNode::Op::Div: opStr = "/"; break;
        case ExprMulDivModNode::Op::Mod: opStr = "%"; break;
    }
    DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type.name);

    switch (node->op()) {
        case ExprMulDivModNode::Op::Mul:
            if (isFloat) {
                return _builder.CreateFMul(left, right);
            }
            return _builder.CreateMul(left, right);
        case ExprMulDivModNode::Op::Div:
            if (isFloat) {
                return _builder.CreateFDiv(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateUDiv(left, right);
            }
            return _builder.CreateSDiv(left, right);
        case ExprMulDivModNode::Op::Mod:
            if (isFloat) {
                return _builder.CreateFRem(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateURem(left, right);
            }
            return _builder.CreateSRem(left, right);
    }
    throw YuxError("Unsupported mul/div/mod operation");
}

llvm::Value* Compiler::compileParenExpr(p<ExprParenNode> node) {
    DEBUG_LOG("    Expr: Paren");
    return compileExpr(node->expr());
}

llvm::Value* Compiler::compileCallExpr(p<ExprCallNode> node) {
    auto calleeExpr = node->getCalleeExpr();
    vector<llvm::Value*> args;
    vector<TypeInfo> argTypes;
    for (auto& arg : node->getArgs()) {
        args.push_back(compileExpr(arg));
        argTypes.push_back(arg->getType());
    }

    if (auto dotNode = dynamic_cast<ExprDotNode*>(calleeExpr)) {
        auto result = compileMethodCall(node, dotNode, args, argTypes);
        if (result) {
            return result;
        }
    }

    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            return compileFunctionCall(node, objLiteral->getValue()->getText(), args, argTypes);
        }
    }

    throw YuxError("Unsupported call expression");
}

llvm::Value* Compiler::compileMethodCall(p<ExprCallNode> callNode, p<ExprDotNode> dotNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    auto baseExpr = dotNode->baseExpr();
    auto member = dotNode->member();

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("    Expr: CastCall (to_)", dstType);
        auto baseVal = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();
        return createCast(baseVal, srcType, TypeInfo(dstType));
    }

    auto baseType = baseExpr->getType();
    string methodFullName = baseType.name + "." + member;
    
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(baseType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }
    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);

    if (methodSymbol) {
        DEBUG_LOG_VAL("    Expr: MethodCall", methodFullName);

        auto baseVal = compileExpr(baseExpr);

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(baseVal);
        for (auto& arg : args) {
            methodArgs.push_back(arg);
        }

        string mangledStructName = _file->getMangledName(baseType.name);
        string mangledName = mangledStructName + "_" + member;
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(getLLVMType(baseType));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue()->getText();
            auto fnSymbol = _file->lookupFnSymbolWithParams(objName, argTypes);
            if (fnSymbol) {
                auto cName = Node::getCName(objName, argTypes);
                DEBUG_LOG_VAL("    Expr: InnerFnCall (dot)", objName << " -> " << cName);
                auto fn = _module->getFunction(cName);
                if (!fn) {
                    vector<llvm::Type*> paramTypes;
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

llvm::Value* Compiler::compileFunctionCall(p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    if (_castFunctions.contains(fnName)) {
        DEBUG_LOG_VAL("    Expr: CastFunction", fnName);
        auto& castInfo = _castFunctions[fnName];
        return createCast(castInfo.value, castInfo.srcType, castInfo.dstType);
    }

    auto structDecl = _file->getStructDecl(fnName);
    if (structDecl) {
        auto result = compileConstructorCall(fnName, args, argTypes);
        if (result) {
            return result;
        }
    }

    auto fnSymbol = _file->lookupFnSymbolWithParams(fnName, argTypes);
    if (fnName == "_stdout_write" && !fnSymbol) {
        static FnSymbolInfo stdoutWriteFnSymbol{"_stdout_write", "", {}, TypeInfo()};
        fnSymbol = &stdoutWriteFnSymbol;
    }
    
    if (fnSymbol) {
        return compileKnownFunctionCall(callNode, fnName, args, argTypes, fnSymbol);
    }

    DEBUG_LOG_VAL("    Expr: ExternalFunctionCall", fnName);
    auto fn = _module->getFunction(fnName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (auto& arg : args) {
            paramTypes.push_back(arg->getType());
        }
        auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
    }
    return _builder.CreateCall(fn, args);
}

llvm::Value* Compiler::compileConstructorCall(const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    string ctorFullName = fnName + "." + fnName;
    
    vector<TypeInfo> ctorParamTypes;
    ctorParamTypes.push_back(TypeInfo(fnName));
    for (auto& t : argTypes) {
        ctorParamTypes.push_back(t);
    }
    auto ctorSymbol = _file->lookupFnSymbolWithParams(ctorFullName, ctorParamTypes);

    if (ctorSymbol) {
        DEBUG_LOG_VAL("    Expr: ConstructorCall", fnName);

        auto structType = getLLVMType(TypeInfo(fnName));
        auto alloca = _builder.CreateAlloca(structType, nullptr, fnName + "_tmp");

        vector<llvm::Value*> ctorArgs;
        ctorArgs.push_back(alloca);
        for (auto& arg : args) {
            ctorArgs.push_back(arg);
        }

        string mangledStructName = _file->getMangledName(fnName);
        string mangledName = mangledStructName + "_" + fnName;
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(llvm::PointerType::get(structType, 0));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = _builder.getVoidTy();
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        _builder.CreateCall(fn, ctorArgs);

        return _builder.CreateLoad(structType, alloca);
    }
    return nullptr;
}

llvm::Value* Compiler::compileKnownFunctionCall(p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes, FnSymbolInfo* fnSymbol) {
    string cName;
    if (fnName == "_stdout_write") {
        cName = "_stdout_write";
    } else if (fnName == "main") {
        cName = "yux_main";
    } else {
        cName = _file->getMangledName(fnName, fnSymbol->params);
    }

    DEBUG_LOG_VAL("    Expr: FunctionCall", fnName << " -> " << cName);
    auto fn = _module->getFunction(cName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        if (fnName == "_stdout_write") {
            paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
            paramTypes.push_back(_builder.getInt64Ty());
            paramTypes.push_back(_builder.getInt64Ty());
        } else {
            for (size_t i = 0; i < fnSymbol->params.size(); ++i) {
                paramTypes.push_back(getLLVMType(fnSymbol->params[i]));
            }
        }
        auto retType = fnSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(fnSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }
    
    if (fnName == "_stdout_write") {
        return compileStdoutWriteCall(fn, args, argTypes);
    }
    
    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size() && i < fnSymbol->params.size(); ++i) {
        if (fnSymbol->params[i].isRef()) {
            if (auto literalNode = dynamic_cast<ExprLiteralNode*>(callNode->getArgs()[i])) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                    auto varName = objLiteral->getValue()->getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        callArgs.push_back(it->second);
                        continue;
                    }
                }
            }
        }
        callArgs.push_back(args[i]);
    }
    
    return _builder.CreateCall(fn, callArgs);
}

llvm::Value* Compiler::compileStdoutWriteCall(llvm::Function* fn, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    DEBUG_LOG_VAL("    _stdout_write - args.size", args.size());
    DEBUG_LOG_VAL("    _stdout_write - fn->getNumParams", fn->getFunctionType()->getNumParams());
    for (size_t i = 0; i < args.size(); ++i) {
        DEBUG_LOG_VAL("    _stdout_write - args[" + to_string(i) + "] type", args[i]->getType()->getTypeID());
        if (i < fn->getFunctionType()->getNumParams()) {
            DEBUG_LOG_VAL("    _stdout_write - fn param[" + to_string(i) + "] type", fn->getFunctionType()->getParamType(i)->getTypeID());
        }
    }
    vector<llvm::Value*> stdoutArgs;
    if (args.size() >= 3) {
        auto arrayVal = args[0];
        auto arrayType = argTypes[0];
        DEBUG_LOG_VAL("    _stdout_write - isArray", arrayType.isArray());
        if (arrayType.isArray()) {
            auto alloca = _builder.CreateAlloca(getLLVMType(arrayType), nullptr, "stdout_buf");
            _builder.CreateStore(arrayVal, alloca);
            auto ptr = _builder.CreateBitCast(alloca, llvm::PointerType::get(_builder.getInt8Ty(), 0));
            stdoutArgs.push_back(ptr);
        } else {
            stdoutArgs.push_back(_builder.CreateBitCast(args[0], llvm::PointerType::get(_builder.getInt8Ty(), 0)));
        }
        stdoutArgs.push_back(args[1]);
        stdoutArgs.push_back(args[2]);
    }
    DEBUG_LOG_VAL("    _stdout_write - stdoutArgs.size", stdoutArgs.size());
    for (size_t i = 0; i < stdoutArgs.size(); ++i) {
        DEBUG_LOG_VAL("    _stdout_write - stdoutArgs[" + to_string(i) + "] type", stdoutArgs[i]->getType()->getTypeID());
    }
    return _builder.CreateCall(fn, stdoutArgs);
}

llvm::Value* Compiler::compileDotExpr(p<ExprDotNode> node) {
    auto baseExpr = node->baseExpr();
    auto member = node->member();

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("    Expr: DotCast", member);
        auto baseVal = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();

        string castFnName = "__cast_" + to_string(_castCounter++);
        _castFunctions[castFnName] = {baseVal, srcType, TypeInfo(dstType)};

        return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
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
    
    auto structDecl = _file->getStructDecl(actualType.name);

    if (structDecl) {
        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue()->getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        structPtr = it->second;
                    }
                }
            }

            if (!structPtr) {
                throw YuxError("Cannot access field on non-variable struct");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            llvm::Value* indices[] = {zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "struct.field");
            auto fieldType = structDecl->fields()[fieldIndex]->getType();

            return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "field.load");
        }
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue()->getText();
            auto sym = _currentFnNode->lookupSymbol(objName);
            if (sym && _localVarPtrs.contains(objName)) {
                DEBUG_LOG_VAL("    Expr: DotMemberLoad", objName << "." << member);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName]);
            }
        }
    }
    throw YuxError("Unsupported dot expression");
}

llvm::Value* Compiler::compileCompareExpr(p<ExprCompareNode> node) {
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    auto leftType = node->left()->getType();
    auto rightType = node->right()->getType();

    if (leftType != rightType) {
        throw YuxError("Type mismatch in comparison: left is {}, right is {}", leftType.name, rightType.name);
    }

    bool isFloat = leftType.startsWith('f');
    bool isUnsigned = leftType.startsWith('u');

    string opStr;
    switch (node->op()) {
        case ExprCompareNode::Op::Eq: opStr = "=="; break;
        case ExprCompareNode::Op::Ne: opStr = "!="; break;
        case ExprCompareNode::Op::Lt: opStr = "<"; break;
        case ExprCompareNode::Op::Le: opStr = "<="; break;
        case ExprCompareNode::Op::Gt: opStr = ">"; break;
        case ExprCompareNode::Op::Ge: opStr = ">="; break;
    }
    DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << leftType.name);

    switch (node->op()) {
        case ExprCompareNode::Op::Eq:
            if (isFloat) {
                return _builder.CreateFCmpOEQ(left, right);
            }
            return _builder.CreateICmpEQ(left, right);
        case ExprCompareNode::Op::Ne:
            if (isFloat) {
                return _builder.CreateFCmpONE(left, right);
            }
            return _builder.CreateICmpNE(left, right);
        case ExprCompareNode::Op::Lt:
            if (isFloat) {
                return _builder.CreateFCmpOLT(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateICmpULT(left, right);
            }
            return _builder.CreateICmpSLT(left, right);
        case ExprCompareNode::Op::Le:
            if (isFloat) {
                return _builder.CreateFCmpOLE(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateICmpULE(left, right);
            }
            return _builder.CreateICmpSLE(left, right);
        case ExprCompareNode::Op::Gt:
            if (isFloat) {
                return _builder.CreateFCmpOGT(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGT(left, right);
            }
            return _builder.CreateICmpSGT(left, right);
        case ExprCompareNode::Op::Ge:
            if (isFloat) {
                return _builder.CreateFCmpOGE(left, right);
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGE(left, right);
            }
            return _builder.CreateICmpSGE(left, right);
    }
    throw YuxError("Unsupported comparison operation");
}

llvm::Value* Compiler::compileIfElseExpr(p<ExprIfElseNode> node) {
    auto resultType = node->getType();
    bool hasResult = !resultType.empty();

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);

    llvm::PHINode* phi = nullptr;
    if (hasResult) {
        phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    }

    compileStatementBlockWithResult(node->thenBlock(), mergeBB, phi, resultType);

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);

    auto& elifs = node->elifs();
    auto elseBlock = node->elseBlock();

    if (!elifs.empty()) {
        for (size_t i = 0; i < elifs.size(); ++i) {
            auto& elif = elifs[i];
            auto elifCond = compileExpr(elif->condition());
            auto elifCondBool = _builder.CreateICmpNE(elifCond, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "elif.cond");

            llvm::BasicBlock* elifThenBB = llvm::BasicBlock::Create(_context, "elif.then", func);
            llvm::BasicBlock* elifElseBB = llvm::BasicBlock::Create(_context, "elif.else");

            _builder.CreateCondBr(elifCondBool, elifThenBB, elifElseBB);

            _builder.SetInsertPoint(elifThenBB);
            compileStatementBlockWithResult(elif->block(), mergeBB, phi, resultType);

            func->insert(func->end(), elifElseBB);
            _builder.SetInsertPoint(elifElseBB);
        }
    }

    if (elseBlock) {
        compileStatementBlockWithResult(elseBlock, mergeBB, phi, resultType);
    } else {
        if (hasResult) {
            phi->addIncoming(llvm::UndefValue::get(getLLVMType(resultType)), _builder.GetInsertBlock());
        }
        _builder.CreateBr(mergeBB);
    }

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    if (hasResult) {
        return phi;
    }
    return nullptr;
}

llvm::Value* Compiler::compileArrayGetExpr(p<ExprGetNode> node) {
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    if (!arrayType.isArray()) {
        throw YuxError("Cannot index non-array type: {}", arrayType.name);
    }

    auto& indices = node->indices();
    if (indices.empty()) {
        throw YuxError("Array access requires at least one index");
    }

    DEBUG_LOG_VAL("    Expr: ArrayGet", arrayType.name);

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue()->getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError("Array variable not found: {}", varName);
            }
            currentPtr = it->second;
        }
    }

    if (!currentPtr) {
        throw YuxError("Array access requires a variable");
    }

    for (auto& indexExpr : indices) {
        auto indexVal = compileExpr(indexExpr);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* gepIndices[] = {zero, indexVal};

        auto llvmArrayType = getLLVMType(currentType);
        currentPtr = _builder.CreateGEP(llvmArrayType, currentPtr, gepIndices, "array.element");

        if (currentType.elementType) {
            currentType = *currentType.elementType;
        }
    }

    return _builder.CreateLoad(getLLVMType(currentType), currentPtr, "array.load");
}

llvm::Value* Compiler::compileArrayLiteralExpr(p<ExprArrayNode> node) {
    auto& elements = node->elements();
    if (elements.empty()) {
        throw YuxError("Empty array literal not supported");
    }

    auto arrayType = node->getType();
    auto llvmArrayType = getLLVMType(arrayType);

    DEBUG_LOG_VAL("    Expr: ArrayLiteral", arrayType.name);

    auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.literal");

    for (size_t i = 0; i < elements.size(); ++i) {
        auto elemVal = compileExpr(elements[i]);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, index};
        auto elemPtr = _builder.CreateGEP(llvmArrayType, alloca, indices, "array.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
    }

    return _builder.CreateLoad(llvmArrayType, alloca, "array.load");
}

llvm::Value* Compiler::compileGetRefExpr(p<ExprGetRefNode> node) {
    auto objName = node->obj()->getText();
    auto& subs = node->subs();
    
    DEBUG_LOG_VAL("    Expr: GetRef", objName);
    
    auto it = _localVarPtrs.find(objName);
    if (it == _localVarPtrs.end()) {
        throw YuxError("Variable not found: {}", objName);
    }
    
    llvm::Value* currentPtr = it->second;
    auto sym = _currentFnNode->lookupSymbol(objName);
    if (!sym) {
        throw YuxError("Undefined variable: {}", objName);
    }
    
    TypeInfo currentType = sym->type;
    
    for (auto& sub : subs) {
        auto memberName = sub->getText();
        auto structDecl = _file->getStructDecl(currentType.name);
        if (!structDecl) {
            throw YuxError("Cannot access field on non-struct type: {}", currentType.name);
        }
        
        int fieldIndex = structDecl->fieldIndex(memberName);
        if (fieldIndex < 0) {
            throw YuxError("Struct {} has no field: {}", currentType.name, memberName);
        }
        
        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        llvm::Value* indices[] = {zero, idx};
        
        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        currentType = structDecl->fields()[fieldIndex]->getType();
    }
    
    return currentPtr;
}

llvm::Value* Compiler::compileExpr(p<ExprNode> node) {
    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(node)) {
        return compileLiteralExpr(literalNode);
    }
    else if (auto addSubNode = dynamic_cast<ExprAddSubNode*>(node)) {
        return compileAddSubExpr(addSubNode);
    }
    else if (auto mulDivModNode = dynamic_cast<ExprMulDivModNode*>(node)) {
        return compileMulDivModExpr(mulDivModNode);
    }
    else if (auto parenNode = dynamic_cast<ExprParenNode*>(node)) {
        return compileParenExpr(parenNode);
    }
    else if (auto callNode = dynamic_cast<ExprCallNode*>(node)) {
        return compileCallExpr(callNode);
    }
    else if (auto dotNode = dynamic_cast<ExprDotNode*>(node)) {
        return compileDotExpr(dotNode);
    }
    else if (auto compareNode = dynamic_cast<ExprCompareNode*>(node)) {
        return compileCompareExpr(compareNode);
    }
    else if (auto ifElseNode = dynamic_cast<ExprIfElseNode*>(node)) {
        return compileIfElseExpr(ifElseNode);
    }
    else if (auto getNode = dynamic_cast<ExprGetNode*>(node)) {
        return compileArrayGetExpr(getNode);
    }
    else if (auto arrayNode = dynamic_cast<ExprArrayNode*>(node)) {
        return compileArrayLiteralExpr(arrayNode);
    }
    else if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(node)) {
        return compileGetRefExpr(getRefNode);
    }

    throw YuxError("Unsupported expression type");
}

void Compiler::compileStatementBlock(p<StatementBlockNode> block) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }
    if (block->hasResult()) {
        compileExpr(block->resultExpr());
    }
}

llvm::Value* Compiler::compileStatementBlockWithResult(p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }
    
    if (_builder.GetInsertBlock()->getTerminator()) {
        return nullptr;
    }
    
    if (block->hasResult()) {
        auto resultVal = compileExpr(block->resultExpr());
        if (phi && !resultType.empty()) {
            phi->addIncoming(resultVal, _builder.GetInsertBlock());
        }
    }
    
    _builder.CreateBr(continueBlock);
    return nullptr;
}

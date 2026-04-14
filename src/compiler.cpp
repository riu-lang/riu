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

    if (type.isPtr()) {
        vector<llvm::Type*> ptrFields;
        ptrFields.push_back(_builder.getInt64Ty());
        return llvm::StructType::get(_context, ptrFields);
    }

    if (type.isBox()) {
        auto elemType = type.boxElementType();
        if (elemType) {
            vector<llvm::Type*> boxFields;
            boxFields.push_back(llvm::PointerType::get(getLLVMType(*elemType), 0));
            boxFields.push_back(llvm::PointerType::get(_builder.getInt64Ty(), 0));
            return llvm::StructType::get(_context, boxFields);
        }
        return llvm::PointerType::get(_builder.getInt8Ty(), 0);
    }

    if (type.isArrayGeneric()) {
        auto elemType = type.arrayGenericElementType();
        if (elemType) {
            vector<llvm::Type*> arrayFields;
            arrayFields.push_back(llvm::PointerType::get(getLLVMType(*elemType), 0));
            arrayFields.push_back(_builder.getInt64Ty());
            arrayFields.push_back(_builder.getInt64Ty());
            return llvm::StructType::get(_context, arrayFields);
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

llvm::Function* Compiler::getMethodFunction(const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes, const TypeInfo& retType) {
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
    
    auto llvmRetType = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
    auto fnType = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

llvm::Function* Compiler::getDestructorFunction(const string& structName) {
    string mangledStructName = _file->getMangledName(structName);
    string mangledName = mangledStructName + "__destructor";
    
    auto func = _module->getFunction(mangledName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(llvm::PointerType::get(getLLVMType(TypeInfo(structName)), 0));
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), llvmParamTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

llvm::Function* Compiler::getBoxAllocFn() {
    string fnName = "_box_alloc";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(_builder.getInt64Ty());
    
    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(_builder.getInt8Ty(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

llvm::Function* Compiler::getBoxRetainFn() {
    string fnName = "_box_retain";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt64Ty(), 0));
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

llvm::Function* Compiler::getBoxReleaseFn() {
    string fnName = "_box_release";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt64Ty(), 0));
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

void Compiler::emitBoxHelpers() {
    auto getProcessHeapFn = _module->getFunction("GetProcessHeap");
    if (!getProcessHeapFn) {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {},
            false
        );
        getProcessHeapFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "GetProcessHeap",
            _module
        );
    }
    
    auto heapAllocFn = _module->getFunction("HeapAlloc");
    if (!heapAllocFn) {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty(), _builder.getInt64Ty()},
            false
        );
        heapAllocFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "HeapAlloc",
            _module
        );
    }
    
    auto heapFreeFn = _module->getFunction("HeapFree");
    if (!heapFreeFn) {
        auto fnType = llvm::FunctionType::get(
            _builder.getInt32Ty(),
            {llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty(), llvm::PointerType::get(_builder.getInt8Ty(), 0)},
            false
        );
        heapFreeFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "HeapFree",
            _module
        );
    }

    {
        auto allocFn = getBoxAllocFn();
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", allocFn);
            _builder.SetInsertPoint(entry);
            
            auto args = allocFn->args();
            auto argIt = args.begin();
            llvm::Value* sizeVal = &(*argIt);
            
            auto heap = _builder.CreateCall(getProcessHeapFn, {}, "heap");
            
            auto refCountSize = _builder.getInt64(8);
            auto totalSize = _builder.CreateAdd(sizeVal, refCountSize, "total_size");
            
            auto mem = _builder.CreateCall(heapAllocFn, {heap, _builder.getInt64(0), totalSize}, "mem");
            
            auto refCountPtr = _builder.CreateBitCast(mem, llvm::PointerType::get(_builder.getInt64Ty(), 0), "ref_count_ptr");
            _builder.CreateStore(_builder.getInt64(1), refCountPtr);
            
            auto dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), mem, {refCountSize}, "data_ptr");
            
            _builder.CreateRet(dataPtr);
        }
    }
    
    {
        auto retainFn = getBoxRetainFn();
        if (retainFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", retainFn);
            _builder.SetInsertPoint(entry);
            
            auto args = retainFn->args();
            auto argIt = args.begin();
            llvm::Value* refCountPtr = &(*argIt);
            
            auto currentCount = _builder.CreateLoad(_builder.getInt64Ty(), refCountPtr, "current_count");
            auto newCount = _builder.CreateAdd(currentCount, _builder.getInt64(1), "new_count");
            _builder.CreateStore(newCount, refCountPtr);
            
            _builder.CreateRetVoid();
        }
    }
    
    {
        auto releaseFn = getBoxReleaseFn();
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", releaseFn);
            _builder.SetInsertPoint(entry);
            
            auto args = releaseFn->args();
            auto argIt = args.begin();
            llvm::Value* refCountPtr = &(*argIt);
            ++argIt;
            llvm::Value* dataPtr = &(*argIt);
            
            auto currentCount = _builder.CreateLoad(_builder.getInt64Ty(), refCountPtr, "current_count");
            auto newCount = _builder.CreateSub(currentCount, _builder.getInt64(1), "new_count");
            _builder.CreateStore(newCount, refCountPtr);
            
            auto isZero = _builder.CreateICmpEQ(newCount, _builder.getInt64(0), "is_zero");
            
            auto freeBB = llvm::BasicBlock::Create(_context, "free", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(_context, "done", releaseFn);
            
            _builder.CreateCondBr(isZero, freeBB, doneBB);
            
            _builder.SetInsertPoint(freeBB);
            auto heap = _builder.CreateCall(getProcessHeapFn, {}, "heap");
            auto refCountSize = _builder.getInt64(8);
            auto memPtr = _builder.CreateGEP(_builder.getInt8Ty(), dataPtr, {_builder.CreateNeg(refCountSize)}, "mem_ptr");
            _builder.CreateCall(heapFreeFn, {heap, _builder.getInt64(0), memPtr});
            _builder.CreateBr(doneBB);
            
            _builder.SetInsertPoint(doneBB);
            _builder.CreateRetVoid();
        }
    }
}

llvm::Function* Compiler::getArrayAllocFn() {
    string fnName = "_array_alloc";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(_builder.getInt64Ty());
    
    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(_builder.getInt8Ty(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

llvm::Function* Compiler::getArrayGrowFn() {
    string fnName = "_array_grow";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
    paramTypes.push_back(_builder.getInt64Ty());
    
    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(_builder.getInt8Ty(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

llvm::Function* Compiler::getArrayReleaseFn() {
    string fnName = "_array_release";
    auto func = _module->getFunction(fnName);
    if (func) {
        return func;
    }
    
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
    
    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
}

void Compiler::emitArrayHelpers() {
    auto getProcessHeapFn = _module->getFunction("GetProcessHeap");
    if (!getProcessHeapFn) {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {},
            false
        );
        getProcessHeapFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "GetProcessHeap",
            _module
        );
    }
    
    auto heapAllocFn = _module->getFunction("HeapAlloc");
    if (!heapAllocFn) {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty(), _builder.getInt64Ty()},
            false
        );
        heapAllocFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "HeapAlloc",
            _module
        );
    }
    
    auto heapReAllocFn = _module->getFunction("HeapReAlloc");
    if (!heapReAllocFn) {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(_builder.getInt8Ty(), 0),
            {llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty(), llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty()},
            false
        );
        heapReAllocFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "HeapReAlloc",
            _module
        );
    }
    
    auto heapFreeFn = _module->getFunction("HeapFree");
    if (!heapFreeFn) {
        auto fnType = llvm::FunctionType::get(
            _builder.getInt32Ty(),
            {llvm::PointerType::get(_builder.getInt8Ty(), 0), _builder.getInt64Ty(), llvm::PointerType::get(_builder.getInt8Ty(), 0)},
            false
        );
        heapFreeFn = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "HeapFree",
            _module
        );
    }

    {
        auto allocFn = getArrayAllocFn();
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", allocFn);
            _builder.SetInsertPoint(entry);
            
            auto args = allocFn->args();
            auto argIt = args.begin();
            llvm::Value* sizeVal = &(*argIt);
            
            auto heap = _builder.CreateCall(getProcessHeapFn, {}, "heap");
            auto mem = _builder.CreateCall(heapAllocFn, {heap, _builder.getInt64(0), sizeVal}, "mem");
            
            _builder.CreateRet(mem);
        }
    }
    
    {
        auto growFn = getArrayGrowFn();
        if (growFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", growFn);
            _builder.SetInsertPoint(entry);
            
            auto args = growFn->args();
            auto argIt = args.begin();
            llvm::Value* oldPtr = &(*argIt);
            ++argIt;
            llvm::Value* newSize = &(*argIt);
            
            auto heap = _builder.CreateCall(getProcessHeapFn, {}, "heap");
            
            auto isNull = _builder.CreateICmpEQ(oldPtr, llvm::ConstantPointerNull::get(llvm::PointerType::get(_builder.getInt8Ty(), 0)), "is_null");
            
            auto allocBB = llvm::BasicBlock::Create(_context, "alloc", growFn);
            auto reallocBB = llvm::BasicBlock::Create(_context, "realloc", growFn);
            auto doneBB = llvm::BasicBlock::Create(_context, "done", growFn);
            
            _builder.CreateCondBr(isNull, allocBB, reallocBB);
            
            _builder.SetInsertPoint(allocBB);
            auto newMemAlloc = _builder.CreateCall(heapAllocFn, {heap, _builder.getInt64(0), newSize}, "new_mem");
            _builder.CreateBr(doneBB);
            
            _builder.SetInsertPoint(reallocBB);
            auto newMemRealloc = _builder.CreateCall(heapReAllocFn, {heap, _builder.getInt64(0), oldPtr, newSize}, "new_mem");
            _builder.CreateBr(doneBB);
            
            _builder.SetInsertPoint(doneBB);
            auto phi = _builder.CreatePHI(llvm::PointerType::get(_builder.getInt8Ty(), 0), 2, "result");
            phi->addIncoming(newMemAlloc, allocBB);
            phi->addIncoming(newMemRealloc, reallocBB);
            
            _builder.CreateRet(phi);
        }
    }
    
    {
        auto releaseFn = getArrayReleaseFn();
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(_context, "entry", releaseFn);
            _builder.SetInsertPoint(entry);
            
            auto args = releaseFn->args();
            auto argIt = args.begin();
            llvm::Value* dataPtr = &(*argIt);
            
            auto isNull = _builder.CreateICmpEQ(dataPtr, llvm::ConstantPointerNull::get(llvm::PointerType::get(_builder.getInt8Ty(), 0)), "is_null");
            
            auto freeBB = llvm::BasicBlock::Create(_context, "free", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(_context, "done", releaseFn);
            
            _builder.CreateCondBr(isNull, doneBB, freeBB);
            
            _builder.SetInsertPoint(freeBB);
            auto heap = _builder.CreateCall(getProcessHeapFn, {}, "heap");
            _builder.CreateCall(heapFreeFn, {heap, _builder.getInt64(0), dataPtr});
            _builder.CreateBr(doneBB);
            
            _builder.SetInsertPoint(doneBB);
            _builder.CreateRetVoid();
        }
    }
}

void Compiler::emitMainStartup() {
    auto setConsoleOutputCP = _module->getFunction("SetConsoleOutputCP");
    if (!setConsoleOutputCP) {
        auto fnType = llvm::FunctionType::get(
            _builder.getInt1Ty(),
            {_builder.getInt32Ty()},
            false
        );
        setConsoleOutputCP = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "SetConsoleOutputCP",
            _module
        );
    }
    
    auto setConsoleCP = _module->getFunction("SetConsoleCP");
    if (!setConsoleCP) {
        auto fnType = llvm::FunctionType::get(
            _builder.getInt1Ty(),
            {_builder.getInt32Ty()},
            false
        );
        setConsoleCP = llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            "SetConsoleCP",
            _module
        );
    }
    
    auto fnType = llvm::FunctionType::get(_builder.getInt32Ty(), {}, false);
    auto mainStartup = llvm::Function::Create(
        fnType,
        llvm::Function::ExternalLinkage,
        "mainStartup",
        _module
    );
    
    auto entry = llvm::BasicBlock::Create(_context, "entry", mainStartup);
    _builder.SetInsertPoint(entry);
    
    auto cpUtf8 = llvm::ConstantInt::get(_builder.getInt32Ty(), 65001);
    _builder.CreateCall(setConsoleOutputCP, {cpUtf8});
    _builder.CreateCall(setConsoleCP, {cpUtf8});
    
    auto yuxMain = _module->getFunction("yux_main");
    if (yuxMain) {
        _builder.CreateCall(yuxMain, {});
    }
    _builder.CreateRet(_builder.getInt32(0));
}

void Compiler::emitRuntimeHelpers() {
    auto chkstkFnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
    auto chkstk = llvm::Function::Create(
        chkstkFnType,
        llvm::Function::ExternalLinkage,
        "__chkstk",
        _module
    );
    auto chkstkEntry = llvm::BasicBlock::Create(_context, "entry", chkstk);
    _builder.SetInsertPoint(chkstkEntry);
    _builder.CreateRetVoid();
    
    auto fltused = new llvm::GlobalVariable(
        *_module,
        _builder.getInt32Ty(),
        true,
        llvm::GlobalValue::ExternalLinkage,
        _builder.getInt32(0),
        "_fltused"
    );
}

void Compiler::compile(p<FileNode> file) {
    compileGlobalConsts();
    compileStructDecls();
    compileStructImpls();
    
    if (_isSdk) {
        emitRuntimeHelpers();
    } else {
        emitBoxHelpers();
        emitArrayHelpers();
    }
    
    auto functions = file->getFunctions();
    for (auto fn : functions) {
        auto func = getFunction(fn->header());
        compileFn(fn, func);
    }
    
    if (!_isSdk) {
        emitMainStartup();
    }
}

void Compiler::compileGlobalConsts() {
    for (auto globalConst : _file->getGlobalConsts()) {
        string name = globalConst->name()->getText();
        string mangledName = _file->getMangledName(name);
        TypeInfo type = globalConst->getType();
        auto llvmType = getLLVMType(type);
        
        llvm::Constant* initValue = nullptr;
        auto literal = globalConst->value();
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
            initValue = llvm::ConstantInt::get(llvmType, numVal, true);
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
            initValue = llvm::ConstantFP::get(llvmType, numVal);
        }
        else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
            bool boolVal = (text == "true");
            initValue = llvm::ConstantInt::get(llvmType, boolVal ? 1 : 0, false);
        }
        else {
            throw YuxError("Unsupported literal type for global constant: {}", type.name);
        }
        
        auto linkage = globalConst->isPrivate() ? 
            llvm::GlobalValue::InternalLinkage : 
            llvm::GlobalValue::ExternalLinkage;
        
        auto globalVar = new llvm::GlobalVariable(
            *_module,
            llvmType,
            true,
            linkage,
            initValue,
            mangledName
        );
        
        DEBUG_LOG_VAL("Created global constant", mangledName << " : " << type.name);
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
        
        if (structImpl->hasDestructor()) {
            auto destructor = structImpl->destructor();
            vector<TypeInfo> paramTypes;
            paramTypes.push_back(TypeInfo(structName));
            
            auto func = getDestructorFunction(structName);
            compileMethod(destructor, func, structName);
        }
        
        for (auto method : structImpl->methods()) {
            vector<TypeInfo> paramTypes;
            for (auto param : method->header()->params()) {
                if (param->type()) {
                    paramTypes.push_back(param->type()->getType());
                }
            }
            TypeInfo retType;
            if (method->header()->retType()) {
                retType = method->header()->retType()->getType();
            }
            auto func = getMethodFunction(structName, method->header()->name()->getText(), paramTypes, retType);
            compileMethod(method, func, structName);
        }
    }
}

void Compiler::compileFn(p<FnNode> node, llvm::Function* func) {
    _currentFn = func;
    _currentFnNode = node;
    _currentStructName.clear();
    _localVarPtrs.clear();
    _scopeVars.clear();

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
    if (!_builder.GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling function", node->header()->name()->getText());
}

void Compiler::compileMethod(p<FnNode> node, llvm::Function* func, const string& structName) {
    _currentFn = func;
    _currentFnNode = node;
    _currentStructName = structName;
    _localVarPtrs.clear();
    _scopeVars.clear();

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

    if (!_builder.GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling method", structName << "." << node->header()->name()->getText());
}

void Compiler::compileRetStatement(p<StatementRetNode> node) {
    DEBUG_LOG("  Statement: Return");
    auto retType = node->expr()->getType();
    llvm::Value* retVal = nullptr;
    if (retType.empty()) {
        compileExpr(node->expr());
        DEBUG_LOG("    Expression compiled as void return");
    } else {
        retVal = compileExpr(node->expr());
        DEBUG_LOG("    Created return value");
    }
    callDestructorsForScope();
    if (retVal) {
        _builder.CreateRet(retVal);
        DEBUG_LOG("    Created return instruction");
    } else {
        _builder.CreateRetVoid();
        DEBUG_LOG("    Created void return instruction");
    }
}

void Compiler::compileRetVoidStatement(p<StatementRetVoidNode> node) {
    DEBUG_LOG("  Statement: Return Void");
    callDestructorsForScope();
    _builder.CreateRetVoid();
    DEBUG_LOG("    Created void return instruction");
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
        
        if (varType.isBox()) {
            auto elemType = varType.boxElementType();
            if (!elemType) {
                throw YuxError("Box type requires element type");
            }
            
            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();
            
            auto boxStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            
            auto elemLLVMType = getLLVMType(*elemType);
            auto sizeVal = _builder.CreateIntCast(
                _builder.getInt64(elemLLVMType->getPrimitiveSizeInBits() / 8),
                _builder.getInt64Ty(),
                false
            );
            
            auto allocFn = getBoxAllocFn();
            auto dataPtr = _builder.CreateCall(allocFn, {sizeVal}, "box_data_ptr");
            
            auto dataPtrTyped = _builder.CreateBitCast(dataPtr, llvm::PointerType::get(elemLLVMType, 0), "box_data_typed");
            
            if (exprType == *elemType) {
                _builder.CreateStore(exprVal, dataPtrTyped);
            } else if (exprType.isBox() && exprType.boxElementType() && *exprType.boxElementType() == *elemType) {
                auto srcBoxPtr = _localVarPtrs.find(varName);
                if (srcBoxPtr != _localVarPtrs.end()) {
                    auto srcDataPtrPtr = _builder.CreateGEP(boxStructType, exprVal, {zero, zero}, "src_data_ptr_ptr");
                    auto srcDataPtr = _builder.CreateLoad(llvm::PointerType::get(elemLLVMType, 0), srcDataPtrPtr, "src_data_ptr");
                    _builder.CreateStore(srcDataPtr, dataPtrTyped);
                }
            } else {
                throw YuxError("Box type mismatch: expected Box<{}>, got {}", elemType->name, exprType.name);
            }
            
            auto refCountPtr = _builder.CreateGEP(
                _builder.getInt8Ty(),
                dataPtr,
                {_builder.getInt64(-8)},
                "ref_count_ptr_raw"
            );
            auto refCountPtrTyped = _builder.CreateBitCast(
                refCountPtr,
                llvm::PointerType::get(_builder.getInt64Ty(), 0),
                "ref_count_ptr"
            );
            
            llvm::Value* indices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(boxStructType, alloca, indices, "data_ptr_field");
            _builder.CreateStore(dataPtrTyped, dataPtrField);
            
            llvm::Value* indices2[] = {zero, one};
            auto refCountField = _builder.CreateGEP(boxStructType, alloca, indices2, "ref_count_field");
            _builder.CreateStore(refCountPtrTyped, refCountField);
            
            _scopeVars.push_back(varName);
        } else if (varType.isArrayGeneric()) {
            auto elemType = varType.arrayGenericElementType();
            if (!elemType) {
                throw YuxError("Array type requires element type");
            }
            
            auto arrayStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto two = llvm::ConstantInt::get(_builder.getInt32Ty(), 2);
            
            auto elemLLVMType = getLLVMType(*elemType);
            auto elemSize = elemLLVMType->getPrimitiveSizeInBits() / 8;
            
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto& elements = arrayNode->elements();
                auto count = elements.size();
                
                llvm::Value* dataPtr = nullptr;
                if (count > 0) {
                    auto totalSize = _builder.getInt64(count * elemSize);
                    auto allocFn = getArrayAllocFn();
                    dataPtr = _builder.CreateCall(allocFn, {totalSize}, "array_data_ptr");
                    
                    auto dataPtrTyped = _builder.CreateBitCast(dataPtr, llvm::PointerType::get(elemLLVMType, 0), "array_data_typed");
                    
                    for (size_t i = 0; i < count; ++i) {
                        auto elemVal = compileExpr(elements[i]);
                        auto index = llvm::ConstantInt::get(_builder.getInt64Ty(), i);
                        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtrTyped, {index}, "elem.ptr");
                        _builder.CreateStore(elemVal, elemPtr);
                    }
                } else {
                    dataPtr = llvm::ConstantPointerNull::get(llvm::PointerType::get(_builder.getInt8Ty(), 0));
                }
                
                llvm::Value* indices0[] = {zero, zero};
                auto dataField = _builder.CreateGEP(arrayStructType, alloca, indices0, "data_field");
                _builder.CreateStore(dataPtr, dataField);
                
                llvm::Value* indices1[] = {zero, one};
                auto lenField = _builder.CreateGEP(arrayStructType, alloca, indices1, "len_field");
                _builder.CreateStore(_builder.getInt64(count), lenField);
                
                llvm::Value* indices2[] = {zero, two};
                auto capField = _builder.CreateGEP(arrayStructType, alloca, indices2, "cap_field");
                _builder.CreateStore(_builder.getInt64(count), capField);
            } else {
                throw YuxError("Array<T> initialization requires array literal");
            }
            
            _scopeVars.push_back(varName);
        } else {
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
            
            auto structDecl = _file->getStructDecl(varType.name);
            if (structDecl) {
                _scopeVars.push_back(varName);
            }
        }
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
        
        if (sym->type.isPtr()) {
            auto memberName = subs[0]->getText();
            if (memberName == "_value") {
                if (!_isSdk) {
                    throw YuxError("Cannot access private field '_value' of Ptr type (sdk only)");
                }
                if (subs.size() != 1) {
                    throw YuxError("Nested member access not supported for Ptr._value");
                }
                
                DEBUG_LOG_VAL("  Statement: PtrValueAssign", objName << "._value");
                
                auto it = _localVarPtrs.find(objName);
                if (it == _localVarPtrs.end()) {
                    throw YuxError("Variable not found: {}", objName);
                }
                
                auto ptrStructType = getLLVMType(sym->type);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                llvm::Value* indices[] = {zero, zero};
                auto valueField = _builder.CreateGEP(ptrStructType, it->second, indices, "ptr_value_field");
                _builder.CreateStore(exprVal, valueField);
                return;
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
            if (field->isPrivate() && _currentStructName != actualType.name) {
                throw YuxError("Cannot access private field '{}' of struct '{}'", memberName, actualType.name);
            }
            
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

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError("Array type requires element type");
        }
        
        auto arrayStructType = getLLVMType(arrayType);
        auto elemLLVMType = getLLVMType(*elemType);
        
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices0[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, currentPtr, indices0, "array.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(elemLLVMType, 0), dataFieldPtr, "array.data.ptr");
        
        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");
        
        auto valueVal = compileExpr(node->valueExpr());
        _builder.CreateStore(valueVal, elemPtr);
        return;
    }

    if (!arrayType.isArray()) {
        throw YuxError("Cannot index non-array type: {}", arrayType.name);
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
    else if (auto retVoidNode = dynamic_cast<StatementRetVoidNode*>(node)) {
        compileRetVoidStatement(retVoidNode);
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

    if (dstType.isPtr()) {
        if (srcType.isRef()) {
            auto refElemType = srcType.refElementType();
            if (refElemType) {
                auto ptrElemType = dstType.ptrElementType();
                if (ptrElemType && *refElemType == *ptrElemType) {
                    DEBUG_LOG("      Ref -> Ptr");
                    auto ptrToInt = _builder.CreatePtrToInt(val, _builder.getInt64Ty(), "ptr_to_int");
                    auto ptrStructType = getLLVMType(dstType);
                    auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ptr_tmp");
                    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    llvm::Value* indices[] = {zero, zero};
                    auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "ptr_value_field");
                    _builder.CreateStore(ptrToInt, valueField);
                    return _builder.CreateLoad(ptrStructType, alloca, "ptr_struct");
                }
            }
            return _builder.CreateBitCast(val, getLLVMType(dstType));
        }
        if (srcType.isBox()) {
            auto boxElemType = srcType.boxElementType();
            if (boxElemType) {
                auto ptrElemType = dstType.ptrElementType();
                if (ptrElemType && *boxElemType == *ptrElemType) {
                    DEBUG_LOG("      Box -> Ptr");
                    auto boxStructType = getLLVMType(srcType);
                    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    llvm::Value* indices[] = {zero, zero};
                    auto dataPtrField = _builder.CreateGEP(boxStructType, val, indices, "box.data_ptr_field");
                    auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(getLLVMType(*ptrElemType), 0), dataPtrField, "box.data_ptr");
                    auto ptrToInt = _builder.CreatePtrToInt(dataPtr, _builder.getInt64Ty(), "ptr_to_int");
                    auto ptrStructType = getLLVMType(dstType);
                    auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ptr_tmp");
                    llvm::Value* indices2[] = {zero, zero};
                    auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices2, "ptr_value_field");
                    _builder.CreateStore(ptrToInt, valueField);
                    return _builder.CreateLoad(ptrStructType, alloca, "ptr_struct");
                }
            }
            return _builder.CreateBitCast(val, getLLVMType(dstType));
        }
    }

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
        
        string mangledName = _file->getMangledName(varName);
        auto globalVar = _module->getGlobalVariable(mangledName, true);
        if (globalVar) {
            DEBUG_LOG_VAL("    Expr: GlobalConstLoad", varName << " : " << (sym ? sym->type.name : "unknown"));
            return _builder.CreateLoad(globalVar->getValueType(), globalVar, "global.load");
        }
        
        throw YuxError("Undefined variable: {}", varName);
    }
    else if (auto nullLiteral = dynamic_cast<LiteralNullNode*>(literal)) {
        DEBUG_LOG("    Expr: NullLiteral");
        auto ptrStructType = getLLVMType(TypeInfo("Ptr", {make_shared<TypeInfo>("u8")}));
        auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "null_tmp");
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices[] = {zero, zero};
        auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "null_value_field");
        _builder.CreateStore(_builder.getInt64(0), valueField);
        return _builder.CreateLoad(ptrStructType, alloca, "null_ptr");
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

llvm::Value* Compiler::compileBinOpExpr(p<ExprBinOpNode> node) {
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    auto type = node->getType();

    string opStr;
    switch (node->op()) {
        case ExprBinOpNode::Op::And: opStr = "&"; break;
        case ExprBinOpNode::Op::Or: opStr = "|"; break;
        case ExprBinOpNode::Op::Xor: opStr = "^"; break;
        case ExprBinOpNode::Op::Shl: opStr = "<<"; break;
        case ExprBinOpNode::Op::Shr: opStr = ">>"; break;
    }
    DEBUG_LOG_VAL("    Expr: BinOp", opStr << " : " << type.name);

    switch (node->op()) {
        case ExprBinOpNode::Op::And:
            return _builder.CreateAnd(left, right);
        case ExprBinOpNode::Op::Or:
            return _builder.CreateOr(left, right);
        case ExprBinOpNode::Op::Xor:
            return _builder.CreateXor(left, right);
        case ExprBinOpNode::Op::Shl:
            return _builder.CreateShl(left, right);
        case ExprBinOpNode::Op::Shr:
            if (type.startsWith('u')) {
                return _builder.CreateLShr(left, right);
            }
            return _builder.CreateAShr(left, right);
    }
    throw YuxError("Unsupported binary operation");
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
    TypeInfo actualType = baseType;
    
    if (baseType.isBox()) {
        auto boxElemType = baseType.boxElementType();
        if (boxElemType) {
            actualType = *boxElemType;
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

        if (methodSymbol->isPrivate && _currentStructName != actualType.name) {
            throw YuxError("Cannot call private method '{}' of struct '{}'", member, actualType.name);
        }

        llvm::Value* basePtr = nullptr;
        if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                auto varName = objLiteral->getValue()->getText();
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
        
        if (baseType.isBox()) {
            auto boxStructType = getLLVMType(baseType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            llvm::Value* indices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(boxStructType, basePtr, indices, "box.data_ptr_field");
            dataPtr = _builder.CreateLoad(llvm::PointerType::get(getLLVMType(actualType), 0), dataPtrField, "box.data_ptr");
        }

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(dataPtr);
        for (auto& arg : args) {
            methodArgs.push_back(arg);
        }

        string mangledStructName = _file->getMangledName(actualType.name);
        string mangledName = mangledStructName + "_" + member;
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(llvm::PointerType::get(getLLVMType(actualType), 0));
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
        if (structDecl->isPrivate()) {
            throw YuxError("Cannot use private struct '{}' in constructor", fnName);
        }
        auto result = compileConstructorCall(fnName, args, argTypes);
        if (result) {
            return result;
        }
    }

    auto fnSymbol = _file->lookupFnSymbolWithParams(fnName, argTypes);
    
    if (fnSymbol) {
        if (fnSymbol->isPrivate && !fnSymbol->moduleName.empty() && fnSymbol->moduleName != _file->moduleName()) {
            throw YuxError("Cannot call private function '{}'", fnName);
        }
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
                paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
            } else {
                paramTypes.push_back(args[i]->getType());
            }
        }
        auto llvmRetType = retIsPtr ? llvm::PointerType::get(_builder.getInt8Ty(), 0) : _builder.getVoidTy();
        auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
    }
    
    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        bool paramIsPtrInSignature = (i < fn->getFunctionType()->getNumParams()) && 
                                      fn->getFunctionType()->getParamType(i)->isPointerTy();
        bool argIsPtrStruct = argTypes[i].isPtr();
        bool argIsRef = argTypes[i].isRef();
        
        if (argIsPtrStruct && paramIsPtrInSignature) {
            auto ptrStructType = getLLVMType(argTypes[i]);
            auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ptr_arg_tmp");
            _builder.CreateStore(args[i], alloca);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            llvm::Value* indices[] = {zero, zero};
            auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "ptr_value_field");
            auto valueInt = _builder.CreateLoad(_builder.getInt64Ty(), valueField, "ptr_value_int");
            auto ptrVal = _builder.CreateIntToPtr(valueInt, llvm::PointerType::get(_builder.getInt8Ty(), 0), "ptr_value");
            callArgs.push_back(ptrVal);
        } else if (paramIsPtrInSignature && args[i]->getType()->isPointerTy()) {
            auto ptrVal = _builder.CreateBitCast(args[i], llvm::PointerType::get(_builder.getInt8Ty(), 0), "ptr_cast");
            callArgs.push_back(ptrVal);
        } else {
            callArgs.push_back(args[i]);
        }
    }
    
    auto callResult = _builder.CreateCall(fn, callArgs);
    
    if (retIsPtr) {
        auto ptrToInt = _builder.CreatePtrToInt(callResult, _builder.getInt64Ty(), "ret_ptr_to_int");
        auto ptrStructType = getLLVMType(retType);
        auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ret_ptr_tmp");
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices[] = {zero, zero};
        auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "ret_ptr_value_field");
        _builder.CreateStore(ptrToInt, valueField);
        return _builder.CreateLoad(ptrStructType, alloca, "ret_ptr_struct");
    }
    
    return callResult;
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
    if (fnSymbol->isExternal) {
        cName = fnName;
    } else if (fnName == "main") {
        cName = "yux_main";
    } else {
        cName = _file->getMangledName(fnName, fnSymbol->params);
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
        for (size_t i = 0; i < fnSymbol->params.size(); ++i) {
            if (fnSymbol->isExternal && fnSymbol->params[i].isPtr()) {
                paramTypes.push_back(llvm::PointerType::get(_builder.getInt8Ty(), 0));
            } else {
                paramTypes.push_back(getLLVMType(fnSymbol->params[i]));
            }
        }
        auto retType = fnSymbol->retType.empty() ? _builder.getVoidTy() : 
                        (fnSymbol->isExternal && TypeInfo(fnSymbol->retType).isPtr() ? 
                         llvm::PointerType::get(_builder.getInt8Ty(), 0) : 
                         getLLVMType(TypeInfo(fnSymbol->retType)));
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }
    
    DEBUG_LOG_VAL("    Function signature check", "numParams=" << fn->getFunctionType()->getNumParams());
    for (size_t i = 0; i < fn->getFunctionType()->getNumParams() && i < args.size(); ++i) {
        auto expectedType = fn->getFunctionType()->getParamType(i);
        auto actualType = args[i]->getType();
        DEBUG_LOG_VAL("    Param type", i << " expected=" << expectedType->getTypeID() << " actual=" << actualType->getTypeID());
        if (expectedType->isIntegerTy() && actualType->isIntegerTy()) {
            DEBUG_LOG_VAL("    Integer bit width", "expected=" << expectedType->getIntegerBitWidth() << " actual=" << actualType->getIntegerBitWidth());
        }
        if (expectedType != actualType) {
            DEBUG_LOG_VAL("    TYPE MISMATCH", "need conversion");
        }
    }
    
    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size() && i < fnSymbol->params.size(); ++i) {
        DEBUG_LOG_VAL("    Param", i << " argType=" << argTypes[i].name << " paramType=" << fnSymbol->params[i].name);
        DEBUG_LOG_VAL("    Param isPtr", argTypes[i].isPtr() << " paramIsPtr=" << fnSymbol->params[i].isPtr());
        if (fnSymbol->params[i].isRef() && !fnSymbol->isExternal) {
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
        
        if (fnSymbol->params[i].isPtr()) {
            if (argTypes[i].isPtr()) {
                DEBUG_LOG_VAL("    Converting Ptr struct to pointer for external function", "arg " << i);
                DEBUG_LOG_VAL("    args[i] type", args[i]->getType()->getTypeID());
                auto ptrStructType = getLLVMType(argTypes[i]);
                auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ptr_arg_tmp");
                _builder.CreateStore(args[i], alloca);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                llvm::Value* indices[] = {zero, zero};
                auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "ptr_value_field");
                auto valueInt = _builder.CreateLoad(_builder.getInt64Ty(), valueField, "ptr_value_int");
                auto ptrVal = _builder.CreateIntToPtr(valueInt, llvm::PointerType::get(_builder.getInt8Ty(), 0), "ptr_value");
                callArgs.push_back(ptrVal);
                continue;
            }
            if (argTypes[i].isRef()) {
                DEBUG_LOG_VAL("    Converting Ref to Ptr struct", "arg " << i);
                if (args[i]->getType()->isPointerTy()) {
                    auto refPtr = args[i];
                    auto refElemType = argTypes[i].refElementType();
                    auto targetPtrStructType = getLLVMType(TypeInfo("Ptr", {make_shared<TypeInfo>("u8")}));
                    auto alloca = _builder.CreateAlloca(targetPtrStructType, nullptr, "ref_to_ptr_tmp");
                    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    llvm::Value* indices[] = {zero, zero};
                    auto valueField = _builder.CreateGEP(targetPtrStructType, alloca, indices, "ptr_value_field");
                    auto ptrToInt = _builder.CreatePtrToInt(refPtr, _builder.getInt64Ty(), "ref_ptr_to_int");
                    _builder.CreateStore(ptrToInt, valueField);
                    auto result = _builder.CreateLoad(targetPtrStructType, alloca, "ptr_struct");
                    callArgs.push_back(result);
                    continue;
                }
            }
            if (argTypes[i].isRef() && args[i]->getType()->isPointerTy()) {
                auto ptrVal = _builder.CreateBitCast(args[i], llvm::PointerType::get(_builder.getInt8Ty(), 0), "ptr_cast");
                callArgs.push_back(ptrVal);
                continue;
            }
            if (argTypes[i].isArray() && args[i]->getType()->isPointerTy()) {
                DEBUG_LOG_VAL("    Converting array to pointer for external function", "arg " << i);
                auto ptrVal = _builder.CreateBitCast(args[i], llvm::PointerType::get(_builder.getInt8Ty(), 0), "arr_to_ptr");
                callArgs.push_back(ptrVal);
                continue;
            }
            if (args[i]->getType()->isPointerTy()) {
                auto ptrVal = _builder.CreateBitCast(args[i], llvm::PointerType::get(_builder.getInt8Ty(), 0), "generic_ptr_cast");
                callArgs.push_back(ptrVal);
                continue;
            }
        }
        
        if (argTypes[i].isBox()) {
            auto boxStructType = getLLVMType(argTypes[i]);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            
            auto boxAlloca = _builder.CreateAlloca(boxStructType, nullptr, "box_arg_tmp");
            _builder.CreateStore(args[i], boxAlloca);
            
            llvm::Value* indices1[] = {zero, one};
            auto refCountFieldPtr = _builder.CreateGEP(boxStructType, boxAlloca, indices1, "ref_count_field_ptr");
            auto refCountPtr = _builder.CreateLoad(llvm::PointerType::get(_builder.getInt64Ty(), 0), refCountFieldPtr, "ref_count_ptr");
            
            auto retainFn = getBoxRetainFn();
            _builder.CreateCall(retainFn, {refCountPtr});
            
            callArgs.push_back(args[i]);
            continue;
        }
        
        callArgs.push_back(args[i]);
    }
    
    auto callResult = _builder.CreateCall(fn, callArgs);
    
    if (fnSymbol->isExternal && !fnSymbol->retType.empty() && TypeInfo(fnSymbol->retType).isPtr()) {
        auto ptrToInt = _builder.CreatePtrToInt(callResult, _builder.getInt64Ty(), "ret_ptr_to_int");
        auto ptrStructType = getLLVMType(TypeInfo(fnSymbol->retType));
        auto alloca = _builder.CreateAlloca(ptrStructType, nullptr, "ret_ptr_tmp");
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices[] = {zero, zero};
        auto valueField = _builder.CreateGEP(ptrStructType, alloca, indices, "ret_ptr_value_field");
        _builder.CreateStore(ptrToInt, valueField);
        return _builder.CreateLoad(ptrStructType, alloca, "ret_ptr_struct");
    }
    
    return callResult;
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
    
    if (baseType.isBox()) {
        auto boxElemType = baseType.boxElementType();
        if (boxElemType) {
            actualType = *boxElemType;
        }
    }
    
    if (baseType.isPtr()) {
        if (member == "_value") {
            if (!_isSdk) {
                throw YuxError("Cannot access private field '_value' of Ptr type (sdk only)");
            }
            DEBUG_LOG_VAL("    Expr: PtrFieldValue", "_value");
            
            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue()->getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        auto ptrStructType = getLLVMType(baseType);
                        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                        llvm::Value* indices[] = {zero, zero};
                        auto valueField = _builder.CreateGEP(ptrStructType, it->second, indices, "ptr_value_field");
                        return _builder.CreateLoad(_builder.getInt64Ty(), valueField, "ptr_value");
                    }
                }
            }
            throw YuxError("Cannot access _value on non-variable Ptr");
        }
    }
    
    auto structDecl = _file->getStructDecl(actualType.name);

    if (structDecl) {
        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            auto field = structDecl->fields()[fieldIndex];
            if (field->isPrivate() && _currentStructName != actualType.name) {
                throw YuxError("Cannot access private field '{}' of struct '{}'", member, actualType.name);
            }

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
            
            llvm::Value* dataPtr = structPtr;
            
            if (baseType.isBox()) {
                auto boxStructType = getLLVMType(baseType);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                llvm::Value* indices[] = {zero, zero};
                auto dataPtrField = _builder.CreateGEP(boxStructType, structPtr, indices, "box.data_ptr_field");
                dataPtr = _builder.CreateLoad(llvm::PointerType::get(getLLVMType(actualType), 0), dataPtrField, "box.data_ptr");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            llvm::Value* indices[] = {zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, dataPtr, indices, "struct.field");
            auto fieldType = field->getType();

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
        case ExprCompareNode::Op::AndAnd: opStr = "&&"; break;
        case ExprCompareNode::Op::OrOr: opStr = "||"; break;
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
        case ExprCompareNode::Op::AndAnd: {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.rhs");
            return _builder.CreateAnd(leftBool, rightBool, "and");
        }
        case ExprCompareNode::Op::OrOr: {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.rhs");
            return _builder.CreateOr(leftBool, rightBool, "or");
        }
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

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError("Array type requires element type");
        }
        
        auto arrayStructType = getLLVMType(arrayType);
        auto elemLLVMType = getLLVMType(*elemType);
        
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices0[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, currentPtr, indices0, "array.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(elemLLVMType, 0), dataFieldPtr, "array.data.ptr");
        
        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");
        
        return _builder.CreateLoad(elemLLVMType, elemPtr, "array.elem.load");
    }

    if (!arrayType.isArray()) {
        throw YuxError("Cannot index non-array type: {}", arrayType.name);
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
        
        auto field = structDecl->fields()[fieldIndex];
        if (field->isPrivate() && _currentStructName != currentType.name) {
            throw YuxError("Cannot access private field '{}' of struct '{}'", memberName, currentType.name);
        }
        
        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        llvm::Value* indices[] = {zero, idx};
        
        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        currentType = field->getType();
    }
    
    return currentPtr;
}

llvm::Value* Compiler::compileUnaryExpr(p<ExprUnaryNode> node) {
    auto right = compileExpr(node->right());
    auto type = node->getType();
    bool isFloat = type.startsWith('f');
    bool isBool = type.name == "bool";

    string opStr;
    switch (node->op()) {
        case ExprUnaryNode::Op::Neg:
            opStr = "-";
            DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);
            if (isFloat) {
                return _builder.CreateFNeg(right, "neg");
            }
            return _builder.CreateNeg(right, "neg");
        case ExprUnaryNode::Op::Rev:
            opStr = "~";
            DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);
            if (isFloat) {
                throw YuxError("Cannot apply bitwise NOT to float type: {}", type.name);
            }
            return _builder.CreateNot(right, "not");
        case ExprUnaryNode::Op::Not:
            opStr = "!";
            DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);
            if (!isBool) {
                throw YuxError("Cannot apply logical NOT to non-bool type: {}", type.name);
            }
            return _builder.CreateNot(right, "lnot");
    }
    
    throw YuxError("Unknown unary operator");
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
    else if (auto binOpNode = dynamic_cast<ExprBinOpNode*>(node)) {
        return compileBinOpExpr(binOpNode);
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
    else if (auto unaryNode = dynamic_cast<ExprUnaryNode*>(node)) {
        return compileUnaryExpr(unaryNode);
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

void Compiler::callDestructor(const string& varName, const TypeInfo& varType) {
    if (varType.isArray() || varType.isRef()) {
        return;
    }
    
    if (varType.isBox()) {
        auto it = _localVarPtrs.find(varName);
        if (it == _localVarPtrs.end()) {
            return;
        }
        
        DEBUG_LOG_VAL("  Calling Box destructor for", varName << " : " << varType.getFullName());
        
        auto boxStructType = getLLVMType(varType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
        
        llvm::Value* indices1[] = {zero, one};
        auto refCountFieldPtr = _builder.CreateGEP(boxStructType, it->second, indices1, "ref_count_field_ptr");
        auto refCountPtr = _builder.CreateLoad(llvm::PointerType::get(_builder.getInt64Ty(), 0), refCountFieldPtr, "ref_count_ptr");
        
        llvm::Value* indices2[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(boxStructType, it->second, indices2, "data_field_ptr");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_builder.getInt8Ty(), 0), dataFieldPtr, "data_ptr");
        
        auto releaseFn = getBoxReleaseFn();
        _builder.CreateCall(releaseFn, {refCountPtr, dataPtr});
        
        return;
    }
    
    if (varType.isArrayGeneric()) {
        auto it = _localVarPtrs.find(varName);
        if (it == _localVarPtrs.end()) {
            return;
        }
        
        DEBUG_LOG_VAL("  Calling Array destructor for", varName << " : " << varType.getFullName());
        
        auto arrayStructType = getLLVMType(varType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        
        llvm::Value* indices0[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, it->second, indices0, "data_field_ptr");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_builder.getInt8Ty(), 0), dataFieldPtr, "data_ptr");
        
        auto releaseFn = getArrayReleaseFn();
        _builder.CreateCall(releaseFn, {dataPtr});
        
        return;
    }
    
    auto structDecl = _file->getStructDecl(varType.name);
    if (!structDecl) {
        return;
    }
    
    auto structImpl = _file->getStructImpl(varType.name);
    if (!structImpl || !structImpl->hasDestructor()) {
        return;
    }
    
    auto it = _localVarPtrs.find(varName);
    if (it == _localVarPtrs.end()) {
        return;
    }
    
    DEBUG_LOG_VAL("  Calling destructor for", varName << " : " << varType.name);
    
    llvm::Value* selfPtr = it->second;
    
    auto destructorFn = getDestructorFunction(varType.name);
    
    _builder.CreateCall(destructorFn, {selfPtr});
}

void Compiler::callDestructorsForScope() {
    for (auto it = _scopeVars.rbegin(); it != _scopeVars.rend(); ++it) {
        const string& varName = *it;
        auto sym = _currentFnNode->lookupSymbol(varName);
        if (sym) {
            callDestructor(varName, sym->type);
        }
    }
}

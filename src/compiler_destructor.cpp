// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 析构函数编译实现
// 
// 本文件包含析构函数相关的编译逻辑:
// - 自动生成默认析构函数
// - 调用结构体字段的析构函数
// - 调用作用域内所有变量的析构函数
// - 检查类型是否需要析构函数

#include "compiler.h"
#include "mangler.h"
#include <llvm/IR/Instructions.h>

// ==================== 析构函数调用 ====================

// 调用单个变量的析构函数
// 如果变量类型有析构函数，则调用它
void Compiler::callDestructor(const string& varName, const TypeInfo& varType) {
    // 内置类型不需要析构函数
    if (isBuiltinType(varType.name)) return;

    // 引用类型不需要析构 (不拥有数据)
    if (varType.isRef()) return;

    // 指针类型不需要析构 (不拥有数据)
    if (varType.isPtr()) return;

    // 检查变量是否存在
    auto it = _localVarPtrs.find(varName);
    if (it == _localVarPtrs.end()) return;

    // 获取变量指针
    llvm::Value* varPtr = it->second;

    // 处理 Box<T> 类型 (智能指针)
    // Box 需要减少引用计数，如果计数为 0 则释放内存
    if (varType.isBox()) {
        auto elemType = varType.boxElementType();
        if (!elemType) return;

        DEBUG_LOG_VAL("  Calling Box destructor for", varName);

        // 获取 Box 结构体类型
        auto boxStructType = getLLVMType(varType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

        // 获取数据指针和引用计数指针
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataPtrField = _builder.CreateGEP(boxStructType, varPtr, dataIndices, "box.data_ptr_field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField, "box.data_ptr");

        llvm::Value* refCountIndices[] = {zero, one};
        auto refCountField = _builder.CreateGEP(boxStructType, varPtr, refCountIndices, "box.ref_count_field");
        auto refCountPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), refCountField, "box.ref_count_ptr");

        // 调用 Box 释放函数
        auto boxReleaseFn = runtime::getBoxReleaseFn(_module, _builder);
        _builder.CreateCall(boxReleaseFn, {dataPtr, refCountPtr});
        return;
    }

    // 处理 Array<T> 类型 (动态数组)
    // Array 需要释放数据内存
    if (varType.isArrayGeneric()) {
        DEBUG_LOG_VAL("  Calling Array destructor for", varName);

        auto arrayStructType = getLLVMType(varType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

        // 获取数据指针
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataPtrField = _builder.CreateGEP(arrayStructType, varPtr, dataIndices, "array.data_ptr_field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField, "array.data_ptr");

        // 调用 Array 释放函数
        auto arrayReleaseFn = runtime::getArrayReleaseFn(_module, _builder);
        _builder.CreateCall(arrayReleaseFn, {dataPtr});
        return;
    }

    // 处理结构体类型
    // 调用结构体的析构函数
    auto structDecl = _file->getStructDecl(varType.name);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(varType.name);
    }

    if (structDecl && structNeedsDestructor(varType.name)) {
        DEBUG_LOG_VAL("  Calling struct destructor for", varName << " : " << varType.name);

        // 获取析构函数
        auto dtorFn = getDestructorFunction(varType.name);
        if (dtorFn) {
            _builder.CreateCall(dtorFn, {varPtr});
        }
    }
}

// 调用当前作用域所有变量的析构函数
// 按照变量声明的逆序调用 (后进先出)
void Compiler::callDestructorsForScope() {
    // 逆序遍历作用域变量列表
    for (auto it = _scopeVars.rbegin(); it != _scopeVars.rend(); ++it) {
        auto varName = *it;
        auto sym = _currentFnNode->lookupSymbol(varName);
        if (sym) {
            callDestructor(varName, sym->type);
        }
    }
}

// 调用结构体字段的析构函数
// 用于结构体析构函数中，递归调用所有字段的析构函数
void Compiler::callFieldDestructor(llvm::Value* structPtr, const string& structName) {
    auto structDecl = _file->getStructDecl(structName);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(structName);
    }

    if (!structDecl) return;

    auto structType = getLLVMType(TypeInfo(structName));
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

    // 遍历所有字段
    for (size_t i = 0; i < structDecl->fields().size(); ++i) {
        auto field = structDecl->fields()[i];
        auto fieldType = field->getType();

        // 检查字段是否需要析构
        if (!typeNeedsDestructor(fieldType)) continue;

        // 获取字段指针
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, idx};
        auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "field.ptr");

        // 调用字段析构函数
        if (fieldType.isBox()) {
            // Box 字段: 调用 Box 释放函数
            auto boxStructType = getLLVMType(fieldType);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            llvm::Value* dataIndices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(boxStructType, fieldPtr, dataIndices);
            auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField);

            llvm::Value* refCountIndices[] = {zero, one};
            auto refCountField = _builder.CreateGEP(boxStructType, fieldPtr, refCountIndices);
            auto refCountPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), refCountField);

            auto boxReleaseFn = runtime::getBoxReleaseFn(_module, _builder);
            _builder.CreateCall(boxReleaseFn, {dataPtr, refCountPtr});
        } else if (fieldType.isArrayGeneric()) {
            // Array 字段: 调用 Array 释放函数
            auto arrayStructType = getLLVMType(fieldType);

            llvm::Value* dataIndices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(arrayStructType, fieldPtr, dataIndices);
            auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField);

            auto arrayReleaseFn = runtime::getArrayReleaseFn(_module, _builder);
            _builder.CreateCall(arrayReleaseFn, {dataPtr});
        } else if (!isBuiltinType(fieldType.name)) {
            // 结构体字段: 调用其析构函数
            auto fieldDtorsFn = getDestructorFunction(fieldType.name);
            if (fieldDtorsFn) {
                _builder.CreateCall(fieldDtorsFn, {fieldPtr});
            }
        }
    }
}

// ==================== 默认析构函数生成 ====================

// 为结构体生成默认析构函数
// 默认析构函数递归调用所有字段的析构函数
void Compiler::generateDefaultDestructor(const string& structName) {
    DEBUG_LOG_VAL("  Generating default destructor for", structName);

    // 获取或创建析构函数
    auto dtorFn = getDestructorFunction(structName);
    if (!dtorFn) return;

    // 创建入口基本块
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", dtorFn);
    _builder.SetInsertPoint(entry);

    // 获取 self 参数
    auto selfArg = &*dtorFn->arg_begin();

    // 调用字段析构函数
    callFieldDestructor(selfArg, structName);

    // 返回
    _builder.CreateRetVoid();
}

// ==================== 析构函数需求检查 ====================

// 检查类型是否需要析构函数
bool Compiler::typeNeedsDestructor(const TypeInfo& type) {
    // 内置类型不需要析构
    if (isBuiltinType(type.name)) return false;

    // 引用类型不需要析构 (不拥有数据)
    if (type.isRef()) return false;

    // 指针类型不需要析构 (不拥有数据)
    if (type.isPtr()) return false;

    // Box 和 Array 需要析构
    if (type.isBox() || type.isArrayGeneric()) return true;

    // 检查结构体是否需要析构
    return structNeedsDestructor(type.name);
}

// 检查结构体是否需要析构函数
// 如果结构体有任何需要析构的字段，则需要析构函数
bool Compiler::structNeedsDestructor(const string& structName) {
    auto structDecl = _file->getStructDecl(structName);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(structName);
    }

    if (!structDecl) return false;

    // 检查所有字段
    for (auto field : structDecl->fields()) {
        if (typeNeedsDestructor(field->getType())) {
            return true;
        }
    }

    return false;
}

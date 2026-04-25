// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 语句编译实现
// 
// 本文件包含所有语句类型的编译逻辑:
// - return 语句 (有返回值和无返回值)
// - 变量声明语句
// - 赋值语句 (普通赋值和复合赋值)
// - loop 循环语句
// - break 语句
// - 数组元素赋值语句

#include "compiler.h"
#include "node/statement_node.h"
#include "node/expr_node.h"
#include "compiler_runtime.h"
#include "mangler.h"
#include <algorithm>

// ==================== Return 语句编译 ====================

// 编译带返回值的 return 语句
// 检查返回类型是否匹配函数声明，调用析构函数后返回
void Compiler::compileRetStatement(p<StatementRetNode> node) {
    DEBUG_LOG("  Statement: Return");
    
    // 获取函数声明的返回类型
    TypeInfo declRetType;
    bool hasDeclaredRetType = false;
    if (_currentFnNode && _currentFnNode->header() && _currentFnNode->header()->retType()) {
        declRetType = _currentFnNode->header()->retType()->getType();
        hasDeclaredRetType = true;
        // 推断灵活整数的类型
        if (isIntTypeName(declRetType.name) && isFlexibleIntExpr(node->expr())) {
            tryInferIntType(node->expr(), declRetType);
        }
    }
    
    auto retType = node->expr()->getType();
    
    // 获取行号 (用于错误报告)
    int lineNum = node->getLineNumber();
    if (lineNum < 0) {
        lineNum = node->expr()->resolveLineNumber();
    }
    
    // 类型检查
    if (hasDeclaredRetType) {
        if (retType.empty()) {
            throw YuxError(lineNum, 
                "Function declares return type '{}', but returns void", 
                declRetType.getFullName());
        }
        if (retType != declRetType) {
            throw YuxError(lineNum,
                "Return type mismatch: function declares '{}', but expression has type '{}'",
                declRetType.getFullName(), retType.getFullName());
        }
    } else {
        if (!retType.empty()) {
            throw YuxError(lineNum,
                "Void function cannot return a value of type '{}'",
                retType.getFullName());
        }
    }
    
    // 编译返回值表达式
    llvm::Value* retVal = nullptr;
    if (retType.empty()) {
        compileExpr(node->expr());
        DEBUG_LOG("    Expression compiled as void return");
    } else {
        retVal = compileExpr(node->expr());
        DEBUG_LOG("    Created return value");
    }
    
    // 从作用域变量列表中移除返回的变量 (避免重复析构)
    if (auto litNode = dynamic_cast<ExprLiteralNode*>(node->expr())) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(litNode->literal())) {
            auto varName = objLit->getValue().getText();
            _scopeVars.erase(std::remove(_scopeVars.begin(), _scopeVars.end(), varName), _scopeVars.end());
        }
    }
    
    // 调用析构函数并返回
    callDestructorsForScope();
    if (retVal) {
        _builder.CreateRet(retVal);
        DEBUG_LOG("    Created return instruction");
    } else {
        _builder.CreateRetVoid();
        DEBUG_LOG("    Created void return instruction");
    }
}

// 编译无返回值的 return; 语句
void Compiler::compileRetVoidStatement(p<StatementRetVoidNode> node) {
    DEBUG_LOG("  Statement: Return Void");
    callDestructorsForScope();
    _builder.CreateRetVoid();
    DEBUG_LOG("    Created void return instruction");
}

// ==================== 变量声明语句编译 ====================

// 编译变量声明语句
// 处理普通变量、数组初始化、Box 类型、Array<T> 类型
void Compiler::compileDeclareAssignStatement(p<StatementDeclareAssignNode> node) {
    auto expr = node->expr();
    auto varName = node->name().getText();

    // 处理数组填充表达式 ([N; value] 语法)
    if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(expr)) {
        if (!node->varType()) {
            throw YuxError(node->getLineNumber(), "Array fill expression requires array type annotation with size");
        }

        TypeInfo varType = node->varType()->getType();
        if (!varType.isArray()) {
            throw YuxError(node->getLineNumber(), "Array fill expression requires array type annotation");
        }

        DEBUG_LOG_VAL("  Statement: Declare (ArrayFill)", varName << " : " << varType.name);

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _localVarPtrs[varName] = alloca;

        compileArrayInitExpr(arrayInitNode, varType, alloca);
    } else {
        // 普通变量声明
        TypeInfo varType;
        if (node->varType()) {
            varType = node->varType()->getType();
            // 推断灵活整数的类型
            if (isIntTypeName(varType.name) && isFlexibleIntExpr(expr)) {
                tryInferIntType(expr, varType);
            }
        } else {
            varType = expr->getType();
        }

        DEBUG_LOG_VAL("  Statement: Declare", varName << " : " << varType.name);

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _localVarPtrs[varName] = alloca;

        // 处理 Box<T> 类型 (堆分配的智能指针)
        if (varType.isBox()) {
            auto elemType = varType.boxElementType();
            if (!elemType) {
                throw YuxError(node->getLineNumber(), "Box type requires element type");
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            auto boxStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            // 分配堆内存
            auto elemLLVMType = getLLVMType(*elemType);
            auto sizeVal = _builder.CreateIntCast(
                _builder.getInt64(elemLLVMType->getPrimitiveSizeInBits() / 8),
                _builder.getInt64Ty(),
                false
            );

            auto allocFn = runtime::getBoxAllocFn(_module, _builder);
            auto dataPtr = _builder.CreateCall(allocFn, {sizeVal}, "box_data_ptr");

            auto dataPtrTyped = _builder.CreateBitCast(
                dataPtr, llvm::PointerType::get(_context, 0), "box_data_typed");

            // 存储值到堆内存
            if (exprType == *elemType) {
                _builder.CreateStore(exprVal, dataPtrTyped);
            } else if (exprType.isBox() && exprType.boxElementType() && *exprType.boxElementType() == *elemType) {
                // Box 到 Box 的复制
                auto srcBoxPtr = _localVarPtrs.find(varName);
                if (srcBoxPtr != _localVarPtrs.end()) {
                    auto srcDataPtrPtr = _builder.CreateGEP(boxStructType, exprVal, {zero, zero}, "src_data_ptr_ptr");
                    auto srcDataPtr = _builder.CreateLoad(
                        llvm::PointerType::get(_context, 0), srcDataPtrPtr, "src_data_ptr");
                    _builder.CreateStore(srcDataPtr, dataPtrTyped);
                }
            } else {
                throw YuxError(node->getLineNumber(), "Box type mismatch: expected Box<{}>, got {}", elemType->name, exprType.name);
            }

            // 设置引用计数指针
            auto refCountPtr = _builder.CreateGEP(
                _builder.getInt8Ty(),
                dataPtr,
                {_builder.getInt64(-8)},
                "ref_count_ptr_raw"
            );
            auto refCountPtrTyped = _builder.CreateBitCast(
                refCountPtr,
                llvm::PointerType::get(_context, 0),
                "ref_count_ptr"
            );

            // 初始化 Box 结构体字段
            llvm::Value* indices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(boxStructType, alloca, indices, "data_ptr_field");
            _builder.CreateStore(dataPtrTyped, dataPtrField);

            llvm::Value* indices2[] = {zero, one};
            auto refCountField = _builder.CreateGEP(boxStructType, alloca, indices2, "ref_count_field");
            _builder.CreateStore(refCountPtrTyped, refCountField);

            _scopeVars.push_back(varName);  // 加入作用域变量列表 (需要析构)
        } 
        // 处理 Array<T> 类型 (动态数组)
        else if (varType.isArrayGeneric()) {
            auto elemType = varType.arrayGenericElementType();
            if (!elemType) {
                throw YuxError(node->getLineNumber(), "Array type requires element type");
            }

            auto arrayStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
            auto two = llvm::ConstantInt::get(_builder.getInt32Ty(), 2);

            auto elemLLVMType = getLLVMType(*elemType);
            auto elemSize = elemLLVMType->getPrimitiveSizeInBits() / 8;

            // 处理数组字面量
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto& elements = arrayNode->elements();
                auto count = elements.size();

                llvm::Value* dataPtr = nullptr;
                if (count > 0) {
                    // 分配堆内存并初始化元素
                    auto totalSize = _builder.getInt64(count * elemSize);
                    auto allocFn = runtime::getArrayAllocFn(_module, _builder);
                    dataPtr = _builder.CreateCall(allocFn, {totalSize}, "array_data_ptr");

                    auto dataPtrTyped = _builder.CreateBitCast(
                        dataPtr, llvm::PointerType::get(_context, 0), "array_data_typed");

                    for (size_t i = 0; i < count; ++i) {
                        auto elemVal = compileExpr(elements[i]);
                        auto index = llvm::ConstantInt::get(_builder.getInt64Ty(), i);
                        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtrTyped, {index}, "elem.ptr");
                        _builder.CreateStore(elemVal, elemPtr);
                    }
                } else {
                    dataPtr = llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0));
                }

                // 初始化数组结构体字段
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
                // 从其他表达式初始化
                auto exprType = expr->getType();
                if (!exprType.isArrayGeneric() && exprType.name != "Array") {
                    throw YuxError(node->getLineNumber(), "Array<T> initialization requires Array<T> expression or array literal");
                }
                auto exprVal = compileExpr(expr);
                _builder.CreateStore(exprVal, alloca);
            }

            _scopeVars.push_back(varName);  // 加入作用域变量列表 (需要析构)
        } else {
            // 普通变量
            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            // 数组类型检查
            if (varType.isArray() && exprType.isArray()) {
                if (varType.arraySize != exprType.arraySize) {
                    throw YuxError(node->getLineNumber(), "Array size mismatch: expected {}, got {}", varType.arraySize, exprType.arraySize);
                }
                if (varType.elementType && exprType.elementType) {
                    if (*varType.elementType != *exprType.elementType) {
                        throw YuxError(node->getLineNumber(),
                            "Array element type mismatch: expected {}, got {}", varType.elementType->name,
                            exprType.elementType->name);
                    }
                }
            }

            _builder.CreateStore(exprVal, alloca);

            // 结构体类型需要加入作用域变量列表
            auto structDecl = _file->getStructDecl(varType.name);
            if (!structDecl && _yux) {
                structDecl = _yux->sdkFile()->getStructDecl(varType.name);
            }
            if (structDecl) {
                _scopeVars.push_back(varName);
            }
        }
    }
}

// ==================== 赋值语句编译 ====================

// 编译赋值语句
// 支持普通赋值和复合赋值 (+=, -=, *=, /=, %=, <<=, >>=)
void Compiler::compileAssignStatement(p<StatementAssignNode> node) {
    auto objName = node->obj().getText();
    auto expr = node->expr();
    auto& subs = node->subs();
    auto assignOp = node->op();

    // 辅助函数: 判断是否为浮点类型
    auto isFloatType = [](const TypeInfo& type) -> bool {
        return type.name == "f32" || type.name == "f64";
    };

    // 辅助函数: 判断是否为无符号类型
    auto isUnsignedType = [](const TypeInfo& type) -> bool {
        return type.name == "u8" || type.name == "u16" || type.name == "u32" || type.name == "u64";
    };

    // 辅助函数: 应用复合赋值运算符
    auto applyCompoundOp = [this, isFloatType, isUnsignedType](llvm::Value* currentVal, llvm::Value* exprVal, AssignOp op, const TypeInfo& type) -> llvm::Value* {
        switch (op) {
            case AssignOp::AddEq:
                if (isFloatType(type)) {
                    return _builder.CreateFAdd(currentVal, exprVal, "addtmp");
                }
                return _builder.CreateAdd(currentVal, exprVal, "addtmp");
            case AssignOp::SubEq:
                if (isFloatType(type)) {
                    return _builder.CreateFSub(currentVal, exprVal, "subtmp");
                }
                return _builder.CreateSub(currentVal, exprVal, "subtmp");
            case AssignOp::MulEq:
                if (isFloatType(type)) {
                    return _builder.CreateFMul(currentVal, exprVal, "multmp");
                }
                return _builder.CreateMul(currentVal, exprVal, "multmp");
            case AssignOp::DivEq:
                if (isFloatType(type)) {
                    return _builder.CreateFDiv(currentVal, exprVal, "divtmp");
                }
                if (isUnsignedType(type)) {
                    return _builder.CreateUDiv(currentVal, exprVal, "divtmp");
                }
                return _builder.CreateSDiv(currentVal, exprVal, "divtmp");
            case AssignOp::ModEq:
                if (isFloatType(type)) {
                    return _builder.CreateFRem(currentVal, exprVal, "modtmp");
                }
                if (isUnsignedType(type)) {
                    return _builder.CreateURem(currentVal, exprVal, "modtmp");
                }
                return _builder.CreateSRem(currentVal, exprVal, "modtmp");
            case AssignOp::MtMtEq:  // >>=
                return _builder.CreateAShr(currentVal, exprVal, "shrtmp");
            case AssignOp::LtLtEq:  // <<=
                return _builder.CreateShl(currentVal, exprVal, "shltmp");
            default:
                return exprVal;
        }
    };

    // 处理简单变量赋值 (无成员访问)
    if (subs.empty()) {
        auto sym = _currentFnNode->lookupSymbol(objName);
        if (!sym) {
            throw YuxError(node->getLineNumber(), "Undefined variable: {}", objName);
        }

        if (!sym->writeable) {
            throw YuxError(node->getLineNumber(), "Cannot assign to immutable variable: {}", objName);
        }

        DEBUG_LOG_VAL("  Statement: Assign", objName << " : " << sym->type.name);

        // 推断灵活整数的类型
        if (isIntTypeName(sym->type.name) && isFlexibleIntExpr(expr)) {
            tryInferIntType(expr, sym->type);
        }

        // 处理 Array<T> 空数组赋值
        if (sym->type.isArrayGeneric()) {
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                if (arrayNode->elements().empty()) {
                    auto arrayStructType = getLLVMType(sym->type);
                    auto it = _localVarPtrs.find(objName);
                    if (it != _localVarPtrs.end()) {
                        auto alloca = _builder.CreateAlloca(arrayStructType, nullptr, "empty_array_assign");
                        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

                        llvm::Value* dataIndices[] = {zero, zero};
                        auto dataPtrField = _builder.CreateGEP(arrayStructType, alloca, dataIndices, "data_ptr_field");

                        auto elemType = sym->type.arrayGenericElementType();
                        auto elemLLVMType = elemType ? getLLVMType(*elemType) : _builder.getInt8Ty();
                        _builder.CreateStore(
                            llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)), dataPtrField);

                        llvm::Value* lenIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 1)};
                        auto lenField = _builder.CreateGEP(arrayStructType, alloca, lenIndices, "len_field");
                        _builder.CreateStore(_builder.getInt64(0), lenField);

                        llvm::Value* capIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 2)};
                        auto capField = _builder.CreateGEP(arrayStructType, alloca, capIndices, "cap_field");
                        _builder.CreateStore(_builder.getInt64(0), capField);

                        auto emptyArrayVal = _builder.CreateLoad(arrayStructType, alloca, "empty_array.load");
                        _builder.CreateStore(emptyArrayVal, it->second);
                        return;
                    }
                }
            }
        }

        auto exprVal = compileExpr(expr);
        auto exprType = expr->getType();
        llvm::Value* valToStore;

        // 应用复合赋值或类型转换
        if (assignOp != AssignOp::Eq) {
            auto currentVal = _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName], "current.load");
            auto castedExprVal = createCast(exprVal, exprType, sym->type);
            valToStore = applyCompoundOp(currentVal, castedExprVal, assignOp, sym->type);
        } else {
            valToStore = createCast(exprVal, exprType, sym->type);
        }

        _builder.CreateStore(valToStore, _localVarPtrs[objName]);
    } else {
        // 处理成员访问赋值 (obj.field = value)
        auto sym = _currentFnNode->lookupSymbol(objName);
        if (!sym) {
            throw YuxError(node->getLineNumber(), "Undefined variable: {}", objName);
        }

        TypeInfo actualType = sym->type;
        if (sym->type.isRef()) {
            auto refElemType = sym->type.refElementType();
            if (refElemType) {
                actualType = *refElemType;
            }
        }

        auto structDecl = _file->getStructDecl(actualType.name);
        if (!structDecl && _yux && _yux->sdkFile()) {
            structDecl = _yux->sdkFile()->getStructDecl(actualType.name);
        }
        if (!structDecl) {
            throw YuxError(node->getLineNumber(), "Cannot access member on non-struct type: {}", actualType.name);
        }

        DEBUG_LOG_VAL("  Statement: MemberAssign", objName << "." << subs[0].getText());

        auto it = _localVarPtrs.find(objName);
        if (it == _localVarPtrs.end()) {
            throw YuxError(node->getLineNumber(), "Variable not found: {}", objName);
        }

        llvm::Value* structPtr = it->second;

        auto structType = getLLVMType(actualType);

        // 遍历成员访问链
        for (size_t i = 0; i < subs.size(); ++i) {
            auto memberName = subs[i].getText();
            int fieldIndex = structDecl->fieldIndex(memberName);
            if (fieldIndex < 0) {
                throw YuxError(node->getLineNumber(), "Struct {} has no field: {}", actualType.name, memberName);
            }

            auto field = structDecl->fields()[fieldIndex];
            // 检查私有字段访问权限
            if (field->isPrivate()) {
                string currentBase = _currentStructName;
                auto dollarPos = currentBase.find('$');
                if (dollarPos != string::npos) currentBase = currentBase.substr(0, dollarPos);
                if (currentBase != actualType.name) {
                    throw YuxError(node->getLineNumber(), "Cannot access private field '{}' of struct '{}'", memberName, actualType.name);
                }
            }

            if (i == subs.size() - 1) {
                // 最后一个成员: 执行赋值
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
                llvm::Value* indices[] = {zero, idx};

                auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "struct.field");
                auto fieldType = field->getType();

                // 处理 Array<T> 字段赋值
                if (fieldType.isArrayGeneric()) {
                    if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                        auto& elements = arrayNode->elements();
                        auto arrayStructType = getLLVMType(fieldType);
                        auto alloca = _builder.CreateAlloca(arrayStructType, nullptr, "array_field_tmp");

                        auto elemType = fieldType.arrayGenericElementType();
                        auto elemLLVMType = elemType ? getLLVMType(*elemType) : _builder.getInt8Ty();

                        if (elements.empty()) {
                            llvm::Value* dataIndices[] = {zero, zero};
                            auto dataPtrField = _builder.CreateGEP(
                                arrayStructType, alloca, dataIndices, "data_ptr_field");
                            _builder.CreateStore(
                                llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0)), dataPtrField);

                            llvm::Value* lenIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 1)};
                            auto lenField = _builder.CreateGEP(arrayStructType, alloca, lenIndices, "len_field");
                            _builder.CreateStore(_builder.getInt64(0), lenField);

                            llvm::Value* capIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 2)};
                            auto capField = _builder.CreateGEP(arrayStructType, alloca, capIndices, "cap_field");
                            _builder.CreateStore(_builder.getInt64(0), capField);
                        } else {
                            auto arrType = llvm::ArrayType::get(elemLLVMType, elements.size());
                            auto arrAlloca = _builder.CreateAlloca(arrType, nullptr, "arr_data");

                            for (size_t j = 0; j < elements.size(); ++j) {
                                auto elemVal = compileExpr(elements[j]);
                                llvm::Value* arrIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt64Ty(), j)};
                                auto elemPtr = _builder.CreateGEP(arrType, arrAlloca, arrIndices);
                                _builder.CreateStore(elemVal, elemPtr);
                            }

                            llvm::Value* dataIndices[] = {zero, zero};
                            auto dataPtrField = _builder.CreateGEP(
                                arrayStructType, alloca, dataIndices, "data_ptr_field");
                            auto arrPtr = _builder.CreateBitCast(arrAlloca, llvm::PointerType::get(_context, 0));
                            _builder.CreateStore(arrPtr, dataPtrField);

                            llvm::Value* lenIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 1)};
                            auto lenField = _builder.CreateGEP(arrayStructType, alloca, lenIndices, "len_field");
                            _builder.CreateStore(_builder.getInt64(elements.size()), lenField);

                            llvm::Value* capIndices[] = {zero, llvm::ConstantInt::get(_builder.getInt32Ty(), 2)};
                            auto capField = _builder.CreateGEP(arrayStructType, alloca, capIndices, "cap_field");
                            _builder.CreateStore(_builder.getInt64(elements.size()), capField);
                        }

                        auto arrayVal = _builder.CreateLoad(arrayStructType, alloca, "array.load");
                        _builder.CreateStore(arrayVal, fieldPtr);
                        return;
                    }
                }

                auto exprVal = compileExpr(expr);
                auto exprType = expr->getType();
                llvm::Value* valToStore;

                if (assignOp != AssignOp::Eq) {
                    auto fieldLLVMType = getLLVMType(fieldType);
                    auto currentVal = _builder.CreateLoad(fieldLLVMType, fieldPtr, "current.load");
                    auto castedExprVal = createCast(exprVal, exprType, fieldType);
                    valToStore = applyCompoundOp(currentVal, castedExprVal, assignOp, fieldType);
                } else {
                    valToStore = createCast(exprVal, exprType, fieldType);
                }

                _builder.CreateStore(valToStore, fieldPtr);
            } else {
                // TODO: 支持嵌套成员访问
                throw YuxError(node->getLineNumber(), "Nested member access not yet supported");
            }
        }
    }
}

// ==================== 循环语句编译 ====================

// 编译 loop 循环语句
// 生成无限循环结构，配合 break 语句使用
void Compiler::compileLoopStatement(p<StatementLoopNode> node) {
    DEBUG_LOG("  Statement: Loop");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    // 创建循环基本块: 条件块、循环体块、退出块
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(_context, "loop.cond");
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(_context, "loop.body");
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(_context, "loop.exit");

    // 跳转到条件块
    _builder.CreateBr(condBB);

    // 设置条件块: 无条件跳转到循环体
    func->insert(func->end(), condBB);
    _builder.SetInsertPoint(condBB);
    _builder.CreateBr(bodyBB);

    // 设置循环体块
    func->insert(func->end(), bodyBB);
    _builder.SetInsertPoint(bodyBB);

    // 将退出块压入栈 (供 break 使用)
    _loopExitBlocks.push_back(exitBB);

    // 编译循环体语句
    for (auto& stmt : node->block()->statements()) {
        compileStatement(stmt);
    }

    // 编译结果表达式 (如果有)
    if (node->block()->hasResult()) {
        compileExpr(node->block()->resultExpr());
    }

    // 移除退出块
    _loopExitBlocks.pop_back();

    // 无限循环: 跳回条件块
    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(condBB);
    }

    // 设置退出块
    func->insert(func->end(), exitBB);
    _builder.SetInsertPoint(exitBB);
}

// ==================== Break 语句编译 ====================

// 编译 break 语句
// 跳出当前循环
void Compiler::compileBreakStatement(p<StatementBreakNode> node) {
    DEBUG_LOG("  Statement: Break");

    // 检查是否在循环内
    if (_loopExitBlocks.empty()) {
        throw YuxError(node->getLineNumber(), "break statement not within a loop");
    }

    // 跳转到循环退出块
    llvm::BasicBlock* exitBB = _loopExitBlocks.back();
    _builder.CreateBr(exitBB);

    // 创建不可达基本块 (break 后的代码不应执行)
    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* unreachableBB = llvm::BasicBlock::Create(_context, "unreachable", func);
    _builder.SetInsertPoint(unreachableBB);
}

// ==================== 数组元素赋值语句编译 ====================

// 编译数组元素赋值语句 (arr[idx] = value)
// 支持固定大小数组、动态数组(Array<T>)、结构体字段中的数组
void Compiler::compileArraySetStatement(p<StatementSetNode> node) {
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        throw YuxError(node->getLineNumber(), "Array assignment requires at least one index");
    }

    DEBUG_LOG_VAL("  Statement: ArraySet", arrayType.name);

    // 获取数组指针
    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    // 处理简单变量访问
    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), "Array variable not found: {}", varName);
            }
            currentPtr = it->second;
        }
    } 
    // 处理成员访问 (obj.field[idx] = value)
    else if (auto dotExpr = dynamic_cast<ExprDotNode*>(arrayExpr)) {
        auto outerBase = dotExpr->baseExpr();
        auto outerType = outerBase->getType();
        TypeInfo outerActual = outerType;
        
        // 解引用类型
        if (outerType.isRef()) {
            auto t = outerType.refElementType();
            if (t) outerActual = *t;
        }
        if (outerType.isBox()) {
            auto t = outerType.boxElementType();
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
            int fi = outerStructDecl->fieldIndex(dotExpr->member());
            if (fi >= 0) {
                llvm::Value* dataPtr = outerPtr;
                auto zeroIdx = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                
                // Box 类型需要先解引用获取数据指针
                if (outerType.isBox()) {
                    auto boxStructType = getLLVMType(outerType);
                    llvm::Value* bIndices[] = {zeroIdx, zeroIdx};
                    auto dataPtrField = _builder.CreateGEP(
                        boxStructType, outerPtr, bIndices, "box.data_ptr_field");
                    dataPtr = _builder.CreateLoad(
                        llvm::PointerType::get(_context, 0), dataPtrField, "box.data_ptr");
                }
                
                auto outerLLVM = getLLVMType(outerActual);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fi);
                llvm::Value* indicesF[] = {zeroIdx, idx};
                currentPtr = _builder.CreateGEP(outerLLVM, dataPtr, indicesF, "array.field.ptr");
            }
        }
    }

    if (!currentPtr) {
        throw YuxError(node->getLineNumber(), "Array assignment requires a variable");
    }

    // 处理动态数组 Array<T>
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError(node->getLineNumber(), "Array type requires element type");
        }

        auto arrayStructType = getLLVMType(arrayType);
        auto elemLLVMType = getLLVMType(*elemType);

        // 获取数据指针字段
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* indices0[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, currentPtr, indices0, "array.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataFieldPtr, "array.data.ptr");

        // 计算元素地址并存储
        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        auto valueVal = compileExpr(node->valueExpr());
        _builder.CreateStore(valueVal, elemPtr);
        return;
    }

    // 处理固定大小数组 [N]T
    if (!arrayType.isArray()) {
        throw YuxError(node->getLineNumber(), "Cannot index non-array type: {}", arrayType.name);
    }

    // 支持多维数组索引
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

// ==================== 语句分发 ====================

// 编译语句的主入口
// 根据语句类型分发到对应的编译函数
void Compiler::compileStatement(p<StatementNode> node) {
    if (auto retNode = dynamic_cast<StatementRetNode*>(node)) {
        compileRetStatement(retNode);
    } else if (auto retVoidNode = dynamic_cast<StatementRetVoidNode*>(node)) {
        compileRetVoidStatement(retVoidNode);
    } else if (auto declareNode = dynamic_cast<StatementDeclareAssignNode*>(node)) {
        compileDeclareAssignStatement(declareNode);
    } else if (auto assignNode = dynamic_cast<StatementAssignNode*>(node)) {
        compileAssignStatement(assignNode);
    } else if (auto exprNode = dynamic_cast<StatementExprNode*>(node)) {
        // 表达式语句: 编译表达式并丢弃结果
        DEBUG_LOG("  Statement: Expression");
        compileExpr(exprNode->expr());
    } else if (auto loopNode = dynamic_cast<StatementLoopNode*>(node)) {
        compileLoopStatement(loopNode);
    } else if (auto breakNode = dynamic_cast<StatementBreakNode*>(node)) {
        compileBreakStatement(breakNode);
    } else if (auto setNode = dynamic_cast<StatementSetNode*>(node)) {
        compileArraySetStatement(setNode);
    } else {
        throw YuxError(node->getLineNumber(), "Unknown statement type");
    }
}

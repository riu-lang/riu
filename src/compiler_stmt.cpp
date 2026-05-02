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
    
    // 返回 Nullable<T>：允许 null 字面量或 T 值，自动包装
    bool nullableWrap = false;
    bool nullableWrapNullLit = false;
    if (hasDeclaredRetType && declRetType.isNullable()) {
        auto innerType = declRetType.nullableInnerType();
        if (innerType) {
            if (auto litWrap = dynamic_cast<ExprLiteralNode*>(node->expr())) {
                if (dynamic_cast<LiteralNullNode*>(litWrap->literal())) {
                    nullableWrap = true;
                    nullableWrapNullLit = true;
                }
            }
            if (!nullableWrap) {
                if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->expr())) {
                    tryInferIntType(node->expr(), *innerType);
                    retType = node->expr()->getType();
                }
                if (retType == *innerType) {
                    nullableWrap = true;
                }
            }
        }
    }

    // 类型检查
    if (hasDeclaredRetType) {
        if (retType.empty()) {
            throw YuxError(lineNum,
                "Function declares return type '{}', but returns void",
                declRetType.getFullName());
        }
        if (!nullableWrap && retType != declRetType) {
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
    } else if (nullableWrap) {
        auto innerType = declRetType.nullableInnerType();
        auto nullableStructType = getLLVMType(declRetType);
        auto innerLLVMType = getLLVMType(*innerType);
        auto tmp = _builder.CreateAlloca(nullableStructType, nullptr, "nullable_ret");
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
        llvm::Value* hasField = _builder.CreateGEP(nullableStructType, tmp, {zero, zero}, "nullable_has");
        llvm::Value* valueField = _builder.CreateGEP(nullableStructType, tmp, {zero, one}, "nullable_value");
        if (nullableWrapNullLit) {
            _builder.CreateStore(_builder.getInt1(false), hasField);
            _builder.CreateStore(llvm::Constant::getNullValue(innerLLVMType), valueField);
        } else {
            auto innerVal = compileExpr(node->expr());
            _builder.CreateStore(_builder.getInt1(true), hasField);
            _builder.CreateStore(innerVal, valueField);
        }
        retVal = _builder.CreateLoad(nullableStructType, tmp, "nullable_ret.load");
        DEBUG_LOG("    Created Nullable-wrapped return value");
    } else {
        retVal = compileExpr(node->expr());
        DEBUG_LOG("    Created return value");
    }
    
    // Phase 3b: move-return retain
    // 堆句柄返回类型（Box / Array / Weak）在返回前 retain 一次，配合 callee-clean
    // 局部变量 release（callDestructorsForScope）让调用方接住净 +1 句柄；
    // 不做 peephole（DRAFT §7.3）——纯局部 var 路径下 retain+release 互抵，函数调用
    // 临时值的多余 retain 由 Phase 8 临时值清单负责。
    // Phase 8c: fresh retVal（call/array literal）已自带 +1，跳过 retain
    bool didMoveRetainHandle = false;
    if (retVal && hasDeclaredRetType && !nullableWrap) {
        if (declRetType.isBox() || declRetType.isArrayGeneric() || declRetType.isWeak()) {
            if (!isFreshHandleExpr(node->expr())) {
                retainHandleAtCallSite(retVal, declRetType);
            } else {
                // Phase 8d.1: fresh 返回值的 +1 直接交给调用方，从临时帧消费
                consumeTemp(retVal);
            }
            didMoveRetainHandle = true;
        } else if (typeNeedsDestructor(declRetType) && isFreshHandleExpr(node->expr())) {
            // Phase 8e: fresh 含 RC 字段 struct value 返回（如 i64.to_string() 的 String）：
            // +1 直接交给调用方，从临时帧消费，避免 popAndReleaseTempFrame 调 dtor 双释放。
            consumeTemp(retVal);
        }
    }

    // 从作用域变量列表中移除返回的变量 (避免重复析构)
    // 仅对非堆句柄返回类型保留旧的 peephole；堆句柄走 Phase 3b retain + 析构 release
    if (!didMoveRetainHandle) {
        if (auto litNode = dynamic_cast<ExprLiteralNode*>(node->expr())) {
            if (auto objLit = dynamic_cast<LiteralObjNode*>(litNode->literal())) {
                auto varName = objLit->getValue().getText();
                _scopeVars.erase(std::remove(_scopeVars.begin(), _scopeVars.end(), varName), _scopeVars.end());
            }
        }
    }

    // Phase 8e: 在 CreateRet 之前释放本语句临时帧（fresh 返回值已被 consumeTemp 移除，
    // 帧里只剩中间 fresh 子表达式如 `ret f(make_aux())` 里的 make_aux）。
    // 否则 CreateRet 后 BB 终结，外层 compileStatement 的 popAndReleaseTempFrame
    // 会因 BB 已终结而早返、临时帧被静默丢弃 → leak。
    // pop+push 空帧保持栈平衡（外层 compileStatement 的 pop 仍能消掉本帧）。
    popAndReleaseTempFrame();
    pushTempFrame();

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
    // Phase 8e: 同上，先释放临时帧再 CreateRetVoid（pop+push 保持栈平衡）
    popAndReleaseTempFrame();
    pushTempFrame();
    callDestructorsForScope();
    _builder.CreateRetVoid();
    DEBUG_LOG("    Created void return instruction");
}

// ==================== 变量声明语句编译 ====================

// 编译变量声明语句（无初始化）
// 为变量分配栈空间，但不进行初始化
void Compiler::compileDeclareStatement(p<StatementDeclareNode> node) {
    auto varName = node->name().getText();
    TypeInfo varType = node->varType()->getType();

    DEBUG_LOG_VAL("  Statement: Declare (uninitialized)", varName << " : " << varType.name);

    auto llvmType = getLLVMType(varType);
    auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
    _localVarPtrs[varName] = alloca;

    // 对于需要析构的类型，加入作用域变量列表
    if (typeNeedsDestructor(varType)) {
        _scopeVars.push_back(varName);
    }
}

// 编译变量声明并赋值语句
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

        // Phase 4a: T& 借用局部变量；不分配独立 slot，直接绑到来源指针
        // expr 必须是 ExprGetRef（&x）或拷贝绑定 d2 = d（d 已为 Ref<T>）
        if (varType.isRef()) {
            auto innerType = varType.refElementType();
            if (!innerType) {
                throw YuxError(node->getLineNumber(), "Ref type requires element type");
            }
            llvm::Value* rhsPtr = nullptr;
            if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(expr)) {
                rhsPtr = compileGetRefExpr(getRefNode);
                // 校验 &expr 内层类型与声明 inner 一致
                auto innerOfGetRef = getRefNode->getType().refElementType();
                if (!innerOfGetRef || *innerOfGetRef != *innerType) {
                    throw YuxError(node->getLineNumber(),
                        "T& local initializer type mismatch: expected {}&, got {}&",
                        innerType->name,
                        innerOfGetRef ? innerOfGetRef->name : "?");
                }
            } else if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr); litExpr && dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                auto litObj = dynamic_cast<LiteralObjNode*>(litExpr->literal());
                auto srcName = litObj->getValue().getText();
                auto sym = _currentFnNode->lookupSymbol(srcName);
                if (!sym || !sym->type.isRef() || !sym->type.refElementType()
                    || *sym->type.refElementType() != *innerType) {
                    throw YuxError(node->getLineNumber(),
                        "T& copy-bind source type mismatch: '{}' is not {}&",
                        srcName, innerType->name);
                }
                auto it = _localVarPtrs.find(srcName);
                if (it == _localVarPtrs.end()) {
                    throw YuxError(node->getLineNumber(), "Cannot bind T& to non-local: {}", srcName);
                }
                rhsPtr = it->second;
            } else {
                throw YuxError(node->getLineNumber(),
                    "T& local initializer must be &expr or copy-bind from a T& variable");
            }
            _localVarPtrs[varName] = rhsPtr;
            return;
        }

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _localVarPtrs[varName] = alloca;

        // 处理 Box<T> 类型（Phase 1a 新布局：单 handle 指针 + Block 单分配）
        // Box 实例 = { handle: Block* }；Block = { u32 strong, u32 weak, payload }
        if (varType.isBox()) {
            auto elemType = varType.boxElementType();
            if (!elemType) {
                throw YuxError(node->getLineNumber(), "Box type requires element type");
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            auto boxStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto ptrTy = llvm::PointerType::get(_context, 0);

            if (exprType.isBox() && exprType.boxElementType() && *exprType.boxElementType() == *elemType) {
                // Box -> Box 复制：复制 handle 并 retain（DRAFT §7.3 callee-clean 还在 Phase 3，但句柄共享 retain 必须在 1a 启用）
                // exprVal 是源 Box 的 struct 值，先存 tmp alloca 才能 GEP 取 handle 字段
                auto tmpAlloca = _builder.CreateAlloca(boxStructType, nullptr, "box_src_tmp");
                _builder.CreateStore(exprVal, tmpAlloca);
                auto srcHandleField = _builder.CreateGEP(boxStructType, tmpAlloca, {zero, zero}, "src_handle_field");
                auto srcHandle = _builder.CreateLoad(ptrTy, srcHandleField, "src_handle");

                // 句柄复制 = retain（_box_retain 内部哨兵跳过 .rodata 字面量）
                // Phase 8b: fresh 来源（call/method/ctor 调用）已在 callee ret 处 move-return retain，跳过
                // Phase 8d.1: fresh 来源的 +1 转给新 var，从临时帧消费
                if (!isFreshHandleExpr(expr)) {
                    auto retainFn = runtime::getBoxRetainFn(_module, _builder);
                    _builder.CreateCall(retainFn, {srcHandle});
                } else {
                    consumeTemp(exprVal);
                }

                // 写入新 Box 的 handle 字段
                auto handleField = _builder.CreateGEP(boxStructType, alloca, {zero, zero}, "handle_field");
                _builder.CreateStore(srcHandle, handleField);
            } else if (exprType == *elemType) {
                // 由值构造 Box：分配 Block，把 payload 存入 block+8
                auto elemLLVMType = getLLVMType(*elemType);
                auto sizeVal = _builder.getInt64(elemLLVMType->getPrimitiveSizeInBits() / 8);

                auto allocFn = runtime::getBoxAllocFn(_module, _builder);
                auto block = _builder.CreateCall(allocFn, {sizeVal}, "box_block");

                // payload 起始 = block + 8
                auto payloadPtr = _builder.CreateGEP(_builder.getInt8Ty(), block, {_builder.getInt64(8)}, "box_payload");
                _builder.CreateStore(exprVal, payloadPtr);

                // 写 handle 字段
                auto handleField = _builder.CreateGEP(boxStructType, alloca, {zero, zero}, "handle_field");
                _builder.CreateStore(block, handleField);
            } else {
                throw YuxError(node->getLineNumber(), "Box type mismatch: expected Box<{}>, got {}", elemType->name, exprType.name);
            }

            _scopeVars.push_back(varName);  // 加入作用域变量列表 (需要析构)
        }
        // 处理 Weak<T> 类型（Phase 1d.2：支持从 Box<T> 或 Weak<T> 构造，weak++）
        else if (varType.isWeak()) {
            auto elemType = varType.weakElementType();
            if (!elemType) {
                throw YuxError(node->getLineNumber(), "Weak type requires element type");
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            bool fromBox = exprType.isBox() && exprType.boxElementType() && *exprType.boxElementType() == *elemType;
            bool fromWeak = exprType.isWeak() && exprType.weakElementType() && *exprType.weakElementType() == *elemType;
            if (!fromBox && !fromWeak) {
                throw YuxError(node->getLineNumber(),
                    "Weak<{}> 仅支持从 Box<{}> 或 Weak<{}> 构造", elemType->name, elemType->name, elemType->name);
            }

            auto srcStructType = getLLVMType(exprType);
            auto weakStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto i32Ty = _builder.getInt32Ty();
            auto i8Ty = _builder.getInt8Ty();

            // 取源 Box/Weak 的 handle（两者 layout 同形 { ptr handle }）
            auto tmpAlloca = _builder.CreateAlloca(srcStructType, nullptr, "weak_src_tmp");
            _builder.CreateStore(exprVal, tmpAlloca);
            auto srcHandleField = _builder.CreateGEP(srcStructType, tmpAlloca, {zero, zero}, "src_handle_field");
            auto srcHandle = _builder.CreateLoad(ptrTy, srcHandleField, "src_handle");

            // weak++（哨兵 / null 跳过）
            // Phase 8b: Weak-from-Weak fresh 源已 +1 weak（callee move-return retain 用 _weak_retain），跳过；
            // Box 源始终需要 weak++（不是 retain，是 Weak 句柄首次被引用，与 Box 的 strong 计数无关）
            bool needWeakInc = !(fromWeak && isFreshHandleExpr(expr));
            // Phase 8d.1: fromWeak fresh 路径直接接 +1 weak，从临时帧消费
            if (fromWeak && isFreshHandleExpr(expr)) {
                consumeTemp(exprVal);
            }
            if (needWeakInc) {
            auto incBB = llvm::BasicBlock::Create(_context, "weak_inc", _builder.GetInsertBlock()->getParent());
            auto checkBB = llvm::BasicBlock::Create(_context, "weak_check", _builder.GetInsertBlock()->getParent());
            auto doneBB = llvm::BasicBlock::Create(_context, "weak_inc_done", _builder.GetInsertBlock()->getParent());
            auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
            auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

            auto isNull = _builder.CreateICmpEQ(srcHandle, nullPtr, "is_null");
            _builder.CreateCondBr(isNull, doneBB, checkBB);

            _builder.SetInsertPoint(checkBB);
            auto strong = _builder.CreateLoad(i32Ty, srcHandle, "strong");
            auto isSentinel = _builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
            _builder.CreateCondBr(isSentinel, doneBB, incBB);

            _builder.SetInsertPoint(incBB);
            auto weakPtr = _builder.CreateGEP(i8Ty, srcHandle, {_builder.getInt64(4)}, "weak_ptr");
            auto weak = _builder.CreateLoad(i32Ty, weakPtr, "weak");
            auto newWeak = _builder.CreateAdd(weak, llvm::ConstantInt::get(i32Ty, 1), "new_weak");
            _builder.CreateStore(newWeak, weakPtr);
            _builder.CreateBr(doneBB);

            _builder.SetInsertPoint(doneBB);
            }

            // 写 Weak.handle 字段
            auto handleField = _builder.CreateGEP(weakStructType, alloca, {zero, zero}, "weak_handle_field");
            _builder.CreateStore(srcHandle, handleField);

            _scopeVars.push_back(varName);
        }
        // 处理 Array<T> 类型 (动态数组，Phase 1b 单 handle Block 布局)
        else if (varType.isArrayGeneric()) {
            auto elemType = varType.arrayGenericElementType();
            if (!elemType) {
                throw YuxError(node->getLineNumber(), "Array type requires element type");
            }

            auto elemLLVMType = getLLVMType(*elemType);
            auto ptrTy = llvm::PointerType::get(_context, 0);

            // 处理数组字面量：直接分配 Block + 写入元素 + 把 handle 存进新 alloca
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto& elements = arrayNode->elements();
                auto count = elements.size();
                auto countVal = _builder.getInt64(count);

                auto block = allocArrayBlock(elemLLVMType, countVal, countVal);
                if (count > 0) {
                    auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(block), "init.data");
                    for (size_t i = 0; i < count; ++i) {
                        auto elemVal = compileExpr(elements[i]);
                        auto idx = _builder.getInt64(i);
                        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "init.elem.ptr");
                        // Phase 3d: RC 元素从已有 var/field 读出 → 复制语义 retain
                        // Phase 8b: fresh 元素表达式（如 [make_box()]）已 +1，跳过 retain
                        // Phase 8d.1: fresh 元素从临时帧消费
                        if (typeNeedsDestructor(*elemType)) {
                            if (!isFreshHandleExpr(elements[i])) {
                                retainHandleAtCallSite(elemVal, *elemType);
                            } else {
                                consumeTemp(elemVal);
                            }
                        }
                        _builder.CreateStore(elemVal, elemPtr);
                    }
                }
                storeArrayHandle(alloca, block);
            } else {
                // 从其他 Array<T> 表达式初始化：句柄复制 + retain
                // 与 Box 的 var q = p 路径同形（Phase 1a），否则作用域结束 LIFO 双重 release
                // 触发同 handle freed-block read。修复 BUGS.md「Array 声明拷贝漏 retain」。
                // Phase 8b: fresh 来源（call/method 调用）已 move-return retain，跳过
                // Phase 8d.1: fresh 来源从临时帧消费
                auto exprType = expr->getType();
                if (!exprType.isArrayGeneric() && exprType.name != "Array") {
                    throw YuxError(node->getLineNumber(), "Array<T> initialization requires Array<T> expression or array literal");
                }
                auto exprVal = compileExpr(expr);
                if (!isFreshHandleExpr(expr)) {
                    retainHandleAtCallSite(exprVal, exprType);
                } else {
                    consumeTemp(exprVal);
                }
                _builder.CreateStore(exprVal, alloca);
            }

            _scopeVars.push_back(varName);  // 加入作用域变量列表 (需要析构)
        }
        // 处理 Nullable<T> 类型 (T? 的解糖)
        // 三种 RHS:
        //   1) null 字面量 → { _has=false, _value=zeroinit }
        //   2) T 值 → 隐式包装为 { _has=true, _value=expr }
        //   3) 已是 Nullable<T> → 整体结构体复制
        else if (varType.isNullable()) {
            auto innerType = varType.nullableInnerType();
            if (!innerType) {
                throw YuxError(node->getLineNumber(), "Nullable type requires inner type");
            }

            auto nullableStructType = getLLVMType(varType);
            auto innerLLVMType = getLLVMType(*innerType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            llvm::Value* hasField = _builder.CreateGEP(nullableStructType, alloca, {zero, zero}, "nullable_has");
            llvm::Value* valueField = _builder.CreateGEP(nullableStructType, alloca, {zero, one}, "nullable_value");

            // 是否是 null 字面量？
            bool isNullLit = false;
            if (auto litWrap = dynamic_cast<ExprLiteralNode*>(expr)) {
                if (dynamic_cast<LiteralNullNode*>(litWrap->literal())) {
                    isNullLit = true;
                }
            }

            if (isNullLit) {
                // null → _has=false, _value=zero
                _builder.CreateStore(_builder.getInt1(false), hasField);
                _builder.CreateStore(llvm::Constant::getNullValue(innerLLVMType), valueField);
            } else {
                // 触发 flexible int 推断（如 var x i32? = 5 中 5 推断为 i32）
                if (isIntTypeName(innerType->name) && isFlexibleIntExpr(expr)) {
                    tryInferIntType(expr, *innerType);
                }
                auto exprType = expr->getType();
                auto exprVal = compileExpr(expr);

                if (exprType.isNullable() && exprType == varType) {
                    // 整体复制 Nullable<T>
                    _builder.CreateStore(exprVal, alloca);
                } else if (exprType == *innerType) {
                    // 隐式包装：T → Nullable<T>
                    _builder.CreateStore(_builder.getInt1(true), hasField);
                    _builder.CreateStore(exprVal, valueField);
                } else {
                    throw YuxError(node->getLineNumber(),
                        "Cannot assign {} to Nullable<{}>",
                        exprType.name, innerType->name);
                }
            }
            _scopeVars.push_back(varName);
        }
        else {
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
                // Phase 8d.4: fresh 含 RC 字段 struct value（如 String = i64.to_string()）
                // 的 +1 已转给 var slot；从临时帧消费，避免帧弹出时再调 dtor 双释放
                if (typeNeedsDestructor(varType) && isFreshHandleExpr(expr)) {
                    consumeTemp(exprVal);
                }
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

        // Phase 4b: T& 赋值是 store-through（改被引对象），不是 rebind；
        // T& 形参 / val 局部 T& 的 writeable=false 不影响"写被引"，写权由源对象决定（4d 校验）
        if (!sym->writeable && !sym->type.isRef()) {
            throw YuxError(node->getLineNumber(), "Cannot assign to immutable variable: {}", objName);
        }

        DEBUG_LOG_VAL("  Statement: Assign", objName << " : " << sym->type.name);

        // Phase 4b: T& 赋值落 store-through 改被引对象（rebind 禁）
        // _localVarPtrs[objName] 持有底层 T 的地址（由声明 / 形参路径建立）
        if (sym->type.isRef()) {
            auto innerType = sym->type.refElementType();
            if (!innerType) {
                throw YuxError(node->getLineNumber(), "Ref type missing inner type");
            }
            llvm::Value* targetPtr = _localVarPtrs[objName];
            auto innerLLVMType = getLLVMType(*innerType);

            if (isIntTypeName(innerType->name) && isFlexibleIntExpr(expr)) {
                tryInferIntType(expr, *innerType);
            }
            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();
            llvm::Value* valToStore;
            if (assignOp != AssignOp::Eq) {
                auto currentVal = _builder.CreateLoad(innerLLVMType, targetPtr, "ref.current.load");
                auto castedExprVal = createCast(exprVal, exprType, *innerType);
                valToStore = applyCompoundOp(currentVal, castedExprVal, assignOp, *innerType);
            } else {
                valToStore = createCast(exprVal, exprType, *innerType);
            }
            _builder.CreateStore(valToStore, targetPtr);
            return;
        }

        // 推断灵活整数的类型
        if (isIntTypeName(sym->type.name) && isFlexibleIntExpr(expr)) {
            tryInferIntType(expr, sym->type);
        }

        // 处理 Array<T> 字面量赋值（包括空数组 = []）
        // Phase 3d: 新 handle 来自 _array_alloc（strong=1），无需 retain；旧 handle 必须 release
        if (sym->type.isArrayGeneric()) {
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto it = _localVarPtrs.find(objName);
                if (it != _localVarPtrs.end()) {
                    auto& elements = arrayNode->elements();
                    auto count = elements.size();
                    auto elemType = sym->type.arrayGenericElementType();
                    auto elemLLVMType = elemType ? getLLVMType(*elemType) : _builder.getInt8Ty();
                    auto ptrTy = llvm::PointerType::get(_context, 0);

                    auto block = allocArrayBlock(elemLLVMType, _builder.getInt64(count), _builder.getInt64(count));
                    if (count > 0) {
                        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(block), "assign.data");
                        for (size_t i = 0; i < count; ++i) {
                            auto elemVal = compileExpr(elements[i]);
                            auto idx = _builder.getInt64(i);
                            auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "assign.elem.ptr");
                            // Phase 8b: fresh 元素表达式跳过 retain
                            // Phase 8d.1: fresh 元素从临时帧消费
                            if (typeNeedsDestructor(*elemType)) {
                                if (!isFreshHandleExpr(elements[i])) {
                                    retainHandleAtCallSite(elemVal, *elemType);
                                } else {
                                    consumeTemp(elemVal);
                                }
                            }
                            _builder.CreateStore(elemVal, elemPtr);
                        }
                    }
                    // Phase 3d: 释放旧 handle 后再写入新 handle
                    releaseAtPtr(it->second, sym->type);
                    storeArrayHandle(it->second, block);
                    return;
                }
            }
        }

        // Nullable<T> 赋值：与 compileVarStatement 的初始化路径保持一致
        // 三种 RHS:
        //   1) null 字面量 → { _has=false, _value=zeroinit }
        //   2) T 值 → 隐式包装为 { _has=true, _value=expr }
        //   3) 已是 Nullable<T> → 整体结构体复制
        if (assignOp == AssignOp::Eq && sym->type.isNullable()) {
            auto innerType = sym->type.nullableInnerType();
            if (!innerType) {
                throw YuxError(node->getLineNumber(), "Nullable type requires inner type");
            }

            auto nullableStructType = getLLVMType(sym->type);
            auto innerLLVMType = getLLVMType(*innerType);
            auto alloca = _localVarPtrs[objName];
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            llvm::Value* hasField = _builder.CreateGEP(nullableStructType, alloca, {zero, zero}, "nullable_has");
            llvm::Value* valueField = _builder.CreateGEP(nullableStructType, alloca, {zero, one}, "nullable_value");

            bool isNullLit = false;
            if (auto litWrap = dynamic_cast<ExprLiteralNode*>(expr)) {
                if (dynamic_cast<LiteralNullNode*>(litWrap->literal())) {
                    isNullLit = true;
                }
            }

            if (isNullLit) {
                _builder.CreateStore(_builder.getInt1(false), hasField);
                _builder.CreateStore(llvm::Constant::getNullValue(innerLLVMType), valueField);
            } else {
                if (isIntTypeName(innerType->name) && isFlexibleIntExpr(expr)) {
                    tryInferIntType(expr, *innerType);
                }
                auto exprType = expr->getType();
                auto exprVal = compileExpr(expr);

                if (exprType.isNullable() && exprType == sym->type) {
                    _builder.CreateStore(exprVal, alloca);
                } else if (exprType == *innerType) {
                    _builder.CreateStore(_builder.getInt1(true), hasField);
                    _builder.CreateStore(exprVal, valueField);
                } else {
                    throw YuxError(node->getLineNumber(),
                        "Cannot assign {} to Nullable<{}>",
                        exprType.name, innerType->name);
                }
            }
            return;
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

        // Phase 3d: RC 类型 / 含 RC 字段 struct 的赋值 → retain new → release old → store
        // 自赋值 / 别名安全：先 retain 再 release，避免计数过早归零
        // Phase 8b: fresh 来源（call/method/ctor 调用）已 +1，跳过 retain；旧值仍需 release
        // Phase 8d.1: fresh 来源从临时帧消费
        if (assignOp == AssignOp::Eq && typeNeedsDestructor(sym->type)) {
            if (!isFreshHandleExpr(expr)) {
                retainHandleAtCallSite(valToStore, sym->type);
            } else {
                consumeTemp(valToStore);
            }
            releaseAtPtr(_localVarPtrs[objName], sym->type);
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

                // 处理 Array<T> 字段赋值（Phase 1b：分配 Block 并把 handle 写入字段）
                // Phase 3d: 新 handle 来自 _array_alloc（strong=1），无需 retain；旧 handle 必须 release
                if (fieldType.isArrayGeneric()) {
                    if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                        auto& elements = arrayNode->elements();
                        auto count = elements.size();
                        auto elemType = fieldType.arrayGenericElementType();
                        auto elemLLVMType = elemType ? getLLVMType(*elemType) : _builder.getInt8Ty();
                        auto ptrTy = llvm::PointerType::get(_context, 0);

                        auto block = allocArrayBlock(elemLLVMType, _builder.getInt64(count), _builder.getInt64(count));
                        if (count > 0) {
                            auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(block), "field.data");
                            for (size_t j = 0; j < count; ++j) {
                                auto elemVal = compileExpr(elements[j]);
                                auto idx = _builder.getInt64(j);
                                auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "field.elem.ptr");
                                // Phase 8b: fresh 元素表达式跳过 retain
                                // Phase 8d.1: fresh 元素从临时帧消费
                                if (typeNeedsDestructor(*elemType)) {
                                    if (!isFreshHandleExpr(elements[j])) {
                                        retainHandleAtCallSite(elemVal, *elemType);
                                    } else {
                                        consumeTemp(elemVal);
                                    }
                                }
                                _builder.CreateStore(elemVal, elemPtr);
                            }
                        }
                        // Phase 3d: 释放旧 handle 后再写入新 handle
                        releaseAtPtr(fieldPtr, fieldType);
                        // fieldPtr 指向 Array<T> 实例（{ ptr handle }）；handle 在 offset 0
                        storeArrayHandle(fieldPtr, block);
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

                // Phase 3d: RC 字段 / 含 RC 字段 struct 字段 → retain new → release old → store
                // Phase 8b: fresh 来源跳过 retain
                // Phase 8d.1: fresh 来源从临时帧消费
                if (assignOp == AssignOp::Eq && typeNeedsDestructor(fieldType)) {
                    if (!isFreshHandleExpr(expr)) {
                        retainHandleAtCallSite(valToStore, fieldType);
                    } else {
                        consumeTemp(valToStore);
                    }
                    releaseAtPtr(fieldPtr, fieldType);
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

    // 处理动态数组 Array<T>（Phase 1b：经由 handle 间接访问 Block.data）
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError(node->getLineNumber(), "Array type requires element type");
        }

        auto elemLLVMType = getLLVMType(*elemType);
        auto handle = loadArrayHandle(currentPtr);
        auto dataPtr = _builder.CreateLoad(
            llvm::PointerType::get(_context, 0), arrayBlockDataFieldPtr(handle), "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        auto valueVal = compileExpr(node->valueExpr());

        // Phase 3d: RC 元素 / 含 RC 字段 struct 元素 → retain new → release old → store
        // Phase 8b: fresh 来源跳过 retain
        // Phase 8d.1: fresh 来源从临时帧消费
        if (typeNeedsDestructor(*elemType)) {
            if (!isFreshHandleExpr(node->valueExpr())) {
                retainHandleAtCallSite(valueVal, *elemType);
            } else {
                consumeTemp(valueVal);
            }
            releaseAtPtr(elemPtr, *elemType);
        }

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
    // Phase 8d.1: 入口 push 临时帧；分发完成后 pop+release 未消费的 fresh RC 句柄
    pushTempFrame();

    if (auto retNode = dynamic_cast<StatementRetNode*>(node)) {
        compileRetStatement(retNode);
    } else if (auto retVoidNode = dynamic_cast<StatementRetVoidNode*>(node)) {
        compileRetVoidStatement(retVoidNode);
    } else if (auto declareNode = dynamic_cast<StatementDeclareNode*>(node)) {
        compileDeclareStatement(declareNode);
    } else if (auto declareAssignNode = dynamic_cast<StatementDeclareAssignNode*>(node)) {
        compileDeclareAssignStatement(declareAssignNode);
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
        popAndReleaseTempFrame();
        throw YuxError(node->getLineNumber(), "Unknown statement type");
    }

    popAndReleaseTempFrame();
}

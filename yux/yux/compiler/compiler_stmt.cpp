// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 语句编译实现
//
// 本文件包含所有语句类型的编译逻辑:
// - return 语句 (有返回值和无返回值)
// - 变量声明语句
// - 赋值语句 (普通赋值和复合赋值)
// - loop / for-in 循环语句
// - break / continue 语句
// - 数组元素赋值语句

#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/statement_node.h"
#include "compiler.h"
#include "compiler_runtime.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <memory>

// ==================== Return 语句编译 ====================

// 编译带返回值的 return 语句
// 检查返回类型是否匹配函数声明，调用析构函数后返回
void Compiler::compileRetStatement(p<StatementRetNode> node) {
    DEBUG_LOG("  Statement: Return");

    // 获取函数声明的返回类型（lambda 体内用 lambda 自身的 Ret，不用外层 fn）
    TypeInfo declRetType;
    bool hasDeclaredRetType = false;
    if (_currentLambdaForCapture) {
        if (_currentLambdaForCapture->retType()) {
            declRetType = _currentLambdaForCapture->retType()->getType();
            hasDeclaredRetType = true;
        } else {
            auto ft = _currentLambdaForCapture->getType();
            if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->empty()) {
                declRetType = ft.fnReturnType()->withoutFallible();
                hasDeclaredRetType = true;
            }
        }
        if (hasDeclaredRetType) {
            if (isIntTypeName(declRetType.name) && isFlexibleIntExpr(node->expr())) {
                tryInferIntType(node->expr(), declRetType);
            }
            inferFlexibleInts(node->expr(), declRetType);
        }
    } else if (_currentFnNode && _currentFnNode->header() && _currentFnNode->header()->retType()) {
        declRetType = _currentFnNode->header()->retType()->getType();
        hasDeclaredRetType = true;
        // 推断灵活整数的类型
        if (isIntTypeName(declRetType.name) && isFlexibleIntExpr(node->expr())) {
            tryInferIntType(node->expr(), declRetType);
        }
        // 推断 tuple 字面量中灵活整数的类型（含泛型别名展开）
        inferFlexibleInts(node->expr(), declRetType);
    }

    TypeInfo retType = node->expr()->getType();
    // 数组字面量 getType 是 `[T * N]` / `[__empty * 0]`；靶向 Array<T> 时用 resolved / 声明类型。
    if (node->expr()->hasResolvedType()) {
        const auto& resolved = node->expr()->resolvedType();
        if (resolved.isArrayGeneric() && retType.isArray()) {
            retType = resolved;
        }
    }
    if (hasDeclaredRetType && declRetType.isArrayGeneric()) {
        if (auto arr = dynamic_cast<ExprArrayNode*>(node->expr())) {
            if (arr->elements().empty()) {
                arr->setResolvedType(declRetType);
                retType = declRetType;
            }
        }
    }

    // ==================== #Fallible(E) 错误返回路径（DRAFT-错误.md [#10.A] / [#10.B]） ====================
    // 当前 fn 标 #Fallible(E) 时，函数 LLVM 返回类型已被 wrapFallibleRetType 包成
    //   { i1 isErr, T_ok?, ErrEnum }（10g-2）。本段处理 ret 的两条分流：
    //   - ret expr，expr 类型 == declRetType  → 成功路径：构 { false, expr_val, zero(ErrEnum) }
    //   - ret expr，expr 类型 == fallibleErr  → 错误路径：构 { true,  zero(T_ok), expr_val  }
    //   - 都不匹配 → 复用既有 E3020
    // 析构序与成功路径完全一致（[#10.B] U1）：构 retStruct 后调 callDestructorsForScope。
    string fallibleErrName;
    if (_currentFnNode && _currentFnNode->header()) {
        fallibleErrName = _currentFnNode->header()->resolvedFallibleErr();
    } else if (_currentLambdaForCapture) {
        if (_currentLambdaForCapture->fallibleErrTypeNode()) {
            fallibleErrName = _currentLambdaForCapture->fallibleErrTypeNode()->getType().name;
        } else {
            auto ft = _currentLambdaForCapture->getType();
            if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->fallibleErr.empty()) {
                fallibleErrName = ft.fnReturnType()->fallibleErr;
            }
        }
    }
    if (!fallibleErrName.empty()) {
        // 灵活整数：成功通道按 declRetType 推断（与下方非 Fallible 路径同型）
        if (hasDeclaredRetType && isIntTypeName(declRetType.name) && isFlexibleIntExpr(node->expr())) {
            tryInferIntType(node->expr(), declRetType);
            retType = node->expr()->getType();
        }
        bool isSuccess = hasDeclaredRetType && (resolveAlias(retType) == resolveAlias(declRetType));
        bool isError = (resolveAlias(retType).name == fallibleErrName);
        if (!isSuccess && !isError) {
            int ln = node->getLineNumber();
            if (ln < 0) ln = node->expr()->resolveLineNumber();
            // fallible void：`ret <void-expr>` 与 `ret;` 同义（先求值再成功-void 返回）。
            if (!hasDeclaredRetType && retType.empty()) {
                compileExpr(node->expr());
                popAndReleaseTempFrame();
                pushTempFrame();
                callDestructorsForScope();
                auto retStructTy = getFallibleRetStructType(TypeInfo(), fallibleErrName);
                TypeInfo voidErrType(fallibleErrName);
                auto errLLVMTy = getLLVMType(voidErrType);
                llvm::Value* retStruct = llvm::UndefValue::get(retStructTy);
                retStruct = _builder.CreateInsertValue(retStruct, _builder.getInt1(false), {0});
                retStruct = _builder.CreateInsertValue(retStruct, llvm::Constant::getNullValue(errLLVMTy), {1});
                _builder.CreateRet(retStruct);
                DEBUG_LOG("    Created #Fallible void-success return from void expr");
                return;
            }
            throwSemaGap(ln);
        }
        // 求值表达式（错误 / 成功通道复用现有 enum / value 求值路径）
        llvm::Value* val = compileExpr(node->expr());

        // Phase 8e: 同既有路径，先释放临时帧再构 retStruct + CreateRet
        bool didMoveRetainHandle = false;
        if (isSuccess && hasDeclaredRetType) {
            didMoveRetainHandle =
                returnValue(val, declRetType, node->expr(), declRetType.isRc() || declRetType.isWeak());
        }
        // 错误通道：ErrEnum payload 由 enum 构造路径自带 +1（[#10.C]），不重复 retain；
        // 但 fresh enum value 需要 consumeTemp 避免双析构（与 success 同型）。
        if (isError) {
            returnValue(val, retType, node->expr(), false);
        }
        if (!didMoveRetainHandle) {
            if (auto litNode = dynamic_cast<ExprLiteralNode*>(node->expr())) {
                if (auto objLit = dynamic_cast<LiteralObjNode*>(litNode->literal())) {
                    auto varName = objLit->getValue().getText();
                    eraseScopeVar(varName);
                }
            }
        }

        // 构 retStruct
        TypeInfo successType = hasDeclaredRetType ? declRetType : TypeInfo();
        TypeInfo errTy(fallibleErrName);
        auto retStructTy = getFallibleRetStructType(successType, fallibleErrName);
        auto errLLVMTy = getLLVMType(errTy);
        llvm::Value* retStruct = llvm::UndefValue::get(retStructTy);
        retStruct = _builder.CreateInsertValue(retStruct, _builder.getInt1(isError ? true : false), {0});
        unsigned errFieldIdx;
        if (!successType.empty()) {
            // T_ok 在字段 1，ErrEnum 在字段 2
            auto okLLVMTy = getLLVMType(successType);
            llvm::Value* okSlot = isSuccess ? val : llvm::Constant::getNullValue(okLLVMTy);
            retStruct = _builder.CreateInsertValue(retStruct, okSlot, {1});
            errFieldIdx = 2;
        } else {
            errFieldIdx = 1;
        }
        llvm::Value* errSlot = isError ? val : llvm::Constant::getNullValue(errLLVMTy);
        retStruct = _builder.CreateInsertValue(retStruct, errSlot, {errFieldIdx});

        popAndReleaseTempFrame();
        pushTempFrame();
        callDestructorsForScope();
        _builder.CreateRet(retStruct);
        DEBUG_LOG("    Created #Fallible return instruction");
        return;
    }

    // 返回 T&（spec §6.3.X / §8.6.X）：表达式上下文 T& 变量会自动解引用为 T，
    // 这里识别 T& 上下文并按"原始借用"路径取指针，跳过 RC / nullable 包装。
    if (hasDeclaredRetType && declRetType.isRef()) {
        // 解析 Self → 实际结构体名（方法上下文）；不动其他类型替换以免影响泛型场景。
        declRetType = applySubst(declRetType);
        int lineNum = node->getLineNumber();
        if (lineNum < 0) lineNum = node->expr()->resolveLineNumber();
        // 仅校验来源形态合法（borrow_checker 已做溯源）：要求 expr 是
        //   - LiteralObjNode("$")            —— 方法返回 Self& 直接 ret $
        //   - LiteralObjNode(name)，sym.type.isRef() —— ret 一个 T& 形参/局部
        //   - ExprGetRefNode               —— ret &x.f.f...
        //   - 任何 expr.getType() == declRetType 的形态（如调用返 T&、as_ref(box)）
        llvm::Value* refPtr = nullptr;
        TypeInfo srcInner;
        if (auto litExpr = dynamic_cast<ExprLiteralNode*>(node->expr())) {
            if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                auto vname = objLit->getValue().getText();
                auto sym = lookupVarSymbol(vname, node);
                bool isDollar = (vname == "$");
                bool isRefVar = sym && sym->type.isRef();
                if ((isDollar || isRefVar) && _localVarPtrs.contains(vname)) {
                    refPtr = _localVarPtrs[vname];
                    if (isDollar) {
                        srcInner = sym ? sym->type : TypeInfo();
                        if (srcInner.isRef()) {
                            if (auto in = srcInner.refElementType()) srcInner = *in;
                        }
                    } else if (auto in = sym->type.refElementType()) {
                        srcInner = *in;
                    }
                }
            }
        }
        if (!refPtr) {
            if (auto getRef = dynamic_cast<ExprGetRefNode*>(node->expr())) {
                refPtr = compileGetRefExpr(static_cast<p<ExprGetRefNode>>(getRef));
                srcInner = applySubst(getRef->getType());
                if (srcInner.isRef()) {
                    if (auto in = srcInner.refElementType()) srcInner = *in;
                }
            }
        }
        if (!refPtr) {
            // 通用路径：表达式自身类型就是 T&（如调用返 T&、as_ref）。compileExpr 在 T& 类型下
            // 应当返回指针；目前 LiteralObj 路径会自动解引用，不在此分支命中。
            if (retType.isRef()) {
                refPtr = compileExpr(node->expr());
                auto resolvedRet = applySubst(retType);
                if (auto in = resolvedRet.refElementType()) srcInner = *in;
            }
        }
        if (!refPtr) {
            throwSemaGap(lineNum);
        }
        // 内层类型校验：declRetType 的 inner 必须等于 srcInner（v1 不做协变）。
        // 方法上下文中 `Self` 解析为当前结构体名（applySubst 不覆盖该映射）。
        auto declInner = declRetType.refElementType();
        TypeInfo declInnerResolved = declInner ? *declInner : TypeInfo();
        if (!_currentStructName.empty() && declInnerResolved.isSelf()) {
            declInnerResolved.name = _currentStructName;
        }
        if (declInner && !srcInner.empty() && declInnerResolved != srcInner) {
            throwSemaGap(lineNum);
        }
        popAndReleaseTempFrame();
        pushTempFrame();
        callDestructorsForScope();
        _builder.CreateRet(refPtr);
        DEBUG_LOG("    Created T& return instruction");
        return;
    }

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
            if (isFlexibleNullExpr(node->expr())) {
                nullableWrap = true;
                nullableWrapNullLit = true;
            }
            if (!nullableWrap) {
                if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->expr())) {
                    tryInferIntType(node->expr(), *innerType);
                    retType = node->expr()->getType();
                }
                if (resolveAlias(retType) == resolveAlias(*innerType)) {
                    nullableWrap = true;
                }
            }
        }
    }

    // 类型检查
    if (hasDeclaredRetType) {
        if (retType.empty()) {
            throwSemaGap(lineNum);
        }
        if (!nullableWrap && resolveAlias(retType) != resolveAlias(declRetType)) {
            throwSemaGap(lineNum);
        }
    } else {
        if (!retType.empty()) {
            throwSemaGap(lineNum);
        }
    }

    // 编译返回值表达式
    llvm::Value* retVal = nullptr;
    const bool declIsUnit = hasDeclaredRetType && declRetType.isUnit();
    if (retType.empty() || declIsUnit) {
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

    // Phase 3b: move-return — Rc/Weak/Fn 走 takeOwnership；其余需析构且 fresh 只 consumeTemp
    bool didMoveRetainHandle = false;
    if (retVal && hasDeclaredRetType && !nullableWrap) {
        didMoveRetainHandle = returnValue(retVal, declRetType, node->expr(),
                                          declRetType.isRc() || declRetType.isWeak() || declRetType.isFn());
    }

    // 从作用域变量列表中移除返回的变量 (避免重复析构)
    // 仅对非堆句柄返回类型保留旧的 peephole；堆句柄走 Phase 3b retain + 析构 release
    if (!didMoveRetainHandle) {
        if (auto litNode = dynamic_cast<ExprLiteralNode*>(node->expr())) {
            if (auto objLit = dynamic_cast<LiteralObjNode*>(litNode->literal())) {
                auto varName = objLit->getValue().getText();
                eraseScopeVar(varName);
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
    if (_currentLambdaForCapture) {
        auto ft = _currentLambdaForCapture->getType();
        if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->empty()) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
    }
    // Phase 8e: 同上，先释放临时帧再 CreateRetVoid（pop+push 保持栈平衡）
    popAndReleaseTempFrame();
    pushTempFrame();
    callDestructorsForScope();
    // #Fallible(E) 函数体内 `ret;` 表示成功-void 通道（[#10.A] T_ok=void）：
    // 构 { false, zero(ErrEnum) }（字段 0 = isErr, 字段 1 = ErrEnum）
    string fallibleErrName;
    if (_currentFnNode && _currentFnNode->header()) {
        fallibleErrName = _currentFnNode->header()->resolvedFallibleErr();
    } else if (_currentLambdaForCapture) {
        if (_currentLambdaForCapture->fallibleErrTypeNode()) {
            fallibleErrName = _currentLambdaForCapture->fallibleErrTypeNode()->getType().name;
        } else {
            auto ft = _currentLambdaForCapture->getType();
            if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->fallibleErr.empty()) {
                fallibleErrName = ft.fnReturnType()->fallibleErr;
            }
        }
    }
    if (!fallibleErrName.empty()) {
        auto retStructTy = getFallibleRetStructType(TypeInfo(), fallibleErrName);
        TypeInfo voidErrType(fallibleErrName);
        auto errLLVMTy = getLLVMType(voidErrType);
        llvm::Value* retStruct = llvm::UndefValue::get(retStructTy);
        retStruct = _builder.CreateInsertValue(retStruct, _builder.getInt1(false), {0});
        retStruct = _builder.CreateInsertValue(retStruct, llvm::Constant::getNullValue(errLLVMTy), {1});
        _builder.CreateRet(retStruct);
        DEBUG_LOG("    Created #Fallible void-success return instruction");
        return;
    }
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
    // Phase 3a: 需要析构的类型未初始化时零填充，让析构期指针字段为 null（release 函数 null 安全早返）
    // 否则栈上指针字段为垃圾，析构读到非 null 指针即段错（如 BUG2：Rc<fn> 2+ 同作用域）。
    // 需要 resolveAlias：类型别名（如 Callback = fn(s String)bool）底层的 LLVM 类型含指针，
    // 不解别名会导致零初始化被跳过 → 后续 retain/release 段错。
    if (typeNeedsDestructor(resolveAlias(varType))) {
        _builder.CreateStore(llvm::Constant::getNullValue(llvmType), alloca);
    }

    registerLocalVar(varName, alloca, varType);
}

// 编译变量声明并赋值语句
// 处理普通变量、数组初始化、Rc 类型、Array<T> 类型
void Compiler::compileDeclareAssignStatement(p<StatementDeclareAssignNode> node) {
    auto expr = node->expr();
    auto varName = node->name().getText();

    // Phase 4c：lambda 字面量直接作 var/val 初始化值时，反推 fn 类型到 lambda
    // 以支持 0 参块 / 缺标注 lambda 的 retType 上下文反推。
    if (auto litLambda = dynamic_cast<LambdaExprNode*>(expr)) {
        // 显式 fn 类型反推到 lambda，让 0 参块 / 缺标注 lambda 的 retType 走上下文反推。
        if (node->varType()) {
            auto declType = node->varType()->getType();
            if (declType.isFn()) {
                inferLambdaParamsFromFnType(litLambda, declType);
            }
        }
        emitLambdaFunction(static_cast<p<LambdaExprNode>>(litLambda), litLambda->getType());
    }

    // 处理数组填充表达式 ([N; value] 语法)。E3067 由 SemaPass 先抛；此处防 IR 空指针。
    if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(expr)) {
        if (!node->varType()) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        TypeInfo varType = node->varType()->getType();
        if (!varType.isArray()) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        DEBUG_LOG_VAL("  Statement: Declare (ArrayFill)", varName << " : " << varType.name);

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        registerLocalVar(varName, alloca, varType);

        compileArrayInitExpr(arrayInitNode, varType, alloca);
    } else {
        // 普通变量声明
        TypeInfo varType;
        if (node->varType()) {
            varType = node->varType()->getType();
            // 推断灵活整数的类型
            if (isFlexibleIntExpr(expr)) {
                if (isIntTypeName(varType.name)) {
                    tryInferIntType(expr, varType);
                } else if (varType.isRc()) {
                    if (auto elem = varType.rcElementType()) {
                        if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                    }
                } else if (varType.isWeak()) {
                    if (auto elem = varType.weakElementType()) {
                        if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                    }
                } else if (varType.isHeap()) {
                    if (auto elem = varType.heapElementType()) {
                        if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                    }
                }
            }
            // 推断 tuple 字面量中灵活整数的类型：
            // 当 target type 展开为 tuple（含泛型别名如 Triple<i64> → (i64,i64,i64)）,
            // 递归推断每个元素的灵活整数类型。
            // 否则元素默认为 i32，与 target 的 i64 不匹配 → LLVM store 类型断言崩溃。
            inferFlexibleInts(expr, varType);
        } else {
            varType = expr->getType();
        }

        DEBUG_LOG_VAL("  Statement: Declare", varName << " : " << varType.name);

        // Phase 4a: T& 借用局部变量；不分配独立 slot，直接绑到来源指针
        // expr 必须是 ExprGetRef（&x）或拷贝绑定 d2 = d（d 已为 Ref<T>）
        if (varType.isRef()) {
            auto innerType = varType.refElementType();
            if (!innerType) {
                // E3050：SemaPass / TypeInfo 构造已保证包装类型有内层；此处防 IR 走空路径。
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            llvm::Value* rhsPtr = nullptr;
            if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(expr)) {
                rhsPtr = compileGetRefExpr(getRefNode);
                // 校验 &expr 内层类型与声明 inner 一致
                auto innerOfGetRef = getRefNode->getType().refElementType();
                if (!innerOfGetRef || *innerOfGetRef != *innerType) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
            } else if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr);
                       litExpr && dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                auto litObj = dynamic_cast<LiteralObjNode*>(litExpr->literal());
                auto srcName = litObj->getValue().getText();
                auto it = _localVarPtrs.find(srcName);
                if (it == _localVarPtrs.end()) {
                    // E4004 由 SemaPass 先抛；此处防 IR 绑到非本帧槽。
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                rhsPtr = it->second;
            } else if (auto callExpr = dynamic_cast<ExprCallNode*>(expr)) {
                // §8.3.5.5 as_ref(box) 站点：直接消费 baked codegen 的返回值（已是非空 ptr to payload）
                std::string calleeName;
                if (auto litCallee = dynamic_cast<ExprLiteralNode*>(callExpr->getCalleeExpr())) {
                    if (auto obj = dynamic_cast<LiteralObjNode*>(litCallee->literal())) {
                        calleeName = obj->getValue().getText();
                    }
                }
                // §8.3.5.5 as_ref(box) 与 §8.6.X 用户函数返回 T&：调用结果直接是指针。
                bool callRetIsRef = callExpr->getType().isRef();
                if (calleeName != "as_ref" && !callRetIsRef) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto callValue = compileExpr(expr);
                rhsPtr = callValue;
            } else if (auto pathCall = dynamic_cast<ExprPathCallNode*>(expr)) {
                // Static path returning T& (e.g. Counter::fields → [Field& * N]&)
                if (!pathCall->getType().isRef()) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto pathValue = compileExpr(expr);
                rhsPtr = pathValue;
            } else if (auto getNode = dynamic_cast<ExprGetNode*>(expr)) {
                // Array indexing returning T& (e.g. fs[0] where fs: [T& * N]&)
                if (!getNode->getType().isRef()) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto getValue = compileExpr(expr);
                rhsPtr = getValue;
            } else {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            registerLocalVar(varName, rhsPtr, varType);
            return;
        }

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        // RHS 求值前零填充：错误通道（try-catch / `!` 透传）会跳过后续 store，
        // 作用域尾仍析构此槽。未初始化的 Array._data 等是栈垃圾，free 即崩溃
        // （BUGS：try 里 `let arr Array<T> = fallible()` 失败路径）。
        // 与 compileDeclareStatement Phase 3a 同款；成功路径 Init store 覆盖，不 release 旧值。
        if (typeNeedsDestructor(resolveAlias(varType))) {
            _builder.CreateStore(llvm::Constant::getNullValue(llvmType), alloca);
        }
        registerLocalVar(varName, alloca, varType);

        // 处理 Rc<T> 类型（Phase 1a 新布局：单 handle 指针 + Block 单分配）
        // Rc 实例 = { handle: Block* }；Block = { u32 strong, u32 weak, payload }
        if (varType.isRc()) {
            auto elemType = varType.rcElementType();
            if (!elemType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            auto rcStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

            if (exprType.isRc() && exprType.rcElementType() && *exprType.rcElementType() == *elemType) {
                storeIntoSlot(alloca, exprVal, varType, expr, SlotStore::Init);
            } else if (exprType == *elemType) {
                // 由值构造 Rc：分配 Block，把 payload 存入 block+8
                auto elemLLVMType = getLLVMType(*elemType);
                auto sizeVal =
                    _builder.getInt64(_module->getDataLayout().getTypeAllocSize(elemLLVMType).getFixedValue());

                auto allocFn = runtime::getRcAllocFn(_module, _builder);
                auto block = _builder.CreateCall(allocFn, {sizeVal}, "rc_block");

                // payload 起始 = block + 8
                auto payloadPtr = _builder.CreateGEP(_builder.getInt8Ty(), block, {_builder.getInt64(8)}, "rc_payload");
                _builder.CreateStore(exprVal, payloadPtr);

                // 写 handle 字段
                auto handleField = _builder.CreateGEP(rcStructType, alloca, {zero, zero}, "handle_field");
                _builder.CreateStore(block, handleField);

                // B-4: 消费临时帧中的 exprVal（对齐 Rc-from-Rc / Array-from-Array 路径），
                // 避免 popAndReleaseTempFrame 双释放已移入 Rc block 的数据缓冲。
                if (isFreshHandleExpr(expr)) {
                    consumeTemp(exprVal);
                }
            } else {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

        }
        // 处理 Weak<T> 类型（Phase 1d.2：支持从 Rc<T> 或 Weak<T> 构造，weak++）
        else if (varType.isWeak()) {
            auto elemType = varType.weakElementType();
            if (!elemType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            bool fromRc = exprType.isRc() && exprType.rcElementType() && *exprType.rcElementType() == *elemType;
            bool fromWeak = exprType.isWeak() && exprType.weakElementType() && *exprType.weakElementType() == *elemType;
            if (!fromRc && !fromWeak) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto srcStructType = getLLVMType(exprType);
            auto weakStructType = getLLVMType(varType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto i32Ty = _builder.getInt32Ty();
            auto i8Ty = _builder.getInt8Ty();

            // 取源 Rc/Weak 的 handle（两者 layout 同形 { ptr handle }）
            auto tmpAlloca = _builder.CreateAlloca(srcStructType, nullptr, "weak_src_tmp");
            _builder.CreateStore(exprVal, tmpAlloca);
            auto srcHandleField = _builder.CreateGEP(srcStructType, tmpAlloca, {zero, zero}, "src_handle_field");
            auto srcHandle = _builder.CreateLoad(ptrTy, srcHandleField, "src_handle");

            // weak++（哨兵 / null 跳过）
            // Phase 8b: Weak-from-Weak fresh 源已 +1 weak（callee move-return retain 用 _weak_retain），跳过；
            // Rc 源始终需要 weak++（不是 retain，是 Weak 句柄首次被引用，与 Rc 的 strong 计数无关）
            bool needWeakInc = !(fromWeak && isFreshHandleExpr(expr));
            // Phase 8d.1: fromWeak fresh 路径直接接 +1 weak，从临时帧消费
            if (fromWeak && isFreshHandleExpr(expr)) {
                consumeTemp(exprVal);
            }
            if (needWeakInc) {
                auto incBB = llvm::BasicBlock::Create(_context, "weak_inc", _builder.GetInsertBlock()->getParent());
                auto checkBB = llvm::BasicBlock::Create(_context, "weak_check", _builder.GetInsertBlock()->getParent());
                auto doneBB =
                    llvm::BasicBlock::Create(_context, "weak_inc_done", _builder.GetInsertBlock()->getParent());
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

        }
        // 处理 Array<T> 类型 (动态数组，Phase 1b 单 handle Block 布局)
        else if (varType.isArrayGeneric()) {
            auto elemType = varType.arrayGenericElementType();
            if (!elemType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto elemLLVMType = getLLVMType(*elemType);
            auto ptrTy = llvm::PointerType::get(_context, 0);

            // 处理数组字面量：走统一 helper（含嵌套 Array<Array<U>> 字面量递归修复）
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto block = buildArrayLiteralBlock(arrayNode, *elemType);
                _builder.CreateStore(block, alloca);
            } else {
                // 从其他 Array<T> 表达式初始化：句柄复制 + retain
                // 与 Rc 的 var q = p 路径同形（Phase 1a），否则作用域结束 LIFO 双重 release
                // 触发同 handle freed-block read。修复 BUGS.md「Array 声明拷贝漏 retain」。
                // Phase 8b: fresh 来源（call/method 调用）已 move-return retain，跳过
                // Phase 8d.1: fresh 来源从临时帧消费
                auto exprType = expr->getType();
                if (!exprType.isArrayGeneric() && exprType.name != "Array") {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto exprVal = compileExpr(expr);
                storeIntoSlot(alloca, exprVal, varType, expr, SlotStore::Init);
            }

        }
        // 处理 Nullable<T> 类型 (T? 的解糖)
        // 三种 RHS:
        //   1) null 字面量 → { _has=false, _value=zeroinit }
        //   2) T 值 → 隐式包装为 { _has=true, _value=expr }
        //   3) 已是 Nullable<T> → 整体结构体复制
        else if (varType.isNullable()) {
            auto innerType = varType.nullableInnerType();
            if (!innerType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto nullableStructType = getLLVMType(varType);
            auto innerLLVMType = getLLVMType(*innerType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            llvm::Value* hasField = _builder.CreateGEP(nullableStructType, alloca, {zero, zero}, "nullable_has");
            llvm::Value* valueField = _builder.CreateGEP(nullableStructType, alloca, {zero, one}, "nullable_value");

            // 是否是 null 字面量？
            bool isNullLit = isFlexibleNullExpr(expr);

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
                    takeOwnership(exprVal, *innerType, expr);
                    _builder.CreateStore(exprVal, alloca);
                } else if (exprType == *innerType) {
                    takeOwnership(exprVal, *innerType, expr);
                    _builder.CreateStore(_builder.getInt1(true), hasField);
                    _builder.CreateStore(exprVal, valueField);
                } else {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
            }
        }
        // Heap<T>：单所有权堆作用域句柄（DRAFT-heap-types §8.3a）
        // Phase 2.5：仅接受 Heap:<T>(...) 等同型 Heap<T> RHS，作用域尾走 __yux_heap_free
        // 注：Heap-from-Heap 拷贝会双释放 — 借用检查 Phase 2.8 静态拒绝
        else if (varType.isHeap()) {
            auto elemType = varType.heapElementType();
            if (!elemType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            auto exprType = expr->getType();
            if (!exprType.isHeap() || !(*exprType.heapElementType() == *elemType)) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            auto exprVal = compileExpr(expr);
            _builder.CreateStore(exprVal, alloca);
        } else {
            // 普通变量
            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();

            // Ref 类型不能隐式转换为值类型（yux 无隐式转换）
            // 例：let v i32 = arr[0] — arr[0] 返回 i32&，不能隐式转 i32
            if (exprType.isRef() && !varType.isRef()) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            // 数组类型检查
            if (varType.isArray() && exprType.isArray()) {
                if (varType.arraySize != exprType.arraySize) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                // E3009 元素类型：SemaPass 带 target-type 已查
            }

            // 通用类型匹配检查：声明类型与表达式类型必须严格一致
            // 先经 applySubst 解析透明类型别名（如 `type A = i32`），避免别名名与目标类型名假阳性
            // 跳过 Self（TypeInfo("Self") 由 substStack 替换，此时比较无意义）
            // 跳过空类型名（getType 未完全解析的退化情况）
            // 跳过灵活整数字面量（类型会由 tryInferIntType 按目标类型推断）
            auto cmpVarType = applySubst(varType);
            auto cmpExprType = applySubst(exprType);
            // 跳过含泛型形参的类型（此时尚未实例化，比较会产生假阳性）
            // 也跳过非已知类型名（如 T, U 等泛型形参，applySubst 未覆盖时仍是占位名）
            auto isKnownTypeName = [&](const string& n) -> bool {
                if (isBuiltinType(n)) return true;
                if (_file->getStructDecl(n)) return true;
                if (_yux && _yux->sdkFile() && _yux->sdkFile()->getStructDecl(n)) return true;
                return false;
            };
            if (!cmpVarType.name.empty() && !cmpExprType.name.empty() && !cmpVarType.isSelf() &&
                !cmpExprType.isSelf() && !cmpVarType.isRef() && !cmpExprType.isRef() &&
                cmpVarType.genericArgs.empty() && cmpExprType.genericArgs.empty() && isKnownTypeName(cmpVarType.name) &&
                isKnownTypeName(cmpExprType.name) && !isFlexibleIntExpr(expr)) {
                if (cmpVarType != cmpExprType) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
            }

            storeIntoSlot(alloca, exprVal, resolveAlias(varType), expr, SlotStore::Init);
        }
    }
}

// 编译元组解构声明语句：var (a, b, ...) = expr
// Phase 5：仅支持一层平铺 ID，不支持嵌套和 _
// 流程：
//   1. 编译 expr 得 struct value（匿名 tuple struct）
//   2. applySubst 解析 expr 类型 / 标注类型，要求是 Tuple
//   3. 元素数与 names 数对齐校验（E3102）
//   4. 逐元素 alloca + ExtractValue + Store；同步注册 _localVarPtrs / 符号表类型
void Compiler::compileDeclareAssignTupleStatement(p<StatementDeclareAssignTupleNode> node) {
    auto expr = node->expr();
    const auto& names = node->names();

    // 解析期望的元组类型：显式标注优先，其次 expr 推断
    TypeInfo wholeType;
    if (node->varType()) {
        wholeType = node->varType()->getType();
    } else {
        wholeType = expr->getType();
    }
    auto resolved = applySubst(wholeType);
    // E3101 / E3102 由 SemaPass 先抛；此处防 IR 把非元组当 ExtractValue。
    if (!resolved.isTuple()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }
    const auto& elems = resolved.tupleElements();
    if (elems.size() != names.size()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    // 编译 expr 得元组 struct value
    auto exprVal = compileExpr(expr);
    if (!exprVal) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }

    DEBUG_LOG_VAL("  Statement: DeclareTuple", names.size() << " names from " << resolved.name);

    // 逐元素 ExtractValue + alloca + Store；同步刷新符号表类型（visit 阶段 alias 路径用占位）
    for (size_t i = 0; i < names.size(); ++i) {
        auto varName = names[i].getText();
        const auto& elemType = *elems[i];

        auto llvmType = getLLVMType(elemType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        registerLocalVar(varName, alloca, elemType);

        auto elemVal = _builder.CreateExtractValue(exprVal, {static_cast<unsigned>(i)}, "tuple.bind");
        _builder.CreateStore(elemVal, alloca);

        // 刷新符号表类型（visit 阶段对 alias 路径登记的是空 TypeInfo）
        if (auto sc = node->findNearestScope()) {
            if (auto sym = sc->lookupSymbol(varName)) {
                sym->type = elemType;
            }
        }
        // TODO: 元素若为 RC / Rc / 含析构 struct，需要在此处 retain；当前 Phase 5 仅覆盖值类型
    }
}

// ==================== 赋值语句编译 ====================

// 编译赋值语句
// 支持普通赋值和复合赋值 (+=, -=, *=, /=, %=, ^=, <<=, >>=)
void Compiler::compileAssignStatement(p<StatementAssignNode> node) {
    auto objName = node->obj().getText();
    auto expr = node->expr();
    auto& subs = node->subs();
    auto assignOp = node->op();

    auto applyCompoundOp = [this](llvm::Value* currentVal, llvm::Value* exprVal, AssignOp op,
                                  const TypeInfo& type) -> llvm::Value* {
        switch (op) {
        case AssignOp::AddEq:
            if (type.isFloat()) {
                return _builder.CreateFAdd(currentVal, exprVal, "addtmp");
            }
            return _builder.CreateAdd(currentVal, exprVal, "addtmp");
        case AssignOp::SubEq:
            if (type.isFloat()) {
                return _builder.CreateFSub(currentVal, exprVal, "subtmp");
            }
            return _builder.CreateSub(currentVal, exprVal, "subtmp");
        case AssignOp::MulEq:
            if (type.isFloat()) {
                return _builder.CreateFMul(currentVal, exprVal, "multmp");
            }
            return _builder.CreateMul(currentVal, exprVal, "multmp");
        case AssignOp::DivEq:
            if (type.isFloat()) {
                return _builder.CreateFDiv(currentVal, exprVal, "divtmp");
            }
            if (type.isUnsigned()) {
                return _builder.CreateUDiv(currentVal, exprVal, "divtmp");
            }
            return _builder.CreateSDiv(currentVal, exprVal, "divtmp");
        case AssignOp::ModEq:
            if (type.isFloat()) {
                return _builder.CreateFRem(currentVal, exprVal, "modtmp");
            }
            if (type.isUnsigned()) {
                return _builder.CreateURem(currentVal, exprVal, "modtmp");
            }
            return _builder.CreateSRem(currentVal, exprVal, "modtmp");
        case AssignOp::XorEq: // ^=
            return _builder.CreateXor(currentVal, exprVal, "xortmp");
        case AssignOp::OrEq: // |=
            return _builder.CreateOr(currentVal, exprVal, "ortmp");
        case AssignOp::AndEq: // &=
            return _builder.CreateAnd(currentVal, exprVal, "andtmp");
        case AssignOp::MtMtEq: // >>=
            return _builder.CreateAShr(currentVal, exprVal, "shrtmp");
        case AssignOp::LtLtEq: // <<=
            return _builder.CreateShl(currentVal, exprVal, "shltmp");
        default:
            return exprVal;
        }
    };

    // 处理简单变量赋值 (无成员访问)
    if (subs.empty()) {
        auto sym = lookupVarSymbol(objName, node);
        if (!sym) {
            // E3030 由 SemaPass 赋值 LHS / getType 先抛；此处防 IR 槽表与符号表不一致。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        // DRAFT-static-vars Phase 2: 全局变量 —— 先于 _localVarPtrs 路径独立处理。
        // 全局变量不在 _localVarPtrs 中，通过 Mangler 找 LLVM GlobalVariable。
        {
            string ownerMod = (!sym->moduleName.empty()) ? sym->moduleName : _file->moduleName();
            bool globPriv = !objName.empty() && objName[0] == '_';
            string mangledName = Mangler::global(ownerMod, objName, globPriv);
            if (auto globalVar = _module->getGlobalVariable(mangledName, true)) {
                if (!sym->writeable) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto exprVal = compileExpr(expr);
                auto exprType = expr->getType();
                llvm::Value* valToStore;
                if (assignOp != AssignOp::Eq) {
                    auto currentVal = _builder.CreateLoad(globalVar->getValueType(), globalVar, "global.current.load");
                    auto castedExprVal = createCast(exprVal, exprType, sym->type);
                    valToStore = applyCompoundOp(currentVal, castedExprVal, assignOp, sym->type);
                } else {
                    valToStore = createCast(exprVal, exprType, sym->type);
                }
                _builder.CreateStore(valToStore, globalVar);
                return;
            }
        }

        // Phase 4b: T& 赋值是 store-through（改被引对象），不是 rebind；
        // T& 形参 / val 局部 T& 的 writeable=false 不影响"写被引"，写权由源对象决定（4d 校验）
        if (!sym->writeable && !sym->type.isRef()) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        DEBUG_LOG_VAL("  Statement: Assign", objName << " : " << sym->type.name);

        // Phase 4b: T& 赋值落 store-through 改被引对象（rebind 禁）
        // _localVarPtrs[objName] 持有底层 T 的地址（由声明 / 形参路径建立）
        if (sym->type.isRef()) {
            auto innerType = sym->type.refElementType();
            if (!innerType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
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
        if (isFlexibleIntExpr(expr)) {
            if (isIntTypeName(sym->type.name)) {
                tryInferIntType(expr, sym->type);
            } else if (sym->type.isRc()) {
                if (auto elem = sym->type.rcElementType()) {
                    if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                }
            } else if (sym->type.isWeak()) {
                if (auto elem = sym->type.weakElementType()) {
                    if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                }
            } else if (sym->type.isHeap()) {
                if (auto elem = sym->type.heapElementType()) {
                    if (isIntTypeName(elem->name)) tryInferIntType(expr, *elem);
                }
            }
        }

        // 处理 Array<T> 字面量赋值（包括空数组 = []）
        // Phase 3d: 新 handle 来自 _array_alloc（strong=1），无需 retain；旧 handle 必须 release
        if (sym->type.isArrayGeneric()) {
            if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                auto it = _localVarPtrs.find(objName);
                if (it != _localVarPtrs.end()) {
                    auto elemType = sym->type.arrayGenericElementType();
                    // 走统一 helper：含嵌套 Array<Array<U>> 字面量按外层 elemType 递归编译
                    auto block = buildArrayLiteralBlock(arrayNode, elemType ? *elemType : TypeInfo("i8"));
                    storeIntoSlot(it->second, block, sym->type, expr, SlotStore::Replace);
                    return;
                }
            }
        }

        // Rc<T> 赋值：处理 Rc -> Rc 复制和 T -> Rc<T> 构造
        // 与 compileDeclareAssignStatement 的 Rc 初始化路径保持一致
        if (assignOp == AssignOp::Eq && sym->type.isRc()) {
            auto elemType = sym->type.rcElementType();
            if (!elemType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto it = _localVarPtrs.find(objName);
            if (it == _localVarPtrs.end()) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();
            auto rcStructType = getLLVMType(sym->type);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

            if (exprType.isRc() && exprType.rcElementType() && *exprType.rcElementType() == *elemType) {
                storeIntoSlot(it->second, exprVal, sym->type, expr, SlotStore::Replace);
            } else if (exprType == *elemType) {
                // 由值构造 Rc：分配 Block，把 payload 存入 block+8
                auto elemLLVMType = getLLVMType(*elemType);
                auto sizeVal =
                    _builder.getInt64(_module->getDataLayout().getTypeAllocSize(elemLLVMType).getFixedValue());

                auto allocFn = runtime::getRcAllocFn(_module, _builder);
                auto block = _builder.CreateCall(allocFn, {sizeVal}, "rc_block");

                // payload 起始 = block + 8
                auto payloadPtr = _builder.CreateGEP(_builder.getInt8Ty(), block, {_builder.getInt64(8)}, "rc_payload");
                _builder.CreateStore(exprVal, payloadPtr);

                // 释放旧 Rc
                releaseAtPtr(it->second, sym->type);

                // 写 handle 字段
                auto handleField = _builder.CreateGEP(rcStructType, it->second, {zero, zero}, "handle_field");
                _builder.CreateStore(block, handleField);
            } else {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            return;
        }

        // Nullable<T> 赋值：与 compileVarStatement 的初始化路径保持一致
        // 三种 RHS:
        //   1) null 字面量 → { _has=false, _value=zeroinit }
        //   2) T 值 → 隐式包装为 { _has=true, _value=expr }
        //   3) 已是 Nullable<T> → 整体结构体复制
        if (assignOp == AssignOp::Eq && sym->type.isNullable()) {
            auto innerType = sym->type.nullableInnerType();
            if (!innerType) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto nullableStructType = getLLVMType(sym->type);
            auto innerLLVMType = getLLVMType(*innerType);
            auto alloca = _localVarPtrs[objName];
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            llvm::Value* hasField = _builder.CreateGEP(nullableStructType, alloca, {zero, zero}, "nullable_has");
            llvm::Value* valueField = _builder.CreateGEP(nullableStructType, alloca, {zero, one}, "nullable_value");

            bool isNullLit = isFlexibleNullExpr(expr);

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
                    throwSemaGap(node->getLineNumber(), node->getColumn());
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

        if (assignOp == AssignOp::Eq) {
            storeIntoSlot(_localVarPtrs[objName], valToStore, sym->type, expr, SlotStore::Replace);
        } else {
            _builder.CreateStore(valToStore, _localVarPtrs[objName]);
        }
    } else {
        // 处理成员访问赋值 (obj.field = value)
        auto sym = lookupVarSymbol(objName, node);
        if (!sym) {
            // E3030 由 SemaPass 赋值 LHS / getType 先抛；此处防 IR 槽表与符号表不一致。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        TypeInfo actualType = sym->type;
        if (sym->type.isRef()) {
            auto refElemType = sym->type.refElementType();
            if (refElemType) {
                actualType = *refElemType;
            }
        }

        // 元组成员赋值: t.0 = e / t.0.1 = e（透明 alias 由 applySubst 兜底）
        // 仅当顶层就是 tuple 时进此分支；混合路径 struct.field.0 暂未支持
        // TODO: 支持 struct.field.<N> 混合链路
        // TODO: 元组元素若为 RC / Rc / 含析构 struct 时，需要 retain new + release old；当前仅覆盖值类型
        {
            auto resolvedTop = applySubst(actualType);
            if (resolvedTop.isTuple()) {
                auto it = _localVarPtrs.find(objName);
                if (it == _localVarPtrs.end()) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                llvm::Value* curPtr = it->second;
                TypeInfo curType = resolvedTop;

                auto isPureDigits = [](const string& s) {
                    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
                };

                DEBUG_LOG_VAL("  Statement: TupleMemberAssign", objName << "." << subs[0].getText());

                for (size_t i = 0; i < subs.size(); ++i) {
                    auto memberText = subs[i].getText();
                    if (!isPureDigits(memberText)) {
                        // 元组段必须是数字索引
                        throwSemaGap(node->getLineNumber(), node->getColumn());
                    }
                    auto resolvedCur = applySubst(curType);
                    if (!resolvedCur.isTuple()) {
                        // 链中段已不是 tuple（嵌套 struct/数组等）暂不支持
                        throwSemaGap(node->getLineNumber(), node->getColumn());
                    }
                    auto& elems = resolvedCur.tupleElements();
                    auto idx = static_cast<size_t>(std::stoul(memberText));
                    // E3100 由 SemaPass tryValidateFieldChain 先抛；此处防 IR GEP 越界。
                    if (idx >= elems.size()) {
                        throwSemaGap(node->getLineNumber(), node->getColumn());
                    }
                    auto elemType = *elems[idx];
                    auto llvmTupleType = getLLVMType(resolvedCur);
                    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    auto idxVal = llvm::ConstantInt::get(_builder.getInt32Ty(), static_cast<unsigned>(idx));
                    auto fieldPtr = _builder.CreateGEP(llvmTupleType, curPtr, {zero, idxVal}, "tuple.field");

                    if (i == subs.size() - 1) {
                        // 末段：编译 RHS 并写入
                        if (isIntTypeName(elemType.name) && isFlexibleIntExpr(expr)) {
                            tryInferIntType(expr, elemType);
                        }
                        auto exprVal = compileExpr(expr);
                        auto exprType = expr->getType();
                        llvm::Value* valToStore;
                        if (assignOp != AssignOp::Eq) {
                            auto curVal = _builder.CreateLoad(getLLVMType(elemType), fieldPtr, "current.load");
                            auto castedExprVal = createCast(exprVal, exprType, elemType);
                            valToStore = applyCompoundOp(curVal, castedExprVal, assignOp, elemType);
                        } else {
                            valToStore = createCast(exprVal, exprType, elemType);
                        }
                        _builder.CreateStore(valToStore, fieldPtr);
                        return;
                    }
                    curPtr = fieldPtr;
                    curType = elemType;
                }
                return;
            }
        }

        // DRAFT-spec-reflect §6: Field.value 写路径
        // 检测 f.value = expr 形态，其中 f 是 Field 类型（反射字段句柄）
        // 直接追踪 let 绑定链路，避免构建临时 AST 节点
        if (actualType.name == "Field" && subs.size() == 1 && subs[0].getText() == "value") {
            if (!_currentFnNode) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            // 追踪变量 f 的 let 定义，提取 struct 名和字段索引
            const StructDeclNode* sd = nullptr;
            int fieldIdx = -1;
            // 在 FnNode body 中查找 let 语句，追索 init 表达式
            for (auto& stmt : _currentFnNode->body()) {
                if (auto* letStmt = dynamic_cast<StatementDeclareAssignNode*>(stmt)) {
                    if (letStmt->name().getText() == objName) {
                        // 追踪 init 表达式: 期望 Point::fields.get(N) 或 Point::fields[N] 形态
                        auto* init = letStmt->expr();
                        // Pattern A: .get(N) call
                        if (auto* call = dynamic_cast<ExprCallNode*>(init)) {
                            if (call->getArgs().size() == 1) {
                                if (auto* pc = dynamic_cast<ExprPathCallNode*>(call->getCalleeExpr())) {
                                    if (pc->variantName().getText() == "get" && pc->enumName().getText() != "") {
                                        string structName = pc->enumName().getText();
                                        auto* idxExpr = call->getArgs()[0];
                                        if (auto* idxLit = dynamic_cast<ExprLiteralNode*>(idxExpr)) {
                                            if (auto* intLit = dynamic_cast<LiteralIntNode*>(idxLit->literal())) {
                                                i64 idx = sema::parseIntLiteral(
                                                    intLit->getValue().getText(),
                                                    static_cast<int>(intLit->getValue().getLine()),
                                                    static_cast<int>(intLit->getValue().getCharPositionInLine()) + 1);
                                                if (idx >= 0) {
                                                    sd = names().lookupStruct(structName);
                                                    if (sd) {
                                                        int nonStaticCount = 0;
                                                        for (auto& f : sd->fields()) {
                                                            if (f->isStatic()) continue;
                                                            if (nonStaticCount == static_cast<int>(idx)) {
                                                                fieldIdx = sd->fieldIndex(f->name().getText());
                                                                break;
                                                            }
                                                            ++nonStaticCount;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        // Pattern B: [N] indexing (new [Field& * N]&)
                        if (!sd)
                            if (auto* getNode = dynamic_cast<ExprGetNode*>(init)) {
                                if (getNode->indices().size() == 1) {
                                    if (auto* pc = dynamic_cast<ExprPathCallNode*>(getNode->arrayExpr())) {
                                        if (pc->variantName().getText() == "fields" && pc->enumName().getText() != "") {
                                            string structName = pc->enumName().getText();
                                            auto* idxExpr = getNode->indices()[0];
                                            if (auto* idxLit = dynamic_cast<ExprLiteralNode*>(idxExpr)) {
                                                if (auto* intLit = dynamic_cast<LiteralIntNode*>(idxLit->literal())) {
                                                    i64 idx = sema::parseIntLiteral(
                                                        intLit->getValue().getText(),
                                                        static_cast<int>(intLit->getValue().getLine()),
                                                        static_cast<int>(intLit->getValue().getCharPositionInLine()) +
                                                            1);
                                                    if (idx >= 0) {
                                                        sd = names().lookupStruct(structName);
                                                        if (sd) {
                                                            int nonStaticCount = 0;
                                                            for (auto& f : sd->fields()) {
                                                                if (f->isStatic()) continue;
                                                                if (nonStaticCount == static_cast<int>(idx)) {
                                                                    fieldIdx = sd->fieldIndex(f->name().getText());
                                                                    break;
                                                                }
                                                                ++nonStaticCount;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        break;
                    }
                }
            }
            if (!sd || fieldIdx < 0) {
                // E3133 由 SemaPass tryValidateReflectFieldValueWrite 先抛；此处防 IR 无字段可写。
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
            auto fIdxVal = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIdx);
            std::array<llvm::Value*, 2> indices{zero, fIdxVal};
            auto fieldPtr = _builder.CreateGEP(structType, selfIt->second, indices, "reflect.field");
            auto fieldType = sd->fields()[fieldIdx]->getType();

            auto exprVal = compileExpr(expr);
            auto exprType = expr->getType();
            llvm::Value* valToStore;
            if (assignOp != AssignOp::Eq) {
                auto currentVal = _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "current.load");
                auto castedExprVal = createCast(exprVal, exprType, fieldType);
                valToStore = applyCompoundOp(currentVal, castedExprVal, assignOp, fieldType);
            } else {
                valToStore = createCast(exprVal, exprType, fieldType);
            }
            _builder.CreateStore(valToStore, fieldPtr);
            return;
        }

        // Phase 4c 对称：Rc<T>.field = ... 自动 deref
        // 读路径已在 compileMemberAccess（compiler_expr.cpp）里对 Rc<T> 做了 deref：
        // load handle，payload = handle + 8，再按内层 T 走字段 GEP。
        // 写路径之前漏了这一段，命中 Rc<T> 会因为 "Rc" 没有 StructDecl 抛 E3045。
        // 这里把同样的处理补齐：先记下 Rc 形态，待取到 structPtr 后再做 GEP+load。
        bool needRcDeref = false;
        TypeInfo rcOuterType = actualType;
        if (actualType.isRc()) {
            needRcDeref = true;
            if (auto inner = actualType.rcElementType()) {
                actualType = *inner;
            }
        }

        auto structDecl = names().lookupStruct(actualType);
        if (!structDecl) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        DEBUG_LOG_VAL("  Statement: MemberAssign", objName << "." << subs[0].getText());

        auto it = _localVarPtrs.find(objName);
        if (it == _localVarPtrs.end()) {
            // E3030 由 SemaPass 赋值 LHS / getType 先抛；此处防 IR 槽表与符号表不一致。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        llvm::Value* structPtr = it->second;

        // Rc<T> 写入：load handle 字段（offset 0），payload 起始 = handle + 8
        if (needRcDeref) {
            auto rcStructType = getLLVMType(rcOuterType);
            auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, structPtr, {zero32, zero32}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            structPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        }

        auto structType = getLLVMType(actualType);

        // 遍历成员访问链
        for (size_t i = 0; i < subs.size(); ++i) {
            auto memberName = subs[i].getText();
            int fieldIndex = structDecl->fieldIndex(memberName);
            if (fieldIndex < 0) {
                // E3152: 实例写静态字段 (DRAFT-static-vars §4.4)
                if (structDecl->staticField(memberName)) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            auto field = structDecl->fields()[fieldIndex];
            // Phase 3.5.a: 私有字段可见性 E3042 整体走 sema::validatePrivateFieldAccess
            // (helper 内部剥 `$<泛型实例>` 后缀比对 base, 与原 inline 等价).
            sema::validatePrivateFieldAccess(structDecl, memberName, actualType.name, _currentStructName,
                                             node->getLineNumber(), node->getColumn());

            if (i == subs.size() - 1) {
                // 最后一个成员: 执行赋值
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
                std::array<llvm::Value*, 2> indices{zero, idx};

                auto fieldPtr = _builder.CreateGEP(structType, structPtr, indices, "struct.field");
                auto fieldType = applySubst(field->getType());

                // 处理 Array<T> 字段赋值（Phase 1b：分配 Block 并把 handle 写入字段）
                // Phase 3d: 新 handle 来自 _array_alloc（strong=1），无需 retain；旧 handle 必须 release
                if (fieldType.isArrayGeneric()) {
                    if (auto arrayNode = dynamic_cast<ExprArrayNode*>(expr)) {
                        auto elemType = fieldType.arrayGenericElementType();
                        // 走统一 helper：含嵌套 Array<Array<U>> 字面量按外层 elemType 递归编译
                        auto block = buildArrayLiteralBlock(arrayNode, elemType ? *elemType : TypeInfo("i8"));
                        storeIntoSlot(fieldPtr, block, fieldType, expr, SlotStore::Replace);
                        return;
                    }
                }

                // Nullable<T> 字段赋值：与局部赋值相同的三路包装
                //   1) null 字面量 → { _has=false, _value=zeroinit }
                //   2) T 值 → 隐式包装为 { _has=true, _value=expr }
                //   3) 已是 Nullable<T> → 整体结构体复制
                // 不能 createCast(T → Nullable)：LLVM cast<Ty>() 会 abort（结构体字段 String? 赋值）
                if (assignOp == AssignOp::Eq && fieldType.isNullable()) {
                    auto innerType = fieldType.nullableInnerType();
                    if (!innerType) {
                        throwSemaGap(node->getLineNumber(), node->getColumn());
                    }

                    auto nullableStructType = getLLVMType(fieldType);
                    auto innerLLVMType = getLLVMType(*innerType);
                    auto nZero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                    auto nOne = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

                    llvm::Value* hasField =
                        _builder.CreateGEP(nullableStructType, fieldPtr, {nZero, nZero}, "nullable_has");
                    llvm::Value* valueField =
                        _builder.CreateGEP(nullableStructType, fieldPtr, {nZero, nOne}, "nullable_value");

                    if (isFlexibleNullExpr(expr)) {
                        if (typeNeedsDestructor(fieldType)) {
                            releaseAtPtr(fieldPtr, fieldType);
                        }
                        _builder.CreateStore(_builder.getInt1(false), hasField);
                        _builder.CreateStore(llvm::Constant::getNullValue(innerLLVMType), valueField);
                    } else {
                        if (isIntTypeName(innerType->name) && isFlexibleIntExpr(expr)) {
                            tryInferIntType(expr, *innerType);
                        }
                        auto exprType = expr->getType();
                        auto exprVal = compileExpr(expr);

                        if (exprType.isNullable() && exprType == fieldType) {
                            storeIntoSlot(fieldPtr, exprVal, fieldType, expr, SlotStore::Replace);
                        } else if (exprType == *innerType) {
                            takeOwnership(exprVal, *innerType, expr);
                            if (typeNeedsDestructor(fieldType)) {
                                releaseAtPtr(fieldPtr, fieldType);
                            }
                            _builder.CreateStore(_builder.getInt1(true), hasField);
                            _builder.CreateStore(exprVal, valueField);
                        } else {
                            throwSemaGap(node->getLineNumber(), node->getColumn());
                        }
                    }
                    return;
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

                if (assignOp == AssignOp::Eq) {
                    storeIntoSlot(fieldPtr, valToStore, fieldType, expr, SlotStore::Replace);
                } else {
                    _builder.CreateStore(valToStore, fieldPtr);
                }
            } else {
                // 中间段：当前仅支持纯 struct 嵌套（不含 Rc/Array/Ref/Nullable/RC 字段）
                // 中段若是 RC / Rc / Array / Ref / Nullable，自动 deref / 写穿语义未对齐，先拒收。
                auto interType = field->getType();
                if (interType.isRc() || interType.isArrayGeneric() || interType.isRef() || interType.isNullable() ||
                    interType.isWeak() || interType.isPtr() || isBuiltinType(interType.name) ||
                    typeNeedsDestructor(interType)) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto interStructDecl = names().lookupStruct(interType);
                if (!interStructDecl) {
                    throwSemaGap(node->getLineNumber(), node->getColumn());
                }
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
                std::array<llvm::Value*, 2> indices{zero, idx};
                structPtr = _builder.CreateGEP(structType, structPtr, indices, "struct.field");
                actualType = interType;
                structDecl = interStructDecl;
                structType = getLLVMType(actualType);
            }
        }
    }
}

// ==================== 循环语句编译 ====================

// 编译 loop 循环语句
// 生成无限循环结构，配合 break 语句使用
// loopInit 可选：loop name = expr { } / loop (a, b) = expr { }
void Compiler::compileLoopStatement(p<StatementLoopNode> node) {
    DEBUG_LOG("  Statement: Loop" << (node->hasInit() ? " (with init)" : ""));

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    const size_t frameDepthBeforeLoop = scopeFrameDepth();

    // ==== loop init 子句（pre-header 分配；跨迭代，仅 loop 退出时析构）====
    if (node->hasInit()) {
        pushScopeFrame(); // loop-init 帧
        auto initExprNode = node->initExpr();
        const auto& names = node->initNames();

        if (node->initType()) {
            auto declaredType = node->initType()->getType();
            if (names.size() == 1) {
                if (isFlexibleIntExpr(initExprNode) && isIntTypeName(declaredType.name)) {
                    tryInferIntType(initExprNode, declaredType);
                }
            }
        }

        // 编译 init 表达式（pre-header 中）
        auto initVal = compileExpr(initExprNode);
        if (!initVal) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
        }

        if (names.size() == 1) {
            // 单变量：loop i = expr
            TypeInfo varType;
            if (node->initType()) {
                varType = node->initType()->getType();
            } else {
                varType = initExprNode->getType();
            }

            auto llvmType = getLLVMType(varType);
            auto alloca = _builder.CreateAlloca(llvmType, nullptr, names[0].getText());
            registerLocalVar(names[0].getText(), alloca, varType);
            _builder.CreateStore(initVal, alloca);
            if (typeNeedsDestructor(resolveAlias(varType))) {
            }
        } else {
            // Tuple 解构：loop (a, b) = expr
            TypeInfo wholeType;
            if (node->initType()) {
                wholeType = node->initType()->getType();
            } else {
                wholeType = initExprNode->getType();
            }
            auto resolved = applySubst(wholeType);
            // E3101 / E3102 由 SemaPass 先抛；此处防 IR 把非元组当 ExtractValue。
            if (!resolved.isTuple()) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
            const auto& elems = resolved.tupleElements();
            if (elems.size() != names.size()) {
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }

            for (size_t i = 0; i < names.size(); ++i) {
                auto varName = names[i].getText();
                const auto& elemType = *elems[i];

                auto llvmType = getLLVMType(elemType);
                auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
                registerLocalVar(varName, alloca, elemType);

                auto elemVal = _builder.CreateExtractValue(initVal, {static_cast<unsigned>(i)}, "loop.bind");
                _builder.CreateStore(elemVal, alloca);
            }
            // TODO: 元素若为 RC / Rc / 含析构 struct，需要 retain
        }
    }

    // ==== 循环结构 ====
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(_context, "loop.cond");
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(_context, "loop.body");
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(_context, "loop.exit");

    _builder.CreateBr(condBB);

    func->insert(func->end(), condBB);
    _builder.SetInsertPoint(condBB);
    _builder.CreateBr(bodyBB);

    func->insert(func->end(), bodyBB);
    _builder.SetInsertPoint(bodyBB);

    const size_t frameDepthBeforeBody = scopeFrameDepth();
    _loopExitBlocks.push_back({.label = node->label().getText(),
                               .exitBB = exitBB,
                               .continueBB = condBB,
                               .frameDepthBeforeLoop = frameDepthBeforeLoop,
                               .frameDepthBeforeBody = frameDepthBeforeBody});

    // 体：compileStatementBlock 推 body 帧，每轮尾（br cond 前）析构体 let
    compileStatementBlock(node->block());

    _loopExitBlocks.pop_back();

    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(condBB);
    }

    func->insert(func->end(), exitBB);
    _builder.SetInsertPoint(exitBB);

    // break 已在跳转前 emitDestructorsAbove(frameDepthBeforeLoop)；exitBB 不再析构。
    // 编译期清掉 loop-init 帧（若有），避免泄漏到 loop 之后的代码。
    unwindScopeFramesTo(frameDepthBeforeLoop);

    if (!exitBB->hasNPredecessorsOrMore(1)) {
        _builder.CreateUnreachable();
    }
}

// ==================== Break 语句编译 ====================

// 编译 break 语句
// 跳出当前循环
void Compiler::compileBreakStatement(p<StatementBreakNode> node) {
    const auto& brLabel = node->label();
    DEBUG_LOG("  Statement: Break" << (brLabel.getText().empty() ? "" : " (label: " + brLabel.getText() + ")"));

    if (_loopExitBlocks.empty()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    const LoopExitInfo* target = nullptr;
    if (brLabel.getText().empty()) {
        target = &_loopExitBlocks.back();
    } else {
        for (auto it = _loopExitBlocks.rbegin(); it != _loopExitBlocks.rend(); ++it) {
            if (it->label == brLabel.getText()) {
                target = &(*it);
                break;
            }
        }
        if (!target) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
    }

    // 先发射析构 IR（含 body / 嵌套 if / loop-init），不 pop 编译期帧；再跳 exit
    emitDestructorsAbove(target->frameDepthBeforeLoop);
    _builder.CreateBr(target->exitBB);

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* unreachableBB = llvm::BasicBlock::Create(_context, "unreachable", func);
    _builder.SetInsertPoint(unreachableBB);
}

void Compiler::compileContinueStatement(p<StatementContinueNode> node) {
    const auto& cLabel = node->label();
    DEBUG_LOG("  Statement: Continue" << (cLabel.getText().empty() ? "" : " (label: " + cLabel.getText() + ")"));

    if (_loopExitBlocks.empty()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    const LoopExitInfo* target = nullptr;
    if (cLabel.getText().empty()) {
        target = &_loopExitBlocks.back();
    } else {
        for (auto it = _loopExitBlocks.rbegin(); it != _loopExitBlocks.rend(); ++it) {
            if (it->label == cLabel.getText()) {
                target = &(*it);
                break;
            }
        }
        if (!target) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
    }

    emitDestructorsAbove(target->frameDepthBeforeBody);
    _builder.CreateBr(target->continueBB);

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* unreachableBB = llvm::BasicBlock::Create(_context, "unreachable", func);
    _builder.SetInsertPoint(unreachableBB);
}

void Compiler::compileForInStatement(p<StatementForInNode> node) {
    DEBUG_LOG("  Statement: ForIn item=" << node->item().getText());

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    const size_t frameDepthBeforeLoop = scopeFrameDepth();
    pushScopeFrame(); // 集合临时 / 索引
    const size_t frameDepthBeforeBody = scopeFrameDepth();

    auto collExpr = node->expr();
    TypeInfo collType = collExpr->hasResolvedType() ? collExpr->resolvedType() : collExpr->getType();
    collType = applySubst(collType);
    const bool isRefColl = collType.isRef();
    TypeInfo peeled = collType.peelRef();

    llvm::Value* collPtr = nullptr;
    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(collExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                collPtr = it->second;
            }
        }
    }
    if (!collPtr) {
        auto baseVal = compileExpr(collExpr);
        if (!baseVal) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        auto valTy = getLLVMType(collType);
        auto alloca = _builder.CreateAlloca(valTy, nullptr, "for.coll");
        _builder.CreateStore(baseVal, alloca);
        if (isRefColl) {
            collPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), alloca, "for.coll.ref");
        } else {
            collPtr = alloca;
            if (typeNeedsDestructor(resolveAlias(collType))) {
                registerLocalVar(".for.coll." + std::to_string(_forInSerial++), alloca, collType);
            }
        }
    }

    auto* sizeTy = getSizeType();
    llvm::Value* lenVal = nullptr;
    if (peeled.isArrayGeneric()) {
        lenVal = _builder.CreateLoad(sizeTy, arrayLenFieldPtr(collPtr, "for"), "for.len");
    } else if (peeled.isArray()) {
        lenVal = llvm::ConstantInt::get(sizeTy, peeled.arraySize);
    } else {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    auto iAlloca = _builder.CreateAlloca(sizeTy, nullptr, "for.i");
    _builder.CreateStore(llvm::ConstantInt::get(sizeTy, 0), iAlloca);

    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(_context, "for.cond");
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(_context, "for.body");
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(_context, "for.inc");
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(_context, "for.exit");

    _builder.CreateBr(condBB);

    func->insert(func->end(), condBB);
    _builder.SetInsertPoint(condBB);
    auto iCur = _builder.CreateLoad(sizeTy, iAlloca, "for.i.cur");
    auto done = _builder.CreateICmpUGE(iCur, lenVal, "for.done");
    _builder.CreateCondBr(done, exitBB, bodyBB);

    func->insert(func->end(), bodyBB);
    _builder.SetInsertPoint(bodyBB);

    auto iBody = _builder.CreateLoad(sizeTy, iAlloca, "for.i.body");
    llvm::Value* elemPtr = nullptr;
    TypeInfo elemTy;
    if (peeled.isArrayGeneric()) {
        auto et = peeled.arrayGenericElementType();
        if (!et) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        elemTy = *et;
        auto elemLLVM = getLLVMType(elemTy);
        auto dataPtr =
            _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayDataFieldPtr(collPtr, "for"), "for.data");
        elemPtr = _builder.CreateGEP(elemLLVM, dataPtr, {iBody}, "for.elem");
    } else {
        if (!peeled.elementType) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        elemTy = *peeled.elementType;
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto llvmArr = getLLVMType(peeled);
        elemPtr = _builder.CreateGEP(llvmArr, collPtr, {zero, iBody}, "for.elem");
        if (elemTy.isRef()) {
            elemPtr = _builder.CreateLoad(getLLVMType(elemTy), elemPtr, "for.elem.ref");
        }
    }

    TypeInfo itemTy("Ref", {std::make_shared<TypeInfo>(elemTy)});
    registerLocalVar(node->item().getText(), elemPtr, itemTy);

    _loopExitBlocks.push_back({.label = node->label().getText(),
                               .exitBB = exitBB,
                               .continueBB = incBB,
                               .frameDepthBeforeLoop = frameDepthBeforeLoop,
                               .frameDepthBeforeBody = frameDepthBeforeBody});

    compileStatementBlock(node->block());
    _loopExitBlocks.pop_back();

    if (!_builder.GetInsertBlock()->getTerminator()) {
        _builder.CreateBr(incBB);
    }

    func->insert(func->end(), incBB);
    _builder.SetInsertPoint(incBB);
    auto iInc = _builder.CreateLoad(sizeTy, iAlloca, "for.i.inc");
    auto iNext = _builder.CreateAdd(iInc, llvm::ConstantInt::get(sizeTy, 1), "for.i.next");
    _builder.CreateStore(iNext, iAlloca);
    _builder.CreateBr(condBB);

    func->insert(func->end(), exitBB);
    _builder.SetInsertPoint(exitBB);
    unwindScopeFramesTo(frameDepthBeforeLoop);
    if (!exitBB->hasNPredecessorsOrMore(1)) {
        _builder.CreateUnreachable();
    }
}

// ==================== 数组元素赋值语句编译 ====================

// 编译数组元素赋值语句 (arr[idx] = value)
// 支持固定大小数组、动态数组(Array<T>)、结构体字段中的数组
void Compiler::compileArraySetStatement(p<StatementSetNode> node) {
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
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
                // E3030 由 SemaPass getType 先抛；此处防 IR 槽表漏登记。
                throwSemaGap(node->getLineNumber(), node->getColumn());
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
            int fi = outerStructDecl->fieldIndex(dotExpr->member());
            if (fi >= 0) {
                llvm::Value* dataPtr = outerPtr;
                auto zeroIdx = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);

                // Rc 类型需要先解引用获取数据指针
                if (outerType.isRc()) {
                    auto rcStructType = getLLVMType(outerType);
                    std::array<llvm::Value*, 2> bIndices{zeroIdx, zeroIdx};
                    auto dataPtrField = _builder.CreateGEP(rcStructType, outerPtr, bIndices, "rc.data_ptr_field");
                    dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField, "rc.data_ptr");
                }

                auto outerLLVM = getLLVMType(outerActual);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fi);
                std::array<llvm::Value*, 2> indicesF{zeroIdx, idx};
                currentPtr = _builder.CreateGEP(outerLLVM, dataPtr, indicesF, "array.field.ptr");
            } else if (outerStructDecl->staticField(dotExpr->member())) {
                // E3152: 实例写静态字段 (DRAFT-static-vars §4.4)
                throwSemaGap(node->getLineNumber(), node->getColumn());
            }
        }
    }

    if (!currentPtr) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    // 处理动态数组 Array<T>（B-3：字段内联，直接访问 _data）
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        auto elemLLVMType = getLLVMType(*elemType);
        // B-3: _data 字段内联，直接 load，不再经过 Block 间接
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayDataFieldPtr(currentPtr, "arr"),
                                           "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        auto valueVal = compileExpr(node->valueExpr());

        storeIntoSlot(elemPtr, valueVal, *elemType, node->valueExpr(), SlotStore::Replace);
        return;
    }

    // 处理固定大小数组 [N]T
    if (!arrayType.isArray()) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    // 支持多维数组索引
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

    auto valueVal = compileExpr(node->valueExpr());
    _builder.CreateStore(valueVal, currentPtr);
}

// ==================== 静态字段写语句 ====================

// 编译静态字段写语句（DRAFT-static-vars Phase 5）
// 语法形态: Type::FIELD = expr
// 查找对应 struct 的静态字段 GlobalVariable，check #Mut 位后 emit StoreInst
void Compiler::compileStaticFieldSetStatement(p<StatementStaticFieldSetNode> node) {
    auto r = sema::resolveExprTypeLhs(_file, _yux, node->typePath(), node->getLineNumber(), node->getColumn());
    auto typeName = r.type.name;
    auto fieldName = node->fieldName().getText();

    StructDeclNode* structDecl = r.structDecl ? r.structDecl : names().lookupStruct(r.type, true);
    if (!structDecl) {
        // E3030 由 SemaPass 静态字段写先抛。
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    const auto* sf = structDecl->staticField(fieldName);
    if (!sf) {
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }
    // E3151 非 #Mut 写：SemaPass 已查

    string ownerMod = r.type.ownerModule;
    if (ownerMod.empty() && r.owner) ownerMod = r.owner->moduleName();
    if (ownerMod.empty()) {
        FileNode* owner = nullptr;
        if (names().lookupStruct(r.type, true, &owner) && owner) ownerMod = owner->moduleName();
    }
    if (ownerMod.empty()) ownerMod = _file->moduleName();

    auto mangledName = Mangler::staticField(ownerMod, typeName, fieldName);
    auto llvmType = getLLVMType(sf->type->getType());
    auto* gv = getOrDeclareStaticFieldGV(mangledName, llvmType, !sf->isMutable);

    auto exprVal = compileExpr(node->valueExpr());
    auto exprType = node->valueExpr()->getType();
    auto targetType = sf->type->getType();
    auto valToStore = createCast(exprVal, exprType, targetType);
    _builder.CreateStore(valToStore, gv);

    DEBUG_LOG_VAL("  StaticFieldSet", typeName << "::" << fieldName << " = ...");
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
    } else if (auto declareAssignTupleNode = dynamic_cast<StatementDeclareAssignTupleNode*>(node)) {
        compileDeclareAssignTupleStatement(declareAssignTupleNode);
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
    } else if (auto forInNode = dynamic_cast<StatementForInNode*>(node)) {
        compileForInStatement(forInNode);
    } else if (auto breakNode = dynamic_cast<StatementBreakNode*>(node)) {
        compileBreakStatement(breakNode);
    } else if (auto continueNode = dynamic_cast<StatementContinueNode*>(node)) {
        compileContinueStatement(continueNode);
    } else if (auto setNode = dynamic_cast<StatementSetNode*>(node)) {
        compileArraySetStatement(setNode);
    } else if (auto staticFieldSetNode = dynamic_cast<StatementStaticFieldSetNode*>(node)) {
        compileStaticFieldSetStatement(staticFieldSetNode);
    } else {
        popAndReleaseTempFrame();
        throwSemaGap(node->getLineNumber(), node->getColumn());
    }

    popAndReleaseTempFrame();
}

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 二元 / 算术 / 比较表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
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

// ==================== 自定义类型运算符方法调用 ====================

// 编译自定义类型的二元运算符方法调用
// 将运算符表达式转换为方法调用，如 a + b -> a.plus(b)
llvm::Value* Compiler::compileCustomTypeBinaryOp(
    p<ExprNode> leftExpr, p<ExprNode> rightExpr, const TypeInfo& leftType,
    const string& methodName, int lineNum) {
    
    DEBUG_LOG_VAL("    Expr: CustomTypeBinaryOp", leftType.name << "." << methodName);
    
    // 获取左操作数的指针
    llvm::Value* leftPtr = nullptr;
    if (auto leftLiteral = dynamic_cast<ExprLiteralNode*>(leftExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(leftLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                leftPtr = it->second;
            }
        }
    }
    
    if (!leftPtr) {
        auto leftVal = compileExpr(leftExpr);
        auto structType = getLLVMType(leftType);
        if (!structType) {
            throw YuxError(lineNum, ErrorCode::E3096, leftType.name);
        }
        auto alloca = _builder.CreateAlloca(structType, nullptr, "op_lhs_tmp");
        _builder.CreateStore(leftVal, alloca);
        leftPtr = alloca;
    }
    
    auto rightType = rightExpr->getType();
    // Phase 4b: 右操作数是 T& 字面变量时，从 _localVarPtrs 直接取裸 ptr，
    // 避免 compileExpr 对 ref 自动 load 出 struct 值后又 alloca 写回 —— 写回会
    // 把 struct 看成是 ptr 类型，触发 LLVM 签名校验失败。
    llvm::Value* rightVal = nullptr;
    if (rightType.isRef()) {
        if (auto rl = dynamic_cast<ExprLiteralNode*>(rightExpr)) {
            if (auto ol = dynamic_cast<LiteralObjNode*>(rl->literal())) {
                auto vn = ol->getValue().getText();
                auto rit = _localVarPtrs.find(vn);
                if (rit != _localVarPtrs.end()) rightVal = rit->second;
            }
        }
    }
    if (!rightVal) {
        rightVal = compileExpr(rightExpr);
    }

    // 查找方法（spec §7.2.3.3）：二元运算符方法形参强制 Self&，
    // 因此查表使用 [leftType, Ref<rightType>]，原 eq(other Self) 形态不再被运算符触发。
    // 运算符位置自动取址（spec §7.2.3.6）：右操作数自动包成 Ref，无需用户写 &。
    // Phase 4b: 操作数本身是 T& 时（如 fn 形参 `actual String&`），剥掉一层 Ref
    // 与方法注册的 [Self, Self&] 对齐；不剥则 lookup 失败导致调用方编译期崩溃。
    // v0.6 Phase 2b: 透明类型别名解析，使 `A = i32` 这类别名走到运算符方法时
    // 仍能匹配到 `i32.plus` 等内置方法。
    TypeInfo effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;
    TypeInfo effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;
    effLeftType = applySubst(effLeftType);
    effRightType = applySubst(effRightType);
    string methodFullName = effLeftType.name + "." + methodName;
    TypeInfo rightRefType;
    rightRefType.kind = TypeKind::Generic;
    rightRefType.name = "Ref";
    rightRefType.genericArgs.push_back(make_shared<TypeInfo>(effRightType));
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(effLeftType);
    methodParamTypes.push_back(rightRefType);

    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (!methodSymbol) {
        // Bucket 6 (CURRENT-check.md): E3073 + byval hint 已由 SemaPass 接管,
        // 这里保留作幂等防御性双跑 (sema 已抛, 正常情况到不了).
        FileNode* sdkFilePtr = (_yux && _yux->sdkFile()) ? _yux->sdkFile() : nullptr;
        sema::validateBinOpMethodResolution(_file, sdkFilePtr,
                                            effLeftType, effRightType, methodName,
                                            lineNum, 0);
        // unreachable: helper 内必抛
    }
    
    // 准备方法参数
    vector<llvm::Value*> methodArgs;
    methodArgs.push_back(leftPtr);
    
    // 检查右操作数是否需要通过指针传递
    auto rightStructDecl = _file->getStructDecl(effRightType.name);
    if (!rightStructDecl && _yux && _yux->sdkFile()) {
        rightStructDecl = _yux->sdkFile()->getStructDecl(effRightType.name);
    }
    if (rightStructDecl && !isBuiltinType(effRightType.name)) {
        // 当右操作数本身是 T&（即 rightVal 已是 ptr）时，直接传 ptr，避免错误的
        // alloca-then-store-into-Struct 路径（structType=Struct，但 rightVal=ptr，类型不匹配）
        if (rightType.isRef()) {
            methodArgs.push_back(rightVal);
        } else {
            auto structType = getLLVMType(effRightType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_rhs_tmp");
            _builder.CreateStore(rightVal, alloca);
            methodArgs.push_back(alloca);
        }
    } else {
        methodArgs.push_back(rightVal);
    }
    
    // 获取或创建方法函数
    // mangle 与 LLVM 签名都按 methodSymbol 实际声明的形参类型走（spec §7.2.3.3 后是 Ref<T>），
    // 与 SDK / 用户代码侧定义的方法符号一一对应。
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    TypeInfo declaredRhsType = methodSymbol->params.size() >= 2 ? methodSymbol->params[1] : rightType;
    vector<TypeInfo> argTypes;
    argTypes.push_back(declaredRhsType);
    string mangledName = Mangler::method(ownerMod, effLeftType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        // 形参为 Ref<T> / 非 builtin struct 时按 ptr 传，其余按值
        bool rhsByPtr = declaredRhsType.isRef()
                        || (rightStructDecl && !isBuiltinType(declaredRhsType.name));
        if (rhsByPtr) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            paramTypes.push_back(getLLVMType(declaredRhsType));
        }
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }
    
    return _builder.CreateCall(fn, methodArgs);
}


llvm::Value* Compiler::compileAddSubExpr(p<ExprAddSubNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // v0.6 Phase 2b: 透明别名解析，使 `A = i32` 后 `A + A` 仍走内置算子路径
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr = (node->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
    DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type.name);

    // v0.6 Phase 2c：`+` 表达式整体类型为 String 时，
    // 走 StringBuilder 整链 lower（多段连续 `+` 合并为单次 builder）。
    if (node->op() == ExprAddSubNode::Op::Add && type.name == "String") {
        return compileStringPlusChain(node);
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName = (node->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }
    
    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');

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
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul: opStr = "*";
        break;
    case ExprMulDivModNode::Op::Div: opStr = "/";
        break;
    case ExprMulDivModNode::Op::Mod: opStr = "%";
        break;
    }
    DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul: methodName = "mul"; break;
        case ExprMulDivModNode::Op::Div: methodName = "div"; break;
        case ExprMulDivModNode::Op::Mod: methodName = "mod"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isUnsigned = type.startsWith('u');

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
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3077);
}

llvm::Value* Compiler::compileBinOpExpr(p<ExprBinOpNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprBinOpNode::Op::And: opStr = "&";
        break;
    case ExprBinOpNode::Op::Or: opStr = "|";
        break;
    case ExprBinOpNode::Op::Xor: opStr = "^";
        break;
    case ExprBinOpNode::Op::Shl: opStr = "<<";
        break;
    case ExprBinOpNode::Op::Shr: opStr = ">>";
        break;
    }
    DEBUG_LOG_VAL("    Expr: BinOp", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprBinOpNode::Op::And: methodName = "and"; break;
        case ExprBinOpNode::Op::Or: methodName = "or"; break;
        case ExprBinOpNode::Op::Xor: methodName = "xor"; break;
        case ExprBinOpNode::Op::Shl: methodName = "shl"; break;
        case ExprBinOpNode::Op::Shr: methodName = "shr"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());

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
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3075);
}


llvm::Value* Compiler::compileCompareExpr(p<ExprCompareNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    (void)node->getType();
    auto leftType = applySubst(node->left()->getType());
    auto rightType = applySubst(node->right()->getType());

    if (leftType != rightType) {
        // Phase 3.4.g: ExprCompareNode::getType 已抛 E3004 (kMigratedCodes 命中,
        // SemaPass 顶部 setResolvedType 自动重抛), 此处不可达; 保留作幂等防御性双跑。
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3004, leftType.name, rightType.name);
    }

    // Bucket 6 (CURRENT-check.md): E3078 (Weak ==/!=) + E3073 (Ptr ordering) 形态校验
    // 抠到 sema::validateCompareOpForm; SemaPass 已接管实际抛出点, 此处幂等防御性双跑.
    sema::validateCompareOpForm(leftType, node->op(), node->getLineNumber(), node->getColumn());

    // Ptr：内置 == / !=（用于 `p == null` 等场景）
    if (leftType.isPtr()) {
        if (node->op() == ExprCompareNode::Op::Eq || node->op() == ExprCompareNode::Op::Ne) {
            auto left = compileExpr(node->left());
            auto right = compileExpr(node->right());
            if (node->op() == ExprCompareNode::Op::Eq) {
                return _builder.CreateICmpEQ(left, right);
            }
            return _builder.CreateICmpNE(left, right);
        }
    }

    string opStr;
    switch (node->op()) {
    case ExprCompareNode::Op::Eq: opStr = "==";
        break;
    case ExprCompareNode::Op::Ne: opStr = "!=";
        break;
    case ExprCompareNode::Op::Lt: opStr = "<";
        break;
    case ExprCompareNode::Op::Le: opStr = "<=";
        break;
    case ExprCompareNode::Op::Gt: opStr = ">";
        break;
    case ExprCompareNode::Op::Ge: opStr = ">=";
        break;
    case ExprCompareNode::Op::AndAnd: opStr = "&&";
        break;
    case ExprCompareNode::Op::OrOr: opStr = "||";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << leftType.name);

    // && 和 || 是逻辑运算符，不转换为方法调用
    if (node->op() == ExprCompareNode::Op::AndAnd || node->op() == ExprCompareNode::Op::OrOr) {
        auto left = compileExpr(node->left());
        auto right = compileExpr(node->right());
        if (node->op() == ExprCompareNode::Op::AndAnd) {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.rhs");
            return _builder.CreateAnd(leftBool, rightBool, "and");
        } else {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.rhs");
            return _builder.CreateOr(leftBool, rightBool, "or");
        }
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprCompareNode::Op::Eq: methodName = "eq"; break;
        case ExprCompareNode::Op::Ne: methodName = "ne"; break;
        case ExprCompareNode::Op::Lt: methodName = "lt"; break;
        case ExprCompareNode::Op::Le: methodName = "le"; break;
        case ExprCompareNode::Op::Gt: methodName = "gt"; break;
        case ExprCompareNode::Op::Ge: methodName = "ge"; break;
        default: break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = leftType.startsWith('f');
    bool isUnsigned = leftType.startsWith('u');

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
    default:
        break;
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3076);
}

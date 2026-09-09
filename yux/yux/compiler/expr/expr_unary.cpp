// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 一元 / 取引用 / 括号表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <set>

// 编译自定义类型的一元运算符方法调用
// 将运算符表达式转换为方法调用，如 -a -> a.neg()
llvm::Value* Compiler::compileCustomTypeUnaryOp(ExprNode* expr, const TypeInfo& type, const string& methodName,
                                                int lineNum) {

    // v0.16: [] 返回 T&——剥 Ref 用于方法名查找
    auto effType = type.isRef() ? *type.refElementType() : type;
    // Heap<T> → T：运算符穿透 Heap wrapper，方法在内部类型上查找
    if (effType.isHeap()) {
        if (auto inner = effType.heapElementType()) effType = *inner;
    }
    // Rc<T> → T：运算符穿透 Rc wrapper，方法在内部类型上查找
    if (effType.isRc()) {
        if (auto inner = effType.rcElementType()) effType = *inner;
    }

    DEBUG_LOG_VAL("    Expr: CustomTypeUnaryOp", effType.name << "." << methodName);

    // 获取操作数的指针
    llvm::Value* ptr = nullptr;
    if (type.isRc()) {
        // Rc<T>：解引用 handle → payload 指针作为 self
        auto rcVal = compileExpr(expr);
        auto handle = _builder.CreateExtractValue(rcVal, {0}, "rc.op.handle");
        ptr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.op.payload");
    } else if (type.isHeap() || type.isRef()) { // NOLINT(bugprone-branch-clone): 语义不同但 body 相同
        // Heap<T> / T&：compileExpr 已返回指针，直接用作 self ptr
        ptr = compileExpr(expr);
    } else {
        if (auto literal = dynamic_cast<ExprLiteralNode*>(expr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal->literal())) {
                auto varName = objLiteral->getValue().getText();
                auto it = _localVarPtrs.find(varName);
                if (it != _localVarPtrs.end()) {
                    ptr = it->second;
                }
            }
        }

        if (!ptr) {
            auto val = compileExpr(expr);
            auto structType = getLLVMType(effType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_tmp");
            _builder.CreateStore(val, alloca);
            ptr = alloca;
        }
    }

    // 查找方法
    string methodFullName = effType.name + "." + methodName;
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(effType);

    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }

    if (!methodSymbol) {
        throwSemaGap(lineNum);
    }

    // 获取或创建方法函数
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    vector<TypeInfo> argTypes;
    string mangledName = mangleMethod(ownerMod, effType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }

    return _builder.CreateCall(fn, {ptr});
}

llvm::Value* Compiler::compileParenExpr(ExprParenNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    DEBUG_LOG("    Expr: Paren");
    return compileExpr(node->expr());
}

llvm::Value* Compiler::compileGetRefExpr(ExprGetRefNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto objName = node->obj().getText();
    auto& subs = node->subs();

    DEBUG_LOG_VAL("    Expr: GetRef", objName);

    // 先查局部变量（_localVarPtrs），不在则 fallback 到全局变量（LLVM GlobalVariable）
    llvm::Value* currentPtr = nullptr;
    SymbolInfo* sym = nullptr;

    auto it = _localVarPtrs.find(objName);
    if (it != _localVarPtrs.end()) {
        // —— 局部变量路径 ——
        currentPtr = it->second;
        sym = lookupVarSymbol(objName, node);
        if (!sym) {
            // E3030 由 ExprGetRefNode::getType 先抛。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
    } else {
        // —— 全局变量路径（DRAFT-static-ref） ——
        // 全局变量不在 _localVarPtrs 中，通过文件级符号表查找，再走 Mangler 获取 LLVM GlobalVariable
        sym = lookupVarSymbol(objName, node);
        if (!sym) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        string ownerMod = (!sym->moduleName.empty()) ? sym->moduleName : _file->moduleName();
        bool globPriv = !objName.empty() && objName[0] == '_';
        string mangledName = Mangler::global(ownerMod, objName, globPriv);

        // #Inline #Cval：无 GlobalVariable 存储地址，无法取址。报 E3118 提示改用普通 #Cval。
        if (_inlineConstantValues.contains(mangledName)) {
            // E3118 由 SemaPass GetRef 先抛。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }

        auto globalVar = _module->getGlobalVariable(mangledName, true);
        if (!globalVar) {
            // 非全局变量也非局部变量（如闭包外层变量）→ SemaPass getType 已报 E3030。
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        currentPtr = globalVar;
    }

    TypeInfo currentType = sym->type;
    // Phase 4a: 若源是 T&（参数 / 局部 ref），currentPtr 已经是底层 T 的地址；剥到 T
    if (currentType.isRef()) {
        if (auto inner = currentType.refElementType()) currentType = *inner;
    }

    for (auto& sub : subs) {
        const auto& memberName = sub.getText();

        // Phase 4c: Heap<T>.field —— 自动 deref：currentPtr 是 alloca slot (T**)，load 出 T*
        if (currentType.isHeap()) {
            currentPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), currentPtr, "heap.ptr");
            if (auto inner = currentType.heapElementType()) currentType = *inner;
        }
        // Phase 4c: Rc<T>.field —— 自动 deref：load handle，payload = handle + 8
        if (currentType.isRc()) {
            auto rcStructType = getLLVMType(currentType);
            auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, currentPtr, {zero32, zero32}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            currentPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            if (auto inner = currentType.rcElementType()) currentType = *inner;
        }

        auto structDecl = names().lookupStruct(currentType);
        if (!structDecl) {
            throwSemaGap(node->getLineNumber(), node->getColumn());
        }
        int fieldIndex = structDecl->fieldIndex(memberName);
        auto field = structDecl->fields()[fieldIndex];
        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        std::array<llvm::Value*, 2> indices{zero, idx};

        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        TypeInfo fieldType = field->getType();
        if (currentType.hasGenericArgs() && structDecl->isGeneric() &&
            currentType.genericArgs.size() == structDecl->typeParams().size()) {
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    currentType.genericArgs[i] ? *currentType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        // 泛型 struct 方法体内 `$` 的类型不含泛型实参时，通过 _substStack 替换
        fieldType = applySubst(fieldType);
        currentType = fieldType;
    }

    return currentPtr;
}

llvm::Value* Compiler::compileUnaryExpr(ExprUnaryNode* node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = node->getType();
    auto rightType = node->right()->getType();
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref
    auto effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;
    // Heap<T> → T：运算符自动穿透 Heap wrapper，作用在内部 T
    bool rightIsHeap = false;
    if (effRightType.isHeap()) {
        if (auto inner = effRightType.heapElementType()) {
            effRightType = *inner;
            rightIsHeap = true;
        }
    }
    // Rc<T> → T：运算符自动穿透 Rc wrapper，作用在内部 T
    bool rightIsRc = false;
    if (effRightType.isRc()) {
        if (auto inner = effRightType.rcElementType()) {
            effRightType = *inner;
            rightIsRc = true;
        }
    }

    string opStr;
    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        opStr = "-";
        break;
    case ExprUnaryNode::Op::Rev:
        opStr = "~";
        break;
    case ExprUnaryNode::Op::Not:
        opStr = "!";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);

    // 检查是否为自定义类型（用剥 Ref/Heap/Rc 后的标量名）
    if (!isBuiltinType(effRightType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprUnaryNode::Op::Neg:
            methodName = "neg";
            break;
        case ExprUnaryNode::Op::Rev:
            methodName = "inv";
            break;
        case ExprUnaryNode::Op::Not:
            methodName = "not";
            break;
        }
        return compileCustomTypeUnaryOp(node->right(), rightType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto right = compileExpr(node->right());
    bool isFloat = type.isFloat();
    bool isBool = type.name == "bool";

    // v0.16: 操作数若是 T& 则 load 出值
    if (rightIsHeap) {
        right = _builder.CreateLoad(getLLVMType(effRightType), right, "heap.val");
    } else if (rightIsRc) {
        auto handle = _builder.CreateExtractValue(right, {0}, "rc.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
        right = _builder.CreateLoad(getLLVMType(effRightType), payload, "rc.val");
    } else if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effRightType), right, "unary_op");
    }

    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        if (isFloat) {
            return _builder.CreateFNeg(right, "neg");
        }
        return _builder.CreateNeg(right, "neg");
    case ExprUnaryNode::Op::Rev:
        return _builder.CreateNot(right, "not");
    case ExprUnaryNode::Op::Not:
        return _builder.CreateNot(right, "lnot");
    }

    // switch default unreachable：上面三个 case 已覆盖全部 Op。
    throwSemaGap(node->getLineNumber(), node->getColumn());
}

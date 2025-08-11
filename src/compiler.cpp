// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#include "compiler.h"
#include "node/fn_node.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "types.h"
#include <iostream>

#ifdef _DEBUG
#define DEBUG_LOG(msg) if(debug) { std::cerr << "[DEBUG] " << msg << std::endl; }
#define DEBUG_LOG_VAL(msg, val) if(debug) { std::cerr << "[DEBUG] " << msg << ": " << val << std::endl; }
#else
#define DEBUG_LOG(msg)
#define DEBUG_LOG_VAL(msg, val)
#endif
#include <utility>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

llvm::Type* Compiler::getLLVMType(const string& name) {
    return _typeMap[name];
}

llvm::FunctionType* Compiler::getLLVMFunctionType(p<FnHeaderNode> header) {
    vector<llvm::Type*> paramTypes;
    for (auto param : header->params()) {
        paramTypes.push_back(getLLVMType(param->type()->getText()));
    }
    auto retType = header->retType();
    string retTypeName = retType ? retType->getText() : "";
    return llvm::FunctionType::get(getLLVMType(retTypeName), paramTypes, false);
}

Compiler::Compiler(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file) :
    _context(context), _builder(builder), _module(mod), _file(file) {
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
    }
    
    auto fnType = getLLVMFunctionType(header);
    auto func = _module->getFunction(name);
    if (!func) {
        func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, _module);
    }
    return func;
}

void Compiler::compile(p<FileNode> file) {
    auto functions = file->getFunctions();
    for (auto fn : functions) {
        auto func = getFunction(fn->header());
        compileFn(fn, func);
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
        auto paramType = node->header()->params()[arg.getArgNo()]->type()->getText();
        auto llvmType = getLLVMType(paramType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
        _builder.CreateStore(&arg, alloca);
        _localVarPtrs[paramName] = alloca;

        DEBUG_LOG_VAL("  Param", paramName << " : " << paramType);
    }

    for (auto s : node->body()) {
        compileStatement(s);
    }

    if (func->getReturnType()->isVoidTy()) {
        _builder.CreateRetVoid();
        DEBUG_LOG("  Added implicit void return");
    }
    DEBUG_LOG_VAL("Finished compiling function", node->header()->name()->getText());
}

void Compiler::compileStatement(p<StatementNode> node) {
    if (auto retNode = dynamic_cast<StatementRetNode*>(node)) {
        DEBUG_LOG("  Statement: Return");
        auto retType = retNode->expr()->getType();
        if (retType.empty()) {
            compileExpr(retNode->expr());
            DEBUG_LOG("    Expression compiled as void return");
        } else {
            auto retVal = compileExpr(retNode->expr());
            _builder.CreateRet(retVal);
            DEBUG_LOG("    Created return instruction");
        }
    }
    else if (auto declareNode = dynamic_cast<StatementDeclareAssignNode*>(node)) {
        auto expr = declareNode->expr();
        auto exprVal = compileExpr(expr);
        auto exprType = expr->getType();
        auto varName = declareNode->name()->getText();

        string varType;
        if (declareNode->varType()) {
            varType = declareNode->varType()->getText();
        } else {
            varType = exprType;
        }

        DEBUG_LOG_VAL("  Statement: Declare", varName << " : " << varType);

        auto llvmType = getLLVMType(varType);
        auto alloca = _builder.CreateAlloca(llvmType, nullptr, varName);
        _builder.CreateStore(exprVal, alloca);
        _localVarPtrs[varName] = alloca;
    }
    else if (auto assignNode = dynamic_cast<StatementAssignNode*>(node)) {
        auto varName = assignNode->name()->getText();
        auto expr = assignNode->expr();
        auto exprVal = compileExpr(expr);

        auto sym = _currentFnNode->lookupSymbol(varName);
        if (!sym) {
            throw YuxError("Undefined variable: {}", varName);
        }

        if (!sym->writeable) {
            throw YuxError("Cannot assign to immutable variable: {}", varName);
        }

        DEBUG_LOG_VAL("  Statement: Assign", varName << " : " << sym->type);

        auto exprType = expr->getType();
        auto valToStore = createCast(exprVal, exprType, sym->type);
        _builder.CreateStore(valToStore, _localVarPtrs[varName]);
    }
    else if (auto exprNode = dynamic_cast<StatementExprNode*>(node)) {
        DEBUG_LOG("  Statement: Expression");
        compileExpr(exprNode->expr());
    }
}

llvm::Value* Compiler::createCast(llvm::Value* val, const string& srcType, const string& dstType) {
    if (srcType == dstType) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType << " -> " << dstType);

    auto dstLLVMType = getLLVMType(dstType);
    bool srcIsFloat = srcType.starts_with('f');
    bool dstIsFloat = dstType.starts_with('f');
    bool srcIsUnsigned = srcType.starts_with('u');
    bool dstIsUnsigned = dstType.starts_with('u');

    if (srcIsFloat && dstIsFloat) {
        if (srcType == "f64" && dstType == "f32") {
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

llvm::Value* Compiler::compileExpr(p<ExprNode> node) {
    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(node)) {
        auto literal = literalNode->literal();
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
            DEBUG_LOG_VAL("    Expr: IntLiteral", text << " : " << type);
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
            DEBUG_LOG_VAL("    Expr: FloatLiteral", text << " : " << type);
            return llvm::ConstantFP::get(getLLVMType(type), numVal);
        }
        else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
            bool boolVal = (text == "true");
            DEBUG_LOG_VAL("    Expr: BoolLiteral", text << " : " << type);
            return llvm::ConstantInt::get(getLLVMType(type), boolVal ? 1 : 0, false);
        }
        else if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal)) {
            auto varName = text;
            auto sym = _currentFnNode->lookupSymbol(varName);
            if (sym && _localVarPtrs.contains(varName)) {
                DEBUG_LOG_VAL("    Expr: VariableLoad", varName << " : " << sym->type);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[varName]);
            }
            throw YuxError("Undefined variable: {}", varName);
        }
    }
    else if (auto addSubNode = dynamic_cast<ExprAddSubNode*>(node)) {
        auto left = compileExpr(addSubNode->left());
        auto right = compileExpr(addSubNode->right());
        auto type = addSubNode->getType();
        bool isFloat = type.starts_with('f');

        string opStr = (addSubNode->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
        DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type);

        if (addSubNode->op() == ExprAddSubNode::Op::Add) {
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
    else if (auto mulDivModNode = dynamic_cast<ExprMulDivModNode*>(node)) {
        auto left = compileExpr(mulDivModNode->left());
        auto right = compileExpr(mulDivModNode->right());
        auto type = mulDivModNode->getType();
        bool isFloat = type.starts_with('f');
        bool isUnsigned = type.starts_with('u');

        string opStr;
        switch (mulDivModNode->op()) {
            case ExprMulDivModNode::Op::Mul: opStr = "*"; break;
            case ExprMulDivModNode::Op::Div: opStr = "/"; break;
            case ExprMulDivModNode::Op::Mod: opStr = "%"; break;
        }
        DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type);

        switch (mulDivModNode->op()) {
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
    }
    else if (auto parenNode = dynamic_cast<ExprParenNode*>(node)) {
        DEBUG_LOG("    Expr: Paren");
        return compileExpr(parenNode->expr());
    }
    else if (auto callNode = dynamic_cast<ExprCallNode*>(node)) {
        auto calleeExpr = callNode->getCalleeExpr();
        vector<llvm::Value*> args;
        vector<string> argTypes;
        for (auto& arg : callNode->getArgs()) {
            args.push_back(compileExpr(arg));
            argTypes.push_back(arg->getType());
        }

        if (auto dotNode = dynamic_cast<ExprDotNode*>(calleeExpr)) {
            auto baseExpr = dotNode->baseExpr();
            auto member = dotNode->member();

            if (member.starts_with("to_")) {
                string dstType = member.substr(3);
                DEBUG_LOG_VAL("    Expr: CastCall (to_)", dstType);
                auto baseVal = compileExpr(baseExpr);
                auto srcType = baseExpr->getType();
                return createCast(baseVal, srcType, dstType);
            }

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto objName = objLiteral->getValue()->getText();
                    auto fnSymbol = _file->lookupFnSymbol(objName);
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
        }

        if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
                auto fnName = objLiteral->getValue()->getText();

                if (_castFunctions.contains(fnName)) {
                    DEBUG_LOG_VAL("    Expr: CastFunction", fnName);
                    auto& castInfo = _castFunctions[fnName];
                    return createCast(castInfo.value, castInfo.srcType, castInfo.dstType);
                }

                auto fnSymbol = _file->lookupFnSymbol(fnName);
                if (fnSymbol) {
                    string cName;
                    if (_file->isInnerFn(fnName)) {
                        cName = Node::getCName(fnName, argTypes);
                    } else if (fnName == "main") {
                        cName = "yux_main";
                    } else {
                        cName = fnName;
                    }
                    
                    DEBUG_LOG_VAL("    Expr: FunctionCall", fnName << " -> " << cName);
                    auto fn = _module->getFunction(cName);
                    if (!fn) {
                        vector<llvm::Type*> paramTypes;
                        for (auto& t : argTypes) {
                            paramTypes.push_back(getLLVMType(t));
                        }
                        auto retType = fnSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(fnSymbol->retType);
                        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
                        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
                    }
                    return _builder.CreateCall(fn, args);
                }
                
                DEBUG_LOG_VAL("    Expr: ExternalFunctionCall", fnName);
                auto fn = _module->getFunction(fnName);
                if (!fn) {
                    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
                    fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
                }
                return _builder.CreateCall(fn, args);
            }
        }

        throw YuxError("Unsupported call expression");
    }
    else if (auto dotNode = dynamic_cast<ExprDotNode*>(node)) {
        auto baseExpr = dotNode->baseExpr();
        auto member = dotNode->member();

        if (member.starts_with("to_")) {
            string dstType = member.substr(3);
            DEBUG_LOG_VAL("    Expr: DotCast", member);
            auto baseVal = compileExpr(baseExpr);
            auto srcType = baseExpr->getType();

            string castFnName = "__cast_" + to_string(_castCounter++);
            _castFunctions[castFnName] = {baseVal, srcType, dstType};

            return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
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
    else if (auto compareNode = dynamic_cast<ExprCompareNode*>(node)) {
        auto left = compileExpr(compareNode->left());
        auto right = compileExpr(compareNode->right());
        auto leftType = compareNode->left()->getType();
        auto rightType = compareNode->right()->getType();

        if (leftType != rightType) {
            throw YuxError("Type mismatch in comparison: left is {}, right is {}", leftType, rightType);
        }

        bool isFloat = leftType.starts_with('f');
        bool isUnsigned = leftType.starts_with('u');

        string opStr;
        switch (compareNode->op()) {
            case ExprCompareNode::Op::Eq: opStr = "=="; break;
            case ExprCompareNode::Op::Ne: opStr = "!="; break;
            case ExprCompareNode::Op::Lt: opStr = "<"; break;
            case ExprCompareNode::Op::Le: opStr = "<="; break;
            case ExprCompareNode::Op::Gt: opStr = ">"; break;
            case ExprCompareNode::Op::Ge: opStr = ">="; break;
        }
        DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << leftType);

        switch (compareNode->op()) {
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
    }
    else if (auto ifElseNode = dynamic_cast<ExprIfElseNode*>(node)) {
        auto resultType = ifElseNode->getType();
        bool hasResult = !resultType.empty();
        
        auto condVal = compileExpr(ifElseNode->condition());
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
        
        compileStatementBlockWithResult(ifElseNode->thenBlock(), mergeBB, phi, resultType);
        
        func->insert(func->end(), elseBB);
        _builder.SetInsertPoint(elseBB);
        
        auto& elifs = ifElseNode->elifs();
        auto elseBlock = ifElseNode->elseBlock();
        
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

llvm::Value* Compiler::compileStatementBlockWithResult(p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const string& resultType) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
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

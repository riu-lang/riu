// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#ifndef YUX_LANG_COMPILER_H
#define YUX_LANG_COMPILER_H


#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "node/file_node.h"
#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "node/statement_node.h"
#include "node/expr_node.h"
#include "node/struct_node.h"

struct CastInfo {
    llvm::Value* value;
    TypeInfo srcType;
    TypeInfo dstType;
};

class Compiler {
    llvm::LLVMContext& _context;
    llvm::IRBuilder<>& _builder;
    llvm::Module* _module;
    p<FileNode> _file;
    bool _isSdk = false;

    map<string, llvm::Type*> _typeMap;
    map<string, llvm::StructType*> _structTypes;
    map<string, llvm::Value*> _localVarPtrs;
    map<string, CastInfo> _castFunctions;
    int _castCounter = 0;
    
    vector<string> _scopeVars;

    llvm::Function* _currentFn = nullptr;
    p<FnNode> _currentFnNode;
    string _currentStructName;

    vector<llvm::BasicBlock*> _loopExitBlocks;

    llvm::Type* getLLVMType(const TypeInfo& type);
    llvm::FunctionType* getLLVMFunctionType(p<FnHeaderNode> header);
    llvm::StructType* getOrCreateStructType(p<StructDeclNode> structDecl);

    void emitStdoutWrite();
    void emitRuntimeHelpers();
    void emitBoxHelpers();
    void emitMainStartup();
    llvm::Function* getStdoutWriteFn();
    llvm::Function* getBoxAllocFn();
    llvm::Function* getBoxRetainFn();
    llvm::Function* getBoxReleaseFn();

    llvm::Function* getFunction(p<FnHeaderNode> header);
    llvm::Function* getMethodFunction(const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes, const TypeInfo& retType);
    llvm::Function* getDestructorFunction(const string& structName);
    llvm::Value* compileExpr(p<ExprNode> node);
    llvm::Value* compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType);
    llvm::Value* createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType);
    void compileStatementBlock(p<StatementBlockNode> block);
    llvm::Value* compileStatementBlockWithResult(p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType);
    
    void callDestructor(const string& varName, const TypeInfo& varType);
    void callDestructorsForScope();

    void compileRetStatement(p<StatementRetNode> node);
    void compileRetVoidStatement(p<StatementRetVoidNode> node);
    void compileDeclareAssignStatement(p<StatementDeclareAssignNode> node);
    void compileAssignStatement(p<StatementAssignNode> node);
    void compileLoopStatement(p<StatementLoopNode> node);
    void compileBreakStatement(p<StatementBreakNode> node);
    void compileArraySetStatement(p<StatementSetNode> node);

    llvm::Value* compileLiteralExpr(p<ExprLiteralNode> node);
    llvm::Value* compileAddSubExpr(p<ExprAddSubNode> node);
    llvm::Value* compileMulDivModExpr(p<ExprMulDivModNode> node);
    llvm::Value* compileBinOpExpr(p<ExprBinOpNode> node);
    llvm::Value* compileParenExpr(p<ExprParenNode> node);
    llvm::Value* compileCallExpr(p<ExprCallNode> node);
    llvm::Value* compileDotExpr(p<ExprDotNode> node);
    llvm::Value* compileCompareExpr(p<ExprCompareNode> node);
    llvm::Value* compileIfElseExpr(p<ExprIfElseNode> node);
    llvm::Value* compileArrayGetExpr(p<ExprGetNode> node);
    llvm::Value* compileArrayLiteralExpr(p<ExprArrayNode> node);
    llvm::Value* compileGetRefExpr(p<ExprGetRefNode> node);
    llvm::Value* compileUnaryExpr(p<ExprUnaryNode> node);

    llvm::Value* compileMethodCall(p<ExprCallNode> callNode, p<ExprDotNode> dotNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileFunctionCall(p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileConstructorCall(const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileKnownFunctionCall(p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes, FnSymbolInfo* fnSymbol);
    llvm::Value* compileStdoutWriteCall(llvm::Function* fn, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);

public:
    Compiler(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file, bool isSdk = false);

    void compile(p<FileNode> file);
    void compileGlobalConsts();
    void compileStructDecls();
    void compileStructImpls();
    void compileFn(p<FnNode> node, llvm::Function* func);
    void compileMethod(p<FnNode> node, llvm::Function* func, const string& structName);
    void compileStatement(p<StatementNode> node);
};

#endif //YUX_LANG_COMPILER_H

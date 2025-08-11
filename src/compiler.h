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
#include "node/statement_node.h"
#include "node/expr_node.h"

struct CastInfo {
    llvm::Value* value;
    string srcType;
    string dstType;
};

class Compiler {
    llvm::LLVMContext& _context;
    llvm::IRBuilder<>& _builder;
    llvm::Module* _module;
    p<FileNode> _file;

    map<string, llvm::Type*> _typeMap;
    map<string, llvm::Value*> _localVarPtrs;
    map<string, CastInfo> _castFunctions;
    int _castCounter = 0;

    llvm::Function* _currentFn = nullptr;
    p<FnNode> _currentFnNode;

    llvm::Type* getLLVMType(const string& name);
    llvm::FunctionType* getLLVMFunctionType(p<FnHeaderNode> header);

    llvm::Function* getFunction(p<FnHeaderNode> header);
    llvm::Value* compileExpr(p<ExprNode> node);
    llvm::Value* createCast(llvm::Value* val, const string& srcType, const string& dstType);
    void compileStatementBlock(p<StatementBlockNode> block);
    llvm::Value* compileStatementBlockWithResult(p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const string& resultType);

public:
    Compiler(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file);

    void compile(p<FileNode> file);
    void compileFn(p<FnNode> node, llvm::Function* func);
    void compileStatement(p<StatementNode> node);
};

#endif //YUX_LANG_COMPILER_H

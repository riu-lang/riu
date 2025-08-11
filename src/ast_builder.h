// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#ifndef YUX_LANG_AST_BUILDER_H
#define YUX_LANG_AST_BUILDER_H

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include "node/file_node.h"
#include "node/fn_node.h"
#include "yux/yuxBaseVisitor.h"
#include "yux/yuxVisitor.h"

class ASTBuilder : public yux::yuxBaseVisitor {
    llvm::LLVMContext& context;
    llvm::IRBuilder<> irBuilder;

    vector<std::any> stack;
    vector<p<Node>> _nodes;
    vector<p<ScopeNode>> _scopeStack;

    template<typename T, typename... Args>
    p<T> create(Args&&... args) {
        auto node = new T(std::forward<Args>(args)...);
        _nodes.push_back(node);
        return node;
    }
    
    p<ScopeNode> currentScope() const {
        if (_scopeStack.empty()) return nullptr;
        return _scopeStack.back();
    }

public:
    explicit ASTBuilder(llvm::LLVMContext& ctx);
    ~ASTBuilder() override;

    p<FileNode> build(yux::yuxParser::ProgramContext* ctx);

    std::any visitComment(yux::yuxParser::CommentContext* ctx) override;
    std::any visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) override;
    std::any visitProgram(yux::yuxParser::ProgramContext* ctx) override;
    std::any visitFn(yux::yuxParser::FnContext* ctx) override;
    std::any visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) override;
    std::any visitFnParam(yux::yuxParser::FnParamContext* ctx) override;

    std::any visitStatementDeclareAssign(yux::yuxParser::StatementDeclareAssignContext* ctx) override;
    std::any visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) override;
    std::any visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) override;
    std::any visitStatementRet(yux::yuxParser::StatementRetContext* ctx) override;
    std::any visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) override;

    std::any visitExprParen(yux::yuxParser::ExprParenContext* ctx) override;
    std::any visitExprCall(yux::yuxParser::ExprCallContext* ctx) override;
    std::any visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) override;
    std::any visitExprMulDivMod(yux::yuxParser::ExprMulDivModContext* ctx) override;
    std::any visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) override;
    std::any visitExprDot(yux::yuxParser::ExprDotContext* ctx) override;
    std::any visitExprCompare(yux::yuxParser::ExprCompareContext* ctx) override;
    std::any visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) override;
    std::any visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) override;
    std::any visitExprElse(yux::yuxParser::ExprElseContext* ctx) override;

    std::any visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) override;
    std::any visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) override;
    std::any visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) override;
    std::any visitNumInt(yux::yuxParser::NumIntContext* ctx) override;
    std::any visitNumFloat(yux::yuxParser::NumFloatContext* ctx) override;
};

#endif //YUX_LANG_AST_BUILDER_H

// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_AST_BUILDER_H
#define YUX_LANG_AST_BUILDER_H

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include "yux.h"
#include "node/fn_node.h"
#include "yux/yuxBaseVisitor.h"
#include "yux/yuxVisitor.h"

class ASTBuilder : public yux::yuxBaseVisitor {
    llvm::LLVMContext& context;
    llvm::IRBuilder<> irBuilder;
    Yux& _yux;
    bool _isSdk = false;

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
    explicit ASTBuilder(llvm::LLVMContext& ctx, Yux& yux, bool isSdk = false);
    ~ASTBuilder() override;

    p<FileNode> build(yux::yuxParser::ProgramContext* ctx);

    std::any visitComment(yux::yuxParser::CommentContext* ctx) override;
    std::any visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) override;
    std::any visitProgram(yux::yuxParser::ProgramContext* ctx) override;
    std::any visitFn(yux::yuxParser::FnContext* ctx) override;
    std::any visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) override;
    std::any visitFnParam(yux::yuxParser::FnParamContext* ctx) override;
    std::any visitFnClean(yux::yuxParser::FnCleanContext* ctx) override;

    std::any visitStructDecl(yux::yuxParser::StructDeclContext* ctx) override;
    std::any visitStructImpl(yux::yuxParser::StructImplContext* ctx) override;
    std::any visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) override;

    std::any visitStatementDeclareAssign(yux::yuxParser::StatementDeclareAssignContext* ctx) override;
    std::any visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) override;
    std::any visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) override;
    std::any visitStatementRet(yux::yuxParser::StatementRetContext* ctx) override;
    std::any visitStatementRetVoid(yux::yuxParser::StatementRetVoidContext* ctx) override;
    std::any visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) override;
    std::any visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) override;
    std::any visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) override;
    std::any visitStatementSet(yux::yuxParser::StatementSetContext* ctx) override;

    std::any visitExprParen(yux::yuxParser::ExprParenContext* ctx) override;
    std::any visitExprCall(yux::yuxParser::ExprCallContext* ctx) override;
    std::any visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) override;
    std::any visitExprMulDivMod(yux::yuxParser::ExprMulDivModContext* ctx) override;
    std::any visitExprBinOp(yux::yuxParser::ExprBinOpContext* ctx) override;
    std::any visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) override;
    std::any visitExprDot(yux::yuxParser::ExprDotContext* ctx) override;
    std::any visitExprCompare(yux::yuxParser::ExprCompareContext* ctx) override;
    std::any visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) override;
    std::any visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) override;
    std::any visitExprElse(yux::yuxParser::ExprElseContext* ctx) override;
    std::any visitExprGet(yux::yuxParser::ExprGetContext* ctx) override;
    std::any visitExprGetRef(yux::yuxParser::ExprGetRefContext* ctx) override;
    std::any visitExprArray(yux::yuxParser::ExprArrayContext* ctx) override;
    std::any visitExprArrayInit(yux::yuxParser::ExprArrayInitContext* ctx) override;
    std::any visitExprUnary(yux::yuxParser::ExprUnaryContext* ctx) override;

    std::any visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) override;
    std::any visitTypeGeneric(yux::yuxParser::TypeGenericContext* ctx) override;
    std::any visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) override;

    std::any visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) override;
    std::any visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) override;
    std::any visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) override;
    std::any visitNumInt(yux::yuxParser::NumIntContext* ctx) override;
    std::any visitNumFloat(yux::yuxParser::NumFloatContext* ctx) override;
};

#endif //YUX_LANG_AST_BUILDER_H

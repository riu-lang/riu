// Copyright (c) 2026. Yin-Jinlong@github

#include "ast_builder.h"
#include "node/expr_node.h"
#include "node/statement_node.h"
#include "node/literal_node.h"
#include <algorithm>

ASTBuilder::ASTBuilder(llvm::LLVMContext& ctx) :
    context(ctx), irBuilder(ctx) {
}

ASTBuilder::~ASTBuilder() {
    for (auto node : _nodes) {
        delete node;
    }
}

p<FileNode> ASTBuilder::build(yux::yuxParser::ProgramContext* ctx) {
    return any_cast_p<FileNode>(visitProgram(ctx));
}

std::any ASTBuilder::visitComment(yux::yuxParser::CommentContext* ctx) {
    return nullptr;
}

std::any ASTBuilder::visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) {
    return nullptr;
}

std::any ASTBuilder::visitProgram(yux::yuxParser::ProgramContext* ctx) {
    auto file = create<FileNode>();

    auto funs = ctx->fn();
    for (auto fn : funs) {
        auto header = fn->fnHeader();
        auto fnName = header->name->getText();

        vector<string> paramTypes;
        for (auto param : header->params) {
            paramTypes.push_back(param->type->getText());
        }
        string retType = header->retType ? header->retType->getText() : "";
        file->registerSymbol(fnName, {SymbolKind::Function, fnName, retType});
        file->registerFnSymbol(fnName, {fnName, paramTypes, retType});
    }

    stack.emplace_back(file);
    _scopeStack.push_back(file);
    visitChildren(ctx);
    _scopeStack.pop_back();
    stack.pop_back();
    return file;
}

std::any ASTBuilder::visitFn(yux::yuxParser::FnContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto header = any_cast_p<FnHeaderNode>(visitFnHeader(ctx->fnHeader()));
    auto fn = create<FnNode>(file, header);
    fn->setParentScope(file);
    file->addFunction(fn);

    stack.emplace_back(fn);
    _scopeStack.push_back(fn);

    for (auto param : header->params()) {
        fn->registerSymbol(
            param->name()->getText(), {SymbolKind::Variable, param->name()->getText(), param->type()->getText()});
    }

    if (ctx->fnBody()->fnExprkBody()) {
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = create<StatementRetNode>(fn, expr);
        fn->addStatement(retStmt);
    } else if (ctx->fnBody()->fnBlockBody()) {
        auto blockBody = ctx->fnBody()->fnBlockBody();
        auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));

        for (auto stmt : stmtBlockNode->statements()) {
            fn->addStatement(stmt);
        }

        if (stmtBlockNode->hasResult()) {
            auto retStmt = create<StatementRetNode>(fn, stmtBlockNode->resultExpr());
            fn->addStatement(retStmt);
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();
    return fn;
}

std::any ASTBuilder::visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());

    auto header = create<FnHeaderNode>(file, ctx->name, ctx->retType);

    stack.emplace_back(header);

    for (auto paramCtx : ctx->params) {
        auto param = any_cast_p<FnParamNode>(visit(paramCtx));
        header->addParam(param);
    }

    stack.pop_back();

    return header;
}

std::any ASTBuilder::visitFnParam(yux::yuxParser::FnParamContext* ctx) {
    p<Node> parent = any_cast_p<FnHeaderNode>(stack.back());
    return p<FnParamNode>(create<FnParamNode>(parent, ctx->name, ctx->type));
}

std::any ASTBuilder::visitStatementDeclareAssign(yux::yuxParser::StatementDeclareAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

    auto declKey = ctx->DeclKey()->getText();
    DeclareType declType;
    if (declKey[2] == 'r') {
        declType = DeclareType::Var;
    } else if (declKey[2] == 'l') {
        declType = DeclareType::Val;
    } else {
        declType = DeclareType::CVal;
    }

    auto name = ctx->name;
    auto type = ctx->type;

    string varType;
    if (type) {
        varType = type->getText();
    } else {
        varType = expr->getType();
    }

    if (scope) {
        scope->registerSymbol(
            name->getText(), {SymbolKind::Variable, name->getText(), varType, declType == DeclareType::Var});
    }

    return p<StatementNode>(create<StatementDeclareAssignNode>(scope, declType, name, type, expr));
}

std::any ASTBuilder::visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    return p<StatementNode>(create<StatementAssignNode>(scope, ctx->obj, expr));
}

std::any ASTBuilder::visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    bool hasSemicolon = ctx->SymbolSemicolon() != nullptr;
    return p<StatementNode>(create<StatementExprNode>(scope, expr, hasSemicolon));
}

std::any ASTBuilder::visitStatementRet(yux::yuxParser::StatementRetContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    return p<StatementNode>(create<StatementRetNode>(scope, expr));
}

std::any ASTBuilder::visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) {
    auto parentScope = currentScope();
    vector<p<StatementNode>> statements;
    for (auto stmtCtx : ctx->statement()) {
        auto stmt = any_cast_p<StatementNode>(visit(stmtCtx));
        statements.push_back(stmt);
    }

    p<ExprNode> resultExpr = nullptr;
    bool hasResult = false;

    if (!statements.empty()) {
        auto lastStmt = statements.back();
        if (auto exprStmt = dynamic_cast<StatementExprNode*>(lastStmt)) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
            }
        }
    }

    auto block = create<StatementBlockNode>(parentScope, statements, resultExpr, hasResult);
    block->setParentScope(parentScope);
    return p<StatementBlockNode>(block);
}

std::any ASTBuilder::visitExprParen(yux::yuxParser::ExprParenContext* ctx) {
    auto scope = currentScope();
    auto inner = any_cast_p<ExprNode>(visit(ctx->expr()));
    return p<ExprNode>(create<ExprParenNode>(scope, inner));
}

std::any ASTBuilder::visitExprCall(yux::yuxParser::ExprCallContext* ctx) {
    auto scope = currentScope();
    auto callee = any_cast_p<ExprNode>(visit(ctx->left));

    auto call = create<ExprCallNode>(scope, callee);
    for (auto arg : ctx->args) {
        call->addArg(any_cast_p<ExprNode>(visit(arg)));
    }
    return p<ExprNode>(call);
}

std::any ASTBuilder::visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    auto op = (opText == "+") ? ExprAddSubNode::Op::Add : ExprAddSubNode::Op::Sub;

    return p<ExprNode>(create<ExprAddSubNode>(scope, op, left, right));
}

std::any ASTBuilder::visitExprMulDivMod(yux::yuxParser::ExprMulDivModContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    ExprMulDivModNode::Op op;
    if (opText == "*") {
        op = ExprMulDivModNode::Op::Mul;
    } else if (opText == "/") {
        op = ExprMulDivModNode::Op::Div;
    } else {
        op = ExprMulDivModNode::Op::Mod;
    }

    return p<ExprNode>(create<ExprMulDivModNode>(scope, op, left, right));
}

std::any ASTBuilder::visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) {
    auto scope = currentScope();
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    return p<ExprNode>(create<ExprLiteralNode>(scope, literal));
}

std::any ASTBuilder::visitExprDot(yux::yuxParser::ExprDotContext* ctx) {
    auto scope = currentScope();
    auto base = any_cast_p<ExprNode>(visit(ctx->left));
    return p<ExprNode>(create<ExprDotNode>(scope, base, ctx->member.back()));
}

std::any ASTBuilder::visitExprCompare(yux::yuxParser::ExprCompareContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    ExprCompareNode::Op op;
    if (opText == "==") {
        op = ExprCompareNode::Op::Eq;
    } else if (opText == "!=") {
        op = ExprCompareNode::Op::Ne;
    } else if (opText == "<") {
        op = ExprCompareNode::Op::Lt;
    } else if (opText == "<=") {
        op = ExprCompareNode::Op::Le;
    } else if (opText == ">") {
        op = ExprCompareNode::Op::Gt;
    } else {
        op = ExprCompareNode::Op::Ge;
    }

    return p<ExprNode>(create<ExprCompareNode>(scope, op, left, right));
}

std::any ASTBuilder::visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) {
    return p<LiteralNode>(any_cast_p<LiteralNode>(visit(ctx->num)));
}

std::any ASTBuilder::visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) {
    auto token = ctx->True() ? ctx->True()->getSymbol() : ctx->False()->getSymbol();
    return p<LiteralNode>(create<LiteralBoolNode>(token));
}

std::any ASTBuilder::visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) {
    auto scope = currentScope();
    return p<LiteralNode>(create<LiteralObjNode>(scope, ctx->name));
}

std::any ASTBuilder::visitNumInt(yux::yuxParser::NumIntContext* ctx) {
    return p<LiteralNode>(create<LiteralIntNode>(ctx->INT()->getSymbol()));
}

std::any ASTBuilder::visitNumFloat(yux::yuxParser::NumFloatContext* ctx) {
    return p<LiteralNode>(create<LiteralFloatNode>(ctx->FLOAT()->getSymbol()));
}

std::any ASTBuilder::visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) {
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto thenBlock = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));

    vector<p<ExprElIfNode>> elifs;
    for (auto elifCtx : ctx->elifs) {
        auto elif = any_cast_p<ExprElIfNode>(visit(elifCtx));
        elifs.push_back(elif);
    }

    p<StatementBlockNode> elseBlock = nullptr;
    if (ctx->exprElse()) {
        elseBlock = any_cast_p<StatementBlockNode>(visit(ctx->exprElse()));
    }

    return p<ExprNode>(create<ExprIfElseNode>(scope, condition, thenBlock, elifs, elseBlock));
}

std::any ASTBuilder::visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) {
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    return p<ExprElIfNode>(create<ExprElIfNode>(scope, condition, block));
}

std::any ASTBuilder::visitExprElse(yux::yuxParser::ExprElseContext* ctx) {
    return visit(ctx->statementBlock());
}

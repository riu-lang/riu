// Copyright (c) 2026. Yin-Jinlong@github

#include "ast_builder.h"
#include "node/expr_node.h"
#include "node/statement_node.h"
#include "node/literal_node.h"
#include <algorithm>
#include "types.h"

ASTBuilder::ASTBuilder(llvm::LLVMContext& ctx, Yux& yux, bool isSdk) :
    context(ctx), irBuilder(ctx), _yux(yux), _isSdk(isSdk) {
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
    DEBUG_LOG("  Visit: Comment");
    return nullptr;
}

std::any ASTBuilder::visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) {
    DEBUG_LOG("  Visit: CodeLineEnd");
    return nullptr;
}

std::any ASTBuilder::visitProgram(yux::yuxParser::ProgramContext* ctx) {
    DEBUG_LOG("Visit: Program");
    p<FileNode> file;
    if (_isSdk) {
        file = _yux.createSdkFile();
    } else {
        file = _yux.createFile("");
    }
    
    auto moduleName = file->moduleName();

    stack.emplace_back(file);
    _scopeStack.push_back(file);

    auto funs = ctx->fn();
    DEBUG_LOG_VAL("  Functions count", funs.size());
    for (auto fn : funs) {
        auto header = fn->fnHeader();
        auto fnName = header->name->getText();

        vector<TypeInfo> paramTypes;
        for (auto param : header->params) {
            if (param->type()) {
                auto typeNode = any_cast_p<TypeNode>(visit(param->type()));
                paramTypes.push_back(typeNode->getType());
            }
        }
        TypeInfo retType;
        if (header->retType) {
            auto typeNode = any_cast_p<TypeNode>(visit(header->retType));
            retType = typeNode->getType();
        }
        DEBUG_LOG_VAL("  Register function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = moduleName;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, moduleName, paramTypes, retType};
        file->registerFnSymbol(fnName, fnFnSym);
    }

    auto structDecls = ctx->structDecl();
    DEBUG_LOG_VAL("  Struct declarations count", structDecls.size());
    for (auto structDecl : structDecls) {
        auto decl = any_cast_p<StructDeclNode>(visit(structDecl));
        file->addStructDecl(decl);

        for (auto field : decl->fields()) {
            string methodKey = decl->name()->getText() + "." + field->name()->getText();
            SymbolInfo fieldSym(SymbolKind::Variable, field->name()->getText(), field->getType());
            fieldSym.moduleName = moduleName;
            file->registerSymbol(methodKey, fieldSym);
        }
    }

    auto structImpls = ctx->structImpl();
    DEBUG_LOG_VAL("  Struct implementations count", structImpls.size());
    for (auto structImpl : structImpls) {
        auto impl = any_cast_p<StructImplNode>(visit(structImpl));
        file->addStructImpl(impl);

        string structName = impl->structName();
        for (auto method : impl->methods()) {
            string methodName = method->header()->name()->getText();
            string fullName = structName + "." + methodName;

            vector<TypeInfo> paramTypes;
            paramTypes.push_back(TypeInfo(structName));
            for (auto param : method->header()->params()) {
                if (param->type()) {
                    paramTypes.push_back(param->type()->getType());
                }
            }

            TypeInfo retType;
            if (method->header()->retType()) {
                retType = method->header()->retType()->getType();
            }

            DEBUG_LOG_VAL("  Register method", fullName);
            SymbolInfo methodSym(SymbolKind::Function, methodName, retType);
            methodSym.moduleName = moduleName;
            file->registerSymbol(fullName, methodSym);

            FnSymbolInfo methodFnSym{fullName, moduleName, paramTypes, retType};
            file->registerFnSymbol(fullName, methodFnSym);
        }
    }

    visitChildren(ctx);
    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG("Finished: Program");
    return file;
}

std::any ASTBuilder::visitFn(yux::yuxParser::FnContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto header = any_cast_p<FnHeaderNode>(visitFnHeader(ctx->fnHeader()));
    auto fn = create<FnNode>(file, header);
    fn->setParentScope(file);
    file->addFunction(fn);

    DEBUG_LOG_VAL("Visit: Function", header->name()->getText());

    stack.emplace_back(fn);
    _scopeStack.push_back(fn);

    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        fn->registerSymbol(
            param->name()->getText(), {SymbolKind::Variable, param->name()->getText(), paramType});
        DEBUG_LOG_VAL("  Param", param->name()->getText() << " : " << paramType.name);
    }

    if (ctx->fnBody()->fnExprkBody()) {
        DEBUG_LOG("  Body: Expression");
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = create<StatementRetNode>(fn, expr);
        fn->addStatement(retStmt);
    } else if (ctx->fnBody()->fnBlockBody()) {
        DEBUG_LOG("  Body: Block");
        auto blockBody = ctx->fnBody()->fnBlockBody();
        auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));

        for (auto stmt : stmtBlockNode->statements()) {
            fn->addStatement(stmt);
        }

        if (stmtBlockNode->hasResult()) {
            DEBUG_LOG("  Block has result, adding return");
            auto retStmt = create<StatementRetNode>(fn, stmtBlockNode->resultExpr());
            fn->addStatement(retStmt);
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG_VAL("Finished: Function", header->name()->getText());
    return fn;
}

std::any ASTBuilder::visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) {
    DEBUG_LOG_VAL("  Visit: FunctionHeader", ctx->name->getText());
    auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
    if (!file) {
        file = any_cast_p<FileNode>(stack.back());
    }

    p<TypeNode> retType = nullptr;
    if (ctx->retType) {
        retType = any_cast_p<TypeNode>(visit(ctx->retType));
        DEBUG_LOG_VAL("    Return type", retType->getType().name);
    }

    auto header = create<FnHeaderNode>(file, ctx->name, retType);

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
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    DEBUG_LOG_VAL("    Param", ctx->name->getText() << " : " << type->getType().name);
    return p<FnParamNode>(create<FnParamNode>(parent, ctx->name, type));
}

std::any ASTBuilder::visitStructDecl(yux::yuxParser::StructDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto structDecl = create<StructDeclNode>(file, ctx->name);

    DEBUG_LOG_VAL("Visit: StructDecl", ctx->name->getText());

    stack.emplace_back(structDecl);
    _scopeStack.push_back(structDecl);

    for (auto fieldCtx : ctx->filedDecl()) {
        auto field = any_cast_p<StructFieldNode>(visit(fieldCtx));
        structDecl->addField(field);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    return p<StructDeclNode>(structDecl);
}

std::any ASTBuilder::visitStructImpl(yux::yuxParser::StructImplContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto structImpl = create<StructImplNode>(file, ctx->name);

    DEBUG_LOG_VAL("Visit: StructImpl", ctx->name->getText());

    stack.emplace_back(structImpl);
    _scopeStack.push_back(structImpl);

    string structName = ctx->name->getText();

    for (auto fnCtx : ctx->fn()) {
        auto header = any_cast_p<FnHeaderNode>(visitFnHeader(fnCtx->fnHeader()));
        auto fn = create<FnNode>(structImpl, header);
        fn->setParentScope(file);

        stack.emplace_back(fn);
        _scopeStack.push_back(fn);

        fn->registerSymbol("self", {SymbolKind::Variable, "self", TypeInfo(structName)});

        for (auto param : header->params()) {
            TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
            fn->registerSymbol(param->name()->getText(), {SymbolKind::Variable, param->name()->getText(), paramType});
        }

        if (fnCtx->fnBody()->fnExprkBody()) {
            auto exprBody = fnCtx->fnBody()->fnExprkBody();
            auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
            auto retStmt = create<StatementRetNode>(fn, expr);
            fn->addStatement(retStmt);
        } else if (fnCtx->fnBody()->fnBlockBody()) {
            auto blockBody = fnCtx->fnBody()->fnBlockBody();
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

        structImpl->addMethod(fn);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    return p<StructImplNode>(structImpl);
}

std::any ASTBuilder::visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) {
    auto parent = currentScope();
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    DEBUG_LOG_VAL("    Field", ctx->name->getText() << " : " << type->getType().name);
    return p<StructFieldNode>(create<StructFieldNode>(parent, ctx->name, type));
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
    p<TypeNode> type = nullptr;
    if (ctx->type()) {
        type = any_cast_p<TypeNode>(visit(ctx->type()));
    }

    TypeInfo varType;
    if (type) {
        varType = type->getType();
    } else {
        varType = expr->getType();
    }

    DEBUG_LOG_VAL("  Statement: Declare", name->getText() << " : " << varType.name << " (" << declKey << ")");

    if (scope) {
        scope->registerSymbol(
            name->getText(), {SymbolKind::Variable, name->getText(), varType, declType == DeclareType::Var});
    }

    return p<StatementNode>(create<StatementDeclareAssignNode>(scope, declType, name, type, expr));
}

std::any ASTBuilder::visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    vector<Token> subs;
    for (auto sub : ctx->subs) {
        subs.push_back(sub);
    }
    DEBUG_LOG_VAL("  Statement: Assign", ctx->obj->getText() << (subs.empty() ? "" : "." + subs[0]->getText()));
    return p<StatementNode>(create<StatementAssignNode>(scope, ctx->obj, subs, expr));
}

std::any ASTBuilder::visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    bool hasSemicolon = ctx->SymbolSemicolon() != nullptr;
    DEBUG_LOG_VAL("  Statement: Expression", (hasSemicolon ? "with semicolon" : "without semicolon"));
    return p<StatementNode>(create<StatementExprNode>(scope, expr, hasSemicolon));
}

std::any ASTBuilder::visitStatementRet(yux::yuxParser::StatementRetContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    DEBUG_LOG("  Statement: Return");
    return p<StatementNode>(create<StatementRetNode>(scope, expr));
}

std::any ASTBuilder::visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) {
    auto scope = currentScope();
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    DEBUG_LOG("  Statement: Loop");
    return p<StatementNode>(create<StatementLoopNode>(scope, block));
}

std::any ASTBuilder::visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("  Statement: Break");
    return p<StatementNode>(create<StatementBreakNode>(scope));
}

std::any ASTBuilder::visitStatementSet(yux::yuxParser::StatementSetContext* ctx) {
    DEBUG_LOG("  Statement: ArraySet");
    auto scope = currentScope();

    auto arrayExpr = any_cast_p<ExprNode>(visit(ctx->obj));

    vector<p<ExprNode>> indices;
    for (auto arg : ctx->args) {
        indices.push_back(any_cast_p<ExprNode>(visit(arg)));
    }

    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    return p<StatementNode>(create<StatementSetNode>(scope, arrayExpr, indices, valueExpr));
}

std::any ASTBuilder::visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) {
    DEBUG_LOG("  Visit: StatementBlock");
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
                DEBUG_LOG("    Block has result expression");
            }
        }
    }

    DEBUG_LOG_VAL("    Statements count", statements.size());
    auto block = create<StatementBlockNode>(parentScope, statements, resultExpr, hasResult);
    block->setParentScope(parentScope);
    return p<StatementBlockNode>(block);
}

std::any ASTBuilder::visitExprParen(yux::yuxParser::ExprParenContext* ctx) {
    DEBUG_LOG("    Expr: Paren");
    auto scope = currentScope();
    auto inner = any_cast_p<ExprNode>(visit(ctx->expr()));
    return p<ExprNode>(create<ExprParenNode>(scope, inner));
}

std::any ASTBuilder::visitExprCall(yux::yuxParser::ExprCallContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG_VAL("    Expr: Call - ctx->left type", typeid(*ctx->left).name());
    auto callee = any_cast_p<ExprNode>(visit(ctx->left));
    DEBUG_LOG_VAL("    Expr: Call - callee type", typeid(*callee).name());

    auto call = create<ExprCallNode>(scope, callee);
    DEBUG_LOG_VAL("    Expr: Call", "args count: " << ctx->args.size());
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

    DEBUG_LOG_VAL("    Expr: AddSub", opText);
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

    DEBUG_LOG_VAL("    Expr: MulDivMod", opText);
    return p<ExprNode>(create<ExprMulDivModNode>(scope, op, left, right));
}

std::any ASTBuilder::visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) {
    DEBUG_LOG("    Expr: Literal");
    auto scope = currentScope();
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    return p<ExprNode>(create<ExprLiteralNode>(scope, literal));
}

std::any ASTBuilder::visitExprDot(yux::yuxParser::ExprDotContext* ctx) {
    DEBUG_LOG("    Expr: Dot - visitExprDot called");
    auto scope = currentScope();
    auto base = any_cast_p<ExprNode>(visit(ctx->left));
    DEBUG_LOG_VAL("    Expr: Dot - member count", ctx->member.size());
    for (size_t i = 0; i < ctx->member.size(); ++i) {
        DEBUG_LOG_VAL("    Expr: Dot - member[" + to_string(i) + "]", ctx->member[i]->getText());
    }
    DEBUG_LOG_VAL("    Expr: Dot", ctx->member.back()->getText());
    auto result = p<ExprNode>(create<ExprDotNode>(scope, base, ctx->member.back()));
    DEBUG_LOG_VAL("    Expr: Dot - result type", typeid(*result).name());
    return result;
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

    DEBUG_LOG_VAL("    Expr: Compare", opText);
    return p<ExprNode>(create<ExprCompareNode>(scope, op, left, right));
}

std::any ASTBuilder::visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) {
    DEBUG_LOG("      Literal: Number");
    return p<LiteralNode>(any_cast_p<LiteralNode>(visit(ctx->num)));
}

std::any ASTBuilder::visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) {
    auto token = ctx->True() ? ctx->True()->getSymbol() : ctx->False()->getSymbol();
    DEBUG_LOG_VAL("      Literal: Bool", (ctx->True() ? "true" : "false"));
    return p<LiteralNode>(create<LiteralBoolNode>(token));
}

std::any ASTBuilder::visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG_VAL("      Literal: Object", ctx->name->getText());
    return p<LiteralNode>(create<LiteralObjNode>(scope, ctx->name));
}

std::any ASTBuilder::visitNumInt(yux::yuxParser::NumIntContext* ctx) {
    DEBUG_LOG_VAL("        Num: Int", ctx->INT()->getSymbol()->getText());
    return p<LiteralNode>(create<LiteralIntNode>(ctx->INT()->getSymbol()));
}

std::any ASTBuilder::visitNumFloat(yux::yuxParser::NumFloatContext* ctx) {
    DEBUG_LOG_VAL("        Num: Float", ctx->FLOAT()->getSymbol()->getText());
    return p<LiteralNode>(create<LiteralFloatNode>(ctx->FLOAT()->getSymbol()));
}

std::any ASTBuilder::visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) {
    DEBUG_LOG("    Expr: IfElse");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto thenBlock = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));

    vector<p<ExprElIfNode>> elifs;
    DEBUG_LOG_VAL("      Elif count", ctx->elifs.size());
    for (auto elifCtx : ctx->elifs) {
        auto elif = any_cast_p<ExprElIfNode>(visit(elifCtx));
        elifs.push_back(elif);
    }

    p<StatementBlockNode> elseBlock = nullptr;
    if (ctx->exprElse()) {
        DEBUG_LOG("      Has else block");
        elseBlock = any_cast_p<StatementBlockNode>(visit(ctx->exprElse()));
    }

    return p<ExprNode>(create<ExprIfElseNode>(scope, condition, thenBlock, elifs, elseBlock));
}

std::any ASTBuilder::visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) {
    DEBUG_LOG("      Visit: Elif");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    return p<ExprElIfNode>(create<ExprElIfNode>(scope, condition, block));
}

std::any ASTBuilder::visitExprElse(yux::yuxParser::ExprElseContext* ctx) {
    DEBUG_LOG("      Visit: Else");
    return visit(ctx->statementBlock());
}

std::any ASTBuilder::visitExprGet(yux::yuxParser::ExprGetContext* ctx) {
    DEBUG_LOG("visitExprGet called");
    auto scope = currentScope();
    auto exprs = ctx->expr();
    auto arrayExpr = any_cast_p<ExprNode>(visit(exprs[0]));

    vector<p<ExprNode>> indices;
    for (size_t i = 1; i < exprs.size(); ++i) {
        indices.push_back(any_cast_p<ExprNode>(visit(exprs[i])));
    }

    return p<ExprNode>(create<ExprGetNode>(scope, arrayExpr, indices));
}

std::any ASTBuilder::visitExprArray(yux::yuxParser::ExprArrayContext* ctx) {
    DEBUG_LOG_VAL("    Expr: Array", "elements: " << ctx->velues.size());
    auto scope = currentScope();
    vector<p<ExprNode>> elements;

    for (auto elemCtx : ctx->velues) {
        elements.push_back(any_cast_p<ExprNode>(visit(elemCtx)));
    }

    return p<ExprNode>(create<ExprArrayNode>(scope, elements));
}

std::any ASTBuilder::visitExprArrayInit(yux::yuxParser::ExprArrayInitContext* ctx) {
    DEBUG_LOG("    Expr: ArrayInit");
    auto scope = currentScope();

    auto literalCtx = ctx->literal();
    auto literal = any_cast_p<LiteralNode>(visit(literalCtx));

    p<TypeNode> explicitType = nullptr;
    if (ctx->type()) {
        explicitType = any_cast_p<TypeNode>(visit(ctx->type()));
    }

    return p<ExprNode>(create<ExprArrayInitNode>(scope, literal, explicitType));
}

std::any ASTBuilder::visitType(yux::yuxParser::TypeContext* ctx) {
    if (ctx->typeNormal()) {
        return visit(ctx->typeNormal());
    }
    return visit(ctx->typeArray());
}

std::any ASTBuilder::visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) {
    p<Node> parent = currentScope();
    DEBUG_LOG_VAL("    Type: Normal", ctx->ID()->getSymbol()->getText());
    return p<TypeNode>(create<TypeNormalNode>(parent, ctx->ID()->getSymbol()));
}

std::any ASTBuilder::visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) {
    p<Node> parent = currentScope();
    auto elementType = any_cast_p<TypeNode>(visit(ctx->type()));
    auto count = ctx->INT()->getSymbol();
    DEBUG_LOG_VAL("    Type: Array", "[" << count->getText() << "]");
    return p<TypeNode>(create<TypeArrayNode>(parent, elementType, count));
}

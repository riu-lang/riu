// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 语句族实现：
//   - visitStatementLet / visitStatementLetTuple / visitStatementAssign
//   - visitStatementExpr / visitStatementRet / visitStatementRetVoid
//   - visitStatementLoop / visitStatementBreak / visitStatementSet
//   - visitStatementBlock
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "types.h"
#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include <algorithm>

// DRAFT-let-unify §3：局部 `let` 声明。
// 注解映射：默认 → isMut=false（不可重赋）；#Mut → isMut=true；#Cval → isConst=true；
// #Frozen → isMut=false + frozen 位（语义同 const-mut §5 局部，深不可变由 checker 传染）。
// type / init 缺失：缺 type 且缺 init → E3113；有 type 但缺 init → E3114（不引入 #Uninit）。
std::any ASTBuilder::visitStatementLet(yux::yuxParser::StatementLetContext* ctx) {
    auto scope = currentScope();
    auto name = ctx->name;
    auto flags = readLetAnnos(ctx->letAnnos);

    bool hasType = ctx->typeWithRef() != nullptr;
    bool hasInit = ctx->expr() != nullptr;

    if (!hasType && !hasInit) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3113, name->getText());
    }
    // 仅 #Mut 允许 `let x T`（延后赋值）；默认 val / #Cval / #Frozen 强制 init。
    if (hasType && !hasInit && !flags.isMut) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3114, name->getText());
    }

    bool isMut = flags.isMut;
    bool isConst = flags.isCval;

    p<TypeNode> type = nullptr;
    if (hasType) {
        type = buildTypeWithRef(ctx->typeWithRef(), scope);
    }

    // #Mut 延后赋形态：`#Mut let x T` 走 StatementDeclareNode（无 init）。
    if (!hasInit) {
        TypeInfo varType = type->getType();
        DEBUG_LOG_VAL("  Statement: Let (no init #Mut)", name->getText() << " : " << varType.name);
        if (scope) {
            scope->registerSymbol(name->getText(), {SymbolKind::Variable, name->getText(), varType, true});
        }
        return static_cast<p<StatementNode>>(
            createWithLine<StatementDeclareNode>(ctx, scope, isMut, isConst, name, type));
    }

    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    TypeInfo varType = type ? type->getType() : expr->getType();

    DEBUG_LOG_VAL("  Statement: Let", name->getText()
                                          << " : " << varType.name << (flags.isMut ? " #Mut" : "")
                                          << (flags.isFrozen ? " #Frozen" : "") << (flags.isCval ? " #Cval" : ""));

    if (scope) {
        SymbolInfo sym(SymbolKind::Variable, name->getText(), varType, isMut);
        if (isConst) sym.isConst = true;
        scope->registerSymbol(name->getText(), sym);
    }

    return static_cast<p<StatementNode>>(
        createWithLine<StatementDeclareAssignNode>(ctx, scope, isMut, isConst, name, type, expr));
}

// DRAFT-let-unify §3：let 元组解构（默认 → 不可重赋 / #Mut → isMut=true / #Cval → isConst=true）。
// 注解 → bool 标志复用 readLetAnnos；后续 alias / 元素类型登记逻辑与 visitStatementLet 同。
std::any ASTBuilder::visitStatementLetTuple(yux::yuxParser::StatementLetTupleContext* ctx) {
    auto scope = currentScope();
    auto flags = readLetAnnos(ctx->letAnnos);
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

    bool isMut = flags.isMut;
    bool isConst = flags.isCval;

    p<TypeNode> type = nullptr;
    if (auto twr = ctx->typeWithRef(); twr) {
        type = buildTypeWithRef(twr, scope);
    }

    TypeInfo wholeType = type ? type->getType() : expr->getType();

    if (auto* file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0])) {
        std::set<std::string> visited;
        TypeInfo cur = wholeType;
        while (cur.kind == TypeKind::Normal) {
            auto* alias = file->getAliasDecl(cur.name);
            if (!alias || alias->isGeneric() || !alias->target()) break;
            if (visited.count(cur.name)) break;
            visited.insert(cur.name);
            cur = alias->target()->getType();
        }
        wholeType = cur;
    }

    vector<Token> names;
    names.reserve(ctx->names.size());
    for (auto idTok : ctx->names) {
        names.emplace_back(idTok);
    }

    if (scope && wholeType.isTuple() && wholeType.tupleElements().size() == names.size()) {
        const auto& elems = wholeType.tupleElements();
        for (size_t i = 0; i < names.size(); ++i) {
            scope->registerSymbol(names[i].getText(), {SymbolKind::Variable, names[i].getText(), *elems[i], isMut});
        }
    } else if (scope) {
        for (auto& n : names) {
            scope->registerSymbol(n.getText(), {SymbolKind::Variable, n.getText(), TypeInfo(), isMut});
        }
    }

    DEBUG_LOG_VAL("  Statement: LetTuple", names.size() << " names, expr type=" << wholeType.name);
    return static_cast<p<StatementNode>>(
        createWithLine<StatementDeclareAssignTupleNode>(ctx, scope, isMut, isConst, names, type, expr));
}

std::any ASTBuilder::visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    vector<Token> subs;
    // DOT_NUM token (如 ".0") 文本带前导 '.'，剥掉以保持 sub 仅是字段名 / 数字
    for (auto sub : ctx->subs) {
        auto text = sub->getText();
        if (!text.empty() && text[0] == '.') {
            subs.emplace_back(text.substr(1), sub->getLine());
        } else {
            subs.emplace_back(sub);
        }
    }

    AssignOp op = AssignOp::Eq;
    if (ctx->opAssign()) {
        auto opText = ctx->opAssign()->getText();
        if (opText == "+=")
            op = AssignOp::AddEq;
        else if (opText == "-=")
            op = AssignOp::SubEq;
        else if (opText == "*=")
            op = AssignOp::MulEq;
        else if (opText == "/=")
            op = AssignOp::DivEq;
        else if (opText == "%=")
            op = AssignOp::ModEq;
        else if (opText == "^=")
            op = AssignOp::XorEq;
        else if (opText == ">>=")
            op = AssignOp::MtMtEq;
        else if (opText == "<<=")
            op = AssignOp::LtLtEq;
    }

    Token objToken =
        (ctx->obj->getType() == yux::yuxParser::SymbolThis) ? Token("$", ctx->obj->getLine()) : Token(ctx->obj);

    DEBUG_LOG_VAL("  Statement: Assign", objToken.getText() << (subs.empty() ? "" : "." + subs[0].getText()));
    return static_cast<p<StatementNode>>(createWithLine<StatementAssignNode>(ctx, scope, objToken, subs, expr, op));
}

std::any ASTBuilder::visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    bool hasSemicolon = ctx->SymbolSemicolon() != nullptr;
    DEBUG_LOG_VAL("  Statement: Expression", (hasSemicolon ? "with semicolon" : "without semicolon"));
    return static_cast<p<StatementNode>>(createWithLine<StatementExprNode>(ctx, scope, expr, hasSemicolon));
}

std::any ASTBuilder::visitStatementRet(yux::yuxParser::StatementRetContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    DEBUG_LOG("  Statement: Return");
    auto retStmt = createWithLine<StatementRetNode>(ctx, scope, expr);
    retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
    return static_cast<p<StatementNode>>(retStmt);
}

std::any ASTBuilder::visitStatementRetVoid(yux::yuxParser::StatementRetVoidContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("  Statement: Return Void");
    return static_cast<p<StatementNode>>(createWithLine<StatementRetVoidNode>(ctx, scope));
}

std::any ASTBuilder::visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) {
    auto scope = currentScope();
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));

    // 处理可选的 label 前缀：label: loop { }
    Token label;
    if (ctx->ID()) {
        label = Token(ctx->ID()->getText(), static_cast<int>(ctx->ID()->getSymbol()->getLine()));
    }

    // 处理可选的 loop init 子句
    vector<Token> initNames;
    p<TypeNode> initType = nullptr;
    p<ExprNode> initExpr = nullptr;

    if (auto initCtx = ctx->loopInit(); initCtx) {
        // 提取变量名
        if (initCtx->name) {
            // 单变量：loop i = expr
            initNames.emplace_back(initCtx->name);
        } else {
            // tuple 解构：loop (i, j) = expr
            for (auto id : initCtx->names) {
                initNames.emplace_back(id);
            }
        }

        // 可选类型标注
        if (auto twr = initCtx->typeWithRef(); twr) {
            initType = buildTypeWithRef(twr, scope);
        }

        // init 表达式
        initExpr = any_cast_p<ExprNode>(visit(initCtx->expr()));

        // 在函数作用域中注册 loop init 变量（yux 无独立块作用域，所有局部变量平级）
        // 变量默认可变
        TypeInfo initExprType = initExpr->getType();
        for (size_t idx = 0; idx < initNames.size(); ++idx) {
            TypeInfo varType;
            if (initType && initNames.size() == 1) {
                varType = initType->getType();
            } else if (initExprType.isTuple() && idx < initExprType.tupleElements().size()) {
                varType = *initExprType.tupleElements()[idx];
            } else {
                varType = initExprType;
            }
            SymbolInfo sym(SymbolKind::Variable, initNames[idx].getText(), varType, /*w=*/true);
            if (scope) {
                scope->registerSymbol(initNames[idx].getText(), sym);
            }
        }
    }

    DEBUG_LOG("  Statement: Loop" << (ctx->loopInit() ? " (with init)" : "")
                                  << (ctx->ID() ? " (label: " + label.getText() + ")" : ""));
    return static_cast<p<StatementNode>>(
        createWithLine<StatementLoopNode>(ctx, scope, block, label, std::move(initNames), initType, initExpr));
}

std::any ASTBuilder::visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) {
    auto scope = currentScope();
    Token label;
    if (ctx->ID()) {
        label = Token(ctx->ID()->getText(), static_cast<int>(ctx->ID()->getSymbol()->getLine()));
    }
    DEBUG_LOG("  Statement: Break" << (ctx->ID() ? " (label: " + label.getText() + ")" : ""));
    return static_cast<p<StatementNode>>(createWithLine<StatementBreakNode>(ctx, scope, label));
}

std::any ASTBuilder::visitStatementSet(yux::yuxParser::StatementSetContext* ctx) {
    DEBUG_LOG("  Statement: ArraySet");
    auto scope = currentScope();

    auto arrayExpr = any_cast_p<ExprNode>(visit(ctx->obj));

    vector<p<ExprNode>> indices;
    indices.reserve(ctx->args.size());
    for (auto arg : ctx->args) {
        indices.push_back(any_cast_p<ExprNode>(visit(arg)));
    }

    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    return static_cast<p<StatementNode>>(createWithLine<StatementSetNode>(ctx, scope, arrayExpr, indices, valueExpr));
}

// DRAFT-static-vars Phase 5：静态字段写语句（Type::FIELD = expr）
std::any ASTBuilder::visitStatementStaticFieldSet(yux::yuxParser::StatementStaticFieldSetContext* ctx) {
    auto scope = currentScope();
    Token typeName(ctx->typeName);
    Token fieldName(ctx->fieldName);
    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    DEBUG_LOG_VAL("  Statement: StaticFieldSet", typeName.getText() << "::" << fieldName.getText() << " = ...");
    return static_cast<p<StatementNode>>(
        createWithLine<StatementStaticFieldSetNode>(ctx, scope, typeName, fieldName, valueExpr));
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
    auto block = createWithLine<StatementBlockNode>(ctx, parentScope, statements, resultExpr, hasResult);
    block->setParentScope(parentScope);
    return block;
}

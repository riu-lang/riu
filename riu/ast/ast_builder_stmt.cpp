// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 语句族实现：
//   - visitStatementLet / visitStatementLetTuple / visitStatementAssign
//   - visitStatementExpr / visitStatementRet / visitStatementRetVoid
//   - visitStatementLoop / visitStatementForIn / visitStatementBreak / visitStatementContinue
//   - visitStatementSet / visitStatementBlock
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "types.h"
#include <algorithm>
#include <memory>

// DRAFT-let-unify §3：局部 `let` 声明。
// 注解映射：默认 → isMut=false（不可重赋）；#Mut → isMut=true；#Cval → isConst=true；
// #Frozen → isMut=false + frozen 位（语义同 const-mut §5 局部，深不可变由 checker 传染）。
// type / init 缺失：缺 type 且缺 init → E3113；有 type 但缺 init → E3114（不引入 #Uninit）。
std::any ASTBuilder::visitStatementLet(riu::riuParser::StatementLetContext* ctx) {
    auto scope = currentScope();
    auto name = ctx->name;
    auto flags = readLetAnnos(ctx->letAnnos);

    bool hasType = ctx->type() != nullptr;
    bool hasInit = ctx->expr() != nullptr;

    if (!hasType && !hasInit) {
        throw RiuError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3113, name->getText());
    }

    bool isMut = flags.isMut;
    bool isConst = flags.isCval;

    TypeNode* type = nullptr;
    if (hasType) {
        type = any_cast_p<TypeNode>(visit(ctx->type()));
    }

    // §5.1.3.1：T& 必须 init（写穿 vs 重指向歧义）；#Mut 延后赋值例外不适用于引用。
    if (hasType && !hasInit && type->getType().isRef()) {
        throw RiuError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3114, name->getText());
    }
    // 仅 #Mut 允许 `let x T`（延后赋值）；默认 val / #Cval / #Frozen 强制 init。
    if (hasType && !hasInit && !flags.isMut) {
        throw RiuError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3114, name->getText());
    }

    // #Mut 延后赋形态：`#Mut let x T` 走 StatementDeclareNode（无 init）。
    if (!hasInit) {
        TypeInfo varType = type->getType();
        DEBUG_LOG_VAL("  Statement: Let (no init #Mut)", name->getText() << " : " << varType.name);
        if (scope) {
            scope->registerSymbol(name->getText(), {SymbolKind::Variable, name->getText(), varType, true});
        }
        auto* decl = createWithLine<StatementDeclareNode>(ctx, scope, isMut, isConst, name, type);
        decl->setFrozen(flags.isFrozen);
        return static_cast<StatementNode*>(decl);
    }

    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    TypeInfo varType = type ? type->getType() : expr->getType();

    DEBUG_LOG_VAL("  Statement: Let", name->getText()
                                          << " : " << varType.name << (flags.isMut ? " #Mut" : "")
                                          << (flags.isFrozen ? " #Frozen" : "") << (flags.isCval ? " #Cval" : ""));

    if (scope) {
        SymbolInfo sym(SymbolKind::Variable, name->getText(), varType, isMut);
        if (isConst) sym.isConst = true;
        if (flags.isFrozen) {
            sym.isFrozen = true;
        }
        scope->registerSymbol(name->getText(), sym);
    }

    auto* assign = createWithLine<StatementDeclareAssignNode>(ctx, scope, isMut, isConst, name, type, expr);
    assign->setFrozen(flags.isFrozen);
    return static_cast<StatementNode*>(assign);
}

// DRAFT-let-unify §3：let 元组解构（默认 → 不可重赋 / #Mut → isMut=true / #Cval → isConst=true / #Frozen → isFrozen）。
// 注解 → bool 标志复用 readLetAnnos；后续 alias / 元素类型登记逻辑与 visitStatementLet 同。
std::any ASTBuilder::visitStatementLetTuple(riu::riuParser::StatementLetTupleContext* ctx) {
    auto scope = currentScope();
    auto flags = readLetAnnos(ctx->letAnnos);
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

    bool isMut = flags.isMut;
    bool isConst = flags.isCval;

    TypeNode* type = nullptr;
    if (auto t = ctx->type(); t) {
        type = any_cast_p<TypeNode>(visit(t));
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

    auto registerLetName = [&](const string& n, TypeInfo t) {
        SymbolInfo sym(SymbolKind::Variable, n, std::move(t), isMut);
        if (isConst) {
            sym.isConst = true;
        }
        if (flags.isFrozen) {
            sym.isFrozen = true;
        }
        scope->registerSymbol(n, std::move(sym));
    };
    if (scope && wholeType.isTuple() && wholeType.tupleElements().size() == names.size()) {
        const auto& elems = wholeType.tupleElements();
        for (size_t i = 0; i < names.size(); ++i) {
            registerLetName(names[i].getText(), *elems[i]);
        }
    } else if (scope) {
        for (auto& n : names) {
            registerLetName(n.getText(), TypeInfo());
        }
    }

    DEBUG_LOG_VAL("  Statement: LetTuple", names.size() << " names, expr type=" << wholeType.name);
    auto* tup = createWithLine<StatementDeclareAssignTupleNode>(ctx, scope, isMut, isConst, names, type, expr);
    tup->setFrozen(flags.isFrozen);
    return static_cast<StatementNode*>(tup);
}

std::any ASTBuilder::visitStatementAssign(riu::riuParser::StatementAssignContext* ctx) {
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
    }

    Token objToken =
        (ctx->obj->getType() == riu::riuParser::SymbolThis) ? Token("$", ctx->obj->getLine()) : Token(ctx->obj);

    DEBUG_LOG_VAL("  Statement: Assign", objToken.getText() << (subs.empty() ? "" : "." + subs[0].getText()));
    return static_cast<StatementNode*>(createWithLine<StatementAssignNode>(ctx, scope, objToken, subs, expr, op));
}

std::any ASTBuilder::visitStatementExpr(riu::riuParser::StatementExprContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    bool hasSemicolon = ctx->SymbolSemicolon() != nullptr;
    DEBUG_LOG_VAL("  Statement: Expression", (hasSemicolon ? "with semicolon" : "without semicolon"));
    return static_cast<StatementNode*>(createWithLine<StatementExprNode>(ctx, scope, expr, hasSemicolon));
}

std::any ASTBuilder::visitStatementRet(riu::riuParser::StatementRetContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    DEBUG_LOG("  Statement: Return");
    auto retStmt = createWithLine<StatementRetNode>(ctx, scope, expr);
    retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
    return static_cast<StatementNode*>(retStmt);
}

std::any ASTBuilder::visitStatementRetVoid(riu::riuParser::StatementRetVoidContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("  Statement: Return Void");
    return static_cast<StatementNode*>(createWithLine<StatementRetVoidNode>(ctx, scope));
}

std::any ASTBuilder::visitStatementLoop(riu::riuParser::StatementLoopContext* ctx) {
    auto outerScope = currentScope();

    // 处理可选的 label 前缀：label: loop { }
    Token label;
    if (ctx->ID()) {
        label = Token(ctx->ID()->getText(), static_cast<int>(ctx->ID()->getSymbol()->getLine()));
    }

    // loop init：RHS 在外层求值（init 名对 RHS 不可见）；符号注册进 loop 体 block（§5.5.1.5）
    vector<Token> initNames;
    TypeNode* initType = nullptr;
    ExprNode* initExpr = nullptr;

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
        if (auto t = initCtx->type(); t) {
            initType = any_cast_p<TypeNode>(visit(t));
        }

        // init 表达式
        initExpr = any_cast_p<ExprNode>(visit(initCtx->expr()));
    }

    // 先建 block scope 并 push，再注册 init，再 visit 体——体内才能看见 init 名
    auto blockCtx = ctx->statementBlock();
    auto block = createWithLine<StatementBlockNode>(blockCtx, outerScope, vector<StatementNode*>{}, nullptr, false);
    block->setParentScope(outerScope);
    _scopeStack.push_back(block);

    if (initExpr) {
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
            // loop init 默认可变（§5.5.1.5）
            block->registerSymbol(initNames[idx].getText(),
                                  SymbolInfo(SymbolKind::Variable, initNames[idx].getText(), varType, /*w=*/true));
        }
    }

    vector<StatementNode*> statements;
    for (auto stmtCtx : blockCtx->statement()) {
        statements.push_back(any_cast_p<StatementNode>(visit(stmtCtx)));
    }

    ExprNode* resultExpr = nullptr;
    bool hasResult = false;
    if (!statements.empty()) {
        if (auto exprStmt = dynamic_cast<StatementExprNode*>(statements.back())) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
            }
        }
    }

    _scopeStack.pop_back();

    // 最终 block 持有 statements；init 符号拷入。体内节点 parent 仍是 push 期的 block（含 init），
    // findNearestScope 可解析 init / 体 let。
    auto filled =
        createWithLine<StatementBlockNode>(blockCtx, outerScope, std::move(statements), resultExpr, hasResult);
    filled->setParentScope(outerScope);
    for (auto& [name, sym] : block->localSymbols()) {
        filled->registerSymbol(name, sym);
    }
    filled->copyLocalAliasesFrom(block);

    DEBUG_LOG("  Statement: Loop" << (ctx->loopInit() ? " (with init)" : "")
                                  << (ctx->ID() ? " (label: " + label.getText() + ")" : ""));
    return static_cast<StatementNode*>(
        createWithLine<StatementLoopNode>(ctx, outerScope, filled, label, std::move(initNames), initType, initExpr));
}

std::any ASTBuilder::visitStatementBreak(riu::riuParser::StatementBreakContext* ctx) {
    auto scope = currentScope();
    Token label;
    if (ctx->ID()) {
        label = Token(ctx->ID()->getText(), static_cast<int>(ctx->ID()->getSymbol()->getLine()));
    }
    DEBUG_LOG("  Statement: Break" << (ctx->ID() ? " (label: " + label.getText() + ")" : ""));
    return static_cast<StatementNode*>(createWithLine<StatementBreakNode>(ctx, scope, label));
}

std::any ASTBuilder::visitStatementContinue(riu::riuParser::StatementContinueContext* ctx) {
    auto scope = currentScope();
    Token label;
    if (ctx->ID()) {
        label = Token(ctx->ID()->getText(), static_cast<int>(ctx->ID()->getSymbol()->getLine()));
    }
    DEBUG_LOG("  Statement: Continue" << (ctx->ID() ? " (label: " + label.getText() + ")" : ""));
    return static_cast<StatementNode*>(createWithLine<StatementContinueNode>(ctx, scope, label));
}

std::any ASTBuilder::visitStatementForIn(riu::riuParser::StatementForInContext* ctx) {
    auto outerScope = currentScope();

    Token label;
    Token item;
    auto ids = ctx->ID();
    if (ctx->SymbolColon() && ids.size() >= 2) {
        label = Token(ids[0]->getText(), static_cast<int>(ids[0]->getSymbol()->getLine()));
        item = Token(ids[1]->getText(), static_cast<int>(ids[1]->getSymbol()->getLine()));
    } else {
        item = Token(ids[0]->getText(), static_cast<int>(ids[0]->getSymbol()->getLine()));
    }

    auto collExpr = any_cast_p<ExprNode>(visit(ctx->expr()));

    auto blockCtx = ctx->statementBlock();
    auto block = createWithLine<StatementBlockNode>(blockCtx, outerScope, vector<StatementNode*>{}, nullptr, false);
    block->setParentScope(outerScope);
    _scopeStack.push_back(block);

    TypeInfo collType = collExpr->getType();
    TypeInfo peeled = collType.peelRef();
    sp<TypeInfo> elem;
    if (peeled.isArrayGeneric()) {
        elem = peeled.arrayGenericElementType();
    } else if (peeled.isArray() && peeled.elementType) {
        elem = peeled.elementType;
    }
    TypeInfo itemType = elem ? TypeInfo("Ref", {std::make_shared<TypeInfo>(*elem)}) : TypeInfo();
    // T& 写穿不依赖 writeable；禁重绑定
    block->registerSymbol(item.getText(), SymbolInfo(SymbolKind::Variable, item.getText(), itemType, /*w=*/false));

    vector<StatementNode*> statements;
    for (auto stmtCtx : blockCtx->statement()) {
        statements.push_back(any_cast_p<StatementNode>(visit(stmtCtx)));
    }

    ExprNode* resultExpr = nullptr;
    bool hasResult = false;
    if (!statements.empty()) {
        if (auto exprStmt = dynamic_cast<StatementExprNode*>(statements.back())) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
            }
        }
    }

    _scopeStack.pop_back();

    auto filled =
        createWithLine<StatementBlockNode>(blockCtx, outerScope, std::move(statements), resultExpr, hasResult);
    filled->setParentScope(outerScope);
    for (auto& [name, sym] : block->localSymbols()) {
        filled->registerSymbol(name, sym);
    }
    filled->copyLocalAliasesFrom(block);

    DEBUG_LOG("  Statement: ForIn item=" << item.getText()
                                         << (label.getText().empty() ? "" : " label=" + label.getText()));
    return static_cast<StatementNode*>(
        createWithLine<StatementForInNode>(ctx, outerScope, filled, std::move(item), collExpr, std::move(label)));
}

std::any ASTBuilder::visitStatementSet(riu::riuParser::StatementSetContext* ctx) {
    DEBUG_LOG("  Statement: ArraySet");
    auto scope = currentScope();

    auto arrayExpr = any_cast_p<ExprNode>(visit(ctx->obj));

    vector<ExprNode*> indices;
    indices.reserve(ctx->args.size());
    for (auto arg : ctx->args) {
        indices.push_back(any_cast_p<ExprNode>(visit(arg)));
    }

    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    return static_cast<StatementNode*>(createWithLine<StatementSetNode>(ctx, scope, arrayExpr, indices, valueExpr));
}

// DRAFT-static-vars Phase 5：静态字段写语句（Type::FIELD = expr）
std::any ASTBuilder::visitStatementStaticFieldSet(riu::riuParser::StatementStaticFieldSetContext* ctx) {
    auto scope = currentScope();
    auto typePath = typePathFromCtx(ctx->typeName);
    Token fieldName(ctx->fieldName);
    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    DEBUG_LOG_VAL("  Statement: StaticFieldSet", typePath.dotted() << "::" << fieldName.getText() << " = ...");
    return static_cast<StatementNode*>(
        createWithLine<StatementStaticFieldSetNode>(ctx, scope, std::move(typePath), fieldName, valueExpr));
}

std::any ASTBuilder::visitStatementAlias(riu::riuParser::StatementAliasContext* ctx) {
    return visit(ctx->aliasDecl());
}

std::any ASTBuilder::visitStatementBlock(riu::riuParser::StatementBlockContext* ctx) {
    DEBUG_LOG("  Visit: StatementBlock");
    auto parentScope = currentScope();
    // §5.7.1：statementBlock 产生新作用域；先 push 再 visit，let 注册到本 block
    auto block = createWithLine<StatementBlockNode>(ctx, parentScope, vector<StatementNode*>{}, nullptr, false);
    block->setParentScope(parentScope);
    _scopeStack.push_back(block);

    vector<StatementNode*> statements;
    for (auto stmtCtx : ctx->statement()) {
        statements.push_back(any_cast_p<StatementNode>(visit(stmtCtx)));
    }

    ExprNode* resultExpr = nullptr;
    bool hasResult = false;
    if (!statements.empty()) {
        if (auto exprStmt = dynamic_cast<StatementExprNode*>(statements.back())) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
                DEBUG_LOG("    Block has result expression");
            }
        }
    }
    _scopeStack.pop_back();

    DEBUG_LOG_VAL("    Statements count", statements.size());
    // 重建带 statements 的 block；visit 期符号已在栈上的 block 中，拷到最终节点。
    // 体内语句 parent 仍指向 push 期 block（符号表所在），findNearestScope 正确。
    auto filled = createWithLine<StatementBlockNode>(ctx, parentScope, std::move(statements), resultExpr, hasResult);
    filled->setParentScope(parentScope);
    for (auto& [name, sym] : block->localSymbols()) {
        filled->registerSymbol(name, sym);
    }
    filled->copyLocalAliasesFrom(block);
    return filled;
}

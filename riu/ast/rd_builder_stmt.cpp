// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "rd_builder.h"

#include "ast_builder.h"
#include "node/alias_node.h"
#include "node/expr_node.h"
#include "node/global_const_node.h"
#include "node/global_var_node.h"
#include "node/statement_node.h"
#include "node/type_node.h"

#include <set>

namespace {

bool isTypeKindStmt(rd::NodeKind k) {
    switch (k) {
    case rd::NodeKind::TypePath:
    case rd::NodeKind::TypeGeneric:
    case rd::NodeKind::TypeNullable:
    case rd::NodeKind::TypeFallible:
    case rd::NodeKind::TypeSelf:
    case rd::NodeKind::TypeArray:
    case rd::NodeKind::TypeUnit:
    case rd::NodeKind::TypeTuple:
        return true;
    default:
        return false;
    }
}

AssignOp assignOpFromKind(rd::Kind k) {
    switch (k) {
    case rd::Kind::SymbolAddEq:
        return AssignOp::AddEq;
    case rd::Kind::SymbolSubEq:
        return AssignOp::SubEq;
    case rd::Kind::SymbolMulEq:
        return AssignOp::MulEq;
    case rd::Kind::SymbolDivEq:
        return AssignOp::DivEq;
    case rd::Kind::SymbolModEq:
        return AssignOp::ModEq;
    default:
        return AssignOp::Eq;
    }
}

} // namespace

StatementBlockNode* RdBuilder::buildBlock(rd::NodeId id, ScopeNode* parentScope, bool extractResult) {
    auto* block = create<StatementBlockNode>(id, parentScope, vector<StatementNode*>{}, nullptr, false);
    block->setParentScope(parentScope);
    _scopeStack.push_back(block);
    vector<StatementNode*> statements;
    const auto& n = at(id);
    statements.reserve(n.children_count);
    for (rd::i32 i = 0; i < n.children_count; ++i) {
        if (auto* s = buildStmt(child(id, i))) statements.push_back(s);
    }
    ExprNode* resultExpr = nullptr;
    bool hasResult = false;
    if (extractResult && !statements.empty()) {
        if (auto* exprStmt = dynamic_cast<StatementExprNode*>(statements.back())) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
            }
        }
    }
    _scopeStack.pop_back();
    auto* filled = create<StatementBlockNode>(id, parentScope, std::move(statements), resultExpr, hasResult);
    filled->setParentScope(parentScope);
    for (auto& [name, sym] : block->localSymbols())
        filled->registerSymbol(name, sym);
    filled->copyLocalAliasesFrom(block);
    return filled;
}

StatementNode* RdBuilder::buildLet(rd::NodeId id, bool global) {
    auto* scope = currentScope();
    const auto& n = at(id);
    vector<rd::NodeId> annos;
    rd::i32 i = 0;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Anno) {
        annos.push_back(child(id, i));
        ++i;
    }
    auto flags = readLetAnnos(annos);

    if (n.kind == rd::NodeKind::LetTuple) {
        vector<Token> names;
        TypeNode* type = nullptr;
        ExprNode* expr = nullptr;
        // 孩子顺序：Ident 名…、可选类型、表达式。RHS 也可能是 Ident，不能从前往后把 Ident 全当名字。
        if (i < n.children_count) {
            const rd::i32 last = n.children_count - 1;
            expr = buildExpr(child(id, last));
            rd::i32 nameEnd = last;
            if (i < last && isTypeKindStmt(at(child(id, last - 1)).kind)) {
                type = buildType(child(id, last - 1));
                nameEnd = last - 1;
            }
            while (i < nameEnd && at(child(id, i)).kind == rd::NodeKind::Ident) {
                names.push_back(makeTok(child(id, i)));
                ++i;
            }
        }
        TypeInfo wholeType = type ? type->getType() : (expr ? expr->getType() : TypeInfo());
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
        auto registerLetName = [&](const string& nm, TypeInfo t) {
            SymbolInfo sym(SymbolKind::Variable, nm, std::move(t), flags.isMut);
            if (flags.isCval) sym.isConst = true;
            if (flags.isFrozen) sym.isFrozen = true;
            if (scope) scope->registerSymbol(nm, std::move(sym));
        };
        if (scope && wholeType.isTuple() && wholeType.tupleElements().size() == names.size()) {
            const auto& elems = wholeType.tupleElements();
            for (size_t k = 0; k < names.size(); ++k)
                registerLetName(names[k].getText(), *elems[k]);
        } else if (scope) {
            for (auto& nm : names)
                registerLetName(nm.getText(), TypeInfo());
        }
        auto* tup = create<StatementDeclareAssignTupleNode>(id, scope, flags.isMut, flags.isCval, names, type, expr);
        tup->setFrozen(flags.isFrozen);
        return static_cast<StatementNode*>(tup);
    }

    Token nameTok = makeTok(id);
    TypeNode* type = nullptr;
    ExprNode* expr = nullptr;
    if (i < n.children_count && isTypeKindStmt(at(child(id, i)).kind)) {
        type = buildType(child(id, i));
        ++i;
    }
    if (i < n.children_count) expr = buildExpr(child(id, i));

    if (global) {
        auto* file = dynamic_cast<FileNode*>(scope);
        if (!flags.isCval && !flags.isMut && flags.isFrozen) {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3116, nameTok.getText());
        }
        if (flags.isMut) {
            if (!type) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
            if (!expr) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3154, nameTok.getText());
            inferFlexibleIntForType(expr, type->getType());
            if (ASTBuilder::exprContainsTryCatch(expr)) {
                throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3155, nameTok.getText());
            }
            auto* globalVar = create<GlobalVarNode>(id, file, nameTok, type, expr, true);
            globalVar->setSourceText(srcSlice(n.pos));
            file->addGlobalVar(globalVar);
            return nullptr;
        }
        if (flags.isCval) {
            if (!type) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
            if (!expr) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3114, nameTok.getText());
            inferFlexibleIntForType(expr, type->getType());
            auto* globalConst = create<GlobalConstNode>(id, file, nameTok, type, expr, flags.isInline);
            globalConst->setSourceText(srcSlice(n.pos));
            file->addGlobalConst(globalConst);
            return nullptr;
        }
        if (flags.isInline) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3117);
        if (!type) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
        if (!expr) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3154, nameTok.getText());
        inferFlexibleIntForType(expr, type->getType());
        if (ASTBuilder::exprContainsTryCatch(expr)) {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3155, nameTok.getText());
        }
        auto* globalVar = create<GlobalVarNode>(id, file, nameTok, type, expr);
        globalVar->setSourceText(srcSlice(n.pos));
        file->addGlobalVar(globalVar);
        return nullptr;
    }

    bool hasType = type != nullptr;
    bool hasInit = expr != nullptr;
    if (!hasType && !hasInit) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
    if (hasType && !hasInit && type->getType().isRef()) {
        throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3114, nameTok.getText());
    }
    if (hasType && !hasInit && !flags.isMut) {
        throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3114, nameTok.getText());
    }
    if (!hasInit) {
        if (scope) {
            scope->registerSymbol(nameTok.getText(), {SymbolKind::Variable, nameTok.getText(), type->getType(), true});
        }
        auto* decl = create<StatementDeclareNode>(id, scope, flags.isMut, flags.isCval, nameTok, type);
        decl->setFrozen(flags.isFrozen);
        return static_cast<StatementNode*>(decl);
    }
    TypeInfo varType = type ? type->getType() : expr->getType();
    if (scope) {
        SymbolInfo sym(SymbolKind::Variable, nameTok.getText(), varType, flags.isMut);
        if (flags.isCval) sym.isConst = true;
        if (flags.isFrozen) sym.isFrozen = true;
        scope->registerSymbol(nameTok.getText(), sym);
    }
    auto* assign = create<StatementDeclareAssignNode>(id, scope, flags.isMut, flags.isCval, nameTok, type, expr);
    assign->setFrozen(flags.isFrozen);
    return static_cast<StatementNode*>(assign);
}

StatementNode* RdBuilder::buildStmt(rd::NodeId id) {
    if (id == rd::kEmptyNode) return nullptr;
    const auto& n = at(id);
    auto* scope = currentScope();
    switch (n.kind) {
    case rd::NodeKind::Let:
    case rd::NodeKind::LetTuple:
        return buildLet(id, false);
    case rd::NodeKind::Alias:
        return addAlias(id);
    case rd::NodeKind::Ret: {
        auto* expr = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto* ret = create<StatementRetNode>(id, scope, expr);
        if (expr) ret->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
        return static_cast<StatementNode*>(ret);
    }
    case rd::NodeKind::RetVoid:
        return static_cast<StatementNode*>(create<StatementRetVoidNode>(id, scope));
    case rd::NodeKind::Break:
        return static_cast<StatementNode*>(create<StatementBreakNode>(id, scope, makeTok(id)));
    case rd::NodeKind::Continue:
        return static_cast<StatementNode*>(create<StatementContinueNode>(id, scope, makeTok(id)));
    case rd::NodeKind::ExprStmt: {
        auto* expr = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        bool semi = n.op == rd::Kind::SymbolSemicolon;
        return static_cast<StatementNode*>(create<StatementExprNode>(id, scope, expr, semi));
    }
    case rd::NodeKind::Assign: {
        Token obj;
        vector<Token> subs;
        ExprNode* expr = nullptr;
        if (n.children_count > 0) {
            const auto& first = at(child(id, 0));
            obj = first.kind == rd::NodeKind::This ? Token("$", first.pos.line) : makeTok(child(id, 0));
        }
        for (rd::i32 i = 1; i + 1 < n.children_count; ++i) {
            Token t = makeTok(child(id, i));
            string text = t.getText();
            if (!text.empty() && text[0] == '.') t = Token(text.substr(1), t.getLine());
            subs.push_back(std::move(t));
        }
        if (n.children_count > 0) expr = buildExpr(child(id, n.children_count - 1));
        return static_cast<StatementNode*>(
            create<StatementAssignNode>(id, scope, obj, subs, expr, assignOpFromKind(n.op)));
    }
    case rd::NodeKind::Set: {
        auto* arr = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        vector<ExprNode*> idx;
        for (rd::i32 i = 1; i + 1 < n.children_count; ++i)
            idx.push_back(buildExpr(child(id, i)));
        auto* val = n.children_count > 1 ? buildExpr(child(id, n.children_count - 1)) : nullptr;
        return static_cast<StatementNode*>(create<StatementSetNode>(id, scope, arr, std::move(idx), val));
    }
    case rd::NodeKind::StaticFieldSet: {
        TypePath path =
            n.children_count > 0 ? pathFromDotted(at(child(id, 0)).value, at(child(id, 0)).pos) : TypePath();
        Token field = makeTok(id);
        auto* val = n.children_count > 1 ? buildExpr(child(id, 1)) : nullptr;
        return static_cast<StatementNode*>(create<StatementStaticFieldSetNode>(id, scope, std::move(path), field, val));
    }
    case rd::NodeKind::Loop: {
        Token label = n.value.empty() ? Token() : makeTok(id);
        vector<Token> initNames;
        TypeNode* initType = nullptr;
        ExprNode* initExpr = nullptr;
        rd::NodeId body = rd::kEmptyNode;
        rd::i32 i = 0;
        while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Ident) {
            initNames.push_back(makeTok(child(id, i)));
            ++i;
        }
        if (i < n.children_count && isTypeKindStmt(at(child(id, i)).kind)) {
            initType = buildType(child(id, i));
            ++i;
        }
        if (i < n.children_count && at(child(id, i)).kind != rd::NodeKind::Block) {
            initExpr = buildExpr(child(id, i));
            ++i;
        }
        if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Block) body = child(id, i);

        auto* outer = scope;
        auto* block = create<StatementBlockNode>(body == rd::kEmptyNode ? id : body, outer, vector<StatementNode*>{},
                                                 nullptr, false);
        block->setParentScope(outer);
        _scopeStack.push_back(block);
        if (initExpr) {
            TypeInfo initExprType = initExpr->getType();
            for (size_t idx = 0; idx < initNames.size(); ++idx) {
                TypeInfo varType;
                if (initType && initNames.size() == 1)
                    varType = initType->getType();
                else if (initExprType.isTuple() && idx < initExprType.tupleElements().size())
                    varType = *initExprType.tupleElements()[idx];
                else
                    varType = initExprType;
                block->registerSymbol(initNames[idx].getText(),
                                      SymbolInfo(SymbolKind::Variable, initNames[idx].getText(), varType, true));
            }
        }
        auto* filled = body != rd::kEmptyNode ? buildBlock(body, block) : block;
        _scopeStack.pop_back();
        if (filled != block) {
            for (auto& [nm, sym] : block->localSymbols())
                filled->registerSymbol(nm, sym);
            filled->copyLocalAliasesFrom(block);
        }
        return static_cast<StatementNode*>(
            create<StatementLoopNode>(id, outer, filled, label, initNames, initType, initExpr));
    }
    case rd::NodeKind::ForIn: {
        Token item = makeTok(id);
        Token label;
        auto* coll = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        rd::NodeId body =
            (n.children_count > 1 && at(child(id, 1)).kind == rd::NodeKind::Block) ? child(id, 1) : rd::kEmptyNode;
        if (n.children_count > 2 && at(child(id, n.children_count - 1)).kind == rd::NodeKind::Ident)
            label = makeTok(child(id, n.children_count - 1));
        auto* outer = scope;
        auto* block = create<StatementBlockNode>(body == rd::kEmptyNode ? id : body, outer, vector<StatementNode*>{},
                                                 nullptr, false);
        block->setParentScope(outer);
        _scopeStack.push_back(block);
        TypeInfo collType = coll ? coll->getType() : TypeInfo();
        TypeInfo peeled = collType.peelRef();
        sp<TypeInfo> elem;
        if (peeled.isArrayGeneric())
            elem = peeled.arrayGenericElementType();
        else if (peeled.isArray() && peeled.elementType)
            elem = peeled.elementType;
        TypeInfo itemType = elem ? TypeInfo("Ref", {std::make_shared<TypeInfo>(*elem)}) : TypeInfo();
        block->registerSymbol(item.getText(), SymbolInfo(SymbolKind::Variable, item.getText(), itemType, false));
        auto* filled = body != rd::kEmptyNode ? buildBlock(body, block) : block;
        _scopeStack.pop_back();
        if (filled != block) {
            for (auto& [nm, sym] : block->localSymbols())
                filled->registerSymbol(nm, sym);
            filled->copyLocalAliasesFrom(block);
        }
        return static_cast<StatementNode*>(create<StatementForInNode>(id, outer, filled, item, coll, label));
    }
    case rd::NodeKind::Block:
        return nullptr;
    default:
        return static_cast<StatementNode*>(create<StatementExprNode>(id, scope, buildExpr(id), /*hasSemicolon=*/true));
    }
}

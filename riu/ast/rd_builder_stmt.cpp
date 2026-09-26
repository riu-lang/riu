// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "rd_builder.h"

#include "ast_builder_helpers.h"
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
    vector<AnnoCall> resultPrefix;
    if (extractResult && !statements.empty()) {
        if (auto* exprStmt = dynamic_cast<StatementExprNode*>(statements.back())) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                resultPrefix = exprStmt->prefixAnnos();
                statements.pop_back();
            }
        }
    }
    _scopeStack.pop_back();
    auto* filled = create<StatementBlockNode>(id, parentScope, std::move(statements), resultExpr, hasResult);
    filled->setResultPrefixAnnos(std::move(resultPrefix));
    filled->setParentScope(parentScope);
    for (auto& [name, sym] : block->localSymbols())
        filled->registerSymbol(name, *sym);
    filled->copyLocalAliasesFrom(block);
    return filled;
}

namespace {

bool isLetFlagAnno(const string& name) {
    return name == "Mut" || name == "Frozen" || name == "Cval" || name == "Inline";
}

} // namespace

StatementNode* RdBuilder::buildLet(rd::NodeId id, bool global) {
    auto* scope = currentScope();
    const auto& n = at(id);
    vector<rd::NodeId> letFlagAnnos;
    vector<AnnoCall> prefixAnnos;
    rd::i32 i = 0;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Anno) {
        rd::NodeId a = child(id, i);
        const string name = string(at(a).value);
        if (isLetFlagAnno(name)) {
            letFlagAnnos.push_back(a);
        } else if (!knownAnnos().contains(name)) {
            throw RiuError(at(a).pos.line, at(a).pos.column + 1, ErrorCode::E3112, name);
        } else {
            prefixAnnos.push_back(buildAnnoCall(a));
        }
        ++i;
    }
    auto flags = readLetAnnos(letFlagAnnos);

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
        applyPrefixAnnos(tup, std::move(prefixAnnos));
        return static_cast<StatementNode*>(tup);
    }

    Token nameTok = makeTok(id);
    if (global) {
        checkDiscardDeclName(nameTok.getText(), "global", n.pos.line, n.pos.column + 1);
    }
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
            if (exprContainsTryCatch(expr)) {
                throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3155, nameTok.getText());
            }
            auto* globalVar = create<GlobalVarNode>(id, file, nameTok, type, expr, true);
            if (_keepSourceText) globalVar->setSourceText(srcSlice(n.pos));
            globalVar->setAnnoCalls(std::move(prefixAnnos));
            file->addGlobalVar(globalVar);
            return nullptr;
        }
        if (flags.isCval) {
            if (!type) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
            if (!expr) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3114, nameTok.getText());
            inferFlexibleIntForType(expr, type->getType());
            auto* globalConst = create<GlobalConstNode>(id, file, nameTok, type, expr, flags.isInline);
            if (_keepSourceText) globalConst->setSourceText(srcSlice(n.pos));
            globalConst->setAnnoCalls(std::move(prefixAnnos));
            file->addGlobalConst(globalConst);
            return nullptr;
        }
        if (flags.isInline) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3117);
        if (!type) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3113, nameTok.getText());
        if (!expr) throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3154, nameTok.getText());
        inferFlexibleIntForType(expr, type->getType());
        if (exprContainsTryCatch(expr)) {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E3155, nameTok.getText());
        }
        auto* globalVar = create<GlobalVarNode>(id, file, nameTok, type, expr);
        if (_keepSourceText) globalVar->setSourceText(srcSlice(n.pos));
        globalVar->setAnnoCalls(std::move(prefixAnnos));
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
        applyPrefixAnnos(decl, std::move(prefixAnnos));
        return static_cast<StatementNode*>(decl);
    }
    // 无标注时不要在 builder 里 getType：match 绑定等还是空槽，会把空类型写进 let。
    // Sema visitDeclareAssign → refreshInferredLetType 在 visitExpr 之后回填。
    TypeInfo varType = type ? type->getType() : TypeInfo();
    if (scope) {
        SymbolInfo sym(SymbolKind::Variable, nameTok.getText(), varType, flags.isMut);
        if (flags.isCval) sym.isConst = true;
        if (flags.isFrozen) sym.isFrozen = true;
        scope->registerSymbol(nameTok.getText(), sym);
    }
    auto* assign = create<StatementDeclareAssignNode>(id, scope, flags.isMut, flags.isCval, nameTok, type, expr);
    assign->setFrozen(flags.isFrozen);
    applyPrefixAnnos(assign, std::move(prefixAnnos));
    return static_cast<StatementNode*>(assign);
}

StatementNode* RdBuilder::buildStmt(rd::NodeId id) {
    if (id == rd::kEmptyNode) return nullptr;
    const auto& n = at(id);
    auto* scope = currentScope();
    rd::i32 annoSkip = 0;
    vector<AnnoCall> prefixAnnos;
    if (n.kind != rd::NodeKind::Let && n.kind != rd::NodeKind::LetTuple) {
        while (annoSkip < n.children_count && at(child(id, annoSkip)).kind == rd::NodeKind::Anno) {
            prefixAnnos.push_back(buildAnnoCall(child(id, annoSkip)));
            ++annoSkip;
        }
    }
    switch (n.kind) {
    case rd::NodeKind::Let:
    case rd::NodeKind::LetTuple:
        return buildLet(id, false);
    case rd::NodeKind::Alias:
        return addAlias(id);
    case rd::NodeKind::Ret: {
        auto* expr = annoSkip < n.children_count ? buildExpr(child(id, annoSkip)) : nullptr;
        auto* ret = create<StatementRetNode>(id, scope, expr);
        if (expr) ret->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
        applyPrefixAnnos(ret, std::move(prefixAnnos));
        return static_cast<StatementNode*>(ret);
    }
    case rd::NodeKind::RetVoid: {
        auto* ret = static_cast<StatementNode*>(create<StatementRetVoidNode>(id, scope));
        applyPrefixAnnos(ret, std::move(prefixAnnos));
        return ret;
    }
    case rd::NodeKind::Break: {
        auto* br = static_cast<StatementNode*>(create<StatementBreakNode>(id, scope, makeTok(id)));
        applyPrefixAnnos(br, std::move(prefixAnnos));
        return br;
    }
    case rd::NodeKind::Continue: {
        auto* cont = static_cast<StatementNode*>(create<StatementContinueNode>(id, scope, makeTok(id)));
        applyPrefixAnnos(cont, std::move(prefixAnnos));
        return cont;
    }
    case rd::NodeKind::ExprStmt: {
        auto* expr = annoSkip < n.children_count ? buildExpr(child(id, annoSkip)) : nullptr;
        bool semi = n.op == rd::Kind::SymbolSemicolon;
        auto* stmt = static_cast<StatementNode*>(create<StatementExprNode>(id, scope, expr, semi));
        applyPrefixAnnos(stmt, std::move(prefixAnnos));
        return stmt;
    }
    case rd::NodeKind::Assign: {
        Token obj;
        vector<Token> subs;
        ExprNode* expr = nullptr;
        if (annoSkip < n.children_count) {
            const auto& first = at(child(id, annoSkip));
            obj = first.kind == rd::NodeKind::This ? Token("$", first.pos.line) : makeTok(child(id, annoSkip));
        }
        for (rd::i32 i = annoSkip + 1; i + 1 < n.children_count; ++i) {
            Token t = makeTok(child(id, i));
            string text = t.getText();
            if (!text.empty() && text[0] == '.') t = Token(text.substr(1), t.getLine());
            subs.push_back(t);
        }
        if (annoSkip < n.children_count) expr = buildExpr(child(id, n.children_count - 1));
        auto* stmt = static_cast<StatementNode*>(
            create<StatementAssignNode>(id, scope, obj, subs, expr, assignOpFromKind(n.op)));
        applyPrefixAnnos(stmt, std::move(prefixAnnos));
        return stmt;
    }
    case rd::NodeKind::Set: {
        auto* arr = annoSkip < n.children_count ? buildExpr(child(id, annoSkip)) : nullptr;
        vector<ExprNode*> idx;
        for (rd::i32 i = annoSkip + 1; i + 1 < n.children_count; ++i)
            idx.push_back(buildExpr(child(id, i)));
        auto* val = annoSkip + 1 < n.children_count ? buildExpr(child(id, n.children_count - 1)) : nullptr;
        auto* stmt = static_cast<StatementNode*>(create<StatementSetNode>(id, scope, arr, std::move(idx), val));
        applyPrefixAnnos(stmt, std::move(prefixAnnos));
        return stmt;
    }
    case rd::NodeKind::StaticFieldSet: {
        TypePath path = annoSkip < n.children_count
                            ? pathFromDotted(at(child(id, annoSkip)).value, at(child(id, annoSkip)).pos)
                            : TypePath();
        Token field = makeTok(id);
        auto* val = annoSkip + 1 < n.children_count ? buildExpr(child(id, annoSkip + 1)) : nullptr;
        auto* stmt =
            static_cast<StatementNode*>(create<StatementStaticFieldSetNode>(id, scope, std::move(path), field, val));
        applyPrefixAnnos(stmt, std::move(prefixAnnos));
        return stmt;
    }
    case rd::NodeKind::Loop: {
        Token label = n.value.empty() ? Token() : makeTok(id);
        vector<Token> initNames;
        TypeNode* initType = nullptr;
        ExprNode* initExpr = nullptr;
        rd::NodeId body = rd::kEmptyNode;
        rd::i32 i = annoSkip;
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
                filled->registerSymbol(nm, *sym);
            filled->copyLocalAliasesFrom(block);
        }
        auto* loop = static_cast<StatementNode*>(
            create<StatementLoopNode>(id, outer, filled, label, initNames, initType, initExpr));
        applyPrefixAnnos(loop, std::move(prefixAnnos));
        return loop;
    }
    case rd::NodeKind::ForIn: {
        Token item = makeTok(id);
        Token label;
        auto* coll = annoSkip < n.children_count ? buildExpr(child(id, annoSkip)) : nullptr;
        rd::NodeId body = (annoSkip + 1 < n.children_count && at(child(id, annoSkip + 1)).kind == rd::NodeKind::Block)
                              ? child(id, annoSkip + 1)
                              : rd::kEmptyNode;
        if (n.children_count > annoSkip + 2 && at(child(id, n.children_count - 1)).kind == rd::NodeKind::Ident)
            label = makeTok(child(id, n.children_count - 1));
        auto* outer = scope;
        auto* block = create<StatementBlockNode>(body == rd::kEmptyNode ? id : body, outer, vector<StatementNode*>{},
                                                 nullptr, false);
        block->setParentScope(outer);
        _scopeStack.push_back(block);
        TypeInfo collType = coll ? coll->getType() : TypeInfo();
        const TypeInfo& peeled = collType.peelRef();
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
                filled->registerSymbol(nm, *sym);
            filled->copyLocalAliasesFrom(block);
        }
        auto* forIn = static_cast<StatementNode*>(create<StatementForInNode>(id, outer, filled, item, coll, label));
        applyPrefixAnnos(forIn, std::move(prefixAnnos));
        return forIn;
    }
    case rd::NodeKind::Block:
        return nullptr;
    default:
        return static_cast<StatementNode*>(create<StatementExprNode>(id, scope, buildExpr(id), /*hasSemicolon=*/true));
    }
}

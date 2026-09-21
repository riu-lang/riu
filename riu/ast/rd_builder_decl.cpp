// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "rd_builder.h"

#include "ast/layout.h"
#include "ast/name_lookup.h"
#include "ast/syntax_diag.h"
#include "ast_builder_helpers.h"
#include "node/alias_node.h"
#include "node/enum_node.h"
#include "node/expr_node.h"
#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "node/global_var_node.h"
#include "node/literal_node.h"
#include "node/spec_node.h"
#include "node/statement_node.h"
#include "node/struct_node.h"
#include "node/type_node.h"

namespace {

bool isTypeKindDecl(rd::NodeKind k) {
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

string dottedJoin(const string& prefix, const string& name) {
    string s = prefix;
    s += '.';
    s += name;
    return s;
}

rd::i32 skipAnnos(const rd::FlatAst& ast, rd::NodeId id, rd::i32 i) {
    const auto& n = ast.at(id);
    while (i < n.children_count && ast.at(ast.child(id, i)).kind == rd::NodeKind::Anno)
        ++i;
    return i;
}

} // namespace

FnHeaderNode* RdBuilder::buildFnHeader(rd::NodeId id, FileNode* file, const vector<rd::NodeId>& annos,
                                       rd::NodeId generic, const vector<rd::NodeId>& params, rd::NodeId ret) {
    const auto& n = at(id);
    TypeNode* retType = nullptr;
    TypeNode* retFallibleFromType = nullptr;
    if (ret != rd::kEmptyNode) {
        auto parsed = buildType(ret);
        std::tie(retType, retFallibleFromType) = peelFallibleRetType(parsed);
    }
    Token nameTok = makeTok(n.value, n.pos);
    const rd::Pos headerPos = annos.empty() ? n.pos : at(annos.front()).pos;
    checkDiscardDeclName(nameTok.getText(), "function", static_cast<int>(headerPos.line),
                         static_cast<int>(headerPos.column) + 1);
    auto* header = create<FnHeaderNode>(headerPos, file, nameTok, retType);

    RdAnnoList al;
    al.names.reserve(annos.size());
    al.args.reserve(annos.size());
    for (rd::NodeId a : annos) {
        string name = string(at(a).value);
        int line = at(a).pos.line;
        int col = at(a).pos.column + 1;
        if (name == "Fallible") {
            throw RiuError(line, col, ErrorCode::E2005, name)
                .withHint("removed; declare failure with `T ! E` in the function signature instead");
        }
        if (!knownAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2005, name);
        if (name == "DraftLike" || name == "Spec" || name == "Impl" || name == "Reflect") {
            throw RiuError(line, col, ErrorCode::E1110);
        }
        if (name == "Packed" || name == "Align") {
            throw RiuError(line, col, ErrorCode::E2011, name);
        }
        const bool hasArg = at(a).op == rd::Kind::ParStart;
        if (argAnnos().contains(name) != hasArg) throw RiuError(line, col, ErrorCode::E2005, name);
        al.names.push_back(std::move(name));
        al.args.push_back(annoArgText(a));
    }
    header->setAnnos(std::move(al.names), std::move(al.args));

    if (generic != rd::kEmptyNode) {
        vector<string> typeParams;
        vector<vector<SpecRef>> typeParamBounds;
        vector<TypeNode*> typeParamDefaults;
        parseTypeParams(generic, typeParams, typeParamBounds, &typeParamDefaults);
        header->setTypeParams(typeParams);
        header->setTypeParamBounds(typeParamBounds);
        header->setTypeParamDefaults(std::move(typeParamDefaults));
    }

    if (retFallibleFromType) header->setFallibleErrType(retFallibleFromType);
    checkFallibleRetMismatch(header);

    for (rd::NodeId p : params) {
        const auto& pn = at(p);
        bool frozen = false;
        TypeNode* ty = nullptr;
        for (rd::i32 i = 0; i < pn.children_count; ++i) {
            rd::NodeId k = child(p, i);
            if (at(k).kind == rd::NodeKind::Anno) {
                if (at(k).value == "Frozen")
                    frozen = true;
                else {
                    throw RiuError(at(k).pos.line, at(k).pos.column + 1, ErrorCode::E3105, string(at(k).value));
                }
            } else if (isTypeKindDecl(at(k).kind)) {
                ty = buildType(k);
            }
        }
        auto* param = create<FnParamNode>(p, header, makeTok(p), ty);
        param->setFrozen(frozen);
        header->addParam(param);
    }
    return header;
}

void RdBuilder::fillFnBody(FnNode* fn, rd::NodeId body) {
    if (body == rd::kEmptyNode) return;
    if (at(body).kind == rd::NodeKind::Block) {
        auto* blk = buildBlock(body, fn);
        for (auto* stmt : blk->statements())
            fn->addStatement(stmt);
        if (blk->hasResult()) {
            auto* resultExpr = blk->resultExpr();
            auto* retStmt = create<StatementRetNode>(body, fn, resultExpr);
            retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
            fn->addStatement(retStmt);
        }
        return;
    }
    auto* expr = buildExpr(body);
    auto* retStmt = create<StatementRetNode>(body, fn, expr);
    retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
    fn->addStatement(retStmt);
}

void RdBuilder::addFn(rd::NodeId id) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    const auto& n = at(id);
    vector<rd::NodeId> annos;
    rd::i32 i = 0;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Anno) {
        annos.push_back(child(id, i));
        ++i;
    }
    rd::NodeId generic = rd::kEmptyNode;
    if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
        generic = child(id, i);
        ++i;
    }
    vector<rd::NodeId> params;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Param) {
        params.push_back(child(id, i));
        ++i;
    }
    rd::NodeId ret = rd::kEmptyNode;
    if (i < n.children_count && isTypeKindDecl(at(child(id, i)).kind)) {
        ret = child(id, i);
        ++i;
    }
    rd::NodeId body = i < n.children_count ? child(id, i) : rd::kEmptyNode;

    auto* header = buildFnHeader(id, file, annos, generic, params, ret);
    auto* fn = create<FnNode>(id, file, header);
    fn->setParentScope(file);
    if (_keepSourceText) fn->setSourceText(srcSlice(n.pos));
    file->addFunction(fn);
    checkNoReturnHeader(header);

    if (header->hasAnno("Test")) {
        const string fnName = header->name().getText();
        int annoLine = header->getLineNumber();
        int annoCol = header->getColumn();
        if (!_isTestFile) {
            throw RiuError(annoLine, annoCol, ErrorCode::E2014, _sourcePath.empty() ? _moduleName : _sourcePath);
        }
        if (header->hasAnno("Builtin")) throw RiuError(annoLine, annoCol, ErrorCode::E2013, fnName);
        bool sigOk = params.empty() && ret == rd::kEmptyNode && body != rd::kEmptyNode;
        if (!sigOk) throw RiuError(annoLine, annoCol, ErrorCode::E2012, fnName, fnName);
    }

    _scopeStack.push_back(fn);
    for (auto& tp : header->typeParams())
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    for (auto* param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
        if (param->isFrozen()) si.isFrozen = true;
        fn->registerSymbol(param->name().getText(), si);
    }
    if (body == rd::kEmptyNode) {
        if (!header->hasAnno("Builtin")) {
            throw RiuError(header->getLineNumber(), header->getColumn(), ErrorCode::E2006, header->name().getText())
                .withHint("普通函数必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#Builtin` 注解");
        }
    } else {
        fillFnBody(fn, body);
    }
    _scopeStack.pop_back();
}

void RdBuilder::addExtern(rd::NodeId id) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    const auto& n = at(id);
    rd::i32 i = skipAnnos(_ast, id, 0);
    (void)collectAnnos(id, 0, i, true, false);

    for (; i < n.children_count; ++i) {
        rd::NodeId fnId = child(id, i);
        if (at(fnId).kind != rd::NodeKind::Fn) continue;
        const auto& fn = at(fnId);
        vector<rd::NodeId> annos;
        rd::i32 j = 0;
        while (j < fn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Anno) {
            annos.push_back(child(fnId, j));
            ++j;
        }
        RdAnnoList headerAnnos;
        for (rd::NodeId a : annos) {
            string name = string(at(a).value);
            int line = at(a).pos.line;
            int col = at(a).pos.column + 1;
            if (!knownAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2005, name);
            if (!externFnAllowedAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2011, name);
            const bool hasArg = at(a).op == rd::Kind::ParStart;
            if (argAnnos().contains(name) != hasArg) throw RiuError(line, col, ErrorCode::E2005, name);
            headerAnnos.names.push_back(std::move(name));
            headerAnnos.args.push_back(annoArgText(a));
        }
        bool externNoReturn = false;
        for (const auto& name : headerAnnos.names)
            if (name == "NoReturn") externNoReturn = true;

        if (j < fn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Generic) ++j;
        vector<TypeInfo> paramTypes;
        while (j < fn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Param) {
            const auto& pn = at(child(fnId, j));
            TypeNode* ty = nullptr;
            for (rd::i32 k = 0; k < pn.children_count; ++k) {
                if (isTypeKindDecl(at(child(child(fnId, j), k)).kind)) ty = buildType(child(child(fnId, j), k));
            }
            if (ty) paramTypes.push_back(ty->getType());
            ++j;
        }
        TypeInfo retType;
        if (j < fn.children_count && isTypeKindDecl(at(child(fnId, j)).kind)) {
            auto* typeNode = buildType(child(fnId, j));
            if (typeNode) retType = typeNode->getType();
        }
        if (externNoReturn && !retType.empty()) {
            throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E7012, string(fn.value));
        }

        const int declLine = fn.pos.line;
        auto throwExternKind = [&](const TypeInfo& t, const char* where) {
            if (t.empty()) return;
            if (t.isPtr()) return;
            const string fnName = string(fn.value);
            if (t.isFallible()) throw RiuError(declLine, ErrorCode::E2034, fnName, t.getFullName(), where);
            if (t.isNormal() && isBuiltinType(t.name)) return;
            if (t.isFn()) throw RiuError(declLine, ErrorCode::E2031, fnName, where);
            if (t.isHeap()) {
                auto inner = t.heapElementType();
                throw RiuError(declLine, ErrorCode::E4028, inner ? inner->name : std::string("?"));
            }
            if (t.isDyn()) {
                auto spec = t.dynSpecType();
                throw RiuError(declLine, ErrorCode::E1136, spec ? spec->getFullName() : t.getFullName());
            }
            if (t.isRc() || t.isWeak() || t.isNullable() || t.isArrayGeneric() || t.isRef() || t.isTuple() ||
                t.isString() || t.isStringBuilder()) {
                throw RiuError(declLine, ErrorCode::E2034, fnName, t.getFullName(), where);
            }
        };
        for (auto& pt : paramTypes)
            throwExternKind(pt, "parameters");
        throwExternKind(retType, "return type");

        const string fnName = string(fn.value);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = file->moduleName();
        fnSym.isExternal = true;
        file->registerSymbol(fnName, fnSym);
        FnSymbolInfo fnFnSym{fnName, file->moduleName(), paramTypes, retType};
        fnFnSym.isExternal = true;
        fnFnSym.isNoReturn = externNoReturn;
        fnFnSym.declLine = declLine;
        for (size_t ai = 0; ai < headerAnnos.names.size(); ++ai) {
            if (headerAnnos.names[ai] == "CName") {
                fnFnSym.cName = headerAnnos.args[ai];
                break;
            }
        }
        file->registerFnSymbol(fnName, fnFnSym);
    }
}

FnNode* RdBuilder::buildFnClean(rd::NodeId id, ScopeNode* parent, FileNode* file) {
    string structName;
    if (auto* impl = dynamic_cast<StructImplNode*>(parent)) structName = impl->structName();
    auto* header = create<FnHeaderNode>(id, file, makeTok(id), nullptr);
    auto* fn = create<FnNode>(id, parent, header);
    _scopeStack.push_back(fn);
    if (!structName.empty()) {
        vector<sp<TypeInfo>> selfArgs;
        TypeInfo selfInner(structName);
        if (file) selfInner.ownerModule = file->moduleName();
        selfArgs.push_back(internTypeSp(std::move(selfInner)));
        fn->registerSymbol("$", {SymbolKind::Variable, "$", internType(TypeInfo("Ref", selfArgs))});
    }
    if (at(id).children_count > 0) fillFnBody(fn, child(id, 0));
    _scopeStack.pop_back();
    return fn;
}

FnNode* RdBuilder::buildMethod(rd::NodeId id, StructImplNode* impl, FileNode* file) {
    const auto& n = at(id);
    vector<rd::NodeId> annos;
    rd::i32 i = 0;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Anno) {
        annos.push_back(child(id, i));
        ++i;
    }
    rd::NodeId generic = rd::kEmptyNode;
    if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
        generic = child(id, i);
        ++i;
    }
    vector<rd::NodeId> params;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Param) {
        params.push_back(child(id, i));
        ++i;
    }
    rd::NodeId ret = rd::kEmptyNode;
    if (i < n.children_count && isTypeKindDecl(at(child(id, i)).kind)) {
        ret = child(id, i);
        ++i;
    }
    rd::NodeId body = i < n.children_count ? child(id, i) : rd::kEmptyNode;

    auto* header = buildFnHeader(id, file, annos, generic, params, ret);
    auto* fn = create<FnNode>(id, impl, header);
    fn->setParentScope(file);
    checkNoReturnHeader(header);
    _scopeStack.push_back(fn);
    for (auto& tp : impl->typeParams())
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    for (auto& tp : header->typeParams())
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    {
        TypeInfo selfInner(impl->structName());
        if (file) selfInner.ownerModule = file->moduleName();
        vector<sp<TypeInfo>> selfArgs;
        selfArgs.push_back(internTypeSp(std::move(selfInner)));
        fn->registerSymbol("$", {SymbolKind::Variable, "$", internType(TypeInfo("Ref", selfArgs))});
    }
    for (auto* param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
        if (param->isFrozen()) si.isFrozen = true;
        fn->registerSymbol(param->name().getText(), si);
    }
    if (body == rd::kEmptyNode) {
        if (!header->hasAnno("Builtin")) {
            throw RiuError(header->getLineNumber(), header->getColumn(), ErrorCode::E2007, impl->structName(),
                           header->name().getText())
                .withHint("结构体方法必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#Builtin` 注解");
        }
    } else {
        fillFnBody(fn, body);
    }
    _scopeStack.pop_back();
    return fn;
}

void RdBuilder::addStruct(rd::NodeId id) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    const auto& n = at(id);
    string structName = string(n.value);
    string moduleName = file->moduleName();

    bool isSpec = false;
    vector<SpecRef> implRefs;
    RdAnnoList annos;
    bool seenPacked = false;
    bool seenAlign = false;
    rd::Pos locPos = n.pos;
    if (n.children_count > 0 && at(child(id, 0)).kind == rd::NodeKind::Anno) locPos = at(child(id, 0)).pos;
    rd::i32 i = 0;
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Anno) {
        rd::NodeId a = child(id, i);
        string name = string(at(a).value);
        int line = at(a).pos.line;
        int col = at(a).pos.column + 1;
        if (!knownAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2005, name);
        string arg = annoArgText(a);
        const bool hasArg = at(a).op == rd::Kind::ParStart;
        if (argAnnos().contains(name) != hasArg) throw RiuError(line, col, ErrorCode::E2005, name);
        if (name != "DraftLike" && !nonFnAllowedAnnos().contains(name))
            throw RiuError(line, col, ErrorCode::E2011, name);
        if (name == "Packed") {
            if (seenPacked) throw RiuError(line, col, ErrorCode::E2011, name);
            seenPacked = true;
        } else if (name == "Align") {
            if (seenAlign) throw RiuError(line, col, ErrorCode::E2011, name);
            seenAlign = true;
            if (layout::parseAlignArg(arg) == 0) {
                throw RiuError(line, col, ErrorCode::E2038, arg.empty() ? string("?") : arg);
            }
        }
        if (name == "Spec")
            isSpec = true;
        else if (name == "Impl")
            implRefs.push_back(specRefFromAnno(a));
        annos.names.push_back(std::move(name));
        annos.args.push_back(std::move(arg));
        ++i;
    }
    if (!isSpec) {
        for (size_t ai = 0; ai < annos.names.size(); ++ai) {
            if (annos.names[ai] == "DraftLike") {
                throw RiuError(at(child(id, static_cast<rd::i32>(ai))).pos.line,
                               at(child(id, static_cast<rd::i32>(ai))).pos.column + 1, ErrorCode::E1110);
            }
        }
    } else if (seenPacked || seenAlign) {
        for (size_t ai = 0; ai < annos.names.size(); ++ai) {
            if (annos.names[ai] == "Packed" || annos.names[ai] == "Align") {
                throw RiuError(at(child(id, static_cast<rd::i32>(ai))).pos.line,
                               at(child(id, static_cast<rd::i32>(ai))).pos.column + 1, ErrorCode::E2011,
                               annos.names[ai]);
            }
        }
    }
    checkDiscardDeclName(structName, isSpec ? "#Spec" : "struct", n.pos.line, n.pos.column + 1);

    rd::NodeId generic = rd::kEmptyNode;
    if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
        generic = child(id, i);
        ++i;
    }
    vector<string> typeParams;
    vector<vector<SpecRef>> typeParamBounds;
    vector<TypeNode*> typeParamDefaults;
    parseTypeParams(generic, typeParams, typeParamBounds, &typeParamDefaults);
    (void)typeParamBounds;

    auto splitFieldsFns = [&](vector<rd::NodeId>& fields, vector<rd::NodeId>& aliases, rd::NodeId& clean,
                              vector<rd::NodeId>& fns) {
        for (; i < n.children_count; ++i) {
            rd::NodeId c = child(id, i);
            switch (at(c).kind) {
            case rd::NodeKind::Field:
                fields.push_back(c);
                break;
            case rd::NodeKind::Alias:
                aliases.push_back(c);
                break;
            case rd::NodeKind::FnClean:
                clean = c;
                break;
            case rd::NodeKind::Fn:
                fns.push_back(c);
                break;
            default:
                break;
            }
        }
    };

    if (isSpec) {
        rd::NodeId clean = rd::kEmptyNode;
        vector<rd::NodeId> fields, aliases, fns;
        splitFieldsFns(fields, aliases, clean, fns);
        if (clean != rd::kEmptyNode) {
            throw RiuError(at(clean).pos.line, at(clean).pos.column + 1, ErrorCode::E2011,
                           std::string("destructor in #Spec body"));
        }
        auto* draft = create<SpecDeclNode>(locPos, file, makeTok(id));
        draft->setAnnos(annos.names, annos.args);
        draft->setTypeParams(typeParams);
        draft->setTypeParamDefaults(std::move(typeParamDefaults));
        if (_keepSourceText) draft->setSourceText(srcSlice(n.pos));
        _scopeStack.push_back(draft);
        for (auto& tp : draft->typeParams())
            draft->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        for (rd::NodeId a : aliases)
            addAlias(a);
        for (rd::NodeId f : fields) {
            const auto& fn = at(f);
            rd::i32 fi = skipAnnos(_ast, f, 0);
            bool isStatic = false;
            for (rd::i32 ai = 0; ai < fi; ++ai) {
                string an = string(at(child(f, ai)).value);
                if (an == "Static" || an == "Cval") isStatic = true;
                if (an == "Packed" || an == "Align") {
                    throw RiuError(at(child(f, ai)).pos.line, at(child(f, ai)).pos.column + 1, ErrorCode::E2011, an);
                }
            }
            TypeNode* ty = fi < fn.children_count ? buildType(child(f, fi)) : nullptr;
            auto* field = create<StructFieldNode>(f, draft, makeTok(f), ty);
            field->setStatic(isStatic);
            if (!field->isStatic()) {
                throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E2011,
                               std::string("instance field in #Spec body (only `#Static` fields allowed)"));
            }
            draft->addStaticField(field);
        }
        bool isDraftLike = draft->isDraftLike();
        for (rd::NodeId fnId : fns) {
            const auto& fnn = at(fnId);
            vector<rd::NodeId> fannos;
            rd::i32 j = 0;
            while (j < fnn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Anno) {
                fannos.push_back(child(fnId, j));
                ++j;
            }
            rd::NodeId g = rd::kEmptyNode;
            if (j < fnn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Generic) {
                g = child(fnId, j);
                ++j;
            }
            vector<rd::NodeId> params;
            while (j < fnn.children_count && at(child(fnId, j)).kind == rd::NodeKind::Param) {
                params.push_back(child(fnId, j));
                ++j;
            }
            rd::NodeId ret = rd::kEmptyNode;
            if (j < fnn.children_count && isTypeKindDecl(at(child(fnId, j)).kind)) {
                ret = child(fnId, j);
                ++j;
            }
            rd::NodeId body = j < fnn.children_count ? child(fnId, j) : rd::kEmptyNode;
            auto* header = buildFnHeader(fnId, file, fannos, g, params, ret);
            if (header->isGeneric()) {
                if (isDraftLike)
                    throw RiuError(header->getLineNumber(), header->getColumn(), ErrorCode::E1112, structName);
                throw RiuError(header->getLineNumber(), header->getColumn(), ErrorCode::E1104, structName,
                               header->name().getText());
            }
            FnNode* defaultBody = nullptr;
            if (body != rd::kEmptyNode) {
                defaultBody = create<FnNode>(fnId, draft, header);
                defaultBody->setParentScope(file);
                _scopeStack.push_back(defaultBody);
                for (auto& tp : draft->typeParams())
                    defaultBody->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
                {
                    vector<sp<TypeInfo>> selfArgs;
                    selfArgs.push_back(make_shared<TypeInfo>("Self"));
                    defaultBody->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
                }
                for (auto* param : header->params()) {
                    TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
                    SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
                    if (param->isFrozen()) si.isFrozen = true;
                    defaultBody->registerSymbol(param->name().getText(), si);
                }
                fillFnBody(defaultBody, body);
                _scopeStack.pop_back();
            }
            draft->addSignature(header, defaultBody);
        }
        _scopeStack.pop_back();
        file->addSpecDecl(draft);
        return;
    }

    auto* structDecl = create<StructDeclNode>(locPos, file, makeTok(id));
    structDecl->setAnnos(annos.names, annos.args);
    if (_keepSourceText) structDecl->setSourceText(srcSlice(n.pos));
    structDecl->setTypeParams(typeParams);
    structDecl->setTypeParamDefaults(std::move(typeParamDefaults));
    _scopeStack.push_back(structDecl);
    for (auto& tp : structDecl->typeParams())
        structDecl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});

    rd::NodeId clean = rd::kEmptyNode;
    vector<rd::NodeId> fields, aliases, fns;
    splitFieldsFns(fields, aliases, clean, fns);

    for (rd::NodeId a : aliases)
        addAlias(a);

    for (rd::NodeId f : fields) {
        const auto& fn = at(f);
        rd::i32 fi = 0;
        bool isStatic = false, isMut = false, isCval = false, isInline = false;
        bool isVal = false, isFrozen = false;
        uint32_t fieldAlign = 0;
        while (fi < fn.children_count && at(child(f, fi)).kind == rd::NodeKind::Anno) {
            auto a = child(f, fi);
            string an = string(at(a).value);
            int line = at(a).pos.line;
            int col = at(a).pos.column + 1;
            const bool hasArg = at(a).op == rd::Kind::ParStart;
            if (an == "Static")
                isStatic = true;
            else if (an == "Mut")
                isMut = true;
            else if (an == "Cval") {
                isStatic = true;
                isCval = true;
            } else if (an == "Inline")
                isInline = true;
            else if (an == "Val")
                isVal = true;
            else if (an == "Frozen")
                isFrozen = true;
            else if (an == "Align") {
                if (isStatic || fieldAlign != 0 || !hasArg) throw RiuError(line, col, ErrorCode::E3108, an);
                string arg = annoArgText(a);
                uint64_t n = layout::parseAlignArg(arg);
                if (n == 0) throw RiuError(line, col, ErrorCode::E2038, arg.empty() ? string("?") : arg);
                fieldAlign = static_cast<uint32_t>(n);
            } else
                throw RiuError(line, col, ErrorCode::E3108, an);
            if (an != "Align" && hasArg) throw RiuError(line, col, ErrorCode::E3108, an);
            ++fi;
        }
        TypeNode* ty =
            fi < fn.children_count && isTypeKindDecl(at(child(f, fi)).kind) ? buildType(child(f, fi++)) : nullptr;
        ExprNode* init = fi < fn.children_count ? buildExpr(child(f, fi)) : nullptr;

        if (isStatic) {
            if (fieldAlign != 0) throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E3108, string("Align"));
            if (isInline && !isCval) throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E3117);
            if (!typeParams.empty()) throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E3157, structName);
            checkDiscardDeclName(fn.value, "static field", fn.pos.line, fn.pos.column + 1);
            if (!init) throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E3150, string(fn.value));
            inferFlexibleIntForType(init, ty->getType());
            if (exprContainsTryCatch(init)) {
                throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E3155, string(fn.value));
            }
            StructDeclNode::StaticFieldEntry sf;
            sf.name = makeTok(f);
            sf.type = ty;
            sf.init = init;
            sf.isMutable = isMut;
            sf.isPrivate = !sf.name.getText().empty() && sf.name.getText()[0] == '_';
            sf.isCval = isCval;
            sf.isInline = isInline;
            structDecl->addStaticField(sf);
            continue;
        }
        auto* field = create<StructFieldNode>(f, structDecl, makeTok(f), ty);
        field->setVal(isVal);
        field->setFrozen(isFrozen);
        field->setStatic(isStatic);
        field->setCval(isCval);
        field->setInline(isInline);
        field->setAlignN(fieldAlign);
        structDecl->addField(field);
        (void)init;
    }

    _scopeStack.pop_back();
    file->addStructDecl(structDecl);

    for (auto* field : structDecl->fields()) {
        if (field->isDiscard()) continue;
        string methodKey = dottedJoin(structName, field->name().getText());
        SymbolInfo fieldSym(SymbolKind::Variable, field->name().getText(), field->getType());
        fieldSym.moduleName = moduleName;
        file->registerSymbol(methodKey, fieldSym);
    }

    if (fns.empty() && clean == rd::kEmptyNode && implRefs.empty()) return;

    auto* structImpl = create<StructImplNode>(locPos, file, makeTok(id));
    structImpl->setAnnos(annos.names, annos.args);
    structImpl->setTypeParams(typeParams);
    structImpl->setSpecRefs(std::move(implRefs));
    structImpl->setParentScope(file);
    _scopeStack.push_back(structImpl);
    for (auto& tp : structImpl->typeParams())
        structImpl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    structImpl->copyLocalAliasesFrom(structDecl);

    if (clean != rd::kEmptyNode) {
        auto* destructor = buildFnClean(clean, structImpl, file);
        destructor->setParentScope(file);
        structImpl->setDestructor(destructor);
        string destructorName = structName + ".~" + structName;
        vector<TypeInfo> paramTypes;
        paramTypes.emplace_back(structName);
        SymbolInfo destructorSym(SymbolKind::Function, "~" + structName, TypeInfo());
        destructorSym.moduleName = moduleName;
        file->registerSymbol(destructorName, destructorSym);
        file->registerFnSymbol(destructorName, FnSymbolInfo{destructorName, moduleName, paramTypes, TypeInfo()});
    }

    for (rd::NodeId fnId : fns) {
        auto* method = buildMethod(fnId, structImpl, file);
        structImpl->addMethod(method);
    }
    _scopeStack.pop_back();
    file->addStructImpl(structImpl);

    for (auto* method : structImpl->methods()) {
        string methodName = method->header()->name().getText();
        string fullName = dottedJoin(structName, methodName);
        vector<TypeInfo> paramTypes;
        paramTypes.emplace_back(structName);
        for (auto* param : method->header()->params()) {
            if (param->type()) paramTypes.push_back(param->type()->getType());
        }
        TypeInfo retType;
        if (method->header()->retType()) retType = method->header()->retType()->getType();
        SymbolInfo methodSym(SymbolKind::Function, methodName, retType);
        methodSym.moduleName = moduleName;
        file->registerSymbol(fullName, methodSym);
        FnSymbolInfo methodFnSym{fullName, moduleName, paramTypes, retType};
        methodFnSym.isNoReturn = method->header()->hasAnno("NoReturn");
        methodFnSym.isConst = method->header()->hasAnno("Const");
        methodFnSym.fallibleErrType = method->header()->resolvedFallibleErr();
        if (!methodFnSym.fallibleErrType.empty() && retType.getFullName() == methodFnSym.fallibleErrType) {
            throw RiuError(method->header()->getLineNumber(), method->header()->getColumn(), ErrorCode::E7008,
                           retType.getFullName(), methodFnSym.fallibleErrType);
        }
        file->registerFnSymbol(fullName, methodFnSym);
    }
}

void RdBuilder::addEnum(rd::NodeId id) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    const auto& n = at(id);
    checkDiscardDeclName(n.value, "enum", n.pos.line, n.pos.column + 1);
    auto* enumDecl = create<EnumDeclNode>(id, file, makeTok(id));
    enumDecl->setParentScope(file);
    rd::i32 i = 0;
    if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
        vector<string> typeParams;
        vector<vector<SpecRef>> typeParamBounds;
        vector<TypeNode*> typeParamDefaults;
        parseTypeParams(child(id, i), typeParams, typeParamBounds, &typeParamDefaults);
        enumDecl->setTypeParams(typeParams);
        enumDecl->setTypeParamBounds(typeParamBounds);
        enumDecl->setTypeParamDefaults(std::move(typeParamDefaults));
        ++i;
    }
    _scopeStack.push_back(enumDecl);
    for (auto& tp : enumDecl->typeParams())
        enumDecl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    for (; i < n.children_count; ++i) {
        rd::NodeId v = child(id, i);
        if (at(v).kind != rd::NodeKind::EnumVariant) continue;
        auto* variant = create<EnumVariantNode>(v, enumDecl, makeTok(v));
        for (rd::i32 k = 0; k < at(v).children_count; ++k)
            variant->addPayloadType(buildType(child(v, k)));
        if (!enumDecl->addVariant(variant)) {
            _scopeStack.pop_back();
            throw RiuError(at(v).pos.line, at(v).pos.column + 1, ErrorCode::E2018, string(at(v).value),
                           string(n.value));
        }
    }
    _scopeStack.pop_back();
    file->addEnumDecl(enumDecl);
}

StatementNode* RdBuilder::addAlias(rd::NodeId id) {
    auto* scope = currentScope();
    Token nameTok = makeTok(id);
    string name = nameTok.getText();
    int line = at(id).pos.line;
    int col = at(id).pos.column + 1;
    checkDiscardDeclName(name, "type alias", line, col);
    auto* aliasDecl = create<AliasDeclNode>(id, scope, nameTok, static_cast<TypeNode*>(nullptr));
    TypeNode* target = at(id).children_count > 0 ? buildType(child(id, 0)) : nullptr;
    aliasDecl->setTarget(target);
    if (auto* file = dynamic_cast<FileNode*>(scope)) {
        file->addAliasDecl(aliasDecl);
    } else if (scope) {
        if (scope->localAlias(name)) throw RiuError(line, col, ErrorCode::E2017, name, string("type alias"), name);
        auto it = scope->localSymbols().find(name);
        if (it != scope->localSymbols().end() && it->second->kind == SymbolKind::TypeParam) {
            throw RiuError(line, col, ErrorCode::E2017, name, string("type param"), name);
        }
        scope->addLocalAlias(aliasDecl);
    }
    return static_cast<StatementNode*>(aliasDecl);
}

void RdBuilder::preregisterFnsAndLets(rd::NodeId program) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    const auto& n = at(program);
    auto moduleName = file->moduleName();
    for (rd::i32 i = 0; i < n.children_count; ++i) {
        rd::NodeId it = child(program, i);
        if (at(it).kind == rd::NodeKind::Fn) {
            const auto& fn = at(it);
            rd::i32 j = skipAnnos(_ast, it, 0);
            if (j < fn.children_count && at(child(it, j)).kind == rd::NodeKind::Generic) ++j;
            vector<TypeInfo> paramTypes;
            while (j < fn.children_count && at(child(it, j)).kind == rd::NodeKind::Param) {
                const auto& pn = at(child(it, j));
                TypeNode* ty = nullptr;
                for (rd::i32 k = 0; k < pn.children_count; ++k) {
                    if (isTypeKindDecl(at(child(child(it, j), k)).kind)) ty = buildType(child(child(it, j), k));
                }
                if (ty) paramTypes.push_back(ty->getType());
                ++j;
            }
            TypeInfo retType;
            string suffixFallibleErr;
            if (j < fn.children_count && isTypeKindDecl(at(child(it, j)).kind)) {
                auto* typeNode = buildType(child(it, j));
                if (auto* fallible = dynamic_cast<TypeFallibleNode*>(typeNode)) {
                    suffixFallibleErr = fallibleErrKey(fallible->errType()->getType());
                    retType = fallible->baseType()->getType();
                } else if (typeNode) {
                    retType = typeNode->getType();
                }
            }
            const string fnName = string(fn.value);
            SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
            fnSym.moduleName = moduleName;
            file->registerSymbol(fnName, fnSym);
            FnSymbolInfo fnFnSym{fnName, moduleName, paramTypes, retType};
            for (rd::i32 ai = 0; ai < fn.children_count && at(child(it, ai)).kind == rd::NodeKind::Anno; ++ai) {
                string aname = string(at(child(it, ai)).value);
                if (aname == "NoReturn")
                    fnFnSym.isNoReturn = true;
                else if (aname == "Const")
                    fnFnSym.isConst = true;
            }
            fnFnSym.fallibleErrType = suffixFallibleErr;
            if (!fnFnSym.fallibleErrType.empty() && retType.getFullName() == fnFnSym.fallibleErrType) {
                throw RiuError(fn.pos.line, fn.pos.column + 1, ErrorCode::E7008, retType.getFullName(),
                               fnFnSym.fallibleErrType);
            }
            file->registerFnSymbol(fnName, fnFnSym);
        } else if (at(it).kind == rd::NodeKind::Let) {
            const auto& ln = at(it);
            rd::i32 j = skipAnnos(_ast, it, 0);
            TypeNode* typeNode = nullptr;
            if (j < ln.children_count && isTypeKindDecl(at(child(it, j)).kind)) typeNode = buildType(child(it, j));
            if (!typeNode) continue;
            vector<rd::NodeId> annos;
            for (rd::i32 ai = 0; ai < ln.children_count && at(child(it, ai)).kind == rd::NodeKind::Anno; ++ai)
                annos.push_back(child(it, ai));
            auto flags = readLetAnnos(annos);
            TypeInfo type = typeNode->getType();
            SymbolInfo sym(SymbolKind::Variable, string(ln.value), type, flags.isMut);
            sym.moduleName = moduleName;
            sym.isConst = flags.isCval;
            file->registerSymbol(string(ln.value), sym);
        }
    }
}

void RdBuilder::addItem(rd::NodeId id) {
    switch (at(id).kind) {
    case rd::NodeKind::Use:
        addUse(id);
        break;
    case rd::NodeKind::Fn:
        addFn(id);
        break;
    case rd::NodeKind::Extern:
        addExtern(id);
        break;
    case rd::NodeKind::Let:
    case rd::NodeKind::LetTuple:
        buildLet(id, true);
        break;
    case rd::NodeKind::Alias:
        addAlias(id);
        break;
    case rd::NodeKind::Enum:
        addEnum(id);
        break;
    case rd::NodeKind::Struct:
        addStruct(id);
        break;
    default:
        break;
    }
}

FileNode* RdBuilder::build() {
    // 混测大量短 ident；预留避免 intern 表反复 rehash。
    if (!_src.empty()) _riu.stringIntern().reserve(_src.size() / 16);
    auto parsed = rd::parseProgram(_src);
    _ast = std::move(parsed.ast);
    _errors = std::move(parsed.errors);
    if (_indexTokens) indexDefaultTokens();
    for (const auto& e : _errors) {
        SyntaxDiag d;
        d.is_lexer = e.is_lexer;
        d.file = _sourcePath;
        d.line = e.pos.line;
        d.col = e.pos.column + 1;
        d.message = e.message;
        d.offending = e.offending;
        d.prev_text = e.prev_text;
        Diagnostic diag;
        if (!fillSyntaxDiagnostic(diag, d)) continue;
        auto err = RiuError(e.pos.line, e.pos.column + 1, e.is_lexer ? ErrorCode::E1001 : ErrorCode::E1002, e.message);
        if (!_sourcePath.empty()) err.withFile(_sourcePath);
        for (const auto& h : diag.hints)
            err.withHint(h);
        throw err;
    }

    auto* file = _targetFile ? _targetFile : _riu.createFile(_moduleName);
    if (!_sourcePath.empty()) file->setSourcePath(_sourcePath);
    _scopeStack.push_back(file);
    if (_ast.root() != rd::kEmptyNode) {
        preregisterFnsAndLets(_ast.root());
        const auto& n = at(_ast.root());
        for (rd::i32 i = 0; i < n.children_count; ++i)
            addItem(child(_ast.root(), i));
    }
    sema::fillFileDeclTypes(file);
    _scopeStack.pop_back();
    releaseParseTemps();
    return file;
}

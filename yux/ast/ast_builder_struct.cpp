// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 结构体族实现：
//   - visitStructDecl  (含 #Spec / #Impl 三路分发：spec-unify v1)
//   - visitFnClean     (析构)
//   - visitFiledDecl   (字段)
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "types.h"
#include <algorithm>

// spec-unify v1：声明合一的 visitStructDecl 入口。
// 分三种形态：
//   1) `#Spec struct Foo { fn x() i32 }`   → 构造 DraftDeclNode（仅签名）
//   2) `struct Foo { fields }`             → 构造 StructDeclNode 只
//   3) `struct Foo { fields; fnClean?; fns }` 或 `#Impl(D) struct ...`
//                                          → 构造 StructDeclNode + StructImplNode
// 注解 `#Spec` 走分支 1；`#Impl(D)` 写入 StructImplNode::draftRefs（替代旧 `: D1 + D2`）。
std::any ASTBuilder::visitStructDecl(yux::yuxParser::StructDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto* stCtx = ctx->structType();
    string structName = stCtx->name->getText();
    string moduleName = file->moduleName();

    DEBUG_LOG_VAL("Visit: StructDecl", structName);

    // === Step 1: 注解收集 — 识别 #Spec / #Impl，其它落 annos
    bool isSpec = false;
    vector<SpecRef> implRefs;
    AnnoList annos;
    for (auto* a : ctx->buildAnnos) {
        string name = a->name->getText();
        int line = static_cast<int>(a->name->getLine());
        int col = static_cast<int>(a->name->getCharPositionInLine()) + 1;
        if (!knownAnnos().contains(name)) {
            throw YuxError(line, col, ErrorCode::E2005, name);
        }
        string arg = getBuildAnnoArgText(a);
        checkAnnoArity(a, name, a->ParStart() != nullptr);

        if (name == "DraftLike") {
            // §12.4.1.1：#DraftLike 只允许在 spec / draft 上（即同时带 #Spec）
            // 本 v1 暂保留旧 #DraftLike 语义；非 spec 位置抛 E1110。
        } else if (!nonFnAllowedAnnos().contains(name)) {
            throw YuxError(line, col, ErrorCode::E2011, name);
        }

        if (name == "Spec") {
            isSpec = true;
        } else if (name == "Impl") {
            SpecRef r;
            r.name = arg;
            // 解析 turbofish 类型实参（若有）
            if (auto* aa = a->annoArg()) {
                if (auto* gd = aa->genericDef()) {
                    for (auto* pCtx : gd->params) {
                        if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(pCtx->type(0))) {
                            r.typeArgs.emplace_back(tn->ID()->getText());
                        }
                        // 复杂泛型实参押后
                    }
                }
            }
            r.line = line;
            r.col = col;
            implRefs.push_back(std::move(r));
        }
        annos.names.push_back(std::move(name));
        annos.args.push_back(std::move(arg));
    }

    // 非 spec 位置仍出现 #DraftLike → 维持旧 E1110 诊断（首个 #DraftLike）
    if (!isSpec) {
        for (auto* a : ctx->buildAnnos) {
            if (a->name->getText() == "DraftLike") {
                throw YuxError(static_cast<int>(a->name->getLine()),
                               static_cast<int>(a->name->getCharPositionInLine()) + 1, ErrorCode::E1110);
            }
        }
    }

    vector<string> typeParams;
    for (auto tCtx : stCtx->types) {
        if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(tCtx)) {
            typeParams.push_back(tn->ID()->getText());
        }
    }

    // === Step 2: #Spec 分支 — 构造 DraftDeclNode（v1 仅签名 + Phase 1 起 #Static 字段段）
    if (isSpec) {
        if (ctx->fnClean()) {
            auto* fc = ctx->fnClean();
            throw YuxError(static_cast<int>(fc->getStart()->getLine()),
                           static_cast<int>(fc->getStart()->getCharPositionInLine()) + 1, ErrorCode::E2011,
                           std::string("destructor in #Spec body"));
        }

        auto draft = createWithLine<SpecDeclNode>(ctx, file, stCtx->name);
        draft->setAnnos(annos.names, annos.args);
        draft->setTypeParams(typeParams);
        draft->setSourceText(ctxSource(ctx));

        stack.emplace_back(draft);
        _scopeStack.push_back(draft);

        for (auto& tp : draft->typeParams()) {
            draft->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }

        // DRAFT-spec-reflect Phase 1: spec body 内仅允许 `#Static` 字段段 (type-bound 契约,
        // [#1.Q] 例外 / [#1.Z]); instance 字段段仍拒 (E2011)。
        for (auto* fieldCtx : ctx->filedDecl()) {
            auto field = any_cast_p<StructFieldNode>(visit(fieldCtx));
            if (!field->isStatic()) {
                throw YuxError(static_cast<int>(fieldCtx->getStart()->getLine()),
                               static_cast<int>(fieldCtx->getStart()->getCharPositionInLine()) + 1, ErrorCode::E2011,
                               std::string("instance field in #Spec body (only `#Static` fields allowed)"));
            }
            draft->addStaticField(field);
        }

        bool isDraftLike = draft->isDraftLike();
        for (auto* fnCtx : ctx->fn()) {
            auto* fnHeaderCtx = fnCtx->fnHeader();
            auto header = any_cast_p<FnHeaderNode>(visitFnHeader(fnHeaderCtx));
            if (header->isGeneric()) {
                int hLine = header->getLineNumber();
                int hCol = header->getColumn();
                if (isDraftLike) {
                    throw YuxError(hLine, hCol, ErrorCode::E1112, structName);
                }
                throw YuxError(hLine, hCol, ErrorCode::E1104, structName, header->name().getText());
            }

            // DRAFT-spec-default-body Phase 1：spec body 内方法可带可选默认体；
            // 体内 `$` 绑 Self 抽象类型变量（Phase 2 sema 完成占位符号校验）。
            // 注: p<T> = T* 裸指针, 必须显式 nullptr 初始化, 否则上传垃圾。
            p<FnNode> defaultBody = nullptr;
            if (fnCtx->fnBody()) {
                defaultBody = createWithLine<FnNode>(fnCtx, draft, header);
                defaultBody->setParentScope(file);

                stack.emplace_back(defaultBody);
                _scopeStack.push_back(defaultBody);

                for (auto& tp : draft->typeParams()) {
                    defaultBody->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
                }

                {
                    vector<sp<TypeInfo>> selfArgs;
                    selfArgs.push_back(make_shared<TypeInfo>("Self"));
                    defaultBody->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
                }

                for (auto param : header->params()) {
                    TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
                    SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
                    if (param->isFrozen()) {
                        si.isFrozen = true;
                    }
                    defaultBody->registerSymbol(param->name().getText(), si);
                }

                if (auto exprBody = fnCtx->fnBody()->fnExprkBody()) {
                    auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
                    auto retStmt = createWithLine<StatementRetNode>(fnCtx, defaultBody, expr);
                    retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
                    defaultBody->addStatement(retStmt);
                } else if (auto blockBody = fnCtx->fnBody()->fnBlockBody()) {
                    auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));
                    for (auto stmt : stmtBlockNode->statements()) {
                        defaultBody->addStatement(stmt);
                    }
                    if (stmtBlockNode->hasResult()) {
                        auto resultExpr = stmtBlockNode->resultExpr();
                        auto retStmt = createWithLine<StatementRetNode>(fnCtx, defaultBody, resultExpr);
                        retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
                        defaultBody->addStatement(retStmt);
                    }
                }

                _scopeStack.pop_back();
                stack.pop_back();
            }

            draft->addSignature(header, defaultBody);
        }

        _scopeStack.pop_back();
        stack.pop_back();

        file->addSpecDecl(draft);
        return draft;
    }

    // === Step 3: 普通 struct 分支 — 字段
    auto structDecl = createWithLine<StructDeclNode>(ctx, file, stCtx->name);
    structDecl->setAnnos(annos.names, annos.args);
    structDecl->setSourceText(ctxSource(ctx));
    structDecl->setTypeParams(typeParams);

    stack.emplace_back(structDecl);
    _scopeStack.push_back(structDecl);

    for (auto& tp : structDecl->typeParams()) {
        structDecl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }

    for (auto fieldCtx : ctx->filedDecl()) {
        // DRAFT-static-vars Phase 4: 检测 #Static 注解，分流静态 / 实例字段
        // #Cval 隐含 #Static 语义（编译期常量不可能是实例字段）
        bool isStatic = false;
        bool isMut = false;
        bool isCval = false;
        bool isInline = false;
        for (auto* a : fieldCtx->buildAnnos) {
            string annoName = a->name->getText();
            if (annoName == "Static") isStatic = true;
            if (annoName == "Mut") isMut = true;
            if (annoName == "Cval") {
                isStatic = true;
                isCval = true;
            }
            if (annoName == "Inline") isInline = true;
        }

        if (isStatic) {
            // 静态字段：仅接受 #Static / #Mut / #Cval / #Inline，其余注解拒
            for (auto* a : fieldCtx->buildAnnos) {
                string annoName = a->name->getText();
                if (annoName != "Static" && annoName != "Mut" && annoName != "Cval" && annoName != "Inline") {
                    auto* tk = a->SymbolHash()->getSymbol();
                    throw YuxError(static_cast<int>(tk->getLine()), static_cast<int>(tk->getCharPositionInLine()) + 1,
                                   ErrorCode::E3108, annoName);
                }
            }

            // #Inline 必须与 #Cval 组合
            if (isInline && !isCval) {
                throw YuxError(static_cast<int>(fieldCtx->name->getLine()),
                               static_cast<int>(fieldCtx->name->getCharPositionInLine()) + 1, ErrorCode::E3117);
            }

            // 决议 [#1.F]: v1 禁泛型 struct 上的 #Static FIELD
            if (!typeParams.empty()) {
                throw YuxError(static_cast<int>(fieldCtx->name->getLine()),
                               static_cast<int>(fieldCtx->name->getCharPositionInLine()) + 1, ErrorCode::E3157,
                               structName);
            }

            // E3150: v1 静态字段必须有 init
            if (!fieldCtx->init) {
                throw YuxError(static_cast<int>(fieldCtx->name->getLine()),
                               static_cast<int>(fieldCtx->name->getCharPositionInLine()) + 1, ErrorCode::E3150,
                               fieldCtx->name->getText());
            }

            auto typeNode = any_cast_p<TypeNode>(visit(fieldCtx->type()));
            auto initExpr = any_cast_p<ExprNode>(visit(fieldCtx->init));

            // E3155: 静态字段 init 内禁 try/catch (DRAFT-static-vars §6)
            if (ASTBuilder::exprContainsTryCatch(initExpr)) {
                throw YuxError(static_cast<int>(fieldCtx->name->getLine()),
                               static_cast<int>(fieldCtx->name->getCharPositionInLine()) + 1, ErrorCode::E3155,
                               fieldCtx->name->getText());
            }

            StructDeclNode::StaticFieldEntry sf;
            sf.name = Token(fieldCtx->name);
            sf.type = typeNode;
            sf.init = initExpr;
            sf.isMutable = isMut;
            sf.isPrivate = !sf.name.getText().empty() && sf.name.getText()[0] == '_';
            sf.isCval = isCval;
            sf.isInline = isInline;
            std::string sfName = sf.name.getText(); // 在 move 前保存，避免 use-after-move
            structDecl->addStaticField(std::move(sf));

            DEBUG_LOG_VAL("    StaticField", sfName << " : " << typeNode->getType().name << (isMut ? " #Mut" : "")
                                                    << (isCval ? " #Cval" : "") << (isInline ? " #Inline" : ""));
            continue;
        }

        auto field = any_cast_p<StructFieldNode>(visit(fieldCtx));
        structDecl->addField(field);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    file->addStructDecl(structDecl);

    // 字段符号
    for (auto field : structDecl->fields()) {
        string methodKey = structName + "." + field->name().getText();
        SymbolInfo fieldSym(SymbolKind::Variable, field->name().getText(), field->getType());
        fieldSym.moduleName = moduleName;
        file->registerSymbol(methodKey, fieldSym);
    }

    // === Step 4: 方法段（含析构）— 若任一存在则构 StructImplNode
    bool hasMethods = !ctx->fn().empty();
    bool hasDestructor = ctx->fnClean() != nullptr;
    bool hasImpl = !implRefs.empty();

    if (!(hasMethods || hasDestructor || hasImpl)) {
        return structDecl;
    }

    auto structImpl = createWithLine<StructImplNode>(ctx, file, stCtx->name);
    structImpl->setAnnos(annos.names, annos.args);
    structImpl->setTypeParams(typeParams);
    structImpl->setSpecRefs(std::move(implRefs));
    structImpl->setParentScope(file);

    stack.emplace_back(structImpl);
    _scopeStack.push_back(structImpl);

    for (auto& tp : structImpl->typeParams()) {
        structImpl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }

    if (ctx->fnClean()) {
        auto destructor = any_cast_p<FnNode>(visitFnClean(ctx->fnClean()));
        destructor->setParentScope(file);
        structImpl->setDestructor(destructor);

        string destructorName = structName + ".~" + structName;
        vector<TypeInfo> paramTypes;
        paramTypes.emplace_back(structName);

        SymbolInfo destructorSym(SymbolKind::Function, "~" + structName, TypeInfo());
        destructorSym.moduleName = moduleName;
        file->registerSymbol(destructorName, destructorSym);

        FnSymbolInfo destructorFnSym{destructorName, moduleName, paramTypes, TypeInfo()};
        file->registerFnSymbol(destructorName, destructorFnSym);
    }

    for (auto fnCtx : ctx->fn()) {
        auto header = any_cast_p<FnHeaderNode>(visitFnHeader(fnCtx->fnHeader()));
        auto fn = createWithLine<FnNode>(ctx, structImpl, header);
        fn->setParentScope(file);
        checkNoReturnHeader(header);

        stack.emplace_back(fn);
        _scopeStack.push_back(fn);

        for (auto& tp : structImpl->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }
        for (auto& tp : header->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }

        {
            vector<sp<TypeInfo>> selfArgs;
            selfArgs.push_back(make_shared<TypeInfo>(structName));
            fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
        }

        for (auto param : header->params()) {
            TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
            SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
            if (param->isFrozen()) {
                si.isFrozen = true;
            }
            fn->registerSymbol(param->name().getText(), si);
        }

        if (!fnCtx->fnBody()) {
            if (!header->hasAnno("Builtin")) {
                throw YuxError(header->getLineNumber(), header->getColumn(), ErrorCode::E2007, structName,
                               header->name().getText())
                    .withHint("结构体方法必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#Builtin` 注解");
            }
        } else if (fnCtx->fnBody()->fnExprkBody()) {
            auto exprBody = fnCtx->fnBody()->fnExprkBody();
            auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
            retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
            fn->addStatement(retStmt);
        } else if (fnCtx->fnBody()->fnBlockBody()) {
            auto blockBody = fnCtx->fnBody()->fnBlockBody();
            auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));
            for (auto stmt : stmtBlockNode->statements()) {
                fn->addStatement(stmt);
            }
            if (stmtBlockNode->hasResult()) {
                auto resultExpr = stmtBlockNode->resultExpr();
                auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
                retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
                fn->addStatement(retStmt);
            }
        }

        _scopeStack.pop_back();
        stack.pop_back();

        structImpl->addMethod(fn);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    file->addStructImpl(structImpl);

    // 方法符号
    for (auto method : structImpl->methods()) {
        string methodName = method->header()->name().getText();
        string fullName = structName;
        fullName += '.';
        fullName += methodName;

        vector<TypeInfo> paramTypes;
        paramTypes.emplace_back(structName);
        for (auto param : method->header()->params()) {
            if (param->type()) {
                paramTypes.push_back(param->type()->getType());
            }
        }

        TypeInfo retType;
        if (method->header()->retType()) {
            retType = method->header()->retType()->getType();
        }

        SymbolInfo methodSym(SymbolKind::Function, methodName, retType);
        methodSym.moduleName = moduleName;
        file->registerSymbol(fullName, methodSym);

        FnSymbolInfo methodFnSym{fullName, moduleName, paramTypes, retType};
        methodFnSym.isNoReturn = method->header()->hasAnno("NoReturn");
        methodFnSym.isConst = method->header()->hasAnno("Const");
        methodFnSym.fallibleErrType = method->header()->resolvedFallibleErr();
        if (!methodFnSym.fallibleErrType.empty() && retType.name == methodFnSym.fallibleErrType) {
            throw YuxError(method->header()->getLineNumber(), method->header()->getColumn(), ErrorCode::E7008,
                           retType.name, methodFnSym.fallibleErrType);
        }
        file->registerFnSymbol(fullName, methodFnSym);
    }

    return structDecl;
}

std::any ASTBuilder::visitFnClean(yux::yuxParser::FnCleanContext* ctx) {
    auto parent = currentScope();
    DEBUG_LOG("  Visit: FnClean (destructor)");

    auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
    if (!file) {
        file = any_cast_p<FileNode>(stack.back());
    }

    string structName;
    if (auto structImpl = dynamic_cast<StructImplNode*>(parent)) {
        structName = structImpl->structName();
    }

    auto destructorNameToken = ctx->getStart();
    auto header = createWithLine<FnHeaderNode>(ctx, file, destructorNameToken, nullptr);

    auto fn = createWithLine<FnNode>(ctx, parent, header);

    stack.emplace_back(fn);
    _scopeStack.push_back(fn);

    if (!structName.empty()) {
        // Phase 4e: 析构函数 receiver 同 §4e：Self&
        vector<sp<TypeInfo>> selfArgs;
        selfArgs.push_back(make_shared<TypeInfo>(structName));
        fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
    }

    if (ctx->fnBody()->fnExprkBody()) {
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
        fn->addStatement(retStmt);
    } else if (ctx->fnBody()->fnBlockBody()) {
        auto blockBody = ctx->fnBody()->fnBlockBody();
        auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));

        for (auto stmt : stmtBlockNode->statements()) {
            fn->addStatement(stmt);
        }

        if (stmtBlockNode->hasResult()) {
            auto resultExpr = stmtBlockNode->resultExpr();
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
            retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
            fn->addStatement(retStmt);
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();

    DEBUG_LOG_VAL("  Finished: FnClean (destructor)", structName);
    return fn;
}

namespace {

// P1-4 const-mut §6.1：字段注解允许 #Val / #Frozen / #Static。
// #Val 与 #Frozen 互斥; #Static 可与 #Frozen 组合 (DRAFT-spec-reflect Phase 1
// 的 `#Static #Frozen type Type&` 形态), 不与 #Val 组合.
// 其他名字 / 非法组合 → 抛 E3108。返回 (isVal, isFrozen, isStatic)。
struct FieldAnnoFlags {
    bool isVal = false;
    bool isFrozen = false;
    bool isStatic = false;
    bool isCval = false;
    bool isInline = false;
};
FieldAnnoFlags readFieldAnnos(const std::vector<yux::yuxParser::BuildAnnoContext*>& annos) {
    FieldAnnoFlags r;
    for (auto* a : annos) {
        const string name = a->name->getText();
        auto* tk = a->SymbolHash()->getSymbol();
        int line = static_cast<int>(tk->getLine());
        int col = static_cast<int>(tk->getCharPositionInLine()) + 1;
        if (a->annoArg() != nullptr) {
            // 字段注解 P1 不接受带实参形态（#Val(x) / #Frozen(x) / #Static(x) 无意义）。
            throw YuxError(line, col, ErrorCode::E3108, name);
        }
        if (name == "Val") {
            if (r.isFrozen) throw YuxError(line, col, ErrorCode::E3108, name);
            r.isVal = true;
        } else if (name == "Frozen") {
            if (r.isVal) throw YuxError(line, col, ErrorCode::E3108, name);
            r.isFrozen = true;
        } else if (name == "Static") {
            // DRAFT-spec-reflect Phase 1: `#Static` 字段段; 与 #Frozen 可组合, 与 #Val 互斥.
            if (r.isVal) throw YuxError(line, col, ErrorCode::E3108, name);
            r.isStatic = true;
        } else if (name == "Cval") {
            // #Cval 关联常量：编译期常量，隐含 #Static 语义。
            if (r.isVal) throw YuxError(line, col, ErrorCode::E3108, name);
            r.isCval = true;
            r.isStatic = true; // #Cval 默认带 #Static
        } else if (name == "Inline") {
            // #Inline 关联常量：使用处直接内联值，无 GlobalVariable。
            if (r.isVal) throw YuxError(line, col, ErrorCode::E3108, name);
            r.isInline = true;
        } else {
            throw YuxError(line, col, ErrorCode::E3108, name);
        }
    }
    // #Inline 只能与 #Cval 组合
    if (r.isInline && !r.isCval) {
        throw YuxError(0, 0, ErrorCode::E3117);
    }
    return r;
}

} // namespace

std::any ASTBuilder::visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) {
    auto parent = currentScope();
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    auto flags = readFieldAnnos(ctx->buildAnnos);
    DEBUG_LOG_VAL("    Field", ctx->name->getText()
                                   << " : " << type->getType().name << (flags.isVal ? " #Val" : "")
                                   << (flags.isFrozen ? " #Frozen" : "") << (flags.isStatic ? " #Static" : "")
                                   << (flags.isCval ? " #Cval" : "") << (flags.isInline ? " #Inline" : ""));
    auto node = createWithLine<StructFieldNode>(ctx, parent, ctx->name, type);
    node->setVal(flags.isVal);
    node->setFrozen(flags.isFrozen);
    node->setStatic(flags.isStatic);
    node->setCval(flags.isCval);
    node->setInline(flags.isInline);
    return node;
}

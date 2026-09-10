// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 自由函数族实现：
//   - visitFn / visitFnHeader / visitFnParams / visitFnParam
//   - visitFnParamStd / visitFnParamGroup
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。
// 结构体内方法 / 析构 / spec 见 ast_builder_struct.cpp。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "types.h"
#include <algorithm>

std::any ASTBuilder::visitFn(yux::yuxParser::FnContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto header = any_cast_p<FnHeaderNode>(visitFnHeader(ctx->fnHeader()));
    auto fn = createWithLine<FnNode>(ctx, file, header);
    fn->setParentScope(file);
    fn->setSourceText(ctxSource(ctx));
    file->addFunction(fn);

    DEBUG_LOG_VAL("Visit: Function", header->name().getText());

    // ==================== #NoReturn 头部校验 (E7012 / E7013，DRAFT-错误.md §8.3) ====================
    checkNoReturnHeader(header);

    // ==================== #Test 注解校验 (spec §11.3) ====================
    // 仅 *.test.yux 允许；与 #Builtin 互斥；签名 `fn name(): void`、必须有体。
    if (header->hasAnno("Test")) {
        // 注意：header->name() 返回 Token 值类型，getText() 返回的 const string& 绑定到临时对象会悬挂；按值拷贝
        const string fnName = header->name().getText();
        int annoLine = header->getLineNumber();
        int annoCol = header->getColumn();

        if (!_isTestFile) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2014, _sourcePath.empty() ? _moduleName : _sourcePath);
        }
        if (header->hasAnno("Builtin")) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2013, fnName);
        }
        bool sigOk = true;
        // 不允许有参数
        if (auto fnParamsCtx = ctx->fnHeader()->fnParams()) {
            if (!fnParamsCtx->fnParam().empty()) sigOk = false;
        }
        // 不允许有返回类型标注
        if (ctx->fnHeader()->retType) sigOk = false;
        // 必须有函数体
        if (!ctx->fnBody()) sigOk = false;
        if (!sigOk) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2012, fnName, fnName);
        }
    }

    stack.emplace_back(fn);
    _scopeStack.push_back(fn);

    for (auto& tp : header->typeParams()) {
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        DEBUG_LOG_VAL("  TypeParam", tp);
    }

    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        SymbolInfo si{SymbolKind::Variable, param->name().getText(), paramType};
        // DRAFT-const-mut §5：#Frozen 参数体内不可重赋（writeable=false 默认即可），并打 frozen 标
        // 让 checker 区分；非 frozen 参数仍走默认 §3.4（参数默认 val）。
        if (param->isFrozen()) {
            si.isFrozen = true;
        }
        fn->registerSymbol(param->name().getText(), si);
        DEBUG_LOG_VAL("  Param", param->name().getText()
                                     << " : " << paramType.name << (param->isFrozen() ? " #Frozen" : ""));
    }

    if (!ctx->fnBody()) {
        if (!header->hasAnno("Builtin")) {
            throw YuxError(header->getLineNumber(), header->getColumn(), ErrorCode::E2006, header->name().getText())
                .withHint("普通函数必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#Builtin` 注解");
        }
        DEBUG_LOG("  Body: (compiler-synthesized)");
    } else if (ctx->fnBody()->fnExprkBody()) {
        DEBUG_LOG("  Body: Expression");
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
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
            auto resultExpr = stmtBlockNode->resultExpr();
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
            retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
            fn->addStatement(retStmt);
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG_VAL("Finished: Function", header->name().getText());
    return fn;
}

std::any ASTBuilder::visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) {
    DEBUG_LOG_VAL("  Visit: FunctionHeader", ctx->name->getText());
    auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
    if (!file) {
        file = any_cast_p<FileNode>(stack.back());
    }

    TypeNode* retType = nullptr;
    TypeNode* retFallibleFromType = nullptr;
    if (ctx->retType) {
        auto parsed = any_cast_p<TypeNode>(visit(ctx->retType));
        std::tie(retType, retFallibleFromType) = peelFallibleRetType(parsed);
        if (retType) {
            DEBUG_LOG_VAL("    Return type", retType->getType().getFullName());
        }
    }

    auto header = createWithLine<FnHeaderNode>(ctx, file, ctx->name, retType);
    {
        auto al = collectAnnos(ctx->buildAnnos);
        for (size_t i = 0; i < al.names.size(); ++i) {
            const string& name = al.names[i];
            if (name == "Spec" || name == "Impl" || name == "Reflect") {
                auto* a = ctx->buildAnnos[i];
                throw YuxError(static_cast<int>(a->name->getLine()),
                               static_cast<int>(a->name->getCharPositionInLine()) + 1, ErrorCode::E1110);
            }
        }
        header->setAnnos(std::move(al.names), std::move(al.args));
    }

    // spec §6.3.X：返回 T& 受溯源约束（根须为 $ 或某 T& 形参），由 borrow_checker 在
    // fn body 检查时强制（E4010）；此处只放过 #Builtin 与有"潜在源"的用户函数。
    // 顶层 free fn 的 "无 T& 形参" 这种 0 源情况此处看不到（我们还没解析完形参），
    // 同样交给 borrow_checker 在拿到完整 fn 后判定。
    (void)retType; // 闸门已撤；保留语义校验给后续阶段

    if (auto gd = ctx->genericDef()) {
        vector<string> typeParams;
        vector<vector<string>> typeParamBounds;
        for (auto pCtx : gd->params) {
            // 形参名：取 typeParam.type 的 typeNormal 分支 ID
            string paramName;
            if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(pCtx->type(0))) {
                paramName = typeNormalLastName(tn);
            }
            typeParams.push_back(paramName);

            // 边界：typeParam.bounds 中每个 type → 取名（仅支持 typeNormal / typeGeneric 的基名）
            vector<string> bounds;
            for (auto bCtx : pCtx->bounds) {
                if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(bCtx)) {
                    bounds.push_back(typeNormalLastName(tn));
                } else if (auto tg = dynamic_cast<yux::yuxParser::TypeGenericContext*>(bCtx)) {
                    bounds.push_back(typeGenericLastName(tg));
                }
            }
            typeParamBounds.push_back(std::move(bounds));
        }
        header->setTypeParams(typeParams);
        header->setTypeParamBounds(typeParamBounds);
        for (const auto& i : header->typeParams()) {
            DEBUG_LOG_VAL("    TypeParam", i);
        }
    }

    if (ctx->errType) {
        header->setFallibleErrType(any_cast_p<TypeNode>(visit(ctx->errType)));
    } else if (retFallibleFromType) {
        header->setFallibleErrType(retFallibleFromType);
    }
    checkFallibleRetMismatch(header);

    stack.emplace_back(header);

    if (auto fnParamsCtx = ctx->fnParams()) {
        auto params = any_cast_v<vector<FnParamNode*>>(visit(fnParamsCtx));
        for (auto param : params) {
            header->addParam(param);
        }
    }

    stack.pop_back();

    return header;
}

std::any ASTBuilder::visitFnParams(yux::yuxParser::FnParamsContext* ctx) {
    vector<FnParamNode*> allParams;

    for (auto paramCtx : ctx->fnParam()) {
        auto params = any_cast_v<vector<FnParamNode*>>(visit(paramCtx));
        allParams.insert(allParams.end(), params.begin(), params.end());
    }

    return allParams;
}

std::any ASTBuilder::visitFnParam(yux::yuxParser::FnParamContext* ctx) {
    if (auto stdCtx = ctx->fnParamStd()) {
        return visit(stdCtx);
    }
    if (auto groupCtx = ctx->fnParamGroup()) {
        return visit(groupCtx);
    }
    return vector<FnParamNode*>();
}

namespace {

// P1-3 const-mut §5.1：参数注解只允许 #Frozen，其他名字报 E3105。
// 返回是否含 #Frozen；遇未知注解直接抛错。
bool readParamAnnos(const std::vector<yux::yuxParser::ParamAnnoContext*>& annos) {
    bool frozen = false;
    for (auto* a : annos) {
        const string name = a->ID()->getText();
        if (name == "Frozen") {
            frozen = true;
        } else {
            auto* tk = a->SymbolHash()->getSymbol();
            throw YuxError(static_cast<int>(tk->getLine()), static_cast<int>(tk->getCharPositionInLine()) + 1,
                           ErrorCode::E3105, name);
        }
    }
    return frozen;
}

} // namespace

std::any ASTBuilder::visitFnParamStd(yux::yuxParser::FnParamStdContext* ctx) {
    Node* parent = any_cast_p<FnHeaderNode>(stack.back());
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    bool frozen = readParamAnnos(ctx->paramAnnos);
    DEBUG_LOG_VAL("    Param", ctx->name->getText() << " : " << type->getType().name << (frozen ? " #Frozen" : ""));

    vector<FnParamNode*> params;
    auto node = (createWithLine<FnParamNode>(ctx, parent, ctx->name, type));
    node->setFrozen(frozen);
    params.push_back(node);
    return params;
}

std::any ASTBuilder::visitFnParamGroup(yux::yuxParser::FnParamGroupContext* ctx) {
    Node* parent = any_cast_p<FnHeaderNode>(stack.back());
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    bool frozen = readParamAnnos(ctx->paramAnnos);

    vector<FnParamNode*> params;
    for (auto nameToken : ctx->names) {
        DEBUG_LOG_VAL("    Param (group)", nameToken->getText()
                                               << " : " << type->getType().name << (frozen ? " #Frozen" : ""));
        auto node = (createWithLine<FnParamNode>(ctx, parent, nameToken, type));
        node->setFrozen(frozen);
        params.push_back(node);
    }
    return params;
}

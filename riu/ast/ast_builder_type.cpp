// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 类型族实现：
//   - visitTypeNormal / visitTypeSelf / visitTypeGeneric / visitTypeArray
//   - visitTypeTuple / visitTypeNullable / makeFunctionType / wrapRefIfAnd
//   - findEnclosingStructName

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "node/type_node.h"
#include "types.h"
#include <algorithm>

std::any ASTBuilder::visitTypeNormal(riu::riuParser::TypeNormalContext* ctx) {
    Node* parent = currentScope();
    auto path = typePathFromCtx(ctx->typePath());
    Token last = path.last();
    // DRAFT-heap-types §9 (Phase 3b): 裸 Arc 形态也走占名拒绝
    if (path.isBare() && last.getText() == "Arc") {
        throw RiuError(static_cast<int>(last.getLine()), static_cast<int>(last.getCharPositionInLine()) + 1,
                       ErrorCode::E4029, std::string("?"));
    }
    if (path.isBare() && last.getText() == "Function") {
        throw RiuError(static_cast<int>(last.getLine()), static_cast<int>(last.getCharPositionInLine()) + 1,
                       ErrorCode::E6011, std::string("Function"), static_cast<size_t>(1), static_cast<size_t>(0))
            .withHint("`Function` 是特殊泛型，须写 `Function<Ret>` 或 `Function<P1, P2, ..., Ret>`（末位为返回类型）");
    }
    DEBUG_LOG_VAL("    Type: Normal", path.dotted());
    auto* inner = static_cast<TypeNode*>(createWithLine<TypeNormalNode>(ctx, parent, std::move(path)));
    return wrapRefIfAnd(ctx, parent, inner, ctx->SymbolAnd());
}

// Self 类型字面量 (Phase 2b): 构造 TypeSelfNode 占位.
// 构造时扫 _scopeStack 找 enclosing StructImplNode 灌入 structName;
// 体外出现时 structName 留空, sema 在 Phase 2d 抛 E3115.
std::any ASTBuilder::visitTypeSelf(riu::riuParser::TypeSelfContext* ctx) {
    Node* parent = currentScope();
    DEBUG_LOG("    Type: Self");
    auto* tk = ctx->SelfType()->getSymbol();
    string structName = findEnclosingStructName();
    auto* inner = static_cast<TypeNode*>(createWithLine<TypeSelfNode>(ctx, parent, tk, structName));
    return wrapRefIfAnd(ctx, parent, inner, ctx->SymbolAnd());
}

// 扫 _scopeStack 找最内层 StructImplNode 的 structName, 找不到返回空串.
string ASTBuilder::findEnclosingStructName() const {
    for (auto it = _scopeStack.rbegin(); it != _scopeStack.rend(); ++it) { // NOLINT(modernize-loop-convert)
        if (auto* impl = dynamic_cast<StructImplNode*>(*it)) {
            return impl->structName();
        }
    }
    return {};
}

std::any ASTBuilder::visitTypeGeneric(riu::riuParser::TypeGenericContext* ctx) {
    Node* parent = currentScope();
    auto path = typePathFromCtx(ctx->typePath());
    Token last = path.last();

    auto typeArgs = typeArgsFromGenericDefWithRef(ctx->genericDefWithRef(), parent);

    // DRAFT-heap-types §9 (Phase 3b): Arc<T> 占名，v1.x 多线程主题落地后实装。
    // 此处先在类型解析点直接拒绝，避免后续路径把 Arc 误当作未知 struct 报泛错。
    if (path.isBare() && last.getText() == "Arc") {
        std::string innerName = typeArgs.empty() ? std::string("?") : typeArgs[0]->getType().name;
        throw RiuError(static_cast<int>(last.getLine()), static_cast<int>(last.getCharPositionInLine()) + 1,
                       ErrorCode::E4029, innerName);
    }

    // Weak<Function<...>> 禁（§3.7）：函数值是值类型，无 RC 头，不能 weak
    if (path.isBare() && last.getText() == "Weak" && typeArgs.size() == 1) {
        if (dynamic_cast<TypeFnNode*>(typeArgs[0]) || typeArgs[0]->getType().isFn()) {
            throw RiuError(static_cast<int>(last.getLine()), static_cast<int>(last.getCharPositionInLine()) + 1,
                           ErrorCode::E2001)
                .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
        }
    }

    TypeNode* inner;
    if (path.isBare() && last.getText() == "Function") {
        inner = makeFunctionType(ctx, parent, std::move(typeArgs), false);
    } else {
        string argsStr;
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (i > 0) argsStr += ", ";
            argsStr += typeArgs[i]->getType().name;
        }
        DEBUG_LOG_VAL("    Type: Generic", path.dotted() << "<" << argsStr << ">");
        inner = static_cast<TypeNode*>(createWithLine<TypeGenericNode>(ctx, parent, std::move(path), typeArgs));
    }
    return wrapRefIfAnd(ctx, parent, inner, ctx->SymbolAnd());
}

std::any ASTBuilder::visitTypeArray(riu::riuParser::TypeArrayContext* ctx) {
    Node* parent = currentScope();
    auto elementType = any_cast_p<TypeNode>(visit(ctx->type()));
    auto count = ctx->INT()->getSymbol();
    DEBUG_LOG_VAL("    Type: Array", "[" << count->getText() << "]");
    auto* inner = static_cast<TypeNode*>(createWithLine<TypeArrayNode>(ctx, parent, elementType, count));
    return wrapRefIfAnd(ctx, parent, inner, ctx->SymbolAnd());
}

// Function<P..., Ret> → TypeFnNode。末位永远是返回类型；() 表 unit。
TypeNode* ASTBuilder::makeFunctionType(antlr4::ParserRuleContext* ctx, Node* parent, vector<TypeNode*> typeArgs,
                                       bool nullable) {
    if (typeArgs.empty()) {
        auto* start = ctx ? ctx->getStart() : nullptr;
        throw RiuError(start ? static_cast<int>(start->getLine()) : 1,
                       start ? static_cast<int>(start->getCharPositionInLine()) + 1 : 1, ErrorCode::E6011,
                       std::string("Function"), static_cast<size_t>(1), static_cast<size_t>(0))
            .withHint("`Function` 须带类型实参：`Function<Ret>` 或 `Function<P1, P2, ..., Ret>`");
    }
    TypeNode* retType = typeArgs.back();
    vector<TypeNode*> paramTypes(typeArgs.begin(), typeArgs.end() - 1);
    if (auto* tup = dynamic_cast<TypeTupleNode*>(retType)) {
        if (tup->elementTypes().empty()) retType = nullptr;
    }
    DEBUG_LOG_VAL("    Type: Function", (nullable ? "Function<...>?" : "Function<...>")
                                            << " params=" << paramTypes.size());
    return static_cast<TypeNode*>(createWithLine<TypeFnNode>(ctx, parent, std::move(paramTypes), retType, nullable));
}

TypeNode* ASTBuilder::applyNullableSuffix(antlr4::ParserRuleContext* ctx, Node* parent, TypeNode* inner,
                                          antlr4::Token* questTok) {
    if (auto* fn = dynamic_cast<TypeFnNode*>(inner)) {
        if (!fn->nullable()) {
            fn->setNullable(true);
            return inner;
        }
    }
    Token nullableName(string("Nullable"), questTok ? questTok->getLine() : 0);
    vector<TypeNode*> args;
    args.push_back(inner);
    return static_cast<TypeNode*>(createWithLine<TypeGenericNode>(ctx, parent, nullableName, args));
}

// 元组类型 (T1, T2, ...)
// 元素列表至少 2 个（g4 语法保证），递归 visit 每个 type 子节点
std::any ASTBuilder::visitTypeTuple(riu::riuParser::TypeTupleContext* ctx) {
    Node* parent = currentScope();
    vector<TypeNode*> elementTypes;
    elementTypes.reserve(ctx->types.size());
    for (auto* tCtx : ctx->types) {
        elementTypes.push_back(any_cast_p<TypeNode>(visit(tCtx)));
    }
    DEBUG_LOG_VAL("    Type: Tuple", "elements=" << elementTypes.size());
    return static_cast<TypeNode*>(createWithLine<TypeTupleNode>(ctx, parent, std::move(elementTypes)));
}

// unit 类型 () —— 0 元素元组
std::any ASTBuilder::visitTypeUnit(riu::riuParser::TypeUnitContext* ctx) {
    Node* parent = currentScope();
    DEBUG_LOG_VAL("    Type: Unit", "()");
    return static_cast<TypeNode*>(createWithLine<TypeTupleNode>(ctx, parent, vector<TypeNode*>{}));
}

// T ! E 类型位
std::any ASTBuilder::visitTypeFallible(riu::riuParser::TypeFallibleContext* ctx) {
    Node* parent = currentScope();
    auto base = any_cast_p<TypeNode>(visit(ctx->base));
    auto err = any_cast_p<TypeNode>(visit(ctx->errType));
    if (err->getType().isNullable()) {
        auto* tok = ctx->SymbolExcl()->getSymbol();
        throw RiuError(tok ? static_cast<int>(tok->getLine()) : 0,
                       tok ? static_cast<int>(tok->getCharPositionInLine()) + 1 : 0, ErrorCode::E2001)
            .withHint("`T ! E?` is invalid — error type `E` must not be nullable");
    }
    DEBUG_LOG_VAL("    Type: Fallible", base->getType().getFullName() << " ! " << err->getType().name);
    auto* inner = static_cast<TypeNode*>(createWithLine<TypeFallibleNode>(ctx, parent, base, err));
    return wrapRefIfAnd(ctx, parent, inner, ctx->SymbolAnd());
}

// 尾部 `&` 包成 Ref<inner>。类型实参槽走 genericDefWithRef（可含 T&）。
vector<TypeNode*> ASTBuilder::typeArgsFromGenericDefWithRef(riu::riuParser::GenericDefWithRefContext* gd,
                                                            Node* parent) {
    (void)parent;
    vector<TypeNode*> typeArgs;
    if (!gd) return typeArgs;
    for (auto innerCtx : gd->types) {
        typeArgs.push_back(any_cast_p<TypeNode>(visit(innerCtx)));
    }
    return typeArgs;
}

TypeNode* ASTBuilder::wrapRefIfAnd(antlr4::ParserRuleContext* ctx, Node* parent, TypeNode* inner,
                                   antlr4::tree::TerminalNode* andTok) {
    if (!andTok) return inner;
    auto* sym = andTok->getSymbol();
    Token refName(string("Ref"), sym ? static_cast<int>(sym->getLine()) : 0);
    vector<TypeNode*> args;
    args.push_back(inner);
    return static_cast<TypeNode*>(createWithLine<TypeGenericNode>(ctx, parent, refName, args));
}

// T? 解糖为 Nullable<T>
// 直接构造 TypeGenericNode("Nullable", [T])，复用现有泛型实例化通路
// "Nullable" 名字 token 用合成构造，line 取自 SymbolQuest
std::any ASTBuilder::visitTypeNullable(riu::riuParser::TypeNullableContext* ctx) {
    Node* parent = currentScope();
    auto inner = any_cast_p<TypeNode>(visit(ctx->type()));

    auto questTok = ctx->SymbolQuest()->getSymbol();

    // Phase 1d.3：禁 Weak<T>?（DRAFT §5：Weak 已原生可空，再裹 Nullable 无意义）
    {
        auto innerTI = inner->getType();
        if (innerTI.isWeak()) {
            int line = questTok ? static_cast<int>(questTok->getLine()) : 0;
            int col = questTok ? static_cast<int>(questTok->getCharPositionInLine()) + 1 : 0;
            throw RiuError(line, col, ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Rc<T>?");
        }
    }

    DEBUG_LOG_VAL("    Type: Nullable", inner->getType().name << "?");
    auto* nul = applyNullableSuffix(ctx, parent, inner, questTok);
    return wrapRefIfAnd(ctx, parent, nul, ctx->SymbolAnd());
}

TypeInfo ASTBuilder::typeArgFromTypeCtx(riu::riuParser::TypeContext* ctx) {
    if (!ctx) return {};
    auto* start = ctx->getStart();
    int line = start ? static_cast<int>(start->getLine()) : 1;
    int col = start ? static_cast<int>(start->getCharPositionInLine()) + 1 : 1;
    auto* tn = any_cast_p<TypeNode>(visit(ctx));
    TypeInfo t = tn ? tn->getType() : TypeInfo();
    validateOwnedTypeArgs("spec type arg", {t}, line, col);
    return t;
}

SpecRef ASTBuilder::specBoundFromTypeCtx(riu::riuParser::TypeContext* ctx) {
    SpecRef r;
    if (!ctx) return r;
    auto* start = ctx->getStart();
    r.line = start ? static_cast<int>(start->getLine()) : 1;
    r.col = start ? static_cast<int>(start->getCharPositionInLine()) + 1 : 1;
    if (auto* tn = dynamic_cast<riu::riuParser::TypeNormalContext*>(ctx)) {
        if (tn->SymbolAnd()) {
            throw RiuError(r.line, r.col, ErrorCode::E4037, std::string("spec bound"))
                .withHint("边界写 `<T : D>` / `<T : D<A>>`，不要 `D&`");
        }
        r.name = typeNormalLastName(tn);
        return r;
    }
    if (auto* tg = dynamic_cast<riu::riuParser::TypeGenericContext*>(ctx)) {
        if (tg->SymbolAnd()) {
            throw RiuError(r.line, r.col, ErrorCode::E4037, std::string("spec bound"))
                .withHint("边界写 `<T : D>` / `<T : D<A>>`，不要 `D&`");
        }
        r.name = typeGenericLastName(tg);
        if (auto* gd = tg->genericDefWithRef()) {
            for (auto* inner : gd->types)
                r.typeArgs.push_back(typeArgFromTypeCtx(inner));
        }
        return r;
    }
    throw RiuError(r.line, r.col, ErrorCode::E3030, ctx->getText());
}

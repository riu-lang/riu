// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 类型族实现：
//   - visitTypeNormal / visitTypeSelf / visitTypeGeneric / visitTypeArray
//   - visitTypeFn / visitTypeTuple / visitTypeNullable
//   - buildTypeWithRef / findEnclosingStructName
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include <algorithm>
#include "ast_builder_helpers.h"
#include "ast_builder.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "types.h"

std::any ASTBuilder::visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) {
    p<Node> parent = currentScope();
    auto* sym = ctx->ID()->getSymbol();
    // DRAFT-heap-types §9 (Phase 3b): 裸 Arc 形态也走占名拒绝
    if (sym->getText() == "Arc") {
        throw YuxError(static_cast<int>(sym->getLine()),
            static_cast<int>(sym->getCharPositionInLine()) + 1,
            ErrorCode::E4029, std::string("?"));
    }
    DEBUG_LOG_VAL("    Type: Normal", sym->getText());
    return static_cast<p<TypeNode>>(createWithLine<TypeNormalNode>(ctx, parent, sym));
}

// Self 类型字面量 (Phase 2b): 构造 TypeSelfNode 占位.
// 构造时扫 _scopeStack 找 enclosing StructImplNode 灌入 structName;
// 体外出现时 structName 留空, sema 在 Phase 2d 抛 E3115.
std::any ASTBuilder::visitTypeSelf(yux::yuxParser::TypeSelfContext* ctx) {
    p<Node> parent = currentScope();
    DEBUG_LOG("    Type: Self");
    auto* tk = ctx->SelfType()->getSymbol();
    string structName = findEnclosingStructName();
    return static_cast<p<TypeNode>>(createWithLine<TypeSelfNode>(ctx, parent, tk, structName));
}

// 扫 _scopeStack 找最内层 StructImplNode 的 structName, 找不到返回空串.
string ASTBuilder::findEnclosingStructName() const {
    for (auto it = _scopeStack.rbegin(); it != _scopeStack.rend(); ++it) {
        if (auto* impl = dynamic_cast<StructImplNode*>(*it)) {
            return impl->structName();
        }
    }
    return {};
}

std::any ASTBuilder::visitTypeGeneric(yux::yuxParser::TypeGenericContext* ctx) {
    p<Node> parent = currentScope();
    auto baseName = ctx->ID()->getSymbol();

    vector<p<TypeNode>> typeArgs;
    for (auto pCtx : ctx->genericDef()->params) {
        // 类型引用位不允许 bound（spec §B.2 / §12 仅声明位允许）
        if (!pCtx->bounds.empty()) {
            auto* tk = pCtx->SymbolColon();
            throw YuxError(
                tk ? static_cast<int>(tk->getSymbol()->getLine()) : 0,
                tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                ErrorCode::E2015);
        }
        typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
    }

    // DRAFT-heap-types §9 (Phase 3b): Arc<T> 占名，v1.x 多线程主题落地后实装。
    // 此处先在类型解析点直接拒绝，避免后续路径把 Arc 误当作未知 struct 报泛错。
    if (baseName->getText() == "Arc") {
        std::string innerName = typeArgs.empty() ? std::string("?")
                                                 : typeArgs[0]->getType().name;
        throw YuxError(static_cast<int>(baseName->getLine()),
            static_cast<int>(baseName->getCharPositionInLine()) + 1,
            ErrorCode::E4029, innerName);
    }

    // Weak<fn(...)> 禁（§3.7 / §5.5）：函数值是值类型，无 RC 头，不能 weak
    if (baseName->getText() == "Weak" && typeArgs.size() == 1) {
        if (dynamic_cast<TypeFnNode*>(typeArgs[0])) {
            throw YuxError(static_cast<int>(baseName->getLine()),
                static_cast<int>(baseName->getCharPositionInLine()) + 1,
                ErrorCode::E2001)
                .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
        }
    }

    string argsStr;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) argsStr += ", ";
        argsStr += typeArgs[i]->getType().name;
    }
    DEBUG_LOG_VAL("    Type: Generic", baseName->getText() << "<" << argsStr << ">");

    return static_cast<p<TypeNode>>(createWithLine<TypeGenericNode>(ctx, parent, baseName, typeArgs));
}

std::any ASTBuilder::visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) {
    p<Node> parent = currentScope();
    auto elementType = any_cast_p<TypeNode>(visit(ctx->type()));
    auto count = ctx->INT()->getSymbol();
    DEBUG_LOG_VAL("    Type: Array", "[" << count->getText() << "]");
    return static_cast<p<TypeNode>>(createWithLine<TypeArrayNode>(ctx, parent, elementType, count));
}

// 函数类型字面量 fn(P1,...) R / fn?(...) R [#16][#23][#24]
// 形参槽位为 fnTypeParam（名可省 + 组糖），仅类型参与判等（§3.4(4)）
// retType 可省 → void；fnNullable 由可选 SymbolQuest 决定
std::any ASTBuilder::visitTypeFn(yux::yuxParser::TypeFnContext* ctx) {
    p<Node> parent = currentScope();
    bool nullable = ctx->SymbolQuest() != nullptr;

    vector<p<TypeNode>> paramTypes;
    if (auto* params = ctx->fnTypeParams()) {
        for (auto* p : params->fnTypeParam()) {
            // 三种 fnTypeParam alt 都以 typeWithRef 收尾：组糖 / 命名 / 无名
            if (auto* g = dynamic_cast<yux::yuxParser::FnTypeParamGroupContext*>(p)) {
                // a, b T → 展开为 N 份相同类型
                auto t = buildTypeWithRef(g->typeWithRef(), parent);
                size_t n = g->names.size();
                for (size_t i = 0; i < n; ++i) paramTypes.push_back(t);
            } else if (auto* n = dynamic_cast<yux::yuxParser::FnTypeParamNamedContext*>(p)) {
                paramTypes.push_back(buildTypeWithRef(n->typeWithRef(), parent));
            } else if (auto* u = dynamic_cast<yux::yuxParser::FnTypeParamUnnamedContext*>(p)) {
                paramTypes.push_back(buildTypeWithRef(u->typeWithRef(), parent));
            }
        }
    }

    p<TypeNode> retType = nullptr;
    if (auto* rt = ctx->retType) {
        retType = buildTypeWithRef(rt, parent);
    }

    DEBUG_LOG_VAL("    Type: Fn", (nullable ? "fn?(" : "fn(") << paramTypes.size() << " params)");
    return static_cast<p<TypeNode>>(createWithLine<TypeFnNode>(ctx, parent, std::move(paramTypes), retType, nullable));
}

// 元组类型 (T1, T2, ...)
// 元素列表至少 2 个（g4 语法保证），递归 visit 每个 type 子节点
std::any ASTBuilder::visitTypeTuple(yux::yuxParser::TypeTupleContext* ctx) {
    p<Node> parent = currentScope();
    vector<p<TypeNode>> elementTypes;
    elementTypes.reserve(ctx->types.size());
    for (auto* tCtx : ctx->types) {
        elementTypes.push_back(any_cast_p<TypeNode>(visit(tCtx)));
    }
    DEBUG_LOG_VAL("    Type: Tuple", "elements=" << elementTypes.size());
    return static_cast<p<TypeNode>>(createWithLine<TypeTupleNode>(ctx, parent, std::move(elementTypes)));
}

// Phase 4a: typeWithRef → TypeNode；SymbolAnd 存在则包成 Ref<inner>
// 语法已改：typeWithRef 现有 4 个分支，与 type 的 4 个分支结构对应，但每个内部位置（generic args / array elem）
// 也允许带 &，从而支持 Rc<i32&> 这类嵌套引用类型作为参数 / 局部 var 类型。
p<TypeNode> ASTBuilder::buildTypeWithRef(yux::yuxParser::TypeWithRefContext* twr, p<Node> parent) {
    using namespace yux;
    p<TypeNode> inner;
    antlr4::tree::TerminalNode* andTok = nullptr;

    if (auto n = dynamic_cast<yuxParser::TypeNormalWithRefContext*>(twr)) {
        // DRAFT-heap-types §9 (Phase 3b): Arc 占名（含裸 Arc 形态）
        if (n->ID()->getSymbol()->getText() == "Arc") {
            throw YuxError(static_cast<int>(n->ID()->getSymbol()->getLine()),
                static_cast<int>(n->ID()->getSymbol()->getCharPositionInLine()) + 1,
                ErrorCode::E4029, std::string("?"));
        }
        inner = static_cast<p<TypeNode>>(createWithLine<TypeNormalNode>(n, parent, n->ID()->getSymbol()));
        andTok = n->SymbolAnd();
    } else if (auto s = dynamic_cast<yuxParser::TypeSelfWithRefContext*>(twr)) {
        // Phase 2b: Self& —— 结构体方法返回 / 形参可写 `Self&`.
        // 出现在 impl 体外由 sema (Phase 2d) 拒收.
        auto* tk = s->SelfType()->getSymbol();
        string structName = findEnclosingStructName();
        inner = static_cast<p<TypeNode>>(createWithLine<TypeSelfNode>(s, parent, tk, structName));
        andTok = s->SymbolAnd();
    } else if (auto nul = dynamic_cast<yuxParser::TypeNullableWithRefContext*>(twr)) {
        // 内层是 type（不带 &），直接复用 visitType* 通路
        auto innerT = any_cast_p<TypeNode>(visit(nul->type()));
        if (innerT->getType().isWeak()) {
            auto qt = nul->SymbolQuest()->getSymbol();
            throw YuxError(qt ? static_cast<int>(qt->getLine()) : 0,
                qt ? static_cast<int>(qt->getCharPositionInLine()) + 1 : 0,
                ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Rc<T>?");
        }
        auto qt = nul->SymbolQuest()->getSymbol();
        Token nullableName(string("Nullable"), qt ? qt->getLine() : 0);
        vector<p<TypeNode>> args; args.push_back(innerT);
        inner = static_cast<p<TypeNode>>(createWithLine<TypeGenericNode>(nul, parent, nullableName, args));
        andTok = nul->SymbolAnd();
    } else if (auto g = dynamic_cast<yuxParser::TypeGenericWithRefContext*>(twr)) {
        auto baseName = g->ID()->getSymbol();
        vector<p<TypeNode>> typeArgs;
        for (auto innerCtx : g->genericDefWithRef()->types) {
            typeArgs.push_back(buildTypeWithRef(innerCtx, parent));
        }
        // DRAFT-heap-types §9 (Phase 3b): Arc<T> 占名
        if (baseName->getText() == "Arc") {
            std::string innerName = typeArgs.empty() ? std::string("?")
                                                     : typeArgs[0]->getType().name;
            throw YuxError(static_cast<int>(baseName->getLine()),
                static_cast<int>(baseName->getCharPositionInLine()) + 1,
                ErrorCode::E4029, innerName);
        }
        // Weak<fn(...)> 禁（§3.7 / §5.5）
        if (baseName->getText() == "Weak" && typeArgs.size() == 1) {
            if (dynamic_cast<TypeFnNode*>(typeArgs[0])) {
                throw YuxError(static_cast<int>(baseName->getLine()),
                    static_cast<int>(baseName->getCharPositionInLine()) + 1,
                    ErrorCode::E2001)
                    .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
            }
        }
        inner = static_cast<p<TypeNode>>(createWithLine<TypeGenericNode>(g, parent, baseName, typeArgs));
        andTok = g->SymbolAnd();
    } else if (auto a = dynamic_cast<yuxParser::TypeArrayWithRefContext*>(twr)) {
        auto elemType = buildTypeWithRef(a->typeWithRef(), parent);
        auto count = a->INT()->getSymbol();
        inner = static_cast<p<TypeNode>>(createWithLine<TypeArrayNode>(a, parent, elemType, count));
        andTok = a->SymbolAnd();
    } else if (auto t = dynamic_cast<yuxParser::TypeTupleWithRefContext*>(twr)) {
        // 元组 (T1, T2, ...)；每个元素本身可带 & 引用
        // 元组本身不带尾随 &（g4 中 typeTupleWithRef 没有 SymbolAnd?）
        vector<p<TypeNode>> elementTypes;
        elementTypes.reserve(t->types.size());
        for (auto* eCtx : t->types) {
            elementTypes.push_back(buildTypeWithRef(eCtx, parent));
        }
        inner = static_cast<p<TypeNode>>(createWithLine<TypeTupleNode>(t, parent, std::move(elementTypes)));
        andTok = nullptr;
    } else if (auto* fn = dynamic_cast<yuxParser::TypeFnWithRefContext*>(twr)) {
        // 函数类型字面量在 typeWithRef 位（fn 形参 / 返回值 / 局部 var）
        // 末尾 & 表借用一个函数值；fnTypeParam 三 alt 同 visitTypeFn 处理
        bool nullable = fn->SymbolQuest() != nullptr;
        vector<p<TypeNode>> paramTypes;
        if (auto* params = fn->fnTypeParams()) {
            for (auto* fp : params->fnTypeParam()) {
                if (auto* g = dynamic_cast<yuxParser::FnTypeParamGroupContext*>(fp)) {
                    auto t2 = buildTypeWithRef(g->typeWithRef(), parent);
                    size_t n = g->names.size();
                    for (size_t i = 0; i < n; ++i) paramTypes.push_back(t2);
                } else if (auto* n2 = dynamic_cast<yuxParser::FnTypeParamNamedContext*>(fp)) {
                    paramTypes.push_back(buildTypeWithRef(n2->typeWithRef(), parent));
                } else if (auto* u = dynamic_cast<yuxParser::FnTypeParamUnnamedContext*>(fp)) {
                    paramTypes.push_back(buildTypeWithRef(u->typeWithRef(), parent));
                }
            }
        }
        p<TypeNode> retType = nullptr;
        if (auto* rt = fn->retType) {
            retType = buildTypeWithRef(rt, parent);
        }
        inner = static_cast<p<TypeNode>>(createWithLine<TypeFnNode>(fn, parent, std::move(paramTypes), retType, nullable));
        andTok = fn->SymbolAnd();
    } else {
        throw YuxError(1, ErrorCode::E2002);
    }

    if (andTok) {
        auto sym = andTok->getSymbol();
        Token refName(string("Ref"), sym ? sym->getLine() : 0);
        vector<p<TypeNode>> args;
        args.push_back(inner);
        return static_cast<p<TypeNode>>(createWithLine<TypeGenericNode>(twr, parent, refName, args));
    }
    return inner;
}

// T? 解糖为 Nullable<T>
// 直接构造 TypeGenericNode("Nullable", [T])，复用现有泛型实例化通路
// "Nullable" 名字 token 用合成构造，line 取自 SymbolQuest
std::any ASTBuilder::visitTypeNullable(yux::yuxParser::TypeNullableContext* ctx) {
    p<Node> parent = currentScope();
    auto inner = any_cast_p<TypeNode>(visit(ctx->type()));

    auto questTok = ctx->SymbolQuest()->getSymbol();

    // Phase 1d.3：禁 Weak<T>?（DRAFT §5：Weak 已原生可空，再裹 Nullable 无意义）
    {
        auto innerTI = inner->getType();
        if (innerTI.isWeak()) {
            int line = questTok ? static_cast<int>(questTok->getLine()) : 0;
            int col  = questTok ? static_cast<int>(questTok->getCharPositionInLine()) + 1 : 0;
            throw YuxError(line, col, ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Rc<T>?");
        }
    }

    Token nullableName(string("Nullable"), questTok ? questTok->getLine() : 0);

    vector<p<TypeNode>> args;
    args.push_back(inner);

    DEBUG_LOG_VAL("    Type: Nullable", inner->getType().name << "?");
    return static_cast<p<TypeNode>>(createWithLine<TypeGenericNode>(ctx, parent, nullableName, args));
}

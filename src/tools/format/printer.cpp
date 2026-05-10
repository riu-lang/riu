// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// AST 驱动格式化器实现
//
// Phase 4a 范围（增量, 表达式简单形态走 Doc, 块形态仍 raw）:
// - 简单内联表达式: literal / paren / unary / binary（含 shift/compare/eq/bool/
//   nullElse）/ call(无尾随 lambda) / dot / tupleMember / get / getRef /
//   array / tuple / enumCtor / arrayInit / this / lambdaSingle / lambdaParen
// - 仍走 raw: lambdaBlock / lambdaZeroBlock / tryCatch / match / ifElse /
//   oneLineIfElse / ifElsePreValue / call 含尾随 lambda
// - 已接入 fnExprkBody（`fn name() T = <expr>` 体）
//
// Phase 3 范围:
// - 类型节点全套：type / typeWithRef 的 Normal / Generic / Nullable / Array
//   / Tuple / Fn 各变体，统一处理后置 `&` 借用标记
// - genericDef / genericDefWithRef / fnTypeParams / fnTypeParam / fnParams /
//   fnParam 全套
// - fnHeader / fnBody 框架：fnHeader 用 Doc 重排参数；fnBody 表达式体走
//   `= <rawExpr>`、块体走 `{...}` 把 statementBlock 内容当原文嵌入并按
//   缩进规整一次
// - externDelc 用 Doc 重排：buildAnnos + extern { fnHeader... }
//
// 已实装顶层：imports / buildAnno / globalConst / aliasDecl / fn / externDelc
// 仍回退顶层：enumDecl / structDecl / structImpl / draftDecl（Phase 4 起细化）
//
// 渲染：每个顶层 item 自身的 Doc 用 render() 处理，块之间用纯 "\n" 拼接，
// 避免顶层间互相影响 group 决策。

#include "tools/format/printer.h"

#include <cstddef>
#include <string>
#include <vector>

#include "antlr4-runtime.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include "tools/format/doc.h"
#include "tools/format/render.h"
#include "tools/format/trivia.h"
#include "tools/formatter.h"

namespace yux::format {

namespace {

// ==================== 工具：原文截取 ====================

// 抓取一棵子树覆盖的源码原文。BufferedTokenStream::getText(Interval) 不过滤
// 通道，因此返回的字符串与输入完全一致（含其中的 hidden 注释 / 空白）。
std::string rawSpan(antlr4::CommonTokenStream& tokens, antlr4::ParserRuleContext* ctx) {
    if (ctx == nullptr || ctx->start == nullptr || ctx->stop == nullptr) return {};
    return tokens.getText(antlr4::misc::Interval(
        ctx->start->getTokenIndex(), ctx->stop->getTokenIndex()));
}

// 把多行原文按"原始公共缩进"剥掉，再补上目标缩进。
// 用于把回退区块嵌入到当前缩进语境里时不带入旧缩进。
// 当前回退路径都在顶层 (indent=0)，简单起见保留原样不做缩进规整。
std::string trimRightLineEnds(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

// ==================== Printer ====================

class Printer {
public:
    Printer(antlr4::CommonTokenStream& tokens, const TriviaMap& trivia)
        : tokens_(tokens), trivia_(trivia) {}

    Doc programDoc(yuxParser::ProgramContext* ctx);

private:
    antlr4::CommonTokenStream& tokens_;
    const TriviaMap& trivia_;

    // 顶层 item 的渲染单元：要么是结构化 Doc (走 render 出现刷格式)，
    // 要么是纯原文块 (按行直出，避免再次 break/flat 决策影响)
    struct Item {
        std::vector<TriviaComment> leading;  // 项前 leading 注释
        bool blankBefore = false;            // 是否需要在前面插入一个空行
        bool blankAfterLeading = false;      // leading 与项之间是否需要空行
        Doc doc;                             // 结构化输出 (与 raw 二选一)
        std::string raw;                     // 原文回退输出 (与 doc 二选一)
    };

    Item visitTopLevel(antlr4::tree::ParseTree* child);

    Doc importsDoc(yuxParser::ImportsContext* ctx);
    Doc buildAnnoDoc(yuxParser::BuildAnnoContext* ctx);
    Doc globalConstDoc(yuxParser::GlobalConstContext* ctx);
    Doc aliasDeclDoc(yuxParser::AliasDeclContext* ctx);
    Doc fnDoc(yuxParser::FnContext* ctx);
    Doc fnHeaderDoc(yuxParser::FnHeaderContext* ctx);
    Doc fnBodyDoc(yuxParser::FnBodyContext* ctx, int indentLevel);
    Doc externDelcDoc(yuxParser::ExternDelcContext* ctx);

    // 类型 / 泛型 / 形参
    Doc typeDoc(yuxParser::TypeContext* ctx);
    Doc typeWithRefDoc(yuxParser::TypeWithRefContext* ctx);
    Doc genericDefDoc(yuxParser::GenericDefContext* ctx);
    Doc genericDefWithRefDoc(yuxParser::GenericDefWithRefContext* ctx);
    Doc fnTypeParamsDoc(yuxParser::FnTypeParamsContext* ctx);
    Doc fnTypeParamDoc(yuxParser::FnTypeParamContext* ctx);
    Doc fnParamsDoc(yuxParser::FnParamsContext* ctx);
    Doc fnParamDoc(yuxParser::FnParamContext* ctx);

    // 表达式
    Doc exprDoc(yuxParser::ExprContext* ctx);
    Doc lambdaParamsDoc(yuxParser::LambdaParamsContext* ctx);
    Doc lambdaParamDoc(yuxParser::LambdaParamContext* ctx);

    // 语句 / 块
    // indentLevel: 当前语句所处的"逻辑缩进层" (顶层 fn body 内是 1，
    // 嵌套 statementBlock 内逐层 +1)，用于多行 raw 回退时计算目标列
    Doc statementBlockDoc(yuxParser::StatementBlockContext* ctx, int indentLevel);
    Doc statementDoc(yuxParser::StatementContext* ctx, int indentLevel);

    // 把多行原文按"原起始列 → 目标列"做整体平移：第 0 行原样，第 i>0 行
    // 把至多 srcStartCol 个前导空格替换为 targetCol 空格
    static std::string reindentMultilineRaw(const std::string& raw,
                                            std::size_t srcStartCol,
                                            std::size_t targetCol);

    // 抓 ctx 的原文，但去掉末尾的 LineEnd token（语句规则末尾固定挂 LineEnd）
    std::string rawSpanWithoutTrailingLineEnd(antlr4::ParserRuleContext* ctx);

    // 取顶层 item 起始 default token 的 index，用于查 trivia
    static std::size_t startTokenIndex(antlr4::ParserRuleContext* ctx) {
        return ctx->start->getTokenIndex();
    }
};

Doc Printer::importsDoc(yuxParser::ImportsContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("use "));
    const auto& pkgs = ctx->pkgs;
    for (std::size_t i = 0; i < pkgs.size(); ++i) {
        if (i > 0) parts.push_back(text("."));
        parts.push_back(text(pkgs[i]->getText()));
    }
    if (ctx->useAll != nullptr) {
        parts.push_back(text(".*"));
    }
    return concat(std::move(parts));
}

Doc Printer::buildAnnoDoc(yuxParser::BuildAnnoContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("#"));
    parts.push_back(text(ctx->name->getText()));
    if (ctx->ParStart() != nullptr) {
        parts.push_back(text("("));
        if (ctx->arg != nullptr) parts.push_back(text(ctx->arg->getText()));
        parts.push_back(text(")"));
    }
    return concat(std::move(parts));
}

Doc Printer::globalConstDoc(yuxParser::GlobalConstContext* ctx) {
    std::vector<Doc> parts;
    for (auto* anno : ctx->buildAnnos) {
        parts.push_back(buildAnnoDoc(anno));
        parts.push_back(hardline());
    }
    parts.push_back(text("cval "));
    parts.push_back(text(ctx->name->getText()));
    parts.push_back(text(" "));
    // type 还没实装；用原文
    parts.push_back(text(rawSpan(tokens_, ctx->type())));
    parts.push_back(text(" = "));
    parts.push_back(text(rawSpan(tokens_, ctx->literal())));
    return concat(std::move(parts));
}

Doc Printer::aliasDeclDoc(yuxParser::AliasDeclContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text(ctx->ID()->getText()));
    if (ctx->genericDef() != nullptr) {
        parts.push_back(genericDefDoc(ctx->genericDef()));
    }
    parts.push_back(text(" = "));
    parts.push_back(typeDoc(ctx->type()));
    return concat(std::move(parts));
}

// ==================== 类型节点 ====================

Doc Printer::typeDoc(yuxParser::TypeContext* ctx) {
    if (auto* n = dynamic_cast<yuxParser::TypeNormalContext*>(ctx)) {
        return text(n->ID()->getText());
    }
    if (auto* n = dynamic_cast<yuxParser::TypeNullableContext*>(ctx)) {
        return concat({typeDoc(n->type()), text("?")});
    }
    if (auto* n = dynamic_cast<yuxParser::TypeGenericContext*>(ctx)) {
        return concat({text(n->ID()->getText()), genericDefDoc(n->genericDef())});
    }
    if (auto* n = dynamic_cast<yuxParser::TypeArrayContext*>(ctx)) {
        return concat({
            text("["), typeDoc(n->type()),
            text(" * "), text(n->INT()->getText()), text("]"),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::TypeTupleContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("("));
        for (std::size_t i = 0; i < n->types.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(typeDoc(n->types[i]));
        }
        parts.push_back(text(")"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::TypeFnContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("fn"));
        if (n->SymbolQuest() != nullptr) parts.push_back(text("?"));
        parts.push_back(text("("));
        if (n->fnTypeParams() != nullptr) {
            parts.push_back(fnTypeParamsDoc(n->fnTypeParams()));
        }
        parts.push_back(text(")"));
        if (n->retType != nullptr) {
            // fn 类型字面量内：返回类型与 ) 之间不加空格 (`fn(i32)i32`)
            parts.push_back(typeWithRefDoc(n->retType));
        }
        return concat(std::move(parts));
    }
    // 兜底：未识别变体，使用原文
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::typeWithRefDoc(yuxParser::TypeWithRefContext* ctx) {
    auto refSuffix = [](antlr4::tree::TerminalNode* andTok) -> Doc {
        return andTok != nullptr ? text("&") : text("");
    };
    if (auto* n = dynamic_cast<yuxParser::TypeNormalWithRefContext*>(ctx)) {
        return concat({text(n->ID()->getText()), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<yuxParser::TypeNullableWithRefContext*>(ctx)) {
        return concat({typeDoc(n->type()), text("?"), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<yuxParser::TypeGenericWithRefContext*>(ctx)) {
        return concat({
            text(n->ID()->getText()),
            genericDefWithRefDoc(n->genericDefWithRef()),
            refSuffix(n->SymbolAnd()),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::TypeArrayWithRefContext*>(ctx)) {
        return concat({
            text("["), typeWithRefDoc(n->typeWithRef()),
            text(" * "), text(n->INT()->getText()), text("]"),
            refSuffix(n->SymbolAnd()),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::TypeTupleWithRefContext*>(ctx)) {
        // 语法上 typeTupleWithRef 没有外层 `&`：(T1, T2) 不可借用整体
        std::vector<Doc> parts;
        parts.push_back(text("("));
        for (std::size_t i = 0; i < n->types.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(typeWithRefDoc(n->types[i]));
        }
        parts.push_back(text(")"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::TypeFnWithRefContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("fn"));
        if (n->SymbolQuest() != nullptr) parts.push_back(text("?"));
        parts.push_back(text("("));
        if (n->fnTypeParams() != nullptr) {
            parts.push_back(fnTypeParamsDoc(n->fnTypeParams()));
        }
        parts.push_back(text(")"));
        if (n->retType != nullptr) {
            parts.push_back(typeWithRefDoc(n->retType));
        }
        parts.push_back(refSuffix(n->SymbolAnd()));
        return concat(std::move(parts));
    }
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::genericDefDoc(yuxParser::GenericDefContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("<"));
    for (std::size_t i = 0; i < ctx->params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        auto* tp = ctx->params[i];
        // typeParam = type (: bound (+ bound)*)?；首个 type 是参数本身
        if (!tp->type().empty()) {
            parts.push_back(typeDoc(tp->type()[0]));
        }
        if (tp->bounds.size() > 0) {
            // 仓库惯例 `T : Bound` (冒号两侧均有空格)
            parts.push_back(text(" : "));
            for (std::size_t b = 0; b < tp->bounds.size(); ++b) {
                if (b > 0) parts.push_back(text(" + "));
                parts.push_back(typeDoc(tp->bounds[b]));
            }
        }
    }
    parts.push_back(text(">"));
    return concat(std::move(parts));
}

Doc Printer::genericDefWithRefDoc(yuxParser::GenericDefWithRefContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("<"));
    for (std::size_t i = 0; i < ctx->types.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(typeWithRefDoc(ctx->types[i]));
    }
    parts.push_back(text(">"));
    return concat(std::move(parts));
}

Doc Printer::fnTypeParamsDoc(yuxParser::FnTypeParamsContext* ctx) {
    std::vector<Doc> parts;
    auto params = ctx->fnTypeParam();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(fnTypeParamDoc(params[i]));
    }
    return concat(std::move(parts));
}

Doc Printer::fnTypeParamDoc(yuxParser::FnTypeParamContext* ctx) {
    if (auto* n = dynamic_cast<yuxParser::FnTypeParamNamedContext*>(ctx)) {
        return concat({text(n->ID()->getText()), text(" "), typeWithRefDoc(n->typeWithRef())});
    }
    if (auto* n = dynamic_cast<yuxParser::FnTypeParamGroupContext*>(ctx)) {
        std::vector<Doc> parts;
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        parts.push_back(text(" "));
        parts.push_back(typeWithRefDoc(n->typeWithRef()));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::FnTypeParamUnnamedContext*>(ctx)) {
        return typeWithRefDoc(n->typeWithRef());
    }
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::fnParamsDoc(yuxParser::FnParamsContext* ctx) {
    std::vector<Doc> parts;
    auto params = ctx->fnParam();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(fnParamDoc(params[i]));
    }
    return concat(std::move(parts));
}

Doc Printer::fnParamDoc(yuxParser::FnParamContext* ctx) {
    if (ctx->fnParamStd() != nullptr) {
        auto* n = ctx->fnParamStd();
        return concat({text(n->ID()->getText()), text(" "), typeWithRefDoc(n->typeWithRef())});
    }
    if (ctx->fnParamGroup() != nullptr) {
        auto* n = ctx->fnParamGroup();
        std::vector<Doc> parts;
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        parts.push_back(text(" "));
        parts.push_back(typeWithRefDoc(n->typeWithRef()));
        return concat(std::move(parts));
    }
    return text(rawSpan(tokens_, ctx));
}

// ==================== 表达式 ====================

Doc Printer::lambdaParamDoc(yuxParser::LambdaParamContext* ctx) {
    if (auto* n = dynamic_cast<yuxParser::LambdaParamStdContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text(n->name->getText()));
        if (n->typeWithRef() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeWithRefDoc(n->typeWithRef()));
        }
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::LambdaParamGroupContext*>(ctx)) {
        std::vector<Doc> parts;
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        if (n->typeWithRef() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeWithRefDoc(n->typeWithRef()));
        }
        return concat(std::move(parts));
    }
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::lambdaParamsDoc(yuxParser::LambdaParamsContext* ctx) {
    std::vector<Doc> parts;
    auto params = ctx->lambdaParam();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(lambdaParamDoc(params[i]));
    }
    return concat(std::move(parts));
}

// 表达式分发：简单内联走 Doc，块形态走 raw 回退（Phase 4b/5 再细化）
Doc Printer::exprDoc(yuxParser::ExprContext* ctx) {
    // ----- 字面 / 原子 -----
    if (auto* n = dynamic_cast<yuxParser::ExprLiteralContext*>(ctx)) {
        // literal 整体保持原文（数字格式 / 字符串模板内表达式不在本期重排）
        return text(rawSpan(tokens_, n->literal()));
    }
    if (dynamic_cast<yuxParser::ExprThisContext*>(ctx)) {
        return text("$");
    }
    if (auto* n = dynamic_cast<yuxParser::ExprParenContext*>(ctx)) {
        return concat({text("("), exprDoc(n->expr()), text(")")});
    }
    // ----- 一元 -----
    if (auto* n = dynamic_cast<yuxParser::ExprUnaryContext*>(ctx)) {
        return concat({text(n->op->getText()), exprDoc(n->right)});
    }
    // ----- 二元（统一通过 op 文本 / 子规则原文）-----
    auto binDoc = [&](yuxParser::ExprContext* l, const std::string& opText,
                      yuxParser::ExprContext* r) -> Doc {
        return concat({exprDoc(l), text(" "), text(opText), text(" "), exprDoc(r)});
    };
    if (auto* n = dynamic_cast<yuxParser::ExprAddSubContext*>(ctx)) {
        return binDoc(n->left, n->op->getText(), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprMulDivModContext*>(ctx)) {
        return binDoc(n->left, n->op->getText(), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprBinOpContext*>(ctx)) {
        return binDoc(n->left, n->op->getText(), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprShiftContext*>(ctx)) {
        // opShift 子规则：抓原文（`<<` / `>>`）
        return binDoc(n->left, rawSpan(tokens_, n->opShift()), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprCompareContext*>(ctx)) {
        return binDoc(n->left, rawSpan(tokens_, n->opCompare()), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprEqContext*>(ctx)) {
        return binDoc(n->left, rawSpan(tokens_, n->opEq()), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprBoolContext*>(ctx)) {
        return binDoc(n->left, rawSpan(tokens_, n->opBool()), n->right);
    }
    if (auto* n = dynamic_cast<yuxParser::ExprNullElseContext*>(ctx)) {
        auto exprs = n->expr();
        return concat({exprDoc(exprs[0]), text(" ?? "), exprDoc(exprs[1])});
    }
    // ----- 成员 / 索引 / 调用 -----
    if (auto* n = dynamic_cast<yuxParser::ExprDotContext*>(ctx)) {
        // a.b / a?.b （链式由递归自然展开）
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->left));
        parts.push_back(text(n->SymbolQuest() != nullptr ? "?." : "."));
        // member 是 ID+；语法上每个 exprDot 节点只产生一个 ID（链式靠左递归）
        const auto& ids = n->member;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) parts.push_back(text("."));
            parts.push_back(text(ids[i]->getText()));
        }
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprTupleMemberContext*>(ctx)) {
        return concat({exprDoc(n->left), text(n->member->getText())});
    }
    if (auto* n = dynamic_cast<yuxParser::ExprGetContext*>(ctx)) {
        // 语法：expr GetStart args+=expr (...) GetEnd —— 接收者是无标签 expr，
        // 即 expr(0)（args 不包含它）
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->expr(0)));
        parts.push_back(text("["));
        for (std::size_t i = 0; i < n->args.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(exprDoc(n->args[i]));
        }
        parts.push_back(text("]"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprGetRefContext*>(ctx)) {
        // &obj.subs.subs...
        std::vector<Doc> parts;
        parts.push_back(text("&"));
        parts.push_back(text(n->obj->getText()));
        for (auto* sub : n->subs) {
            parts.push_back(text("."));
            parts.push_back(text(sub->getText()));
        }
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprCallContext*>(ctx)) {
        if (n->trailing != nullptr) {
            // 含尾随 lambda：暂走 raw（Phase 4b 再结构化）
            return text(rawSpan(tokens_, n));
        }
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->left));
        if (n->genericDef() != nullptr) {
            parts.push_back(text(":"));
            parts.push_back(genericDefDoc(n->genericDef()));
        }
        parts.push_back(text("("));
        for (std::size_t i = 0; i < n->args.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(exprDoc(n->args[i]));
        }
        parts.push_back(text(")"));
        if (n->errPropagate != nullptr) parts.push_back(text("!"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprCallTrailingOnlyContext*>(ctx)) {
        // e { ... }；尾随 lambda 是块形，整体 raw
        return text(rawSpan(tokens_, n));
    }
    // ----- 集合 / 元组 / 枚举构造 -----
    if (auto* n = dynamic_cast<yuxParser::ExprArrayContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("["));
        for (std::size_t i = 0; i < n->velues.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(exprDoc(n->velues[i]));
        }
        parts.push_back(text("]"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprTupleContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("("));
        for (std::size_t i = 0; i < n->values.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(exprDoc(n->values[i]));
        }
        parts.push_back(text(")"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprArrayInitContext*>(ctx)) {
        // [literal ... typeWithRef?]
        std::vector<Doc> parts;
        parts.push_back(text("["));
        parts.push_back(text(rawSpan(tokens_, n->value)));
        parts.push_back(text(" ..."));
        if (n->type() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeDoc(n->type()));
        }
        parts.push_back(text("]"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::ExprEnumCtorContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text(n->enumName->getText()));
        parts.push_back(text("::"));
        parts.push_back(text(n->variant->getText()));
        if (n->ParStart() != nullptr) {
            parts.push_back(text("("));
            for (std::size_t i = 0; i < n->args.size(); ++i) {
                if (i > 0) parts.push_back(text(", "));
                parts.push_back(exprDoc(n->args[i]));
            }
            parts.push_back(text(")"));
        }
        return concat(std::move(parts));
    }
    // ----- lambda（仅单表达式形态走 Doc）-----
    if (auto* n = dynamic_cast<yuxParser::ExprLambdaSingleContext*>(ctx)) {
        return concat({
            text(n->name->getText()),
            text(" => "),
            exprDoc(n->body->expr()),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::ExprLambdaParenContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("("));
        if (n->lambdaParams() != nullptr) {
            parts.push_back(lambdaParamsDoc(n->lambdaParams()));
        }
        parts.push_back(text(")"));
        if (n->retType != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeWithRefDoc(n->retType));
        }
        parts.push_back(text(" => "));
        parts.push_back(exprDoc(n->body->expr()));
        return concat(std::move(parts));
    }
    // ----- 其余块形 / 控制流：raw -----
    // ExprLambdaBlock / ExprLambdaZeroBlock / ExprTryCatch / ExprMatch /
    // ExprIfElse / ExprOneLineIfElse / ExprIfElsePreValue
    return text(rawSpan(tokens_, ctx));
}

// ==================== fn / extern ====================

Doc Printer::fnHeaderDoc(yuxParser::FnHeaderContext* ctx) {
    std::vector<Doc> parts;
    for (auto* anno : ctx->buildAnnos) {
        parts.push_back(buildAnnoDoc(anno));
        parts.push_back(hardline());
    }
    parts.push_back(text("fn "));
    parts.push_back(text(ctx->name->getText()));
    if (ctx->genericDef() != nullptr) {
        parts.push_back(genericDefDoc(ctx->genericDef()));
    }
    parts.push_back(text("("));
    if (ctx->fnParams() != nullptr) {
        parts.push_back(fnParamsDoc(ctx->fnParams()));
    }
    parts.push_back(text(")"));
    if (ctx->retType != nullptr) {
        parts.push_back(text(" "));
        parts.push_back(typeWithRefDoc(ctx->retType));
    }
    return concat(std::move(parts));
}

Doc Printer::fnBodyDoc(yuxParser::FnBodyContext* ctx, int indentLevel) {
    if (ctx->fnExprkBody() != nullptr) {
        auto* eb = ctx->fnExprkBody();
        return concat({text("= "), exprDoc(eb->expr())});
    }
    if (ctx->fnBlockBody() != nullptr) {
        return statementBlockDoc(ctx->fnBlockBody()->statementBlock(), indentLevel);
    }
    return text(rawSpan(tokens_, ctx));
}

// ==================== 语句 / 块 ====================

std::string Printer::reindentMultilineRaw(const std::string& raw,
                                          std::size_t srcStartCol,
                                          std::size_t targetCol) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : raw) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else if (c != '\r') cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }
    std::string pad(targetCol, ' ');
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');
        if (i == 0) {
            out += lines[i];
        } else {
            std::size_t k = 0;
            while (k < lines[i].size() && k < srcStartCol && lines[i][k] == ' ') ++k;
            out += pad;
            out += lines[i].substr(k);
        }
    }
    return out;
}

std::string Printer::rawSpanWithoutTrailingLineEnd(antlr4::ParserRuleContext* ctx) {
    if (ctx == nullptr || ctx->start == nullptr || ctx->stop == nullptr) return {};
    std::size_t startIdx = ctx->start->getTokenIndex();
    std::size_t stopIdx = ctx->stop->getTokenIndex();
    // 末尾若为 LineEnd（默认通道）则回退一个 token
    if (ctx->stop->getType() == yuxLexer::LineEnd && stopIdx > startIdx) {
        --stopIdx;
    }
    return tokens_.getText(antlr4::misc::Interval(startIdx, stopIdx));
}

// 判定一个 expr 是否为"块形 / 多行"形态：含块的 lambda、try-catch、match、
// if-else 各形态，以及含尾随 lambda 的 call。这些不在 Phase 4a 结构化覆盖里。
static bool isBlockExpr(yuxParser::ExprContext* e) {
    if (dynamic_cast<yuxParser::ExprLambdaBlockContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprLambdaZeroBlockContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprTryCatchContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprMatchContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprIfElseContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprOneLineIfElseContext*>(e)) return true;
    if (dynamic_cast<yuxParser::ExprIfElsePreValueContext*>(e)) return true;
    if (auto* c = dynamic_cast<yuxParser::ExprCallContext*>(e)) {
        return c->trailing != nullptr;
    }
    if (dynamic_cast<yuxParser::ExprCallTrailingOnlyContext*>(e)) return true;
    return false;
}

Doc Printer::statementDoc(yuxParser::StatementContext* ctx, int indentLevel) {
    // 多行语句（典型：含块形 expr）一律走 raw 回退；目标列 = indentLevel*2
    std::size_t startLine = ctx->start->getLine();
    std::size_t stopLine = ctx->stop->getLine();
    if (startLine != stopLine) {
        std::size_t srcCol = ctx->start->getCharPositionInLine();
        std::string raw = rawSpanWithoutTrailingLineEnd(ctx);
        return text(reindentMultilineRaw(raw, srcCol,
                                         static_cast<std::size_t>(indentLevel) * 2));
    }

    if (auto* n = dynamic_cast<yuxParser::StatementDeclareContext*>(ctx)) {
        // val name Type
        return concat({
            text(n->DeclKey()->getText()), text(" "),
            text(n->name->getText()), text(" "),
            typeDoc(n->type()),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::StatementDeclareAssignContext*>(ctx)) {
        // var name (Type)? = expr
        std::vector<Doc> parts;
        parts.push_back(text(n->DeclKey()->getText()));
        parts.push_back(text(" "));
        parts.push_back(text(n->name->getText()));
        if (n->typeWithRef() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeWithRefDoc(n->typeWithRef()));
        }
        parts.push_back(text(" = "));
        parts.push_back(exprDoc(n->expr()));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::StatementDeclareAssignTupleContext*>(ctx)) {
        // var (a, b) (Type)? = expr
        std::vector<Doc> parts;
        parts.push_back(text(n->DeclKey()->getText()));
        parts.push_back(text(" ("));
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        parts.push_back(text(")"));
        if (n->typeWithRef() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeWithRefDoc(n->typeWithRef()));
        }
        parts.push_back(text(" = "));
        parts.push_back(exprDoc(n->expr()));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::StatementSetContext*>(ctx)) {
        // obj[args...] = value
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->obj));
        parts.push_back(text("["));
        for (std::size_t i = 0; i < n->args.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(exprDoc(n->args[i]));
        }
        parts.push_back(text("] = "));
        parts.push_back(exprDoc(n->value));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::StatementLoopContext*>(ctx)) {
        return concat({
            text("loop "),
            statementBlockDoc(n->statementBlock(), indentLevel),
        });
    }
    if (auto* n = dynamic_cast<yuxParser::StatementAssignContext*>(ctx)) {
        // obj (.id|DOT_NUM)* opAssign expr
        std::vector<Doc> parts;
        parts.push_back(text(n->obj->getText())); // ID 或 `$`
        for (auto* sub : n->subs) {
            if (sub->getType() == yuxLexer::DOT_NUM) {
                parts.push_back(text(sub->getText())); // 已含 `.`
            } else {
                parts.push_back(text("."));
                parts.push_back(text(sub->getText()));
            }
        }
        parts.push_back(text(" "));
        parts.push_back(text(rawSpan(tokens_, n->opAssign())));
        parts.push_back(text(" "));
        parts.push_back(exprDoc(n->expr()));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::StatementExprContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->expr()));
        if (n->SymbolSemicolon() != nullptr) parts.push_back(text(";"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<yuxParser::StatementRetContext*>(ctx)) {
        return concat({text("ret "), exprDoc(n->expr())});
    }
    if (dynamic_cast<yuxParser::StatementRetVoidContext*>(ctx)) {
        return text("ret;");
    }
    if (dynamic_cast<yuxParser::StatementBreakContext*>(ctx)) {
        return text("break;");
    }
    // 兜底（语法演化时保护）
    std::size_t srcCol = ctx->start->getCharPositionInLine();
    return text(reindentMultilineRaw(rawSpanWithoutTrailingLineEnd(ctx), srcCol,
                                     static_cast<std::size_t>(indentLevel) * 2));
}

Doc Printer::statementBlockDoc(yuxParser::StatementBlockContext* ctx, int indentLevel) {
    // `{` 后插入 indent(2,...) 包住所有语句行；最后一个 hardline 在 indent
    // 外面，让闭合 `}` 落到块入口列。
    std::vector<Doc> inner;
    auto stmts = ctx->statement();
    bool first = true;
    for (auto* s : stmts) {
        std::size_t startIdx = s->start->getTokenIndex();
        auto blankIt = trivia_.blankBefore.find(startIdx);
        bool stmtBlank = (blankIt != trivia_.blankBefore.end()) && blankIt->second;
        auto leadIt = trivia_.leadingByTokenIndex.find(startIdx);
        const std::vector<TriviaComment>* cs = nullptr;
        if (leadIt != trivia_.leadingByTokenIndex.end()) cs = &leadIt->second;

        // "item 前空行" 的判定：若有 leading 注释，看首条注释自身是否处于
        // 空行之后；否则看语句锚点的 blankBefore。这样 src 的"空行 + 注释 +
        // 语句"就不会被吃掉
        bool emitBlank = (cs != nullptr && !cs->empty())
                         ? cs->front().blankBefore
                         : stmtBlank;
        inner.push_back(hardline());
        if (!first && emitBlank) inner.push_back(hardline());
        first = false;

        if (cs != nullptr) {
            for (std::size_t k = 0; k < cs->size(); ++k) {
                if (k > 0 && (*cs)[k].blankBefore) inner.push_back(hardline());
                inner.push_back(text((*cs)[k].text));
                inner.push_back(hardline());
            }
            auto bal = trivia_.blankAfterLeading.find(startIdx);
            if (bal != trivia_.blankAfterLeading.end() && bal->second) {
                inner.push_back(hardline());
            }
        }
        inner.push_back(statementDoc(s, indentLevel + 1));

        // 行尾注释：仅单行语句需手动接回（多行语句的 raw 回退里已包含）。
        // trivia 把行尾注释按"该行最后一个 default token 的 tokenIndex"挂在
        // trailingByTokenIndex；扫描该语句 token 范围即可。
        if (s->start->getLine() == s->stop->getLine()) {
            std::size_t a = s->start->getTokenIndex();
            std::size_t b = s->stop->getTokenIndex();
            for (std::size_t idx = a; idx <= b; ++idx) {
                auto it = trivia_.trailingByTokenIndex.find(idx);
                if (it == trivia_.trailingByTokenIndex.end()) continue;
                for (const auto& c : it->second) {
                    inner.push_back(text("  "));
                    inner.push_back(text(c.text));
                }
            }
        }
    }
    // 块尾部"独立挂在 `}` 之前"的注释 / 空行：trivia map 把它们锚在
    // BlockEnd token 上。空块或最后一条语句之后还有注释时也走这里。
    auto* endTok = ctx->BlockEnd();
    if (endTok != nullptr) {
        std::size_t endIdx = endTok->getSymbol()->getTokenIndex();
        auto leadIt = trivia_.leadingByTokenIndex.find(endIdx);
        if (leadIt != trivia_.leadingByTokenIndex.end()) {
            const auto& cs = leadIt->second;
            for (std::size_t k = 0; k < cs.size(); ++k) {
                inner.push_back(hardline());
                if (cs[k].blankBefore) inner.push_back(hardline());
                inner.push_back(text(cs[k].text));
            }
        }
        auto blankIt = trivia_.blankBefore.find(endIdx);
        if (blankIt != trivia_.blankBefore.end() && blankIt->second
            && leadIt == trivia_.leadingByTokenIndex.end()) {
            // 末尾仅有空行，无注释
            inner.push_back(hardline());
        }
    }

    std::vector<Doc> parts;
    parts.push_back(text("{"));
    if (!inner.empty()) {
        parts.push_back(indent(2, concat(std::move(inner))));
    }
    parts.push_back(hardline()); // 在 indent 之外，闭合 `}` 走外层缩进
    parts.push_back(text("}"));
    return concat(std::move(parts));
}

Doc Printer::fnDoc(yuxParser::FnContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(fnHeaderDoc(ctx->fnHeader()));
    if (ctx->fnBody() != nullptr) {
        parts.push_back(text(" "));
        parts.push_back(fnBodyDoc(ctx->fnBody(), 0));
    }
    return concat(std::move(parts));
}

Doc Printer::externDelcDoc(yuxParser::ExternDelcContext* ctx) {
    std::vector<Doc> parts;
    for (auto* anno : ctx->buildAnnos) {
        parts.push_back(buildAnnoDoc(anno));
        parts.push_back(hardline());
    }
    parts.push_back(text("extern {"));
    // 把内部所有 fn 头放进 Indent(2,...)；fnHeader 内部的 hardline (buildAnno
    // 后) 会自动获得这个缩进
    std::vector<Doc> inner;
    for (auto* h : ctx->fnHeader()) {
        inner.push_back(hardline());
        inner.push_back(fnHeaderDoc(h));
    }
    if (!inner.empty()) {
        parts.push_back(indent(2, concat(std::move(inner))));
    }
    parts.push_back(hardline());
    parts.push_back(text("}"));
    return concat(std::move(parts));
}

Printer::Item Printer::visitTopLevel(antlr4::tree::ParseTree* child) {
    Item item;
    auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(child);
    if (ctx == nullptr) {
        return item; // 非规则节点 (LineEnd terminal) 由调用方过滤
    }
    std::size_t startIdx = startTokenIndex(ctx);
    auto blankIt = trivia_.blankBefore.find(startIdx);
    item.blankBefore = (blankIt != trivia_.blankBefore.end()) && blankIt->second;
    auto leadIt = trivia_.leadingByTokenIndex.find(startIdx);
    if (leadIt != trivia_.leadingByTokenIndex.end()) {
        item.leading = leadIt->second;
    }
    auto bal = trivia_.blankAfterLeading.find(startIdx);
    item.blankAfterLeading = (bal != trivia_.blankAfterLeading.end()) && bal->second;

    if (auto* imp = dynamic_cast<yuxParser::ImportsContext*>(ctx)) {
        item.doc = importsDoc(imp);
    } else if (auto* gc = dynamic_cast<yuxParser::GlobalConstContext*>(ctx)) {
        item.doc = globalConstDoc(gc);
    } else if (auto* al = dynamic_cast<yuxParser::AliasDeclContext*>(ctx)) {
        item.doc = aliasDeclDoc(al);
    } else if (auto* fn = dynamic_cast<yuxParser::FnContext*>(ctx)) {
        item.doc = fnDoc(fn);
    } else {
        // 未实装：enumDecl / structDecl / structImpl / draftDecl
        // 直接落原文。trim 末尾 LineEnd 让顶层间换行由 program 控制
        item.raw = trimRightLineEnds(rawSpan(tokens_, ctx));
    }
    // 顶层 item 单行 trailing 注释：与 statementBlock 同样从 trailingByTokenIndex
    // 扫描；多行 item 的 trailing 已含在 raw 内
    if (item.doc && ctx->start->getLine() == ctx->stop->getLine()) {
        std::size_t a = ctx->start->getTokenIndex();
        std::size_t b = ctx->stop->getTokenIndex();
        std::vector<Doc> withTrailing;
        withTrailing.push_back(item.doc);
        bool any = false;
        for (std::size_t idx = a; idx <= b; ++idx) {
            auto it = trivia_.trailingByTokenIndex.find(idx);
            if (it == trivia_.trailingByTokenIndex.end()) continue;
            for (const auto& c : it->second) {
                withTrailing.push_back(text("  "));
                withTrailing.push_back(text(c.text));
                any = true;
            }
        }
        if (any) item.doc = concat(std::move(withTrailing));
    }
    return item;
}

Doc Printer::programDoc(yuxParser::ProgramContext* ctx) {
    // program 由 imports* (fn|externDelc|globalConst|aliasDecl|enumDecl|
    // draftDecl|structDecl|structImpl|LineEnd)* 组成。
    // 我们只取 ParserRuleContext 子节点（即跳过裸 LineEnd token）。
    std::vector<Item> items;
    for (auto* ch : ctx->children) {
        if (auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(ch)) {
            items.push_back(visitTopLevel(rule));
        }
    }

    std::string out;
    RenderOptions ropt;
    bool first = true;
    for (auto& it : items) {
        // 同 statementBlockDoc：若 item 带 leading 注释，则用首条注释的
        // blankBefore 来判定项前是否需要空行（避免源 "空行 + 注释 + item"
        // 形态被错误压平）
        bool emitBlank = !it.leading.empty()
                         ? it.leading.front().blankBefore
                         : it.blankBefore;
        if (!first) {
            out.push_back('\n');
            if (emitBlank) out.push_back('\n');
        }
        first = false;
        for (std::size_t k = 0; k < it.leading.size(); ++k) {
            const auto& c = it.leading[k];
            // 注释自身的 blankBefore：保留它与上一注释 / item 起点之间的空行；
            // 第一个注释的 blankBefore 已由 item.blankBefore 在外层吃掉
            if (k > 0 && c.blankBefore) out.push_back('\n');
            out += c.text;
            if (out.empty() || out.back() != '\n') out.push_back('\n');
        }
        if (!it.leading.empty() && it.blankAfterLeading) {
            out.push_back('\n');
        }
        if (it.doc) {
            out += render(it.doc, ropt);
        } else {
            out += it.raw;
        }
    }
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    return text(out); // 包一层 Text，外层 render 直出
}

} // namespace

std::string formatAst(const std::string& source, const FormatConfig& config) {
    antlr4::ANTLRInputStream input(source);
    yuxLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    yuxParser parser(&tokens);
    parser.removeErrorListeners();
    auto* tree = parser.program();
    TriviaMap trivia = buildTrivia(tokens);

    Printer printer(tokens, trivia);
    Doc d = printer.programDoc(tree);

    RenderOptions ropt;
    ropt.lineWidth = config.lineWidth;
    ropt.indentSize = config.indentSize;
    std::string out = render(d, ropt);

    // 清除每行末尾的空白：渲染期 hardline 会先吐出"\n + 缩进空格"，紧接着
    // 又来一个 hardline 时这些空格就成了空行的尾部空白。统一在此清掉。
    std::string trimmed;
    trimmed.reserve(out.size());
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i <= out.size(); ++i) {
        if (i == out.size() || out[i] == '\n') {
            std::size_t end = i;
            while (end > lineStart && (out[end - 1] == ' ' || out[end - 1] == '\t')) --end;
            trimmed.append(out, lineStart, end - lineStart);
            if (i < out.size()) trimmed.push_back('\n');
            lineStart = i + 1;
        }
    }
    return trimmed;
}

} // namespace yux::format

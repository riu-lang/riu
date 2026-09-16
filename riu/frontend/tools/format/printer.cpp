// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// AST 驱动格式化器实现
//
// Phase 4a 范围（增量, 表达式简单形态走 Doc, 块形态仍 raw）:
// - 简单内联表达式: literal / paren / unary / binary（含 shift/compare/eq/bool/
//   nullElse）/ call(无尾随 lambda) / dot / tupleMember / get / getRef /
//   array / tuple / enumCtor / arrayInit / this / lambdaParen（表达式体）
// - 仍走 raw: lambda 语句体 / tryCatch / match / ifElse /
//   oneLineIfElse / call 含尾随 lambda
// - 已接入 fnExprkBody（`fn name() T = <expr>` 体）
//
// Phase 3 范围:
// - 类型节点全套：type 的 Normal / Generic / Nullable / Array
//   / Tuple 各变体，统一处理后置 `&` 借用标记
// - genericDef / genericDefWithRef / fnParams / fnParam 全套
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

#include <any>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "antlr4-runtime.h"
#include "ast/ast_builder.h"
#include "ast/node/ast_visitor.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/type_node.h"
#include "ast/riu.h"
#include "misc/Interval.h"
#include "riu/riuLexer.h"
#include "riu/riuParser.h"

#include "tools/format/doc.h"
#include "tools/format/render.h"
#include "tools/format/trivia.h"
#include "tools/formatter.h"
#include "types.h"

namespace riu::format {

namespace {

// ==================== 工具：原文截取 ====================

// 抓取一棵子树覆盖的源码原文。BufferedTokenStream::getText(Interval) 不过滤
// 通道，因此返回的字符串与输入完全一致（含其中的 hidden 注释 / 空白）。
std::string rawSpan(antlr4::CommonTokenStream& tokens, antlr4::ParserRuleContext* ctx) {
    if (ctx == nullptr || ctx->start == nullptr || ctx->stop == nullptr) return {};
    return tokens.getText(antlr4::misc::Interval(ctx->start->getTokenIndex(), ctx->stop->getTokenIndex()));
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

// 4.4：表达式 / 语句经 AstVisitor::accept 分派；顶层 item / 类型节点仍走 parse tree
// （类型在 builder 里会解糖成 Nullable / Ref，parse tree 才能保住 `T?` / `T&` 写法）。
class Printer : public AstVisitor {
public:
    Printer(antlr4::CommonTokenStream& tokens, const TriviaMap& trivia, ASTBuilder& builder)
        : tokens_(tokens), trivia_(trivia), builder_(builder) {}

    Doc programDoc(riuParser::ProgramContext* ctx);

    void visitCall(ExprCallNode&) override;
    void visitLiteral(ExprLiteralNode&) override;
    void visitAddSub(ExprAddSubNode&) override;
    void visitMulDivMod(ExprMulDivModNode&) override;
    void visitBinOp(ExprBinOpNode&) override;
    void visitParen(ExprParenNode&) override;
    void visitDot(ExprDotNode&) override;
    void visitCompare(ExprCompareNode&) override;
    void visitIfElse(ExprIfElseNode&) override;
    void visitOneLineIfElse(ExprOneLineIfElseNode&) override;
    void visitGet(ExprGetNode&) override;
    void visitArray(ExprArrayNode&) override;
    void visitArrayInit(ExprArrayInitNode&) override;
    void visitGetRef(ExprGetRefNode&) override;
    void visitUnary(ExprUnaryNode&) override;
    void visitLambda(LambdaExprNode&) override;
    void visitTuple(ExprTupleNode&) override;
    void visitPathCall(ExprPathCallNode&) override;
    void visitStructLit(ExprStructLitNode&) override;
    void visitMatch(ExprMatchNode&) override;
    void visitTryCatch(ExprTryCatchNode&) override;
    void visitDynCtor(ExprDynCtorNode&) override;
    void visitMoveAssign(ExprMoveAssignNode&) override;
    void visitNullElse(ExprNullElseNode&) override;

    void visitBlock(StatementBlockNode&) override;
    void visitExprStmt(StatementExprNode&) override;
    void visitRet(StatementRetNode&) override;
    void visitRetVoid(StatementRetVoidNode&) override;
    void visitDeclare(StatementDeclareNode&) override;
    void visitDeclareAssign(StatementDeclareAssignNode&) override;
    void visitDeclareAssignTuple(StatementDeclareAssignTupleNode&) override;
    void visitAssign(StatementAssignNode&) override;
    void visitLoop(StatementLoopNode&) override;
    void visitBreak(StatementBreakNode&) override;
    void visitContinue(StatementContinueNode&) override;
    void visitForIn(StatementForInNode&) override;
    void visitStaticFieldSet(StatementStaticFieldSetNode&) override;
    void visitSet(StatementSetNode&) override;

private:
    antlr4::CommonTokenStream& tokens_;
    const TriviaMap& trivia_;
    ASTBuilder& builder_;
    Doc _doc;
    int _stmtIndent = 0;

    // 顶层 item 的渲染单元：要么是结构化 Doc (走 render 出现刷格式)，
    // 要么是纯原文块 (按行直出，避免再次 break/flat 决策影响)
    struct Item {
        std::vector<TriviaComment> leading; // 项前 leading 注释
        bool blankBefore = false;           // 是否需要在前面插入一个空行
        bool blankAfterLeading = false;     // leading 与项之间是否需要空行
        Doc doc;                            // 结构化输出 (与 raw 二选一)
        std::string raw;                    // 原文回退输出 (与 doc 二选一)
    };

    Item visitTopLevel(antlr4::tree::ParseTree* child);

    Doc importsDoc(riuParser::ImportsContext* ctx);
    Doc buildAnnoDoc(riuParser::BuildAnnoContext* ctx);
    Doc letGlobalDoc(riuParser::LetGlobalContext* ctx);
    Doc aliasDeclDoc(riuParser::AliasDeclContext* ctx);
    Doc fnDoc(riuParser::FnContext* ctx);
    Doc fnHeaderDoc(riuParser::FnHeaderContext* ctx);
    Doc fnBodyDoc(riuParser::FnBodyContext* ctx, int indentLevel);
    Doc externDelcDoc(riuParser::ExternDelcContext* ctx);

    // 类型 / 泛型 / 形参
    Doc typePathDoc(riuParser::TypePathContext* ctx);
    Doc typeDoc(riuParser::TypeContext* ctx);
    Doc genericDefDoc(riuParser::GenericDefContext* ctx);
    Doc genericDefWithRefDoc(riuParser::GenericDefWithRefContext* ctx);
    Doc fnParamsDoc(riuParser::FnParamsContext* ctx);
    Doc fnParamDoc(riuParser::FnParamContext* ctx);

    // 表达式
    Doc exprDoc(riuParser::ExprContext* ctx);

    // 语句 / 块
    // indentLevel: 当前语句所处的"逻辑缩进层" (顶层 fn body 内是 1，
    // 嵌套 statementBlock 内逐层 +1)，用于多行 raw 回退时计算目标列
    Doc statementBlockDoc(riuParser::StatementBlockContext* ctx, int indentLevel);
    Doc statementDoc(riuParser::StatementContext* ctx, int indentLevel);

    // 把多行原文按"原起始列 → 目标列"做整体平移：第 0 行原样，第 i>0 行
    // 把至多 srcStartCol 个前导空格替换为 targetCol 空格
    static std::string reindentMultilineRaw(const std::string& raw, std::size_t srcStartCol, std::size_t targetCol);

    // 抓 ctx 的原文，但去掉末尾的 LineEnd token（语句规则末尾固定挂 LineEnd）
    std::string rawSpanWithoutTrailingLineEnd(antlr4::ParserRuleContext* ctx);

    // 取顶层 item 起始 default token 的 index，用于查 trivia
    static std::size_t startTokenIndex(antlr4::ParserRuleContext* ctx) { return ctx->start->getTokenIndex(); }

    Doc formatExpr(ExprNode* n);
    Doc formatStmt(StatementNode* n);
    Doc rawNode(const Node& n);
    Doc typeDocAst(TypeNode* t);
    Doc typeListDoc(const std::vector<TypeNode*>& types);
    Doc turbofishDoc(const std::vector<TypeNode*>& types);
    Doc letAnnosDoc(bool isMut, bool isConst, bool isFrozen);
    Doc lambdaParamsFromSlots(const std::vector<LambdaParamSlot>& slots);
    Doc binExpr(ExprNode* left, const char* op, ExprNode* right);
    Doc astBlockDoc(StatementBlockNode* block, int indentLevel);
    Doc statementDocParse(riuParser::StatementContext* ctx, int indentLevel);
};

Doc Printer::importsDoc(riuParser::ImportsContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("use "));
    parts.push_back(typePathDoc(ctx->typePath()));
    if (ctx->useAll != nullptr) {
        parts.push_back(text(".*"));
    }
    return concat(std::move(parts));
}

Doc Printer::buildAnnoDoc(riuParser::BuildAnnoContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("#"));
    parts.push_back(text(ctx->name->getText()));
    if (ctx->ParStart() != nullptr) {
        parts.push_back(text("("));
        if (auto* aa = ctx->annoArg()) {
            if (aa->arg) {
                parts.push_back(text(aa->arg->getText()));
                if (auto* gd = aa->genericDef()) parts.push_back(genericDefDoc(gd));
            } else if (aa->argNum) {
                parts.push_back(text(aa->argNum->getText()));
            } else if (aa->argStr) {
                parts.push_back(text(aa->argStr->getText()));
            } else if (aa->argTPL) {
                parts.push_back(text("\""));
                for (auto* tn : aa->argText)
                    parts.push_back(text(tn->getText()));
                parts.push_back(text("\""));
            } else if (aa->argType) {
                parts.push_back(typeDoc(aa->argType));
            }
        }
        parts.push_back(text(")"));
    }
    return concat(std::move(parts));
}

// letGlobal: letAnno+ Let name=ID type? (= literal)? — 当前仅合法形态 `#Cval let NAME T = literal`
Doc Printer::letGlobalDoc(riuParser::LetGlobalContext* ctx) {
    std::vector<Doc> parts;
    for (auto* a : ctx->letAnnos) {
        parts.push_back(text("#" + a->name->getText()));
        parts.push_back(hardline());
    }
    parts.push_back(text("let "));
    parts.push_back(text(ctx->name->getText()));
    if (ctx->type() != nullptr) {
        parts.push_back(text(" "));
        parts.push_back(typeDoc(ctx->type()));
    }
    if (ctx->expr() != nullptr) {
        parts.push_back(text(" = "));
        parts.push_back(text(rawSpan(tokens_, ctx->expr())));
    }
    return concat(std::move(parts));
}

Doc Printer::aliasDeclDoc(riuParser::AliasDeclContext* ctx) {
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

Doc Printer::typePathDoc(riuParser::TypePathContext* ctx) {
    if (!ctx) return text("");
    std::vector<Doc> parts;
    const auto& segs = ctx->segs;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i > 0) parts.push_back(text("."));
        parts.push_back(text(segs[i]->getText()));
    }
    return concat(std::move(parts));
}

Doc Printer::typeDoc(riuParser::TypeContext* ctx) {
    auto refSuffix = [](antlr4::tree::TerminalNode* andTok) -> Doc { return andTok != nullptr ? text("&") : text(""); };
    if (auto* n = dynamic_cast<riuParser::TypeNormalContext*>(ctx)) {
        return concat({typePathDoc(n->typePath()), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<riuParser::TypeSelfContext*>(ctx)) {
        return concat({text(n->SelfType()->getText()), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<riuParser::TypeNullableContext*>(ctx)) {
        return concat({typeDoc(n->type()), text("?"), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<riuParser::TypeFallibleContext*>(ctx)) {
        return concat({typeDoc(n->base), text(" ! "), typeDoc(n->errType), refSuffix(n->SymbolAnd())});
    }
    if (auto* n = dynamic_cast<riuParser::TypeGenericContext*>(ctx)) {
        return concat({
            typePathDoc(n->typePath()),
            genericDefWithRefDoc(n->genericDefWithRef()),
            refSuffix(n->SymbolAnd()),
        });
    }
    if (auto* n = dynamic_cast<riuParser::TypeArrayContext*>(ctx)) {
        return concat({
            text("["),
            typeDoc(n->type()),
            text(" * "),
            text(n->INT()->getText()),
            text("]"),
            refSuffix(n->SymbolAnd()),
        });
    }
    if (auto* n = dynamic_cast<riuParser::TypeUnitContext*>(ctx)) {
        (void)n;
        return text("()");
    }
    if (auto* n = dynamic_cast<riuParser::TypeTupleContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(text("("));
        for (std::size_t i = 0; i < n->types.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(typeDoc(n->types[i]));
        }
        parts.push_back(text(")"));
        return concat(std::move(parts));
    }
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::genericDefDoc(riuParser::GenericDefContext* ctx) {
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

Doc Printer::genericDefWithRefDoc(riuParser::GenericDefWithRefContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(text("<"));
    for (std::size_t i = 0; i < ctx->types.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(typeDoc(ctx->types[i]));
    }
    parts.push_back(text(">"));
    return concat(std::move(parts));
}

Doc Printer::fnParamsDoc(riuParser::FnParamsContext* ctx) {
    std::vector<Doc> parts;
    auto params = ctx->fnParam();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(fnParamDoc(params[i]));
    }
    return concat(std::move(parts));
}

Doc Printer::fnParamDoc(riuParser::FnParamContext* ctx) {
    if (ctx->fnParamStd() != nullptr) {
        auto* n = ctx->fnParamStd();
        return concat({text(n->ID()->getText()), text(" "), typeDoc(n->type())});
    }
    if (ctx->fnParamGroup() != nullptr) {
        auto* n = ctx->fnParamGroup();
        std::vector<Doc> parts;
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        parts.push_back(text(" "));
        parts.push_back(typeDoc(n->type()));
        return concat(std::move(parts));
    }
    return text(rawSpan(tokens_, ctx));
}

// ==================== 表达式 ====================

// 表达式：parse ctx → ASTBuilder.visit → accept 分派
Doc Printer::exprDoc(riuParser::ExprContext* ctx) {
    if (!ctx) return text("");
    try {
        auto* n = any_cast_p<ExprNode>(builder_.visit(ctx));
        if (n) return formatExpr(n);
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) — builder 未绑定名抛 E3030，回退 raw
    }
    return text(rawSpan(tokens_, ctx));
}

Doc Printer::formatExpr(ExprNode* n) {
    if (!n) return text("");
    struct Restore {
        Doc& slot;
        Doc prev;
        ~Restore() { slot = std::move(prev); }
    } restore{.slot = _doc, .prev = _doc};
    _doc = text("");
    n->accept(*this);
    return _doc;
}

Doc Printer::formatStmt(StatementNode* n) {
    if (!n) return text("");
    struct Restore {
        Doc& slot;
        Doc prev;
        ~Restore() { slot = std::move(prev); }
    } restore{.slot = _doc, .prev = _doc};
    _doc = text("");
    n->accept(*this);
    return _doc;
}

Doc Printer::rawNode(const Node& n) {
    if (n.tokenStart() >= 0 && n.tokenStop() >= n.tokenStart()) {
        return text(tokens_.getText(
            antlr4::misc::Interval(static_cast<std::size_t>(n.tokenStart()), static_cast<std::size_t>(n.tokenStop()))));
    }
    return text("");
}

Doc Printer::binExpr(ExprNode* left, const char* op, ExprNode* right) {
    return concat({formatExpr(left), text(" "), text(op), text(" "), formatExpr(right)});
}

Doc Printer::typeListDoc(const std::vector<TypeNode*>& types) {
    std::vector<Doc> parts;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(typeDocAst(types[i]));
    }
    return concat(std::move(parts));
}

Doc Printer::turbofishDoc(const std::vector<TypeNode*>& types) {
    return concat({text(":<"), typeListDoc(types), text(">")});
}

Doc Printer::letAnnosDoc(bool isMut, bool isConst, bool isFrozen) {
    std::vector<Doc> parts;
    if (isFrozen) parts.push_back(text("#Frozen "));
    if (isConst) parts.push_back(text("#Cval "));
    if (isMut) parts.push_back(text("#Mut "));
    parts.push_back(text("let "));
    return concat(std::move(parts));
}

Doc Printer::typeDocAst(TypeNode* t) {
    if (!t) return text("");
    if (auto* g = dynamic_cast<TypeGenericNode*>(t)) {
        if (g->path().isBare() && g->typeArgs().size() == 1) {
            const std::string n = g->path().lastName();
            if (n == "Ref") return concat({typeDocAst(g->typeArgs()[0]), text("&")});
            if (n == "Nullable") return concat({typeDocAst(g->typeArgs()[0]), text("?")});
        }
        return concat({text(g->path().dotted()), text("<"), typeListDoc(g->typeArgs()), text(">")});
    }
    if (auto* n = dynamic_cast<TypeNormalNode*>(t)) return text(n->path().dotted());
    if (auto* n = dynamic_cast<TypeSelfNode*>(t)) return text(n->selfToken().getText());
    if (auto* n = dynamic_cast<TypeFallibleNode*>(t)) {
        return concat({typeDocAst(n->baseType()), text(" ! "), typeDocAst(n->errType())});
    }
    if (auto* n = dynamic_cast<TypeArrayNode*>(t)) {
        return concat({text("["), typeDocAst(n->elementType()), text(" * "), text(n->count().getText()), text("]")});
    }
    if (auto* n = dynamic_cast<TypeTupleNode*>(t)) {
        if (n->elementTypes().empty()) return text("()");
        return concat({text("("), typeListDoc(n->elementTypes()), text(")")});
    }
    if (auto* fn = dynamic_cast<TypeFnNode*>(t)) {
        std::vector<Doc> parts;
        parts.push_back(text("Function<"));
        auto params = fn->paramTypes();
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(typeDocAst(params[i]));
        }
        if (!params.empty()) parts.push_back(text(", "));
        if (fn->retType())
            parts.push_back(typeDocAst(fn->retType()));
        else
            parts.push_back(text("()"));
        parts.push_back(text(">"));
        if (fn->nullable()) parts.push_back(text("?"));
        return concat(std::move(parts));
    }
    return rawNode(*t);
}

Doc Printer::lambdaParamsFromSlots(const std::vector<LambdaParamSlot>& slots) {
    std::vector<Doc> parts;
    for (std::size_t i = 0; i < slots.size();) {
        if (i > 0) parts.push_back(text(", "));
        TypeNode* ty = slots[i].type;
        std::size_t j = i + 1;
        if (ty) {
            while (j < slots.size() && slots[j].type == ty)
                ++j;
        }
        if (j - i > 1) {
            for (std::size_t k = i; k < j; ++k) {
                if (k > i) parts.push_back(text(", "));
                parts.push_back(text(slots[k].name.getText()));
            }
            parts.push_back(text(" "));
            parts.push_back(typeDocAst(ty));
        } else {
            parts.push_back(text(slots[i].name.getText()));
            if (ty) {
                parts.push_back(text(" "));
                parts.push_back(typeDocAst(ty));
            }
        }
        i = j;
    }
    return concat(std::move(parts));
}

void Printer::visitLiteral(ExprLiteralNode& node) {
    _doc = rawNode(node);
}
void Printer::visitParen(ExprParenNode& node) {
    _doc = concat({text("("), formatExpr(node.expr()), text(")")});
}
void Printer::visitUnary(ExprUnaryNode& node) {
    const char* op = "-";
    switch (node.op()) {
    case ExprUnaryNode::Op::Neg:
        op = "-";
        break;
    case ExprUnaryNode::Op::Not:
        op = "!";
        break;
    case ExprUnaryNode::Op::Rev:
        op = "~";
        break;
    }
    _doc = concat({text(op), formatExpr(node.right())});
}
void Printer::visitAddSub(ExprAddSubNode& node) {
    _doc = binExpr(node.left(), node.op() == ExprAddSubNode::Op::Add ? "+" : "-", node.right());
}
void Printer::visitMulDivMod(ExprMulDivModNode& node) {
    const char* op = "*";
    switch (node.op()) {
    case ExprMulDivModNode::Op::Mul:
        op = "*";
        break;
    case ExprMulDivModNode::Op::Div:
        op = "/";
        break;
    case ExprMulDivModNode::Op::Mod:
        op = "%";
        break;
    }
    _doc = binExpr(node.left(), op, node.right());
}
void Printer::visitBinOp(ExprBinOpNode& node) {
    const char* op = "&";
    switch (node.op()) {
    case ExprBinOpNode::Op::And:
        op = "&";
        break;
    case ExprBinOpNode::Op::Or:
        op = "|";
        break;
    case ExprBinOpNode::Op::Xor:
        op = "^";
        break;
    case ExprBinOpNode::Op::Shl:
        op = "<<";
        break;
    case ExprBinOpNode::Op::Shr:
        op = ">>";
        break;
    }
    _doc = binExpr(node.left(), op, node.right());
}
void Printer::visitCompare(ExprCompareNode& node) {
    const char* op = "==";
    switch (node.op()) {
    case ExprCompareNode::Op::Eq:
        op = "==";
        break;
    case ExprCompareNode::Op::Ne:
        op = "!=";
        break;
    case ExprCompareNode::Op::Lt:
        op = "<";
        break;
    case ExprCompareNode::Op::Le:
        op = "<=";
        break;
    case ExprCompareNode::Op::Gt:
        op = ">";
        break;
    case ExprCompareNode::Op::Ge:
        op = ">=";
        break;
    case ExprCompareNode::Op::AndAnd:
        op = "&&";
        break;
    case ExprCompareNode::Op::OrOr:
        op = "||";
        break;
    }
    _doc = binExpr(node.left(), op, node.right());
}
void Printer::visitNullElse(ExprNullElseNode& node) {
    _doc = binExpr(node.left(), "??", node.right());
}
void Printer::visitMoveAssign(ExprMoveAssignNode& node) {
    _doc = binExpr(node.left(), "<-", node.right());
}
void Printer::visitDot(ExprDotNode& node) {
    std::vector<Doc> parts;
    parts.push_back(formatExpr(node.baseExpr()));
    const std::string m = node.member();
    if (!m.empty() && m[0] == '.') {
        parts.push_back(text(m));
    } else {
        parts.push_back(text(node.isSafe() ? "?." : "."));
        parts.push_back(text(m));
    }
    _doc = concat(std::move(parts));
}
void Printer::visitGet(ExprGetNode& node) {
    std::vector<Doc> parts;
    parts.push_back(formatExpr(node.arrayExpr()));
    parts.push_back(text("["));
    const auto& idx = node.indices();
    for (std::size_t i = 0; i < idx.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(formatExpr(idx[i]));
    }
    parts.push_back(text("]"));
    _doc = concat(std::move(parts));
}
void Printer::visitGetRef(ExprGetRefNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text("&"));
    parts.push_back(text(node.obj().getText()));
    for (const auto& sub : node.subs()) {
        parts.push_back(text("."));
        parts.push_back(text(sub.getText()));
    }
    _doc = concat(std::move(parts));
}
void Printer::visitCall(ExprCallNode& node) {
    if (node.hasTrailingLambda()) {
        _doc = rawNode(node);
        return;
    }
    std::vector<Doc> parts;
    parts.push_back(formatExpr(node.getCalleeExpr()));
    if (!node.getTypeArgs().empty()) parts.push_back(turbofishDoc(node.getTypeArgs()));
    parts.push_back(text("("));
    const auto& args = node.getArgs();
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(formatExpr(args[i]));
    }
    parts.push_back(text(")"));
    if (node.errPropagate()) parts.push_back(text("!"));
    _doc = concat(std::move(parts));
}
void Printer::visitArray(ExprArrayNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text("["));
    const auto& els = node.elements();
    for (std::size_t i = 0; i < els.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(formatExpr(els[i]));
    }
    parts.push_back(text("]"));
    _doc = concat(std::move(parts));
}
void Printer::visitTuple(ExprTupleNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text("("));
    const auto& els = node.elements();
    for (std::size_t i = 0; i < els.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(formatExpr(els[i]));
    }
    parts.push_back(text(")"));
    _doc = concat(std::move(parts));
}
void Printer::visitArrayInit(ExprArrayInitNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text("["));
    parts.push_back(rawNode(*node.value()));
    parts.push_back(text(" ..."));
    if (node.explicitType()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(node.explicitType()));
    }
    parts.push_back(text("]"));
    _doc = concat(std::move(parts));
}
void Printer::visitPathCall(ExprPathCallNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text(node.lhsPath().dotted()));
    if (!node.lhsTypeArgs().empty()) parts.push_back(turbofishDoc(node.lhsTypeArgs()));
    parts.push_back(text("::"));
    parts.push_back(text(node.variantName().getText()));
    if (!node.rhsTypeArgs().empty()) parts.push_back(turbofishDoc(node.rhsTypeArgs()));
    if (node.hasParens() || !node.args().empty()) {
        parts.push_back(text("("));
        const auto& args = node.args();
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(formatExpr(args[i]));
        }
        parts.push_back(text(")"));
    }
    if (node.errPropagate()) parts.push_back(text("!"));
    _doc = concat(std::move(parts));
}
void Printer::visitLambda(LambdaExprNode& node) {
    if (node.form() == LambdaExprNode::Form::Block) {
        _doc = rawNode(node);
        return;
    }
    std::vector<Doc> parts;
    parts.push_back(text("("));
    parts.push_back(lambdaParamsFromSlots(node.params()));
    parts.push_back(text(")"));
    if (node.retType()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(node.retType()));
    }
    parts.push_back(text(" => "));
    parts.push_back(formatExpr(node.bodyExpr()));
    _doc = concat(std::move(parts));
}
void Printer::visitDynCtor(ExprDynCtorNode& node) {
    std::vector<TypeNode*> args{node.specType()};
    _doc = concat({text("Dyn"), turbofishDoc(args), text("("), formatExpr(node.arg()), text(")")});
}
void Printer::visitIfElse(ExprIfElseNode& node) {
    _doc = rawNode(node);
}
void Printer::visitOneLineIfElse(ExprOneLineIfElseNode& node) {
    _doc = rawNode(node);
}
void Printer::visitMatch(ExprMatchNode& node) {
    _doc = rawNode(node);
}
void Printer::visitTryCatch(ExprTryCatchNode& node) {
    _doc = rawNode(node);
}
void Printer::visitStructLit(ExprStructLitNode& node) {
    _doc = rawNode(node);
}

void Printer::visitBlock(StatementBlockNode& node) {
    _doc = astBlockDoc(&node, _stmtIndent);
}
void Printer::visitExprStmt(StatementExprNode& node) {
    std::vector<Doc> parts;
    parts.push_back(formatExpr(node.expr()));
    if (node.hasSemicolon()) parts.push_back(text(";"));
    _doc = concat(std::move(parts));
}
void Printer::visitRet(StatementRetNode& node) {
    _doc = concat({text("ret "), formatExpr(node.expr())});
}
void Printer::visitRetVoid(StatementRetVoidNode&) {
    _doc = text("ret;");
}
void Printer::visitDeclare(StatementDeclareNode& node) {
    std::vector<Doc> parts;
    parts.push_back(letAnnosDoc(node.isMut(), node.isConst(), node.isFrozen()));
    parts.push_back(text(node.name().getText()));
    if (node.varType()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(node.varType()));
    }
    _doc = concat(std::move(parts));
}
void Printer::visitDeclareAssign(StatementDeclareAssignNode& node) {
    std::vector<Doc> parts;
    parts.push_back(letAnnosDoc(node.isMut(), node.isConst(), node.isFrozen()));
    parts.push_back(text(node.name().getText()));
    if (node.varType()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(node.varType()));
    }
    parts.push_back(text(" = "));
    parts.push_back(formatExpr(node.expr()));
    _doc = concat(std::move(parts));
}
void Printer::visitDeclareAssignTuple(StatementDeclareAssignTupleNode& node) {
    std::vector<Doc> parts;
    parts.push_back(letAnnosDoc(node.isMut(), node.isConst(), node.isFrozen()));
    parts.push_back(text("("));
    const auto& names = node.names();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(text(names[i].getText()));
    }
    parts.push_back(text(")"));
    if (node.varType()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(node.varType()));
    }
    parts.push_back(text(" = "));
    parts.push_back(formatExpr(node.expr()));
    _doc = concat(std::move(parts));
}
void Printer::visitAssign(StatementAssignNode& node) {
    std::vector<Doc> parts;
    parts.push_back(text(node.obj().getText()));
    for (const auto& sub : node.subs()) {
        parts.push_back(text("."));
        parts.push_back(text(sub.getText()));
    }
    const char* op = "=";
    switch (node.op()) {
    case AssignOp::Eq:
        op = "=";
        break;
    case AssignOp::AddEq:
        op = "+=";
        break;
    case AssignOp::SubEq:
        op = "-=";
        break;
    case AssignOp::MulEq:
        op = "*=";
        break;
    case AssignOp::DivEq:
        op = "/=";
        break;
    case AssignOp::ModEq:
        op = "%=";
        break;
    }
    parts.push_back(text(" "));
    parts.push_back(text(op));
    parts.push_back(text(" "));
    parts.push_back(formatExpr(node.expr()));
    _doc = concat(std::move(parts));
}
void Printer::visitSet(StatementSetNode& node) {
    std::vector<Doc> parts;
    parts.push_back(formatExpr(node.arrayExpr()));
    parts.push_back(text("["));
    const auto& idx = node.indices();
    for (std::size_t i = 0; i < idx.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(formatExpr(idx[i]));
    }
    parts.push_back(text("] = "));
    parts.push_back(formatExpr(node.valueExpr()));
    _doc = concat(std::move(parts));
}
void Printer::visitLoop(StatementLoopNode& node) {
    std::vector<Doc> parts;
    if (!node.label().getText().empty()) {
        parts.push_back(text(node.label().getText()));
        parts.push_back(text(": "));
    }
    parts.push_back(text("loop "));
    if (node.hasInit()) {
        const auto& names = node.initNames();
        if (names.size() == 1) {
            parts.push_back(text(names[0].getText()));
        } else {
            parts.push_back(text("("));
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i > 0) parts.push_back(text(", "));
                parts.push_back(text(names[i].getText()));
            }
            parts.push_back(text(")"));
        }
        if (node.initType()) {
            parts.push_back(text(" "));
            parts.push_back(typeDocAst(node.initType()));
        }
        parts.push_back(text(" = "));
        parts.push_back(formatExpr(node.initExpr()));
        parts.push_back(text(" "));
    }
    parts.push_back(astBlockDoc(node.block(), _stmtIndent));
    _doc = concat(std::move(parts));
}
void Printer::visitForIn(StatementForInNode& node) {
    std::vector<Doc> parts;
    if (!node.label().getText().empty()) {
        parts.push_back(text(node.label().getText()));
        parts.push_back(text(": "));
    }
    parts.push_back(text("for "));
    parts.push_back(text(node.item().getText()));
    parts.push_back(text(" in "));
    parts.push_back(formatExpr(node.expr()));
    parts.push_back(text(" "));
    parts.push_back(astBlockDoc(node.block(), _stmtIndent));
    _doc = concat(std::move(parts));
}
void Printer::visitBreak(StatementBreakNode& node) {
    if (!node.label().getText().empty()) {
        _doc = concat({text("break@"), text(node.label().getText()), text(";")});
        return;
    }
    _doc = text("break;");
}
void Printer::visitContinue(StatementContinueNode& node) {
    if (!node.label().getText().empty()) {
        _doc = concat({text("continue@"), text(node.label().getText()), text(";")});
        return;
    }
    _doc = text("continue;");
}
void Printer::visitStaticFieldSet(StatementStaticFieldSetNode& node) {
    _doc = concat({text(node.typePath().dotted()), text("::"), text(node.fieldName().getText()), text(" = "),
                   formatExpr(node.valueExpr())});
}

Doc Printer::astBlockDoc(StatementBlockNode* block, int indentLevel) {
    if (!block) return text("{}");
    std::vector<Doc> inner;
    for (auto* s : block->statements()) {
        inner.push_back(hardline());
        int prev = _stmtIndent;
        _stmtIndent = indentLevel + 1;
        inner.push_back(formatStmt(s));
        _stmtIndent = prev;
    }
    if (block->hasResult() && block->resultExpr()) {
        inner.push_back(hardline());
        inner.push_back(formatExpr(block->resultExpr()));
    }
    std::vector<Doc> parts;
    parts.push_back(text("{"));
    if (!inner.empty()) parts.push_back(indent(2, concat(std::move(inner))));
    parts.push_back(hardline());
    parts.push_back(text("}"));
    return concat(std::move(parts));
}

// ==================== fn / extern ====================

Doc Printer::fnHeaderDoc(riuParser::FnHeaderContext* ctx) {
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
        parts.push_back(typeDoc(ctx->retType));
    }
    return concat(std::move(parts));
}

Doc Printer::fnBodyDoc(riuParser::FnBodyContext* ctx, int indentLevel) {
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

std::string Printer::reindentMultilineRaw(const std::string& raw, std::size_t srcStartCol, std::size_t targetCol) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : raw) {
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            } else if (c != '\r')
                cur.push_back(c);
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
            while (k < lines[i].size() && k < srcStartCol && lines[i][k] == ' ')
                ++k;
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
    if (ctx->stop->getType() == riuLexer::LineEnd && stopIdx > startIdx) {
        --stopIdx;
    }
    return tokens_.getText(antlr4::misc::Interval(startIdx, stopIdx));
}

Doc Printer::statementDoc(riuParser::StatementContext* ctx, int indentLevel) {
    // 多行语句（典型：含块形 expr）一律走 raw 回退；目标列 = indentLevel*2
    std::size_t startLine = ctx->start->getLine();
    std::size_t stopLine = ctx->stop->getLine();
    if (startLine != stopLine) {
        std::size_t srcCol = ctx->start->getCharPositionInLine();
        std::string raw = rawSpanWithoutTrailingLineEnd(ctx);
        return text(reindentMultilineRaw(raw, srcCol, static_cast<std::size_t>(indentLevel) * 2));
    }

    try {
        auto* n = any_cast_p<StatementNode>(builder_.visit(ctx));
        if (n) {
            int prev = _stmtIndent;
            _stmtIndent = indentLevel;
            Doc d = formatStmt(n);
            _stmtIndent = prev;
            return d;
        }
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) — builder 未绑定名抛 E3030，回退 parse tree
    }
    return statementDocParse(ctx, indentLevel);
}

Doc Printer::statementDocParse(riuParser::StatementContext* ctx, int indentLevel) {
    if (auto* n = dynamic_cast<riuParser::StatementLetContext*>(ctx)) {
        // letAnno* let name (Type)? (= expr)?
        std::vector<Doc> parts;
        parts.reserve(n->letAnnos.size());
        for (auto* a : n->letAnnos) {
            parts.push_back(text("#" + a->name->getText() + " "));
        }
        parts.push_back(text("let "));
        parts.push_back(text(n->name->getText()));
        if (n->type() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeDoc(n->type()));
        }
        if (n->expr() != nullptr) {
            parts.push_back(text(" = "));
            parts.push_back(exprDoc(n->expr()));
        }
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<riuParser::StatementLetTupleContext*>(ctx)) {
        // letAnno* let (a, b) (Type)? = expr
        std::vector<Doc> parts;
        parts.reserve(n->letAnnos.size());
        for (auto* a : n->letAnnos) {
            parts.push_back(text("#" + a->name->getText() + " "));
        }
        parts.push_back(text("let ("));
        for (std::size_t i = 0; i < n->names.size(); ++i) {
            if (i > 0) parts.push_back(text(", "));
            parts.push_back(text(n->names[i]->getText()));
        }
        parts.push_back(text(")"));
        if (n->type() != nullptr) {
            parts.push_back(text(" "));
            parts.push_back(typeDoc(n->type()));
        }
        parts.push_back(text(" = "));
        parts.push_back(exprDoc(n->expr()));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<riuParser::StatementSetContext*>(ctx)) {
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
    if (auto* n = dynamic_cast<riuParser::StatementLoopContext*>(ctx)) {
        std::vector<Doc> parts;
        if (n->ID()) {
            parts.push_back(text(n->ID()->getText()));
            parts.push_back(text(": "));
        }
        parts.push_back(text("loop "));
        if (auto initCtx = n->loopInit(); initCtx) {
            if (initCtx->name) {
                // 单变量：loop i = expr
                parts.push_back(text(initCtx->name->getText()));
                if (auto t = initCtx->type(); t) {
                    parts.push_back(text(" "));
                    parts.push_back(typeDoc(t));
                }
                parts.push_back(text(" = "));
                parts.push_back(exprDoc(initCtx->expr()));
            } else {
                // tuple 解构：loop (i, n) = expr
                parts.push_back(text("("));
                for (size_t i = 0; i < initCtx->names.size(); ++i) {
                    if (i > 0) parts.push_back(text(", "));
                    parts.push_back(text(initCtx->names[i]->getText()));
                }
                parts.push_back(text(")"));
                if (auto t = initCtx->type(); t) {
                    parts.push_back(text(" "));
                    parts.push_back(typeDoc(t));
                }
                parts.push_back(text(" = "));
                parts.push_back(exprDoc(initCtx->expr()));
            }
            parts.push_back(text(" "));
        }
        parts.push_back(statementBlockDoc(n->statementBlock(), indentLevel));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<riuParser::StatementForInContext*>(ctx)) {
        std::vector<Doc> parts;
        auto ids = n->ID();
        if (n->SymbolColon() && ids.size() >= 2) {
            parts.push_back(text(ids[0]->getText()));
            parts.push_back(text(": "));
            parts.push_back(text("for "));
            parts.push_back(text(ids[1]->getText()));
        } else {
            parts.push_back(text("for "));
            parts.push_back(text(ids[0]->getText()));
        }
        parts.push_back(text(" in "));
        parts.push_back(exprDoc(n->expr()));
        parts.push_back(text(" "));
        parts.push_back(statementBlockDoc(n->statementBlock(), indentLevel));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<riuParser::StatementAssignContext*>(ctx)) {
        // obj (.id|DOT_NUM)* opAssign expr
        std::vector<Doc> parts;
        parts.push_back(text(n->obj->getText())); // ID 或 `$`
        for (auto* sub : n->subs) {
            if (sub->getType() == riuLexer::DOT_NUM) {
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
    if (auto* n = dynamic_cast<riuParser::StatementExprContext*>(ctx)) {
        std::vector<Doc> parts;
        parts.push_back(exprDoc(n->expr()));
        if (n->SymbolSemicolon() != nullptr) parts.push_back(text(";"));
        return concat(std::move(parts));
    }
    if (auto* n = dynamic_cast<riuParser::StatementRetContext*>(ctx)) {
        return concat({text("ret "), exprDoc(n->expr())});
    }
    if (dynamic_cast<riuParser::StatementRetVoidContext*>(ctx)) {
        return text("ret;");
    }
    if (auto* n = dynamic_cast<riuParser::StatementBreakContext*>(ctx)) {
        if (n->ID()) {
            return concat({text("break@"), text(n->ID()->getText()), text(";")});
        }
        return text("break;");
    }
    if (auto* n = dynamic_cast<riuParser::StatementContinueContext*>(ctx)) {
        if (n->ID()) {
            return concat({text("continue@"), text(n->ID()->getText()), text(";")});
        }
        return text("continue;");
    }
    // 兜底（语法演化时保护）
    std::size_t srcCol = ctx->start->getCharPositionInLine();
    return text(
        reindentMultilineRaw(rawSpanWithoutTrailingLineEnd(ctx), srcCol, static_cast<std::size_t>(indentLevel) * 2));
}

Doc Printer::statementBlockDoc(riuParser::StatementBlockContext* ctx, int indentLevel) {
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
        bool emitBlank = (cs != nullptr && !cs->empty()) ? cs->front().blankBefore : stmtBlank;
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
            for (const auto& c : cs) {
                inner.push_back(hardline());
                if (c.blankBefore) inner.push_back(hardline());
                inner.push_back(text(c.text));
            }
        }
        auto blankIt = trivia_.blankBefore.find(endIdx);
        if (blankIt != trivia_.blankBefore.end() && blankIt->second && leadIt == trivia_.leadingByTokenIndex.end()) {
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

Doc Printer::fnDoc(riuParser::FnContext* ctx) {
    std::vector<Doc> parts;
    parts.push_back(fnHeaderDoc(ctx->fnHeader()));
    if (ctx->fnBody() != nullptr) {
        parts.push_back(text(" "));
        parts.push_back(fnBodyDoc(ctx->fnBody(), 0));
    }
    return concat(std::move(parts));
}

Doc Printer::externDelcDoc(riuParser::ExternDelcContext* ctx) {
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

    if (auto* imp = dynamic_cast<riuParser::ImportsContext*>(ctx)) {
        item.doc = importsDoc(imp);
    } else if (auto* lg = dynamic_cast<riuParser::LetGlobalContext*>(ctx)) {
        item.doc = letGlobalDoc(lg);
    } else if (auto* al = dynamic_cast<riuParser::AliasDeclContext*>(ctx)) {
        item.doc = aliasDeclDoc(al);
    } else if (auto* fn = dynamic_cast<riuParser::FnContext*>(ctx)) {
        item.doc = fnDoc(fn);
    } else {
        // 未实装：enumDecl / structDecl / structImpl / specDecl
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

Doc Printer::programDoc(riuParser::ProgramContext* ctx) {
    // program 由 imports* (fn|externDelc|globalConst|aliasDecl|enumDecl|
    // specDecl|structDecl|structImpl|LineEnd)* 组成。
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
        bool emitBlank = !it.leading.empty() ? it.leading.front().blankBefore : it.blankBefore;
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
    riuLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    riuParser parser(&tokens);
    parser.removeErrorListeners();
    auto* tree = parser.program();
    TriviaMap trivia = buildTrivia(tokens);

    Riu riu;
    ASTBuilder builder(riu);
    Printer printer(tokens, trivia, builder);
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
            while (end > lineStart && (out[end - 1] == ' ' || out[end - 1] == '\t'))
                --end;
            trimmed.append(out, lineStart, end - lineStart);
            if (i < out.size()) trimmed.push_back('\n');
            lineStart = i + 1;
        }
    }
    return trimmed;
}

} // namespace riu::format

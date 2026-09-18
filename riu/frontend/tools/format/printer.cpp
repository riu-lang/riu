// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// AST 驱动格式化器实现
//
// 词法 trivia 走 rd nextRaw()；顶层 / 类型走 FileNode（`T?` / `T&` 由
// typeDocAst 从 Nullable / Ref 印回）。表达式 / 语句经 AstVisitor。
// 多行语句仍按源切片再整体平移缩进；struct / spec / extern 顶层回退原文。

#include "tools/format/printer.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/node/alias_node.h"
#include "ast/node/ast_visitor.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/global_const_node.h"
#include "ast/node/global_var_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/spec_ref.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"
#include "ast/rd/token.h"
#include "ast/rd_builder.h"
#include "ast/riu.h"

#include "tools/format/doc.h"
#include "tools/format/render.h"
#include "tools/format/trivia.h"
#include "tools/formatter.h"
#include "types.h"

namespace riu::format {

namespace {

std::string trimRightLineEnds(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

class Printer : public AstVisitor {
public:
    Printer(std::string_view source, const TriviaMap& trivia, const std::vector<rd::Token>& defaultToks)
        : source_(source), trivia_(trivia), defaultToks_(defaultToks) {}

    Doc programDoc(FileNode& file);

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
    void visitAlias(AliasDeclNode&) override;

private:
    std::string_view source_;
    const TriviaMap& trivia_;
    const std::vector<rd::Token>& defaultToks_;
    Doc _doc;
    int _stmtIndent = 0;

    struct Item {
        std::vector<TriviaComment> leading;
        bool blankBefore = false;
        bool blankAfterLeading = false;
        Doc doc;
        std::string raw;
        int sortTok = 0;
        int sortLine = 0;
    };

    [[nodiscard]] const rd::Token* tokAt(int idx) const;
    [[nodiscard]] rd::Kind kindAt(const Node& n) const;
    [[nodiscard]] std::string rawSpan(int startIdx, int stopIdx) const;
    [[nodiscard]] std::string rawSpan(const Node& n) const;
    [[nodiscard]] std::string rawSpanWithoutTrailingLineEnd(const Node& n) const;
    static std::string reindentMultilineRaw(const std::string& raw, std::size_t srcStartCol, std::size_t targetCol);

    Item itemFromTok(int tokStart);
    void attachTrailing(Item& item, int startIdx, int stopIdx);
    void attachTrailing(std::vector<Doc>& parts, int startIdx, int stopIdx);

    Doc formatExpr(ExprNode* n);
    Doc formatStmt(StatementNode* n);
    Doc rawNode(const Node& n);
    Doc typeDocAst(TypeNode* t);
    Doc typeListDoc(const std::vector<TypeNode*>& types);
    Doc turbofishDoc(const std::vector<TypeNode*>& types);
    Doc letAnnosDoc(bool isMut, bool isConst, bool isFrozen);
    Doc lambdaParamsFromSlots(const std::vector<LambdaParamSlot>& slots);
    Doc binExpr(ExprNode* left, const char* op, ExprNode* right);
    Doc specRefDoc(const SpecRef& r);
    Doc genericDefDoc(const std::vector<std::string>& names, const std::vector<std::vector<SpecRef>>& bounds);
    Doc annoDoc(const std::string& name, const std::string& arg);
    Doc headerAnnosDoc(const FnHeaderNode& h);
    Doc fnHeaderDoc(FnHeaderNode* h);
    Doc fnParamsDoc(FnHeaderNode* h);
    Doc fnDoc(FnNode* fn);
    Doc fnBodyDoc(FnNode* fn, int indentLevel);
    Doc enumDeclDoc(EnumDeclNode* en);
    Doc aliasDeclDoc(AliasDeclNode* al);
    Doc letGlobalDoc(GlobalConstNode* g);
    Doc letGlobalVarDoc(GlobalVarNode* g);
    Doc useDoc(const FileNode::UseSpec& u);
    Doc astBlockDoc(StatementBlockNode* block, int indentLevel);
    Doc blockDoc(const std::vector<StatementNode*>& stmts, ExprNode* result, int blockEndTok, int indentLevel);
    Doc stmtInBlock(StatementNode* s, int indentLevel);
    Doc exprInBlock(ExprNode* e, int indentLevel);
    [[nodiscard]] bool isMultiline(const Node& n) const;
    [[nodiscard]] int useTokIndex(int line) const;
    void collectExternItems(std::vector<Item>& items);
};

const rd::Token* Printer::tokAt(int idx) const {
    if (idx < 0 || static_cast<std::size_t>(idx) >= defaultToks_.size()) return nullptr;
    return &defaultToks_[static_cast<std::size_t>(idx)];
}

rd::Kind Printer::kindAt(const Node& n) const {
    const rd::Token* t = tokAt(n.tokenStart());
    return t ? t->kind : rd::Kind::Invalid;
}

std::string Printer::rawSpan(int startIdx, int stopIdx) const {
    const rd::Token* a = tokAt(startIdx);
    const rd::Token* b = tokAt(stopIdx);
    if (!a || !b) return {};
    const auto off = static_cast<std::size_t>(a->pos.offset);
    const auto end = static_cast<std::size_t>(b->pos.end);
    if (off > source_.size() || end < off) return {};
    auto n = end - off;
    if (off + n > source_.size()) n = source_.size() - off;
    return std::string(source_.substr(off, n));
}

std::string Printer::rawSpan(const Node& n) const {
    if (n.tokenStart() < 0 || n.tokenStop() < n.tokenStart()) return {};
    return rawSpan(n.tokenStart(), n.tokenStop());
}

std::string Printer::rawSpanWithoutTrailingLineEnd(const Node& n) const {
    if (n.tokenStart() < 0 || n.tokenStop() < n.tokenStart()) return {};
    int stop = n.tokenStop();
    const rd::Token* last = tokAt(stop);
    if (last && last->kind == rd::Kind::LineEnd && stop > n.tokenStart()) --stop;
    return rawSpan(n.tokenStart(), stop);
}

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

Printer::Item Printer::itemFromTok(int tokStart) {
    Item item;
    item.sortTok = tokStart;
    if (const rd::Token* t = tokAt(tokStart)) item.sortLine = t->pos.line;
    auto idx = static_cast<std::size_t>(std::max(tokStart, 0));
    auto blankIt = trivia_.blankBefore.find(idx);
    item.blankBefore = (blankIt != trivia_.blankBefore.end()) && blankIt->second;
    auto leadIt = trivia_.leadingByTokenIndex.find(idx);
    if (leadIt != trivia_.leadingByTokenIndex.end()) item.leading = leadIt->second;
    auto bal = trivia_.blankAfterLeading.find(idx);
    item.blankAfterLeading = (bal != trivia_.blankAfterLeading.end()) && bal->second;
    return item;
}

void Printer::attachTrailing(std::vector<Doc>& parts, int startIdx, int stopIdx) {
    if (startIdx < 0 || stopIdx < startIdx) return;
    for (int idx = startIdx; idx <= stopIdx; ++idx) {
        auto it = trivia_.trailingByTokenIndex.find(static_cast<std::size_t>(idx));
        if (it == trivia_.trailingByTokenIndex.end()) continue;
        for (const auto& c : it->second) {
            parts.push_back(text("  "));
            parts.push_back(text(c.text));
        }
    }
}

void Printer::attachTrailing(Item& item, int startIdx, int stopIdx) {
    if (!item.doc) return;
    const rd::Token* a = tokAt(startIdx);
    const rd::Token* b = tokAt(stopIdx);
    if (!a || !b || a->pos.line != b->pos.line) return;
    std::vector<Doc> withTrailing;
    withTrailing.push_back(item.doc);
    const std::size_t before = withTrailing.size();
    attachTrailing(withTrailing, startIdx, stopIdx);
    if (withTrailing.size() > before) item.doc = concat(std::move(withTrailing));
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
    return text(rawSpan(n));
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

Doc Printer::specRefDoc(const SpecRef& r) {
    if (r.typeArgs.empty()) return text(r.name);
    std::vector<Doc> parts;
    parts.push_back(text(r.name));
    parts.push_back(text("<"));
    for (std::size_t i = 0; i < r.typeArgs.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(text(r.typeArgs[i].getFullName()));
    }
    parts.push_back(text(">"));
    return concat(std::move(parts));
}

Doc Printer::genericDefDoc(const std::vector<std::string>& names, const std::vector<std::vector<SpecRef>>& bounds) {
    if (names.empty()) return text("");
    std::vector<Doc> parts;
    parts.push_back(text("<"));
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        parts.push_back(text(names[i]));
        if (i < bounds.size() && !bounds[i].empty()) {
            parts.push_back(text(" : "));
            for (std::size_t b = 0; b < bounds[i].size(); ++b) {
                if (b > 0) parts.push_back(text(" + "));
                parts.push_back(specRefDoc(bounds[i][b]));
            }
        }
    }
    parts.push_back(text(">"));
    return concat(std::move(parts));
}

Doc Printer::annoDoc(const std::string& name, const std::string& arg) {
    if (arg.empty()) return concat({text("#"), text(name)});
    bool ident = !arg.empty() && (std::isalnum(static_cast<unsigned char>(arg[0])) || arg[0] == '_');
    for (char c : arg) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '<' || c == '>' ||
              c == ',')) {
            ident = false;
            break;
        }
    }
    if (ident || arg.front() == '"') return concat({text("#"), text(name), text("("), text(arg), text(")")});
    return concat({text("#"), text(name), text("(\""), text(arg), text("\")")});
}

Doc Printer::headerAnnosDoc(const FnHeaderNode& h) {
    std::vector<Doc> parts;
    const auto& names = h.annos();
    const auto& args = h.annoArgs();
    for (std::size_t i = 0; i < names.size(); ++i) {
        parts.push_back(annoDoc(names[i], i < args.size() ? args[i] : std::string()));
        parts.push_back(hardline());
    }
    return parts.empty() ? text("") : concat(std::move(parts));
}

Doc Printer::fnParamsDoc(FnHeaderNode* h) {
    std::vector<Doc> parts;
    auto params = h->params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) parts.push_back(text(", "));
        if (params[i]->isFrozen()) parts.push_back(text("#Frozen "));
        parts.push_back(text(params[i]->name().getText()));
        if (params[i]->type()) {
            parts.push_back(text(" "));
            parts.push_back(typeDocAst(params[i]->type()));
        }
    }
    return concat(std::move(parts));
}

Doc Printer::fnHeaderDoc(FnHeaderNode* h) {
    if (!h) return text("");
    std::vector<Doc> parts;
    parts.push_back(headerAnnosDoc(*h));
    parts.push_back(text("fn "));
    parts.push_back(text(h->name().getText()));
    parts.push_back(genericDefDoc(h->typeParams(), h->typeParamBounds()));
    parts.push_back(text("("));
    parts.push_back(fnParamsDoc(h));
    parts.push_back(text(")"));
    if (h->retType() || h->fallibleErrTypeNode()) {
        parts.push_back(text(" "));
        if (h->retType())
            parts.push_back(typeDocAst(h->retType()));
        else
            parts.push_back(text("()"));
        if (h->fallibleErrTypeNode()) {
            parts.push_back(text(" ! "));
            parts.push_back(typeDocAst(h->fallibleErrTypeNode()));
        }
    }
    return concat(std::move(parts));
}

bool Printer::isMultiline(const Node& n) const {
    const rd::Token* a = tokAt(n.tokenStart());
    int stop = n.tokenStop();
    const rd::Token* last = tokAt(stop);
    if (last && last->kind == rd::Kind::LineEnd && stop > n.tokenStart()) --stop;
    const rd::Token* b = tokAt(stop);
    return a && b && a->pos.line != b->pos.line;
}

Doc Printer::stmtInBlock(StatementNode* s, int indentLevel) {
    if (!s) return text("");
    if (isMultiline(*s)) {
        const rd::Token* a = tokAt(s->tokenStart());
        std::size_t srcCol = a ? static_cast<std::size_t>(a->pos.column) : 0;
        return text(
            reindentMultilineRaw(rawSpanWithoutTrailingLineEnd(*s), srcCol, static_cast<std::size_t>(indentLevel) * 2));
    }
    int prev = _stmtIndent;
    _stmtIndent = indentLevel;
    Doc d = formatStmt(s);
    _stmtIndent = prev;
    std::vector<Doc> parts;
    parts.push_back(d);
    attachTrailing(parts, s->tokenStart(), s->tokenStop());
    return concat(std::move(parts));
}

Doc Printer::exprInBlock(ExprNode* e, int indentLevel) {
    if (!e) return text("");
    if (isMultiline(*e)) {
        const rd::Token* a = tokAt(e->tokenStart());
        std::size_t srcCol = a ? static_cast<std::size_t>(a->pos.column) : 0;
        return text(
            reindentMultilineRaw(rawSpanWithoutTrailingLineEnd(*e), srcCol, static_cast<std::size_t>(indentLevel) * 2));
    }
    std::vector<Doc> parts;
    parts.push_back(formatExpr(e));
    attachTrailing(parts, e->tokenStart(), e->tokenStop());
    return concat(std::move(parts));
}

Doc Printer::blockDoc(const std::vector<StatementNode*>& stmts, ExprNode* result, int blockEndTok, int indentLevel) {
    std::vector<Doc> inner;
    auto emitLead = [&](int tokStart, bool first) {
        auto idx = static_cast<std::size_t>(std::max(tokStart, 0));
        auto blankIt = trivia_.blankBefore.find(idx);
        bool stmtBlank = (blankIt != trivia_.blankBefore.end()) && blankIt->second;
        auto leadIt = trivia_.leadingByTokenIndex.find(idx);
        const std::vector<TriviaComment>* cs = nullptr;
        if (leadIt != trivia_.leadingByTokenIndex.end()) cs = &leadIt->second;
        bool emitBlank = (cs != nullptr && !cs->empty()) ? cs->front().blankBefore : stmtBlank;
        inner.push_back(hardline());
        if (!first && emitBlank) inner.push_back(hardline());
        if (cs != nullptr) {
            for (std::size_t k = 0; k < cs->size(); ++k) {
                if (k > 0 && (*cs)[k].blankBefore) inner.push_back(hardline());
                inner.push_back(text((*cs)[k].text));
                inner.push_back(hardline());
            }
            auto bal = trivia_.blankAfterLeading.find(idx);
            if (bal != trivia_.blankAfterLeading.end() && bal->second) inner.push_back(hardline());
        }
    };

    bool first = true;
    for (auto* s : stmts) {
        emitLead(s->tokenStart(), first);
        first = false;
        inner.push_back(stmtInBlock(s, indentLevel + 1));
    }
    if (result) {
        emitLead(result->tokenStart(), first);
        first = false;
        inner.push_back(exprInBlock(result, indentLevel + 1));
    }
    if (blockEndTok >= 0) {
        auto endIdx = static_cast<std::size_t>(blockEndTok);
        auto leadIt = trivia_.leadingByTokenIndex.find(endIdx);
        if (leadIt != trivia_.leadingByTokenIndex.end()) {
            for (const auto& c : leadIt->second) {
                inner.push_back(hardline());
                if (c.blankBefore) inner.push_back(hardline());
                inner.push_back(text(c.text));
            }
        }
        auto blankIt = trivia_.blankBefore.find(endIdx);
        if (blankIt != trivia_.blankBefore.end() && blankIt->second && leadIt == trivia_.leadingByTokenIndex.end()) {
            inner.push_back(hardline());
        }
    }

    std::vector<Doc> parts;
    parts.push_back(text("{"));
    if (!inner.empty()) parts.push_back(indent(2, concat(std::move(inner))));
    parts.push_back(hardline());
    parts.push_back(text("}"));
    return concat(std::move(parts));
}

Doc Printer::astBlockDoc(StatementBlockNode* block, int indentLevel) {
    if (!block) return text("{}");
    return blockDoc(block->statements(), block->hasResult() ? block->resultExpr() : nullptr, block->tokenStop(),
                    indentLevel);
}

Doc Printer::fnBodyDoc(FnNode* fn, int indentLevel) {
    const auto& body = fn->body();
    if (body.empty()) return text("");

    if (body.size() == 1) {
        if (auto* ret = dynamic_cast<StatementRetNode*>(body[0])) {
            rd::Kind k = kindAt(*ret);
            if (k != rd::Kind::Ret && k != rd::Kind::BlockStart) {
                return concat({text("= "), formatExpr(ret->expr())});
            }
        }
    }

    std::vector<StatementNode*> stmts;
    ExprNode* result = nullptr;
    stmts.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        auto* ret = dynamic_cast<StatementRetNode*>(body[i]);
        if (i + 1 == body.size() && ret && kindAt(*ret) != rd::Kind::Ret) {
            result = ret->expr();
            break;
        }
        stmts.push_back(body[i]);
    }
    return blockDoc(stmts, result, fn->tokenStop(), indentLevel);
}

Doc Printer::fnDoc(FnNode* fn) {
    std::vector<Doc> parts;
    parts.push_back(fnHeaderDoc(fn->header()));
    if (!fn->body().empty()) {
        parts.push_back(text(" "));
        parts.push_back(fnBodyDoc(fn, 0));
    }
    return concat(std::move(parts));
}

Doc Printer::enumDeclDoc(EnumDeclNode* en) {
    std::vector<Doc> parts;
    parts.push_back(text("enum "));
    parts.push_back(text(en->name().getText()));
    parts.push_back(genericDefDoc(en->typeParams(), en->typeParamBounds()));
    parts.push_back(text(" {"));
    std::vector<Doc> inner;
    for (auto* v : en->variants()) {
        inner.push_back(hardline());
        std::vector<Doc> vp;
        vp.push_back(text(v->name().getText()));
        if (v->hasPayload()) {
            vp.push_back(text("("));
            const auto& ps = v->payloadTypes();
            for (std::size_t i = 0; i < ps.size(); ++i) {
                if (i > 0) vp.push_back(text(", "));
                vp.push_back(typeDocAst(ps[i]));
            }
            vp.push_back(text(")"));
        }
        inner.push_back(concat(std::move(vp)));
    }
    if (!inner.empty()) parts.push_back(indent(2, concat(std::move(inner))));
    parts.push_back(hardline());
    parts.push_back(text("}"));
    return concat(std::move(parts));
}

Doc Printer::aliasDeclDoc(AliasDeclNode* al) {
    return concat({text("type "), text(al->name().getText()), text(" = "), typeDocAst(al->target())});
}

Doc Printer::letGlobalDoc(GlobalConstNode* g) {
    std::vector<Doc> parts;
    if (g->isInline()) {
        parts.push_back(text("#Inline"));
        parts.push_back(hardline());
    }
    parts.push_back(text("#Cval"));
    parts.push_back(hardline());
    parts.push_back(text("let "));
    parts.push_back(text(g->name().getText()));
    if (g->typeNode()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(g->typeNode()));
    }
    if (g->value()) {
        parts.push_back(text(" = "));
        parts.push_back(formatExpr(g->value()));
    }
    return concat(std::move(parts));
}

Doc Printer::letGlobalVarDoc(GlobalVarNode* g) {
    std::vector<Doc> parts;
    if (g->isMutable()) {
        parts.push_back(text("#Mut"));
        parts.push_back(hardline());
    }
    parts.push_back(text("let "));
    parts.push_back(text(g->name().getText()));
    if (g->typeNode()) {
        parts.push_back(text(" "));
        parts.push_back(typeDocAst(g->typeNode()));
    }
    if (g->value()) {
        parts.push_back(text(" = "));
        parts.push_back(formatExpr(g->value()));
    }
    return concat(std::move(parts));
}

Doc Printer::useDoc(const FileNode::UseSpec& u) {
    std::vector<Doc> parts;
    parts.push_back(text("use "));
    parts.push_back(text(u.moduleName));
    if (u.wildcard) parts.push_back(text(".*"));
    return concat(std::move(parts));
}

int Printer::useTokIndex(int line) const {
    for (const auto& t : defaultToks_) {
        if (t.kind == rd::Kind::Use && t.pos.line == line) return t.index;
    }
    return -1;
}

void Printer::collectExternItems(std::vector<Item>& items) {
    int depth = 0;
    for (std::size_t i = 0; i < defaultToks_.size(); ++i) {
        const rd::Kind k = defaultToks_[i].kind;
        if (depth == 0 && k == rd::Kind::Extern) {
            std::size_t start = i;
            for (;;) {
                if (start == 0) break;
                std::size_t j = start - 1;
                while (j > 0 && defaultToks_[j].kind == rd::Kind::LineEnd)
                    --j;
                if (defaultToks_[j].kind == rd::Kind::ParEnd) {
                    int pd = 1;
                    if (j == 0) break;
                    --j;
                    while (pd > 0) {
                        if (defaultToks_[j].kind == rd::Kind::ParEnd)
                            ++pd;
                        else if (defaultToks_[j].kind == rd::Kind::ParStart)
                            --pd;
                        if (pd == 0) break;
                        if (j == 0) break;
                        --j;
                    }
                    if (pd != 0 || j == 0) break;
                    --j;
                }
                if (j >= 1 && defaultToks_[j].kind == rd::Kind::ID &&
                    defaultToks_[j - 1].kind == rd::Kind::SymbolHash) {
                    start = j - 1;
                    continue;
                }
                break;
            }
            int inner = 0;
            std::size_t end = i;
            for (std::size_t k2 = i; k2 < defaultToks_.size(); ++k2) {
                if (defaultToks_[k2].kind == rd::Kind::BlockStart)
                    ++inner;
                else if (defaultToks_[k2].kind == rd::Kind::BlockEnd) {
                    --inner;
                    if (inner == 0) {
                        end = k2;
                        break;
                    }
                }
            }
            Item item = itemFromTok(defaultToks_[start].index);
            item.raw = trimRightLineEnds(rawSpan(defaultToks_[start].index, defaultToks_[end].index));
            items.push_back(std::move(item));
        }
        if (k == rd::Kind::BlockStart)
            ++depth;
        else if (k == rd::Kind::BlockEnd && depth > 0)
            --depth;
    }
}

Doc Printer::programDoc(FileNode& file) {
    std::vector<Item> items;

    auto addNode = [&](Node* n, Doc d) {
        if (!n) return;
        Item item = itemFromTok(n->tokenStart());
        item.sortLine = n->getLineNumber();
        item.doc = std::move(d);
        attachTrailing(item, n->tokenStart(), n->tokenStop());
        items.push_back(std::move(item));
    };
    auto addRaw = [&](Node* n, std::string raw) {
        if (!n) return;
        Item item = itemFromTok(n->tokenStart());
        item.sortLine = n->getLineNumber();
        item.raw = trimRightLineEnds(std::move(raw));
        items.push_back(std::move(item));
    };

    for (const auto& u : file.useSpecs()) {
        int tok = useTokIndex(u.line);
        Item item = itemFromTok(tok);
        item.sortLine = u.line;
        item.doc = useDoc(u);
        if (tok >= 0) attachTrailing(item, tok, tok);
        items.push_back(std::move(item));
    }
    for (auto* g : file.getGlobalConsts())
        addNode(g, letGlobalDoc(g));
    for (auto* g : file.getGlobalVars())
        addNode(g, letGlobalVarDoc(g));
    for (auto* a : file.getAliasDecls())
        addNode(a, aliasDeclDoc(a));
    for (auto* e : file.getEnumDecls())
        addNode(e, enumDeclDoc(e));
    for (auto* fn : file.getFunctions())
        addNode(fn, fnDoc(fn));
    for (auto* s : file.getStructDecls())
        addRaw(s, s->sourceText().empty() ? rawSpan(*s) : s->sourceText());
    for (auto* s : file.getSpecDecls())
        addRaw(s, s->sourceText().empty() ? rawSpan(*s) : s->sourceText());
    collectExternItems(items);

    std::ranges::stable_sort(items, [](const Item& a, const Item& b) {
        if (a.sortLine != b.sortLine) return a.sortLine < b.sortLine;
        return a.sortTok < b.sortTok;
    });

    std::string out;
    RenderOptions ropt;
    bool first = true;
    for (auto& it : items) {
        bool emitBlank = !it.leading.empty() ? it.leading.front().blankBefore : it.blankBefore;
        if (!first) {
            out.push_back('\n');
            if (emitBlank) out.push_back('\n');
        }
        first = false;
        for (std::size_t k = 0; k < it.leading.size(); ++k) {
            const auto& c = it.leading[k];
            if (k > 0 && c.blankBefore) out.push_back('\n');
            out += c.text;
            if (out.empty() || out.back() != '\n') out.push_back('\n');
        }
        if (!it.leading.empty() && it.blankAfterLeading) out.push_back('\n');
        if (it.doc) {
            out += render(it.doc, ropt);
        } else {
            out += it.raw;
        }
    }
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    return text(out);
}

void Printer::visitLiteral(ExprLiteralNode& node) {
    std::string s = rawSpan(node);
    if (s.empty() && node.literal()) s = node.literal()->getValue().getText();
    _doc = text(std::move(s));
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
        const std::string& t = sub.getText();
        if (!t.empty() && t[0] == '.') {
            parts.push_back(text(t));
        } else {
            parts.push_back(text("."));
            parts.push_back(text(t));
        }
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
void Printer::visitAlias(AliasDeclNode& node) {
    _doc = aliasDeclDoc(&node);
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

} // namespace

std::string formatAst(const std::string& source, const FormatConfig& config, const std::string& filePath) {
    const bool isTestFile = filePath.ends_with(".test.ut");
    Riu riu;
    auto builder = std::make_unique<RdBuilder>(riu, source, /*moduleName=*/"", isTestFile, filePath);
    builder->setExpandImports(false);
    FileNode* file = builder->build();

    TriviaScan scan = scanTrivia(source);
    Printer printer(source, scan.map, scan.defaultToks);
    Doc d = printer.programDoc(*file);

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

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/parser.h"

#include <string>
#include <utility>

namespace rd {
namespace {

constexpr int kBpLowest = 0;
constexpr int kBpMove = 1;     // <-
constexpr int kBpNullElse = 2; // ??
constexpr int kBpBool = 3;     // && ||
constexpr int kBpEq = 4;       // == !=
constexpr int kBpCmp = 5;      // < > <= >=
constexpr int kBpAdd = 6;
constexpr int kBpMul = 7;
constexpr int kBpUnary = 8;
constexpr int kBpPostfix = 9;

constexpr int kTyFallible = 2;
constexpr int kTyNullable = 1;

bool isAssignOp(Kind k) {
    switch (k) {
    case Kind::SymbolEq:
    case Kind::SymbolAddEq:
    case Kind::SymbolSubEq:
    case Kind::SymbolMulEq:
    case Kind::SymbolDivEq:
    case Kind::SymbolModEq:
        return true;
    default:
        return false;
    }
}

void appendIf(std::vector<NodeId>& v, NodeId id) {
    if (id != kEmptyNode) v.push_back(id);
}

} // namespace

Parser::Parser(std::string_view src) : scanner_(src, &errors_) {}

void Parser::next() {
    if (tok_.kind != Kind::Invalid && tok_.kind != Kind::Eof && !tok_.text.empty() && tok_.text != "<EOF>") {
        prev_text_ = std::string(tok_.text);
    }
    if (!peeked_.empty()) {
        tok_ = peeked_.front();
        peeked_.erase(peeked_.begin());
        return;
    }
    tok_ = scanner_.next();
}

const Token& Parser::la(int n) {
    while (static_cast<int>(peeked_.size()) < n)
        peeked_.push_back(scanner_.next());
    return peeked_[static_cast<std::size_t>(n - 1)];
}

bool Parser::peekIs(Kind k, int n) {
    return la(n).kind == k;
}

bool Parser::eat(Kind k) {
    if (!at(k)) return false;
    next();
    return true;
}

void Parser::skipLineEnds() {
    while (eat(Kind::LineEnd)) {
    }
}

void Parser::skipToLineEnd() {
    while (!at(Kind::LineEnd) && !at(Kind::Eof))
        next();
    eat(Kind::LineEnd);
}

void Parser::errorSyntax(std::string message) {
    ParseError e;
    e.is_lexer = false;
    e.pos = tok_.pos;
    e.message = std::move(message);
    e.offending = std::string(tok_.text);
    e.prev_text = prev_text_;
    errors_.push_back(std::move(e));
}

void Parser::errorExpected(Kind k) {
    std::string msg = "mismatched input '";
    msg += tok_.text;
    msg += "' expecting ";
    msg += kindName(k);
    errorSyntax(std::move(msg));
    skipToLineEnd();
}

void Parser::errorUnknown() {
    std::string msg = "no viable alternative at input '";
    msg += tok_.text;
    msg += "'";
    errorSyntax(std::move(msg));
    skipToLineEnd();
}

Parser::Mark Parser::mark() const {
    return Mark{tok_, peeked_, scanner_.snapshot()};
}

void Parser::rewind(const Mark& m) {
    tok_ = m.tok;
    peeked_ = m.peeked;
    scanner_.restore(m.scan);
}

Pos Parser::spanTok(const Token& first, const Token& last) const {
    Pos p = first.pos;
    p.end = last.pos.end;
    return p;
}

Pos Parser::spanPos(const Pos& start, const Pos& last) const {
    Pos p = start;
    p.end = last.end;
    return p;
}

std::string_view Parser::sliceTokens(const Token& first, const Token& last) const {
    if (first.text.empty()) return last.text;
    if (last.text.empty()) return first.text;
    const auto* begin = first.text.data();
    const auto* end = last.text.data() + last.text.size();
    if (end < begin) return first.text;
    return {begin, static_cast<std::size_t>(end - begin)};
}

// ==== 类型 ====

bool Parser::looksLikeType() const {
    switch (tok_.kind) {
    case Kind::ID:
    case Kind::SelfType:
    case Kind::GetStart:
    case Kind::ParStart:
        return true;
    default:
        return false;
    }
}

Kind Parser::eatTrailingAnd() {
    return eat(Kind::SymbolAnd) ? Kind::SymbolAnd : Kind::Invalid;
}

NodeId Parser::parseTypePath() {
    if (!at(Kind::ID)) return kEmptyNode;
    const Token first = tok_;
    Token last = tok_;
    next();
    while (at(Kind::SymbolDot) && peekIs(Kind::ID)) {
        next();
        last = tok_;
        next();
    }
    return ast_.add(NodeKind::TypePath, spanTok(first, last), sliceTokens(first, last));
}

NodeId Parser::parseGenericArgs() {
    const Pos start = tok_.pos;
    if (!eat(Kind::SymbolLt)) return kEmptyNode;
    std::vector<NodeId> args;
    if (!at(Kind::SymbolMt)) {
        args.push_back(parseType());
        while (eat(Kind::SymbolComma))
            args.push_back(parseType());
    }
    const Pos end = tok_.pos;
    eat(Kind::SymbolMt);
    return ast_.add(NodeKind::Generic, spanPos(start, end), {}, args);
}

NodeId Parser::parseGenericDef() {
    const Pos start = tok_.pos;
    if (!eat(Kind::SymbolLt)) return kEmptyNode;
    std::vector<NodeId> params;
    auto parseParam = [this, &params]() {
        NodeId t = parseType();
        if (!eat(Kind::SymbolColon)) {
            appendIf(params, t);
            return;
        }
        std::vector<NodeId> kids;
        appendIf(kids, t);
        kids.push_back(parseType());
        while (eat(Kind::SymbolAdd))
            kids.push_back(parseType());
        params.push_back(ast_.add(NodeKind::Generic, ast_.at(t).pos, {}, kids));
    };
    if (!at(Kind::SymbolMt)) {
        parseParam();
        while (eat(Kind::SymbolComma))
            parseParam();
    }
    const Pos end = tok_.pos;
    eat(Kind::SymbolMt);
    return ast_.add(NodeKind::Generic, spanPos(start, end), {}, params);
}

NodeId Parser::parseTypePrimary() {
    if (at(Kind::SelfType)) {
        const Pos p = tok_.pos;
        next();
        return ast_.add(NodeKind::TypeSelf, p, {}, {}, eatTrailingAnd());
    }
    if (at(Kind::GetStart)) {
        const Pos start = tok_.pos;
        next();
        NodeId elem = parseType();
        eat(Kind::SymbolMul);
        NodeId n = kEmptyNode;
        if (at(Kind::INT)) {
            n = ast_.add(NodeKind::IntLit, tok_.pos, tok_.text);
            next();
        }
        const Pos end = tok_.pos;
        eat(Kind::GetEnd);
        std::vector<NodeId> kids;
        appendIf(kids, elem);
        appendIf(kids, n);
        return ast_.add(NodeKind::TypeArray, spanPos(start, end), {}, kids, eatTrailingAnd());
    }
    if (at(Kind::ParStart)) {
        const Pos start = tok_.pos;
        if (peekIs(Kind::ParEnd)) {
            next();
            const Pos end = tok_.pos;
            next();
            return ast_.add(NodeKind::TypeUnit, spanPos(start, end));
        }
        next();
        std::vector<NodeId> ts;
        ts.push_back(parseType());
        while (eat(Kind::SymbolComma))
            ts.push_back(parseType());
        const Pos end = tok_.pos;
        eat(Kind::ParEnd);
        if (ts.size() <= 1) return ts.empty() ? ast_.add(NodeKind::TypeUnit, start) : ts[0];
        return ast_.add(NodeKind::TypeTuple, spanPos(start, end), {}, ts);
    }
    if (!at(Kind::ID)) return kEmptyNode;
    NodeId path = parseTypePath();
    if (at(Kind::SymbolLt)) {
        NodeId gen = parseGenericArgs();
        std::vector<NodeId> kids;
        if (gen != kEmptyNode) {
            const Node& g = ast_.at(gen);
            for (i32 i = 0; i < g.children_count; ++i)
                appendIf(kids, ast_.child(gen, i));
        }
        const Pos p = ast_.at(path).pos;
        return ast_.add(NodeKind::TypeGeneric, p, ast_.at(path).value, kids, eatTrailingAnd());
    }
    ast_.setOp(path, eatTrailingAnd());
    return path;
}

NodeId Parser::parseType(int min_prec) {
    NodeId t = parseTypePrimary();
    if (t == kEmptyNode) return t;
    for (;;) {
        if (at(Kind::SymbolExcl) && kTyFallible >= min_prec) {
            const Pos start = ast_.at(t).pos;
            next();
            NodeId err = parseType(kTyFallible + 1);
            std::vector<NodeId> kids;
            kids.push_back(t);
            appendIf(kids, err);
            t = ast_.add(NodeKind::TypeFallible, spanPos(start, tok_.pos), {}, kids, eatTrailingAnd());
            continue;
        }
        if (at(Kind::SymbolQuest) && kTyNullable >= min_prec) {
            const Pos start = ast_.at(t).pos;
            next();
            t = ast_.add(NodeKind::TypeNullable, spanPos(start, tok_.pos), {}, {t}, eatTrailingAnd());
            continue;
        }
        break;
    }
    return t;
}

// ==== 预读 ====

bool Parser::aheadIsLambda() {
    if (!at(Kind::ParStart)) return false;
    int depth = 1;
    int i = 1;
    while (depth > 0) {
        const Kind k = la(i).kind;
        if (k == Kind::Eof) return false;
        if (k == Kind::ParStart)
            ++depth;
        else if (k == Kind::ParEnd)
            --depth;
        ++i;
    }
    for (;;) {
        const Kind k = la(i).kind;
        if (k == Kind::SymbolEqMt) return true;
        if (k == Kind::LineEnd) {
            ++i;
            continue;
        }
        switch (k) {
        case Kind::ID:
        case Kind::SelfType:
        case Kind::SymbolDot:
        case Kind::SymbolLt:
        case Kind::SymbolMt:
        case Kind::SymbolComma:
        case Kind::SymbolQuest:
        case Kind::SymbolExcl:
        case Kind::SymbolAnd:
        case Kind::GetStart:
        case Kind::GetEnd:
        case Kind::INT:
        case Kind::ParStart:
        case Kind::ParEnd:
            ++i;
            continue;
        default:
            return false;
        }
    }
}

bool Parser::aheadIsStructLit() {
    int i = 0;
    auto kindAt = [this](int n) { return n <= 0 ? tok_.kind : la(n).kind; };
    if (kindAt(0) == Kind::SelfType) {
        i = 1;
    } else if (kindAt(0) == Kind::ID) {
        i = 1;
        while (kindAt(i) == Kind::SymbolDot && kindAt(i + 1) == Kind::ID)
            i += 2;
    } else {
        return false;
    }
    if (kindAt(i) != Kind::BlockStart) return false;
    return kindAt(i + 1) != Kind::ParStart;
}

bool Parser::aheadIsEnumCtor() {
    auto kindAt = [this](int n) { return n <= 0 ? tok_.kind : la(n).kind; };
    int i = 0;
    if (kindAt(0) == Kind::SelfType) {
        i = 1;
    } else if (kindAt(0) == Kind::ID) {
        i = 1;
        while (kindAt(i) == Kind::SymbolDot && kindAt(i + 1) == Kind::ID)
            i += 2;
    } else {
        return false;
    }
    if (kindAt(i) == Kind::SymbolColon && kindAt(i + 1) == Kind::SymbolLt) {
        int depth = 1;
        i += 2;
        while (depth > 0) {
            const Kind k = kindAt(i);
            if (k == Kind::Eof) return false;
            if (k == Kind::SymbolLt)
                ++depth;
            else if (k == Kind::SymbolMt)
                --depth;
            ++i;
        }
    }
    return kindAt(i) == Kind::SymbolColonColon;
}

bool Parser::aheadIsTrailingLambda() {
    if (!at(Kind::BlockStart)) return false;
    int i = 1;
    while (la(i).kind == Kind::LineEnd)
        ++i;
    return la(i).kind == Kind::ParStart;
}

// ==== 表达式 ====

NodeId Parser::parseLiteral() {
    const Pos p = tok_.pos;
    const std::string_view text = tok_.text;
    switch (tok_.kind) {
    case Kind::INT:
        next();
        return ast_.add(NodeKind::IntLit, p, text);
    case Kind::FLOAT:
        next();
        return ast_.add(NodeKind::FloatLit, p, text);
    case Kind::True:
    case Kind::False:
        next();
        return ast_.add(NodeKind::BoolLit, p, text);
    case Kind::Null:
        next();
        return ast_.add(NodeKind::NullLit, p, text);
    case Kind::CODE_POINT:
        next();
        return ast_.add(NodeKind::CodePoint, p, text);
    case Kind::STR_LINE_RAW:
        next();
        return ast_.add(NodeKind::StringLit, p, text);
    case Kind::DOT_NUM:
        next();
        return ast_.add(NodeKind::FloatLit, p, text);
    default:
        return kEmptyNode;
    }
}

NodeId Parser::parseStringTpl() {
    const Pos start = tok_.pos;
    next(); // OPEN
    std::vector<NodeId> parts;
    bool interp = false;
    while (!at(Kind::STR_TPL_CLOSE) && !at(Kind::Eof)) {
        if (at(Kind::STR_TPL_TEXT)) {
            parts.push_back(ast_.add(NodeKind::TplText, tok_.pos, tok_.text));
            next();
            continue;
        }
        if (at(Kind::STR_TPL_DOLLAR_ID)) {
            interp = true;
            parts.push_back(ast_.add(NodeKind::TplDollar, tok_.pos, tok_.text));
            next();
            continue;
        }
        if (at(Kind::STR_TPL_INTERP_OPEN)) {
            interp = true;
            next();
            NodeId e = parseExpr();
            eat(Kind::BlockEnd);
            std::vector<NodeId> kids;
            appendIf(kids, e);
            parts.push_back(ast_.add(NodeKind::TplInterp, tok_.pos, {}, kids));
            continue;
        }
        next();
    }
    const Pos end = tok_.pos;
    eat(Kind::STR_TPL_CLOSE);
    if (!interp && parts.size() <= 1) {
        std::string_view v;
        if (parts.size() == 1) v = ast_.at(parts[0]).value;
        return ast_.add(NodeKind::StringLit, spanPos(start, end), v);
    }
    return ast_.add(NodeKind::StringInterp, spanPos(start, end), {}, parts);
}

void Parser::parseArgList(std::vector<NodeId>& args, Kind closer) {
    next(); // 开括号
    skipLineEnds();
    if (eat(closer)) return;
    args.push_back(parseExpr());
    for (;;) {
        skipLineEnds();
        if (!eat(Kind::SymbolComma)) break;
        skipLineEnds();
        if (at(closer)) break;
        args.push_back(parseExpr());
    }
    skipLineEnds();
    eat(closer);
}

NodeId Parser::parseTrailingLambda() {
    const Pos start = tok_.pos;
    if (!eat(Kind::BlockStart)) return kEmptyNode;
    skipLineEnds();
    std::vector<NodeId> kids;
    if (eat(Kind::ParStart)) {
        skipLineEnds();
        if (!at(Kind::ParEnd)) parseLambdaParams(kids);
        eat(Kind::ParEnd);
    }
    appendIf(kids, parseOptionalFnRet());
    eat(Kind::SymbolEqMt);
    skipLineEnds();
    std::vector<NodeId> stmts;
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        NodeId s = parseStatement();
        if (s != kEmptyNode)
            stmts.push_back(s);
        else if (!at(Kind::BlockEnd) && !at(Kind::Eof))
            errorUnknown();
    }
    const Pos end = tok_.pos;
    eat(Kind::BlockEnd);
    NodeId body = ast_.add(NodeKind::Block, spanPos(start, end), {}, stmts);
    kids.push_back(body);
    return ast_.add(NodeKind::Lambda, spanPos(start, end), {}, kids);
}

NodeId Parser::parseCall(NodeId left) {
    std::vector<NodeId> kids;
    kids.push_back(left);
    parseArgList(kids, Kind::ParEnd);
    if (at(Kind::BlockStart)) appendIf(kids, parseTrailingLambda());
    NodeId call = ast_.add(NodeKind::Call, ast_.at(left).pos, {}, kids);
    if (eat(Kind::SymbolExcl)) ast_.setOp(call, Kind::SymbolExcl);
    return call;
}

NodeId Parser::parseGet(NodeId left) {
    std::vector<NodeId> kids;
    kids.push_back(left);
    parseArgList(kids, Kind::GetEnd);
    return ast_.add(NodeKind::Get, ast_.at(left).pos, {}, kids);
}

NodeId Parser::parseEnumCtor(NodeId lhs, bool lhs_is_self) {
    (void)lhs_is_self;
    std::vector<NodeId> kids;
    kids.push_back(lhs);
    if (at(Kind::SymbolColon) && peekIs(Kind::SymbolLt)) {
        next();
        appendIf(kids, parseGenericArgs());
    }
    eat(Kind::SymbolColonColon);
    std::string_view name;
    Pos pos = tok_.pos;
    if (at(Kind::ID)) {
        name = tok_.text;
        pos = tok_.pos;
        next();
    }
    if (at(Kind::SymbolColon) && peekIs(Kind::SymbolLt)) {
        next();
        appendIf(kids, parseGenericArgs());
    }
    if (at(Kind::ParStart)) parseArgList(kids, Kind::ParEnd);
    NodeId n = ast_.add(NodeKind::EnumCtor, pos, name, kids);
    if (eat(Kind::SymbolExcl)) ast_.setOp(n, Kind::SymbolExcl);
    return n;
}

NodeId Parser::parseStructLit() {
    const Pos start = tok_.pos;
    NodeId lhs = kEmptyNode;
    if (at(Kind::SelfType)) {
        lhs = ast_.add(NodeKind::TypeSelf, tok_.pos);
        next();
    } else {
        lhs = parseTypePath();
    }
    std::vector<NodeId> kids;
    appendIf(kids, lhs);
    eat(Kind::BlockStart);
    if (at(Kind::LineEnd) || peekIs(Kind::LineEnd)) {
        skipLineEnds();
        while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
            if (eat(Kind::LineEnd)) continue;
            if (!at(Kind::SymbolDot)) break;
            next();
            std::string_view fname;
            if (at(Kind::ID)) {
                fname = tok_.text;
                next();
            }
            eat(Kind::SymbolEq);
            NodeId v = parseExpr();
            eat(Kind::LineEnd);
            kids.push_back(ast_.add(NodeKind::FieldInit, tok_.pos, fname, std::vector<NodeId>{v}));
        }
    } else if (!at(Kind::BlockEnd)) {
        appendIf(kids, parseExpr());
    }
    eat(Kind::BlockEnd);
    return ast_.add(NodeKind::StructLit, start, {}, kids);
}

NodeId Parser::parseArrayOrInit() {
    const Pos start = tok_.pos;
    const Mark m = mark();
    next(); // [
    skipLineEnds();
    if (at(Kind::INT) || at(Kind::FLOAT) || at(Kind::True) || at(Kind::False) || at(Kind::Null) ||
        at(Kind::STR_LINE_RAW) || at(Kind::STR_TPL_OPEN) || at(Kind::CODE_POINT) || at(Kind::ID)) {
        NodeId lit = at(Kind::STR_TPL_OPEN) ? parseStringTpl() : (at(Kind::ID) ? kEmptyNode : parseLiteral());
        if (lit == kEmptyNode && at(Kind::ID)) {
            lit = ast_.add(NodeKind::Ident, tok_.pos, tok_.text);
            next();
        }
        if (at(Kind::SymbolDot) && peekIs(Kind::SymbolDot) && la(2).kind == Kind::SymbolDot) {
            next();
            next();
            next();
            NodeId ty = looksLikeType() ? parseType() : kEmptyNode;
            eat(Kind::GetEnd);
            std::vector<NodeId> kids;
            appendIf(kids, lit);
            appendIf(kids, ty);
            return ast_.add(NodeKind::ArrayInit, start, {}, kids);
        }
    }
    rewind(m);
    std::vector<NodeId> elems;
    parseArgList(elems, Kind::GetEnd);
    return ast_.add(NodeKind::Array, start, {}, elems);
}

NodeId Parser::parseParenLambdaOrTuple() {
    const Pos start = tok_.pos;
    if (peekIs(Kind::ParEnd) && la(2).kind != Kind::SymbolEqMt) {
        next();
        next();
        return ast_.add(NodeKind::UnitLit, start);
    }
    if (aheadIsLambda()) {
        next(); // (
        skipLineEnds();
        std::vector<NodeId> kids;
        if (!at(Kind::ParEnd)) parseLambdaParams(kids);
        eat(Kind::ParEnd);
        appendIf(kids, parseOptionalFnRet());
        eat(Kind::SymbolEqMt);
        skipLineEnds();
        if (at(Kind::BlockStart))
            appendIf(kids, parseBlock());
        else
            appendIf(kids, parseExpr());
        return ast_.add(NodeKind::Lambda, start, {}, kids);
    }
    next(); // (
    NodeId first = parseExpr();
    if (at(Kind::SymbolComma)) {
        std::vector<NodeId> vals;
        appendIf(vals, first);
        while (eat(Kind::SymbolComma))
            vals.push_back(parseExpr());
        eat(Kind::ParEnd);
        return ast_.add(NodeKind::Tuple, start, {}, vals);
    }
    eat(Kind::ParEnd);
    return ast_.add(NodeKind::Paren, start, {}, std::vector<NodeId>{first});
}

NodeId Parser::parseIf() {
    const Pos start = tok_.pos;
    next(); // if
    NodeId cond = parseExpr(kBpLowest, false);
    const Mark m = mark();
    if (at(Kind::BlockStart) && !peekIs(Kind::LineEnd)) {
        next();
        NodeId t = parseExpr();
        if (eat(Kind::BlockEnd) && eat(Kind::Else) && eat(Kind::BlockStart)) {
            NodeId f = parseExpr();
            eat(Kind::BlockEnd);
            return ast_.add(NodeKind::IfLine, start, {}, std::vector<NodeId>{cond, t, f});
        }
        rewind(m);
    }
    std::vector<NodeId> kids;
    appendIf(kids, cond);
    appendIf(kids, parseBlock());
    while (at(Kind::Elif)) {
        const Pos ep = tok_.pos;
        next();
        NodeId econd = parseExpr(kBpLowest, false);
        NodeId ebody = parseBlock();
        kids.push_back(ast_.add(NodeKind::Elif, ep, {}, std::vector<NodeId>{econd, ebody}));
    }
    if (eat(Kind::Else)) appendIf(kids, parseBlock());
    return ast_.add(NodeKind::If, start, {}, kids);
}

NodeId Parser::parseMatch() {
    const Pos start = tok_.pos;
    next();
    NodeId scrut = parseExpr(kBpLowest, false);
    eat(Kind::BlockStart);
    skipLineEnds();
    std::vector<NodeId> kids;
    appendIf(kids, scrut);
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        NodeId pat = kEmptyNode;
        if (at(Kind::Else)) {
            pat = ast_.add(NodeKind::Pattern, tok_.pos, tok_.text);
            next();
        } else {
            NodeId path = parseTypePath();
            eat(Kind::SymbolColonColon);
            std::string_view vname;
            if (at(Kind::ID)) {
                vname = tok_.text;
                next();
            }
            std::vector<NodeId> pkids;
            appendIf(pkids, path);
            if (eat(Kind::ParStart)) {
                if (at(Kind::ID)) {
                    pkids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                    next();
                    while (eat(Kind::SymbolComma) && at(Kind::ID)) {
                        pkids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                        next();
                    }
                }
                eat(Kind::ParEnd);
            }
            pat = ast_.add(NodeKind::Pattern, tok_.pos, vname, pkids);
        }
        eat(Kind::SymbolEqMt);
        skipLineEnds();
        NodeId body = at(Kind::BlockStart) ? parseBlock() : parseExpr();
        kids.push_back(ast_.add(NodeKind::MatchArm, ast_.at(pat).pos, {}, std::vector<NodeId>{pat, body}));
        eat(Kind::LineEnd);
    }
    eat(Kind::BlockEnd);
    return ast_.add(NodeKind::Match, start, {}, kids);
}

NodeId Parser::parseTryCatch() {
    const Pos start = tok_.pos;
    next();
    std::vector<NodeId> kids;
    appendIf(kids, parseBlock());
    while (at(Kind::Catch)) {
        const Pos cp = tok_.pos;
        next();
        std::string_view ename;
        if (at(Kind::ID)) {
            ename = tok_.text;
            next();
        }
        NodeId ty = parseType();
        NodeId body = parseBlock();
        kids.push_back(ast_.add(NodeKind::Catch, cp, ename, std::vector<NodeId>{ty, body}));
    }
    return ast_.add(NodeKind::TryCatch, start, {}, kids);
}

NodeId Parser::parsePrefix(bool allow_brace) {
    if (at(Kind::STR_TPL_OPEN)) return parseStringTpl();
    if (NodeId lit = parseLiteral(); lit != kEmptyNode) return lit;
    if (at(Kind::SymbolThis)) {
        const Pos p = tok_.pos;
        next();
        return ast_.add(NodeKind::This, p, "$");
    }
    if (at(Kind::If)) return parseIf();
    if (at(Kind::Match)) return parseMatch();
    if (at(Kind::Try)) return parseTryCatch();
    if (at(Kind::GetStart)) return parseArrayOrInit();
    if (at(Kind::ParStart)) return parseParenLambdaOrTuple();
    if (at(Kind::SymbolAnd) && (peekIs(Kind::ID) || peekIs(Kind::SymbolThis))) {
        const Pos start = tok_.pos;
        next();
        std::vector<NodeId> kids;
        if (at(Kind::SymbolThis)) {
            kids.push_back(ast_.add(NodeKind::This, tok_.pos, "$"));
            next();
        } else {
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
        }
        for (;;) {
            const Mark m = mark();
            skipLineEnds();
            if (eat(Kind::SymbolDot) && at(Kind::ID)) {
                kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                next();
                continue;
            }
            rewind(m);
            break;
        }
        return ast_.add(NodeKind::GetRef, start, {}, kids);
    }
    if (at(Kind::SymbolSub) || at(Kind::SymbolExcl)) {
        const Kind op = tok_.kind;
        const Pos p = tok_.pos;
        next();
        NodeId r = parseExpr(kBpUnary, allow_brace);
        return ast_.add(NodeKind::Prefix, p, {}, std::vector<NodeId>{r}, op);
    }
    if (allow_brace && aheadIsStructLit()) return parseStructLit();
    if (aheadIsEnumCtor()) {
        NodeId lhs = at(Kind::SelfType) ? ast_.add(NodeKind::TypeSelf, tok_.pos) : parseTypePath();
        if (at(Kind::SelfType)) next();
        return parseEnumCtor(lhs, false);
    }
    if (at(Kind::ID)) {
        NodeId id = ast_.add(NodeKind::Ident, tok_.pos, tok_.text);
        next();
        return id;
    }
    if (at(Kind::SelfType)) {
        NodeId n = ast_.add(NodeKind::TypeSelf, tok_.pos);
        next();
        return n;
    }
    if (at(Kind::BlockEnd) || at(Kind::Eof) || at(Kind::LineEnd) || at(Kind::ParEnd) || at(Kind::GetEnd) ||
        at(Kind::SymbolComma) || at(Kind::SymbolMt)) {
        return kEmptyNode;
    }
    next();
    return kEmptyNode;
}

NodeId Parser::parsePostfix(NodeId left) {
    if (at(Kind::SymbolColon) && peekIs(Kind::SymbolLt)) {
        const Mark m = mark();
        next();
        NodeId g = parseGenericArgs();
        if (at(Kind::ParStart)) {
            std::vector<NodeId> kids;
            kids.push_back(left);
            appendIf(kids, g);
            parseArgList(kids, Kind::ParEnd);
            if (at(Kind::BlockStart)) appendIf(kids, parseTrailingLambda());
            NodeId call = ast_.add(NodeKind::Call, ast_.at(left).pos, {}, kids);
            if (eat(Kind::SymbolExcl)) ast_.setOp(call, Kind::SymbolExcl);
            return call;
        }
        if (at(Kind::BlockStart)) {
            std::vector<NodeId> kids;
            kids.push_back(left);
            appendIf(kids, g);
            appendIf(kids, parseTrailingLambda());
            NodeId call = ast_.add(NodeKind::Call, ast_.at(left).pos, {}, kids);
            if (eat(Kind::SymbolExcl)) ast_.setOp(call, Kind::SymbolExcl);
            return call;
        }
        rewind(m);
        return left;
    }
    if (at(Kind::ParStart)) return parseCall(left);
    if (at(Kind::BlockStart)) {
        std::vector<NodeId> kids;
        kids.push_back(left);
        appendIf(kids, parseTrailingLambda());
        NodeId call = ast_.add(NodeKind::Call, ast_.at(left).pos, {}, kids);
        if (eat(Kind::SymbolExcl)) ast_.setOp(call, Kind::SymbolExcl);
        return call;
    }
    if (at(Kind::GetStart)) return parseGet(left);
    if (at(Kind::DOT_NUM)) {
        NodeId n = ast_.add(NodeKind::TupleMember, tok_.pos, tok_.text, std::vector<NodeId>{left});
        next();
        return n;
    }
    Kind quest = Kind::Invalid;
    if (at(Kind::SymbolQuest) && peekIs(Kind::SymbolDot)) {
        quest = Kind::SymbolQuest;
        next();
    }
    if (at(Kind::SymbolDot)) {
        next();
        if (!at(Kind::ID)) return left;
        const std::string_view name = tok_.text;
        const Pos p = tok_.pos;
        next();
        std::vector<NodeId> kids;
        kids.push_back(left);
        if (eat(Kind::SymbolAt) && at(Kind::ID)) {
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
        }
        return ast_.add(NodeKind::Dot, p, name, kids, quest);
    }
    return left;
}

NodeId Parser::parseExpr(int min_bp, bool allow_brace) {
    NodeId left = parsePrefix(allow_brace);
    if (left == kEmptyNode) return left;
    for (;;) {
        if (at(Kind::LineEnd)) {
            int i = 1;
            while (la(i).kind == Kind::LineEnd)
                ++i;
            const Token& t = la(i);
            const bool member = t.kind == Kind::SymbolDot || t.kind == Kind::DOT_NUM ||
                                (t.kind == Kind::SymbolQuest && la(i + 1).kind == Kind::SymbolDot);
            // a\n  .b 只在 `.` 更靠右时续行；`.x = 1\n .y = 2` 的 `.y` 对齐字段，不接到 1 上。
            if (!member || t.pos.column <= ast_.at(left).pos.column) break;
            skipLineEnds();
        }

        const auto knd = ast_.at(left).kind;
        if (at(Kind::SymbolExcl) && (knd == NodeKind::Call || knd == NodeKind::EnumCtor)) {
            if (kBpPostfix < min_bp) break;
            next();
            ast_.setOp(left, Kind::SymbolExcl);
            continue;
        }

        const bool postfix = at(Kind::ParStart) || at(Kind::GetStart) || at(Kind::SymbolDot) || at(Kind::DOT_NUM) ||
                             (allow_brace && aheadIsTrailingLambda()) ||
                             (at(Kind::SymbolColon) && peekIs(Kind::SymbolLt)) ||
                             (at(Kind::SymbolQuest) && peekIs(Kind::SymbolDot));
        if (postfix) {
            if (kBpPostfix < min_bp) break;
            const Token before = tok_;
            left = parsePostfix(left);
            if (tok_.kind == before.kind && tok_.pos.offset == before.pos.offset) break;
            continue;
        }

        auto infix = [&](int lbp, int rbp, Kind op, std::string_view spelling = {}) -> bool {
            if (lbp < min_bp) return false;
            next();
            NodeId r = parseExpr(rbp, allow_brace);
            left = ast_.add(NodeKind::Infix, ast_.at(left).pos, spelling, std::vector<NodeId>{left, r}, op);
            return true;
        };

        if (at(Kind::SymbolQuest) && peekIs(Kind::SymbolQuest)) {
            if (kBpNullElse < min_bp) break;
            next();
            next();
            NodeId r = parseExpr(kBpNullElse, allow_brace);
            left = ast_.add(NodeKind::NullElse, ast_.at(left).pos, {}, std::vector<NodeId>{left, r});
            continue;
        }
        if (at(Kind::SymbolLtSub)) {
            if (!infix(kBpMove, kBpMove, Kind::SymbolLtSub)) break;
            continue;
        }
        if (at(Kind::SymbolOrOr) || at(Kind::SymbolAndAnd)) {
            if (!infix(kBpBool, kBpBool + 1, tok_.kind)) break;
            continue;
        }
        if (at(Kind::SymbolEqEq) || at(Kind::SymbolExclEq)) {
            if (!infix(kBpEq, kBpEq + 1, tok_.kind)) break;
            continue;
        }
        if (at(Kind::SymbolMt) && peekIs(Kind::SymbolEq)) {
            if (kBpCmp < min_bp) break;
            next();
            next();
            NodeId r = parseExpr(kBpCmp + 1, allow_brace);
            left = ast_.add(NodeKind::Infix, ast_.at(left).pos, ">=", std::vector<NodeId>{left, r}, Kind::SymbolMt);
            continue;
        }
        if (at(Kind::SymbolLt) && peekIs(Kind::SymbolEq)) {
            if (kBpCmp < min_bp) break;
            next();
            next();
            NodeId r = parseExpr(kBpCmp + 1, allow_brace);
            left = ast_.add(NodeKind::Infix, ast_.at(left).pos, "<=", std::vector<NodeId>{left, r}, Kind::SymbolLt);
            continue;
        }
        if (at(Kind::SymbolMt) || at(Kind::SymbolLt)) {
            if (!infix(kBpCmp, kBpCmp + 1, tok_.kind)) break;
            continue;
        }
        if (at(Kind::SymbolAdd) || at(Kind::SymbolSub)) {
            if (!infix(kBpAdd, kBpAdd + 1, tok_.kind)) break;
            continue;
        }
        if (at(Kind::SymbolMul) || at(Kind::SymbolDiv) || at(Kind::SymbolMod)) {
            if (!infix(kBpMul, kBpMul + 1, tok_.kind)) break;
            continue;
        }
        break;
    }
    return left;
}

// ==== 形参 / 函数体 ====

void Parser::parseLambdaParams(std::vector<NodeId>& out) {
    parseFnParam(out); // 组糖与 fn 同形，类型可省：见 parseFnParam 的 type?
    while (eat(Kind::SymbolComma)) {
        skipLineEnds();
        if (at(Kind::ParEnd)) break;
        parseFnParam(out);
    }
    skipLineEnds();
}

void Parser::parseFnParam(std::vector<NodeId>& out) {
    std::vector<NodeId> annos;
    while (at(Kind::SymbolHash)) {
        const Pos start = tok_.pos;
        next();
        std::string_view name;
        if (at(Kind::ID)) {
            name = tok_.text;
            next();
        }
        eat(Kind::LineEnd);
        annos.push_back(ast_.add(NodeKind::Anno, start, name));
    }
    std::vector<Token> names;
    if (!at(Kind::ID)) {
        if (!at(Kind::ParEnd) && !at(Kind::SymbolComma) && !at(Kind::LineEnd) && !at(Kind::Eof) &&
            !at(Kind::BlockStart)) {
            errorExpected(Kind::ID);
        }
        return;
    }
    names.push_back(tok_);
    next();
    while (at(Kind::SymbolComma) && peekIs(Kind::ID)) {
        next();
        skipLineEnds();
        if (!at(Kind::ID)) break;
        names.push_back(tok_);
        next();
    }
    NodeId ty = looksLikeType() ? parseType() : kEmptyNode;
    for (const Token& n : names) {
        std::vector<NodeId> kids = annos;
        appendIf(kids, ty);
        out.push_back(ast_.add(NodeKind::Param, n.pos, n.text, kids));
    }
}

void Parser::parseFnParams(std::vector<NodeId>& out) {
    parseFnParam(out);
    while (eat(Kind::SymbolComma)) {
        skipLineEnds();
        if (at(Kind::ParEnd)) break;
        parseFnParam(out);
    }
    skipLineEnds();
}

NodeId Parser::parseOptionalFnRet() {
    if (at(Kind::SymbolExcl)) {
        next();
        NodeId err = parseType();
        NodeId unit = ast_.add(NodeKind::TypeUnit, tok_.pos);
        return ast_.add(NodeKind::TypeFallible, ast_.at(err).pos, {}, std::vector<NodeId>{unit, err});
    }
    if (!looksLikeType()) return kEmptyNode;
    NodeId t = parseType();
    if (at(Kind::SymbolExcl)) {
        next();
        NodeId err = parseType();
        return ast_.add(NodeKind::TypeFallible, ast_.at(t).pos, {}, std::vector<NodeId>{t, err});
    }
    return t;
}

NodeId Parser::parseBlock() {
    const Pos start = tok_.pos;
    if (!eat(Kind::BlockStart)) return kEmptyNode;
    skipLineEnds();
    std::vector<NodeId> stmts;
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        NodeId s = parseStatement();
        if (s != kEmptyNode)
            stmts.push_back(s);
        else if (!at(Kind::BlockEnd) && !at(Kind::Eof))
            errorUnknown();
    }
    const Pos end = tok_.pos;
    eat(Kind::BlockEnd);
    return ast_.add(NodeKind::Block, spanPos(start, end), {}, stmts);
}

NodeId Parser::parseFnBody() {
    skipLineEnds();
    if (eat(Kind::SymbolEq)) {
        NodeId e = parseExpr();
        eat(Kind::LineEnd);
        return e;
    }
    if (at(Kind::BlockStart)) return parseBlock();
    return kEmptyNode;
}

// ==== 语句 ====

NodeId Parser::parseLoop() {
    std::string_view label;
    Pos start = tok_.pos;
    if (at(Kind::ID) && peekIs(Kind::SymbolColon)) {
        label = tok_.text;
        start = tok_.pos;
        next();
        next();
    }
    eat(Kind::Loop);
    std::vector<NodeId> kids;
    if (!at(Kind::BlockStart)) {
        if (eat(Kind::ParStart)) {
            std::vector<NodeId> names;
            if (at(Kind::ID)) {
                names.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                next();
            }
            while (eat(Kind::SymbolComma) && at(Kind::ID)) {
                names.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                next();
            }
            eat(Kind::ParEnd);
            NodeId ty = looksLikeType() ? parseType() : kEmptyNode;
            eat(Kind::SymbolEq);
            NodeId e = parseExpr(kBpLowest, false);
            kids.insert(kids.end(), names.begin(), names.end());
            appendIf(kids, ty);
            appendIf(kids, e);
        } else if (at(Kind::ID)) {
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
            if (looksLikeType()) appendIf(kids, parseType());
            eat(Kind::SymbolEq);
            appendIf(kids, parseExpr(kBpLowest, false));
        }
    }
    appendIf(kids, parseBlock());
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::Loop, start, label, kids);
}

NodeId Parser::parseForIn() {
    std::string_view label;
    Pos start = tok_.pos;
    if (at(Kind::ID) && peekIs(Kind::SymbolColon)) {
        label = tok_.text;
        start = tok_.pos;
        next();
        next();
    }
    eat(Kind::For);
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    }
    eat(Kind::In);
    NodeId e = parseExpr(kBpLowest, false);
    NodeId body = parseBlock();
    eat(Kind::LineEnd);
    std::vector<NodeId> kids;
    appendIf(kids, e);
    appendIf(kids, body);
    return ast_.add(NodeKind::ForIn, start, name, kids);
}

NodeId Parser::parseStatement() {
    std::vector<NodeId> annos;
    while (at(Kind::SymbolHash)) {
        const Mark m = mark();
        NodeId a = parseAnno();
        skipLineEnds();
        if (at(Kind::Let)) {
            appendIf(annos, a);
            continue;
        }
        rewind(m);
        break;
    }
    if (at(Kind::Let)) return parseLet(std::move(annos), false);
    if (at(Kind::TypeKw)) return parseAlias();
    if (at(Kind::Ret)) {
        const Pos p = tok_.pos;
        next();
        if (eat(Kind::SymbolSemicolon)) {
            eat(Kind::LineEnd);
            return ast_.add(NodeKind::RetVoid, p);
        }
        NodeId e = parseExpr();
        eat(Kind::SymbolSemicolon);
        eat(Kind::LineEnd);
        std::vector<NodeId> kids;
        appendIf(kids, e);
        return ast_.add(NodeKind::Ret, p, {}, kids);
    }
    if (at(Kind::Break) || at(Kind::Continue)) {
        const bool brk = at(Kind::Break);
        const Pos p = tok_.pos;
        next();
        std::string_view lab;
        if (eat(Kind::SymbolAt) && at(Kind::ID)) {
            lab = tok_.text;
            next();
        }
        eat(Kind::SymbolSemicolon);
        eat(Kind::LineEnd);
        return ast_.add(brk ? NodeKind::Break : NodeKind::Continue, p, lab);
    }
    if (at(Kind::Loop) || (at(Kind::ID) && peekIs(Kind::SymbolColon) && la(2).kind == Kind::Loop)) return parseLoop();
    if (at(Kind::For) || (at(Kind::ID) && peekIs(Kind::SymbolColon) && la(2).kind == Kind::For)) return parseForIn();

    if (at(Kind::ID)) {
        int i = 1;
        while (peekIs(Kind::SymbolDot, i) && peekIs(Kind::ID, i + 1))
            i += 2;
        if (peekIs(Kind::SymbolColonColon, i) && peekIs(Kind::ID, i + 1) && peekIs(Kind::SymbolEq, i + 2)) {
            NodeId path = parseTypePath();
            eat(Kind::SymbolColonColon);
            std::string_view field;
            if (at(Kind::ID)) {
                field = tok_.text;
                next();
            }
            eat(Kind::SymbolEq);
            NodeId v = parseExpr();
            eat(Kind::SymbolSemicolon);
            eat(Kind::LineEnd);
            return ast_.add(NodeKind::StaticFieldSet, ast_.at(path).pos, field, std::vector<NodeId>{path, v});
        }
        i = 1;
        bool assign = false;
        if (isAssignOp(la(1).kind)) {
            assign = true;
        } else {
            while (true) {
                if (peekIs(Kind::SymbolDot, i) && peekIs(Kind::ID, i + 1)) {
                    i += 2;
                    continue;
                }
                if (peekIs(Kind::DOT_NUM, i)) {
                    ++i;
                    continue;
                }
                assign = isAssignOp(la(i).kind);
                break;
            }
        }
        if (assign) {
            const Pos start = tok_.pos;
            std::vector<NodeId> kids;
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
            while (at(Kind::SymbolDot) || at(Kind::DOT_NUM)) {
                if (at(Kind::DOT_NUM)) {
                    kids.push_back(ast_.add(NodeKind::TupleMember, tok_.pos, tok_.text));
                    next();
                    continue;
                }
                next();
                if (at(Kind::ID)) {
                    kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                    next();
                }
            }
            const Kind op = tok_.kind;
            next();
            appendIf(kids, parseExpr());
            eat(Kind::SymbolSemicolon);
            eat(Kind::LineEnd);
            return ast_.add(NodeKind::Assign, start, {}, kids, op);
        }
    }
    if (at(Kind::SymbolThis)) {
        int i = 1;
        bool assign = isAssignOp(la(i).kind);
        if (!assign) {
            while (true) {
                if (peekIs(Kind::SymbolDot, i) && peekIs(Kind::ID, i + 1)) {
                    i += 2;
                    continue;
                }
                if (peekIs(Kind::DOT_NUM, i)) {
                    ++i;
                    continue;
                }
                assign = isAssignOp(la(i).kind);
                break;
            }
        }
        if (assign) {
            const Pos start = tok_.pos;
            std::vector<NodeId> kids;
            kids.push_back(ast_.add(NodeKind::This, tok_.pos, "$"));
            next();
            while (at(Kind::SymbolDot) || at(Kind::DOT_NUM)) {
                if (at(Kind::DOT_NUM)) {
                    kids.push_back(ast_.add(NodeKind::TupleMember, tok_.pos, tok_.text));
                    next();
                    continue;
                }
                next();
                if (at(Kind::ID)) {
                    kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
                    next();
                }
            }
            const Kind op = tok_.kind;
            next();
            appendIf(kids, parseExpr());
            eat(Kind::SymbolSemicolon);
            eat(Kind::LineEnd);
            return ast_.add(NodeKind::Assign, start, {}, kids, op);
        }
    }

    NodeId e = parseExpr();
    if (e == kEmptyNode) return kEmptyNode;
    if (ast_.at(e).kind == NodeKind::Get && at(Kind::SymbolEq)) {
        next();
        NodeId v = parseExpr();
        eat(Kind::SymbolSemicolon);
        eat(Kind::LineEnd);
        std::vector<NodeId> kids;
        const Node& g = ast_.at(e);
        for (i32 i = 0; i < g.children_count; ++i)
            appendIf(kids, ast_.child(e, i));
        appendIf(kids, v);
        return ast_.add(NodeKind::Set, ast_.at(e).pos, {}, kids);
    }
    eat(Kind::SymbolSemicolon);
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::ExprStmt, ast_.at(e).pos, {}, std::vector<NodeId>{e});
}

// ==== 顶层 ====

NodeId Parser::parseAnno() {
    const Pos start = tok_.pos;
    next(); // #
    if (!at(Kind::ID)) {
        errorExpected(Kind::ID);
        return ast_.add(NodeKind::Anno, start);
    }
    const std::string_view name = tok_.text;
    next();
    std::vector<NodeId> kids;
    if (eat(Kind::ParStart)) {
        if (at(Kind::INT) || at(Kind::FLOAT) || at(Kind::STR_LINE_RAW) || at(Kind::STR_TPL_OPEN) ||
            at(Kind::CODE_POINT)) {
            if (at(Kind::STR_TPL_OPEN))
                kids.push_back(parseStringTpl());
            else {
                NodeId lit = parseLiteral();
                appendIf(kids, lit);
            }
        } else {
            appendIf(kids, parseType());
        }
        eat(Kind::ParEnd);
    }
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::Anno, start, name, kids);
}

NodeId Parser::parseUse() {
    const Pos start = tok_.pos;
    next(); // Use

    if (!at(Kind::ID)) {
        errorExpected(Kind::ID);
        return ast_.add(NodeKind::Use, start);
    }

    const Token first = tok_;
    Token last = tok_;
    next();
    while (at(Kind::SymbolDot) && peekIs(Kind::ID)) {
        next();
        last = tok_;
        next();
    }
    if (at(Kind::SymbolDot) && peekIs(Kind::SymbolMul)) {
        next();
        last = tok_;
        next();
    }

    const std::string_view value = sliceTokens(first, last);
    if (!at(Kind::LineEnd) && !at(Kind::Eof))
        errorUnknown();
    else
        eat(Kind::LineEnd);

    return ast_.add(NodeKind::Use, spanPos(start, last.pos), value);
}

NodeId Parser::parseAlias() {
    const Pos start = tok_.pos;
    next(); // type
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    }
    eat(Kind::SymbolEq);
    NodeId ty = parseType();
    eat(Kind::LineEnd);
    std::vector<NodeId> kids;
    appendIf(kids, ty);
    return ast_.add(NodeKind::Alias, start, name, kids);
}

NodeId Parser::parseLet(std::vector<NodeId> annos, bool /*global*/) {
    const Pos start = tok_.pos;
    next(); // Let
    if (eat(Kind::ParStart)) {
        std::vector<NodeId> kids = std::move(annos);
        if (at(Kind::ID)) {
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
        }
        while (eat(Kind::SymbolComma) && at(Kind::ID)) {
            kids.push_back(ast_.add(NodeKind::Ident, tok_.pos, tok_.text));
            next();
        }
        eat(Kind::ParEnd);
        if (looksLikeType()) appendIf(kids, parseType());
        eat(Kind::SymbolEq);
        appendIf(kids, parseExpr());
        eat(Kind::LineEnd);
        return ast_.add(NodeKind::LetTuple, start, {}, kids);
    }
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    } else {
        errorExpected(Kind::ID);
        return ast_.add(NodeKind::Let, start, {}, annos);
    }
    std::vector<NodeId> kids = std::move(annos);
    if (looksLikeType()) appendIf(kids, parseType());
    if (eat(Kind::SymbolEq)) appendIf(kids, parseExpr());
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::Let, start, name, kids);
}

NodeId Parser::parseFn(std::vector<NodeId> annos) {
    const Pos start = tok_.pos;
    next(); // Fn
    if (at(Kind::SymbolRev)) return parseFnClean();
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    } else {
        errorExpected(Kind::ID);
        return ast_.add(NodeKind::Fn, start, {}, annos);
    }
    std::vector<NodeId> kids = std::move(annos);
    if (at(Kind::SymbolLt)) appendIf(kids, parseGenericDef());
    eat(Kind::ParStart);
    skipLineEnds();
    if (!at(Kind::ParEnd)) parseFnParams(kids);
    eat(Kind::ParEnd);
    appendIf(kids, parseOptionalFnRet());
    appendIf(kids, parseFnBody());
    return ast_.add(NodeKind::Fn, start, name, kids);
}

NodeId Parser::parseFnClean() {
    const Pos start = tok_.pos;
    eat(Kind::SymbolRev);
    eat(Kind::ParStart);
    eat(Kind::ParEnd);
    NodeId body = parseFnBody();
    std::vector<NodeId> kids;
    appendIf(kids, body);
    return ast_.add(NodeKind::FnClean, start, {}, kids);
}

NodeId Parser::parseExtern(std::vector<NodeId> annos) {
    const Pos start = tok_.pos;
    next();
    eat(Kind::BlockStart);
    skipLineEnds();
    std::vector<NodeId> kids = std::move(annos);
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        std::vector<NodeId> fn_annos;
        while (at(Kind::SymbolHash)) {
            appendIf(fn_annos, parseAnno());
            skipLineEnds();
        }
        if (at(Kind::Fn)) {
            appendIf(kids, parseFn(std::move(fn_annos)));
            eat(Kind::LineEnd);
            continue;
        }
        errorUnknown();
    }
    eat(Kind::BlockEnd);
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::Extern, start, {}, kids);
}

NodeId Parser::parseEnum() {
    const Pos start = tok_.pos;
    next();
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    }
    std::vector<NodeId> kids;
    if (at(Kind::SymbolLt)) appendIf(kids, parseGenericDef());
    eat(Kind::BlockStart);
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        if (!at(Kind::ID)) {
            errorExpected(Kind::ID);
            continue;
        }
        const std::string_view vname = tok_.text;
        const Pos vp = tok_.pos;
        next();
        std::vector<NodeId> payloads;
        if (eat(Kind::ParStart)) {
            if (!at(Kind::ParEnd)) {
                payloads.push_back(parseType());
                while (eat(Kind::SymbolComma))
                    payloads.push_back(parseType());
            }
            eat(Kind::ParEnd);
        }
        eat(Kind::LineEnd);
        kids.push_back(ast_.add(NodeKind::EnumVariant, vp, vname, payloads));
    }
    eat(Kind::BlockEnd);
    return ast_.add(NodeKind::Enum, start, name, kids);
}

NodeId Parser::parseField(std::vector<NodeId> annos) {
    if (!at(Kind::ID)) return kEmptyNode;
    const Pos start = tok_.pos;
    const std::string_view name = tok_.text;
    next();
    std::vector<NodeId> kids = std::move(annos);
    appendIf(kids, parseType());
    if (eat(Kind::SymbolEq)) appendIf(kids, parseExpr());
    eat(Kind::LineEnd);
    return ast_.add(NodeKind::Field, start, name, kids);
}

NodeId Parser::parseStruct(std::vector<NodeId> annos) {
    const Pos start = tok_.pos;
    next();
    std::string_view name;
    if (at(Kind::ID)) {
        name = tok_.text;
        next();
    }
    std::vector<NodeId> kids = std::move(annos);
    if (at(Kind::SymbolLt)) appendIf(kids, parseGenericDef());
    eat(Kind::BlockStart);
    eat(Kind::LineEnd);
    while (!at(Kind::BlockEnd) && !at(Kind::Eof) && !at(Kind::Fn)) {
        if (eat(Kind::LineEnd)) continue;
        std::vector<NodeId> fa;
        while (at(Kind::SymbolHash)) {
            appendIf(fa, parseAnno());
            skipLineEnds();
        }
        if (at(Kind::TypeKw)) {
            appendIf(kids, parseAlias());
            continue;
        }
        if (at(Kind::ID)) {
            appendIf(kids, parseField(std::move(fa)));
            continue;
        }
        break;
    }
    if (at(Kind::Fn) && peekIs(Kind::SymbolRev)) appendIf(kids, parseFn({})); // fn ~()
    while (!at(Kind::BlockEnd) && !at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        std::vector<NodeId> fa;
        while (at(Kind::SymbolHash)) {
            appendIf(fa, parseAnno());
            skipLineEnds();
        }
        if (at(Kind::Fn)) {
            appendIf(kids, parseFn(std::move(fa)));
            eat(Kind::LineEnd);
            continue;
        }
        errorUnknown();
    }
    eat(Kind::BlockEnd);
    return ast_.add(NodeKind::Struct, start, name, kids);
}

NodeId Parser::parseItem() {
    std::vector<NodeId> annos;
    while (at(Kind::SymbolHash)) {
        appendIf(annos, parseAnno());
        skipLineEnds();
    }
    if (at(Kind::Fn)) return parseFn(std::move(annos));
    if (at(Kind::Extern)) return parseExtern(std::move(annos));
    if (at(Kind::Let)) return parseLet(std::move(annos), true);
    if (at(Kind::TypeKw)) return parseAlias();
    if (at(Kind::Enum)) return parseEnum();
    if (at(Kind::Struct)) return parseStruct(std::move(annos));
    return kEmptyNode;
}

ParseResult Parser::parse() {
    next();
    skipLineEnds();

    std::vector<NodeId> items;
    while (at(Kind::Use)) {
        items.push_back(parseUse());
        skipLineEnds();
    }
    while (!at(Kind::Eof)) {
        if (eat(Kind::LineEnd)) continue;
        NodeId n = parseItem();
        if (n != kEmptyNode) {
            items.push_back(n);
            continue;
        }
        errorUnknown();
    }

    Pos file_pos;
    file_pos.end = tok_.pos.end;
    const NodeId root = ast_.add(NodeKind::Program, file_pos, {}, items);
    ast_.setRoot(root);
    return ParseResult{std::move(ast_), std::move(errors_)};
}

ParseResult parseProgram(std::string_view src) {
    Parser p(src);
    return p.parse();
}

} // namespace rd

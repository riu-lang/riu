// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/scanner.h"

#include <array>
#include <string>

namespace rd {
namespace {

struct Decoded {
    char32_t cp = 0;
    size_t n = 0;
};

Decoded decodeAt(std::string_view s, size_t off) {
    if (off >= s.size()) return {};
    const auto* p = reinterpret_cast<const unsigned char*>(s.data() + off);
    const size_t remain = s.size() - off;
    const unsigned char c0 = p[0];
    if (c0 < 0x80) return {.cp = c0, .n = 1};
    if ((c0 & 0xE0u) == 0xC0u && remain >= 2 && (p[1] & 0xC0u) == 0x80u) {
        const auto cp =
            static_cast<char32_t>((static_cast<unsigned>(c0 & 0x1Fu) << 6u) | static_cast<unsigned>(p[1] & 0x3Fu));
        if (cp >= 0x80) return {.cp = cp, .n = 2};
    } else if ((c0 & 0xF0u) == 0xE0u && remain >= 3 && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
        const auto cp =
            static_cast<char32_t>((static_cast<unsigned>(c0 & 0x0Fu) << 12u) |
                                  (static_cast<unsigned>(p[1] & 0x3Fu) << 6u) | static_cast<unsigned>(p[2] & 0x3Fu));
        if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) return {.cp = cp, .n = 3};
    } else if ((c0 & 0xF8u) == 0xF0u && remain >= 4 && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u &&
               (p[3] & 0xC0u) == 0x80u) {
        const auto cp = static_cast<char32_t>(
            (static_cast<unsigned>(c0 & 0x07u) << 18u) | (static_cast<unsigned>(p[1] & 0x3Fu) << 12u) |
            (static_cast<unsigned>(p[2] & 0x3Fu) << 6u) | static_cast<unsigned>(p[3] & 0x3Fu));
        if (cp >= 0x10000 && cp <= 0x10FFFF) return {.cp = cp, .n = 4};
    }
    return {.cp = c0, .n = 1};
}

bool isWhiteSpace(char32_t cp) {
    switch (cp) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

bool isUnicodeId(char32_t cp) {
    return cp >= 0x80 && !isWhiteSpace(cp);
}

constexpr auto kIdHead = [] {
    std::array<bool, 256> a{};
    for (int c = 'A'; c <= 'Z'; ++c)
        a[static_cast<size_t>(c)] = true;
    for (int c = 'a'; c <= 'z'; ++c)
        a[static_cast<size_t>(c)] = true;
    a[static_cast<size_t>('_')] = true;
    return a;
}();

constexpr auto kIdTail = [] {
    std::array<bool, 256> a = kIdHead;
    for (int c = '0'; c <= '9'; ++c)
        a[static_cast<size_t>(c)] = true;
    return a;
}();

bool isDigit(unsigned char c) {
    return c >= '0' && c <= '9';
}
bool isBin(unsigned char c) {
    return c == '0' || c == '1';
}
bool isOct(unsigned char c) {
    return c >= '0' && c <= '7';
}
bool isHex(unsigned char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isTrivia(Kind k) {
    return k == Kind::Space || k == Kind::LineComment || k == Kind::LineEndComment;
}

} // namespace

Scanner::Scanner(std::string_view src, std::vector<ParseError>* errors) : src_(src) {
    if (errors) diags_ = errors;
}

void Scanner::setErrors(std::vector<ParseError>* errors) {
    diags_ = errors ? errors : &owned_diags_;
}

Scanner::Snapshot Scanner::snapshot() const {
    Snapshot s;
    s.byte_pos = byte_pos_;
    s.line = line_;
    s.column = column_;
    s.hit_eof = hit_eof_;
    s.mode = static_cast<std::uint8_t>(mode_);
    s.mode_stack.reserve(mode_stack_.size());
    for (Mode m : mode_stack_)
        s.mode_stack.push_back(static_cast<std::uint8_t>(m));
    s.interp_brace_depth = interp_brace_depth_;
    s.diag_count = diags_->size();
    s.default_index = default_index_;
    return s;
}

void Scanner::restore(const Snapshot& s) {
    byte_pos_ = s.byte_pos;
    line_ = s.line;
    column_ = s.column;
    hit_eof_ = s.hit_eof;
    mode_ = static_cast<Mode>(s.mode);
    mode_stack_.clear();
    mode_stack_.reserve(s.mode_stack.size());
    for (std::uint8_t m : s.mode_stack)
        mode_stack_.push_back(static_cast<Mode>(m));
    interp_brace_depth_ = s.interp_brace_depth;
    if (diags_->size() > s.diag_count) diags_->resize(s.diag_count);
    default_index_ = s.default_index;
}

unsigned char Scanner::ch() const {
    return byte_pos_ < src_.size() ? static_cast<unsigned char>(src_[byte_pos_]) : 0;
}

unsigned char Scanner::ch(size_t n) const {
    const size_t i = byte_pos_ + n;
    return i < src_.size() ? static_cast<unsigned char>(src_[i]) : 0;
}

void Scanner::adv(size_t n) {
    byte_pos_ += n;
    column_ += static_cast<i32>(n);
}

void Scanner::consumeNewline() {
    if (ch() == '\r') adv(1);
    if (ch() == '\n') adv(1);
    ++line_;
    column_ = 0;
}

void Scanner::pushMode(Mode m) {
    mode_stack_.push_back(mode_);
    mode_ = m;
}

void Scanner::popMode() {
    if (mode_stack_.empty()) return;
    mode_ = mode_stack_.back();
    mode_stack_.pop_back();
}

void Scanner::applySideEffects(Kind kind) {
    switch (kind) {
    case Kind::STR_TPL_OPEN:
        pushMode(Mode::StrTpl);
        break;
    case Kind::STR_TPL_CLOSE:
        popMode();
        break;
    case Kind::STR_TPL_INTERP_OPEN:
        interp_brace_depth_.push_back(0);
        pushMode(Mode::Default);
        break;
    case Kind::BlockStart:
        if (!interp_brace_depth_.empty()) ++interp_brace_depth_.back();
        break;
    case Kind::BlockEnd:
        if (!interp_brace_depth_.empty()) {
            if (interp_brace_depth_.back() == 0) {
                interp_brace_depth_.pop_back();
                popMode();
            } else {
                --interp_brace_depth_.back();
            }
        }
        break;
    default:
        break;
    }
}

void Scanner::pushLexerError(i32 line, i32 col, std::string_view text) {
    ParseError e;
    e.is_lexer = true;
    e.pos.line = line;
    e.pos.column = col;
    e.pos.offset = static_cast<i32>(byte_pos_);
    e.pos.end = static_cast<i32>(byte_pos_ + text.size());
    e.offending = std::string(text);
    std::string at;
    at.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
        case '\n':
            at += "\\n";
            break;
        case '\r':
            at += "\\r";
            break;
        case '\t':
            at += "\\t";
            break;
        default:
            at.push_back(static_cast<char>(c));
            break;
        }
    }
    e.message = "token recognition error at: '" + at + "'";
    diags_->push_back(std::move(e));
}

Token Scanner::skipIllegal(size_t start_byte, i32 start_line, i32 start_col) {
    const auto d = decodeAt(src_, byte_pos_);
    const size_t n = d.n == 0 ? 1 : d.n;
    const std::string_view text = src_.substr(start_byte, n);
    pushLexerError(start_line, start_col, text);
    adv(n);
    Token skip;
    skip.kind = Kind::Invalid;
    skip.pos = Pos{.offset = static_cast<i32>(start_byte),
                   .end = static_cast<i32>(byte_pos_),
                   .line = start_line,
                   .column = start_col};
    skip.text = text;
    return skip;
}

Token Scanner::makeEof() const {
    Token tok;
    tok.kind = Kind::Eof;
    tok.pos = Pos{
        .offset = static_cast<i32>(byte_pos_), .end = static_cast<i32>(byte_pos_), .line = line_, .column = column_};
    tok.text = {};
    return tok;
}

Token Scanner::emit(Kind kind, size_t start_byte, i32 start_line, i32 start_col) {
    Token t;
    t.kind = kind;
    t.pos = Pos{.offset = static_cast<i32>(start_byte),
                .end = static_cast<i32>(byte_pos_),
                .line = start_line,
                .column = start_col};
    t.text = src_.substr(start_byte, byte_pos_ - start_byte);
    applySideEffects(kind);
    return t;
}

void Scanner::skipLineComment() {
    while (!atEnd() && ch() != '\r' && ch() != '\n')
        adv(1);
    if (!atEnd()) consumeNewline();
}

void Scanner::skipLineEndComment() {
    while (!atEnd() && ch() != '\r' && ch() != '\n')
        adv(1);
}

void Scanner::skipTrivia() {
    if (mode_ != Mode::Default) return;
    for (;;) {
        if (atEnd()) return;
        const unsigned char c = ch();
        if (c == ' ') {
            if (column_ == 0) {
                size_t i = 0;
                while (ch(i) == ' ')
                    ++i;
                if (ch(i) == ';') {
                    adv(i + 1);
                    skipLineComment();
                    continue;
                }
            }
            size_t i = 0;
            while (ch(i) == ' ')
                ++i;
            if (ch(i) == ';') {
                adv(i + 1);
                skipLineEndComment();
                continue;
            }
            adv(i);
            continue;
        }
        if (c == ';' && column_ == 0) {
            adv(1);
            skipLineComment();
            continue;
        }
        return;
    }
}

void Scanner::consumeInt10() {
    if (!isDigit(ch())) return;
    adv(1);
    for (;;) {
        size_t i = 0;
        if (ch() == '_') i = 1;
        if (!isDigit(ch(i))) break;
        if (i != 0) adv(1);
        while (isDigit(ch()))
            adv(1);
    }
}

void Scanner::consumeGrouped(bool (*ok)(unsigned char)) {
    if (!ok(ch())) return;
    adv(1);
    for (;;) {
        size_t i = 0;
        if (ch() == '_') i = 1;
        if (!ok(ch(i))) break;
        if (i != 0) adv(1);
        while (ok(ch()))
            adv(1);
    }
}

bool Scanner::consumeIntSuffix() {
    const unsigned char u = ch();
    if (u != 'i' && u != 'u') return false;
    const unsigned char a = ch(1);
    const unsigned char b = ch(2);
    const unsigned char c = ch(3);
    const unsigned char d = ch(4);
    if (a == 's' && b == 'i' && c == 'z' && d == 'e') {
        adv(5);
        return true;
    }
    if (a == '6' && b == '4') {
        adv(3);
        return true;
    }
    if (a == '3' && b == '2') {
        adv(3);
        return true;
    }
    if (a == '1' && b == '6') {
        adv(3);
        return true;
    }
    if (a == '8') {
        adv(2);
        return true;
    }
    return false;
}

bool Scanner::consumeFloatSuffix() {
    if (ch() == 'f' && ch(1) == '3' && ch(2) == '2') {
        adv(3);
        return true;
    }
    if (ch() == 'f' && ch(1) == '6' && ch(2) == '4') {
        adv(3);
        return true;
    }
    return false;
}

bool Scanner::tryRawString() {
    if (ch() != 'r' || ch(1) != '"') return false;
    size_t i = 2;
    while (byte_pos_ + i < src_.size()) {
        const unsigned char c = ch(i);
        if (c == '\r' || c == '\n') return false;
        if (c == '"') {
            adv(i + 1);
            return true;
        }
        ++i;
    }
    return false;
}

bool Scanner::tryCodePoint() {
    if (ch() != 'c' || ch(1) != '\'') return false;
    size_t i = 2;
    auto d = decodeAt(src_, byte_pos_ + i);
    if (d.n == 0) return false;
    if (d.cp == '\\') {
        auto e = decodeAt(src_, byte_pos_ + i + d.n);
        if (e.n == 0) return false;
        switch (e.cp) {
        case 'b':
        case 'n':
        case 'r':
        case 't':
        case 'v':
        case '0':
        case '\\':
        case '\'':
            i += d.n + e.n;
            break;
        default:
            return false;
        }
    } else if (d.cp == '\r' || d.cp == '\n' || d.cp == '\\' || d.cp == '\'') {
        return false;
    } else {
        i += d.n;
    }
    if (ch(i) != '\'') return false;
    adv(i + 1);
    return true;
}

void Scanner::consumeIdentRest() {
    for (;;) {
        if (atEnd()) break;
        const unsigned char c = ch();
        if (c < 0x80) {
            if (!kIdTail[c]) break;
            adv(1);
            continue;
        }
        const auto d = decodeAt(src_, byte_pos_);
        if (d.n == 0 || !isUnicodeId(d.cp)) break;
        adv(d.n);
    }
}

Token Scanner::scanIdent(size_t start_byte, i32 start_line, i32 start_col) {
    consumeIdentRest();
    const auto ident = src_.substr(start_byte, byte_pos_ - start_byte);
    const Kind kw = keywordKind(ident);
    return emit(kw == Kind::Invalid ? Kind::ID : kw, start_byte, start_line, start_col);
}

Token Scanner::scanNumber(size_t start_byte, i32 start_line, i32 start_col) {
    if (ch() == '0') {
        const unsigned char p = ch(1);
        if (p == 'b' && isBin(ch(2))) {
            adv(2);
            consumeGrouped(isBin);
            consumeIntSuffix();
            return emit(Kind::INT, start_byte, start_line, start_col);
        }
        if (p == 'o' && isOct(ch(2))) {
            adv(2);
            consumeGrouped(isOct);
            consumeIntSuffix();
            return emit(Kind::INT, start_byte, start_line, start_col);
        }
        if (p == 'x' && isHex(ch(2))) {
            adv(2);
            consumeGrouped(isHex);
            consumeIntSuffix();
            return emit(Kind::INT, start_byte, start_line, start_col);
        }
    }
    consumeInt10();
    if (ch() == '.' && isDigit(ch(1))) {
        adv(1);
        consumeInt10();
        if (ch() == 'e') {
            size_t i = 1;
            if (ch(1) == '-') i = 2;
            if (isDigit(ch(i))) {
                adv(i);
                consumeInt10();
            }
        }
        consumeFloatSuffix();
        return emit(Kind::FLOAT, start_byte, start_line, start_col);
    }
    if (consumeFloatSuffix()) return emit(Kind::FLOAT, start_byte, start_line, start_col);
    consumeIntSuffix();
    return emit(Kind::INT, start_byte, start_line, start_col);
}

Token Scanner::scanStrTpl() {
    const size_t start_byte = byte_pos_;
    const i32 start_line = line_;
    const i32 start_col = column_;
    if (atEnd()) {
        Token skip;
        skip.kind = Kind::Invalid;
        return skip;
    }
    if (ch() == '$' && ch(1) == '{') {
        adv(2);
        return emit(Kind::STR_TPL_INTERP_OPEN, start_byte, start_line, start_col);
    }
    if (ch() == '$') {
        const unsigned char n = ch(1);
        const bool id = (n < 0x80) ? kIdHead[n] : isUnicodeId(decodeAt(src_, byte_pos_ + 1).cp);
        if (id) {
            adv(1); // $
            if (n < 0x80) {
                adv(1);
            } else {
                adv(decodeAt(src_, byte_pos_).n);
            }
            consumeIdentRest();
            return emit(Kind::STR_TPL_DOLLAR_ID, start_byte, start_line, start_col);
        }
    }
    if (ch() == '"') {
        adv(1);
        return emit(Kind::STR_TPL_CLOSE, start_byte, start_line, start_col);
    }
    bool any = false;
    while (!atEnd()) {
        const unsigned char c = ch();
        if (c == '"' || c == '$' || c == '\r' || c == '\n') break;
        if (c == '\\') {
            if (ch(1) == 0 && byte_pos_ + 1 >= src_.size()) break;
            if (ch(1) == '\n') break;
            adv(2);
            any = true;
            continue;
        }
        adv(1);
        any = true;
    }
    if (!any) {
        return skipIllegal(start_byte, start_line, start_col);
    }
    return emit(Kind::STR_TPL_TEXT, start_byte, start_line, start_col);
}

Token Scanner::scanOne() {
    const size_t start_byte = byte_pos_;
    const i32 start_line = line_;
    const i32 start_col = column_;

    if (atEnd()) {
        if (mode_ == Mode::Default && src_.empty()) {
            Token t;
            t.kind = Kind::LineEnd;
            t.pos = Pos{.offset = 0, .end = 0, .line = 1, .column = 0};
            t.text = "<EOF>";
            return t;
        }
        Token skip;
        skip.kind = Kind::Invalid;
        return skip;
    }

    if (mode_ == Mode::StrTpl) return scanStrTpl();

    const unsigned char c = ch();

    if (c == 'r' && tryRawString()) return emit(Kind::STR_LINE_RAW, start_byte, start_line, start_col);
    if (c == 'c' && tryCodePoint()) return emit(Kind::CODE_POINT, start_byte, start_line, start_col);

    if (c < 0x80) {
        if (kIdHead[c]) {
            adv(1);
            return scanIdent(start_byte, start_line, start_col);
        }
    } else {
        const auto d = decodeAt(src_, byte_pos_);
        if (d.n != 0 && isUnicodeId(d.cp)) {
            adv(d.n);
            return scanIdent(start_byte, start_line, start_col);
        }
    }

    if (isDigit(c)) return scanNumber(start_byte, start_line, start_col);

    switch (c) {
    case '\n':
        consumeNewline();
        return emit(Kind::LineEnd, start_byte, start_line, start_col);
    case '\r':
        consumeNewline();
        return emit(Kind::LineEnd, start_byte, start_line, start_col);
    case '.':
        if (isDigit(ch(1))) {
            adv(1);
            consumeInt10();
            return emit(Kind::DOT_NUM, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolDot, start_byte, start_line, start_col);
    case '+':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolAddEq, start_byte, start_line, start_col);
        }
        if (isDigit(ch(1))) {
            adv(1);
            return scanNumber(start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolAdd, start_byte, start_line, start_col);
    case '-':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolSubEq, start_byte, start_line, start_col);
        }
        if (isDigit(ch(1))) {
            adv(1);
            return scanNumber(start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolSub, start_byte, start_line, start_col);
    case '*':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolMulEq, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolMul, start_byte, start_line, start_col);
    case '/':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolDivEq, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolDiv, start_byte, start_line, start_col);
    case '%':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolModEq, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolMod, start_byte, start_line, start_col);
    case '=':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolEqEq, start_byte, start_line, start_col);
        }
        if (ch(1) == '>') {
            adv(2);
            return emit(Kind::SymbolEqMt, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolEq, start_byte, start_line, start_col);
    case '!':
        if (ch(1) == '=') {
            adv(2);
            return emit(Kind::SymbolExclEq, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolExcl, start_byte, start_line, start_col);
    case '&':
        if (ch(1) == '&') {
            adv(2);
            return emit(Kind::SymbolAndAnd, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolAnd, start_byte, start_line, start_col);
    case '|':
        if (ch(1) == '|') {
            adv(2);
            return emit(Kind::SymbolOrOr, start_byte, start_line, start_col);
        }
        break;
    case '<':
        if (ch(1) == '-') {
            adv(2);
            return emit(Kind::SymbolLtSub, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolLt, start_byte, start_line, start_col);
    case '>':
        adv(1);
        return emit(Kind::SymbolMt, start_byte, start_line, start_col);
    case ':':
        if (ch(1) == ':') {
            adv(2);
            return emit(Kind::SymbolColonColon, start_byte, start_line, start_col);
        }
        adv(1);
        return emit(Kind::SymbolColon, start_byte, start_line, start_col);
    case '"':
        adv(1);
        return emit(Kind::STR_TPL_OPEN, start_byte, start_line, start_col);
    case '@':
        adv(1);
        return emit(Kind::SymbolAt, start_byte, start_line, start_col);
    case '#':
        adv(1);
        return emit(Kind::SymbolHash, start_byte, start_line, start_col);
    case '$':
        adv(1);
        return emit(Kind::SymbolThis, start_byte, start_line, start_col);
    case '?':
        adv(1);
        return emit(Kind::SymbolQuest, start_byte, start_line, start_col);
    case '~':
        adv(1);
        return emit(Kind::SymbolRev, start_byte, start_line, start_col);
    case ';':
        adv(1);
        return emit(Kind::SymbolSemicolon, start_byte, start_line, start_col);
    case ',':
        adv(1);
        return emit(Kind::SymbolComma, start_byte, start_line, start_col);
    case '(':
        adv(1);
        return emit(Kind::ParStart, start_byte, start_line, start_col);
    case ')':
        adv(1);
        return emit(Kind::ParEnd, start_byte, start_line, start_col);
    case '[':
        adv(1);
        return emit(Kind::GetStart, start_byte, start_line, start_col);
    case ']':
        adv(1);
        return emit(Kind::GetEnd, start_byte, start_line, start_col);
    case '{':
        adv(1);
        return emit(Kind::BlockStart, start_byte, start_line, start_col);
    case '}':
        adv(1);
        return emit(Kind::BlockEnd, start_byte, start_line, start_col);
    default:
        break;
    }

    return skipIllegal(start_byte, start_line, start_col);
}

Token Scanner::nextRaw() {
    if (hit_eof_) return makeEof();
    if (mode_ == Mode::Default && !atEnd()) {
        const unsigned char c = ch();
        const size_t start_byte = byte_pos_;
        const i32 start_line = line_;
        const i32 start_col = column_;
        if (c == ' ') {
            if (column_ == 0) {
                size_t i = 0;
                while (ch(i) == ' ')
                    ++i;
                if (ch(i) == ';') {
                    adv(i + 1);
                    skipLineComment();
                    return emit(Kind::LineComment, start_byte, start_line, start_col);
                }
            }
            size_t i = 0;
            while (ch(i) == ' ')
                ++i;
            if (ch(i) == ';') {
                adv(i + 1);
                skipLineEndComment();
                return emit(Kind::LineEndComment, start_byte, start_line, start_col);
            }
            adv(i);
            return emit(Kind::Space, start_byte, start_line, start_col);
        }
        if (c == ';' && column_ == 0) {
            adv(1);
            skipLineComment();
            return emit(Kind::LineComment, start_byte, start_line, start_col);
        }
    }
    if (atEnd()) {
        hit_eof_ = true;
        if (mode_ == Mode::Default && src_.empty()) {
            Token t;
            t.kind = Kind::LineEnd;
            t.pos = Pos{.offset = 0, .end = 0, .line = 1, .column = 0};
            t.text = "<EOF>";
            t.index = default_index_++;
            return t;
        }
        return makeEof();
    }
    Token t = scanOne();
    if (atEnd()) hit_eof_ = true;
    if (!isTrivia(t.kind) && t.kind != Kind::Invalid && t.kind != Kind::Eof) t.index = default_index_++;
    return t;
}

Token Scanner::next() {
    for (;;) {
        Token t = nextRaw();
        if (t.kind == Kind::Invalid) continue;
        if (isTrivia(t.kind)) continue;
        return t;
    }
}

} // namespace rd

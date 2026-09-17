// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/scanner.h"

#include <algorithm>
#include <array>
#include <utility>

namespace rd {
namespace {

struct Decoded {
    char32_t cp = 0;
    size_t n = 0; // 字节
};

Decoded decodeAt(std::string_view s, size_t off) {
    if (off >= s.size()) return {.cp = 0, .n = 0};
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
    return {.cp = c0, .n = 1}; // 非法 UTF-8：按 1 字节
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

// g4 ID_HEAD / ID_TAIL
bool isIdHead(char32_t cp) {
    if (isWhiteSpace(cp)) return false;
    if (cp >= 0x21 && cp <= 0x40) return false;
    if (cp >= 0x5B && cp <= 0x5E) return false;
    if (cp == 0x60) return false;
    if (cp >= 0x7B && cp <= 0x7F) return false;
    return true;
}

bool isIdTail(char32_t cp) {
    if (isWhiteSpace(cp)) return false;
    if (cp >= 0x21 && cp <= 0x2F) return false;
    if (cp >= 0x3A && cp <= 0x40) return false;
    if (cp >= 0x5B && cp <= 0x5E) return false;
    if (cp == 0x60) return false;
    if (cp >= 0x7B && cp <= 0x7F) return false;
    return true;
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
bool isBin(char c) {
    return c == '0' || c == '1';
}
bool isOct(char c) {
    return c >= '0' && c <= '7';
}
bool isHex(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool startsAt(std::string_view s, size_t off, char c) {
    return off < s.size() && s[off] == c;
}

bool startsAt(std::string_view s, size_t off, std::string_view lit) {
    return off <= s.size() && s.substr(off).starts_with(lit);
}

// [0-9]('_'?[0-9]+)*
size_t measureInt10(std::string_view s, size_t off) {
    if (off >= s.size() || !isDigit(s[off])) return 0;
    size_t i = off + 1;
    while (i < s.size()) {
        size_t j = i;
        if (s[j] == '_') ++j;
        if (j >= s.size() || !isDigit(s[j])) break;
        i = j + 1;
        while (i < s.size() && isDigit(s[i]))
            ++i;
    }
    return i - off;
}

size_t measureGrouped(std::string_view s, size_t off, bool (*ok)(char)) {
    if (off >= s.size() || !ok(s[off])) return 0;
    size_t i = off + 1;
    while (i < s.size()) {
        size_t j = i;
        if (s[j] == '_') ++j;
        if (j >= s.size() || !ok(s[j])) break;
        i = j + 1;
        while (i < s.size() && ok(s[i]))
            ++i;
    }
    return i - off;
}

size_t measureInt2(std::string_view s, size_t off) {
    if (!startsAt(s, off, "0b")) return 0;
    const size_t body = measureGrouped(s, off + 2, isBin);
    return body == 0 ? 0 : 2 + body;
}

size_t measureInt8(std::string_view s, size_t off) {
    if (!startsAt(s, off, "0o")) return 0;
    const size_t body = measureGrouped(s, off + 2, isOct);
    return body == 0 ? 0 : 2 + body;
}

size_t measureInt16(std::string_view s, size_t off) {
    if (!startsAt(s, off, "0x")) return 0;
    const size_t body = measureGrouped(s, off + 2, isHex);
    return body == 0 ? 0 : 2 + body;
}

size_t measureIntSuffix(std::string_view s, size_t off) {
    if (off >= s.size()) return 0;
    const char u = s[off];
    if (u != 'i' && u != 'u') return 0;
    const auto rest = s.substr(off + 1);
    if (rest.starts_with("size")) return 5;
    if (rest.starts_with("64") || rest.starts_with("32") || rest.starts_with("16")) return 3;
    if (!rest.empty() && rest[0] == '8') return 2;
    return 0;
}

size_t measureFloatSuffix(std::string_view s, size_t off) {
    if (startsAt(s, off, "f32") || startsAt(s, off, "f64")) return 3;
    return 0;
}

size_t measureDotNum(std::string_view s, size_t off) {
    if (!startsAt(s, off, '.')) return 0;
    const size_t d = measureInt10(s, off + 1);
    return d == 0 ? 0 : 1 + d;
}

size_t measureFloatDot(std::string_view s, size_t off) {
    const size_t a = measureInt10(s, off);
    if (a == 0) return 0;
    const size_t d = measureDotNum(s, off + a);
    return d == 0 ? 0 : a + d;
}

size_t measureFloatExp(std::string_view s, size_t off) {
    const size_t d = measureFloatDot(s, off);
    if (d == 0) return 0;
    size_t i = off + d;
    if (!startsAt(s, i, 'e')) return 0;
    ++i;
    if (startsAt(s, i, '-')) ++i;
    const size_t e = measureInt10(s, i);
    return e == 0 ? 0 : (i + e) - off;
}

size_t measureSign(std::string_view s, size_t off) {
    if (off < s.size() && (s[off] == '+' || s[off] == '-')) return 1;
    return 0;
}

size_t measureInt(std::string_view s, size_t off) {
    size_t i = off + measureSign(s, off);
    const size_t body = std::max({measureInt10(s, i), measureInt2(s, i), measureInt8(s, i), measureInt16(s, i)});
    if (body == 0) return 0;
    i += body;
    i += measureIntSuffix(s, i);
    return i - off;
}

size_t measureFloat(std::string_view s, size_t off) {
    size_t i = off + measureSign(s, off);
    const size_t body = std::max({measureFloatExp(s, i), measureFloatDot(s, i), measureInt10(s, i)});
    if (body == 0) return 0;
    i += body;
    i += measureFloatSuffix(s, i);
    return i - off;
}

size_t measureId(std::string_view s, size_t off) {
    auto d = decodeAt(s, off);
    if (d.n == 0 || !isIdHead(d.cp)) return 0;
    size_t i = off + d.n;
    for (;;) {
        d = decodeAt(s, i);
        if (d.n == 0 || !isIdTail(d.cp)) break;
        i += d.n;
    }
    return i - off;
}

size_t measureLineEndNl(std::string_view s, size_t off) {
    if (startsAt(s, off, "\r\n")) return 2;
    if (startsAt(s, off, '\n')) return 1;
    return 0;
}

// LineComment：col==0 时 Space* ';' ~[\r\n]* LineEnd（LineEnd 含 EOF）
size_t measureLineComment(std::string_view s, size_t off, i32 column) {
    if (column != 0) return 0;
    size_t i = off;
    while (startsAt(s, i, ' '))
        ++i;
    if (!startsAt(s, i, ';')) return 0;
    ++i;
    while (i < s.size() && s[i] != '\r' && s[i] != '\n')
        ++i;
    if (i >= s.size()) return i - off; // LineEnd = EOF
    const size_t nl = measureLineEndNl(s, i);
    if (nl == 0) return 0;
    return (i + nl) - off;
}

// LineEndComment：Space+ ';' ~[\r\n]*（不含换行）
size_t measureLineEndComment(std::string_view s, size_t off) {
    if (!startsAt(s, off, ' ')) return 0;
    size_t i = off;
    while (startsAt(s, i, ' '))
        ++i;
    if (!startsAt(s, i, ';')) return 0;
    ++i;
    while (i < s.size() && s[i] != '\r' && s[i] != '\n')
        ++i;
    return i - off;
}

size_t measureRawString(std::string_view s, size_t off) {
    if (!startsAt(s, off, "r\"")) return 0;
    size_t i = off + 2;
    while (i < s.size() && s[i] != '\r' && s[i] != '\n') {
        if (s[i] == '"') return (i + 1) - off;
        ++i;
    }
    return 0;
}

size_t measureCodePoint(std::string_view s, size_t off) {
    if (!startsAt(s, off, "c'")) return 0;
    size_t i = off + 2;
    auto d = decodeAt(s, i);
    if (d.n == 0) return 0;
    if (d.cp == '\\') {
        auto e = decodeAt(s, i + d.n);
        if (e.n == 0) return 0;
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
            return 0;
        }
    } else if (d.cp == '\r' || d.cp == '\n' || d.cp == '\\' || d.cp == '\'') {
        return 0;
    } else {
        i += d.n;
    }
    if (!startsAt(s, i, '\'')) return 0;
    return (i + 1) - off;
}

size_t measureTplText(std::string_view s, size_t off) {
    size_t i = off;
    bool any = false;
    while (i < s.size()) {
        auto d = decodeAt(s, i);
        if (d.n == 0) break;
        if (d.cp == '"' || d.cp == '$' || d.cp == '\r' || d.cp == '\n') break;
        if (d.cp == '\\') {
            auto e = decodeAt(s, i + d.n);
            if (e.n == 0 || e.cp == '\n') break; // ANTLR `.` 不含 \n
            i += d.n + e.n;
            any = true;
            continue;
        }
        i += d.n;
        any = true;
    }
    return any ? i - off : 0;
}

size_t measureDollarId(std::string_view s, size_t off) {
    if (!startsAt(s, off, '$')) return 0;
    const size_t id = measureId(s, off + 1);
    return id == 0 ? 0 : 1 + id;
}

bool isTrivia(Kind k) {
    return k == Kind::Space || k == Kind::LineComment || k == Kind::LineEndComment;
}

constexpr auto kOps = std::to_array<std::pair<std::string_view, Kind>>({
    {"+=", Kind::SymbolAddEq},  {"&&", Kind::SymbolAndAnd},   {"::", Kind::SymbolColonColon},
    {"/=", Kind::SymbolDivEq},  {"==", Kind::SymbolEqEq},     {"=>", Kind::SymbolEqMt},
    {"!=", Kind::SymbolExclEq}, {"<-", Kind::SymbolLtSub},    {"%=", Kind::SymbolModEq},
    {"*=", Kind::SymbolMulEq},  {"||", Kind::SymbolOrOr},     {"-=", Kind::SymbolSubEq},
    {"+", Kind::SymbolAdd},     {"&", Kind::SymbolAnd},       {"@", Kind::SymbolAt},
    {":", Kind::SymbolColon},   {",", Kind::SymbolComma},     {"/", Kind::SymbolDiv},
    {".", Kind::SymbolDot},     {"=", Kind::SymbolEq},        {"!", Kind::SymbolExcl},
    {"#", Kind::SymbolHash},    {"<", Kind::SymbolLt},        {"%", Kind::SymbolMod},
    {">", Kind::SymbolMt},      {"*", Kind::SymbolMul},       {"?", Kind::SymbolQuest},
    {"~", Kind::SymbolRev},     {";", Kind::SymbolSemicolon}, {"-", Kind::SymbolSub},
    {"$", Kind::SymbolThis},    {"(", Kind::ParStart},        {")", Kind::ParEnd},
    {"[", Kind::GetStart},      {"]", Kind::GetEnd},          {"{", Kind::BlockStart},
    {"}", Kind::BlockEnd},
});

} // namespace

Scanner::Scanner(std::string_view src) : src_(src) {}

Token Scanner::next() {
    if (hit_eof_) return makeEof();
    for (;;) {
        if (hit_eof_) return makeEof();
        Token t = scanOne();
        if (atEnd()) hit_eof_ = true;
        if (t.kind == Kind::Invalid) continue; // recover / 未匹配
        if (isTrivia(t.kind)) continue;
        return t;
    }
}

Token Scanner::makeEof() const {
    Token tok;
    tok.kind = Kind::Eof;
    tok.pos = Pos{.offset = cp_pos_, .end = cp_pos_, .line = line_, .column = column_};
    tok.text = {};
    return tok;
}

Token Scanner::emit(Kind kind, size_t start_byte, i32 start_cp, i32 start_line, i32 start_col) {
    Token t;
    t.kind = kind;
    t.pos = Pos{.offset = start_cp, .end = cp_pos_, .line = start_line, .column = start_col};
    t.text = src_.substr(start_byte, byte_pos_ - start_byte);
    return t;
}

void Scanner::consumeCp() {
    const auto d = decodeAt(src_, byte_pos_);
    if (d.n == 0) return;
    if (d.cp == U'\n') {
        ++line_;
        column_ = 0;
    } else {
        ++column_;
    }
    byte_pos_ += d.n;
    ++cp_pos_;
}

void Scanner::advanceBytes(size_t n) {
    const size_t target = byte_pos_ + n;
    while (byte_pos_ < target && byte_pos_ < src_.size())
        consumeCp();
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

Token Scanner::scanOne() {
    const size_t start_byte = byte_pos_;
    const i32 start_cp = cp_pos_;
    const i32 start_line = line_;
    const i32 start_col = column_;

    if (atEnd()) {
        // DEFAULT 的 LineEnd 含 EOF；StrTpl 没有，走 recover（不消费）
        if (mode_ == Mode::Default) {
            Token t;
            t.kind = Kind::LineEnd;
            t.pos = Pos{.offset = cp_pos_, .end = cp_pos_, .line = line_, .column = column_};
            t.text = "<EOF>";
            return t;
        }
        Token skip;
        skip.kind = Kind::Invalid;
        return skip; // StrTpl 在 EOF 无规则；next() 见 atEnd 置 hit_eof_
    }

    Kind best = Kind::Invalid;
    size_t best_n = 0;
    auto consider = [&](Kind k, size_t n) {
        if (n == 0) return;
        if (n > best_n) {
            best_n = n;
            best = k;
        }
    };

    if (mode_ == Mode::StrTpl) {
        consider(Kind::STR_TPL_INTERP_OPEN, startsAt(src_, byte_pos_, "${") ? 2 : 0);
        consider(Kind::STR_TPL_DOLLAR_ID, measureDollarId(src_, byte_pos_));
        consider(Kind::STR_TPL_CLOSE, startsAt(src_, byte_pos_, '"') ? 1 : 0);
        consider(Kind::STR_TPL_TEXT, measureTplText(src_, byte_pos_));
    } else {
        // 与 g4 规则顺序一致；同长保留先出现的
        consider(Kind::LineComment, measureLineComment(src_, byte_pos_, column_));
        consider(Kind::LineEndComment, measureLineEndComment(src_, byte_pos_));
        consider(Kind::Space, startsAt(src_, byte_pos_, ' ') ? 1 : 0);
        consider(Kind::LineEnd, measureLineEndNl(src_, byte_pos_));

        if (const size_t n = measureId(src_, byte_pos_)) {
            const auto ident = src_.substr(byte_pos_, n);
            const Kind kw = keywordKind(ident);
            consider(kw == Kind::Invalid ? Kind::ID : kw, n);
        }

        for (const auto& op : kOps) {
            if (startsAt(src_, byte_pos_, op.first)) consider(op.second, op.first.size());
        }

        consider(Kind::INT, measureInt(src_, byte_pos_));
        consider(Kind::FLOAT, measureFloat(src_, byte_pos_));
        consider(Kind::STR_TPL_OPEN, startsAt(src_, byte_pos_, '"') ? 1 : 0);
        consider(Kind::STR_LINE_RAW, measureRawString(src_, byte_pos_));
        consider(Kind::CODE_POINT, measureCodePoint(src_, byte_pos_));
        consider(Kind::INT_SUFFIX, measureIntSuffix(src_, byte_pos_));
        consider(Kind::INT_10, measureInt10(src_, byte_pos_));
        consider(Kind::INT_2, measureInt2(src_, byte_pos_));
        consider(Kind::INT_8, measureInt8(src_, byte_pos_));
        consider(Kind::INT_16, measureInt16(src_, byte_pos_));
        consider(Kind::FLOAT_SUFFIX, measureFloatSuffix(src_, byte_pos_));
        consider(Kind::FLOAT_DOT, measureFloatDot(src_, byte_pos_));
        consider(Kind::FLOAT_EXP, measureFloatExp(src_, byte_pos_));
        consider(Kind::NUN_SIGN, measureSign(src_, byte_pos_));
        consider(Kind::DOT_NUM, measureDotNum(src_, byte_pos_));
    }

    if (best_n == 0) {
        consumeCp(); // LexerNoViableAltException recover：跳过 1 个 code point
        Token skip;
        skip.kind = Kind::Invalid;
        return skip;
    }

    advanceBytes(best_n);
    applySideEffects(best);
    return emit(best, start_byte, start_cp, start_line, start_col);
}

} // namespace rd

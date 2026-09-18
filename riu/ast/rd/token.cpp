// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/token.h"

#include <array>
#include <string>
#include <utility>

namespace rd {
namespace {

constexpr std::array kKindNames{
#define RD_KIND_NAME(_name, dumpName) std::string_view{dumpName},
    RD_KIND_LIST(RD_KIND_NAME)
#undef RD_KIND_NAME
};

constexpr std::array kKeywords{
    std::pair{std::string_view{"Self"}, Kind::SelfType}, std::pair{std::string_view{"break"}, Kind::Break},
    std::pair{std::string_view{"catch"}, Kind::Catch},   std::pair{std::string_view{"continue"}, Kind::Continue},
    std::pair{std::string_view{"elif"}, Kind::Elif},     std::pair{std::string_view{"else"}, Kind::Else},
    std::pair{std::string_view{"enum"}, Kind::Enum},     std::pair{std::string_view{"extern"}, Kind::Extern},
    std::pair{std::string_view{"false"}, Kind::False},   std::pair{std::string_view{"fn"}, Kind::Fn},
    std::pair{std::string_view{"for"}, Kind::For},       std::pair{std::string_view{"if"}, Kind::If},
    std::pair{std::string_view{"in"}, Kind::In},         std::pair{std::string_view{"let"}, Kind::Let},
    std::pair{std::string_view{"loop"}, Kind::Loop},     std::pair{std::string_view{"match"}, Kind::Match},
    std::pair{std::string_view{"null"}, Kind::Null},     std::pair{std::string_view{"ret"}, Kind::Ret},
    std::pair{std::string_view{"struct"}, Kind::Struct}, std::pair{std::string_view{"true"}, Kind::True},
    std::pair{std::string_view{"try"}, Kind::Try},       std::pair{std::string_view{"type"}, Kind::TypeKw},
    std::pair{std::string_view{"use"}, Kind::Use},
};

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (unsigned char c : text) {
        switch (c) {
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        default:
            if (c < 0x20) {
                const auto u = static_cast<unsigned>(c);
                out += "\\x";
                out.push_back(kHex[u >> 4u]);
                out.push_back(kHex[u & 15u]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

} // namespace

std::string_view kindName(Kind k) {
    const auto i = static_cast<std::size_t>(k);
    if (i >= kKindNames.size()) return "INVALID";
    return kKindNames[i];
}

Kind keywordKind(std::string_view ident) {
    for (const auto& e : kKeywords) {
        if (e.first == ident) return e.second;
    }
    return Kind::Invalid;
}

std::string formatTokenLine(std::string_view kind, Pos pos, std::string_view text) {
    std::string out;
    out += kind;
    constexpr std::size_t kKindWidth = 22;
    if (kind.size() < kKindWidth) out.append(kKindWidth - kind.size(), ' ');
    out += ' ';
    out += std::to_string(pos.line);
    out += ':';
    out += std::to_string(pos.column);
    out += "  ";
    out += std::to_string(pos.offset);
    out += '-';
    out += std::to_string(pos.end);
    out += "  ";
    appendEscaped(out, text);
    out += '\n';
    return out;
}

std::string formatTokenLine(const Token& tok) {
    return formatTokenLine(kindName(tok.kind), tok.pos, tok.text);
}

} // namespace rd

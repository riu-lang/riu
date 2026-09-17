// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/flat.h"

namespace rd {
namespace {

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

void dumpNode(std::string& out, const FlatAst& ast, NodeId id, int indent) {
    const Node& n = ast.at(id);
    out.append(static_cast<std::size_t>(indent) * 2, ' ');
    out += nodeKindName(n.kind);
    if (!n.value.empty()) {
        out += ' ';
        appendEscaped(out, n.value);
    }
    if (n.op != Kind::Invalid) {
        out += " op=";
        out += kindName(n.op);
    }
    out += '\n';
    for (i32 i = 0; i < n.children_count; ++i) {
        const NodeId kid = ast.child(id, i);
        if (kid == kEmptyNode) continue;
        dumpNode(out, ast, kid, indent + 1);
    }
}

} // namespace

std::string dumpTree(const FlatAst& ast) {
    if (ast.root() == kEmptyNode) return {};
    std::string out;
    dumpNode(out, ast, ast.root(), 0);
    return out;
}

} // namespace rd

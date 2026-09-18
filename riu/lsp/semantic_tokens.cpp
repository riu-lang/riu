// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "semantic_tokens.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/rd/parser.h"
#include "position.h"
#include "types.h"
#include "utf8.h"

namespace riu::lsp {

namespace {

// 与 semanticTokenTypes() 数组顺序对应；改动需同步两边。
enum class TT : u8 {
    Keyword = 0,
    Operator = 1,
    String = 2,
    Number = 3,
    Comment = 4,
    Variable = 5,
    Class = 6,
    Function = 7,
    Property = 8,
    Parameter = 9,
    Method = 10,
    Metadata = 11,
    Interface = 12,
    Enum = 13,
    EnumMember = 14,
};

constexpr unsigned MOD_DECLARATION = 1u << 0u;

// 返回值 < 0 表示该 token 不参与高亮（空白、标点等）。
int classify(rd::Kind type) {
    switch (type) {
    case rd::Kind::Space:
    case rd::Kind::LineEnd:
    case rd::Kind::Invalid:
    case rd::Kind::Eof:
        return -1;

    case rd::Kind::LineComment:
    case rd::Kind::LineEndComment:
        return static_cast<int>(TT::Comment);

    case rd::Kind::Break:
    case rd::Kind::Catch:
    case rd::Kind::Continue:
    case rd::Kind::Elif:
    case rd::Kind::Else:
    case rd::Kind::Enum:
    case rd::Kind::Extern:
    case rd::Kind::False:
    case rd::Kind::Fn:
    case rd::Kind::For:
    case rd::Kind::If:
    case rd::Kind::In:
    case rd::Kind::Let:
    case rd::Kind::Loop:
    case rd::Kind::Match:
    case rd::Kind::Null:
    case rd::Kind::Ret:
    case rd::Kind::SelfType:
    case rd::Kind::Struct:
    case rd::Kind::True:
    case rd::Kind::Try:
    case rd::Kind::TypeKw:
    case rd::Kind::Use:
        return static_cast<int>(TT::Keyword);

    // 仅着色"真正的运算符"，逗号/分号/点/括号留默认色
    case rd::Kind::SymbolAdd:
    case rd::Kind::SymbolAnd:
    case rd::Kind::SymbolAt:
    case rd::Kind::SymbolDiv:
    case rd::Kind::SymbolEq:
    case rd::Kind::SymbolExcl:
    case rd::Kind::SymbolLt:
    case rd::Kind::SymbolMod:
    case rd::Kind::SymbolMt:
    case rd::Kind::SymbolMul:
    case rd::Kind::SymbolQuest:
    case rd::Kind::SymbolRev:
    case rd::Kind::SymbolSub:
        return static_cast<int>(TT::Operator);

    case rd::Kind::ID:
        return static_cast<int>(TT::Variable);

    case rd::Kind::INT:
    case rd::Kind::FLOAT:
    case rd::Kind::DOT_NUM:
        return static_cast<int>(TT::Number);

    case rd::Kind::STR_TPL_OPEN:
    case rd::Kind::STR_TPL_CLOSE:
    case rd::Kind::STR_TPL_TEXT:
    case rd::Kind::STR_TPL_DOLLAR_ID:
    case rd::Kind::STR_TPL_INTERP_OPEN:
    case rd::Kind::STR_LINE_RAW:
    case rd::Kind::CODE_POINT:
        return static_cast<int>(TT::String);

    default:
        return -1;
    }
}

int utf16Length(std::string_view s) {
    int len = 0;
    auto it = s.begin();
    auto end = s.end();
    while (it != end) {
        try {
            uint32_t cp = utf8::next(it, end);
            len += (cp > 0xFFFF) ? 2 : 1;
        } catch (...) {
            ++it;
            ++len;
        }
    }
    return len;
}

using OverrideEntry = std::pair<int, int>;
using OverrideMap = std::unordered_map<rd::i32, OverrideEntry>;

struct CollectState {
    OverrideMap overrides;
    std::vector<rd::Token> ids; // 按 offset 升序的 default ID
};

void putIndex(OverrideMap& m, rd::i32 index, TT kind, int mods = 0) {
    m[index] = {static_cast<int>(kind), mods};
}

bool inSpan(const rd::Pos& span, const rd::Token& t) {
    if (t.pos.offset < span.offset) return false;
    if (span.end > span.offset && t.pos.offset >= span.end) return false;
    return true;
}

void putAtOffset(CollectState& s, rd::i32 offset, TT kind, int mods = 0) {
    for (const auto& t : s.ids) {
        if (t.pos.offset == offset) {
            putIndex(s.overrides, t.index, kind, mods);
            return;
        }
        if (t.pos.offset > offset) return;
    }
}

void putNameInSpan(CollectState& s, const rd::Pos& span, std::string_view name, TT kind, int mods = 0) {
    if (name.empty()) return;
    for (const auto& t : s.ids) {
        if (!inSpan(span, t) || t.text != name) continue;
        if (s.overrides.contains(t.index)) continue;
        putIndex(s.overrides, t.index, kind, mods);
        return;
    }
}

void putIdsInSpan(CollectState& s, const rd::Pos& span, TT lastKind, int lastMods = 0, TT otherKind = TT::Class) {
    std::vector<rd::i32> idxs;
    for (const auto& t : s.ids) {
        if (inSpan(span, t)) idxs.push_back(t.index);
    }
    for (size_t i = 0; i < idxs.size(); ++i) {
        const bool last = i + 1 == idxs.size();
        putIndex(s.overrides, idxs[i], last ? lastKind : otherKind, last ? lastMods : 0);
    }
}

bool isUpperIdent(std::string_view v) {
    return !v.empty() && v[0] >= 'A' && v[0] <= 'Z';
}

void collectOverrides(const rd::FlatAst& ast, rd::NodeId id, CollectState& state) {
    if (id == rd::kEmptyNode) return;
    const rd::Node& n = ast.at(id);
    for (rd::i32 i = 0; i < n.children_count; ++i)
        collectOverrides(ast, ast.child(id, i), state);

    auto& out = state.overrides;

    switch (n.kind) {
    case rd::NodeKind::Struct: {
        bool isSpec = false;
        for (rd::i32 i = 0; i < n.children_count; ++i) {
            const rd::Node& c = ast.at(ast.child(id, i));
            if (c.kind == rd::NodeKind::Anno && c.value == "Spec") {
                isSpec = true;
                break;
            }
        }
        putNameInSpan(state, n.pos, n.value, isSpec ? TT::Interface : TT::Class, MOD_DECLARATION);
        break;
    }
    case rd::NodeKind::Fn:
        putNameInSpan(state, n.pos, n.value, TT::Function, MOD_DECLARATION);
        break;
    case rd::NodeKind::Anno:
        putNameInSpan(state, n.pos, n.value, TT::Metadata, 0);
        break;
    case rd::NodeKind::Field:
        putAtOffset(state, n.pos.offset, TT::Property, MOD_DECLARATION);
        break;
    case rd::NodeKind::Param:
        putAtOffset(state, n.pos.offset, TT::Parameter, MOD_DECLARATION);
        break;
    case rd::NodeKind::Dot:
        putAtOffset(state, n.pos.offset, TT::Property, 0);
        break;
    case rd::NodeKind::GetRef:
        for (rd::i32 i = 1; i < n.children_count; ++i) {
            const rd::Node& c = ast.at(ast.child(id, i));
            if (c.kind == rd::NodeKind::Ident) putAtOffset(state, c.pos.offset, TT::Property, 0);
        }
        break;
    case rd::NodeKind::StructLit:
        if (n.children_count > 0) {
            const rd::Node& lhs = ast.at(ast.child(id, 0));
            if (lhs.kind == rd::NodeKind::TypePath) putIdsInSpan(state, lhs.pos, TT::Class, 0);
        }
        break;
    case rd::NodeKind::FieldInit:
        putAtOffset(state, n.pos.offset, TT::Property, 0);
        break;
    case rd::NodeKind::StaticFieldSet:
        if (n.children_count > 0) {
            const rd::Node& path = ast.at(ast.child(id, 0));
            if (path.kind == rd::NodeKind::TypePath) putIdsInSpan(state, path.pos, TT::Class, 0);
        }
        putNameInSpan(state, n.pos, n.value, TT::Property, 0);
        // pos 是类型路径，字段名在 :: 之后：再按 value 找尚未覆盖的 ID
        if (!n.value.empty()) {
            for (const auto& t : state.ids) {
                if (t.text != n.value) continue;
                if (n.children_count > 0) {
                    const rd::Pos& pp = ast.at(ast.child(id, 0)).pos;
                    if (t.pos.offset < pp.end) continue;
                }
                if (out.contains(t.index)) continue;
                putIndex(out, t.index, TT::Property, 0);
                break;
            }
        }
        break;
    case rd::NodeKind::TypePath:
    case rd::NodeKind::TypeGeneric:
        putIdsInSpan(state, n.pos, TT::Class, 0);
        break;
    case rd::NodeKind::Alias:
        putNameInSpan(state, n.pos, n.value, TT::Class, MOD_DECLARATION);
        break;
    case rd::NodeKind::Enum:
        putNameInSpan(state, n.pos, n.value, TT::Enum, MOD_DECLARATION);
        break;
    case rd::NodeKind::EnumVariant:
        putAtOffset(state, n.pos.offset, TT::EnumMember, MOD_DECLARATION);
        break;
    case rd::NodeKind::EnumCtor: {
        if (n.children_count > 0) {
            const rd::Node& lhs = ast.at(ast.child(id, 0));
            if (lhs.kind == rd::NodeKind::TypePath) putIdsInSpan(state, lhs.pos, TT::Enum, 0);
        }
        putAtOffset(state, n.pos.offset, isUpperIdent(n.value) ? TT::EnumMember : TT::Function, 0);
        break;
    }
    case rd::NodeKind::Pattern: {
        rd::i32 i = 0;
        if (i < n.children_count) {
            const rd::Node& path = ast.at(ast.child(id, i));
            if (path.kind == rd::NodeKind::TypePath) {
                putIdsInSpan(state, path.pos, TT::Enum, 0);
                ++i;
            }
        }
        putNameInSpan(state, n.pos, n.value, TT::EnumMember, 0);
        // Pattern.pos 可能落在 variant 之后；variant 在 TypePath 与绑定之间
        if (!n.value.empty() && n.children_count > 0) {
            const rd::Node& first = ast.at(ast.child(id, 0));
            if (first.kind == rd::NodeKind::TypePath) {
                for (const auto& t : state.ids) {
                    if (t.text != n.value) continue;
                    if (t.pos.offset < first.pos.end) continue;
                    if (out.contains(t.index)) continue;
                    putIndex(out, t.index, TT::EnumMember, 0);
                    break;
                }
            }
        }
        for (; i < n.children_count; ++i) {
            const rd::Node& b = ast.at(ast.child(id, i));
            if (b.kind == rd::NodeKind::Ident) putAtOffset(state, b.pos.offset, TT::Parameter, MOD_DECLARATION);
        }
        break;
    }
    case rd::NodeKind::Catch:
        putNameInSpan(state, n.pos, n.value, TT::Parameter, MOD_DECLARATION);
        break;
    case rd::NodeKind::Call: {
        if (n.children_count <= 0) break;
        const rd::Node& left = ast.at(ast.child(id, 0));
        if (left.kind == rd::NodeKind::Dot) {
            putAtOffset(state, left.pos.offset, TT::Method, 0);
        } else if (left.kind == rd::NodeKind::Ident) {
            putAtOffset(state, left.pos.offset, TT::Function, 0);
        } else if (left.kind == rd::NodeKind::EnumCtor) {
            putAtOffset(state, left.pos.offset, isUpperIdent(left.value) ? TT::EnumMember : TT::Method, 0);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

const std::vector<std::string>& semanticTokenTypes() {
    static const std::vector<std::string> types = {
        "keyword",  "operator",  "string", "number",   "comment",   "variable", "class",      "function",
        "property", "parameter", "method", "metadata", "interface", "enum",     "enumMember",
    };
    return types;
}
const std::vector<std::string>& semanticTokenModifiers() {
    static const std::vector<std::string> modifiers = {"declaration"};
    return modifiers;
}

std::vector<int> computeSemanticTokens(std::string_view text) {
    std::vector<int> data;
    if (text.empty()) return data;

    std::vector<rd::Token> raw;
    {
        rd::Scanner sc(text);
        for (;;) {
            rd::Token t = sc.nextRaw();
            if (t.kind == rd::Kind::Eof) break;
            raw.push_back(t);
        }
    }

    CollectState state;
    for (const auto& t : raw) {
        if (t.kind == rd::Kind::ID) state.ids.push_back(t);
    }
    try {
        rd::ParseResult parsed = rd::parseProgram(text);
        if (parsed.ast.root() != rd::kEmptyNode) collectOverrides(parsed.ast, parsed.ast.root(), state);
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // 解析失败，沿用 lexer 默认着色
    }
    auto& overrides = state.overrides;

    int prevLine = 0;
    int prevChar = 0;

    for (const auto& tok : raw) {
        int kind = classify(tok.kind);
        int mods = 0;
        if (kind < 0) continue;

        if (tok.kind == rd::Kind::ID) {
            auto it = overrides.find(tok.index);
            if (it != overrides.end()) {
                kind = it->second.first;
                mods = it->second.second;
            }
        }

        if (tok.text.find('\n') != std::string_view::npos) continue;

        int length = utf16Length(tok.text);
        if (length <= 0) continue;

        LspPosition start = utf8OffsetToLsp(text, static_cast<size_t>(std::max(tok.pos.offset, 0)));

        int deltaLine = start.line - prevLine;
        int deltaChar = (deltaLine == 0) ? (start.character - prevChar) : start.character;
        if (deltaLine < 0 || deltaChar < 0) continue;

        data.push_back(deltaLine);
        data.push_back(deltaChar);
        data.push_back(length);
        data.push_back(kind);
        data.push_back(mods);

        prevLine = start.line;
        prevChar = start.character;
    }

    return data;
}

} // namespace riu::lsp

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "semantic_tokens.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "antlr4-runtime.h"
#include "position.h"
#include "utf8.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

namespace yux::lsp {

namespace {

// 与 semanticTokenTypes() 数组顺序对应；改动需同步两边。
enum class TT : int {
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
};

const std::vector<std::string> kTypes = {
    "keyword", "operator", "string", "number", "comment",
    "variable", "class", "function", "property", "parameter", "method", "metadata",
    "interface",
};
const std::vector<std::string> kModifiers = {
    "declaration",
};

constexpr int MOD_DECLARATION = 1 << 0;

// 返回值 < 0 表示该 token 不参与高亮（空白、标点等）。
int classify(size_t type) {
    using L = ::yux::yuxLexer;
    switch (type) {
        case L::Space:
        case L::LineEnd:
        case L::EmptyLine:
            return -1;

        case L::LineComment:
        case L::LineEndComment:
            return static_cast<int>(TT::Comment);

        // T__0 是匿名字面量 'cval'；其余为命名关键字
        case L::T__0:
        case L::Break: case L::DeclKey: case L::Draft: case L::Elif: case L::Else:
        case L::Extern: case L::False: case L::Fn: case L::If:
        case L::Loop: case L::Null: case L::Ret: case L::Struct:
        case L::True: case L::Use:
            return static_cast<int>(TT::Keyword);

        // 仅着色"真正的运算符"，逗号/分号/点/括号留默认色
        case L::SymbolAdd: case L::SymbolAnd: case L::SymbolDiv:
        case L::SymbolEq: case L::SymbolExcl: case L::SymbolLt:
        case L::SymbolMod: case L::SymbolMt: case L::SymbolMul:
        case L::SymbolOr: case L::SymbolQuest: case L::SymbolRev:
        case L::SymbolSub: case L::SymbolXor:
            return static_cast<int>(TT::Operator);

        case L::ID:
            return static_cast<int>(TT::Variable);

        case L::INT: case L::FLOAT:
        case L::INT_10: case L::INT_2: case L::INT_8: case L::INT_16:
        case L::INT_SUFFIX:
        case L::FLOAT_SUFFIX: case L::FLOAT_DOT: case L::FLOAT_EXP:
        case L::NUN_SIGN:
            return static_cast<int>(TT::Number);

        case L::STR_LINE: case L::STR_LINE_RAW: case L::CODE_POINT:
            return static_cast<int>(TT::String);

        default:
            return -1;
    }
}

// 计算 UTF-8 字符串的 UTF-16 code unit 数（LSP 协议默认列单位）。
int utf16Length(const std::string& s) {
    int len = 0;
    auto it = s.begin();
    while (it != s.end()) {
        try {
            uint32_t cp = utf8::next(it, s.end());
            len += (cp > 0xFFFF) ? 2 : 1;
        } catch (...) {
            ++it;
            ++len;
        }
    }
    return len;
}

// (kind, modifiers bitmask)
using OverrideEntry = std::pair<int, int>;
using OverrideMap = std::unordered_map<size_t, OverrideEntry>;

struct CollectState {
    OverrideMap overrides;
    std::unordered_set<std::string> structNames; // 用于识别构造函数 N(...) 这种调用
};

// 把 token 的索引登记为指定语义类型（只覆盖 ID 类 token，避免误标关键字）。
void put(OverrideMap& m, antlr4::Token* tok, TT kind, int mods = 0) {
    if (!tok) return;
    if (tok->getType() != ::yux::yuxLexer::ID) return;
    m[tok->getTokenIndex()] = {static_cast<int>(kind), mods};
}

// 后序遍历：子节点先处理。利用 program 顶层 structDecl/structImpl 出现在 fn 之前，
// 单遍即可在访问 ExprCall 时拿到完整的 structNames 集合。
void collectOverrides(antlr4::tree::ParseTree* node, CollectState& state) {
    if (!node) return;
    using P = ::yux::yuxParser;

    for (auto* child : node->children) {
        collectOverrides(child, state);
    }

    auto& out = state.overrides;

    if (auto* c = dynamic_cast<P::StructDeclContext*>(node)) {
        if (auto* st = c->structType(); st && st->name) {
            put(out, st->name, TT::Class, MOD_DECLARATION);
            state.structNames.insert(st->name->getText());
        }
    } else if (auto* c = dynamic_cast<P::DraftDeclContext*>(node)) {
        if (auto* dt = c->draftType(); dt && dt->name) {
            put(out, dt->name, TT::Interface, MOD_DECLARATION);
        }
    } else if (auto* c = dynamic_cast<P::DraftTypeContext*>(node)) {
        // 引用位置（structImpl 的实现列表）；声明位置已被 DraftDecl 分支覆盖
        if (c->name) put(out, c->name, TT::Interface, 0);
    } else if (auto* c = dynamic_cast<P::StructImplContext*>(node)) {
        if (auto* st = c->structType(); st && st->name) {
            // structImpl 的名字是对 struct 的引用，不是声明
            put(out, st->name, TT::Class, 0);
            state.structNames.insert(st->name->getText());
        }
    } else if (auto* c = dynamic_cast<P::FnHeaderContext*>(node)) {
        put(out, c->name, TT::Function, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::BuildAnnoContext*>(node)) {
        // 构建注解 #Name
        if (c->name) put(out, c->name, TT::Metadata, 0);
    } else if (auto* c = dynamic_cast<P::FiledDeclContext*>(node)) {
        put(out, c->name, TT::Property, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnParamStdContext*>(node)) {
        put(out, c->name, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnParamGroupContext*>(node)) {
        for (auto* tok : c->names) put(out, tok, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::ExprDotContext*>(node)) {
        // 默认全部当字段；如果整个 dot 是被调用的左部，下面 ExprCall 分支会把最后一个 member 改成 method
        for (auto* tok : c->member) put(out, tok, TT::Property, 0);
    } else if (auto* c = dynamic_cast<P::TypeNormalContext*>(node)) {
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    } else if (auto* c = dynamic_cast<P::TypeGenericContext*>(node)) {
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    } else if (auto* c = dynamic_cast<P::ExprCallContext*>(node)) {
        if (auto* dot = dynamic_cast<P::ExprDotContext*>(c->left)) {
            // x.y() / a.b.c() —— 末尾 member 改 method
            if (!dot->member.empty()) {
                auto* tok = dot->member.back();
                if (tok && tok->getType() == ::yux::yuxLexer::ID) {
                    out[tok->getTokenIndex()] = {static_cast<int>(TT::Method), 0};
                }
            }
        } else if (auto* litExpr = dynamic_cast<P::ExprLiteralContext*>(c->left)) {
            // foo() / N() —— 看 literalObj：若名字是已知 struct 则视为构造函数（method），否则普通函数调用
            if (auto* obj = dynamic_cast<P::LiteralObjContext*>(litExpr->literal())) {
                if (auto* tok = obj->name) {
                    bool isCtor = state.structNames.count(tok->getText()) > 0;
                    TT kind = isCtor ? TT::Method : TT::Function;
                    out[tok->getTokenIndex()] = {static_cast<int>(kind), 0};
                }
            }
        }
    }
}

} // namespace

const std::vector<std::string>& semanticTokenTypes() { return kTypes; }
const std::vector<std::string>& semanticTokenModifiers() { return kModifiers; }

std::vector<int> computeSemanticTokens(std::string_view text) {
    std::vector<int> data;
    if (text.empty()) return data;

    std::string textStr(text);
    antlr4::ANTLRInputStream input(textStr);
    ::yux::yuxLexer lexer(&input);
    lexer.removeErrorListeners();
    antlr4::CommonTokenStream tokens(&lexer);
    tokens.fill();

    // 解析阶段：失败时直接退化为纯 lexer 着色。
    CollectState state;
    try {
        ::yux::yuxParser parser(&tokens);
        parser.removeErrorListeners();
        auto* tree = parser.program();
        collectOverrides(tree, state);
    } catch (...) {
        // 解析失败，沿用 lexer 默认着色
    }
    auto& overrides = state.overrides;

    int prevLine = 0;
    int prevChar = 0;

    for (auto* tok : tokens.getTokens()) {
        if (!tok) continue;
        if (tok->getType() == antlr4::Token::EOF) break;

        int kind = classify(tok->getType());
        int mods = 0;
        if (kind < 0) continue;

        // ID 上下文覆盖（class/function/method/property/parameter + declaration modifier）
        if (tok->getType() == ::yux::yuxLexer::ID) {
            auto it = overrides.find(tok->getTokenIndex());
            if (it != overrides.end()) {
                kind = it->second.first;
                mods = it->second.second;
            }
        }

        const std::string& tt = tok->getText();
        // LSP 规定 token 不能跨行，跨行的暂时跳过（yux 字符串都是行内）
        if (tt.find('\n') != std::string::npos) continue;

        int length = utf16Length(tt);
        if (length <= 0) continue;

        LspPosition start = antlrToLsp(text, tok->getLine(), tok->getCharPositionInLine());

        int deltaLine = start.line - prevLine;
        int deltaChar = (deltaLine == 0) ? (start.character - prevChar) : start.character;
        if (deltaLine < 0 || deltaChar < 0) continue; // 防御：保证编码合法

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

} // namespace yux::lsp

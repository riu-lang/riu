// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "semantic_tokens.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../types.h"
#include "antlr4-runtime.h"
#include "position.h"
#include "utf8.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

namespace yux::lsp {

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

constexpr int MOD_DECLARATION = 1 << 0;

// 返回值 < 0 表示该 token 不参与高亮（空白、标点等）。
int classify(size_t type) {
    using L = ::yux::yuxLexer;
    switch (type) {
        case L::Space:
        case L::LineEnd:
            return -1;

        case L::LineComment:
        case L::LineEndComment:
            return static_cast<int>(TT::Comment);

        case L::Break: case L::Catch: case L::Elif: case L::Else:
        case L::Enum: case L::Extern: case L::False: case L::Fn: case L::If: case L::Let:
        case L::Loop: case L::Match: case L::Null: case L::Ret: case L::SelfType:
        case L::Struct: case L::True: case L::Try: case L::Use:
            return static_cast<int>(TT::Keyword);

        // 仅着色"真正的运算符"，逗号/分号/点/括号留默认色
        case L::SymbolAdd: case L::SymbolAnd: case L::SymbolAt: case L::SymbolDiv:
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

        // 字符串模板：开/闭引号、文本片段、$ident 一律按字符串高亮；
        // ${ ... } 内嵌表达式段落由 popMode 后的常规 token 接管，不在此处处理。
        case L::STR_TPL_OPEN: case L::STR_TPL_CLOSE: case L::STR_TPL_TEXT:
        case L::STR_TPL_DOLLAR_ID: case L::STR_TPL_INTERP_OPEN:
        case L::STR_LINE_RAW: case L::CODE_POINT:
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
};

// 把 token 的索引登记为指定语义类型（只覆盖 ID 类 token，避免误标关键字）。
void put(OverrideMap& m, antlr4::Token* tok, TT kind, int mods = 0) {
    if (!tok) return;
    if (tok->getType() != ::yux::yuxLexer::ID) return;
    m[tok->getTokenIndex()] = {static_cast<int>(kind), mods};
}

// 后序遍历：子节点先处理。Phase 6D 后 N(...) 同名 ctor 已被 E3130 拦截,
// 无需再额外维护 structNames 用于 ExprCall 着色。
void collectOverrides(antlr4::tree::ParseTree* node, CollectState& state) {
    if (!node) return;
    using P = ::yux::yuxParser;

    for (auto* child : node->children) {
        collectOverrides(child, state);
    }

    auto& out = state.overrides;

    if (auto* c = dynamic_cast<P::StructDeclContext*>(node)) {
        if (auto* st = c->structType(); st && st->name) {
            // spec-unify v1：#Spec 注解的 struct 渲染为 Interface（spec），否则 Class
            bool isSpec = false;
            for (auto* a : c->buildAnnos) {
                if (a->name && a->name->getText() == "Spec") { isSpec = true; break; }
            }
            put(out, st->name, isSpec ? TT::Interface : TT::Class, MOD_DECLARATION);
        }
    } else if (auto* c = dynamic_cast<P::FnHeaderContext*>(node)) {
        put(out, c->name, TT::Function, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::BuildAnnoContext*>(node)) {
        // 构建注解 #Name（顶行堆叠：fn/struct/extern 上）
        if (c->name) put(out, c->name, TT::Metadata, 0);
    } else if (auto* c = dynamic_cast<P::LetAnnoContext*>(node)) {
        // let 行内注解 #Mut / #Cval / #Frozen
        if (c->name) put(out, c->name, TT::Metadata, 0);
    } else if (auto* c = dynamic_cast<P::FiledDeclContext*>(node)) {
        put(out, c->name, TT::Property, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnParamStdContext*>(node)) {
        put(out, c->name, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnParamGroupContext*>(node)) {
        for (auto* tok : c->names) put(out, tok, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::LambdaParamStdContext*>(node)) {
        put(out, c->name, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::LambdaParamGroupContext*>(node)) {
        for (auto* tok : c->names) put(out, tok, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnTypeParamNamedContext*>(node)) {
        // fn 类型字面量参数名仅作文档（§3.4），但 IDE 仍按 Parameter 高亮
        put(out, c->name, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::FnTypeParamGroupContext*>(node)) {
        for (auto* tok : c->names) put(out, tok, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::ExprDotContext*>(node)) {
        // 默认全部当字段；如果整个 dot 是被调用的左部，下面 ExprCall 分支会把最后一个 member 改成 method
        for (auto* tok : c->member) put(out, tok, TT::Property, 0);
    } else if (auto* c = dynamic_cast<P::ExprGetRefContext*>(node)) {
        // &a.b.c —— subs 链上每个 ID 均为字段名
        for (auto* tok : c->subs) put(out, tok, TT::Property, 0);
    } else if (auto* c = dynamic_cast<P::ExprStructLitContext*>(node)) {
        // Self { .x = 1 .y = 2 } 或 Name { .x = 1 } —— 类型名按 Class，字段名按 Property
        if (c->typeName) put(out, c->typeName, TT::Class, 0);
        for (auto* fi : c->fieldInits) {
            if (fi && fi->name) put(out, fi->name, TT::Property, 0);
        }
    } else if (auto* c = dynamic_cast<P::StatementStaticFieldSetContext*>(node)) {
        // Type::FIELD = expr —— 类型名按 Class，字段名按 Property
        if (c->typeName) put(out, c->typeName, TT::Class, 0);
        if (c->fieldName) put(out, c->fieldName, TT::Property, 0);
    } else if (auto* c = dynamic_cast<P::TypeNormalContext*>(node)) {
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    } else if (auto* c = dynamic_cast<P::TypeGenericContext*>(node)) {
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    } else if (auto* c = dynamic_cast<P::TypeNormalWithRefContext*>(node)) {
        // 函数参数 / 返回类型的 typeWithRef 入口；之前漏覆盖导致 i32 等被当 Variable
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    } else if (auto* c = dynamic_cast<P::TypeGenericWithRefContext*>(node)) {
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, 0);
    // TypeNullable / TypeArray / TypeTuple / TypeFn 及对应 WithRef 变体本身不持有
    // 顶层 ID（仅是结构容器），其内部嵌套的 type / typeWithRef 由父级遍历递归覆盖
    } else if (auto* c = dynamic_cast<P::AliasDeclContext*>(node)) {
        // 类型别名 `Name = T` / `Pair<T> = (T, T)`：左侧名字按用户类型染色
        if (auto* term = c->ID()) put(out, term->getSymbol(), TT::Class, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::EnumDeclContext*>(node)) {
        // enum E { ... } 的 E 染成枚举名
        if (c->name) put(out, c->name, TT::Enum, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::EnumVariantContext*>(node)) {
        // 一行一个 variant 名，染成 enumMember
        if (c->name) put(out, c->name, TT::EnumMember, MOD_DECLARATION);
        // payloads 是 type 列表，递归覆盖
    } else if (auto* c = dynamic_cast<P::ExprEnumCtorContext*>(node)) {
        // E::V / T::foo：枚举构造与静态函数调用共用此节点。
        // 枚举 variant 按惯例大写开头（PascalCase），函数/方法小写开头（camelCase）；
        // 语法层以此启发式分流着色。
        if (c->enumName) put(out, c->enumName, TT::Enum, 0);
        if (c->variant) {
            std::string vName = c->variant->getText();
            bool isUpper = !vName.empty() && (vName[0] >= 'A' && vName[0] <= 'Z');
            put(out, c->variant, isUpper ? TT::EnumMember : TT::Function, 0);
        }
    } else if (auto* c = dynamic_cast<P::PatternEnumContext*>(node)) {
        // match 模式 E::V(a, b)：a/b 是新引入绑定，按 Parameter 染色
        if (c->enumName) put(out, c->enumName, TT::Enum, 0);
        if (c->variant) put(out, c->variant, TT::EnumMember, 0);
        for (auto* tok : c->binds) put(out, tok, TT::Parameter, MOD_DECLARATION);
    } else if (auto* c = dynamic_cast<P::CatchArmContext*>(node)) {
        // catch err T { ... } 中的 err 是绑定参数；T 走 type 递归
        if (c->err) put(out, c->err, TT::Parameter, MOD_DECLARATION);
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
            // foo() —— 普通函数调用; Phase 6D 后同名 ctor `Foo(...)` 已被 E3130 拦截,
            // struct 名作 callee 已非构造形态, 这里一律按 Function 着色.
            if (auto* obj = dynamic_cast<P::LiteralObjContext*>(litExpr->literal())) {
                if (auto* tok = obj->name) {
                    out[tok->getTokenIndex()] = {static_cast<int>(TT::Function), 0};
                }
            }
        } else if (auto* enumCtor = dynamic_cast<P::ExprEnumCtorContext*>(c->left)) {
            // T::foo() 静态函数/方法调用 —— variant 覆盖为 Method；
            // 仅当首字母大写时才认定为枚举构造，已由 ExprEnumCtor 分支着色
            if (enumCtor->variant) {
                std::string vName = enumCtor->variant->getText();
                bool isUpper = !vName.empty() && (vName[0] >= 'A' && vName[0] <= 'Z');
                out[enumCtor->variant->getTokenIndex()] = {
                    static_cast<int>(isUpper ? TT::EnumMember : TT::Method), 0};
            }
        }
    }
}

} // namespace

const std::vector<std::string>& semanticTokenTypes() {
    static const std::vector<std::string> types = {
        "keyword", "operator", "string", "number", "comment",
        "variable", "class", "function", "property", "parameter", "method", "metadata",
        "interface", "enum", "enumMember",
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
    } catch (...) { // NOLINT(bugprone-empty-catch)
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

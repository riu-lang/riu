// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 文档解析实现
//
// 流程:
//   text -> rd::parseProgram
//   ParseError → SyntaxDiag → Diagnostic（E1001/E1002）
//   FlatAst Program 顶层 fn / struct / let 生成 DocumentSymbol

#include "document.h"

#include "ast/rd/parser.h"
#include "ast/syntax_diag.h"
#include "position.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace riu::lsp {

namespace {

LspPosition tokStart(const std::string& text, const rd::Token& tok) {
    return utf8OffsetToLsp(text, static_cast<size_t>(std::max(tok.pos.offset, 0)));
}

LspPosition tokEnd(const std::string& text, const rd::Token& tok) {
    const auto end = tok.pos.end > tok.pos.offset ? tok.pos.end : tok.pos.offset;
    return utf8OffsetToLsp(text, static_cast<size_t>(std::max(end, 0)));
}

LspPosition posStart(const std::string& text, const rd::Pos& pos) {
    return utf8OffsetToLsp(text, static_cast<size_t>(std::max(pos.offset, 0)));
}

LspPosition posEnd(const std::string& text, const rd::Pos& pos) {
    rd::i32 end = pos.end;
    if (end <= pos.offset) end = pos.offset + 1;
    if (end < 0) end = 0;
    return utf8OffsetToLsp(text, static_cast<size_t>(end));
}

std::vector<rd::Token> scanDefaultIds(std::string_view src) {
    std::vector<rd::Token> out;
    rd::Scanner sc(src);
    for (;;) {
        rd::Token t = sc.next();
        if (t.kind == rd::Kind::Eof) break;
        out.push_back(t);
    }
    return out;
}

// 关键字之后第一个匹配 name 的 ID（跳过前导 #Anno）。
const rd::Token* nameAfterKw(const std::vector<rd::Token>& toks, const rd::Pos& span, rd::Kind kw,
                             std::string_view name) {
    bool seenKw = false;
    for (const auto& t : toks) {
        if (t.pos.offset < span.offset) continue;
        if (span.end > span.offset && t.pos.offset >= span.end) break;
        if (!seenKw) {
            if (t.kind == kw) seenKw = true;
            continue;
        }
        if (t.kind == rd::Kind::ID && t.text == name) return &t;
    }
    for (const auto& t : toks) {
        if (t.kind != rd::Kind::ID || t.text != name) continue;
        if (t.pos.offset < span.offset) continue;
        if (span.end > span.offset && t.pos.offset >= span.end) continue;
        return &t;
    }
    return nullptr;
}

void collectSymbols(const std::string& text, const rd::FlatAst& ast, const std::vector<rd::Token>& toks,
                    std::vector<DocSymbol>& out) {
    const rd::NodeId root = ast.root();
    if (root == rd::kEmptyNode) return;
    const rd::Node& prog = ast.at(root);
    for (rd::i32 i = 0; i < prog.children_count; ++i) {
        const rd::NodeId id = ast.child(root, i);
        if (id == rd::kEmptyNode) continue;
        const rd::Node& n = ast.at(id);
        DocSymbol s;
        rd::Kind kw = rd::Kind::Invalid;
        switch (n.kind) {
        case rd::NodeKind::Fn:
            if (n.value.empty()) continue;
            s.kind = SymbolKind::Function;
            kw = rd::Kind::Fn;
            break;
        case rd::NodeKind::Struct:
            if (n.value.empty()) continue;
            s.kind = SymbolKind::Struct;
            kw = rd::Kind::Struct;
            break;
        case rd::NodeKind::Let:
            if (n.value.empty()) continue;
            s.kind = SymbolKind::Constant;
            kw = rd::Kind::Let;
            break;
        default:
            continue;
        }
        s.name = std::string(n.value);
        s.rangeStart = posStart(text, n.pos);
        s.rangeEnd = posEnd(text, n.pos);
        if (const rd::Token* nameTok = nameAfterKw(toks, n.pos, kw, n.value)) {
            s.selStart = tokStart(text, *nameTok);
            s.selEnd = tokEnd(text, *nameTok);
        } else {
            s.selStart = s.rangeStart;
            s.selEnd = s.rangeEnd;
        }
        out.push_back(std::move(s));
    }
}

} // namespace

Document::Document(std::string uri, int version, std::string text)
    : _uri(std::move(uri)), _version(version), _text(std::move(text)) {}

Document::~Document() = default;

void Document::update(int version, std::string text) {
    _version = version;
    _text = std::move(text);
    _dirty = true;
}

bool Document::parseIfDirty() {
    if (!_dirty) return false;
    _dirty = false;
    _diagnostics.clear();
    _symbols.clear();

    try {
        rd::ParseResult parsed = rd::parseProgram(_text);
        for (const auto& e : parsed.errors) {
            SyntaxDiag in;
            in.is_lexer = e.is_lexer;
            in.line = e.pos.line;
            in.col = e.pos.column + 1;
            in.message = e.message;
            in.offending = e.offending;
            in.prev_text = e.prev_text;
            ::Diagnostic filled;
            if (!fillSyntaxDiagnostic(filled, in)) continue;
            Diagnostic d;
            d.severity = Severity::Error;
            d.code = filled.code;
            d.message = filled.message;
            for (const auto& h : filled.hints) {
                d.message += '\n';
                d.message += h;
            }
            d.start = posStart(_text, e.pos);
            d.end = posEnd(_text, e.pos);
            if (d.end.line < d.start.line || (d.end.line == d.start.line && d.end.character <= d.start.character)) {
                d.end = d.start;
                d.end.character += 1;
            }
            _diagnostics.push_back(std::move(d));
        }
        auto toks = scanDefaultIds(_text);
        collectSymbols(_text, parsed.ast, toks, _symbols);
    } catch (const std::exception& e) {
        Diagnostic d;
        d.severity = Severity::Error;
        d.message = std::string("internal parse error: ") + e.what();
        d.start = {.line = 0, .character = 0};
        d.end = {.line = 0, .character = 1};
        _diagnostics.push_back(std::move(d));
    }
    return true;
}

Document* DocumentManager::open(const std::string& uri, int version, std::string text) {
    auto doc = std::make_unique<Document>(uri, version, std::move(text));
    auto* raw = doc.get();
    _docs[uri] = std::move(doc);
    return raw;
}

Document* DocumentManager::update(const std::string& uri, int version, std::string text) {
    auto it = _docs.find(uri);
    if (it == _docs.end()) {
        return open(uri, version, std::move(text));
    }
    it->second->update(version, std::move(text));
    return it->second.get();
}

void DocumentManager::close(const std::string& uri) {
    _docs.erase(uri);
}

Document* DocumentManager::get(const std::string& uri) {
    auto it = _docs.find(uri);
    if (it == _docs.end()) return nullptr;
    return it->second.get();
}

} // namespace riu::lsp

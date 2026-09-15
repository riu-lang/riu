// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 文档解析实现
//
// 流程:
//   text -> ANTLRInputStream -> riuLexer -> CommonTokenStream -> riuParser
//   错误监听器收集 syntaxError(line, col, msg) -> Diagnostic
//   解析成功后遍历 ProgramContext 的 fn / structDecl / structImpl / letGlobal
//   生成 DocumentSymbol（name + range + selectionRange）

#include "document.h"

#include "antlr4-runtime.h"
#include "riu/riuLexer.h"
#include "riu/riuParser.h"
#include "utf8.h"

#include <utility>

namespace riu::lsp {

namespace {

// 计算 UTF-8 字符串的 code point 数。utf8::distance 抛异常时回退按字节数。
size_t codePointCount(const std::string& s) {
    try {
        return utf8::distance(s.begin(), s.end());
    } catch (...) {
        return s.size();
    }
}

// 把 (line, codePointCol) 范围转 LspPosition；end 由 (line, col + cpLen(text)) 求得。
// stopText 是该 token 的源码文本，用于计算右端点的 code point 偏移。
LspPosition tokenStartPos(const std::string& text, antlr4::Token* tok) {
    if (!tok) return {};
    return antlrToLsp(text, tok->getLine(), tok->getCharPositionInLine());
}

LspPosition tokenEndPos(const std::string& text, antlr4::Token* tok) {
    if (!tok) return {};
    // 注意: token 文本可能跨行（多行字符串等）；P1 只用于标识符和关键字 token，
    // 不做跨行兜底。后续如果遇到多行 token，需要按 \n 切分重算列。
    const std::string& t = tok->getText();
    size_t cpLen = codePointCount(t);
    return antlrToLsp(text, tok->getLine(), tok->getCharPositionInLine() + cpLen);
}

// 错误监听器：把 ANTLR 的 syntaxError 转为 Diagnostic。
class DiagnosticListener : public antlr4::BaseErrorListener {
public:
    DiagnosticListener(const std::string& docText, std::vector<Diagnostic>& out) : _text(docText), _out(out) {}

    void syntaxError(antlr4::Recognizer* /*recognizer*/, antlr4::Token* offending, size_t line,
                     size_t charPositionInLine, const std::string& msg, std::exception_ptr /*e*/) override {
        Diagnostic d;
        d.severity = Severity::Error;
        d.message = msg;
        d.start = antlrToLsp(_text, line, charPositionInLine);
        // end: 优先用 offending token 的右端；否则同 start
        if (offending) {
            d.end = tokenEndPos(_text, offending);
            if (d.end.line < d.start.line || (d.end.line == d.start.line && d.end.character < d.start.character)) {
                d.end = d.start;
            }
        } else {
            d.end = d.start;
            d.end.character += 1; // 给个最小宽度，编辑器才能高亮
        }
        _out.push_back(std::move(d));
    }

private:
    const std::string& _text;
    std::vector<Diagnostic>& _out;
};

// 把整个 ParserRuleContext 的范围转 LSP 范围（用 start/stop token 边界）
void contextRange(const std::string& text, antlr4::ParserRuleContext* ctx, LspPosition& start, LspPosition& end) {
    auto* a = ctx->getStart();
    auto* b = ctx->getStop();
    start = tokenStartPos(text, a);
    end = b ? tokenEndPos(text, b) : tokenEndPos(text, a);
}

void collectSymbols(const std::string& text, ::riu::riuParser::ProgramContext* prog, std::vector<DocSymbol>& out) {
    if (!prog) return;

    for (auto* fn : prog->fn()) {
        auto* hdr = fn->fnHeader();
        if (!hdr || !hdr->name) continue;
        DocSymbol s;
        s.name = hdr->name->getText();
        s.kind = SymbolKind::Function;
        contextRange(text, fn, s.rangeStart, s.rangeEnd);
        s.selStart = tokenStartPos(text, hdr->name);
        s.selEnd = tokenEndPos(text, hdr->name);
        out.push_back(std::move(s));
    }

    for (auto* sd : prog->structDecl()) {
        auto* st = sd->structType();
        if (!st || !st->name) continue;
        DocSymbol s;
        s.name = st->name->getText();
        s.kind = SymbolKind::Struct;
        contextRange(text, sd, s.rangeStart, s.rangeEnd);
        s.selStart = tokenStartPos(text, st->name);
        s.selEnd = tokenEndPos(text, st->name);
        out.push_back(std::move(s));
    }

    // spec-unify v1：structImpl 产生式已删，方法段合并进 structDecl —— 上面的循环已覆盖。

    for (auto* lg : prog->letGlobal()) {
        if (!lg->name) continue;
        DocSymbol s;
        s.name = lg->name->getText();
        s.kind = SymbolKind::Constant;
        contextRange(text, lg, s.rangeStart, s.rangeEnd);
        s.selStart = tokenStartPos(text, lg->name);
        s.selEnd = tokenEndPos(text, lg->name);
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
        antlr4::ANTLRInputStream input(_text);
        ::riu::riuLexer lexer(&input);
        DiagnosticListener listener(_text, _diagnostics);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&listener);

        antlr4::CommonTokenStream tokens(&lexer);
        ::riu::riuParser parser(&tokens);
        parser.removeErrorListeners();
        parser.addErrorListener(&listener);

        auto* prog = parser.program();
        // 即使有语法错误，parse 树也可能部分可用
        collectSymbols(_text, prog, _symbols);
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

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 文档模型
//
// 维护 uri -> {version, text}，按需把文本喂给 yuxLexer/yuxParser 产出
// 诊断（语法错误）与文档符号（fn / struct / letGlobal）。
//
// 设计取舍：
// - P1 只跑 lexer + parser，不跑 ASTBuilder。原因：ASTBuilder 在 visitImports
//   时调用 Yux::loadModule 会触发跨文件解析（依赖项目根、yux.toml 等），与
//   单文件 LSP 的"轻量解析-即时反馈"模型不匹配。语义诊断与跨文件能力留给 P2。
// - 解析结果（lexer/parser/tokenStream）需要在符号收集后还活着；用 unique_ptr
//   全部拴在 Document 上，并保留 ProgramContext* 的弱引用。

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "position.h"
#include "types.h"

namespace antlr4 {
class ANTLRInputStream;
class CommonTokenStream;
} // namespace antlr4

namespace yux {
class yuxLexer;
class yuxParser;
namespace yuxParserNS {} // namespace yuxParserNS
} // namespace yux

namespace yux::lsp {

// LSP DiagnosticSeverity
enum class Severity : u8 {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct Diagnostic {
    LspPosition start;
    LspPosition end;
    Severity severity = Severity::Error;
    std::string message;
};

// LSP SymbolKind 子集（参见 lsp 规范）
enum class SymbolKind : u8 {
    Function = 12,
    Struct = 23,
    Constant = 14,
};

struct DocSymbol {
    std::string name;
    SymbolKind kind;
    LspPosition rangeStart; // 整个声明范围
    LspPosition rangeEnd;
    LspPosition selStart; // 名字范围（用于 selectionRange）
    LspPosition selEnd;
};

class Document {
public:
    Document(std::string uri, int version, std::string text);
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    void update(int version, std::string text);

    // 触发解析；幂等。返回是否真的重新解析（脏才解析）。
    bool parseIfDirty();

    [[nodiscard]] int version() const { return _version; }
    [[nodiscard]] const std::string& uri() const { return _uri; }
    [[nodiscard]] const std::string& text() const { return _text; }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return _diagnostics; }
    [[nodiscard]] const std::vector<DocSymbol>& symbols() const { return _symbols; }

private:
    std::string _uri;
    int _version;
    std::string _text;
    bool _dirty = true;

    std::vector<Diagnostic> _diagnostics;
    std::vector<DocSymbol> _symbols;
};

class DocumentManager {
public:
    Document* open(const std::string& uri, int version, std::string text);
    Document* update(const std::string& uri, int version, std::string text);
    void close(const std::string& uri);
    Document* get(const std::string& uri);

private:
    std::unordered_map<std::string, std::unique_ptr<Document>> _docs;
};

} // namespace yux::lsp

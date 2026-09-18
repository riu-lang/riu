// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 文档模型
//
// 维护 uri -> {version, text}，按需把文本喂给 rd Scanner/Parser 产出
// 诊断（E1001/E1002）与文档符号（顶层 fn / struct / let）。
//
// 设计取舍：
// - P1 只跑词法 + 语法 + FlatAst，不跑 RdBuilder。RdBuilder 会 loadModule
//   展开 import（依赖项目根、riu.toml），与单文件「轻量解析-即时反馈」不匹配。
//   语义诊断与跨文件能力留给 P2（Workspace / Project）。

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "position.h"
#include "types.h"

namespace riu::lsp {

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
    std::string code; // E1001 / E1002；可空
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

} // namespace riu::lsp

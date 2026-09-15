// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 位置换算
//
// LSP 协议默认使用 UTF-16 code unit 计列；ANTLR 的 token 行/列
// 在 C++ 运行时下是按 code point 计的（ANTLRInputStream 内部存 UTF-32）。
// 本文件把 (ANTLR 行, ANTLR codepoint 列) 换算为 LSP (line, character) 对。

#pragma once

#include <cstddef>
#include <string_view>

namespace riu::lsp {

// LSP Position：line/character 都是 0-based。character 单位为 UTF-16 code unit。
struct LspPosition {
    int line = 0;
    int character = 0;
};

// 把 ANTLR 风格的 (1-based 行, 0-based 代码点列) 换算为 LSP (0-based 行, UTF-16 code unit 列)。
// `text` 必须为 UTF-8。
// 行号超出范围 → line 截断到最后一行；列超出该行 → character 截断到行末。
LspPosition antlrToLsp(std::string_view text, size_t antlrLine, size_t antlrCharPosInLine);

} // namespace riu::lsp

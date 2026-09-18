// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 位置换算
//
// LSP 协议默认使用 UTF-16 code unit 计列；rd Pos.offset / TokenInfo
// startIndex 是 UTF-8 字节。本文件把字节偏移换成 LSP (line, character)。

#pragma once

#include <cstddef>
#include <string_view>

namespace riu::lsp {

// LSP Position：line/character 都是 0-based。character 单位为 UTF-16 code unit。
struct LspPosition {
    int line = 0;
    int character = 0;
};

// UTF-8 字节偏移 → LSP。越界截到文末；空文本返回 {0,0}。
LspPosition utf8OffsetToLsp(std::string_view text, size_t byteOffset);

} // namespace riu::lsp

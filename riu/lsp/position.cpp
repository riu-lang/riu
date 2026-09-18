// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 位置换算实现
//
// 从文件头扫到目标字节：遇 \n 切行；行内把已走过的 UTF-8 解码成 UTF-16
// code unit（> U+FFFF 计 2）。每次调用重扫；P1 文档小，不做行索引。

#include "position.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "utf8.h"

namespace riu::lsp {

LspPosition utf8OffsetToLsp(std::string_view text, size_t byteOffset) {
    LspPosition out;
    if (text.empty()) return out;

    const size_t cap = std::min(byteOffset, text.size());
    size_t lineStart = 0;
    int line = 0;
    for (size_t i = 0; i < cap; ++i) {
        if (text[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    out.line = line;

    int utf16 = 0;
    auto it = text.begin() + static_cast<std::ptrdiff_t>(lineStart);
    auto end = text.begin() + static_cast<std::ptrdiff_t>(cap);
    try {
        while (it < end) {
            char32_t cp = utf8::next(it, end);
            utf16 += (cp <= 0xFFFF ? 1 : 2);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // 残缺 UTF-8：停在出错处
    }
    out.character = utf16;
    return out;
}

} // namespace riu::lsp

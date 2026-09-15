// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 位置换算实现
//
// 算法：
// 1. 在 UTF-8 文本中按字节扫描，遇到 \n 切行；预扫到目标行的起始字节偏移
// 2. 从行首向后解码 N 个 code point，累加 UTF-16 code unit 数量
//    - cp <= 0xFFFF → 1 个 code unit
//    - cp >  0xFFFF → 2 个 code unit (surrogate pair)
//
// 为简单起见，每次调用都重扫；若 P1 后期诊断量大成为热点，再做行索引缓存。

#include "position.h"

#include <cstddef>
#include <string_view>

#include "utf8.h"

namespace riu::lsp {

namespace {

// 在 utf8 文本里跳过 N 行，返回第 N 行（0-based）的起始迭代器。
// 越界则返回 end()。
const char* findLineStart(std::string_view text, size_t targetLineZeroBased) {
    if (targetLineZeroBased == 0) return text.data();
    size_t line = 0;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        if (*p == '\n') {
            ++line;
            if (line == targetLineZeroBased) return p + 1;
        }
        ++p;
    }
    return end;
}

} // namespace

LspPosition antlrToLsp(std::string_view text, size_t antlrLine, size_t antlrCharPosInLine) {
    // ANTLR 行号 1-based；保护 0 也按第 0 行处理
    size_t lineZero = antlrLine == 0 ? 0 : antlrLine - 1;

    LspPosition out;
    out.line = static_cast<int>(lineZero);

    const char* lineStart = findLineStart(text, lineZero);
    const char* end = text.data() + text.size();
    if (lineStart >= end) {
        out.character = 0;
        return out;
    }

    // 行末：下一个 \n 或 EOF
    const char* lineEnd = lineStart;
    while (lineEnd < end && *lineEnd != '\n')
        ++lineEnd;
    // 去掉 \r
    if (lineEnd > lineStart && *(lineEnd - 1) == '\r') --lineEnd;

    int utf16Col = 0;
    size_t cpCount = 0;
    const char* it = lineStart;
    while (it < lineEnd && cpCount < antlrCharPosInLine) {
        char32_t cp = utf8::next(it, lineEnd);
        utf16Col += (cp <= 0xFFFF ? 1 : 2);
        ++cpCount;
    }
    out.character = utf16Col;
    return out;
}

} // namespace riu::lsp

// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "primitives.h"

// 源码位置：line 为 1-based 行号，col 为 1-based 列号。col == 0 表示未知（合成节点 / 旧路径）。
struct SourceLocation {
    int line = 0;
    int col = 0;

    SourceLocation() = default;
    SourceLocation(int l, int c) : line(l), col(c) {}

    [[nodiscard]] bool valid() const { return line > 0; }
};

// Debug 构建中校验 ErrorCode 消息模板的 {} 占位符数量与实际参数一致
// std::vformat 参数不足时会抛 format_error，此断言让问题在 throw 点立刻暴露
#ifndef NDEBUG
inline constexpr size_t countFmtPlaceholders(std::string_view fmt) {
    size_t count = 0;
    for (size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] == '{' && fmt[i + 1] == '}') {
            ++count;
            ++i;
        }
    }
    return count;
}
#endif

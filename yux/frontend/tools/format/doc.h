// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化器 Doc IR
//
// 本文件定义 AST 驱动格式化器使用的中间表示 (Doc):
// - Text          字面文本
// - Concat        顺序拼接子 Doc
// - Indent        在子 Doc 范围内增加缩进
// - Group         组合点：能放下一行就 flat，否则 break
// - Line          软换行：flat 模式渲染为给定字符（通常空格 / 空串），break 模式换行
// - HardLine      硬换行：始终换行，并强制外层 Group break
//
// 渲染由 render.{h,cpp} 完成。printer.{h,cpp} 负责把 ParseTree 翻译成 Doc。

#ifndef YUX_LANG_FORMAT_DOC_H
#define YUX_LANG_FORMAT_DOC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yux::format {

struct DocNode;
using Doc = std::shared_ptr<const DocNode>;

enum class DocKind : std::uint8_t {
    Text,
    Concat,
    Indent,
    Group,
    Line,
    HardLine,
};

struct DocNode {
    DocKind kind;
    // Text: text
    // Line: flatText（flat 模式下输出的字符，通常 " " 或 ""）
    std::string text;
    // Concat / Group: children；Indent: children[0] 为子 Doc，indent 字段保存增量
    std::vector<Doc> children;
    // Indent: 缩进增量
    int indent = 0;
};

// ==================== 构造工具 ====================

Doc text(std::string s);
Doc concat(std::vector<Doc> parts);
Doc indent(int n, Doc child);
Doc group(Doc child);
// flat 模式下输出 flatText，break 模式下换行
Doc line(std::string flatText = " ");
// 始终换行；同时强制最近一层 Group 进入 break
Doc hardline();
// 等价 line("")，便于阅读
inline Doc softline() { return line(""); }

} // namespace yux::format

#endif // YUX_LANG_FORMAT_DOC_H

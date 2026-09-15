// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 静态补全表
//
// 关键字 / 类型 / 内置函数 / 类型转换函数 / snippet 五类硬编码项；
// 跟随 riu-vscode 旧 TS 扩展中的列表（迁移到编译器端，避免 TS/LSP 双源）。
// 后续 P2 的"基于符号表的动态补全"会在另外的入口拼接，不在这里。

#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace riu::lsp {

// LSP CompletionItemKind 子集
enum class CompletionKind : u8 {
    Class = 7,
    Function = 3,
    Keyword = 14,
    Snippet = 15,
};

// LSP InsertTextFormat
enum class InsertFormat : u8 {
    PlainText = 1,
    Snippet = 2,
};

struct CompletionItem {
    std::string label;
    CompletionKind kind = CompletionKind::Keyword;
    std::string detail;
    std::string insertText; // 空 → 用 label
    InsertFormat format = InsertFormat::PlainText;
    std::string documentation; // 空 → 不发
};

// 全部静态项；幂等、无状态、可重复调用
const std::vector<CompletionItem>& staticCompletions();

} // namespace riu::lsp

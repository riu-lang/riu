// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP textDocument/semanticTokens 支持
//
// 复跑 rd Scanner + FlatAst，仅把语义分类（标识符角色、# 注解）编码为
// (deltaLine, deltaStartChar, length, tokenType, tokenModifiers) 五元组。
// 词法级 token 由编辑器 SyntaxHighlighter / TextMate 负责，不在此重复发送。

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace riu::lsp {

// LSP semantic tokens legend：tokenTypes 数组顺序即 token type 整数索引。
const std::vector<std::string>& semanticTokenTypes();
const std::vector<std::string>& semanticTokenModifiers();

// 把 UTF-8 源码编码成 LSP semanticTokens.data 数组（每 5 个 int 一组）。
std::vector<int> computeSemanticTokens(std::string_view text);

} // namespace riu::lsp

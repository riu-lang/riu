// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP textDocument/semanticTokens 支持
//
// 复跑 riuLexer 把 token 类型映射到 LSP semanticTokens 协议的
// (deltaLine, deltaStartChar, length, tokenType, tokenModifiers)
// 五元组编码。当前实现只看 lexer token，不依赖 parse 树，因此即使
// 文档存在语法错误也能着色。

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

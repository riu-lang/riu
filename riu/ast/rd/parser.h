// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V parser（MIT）。产生式跟 riuParser.g4 的 program / imports，不搬 V 的 ASI / import。
// 本栈只给 riu-ast dump 用；ANTLR FileNode 仍是 riu / riu-check / LSP / format 主路。

#ifndef RIU_LANG_RD_PARSER_H
#define RIU_LANG_RD_PARSER_H

#include "ast/rd/flat.h"
#include "ast/rd/scanner.h"
#include "ast/rd/token.h"

#include <string_view>

namespace rd {

class Parser {
public:
    explicit Parser(std::string_view src);

    // Node.value 指向 src；调用方须在 dump / 使用树期间保持源存活。
    [[nodiscard]] FlatAst parse();

private:
    void next();
    [[nodiscard]] const Token& peek();
    [[nodiscard]] bool at(Kind k) const { return tok_.kind == k; }
    [[nodiscard]] bool peekIs(Kind k);
    bool eat(Kind k);
    void skipLineEnds();
    void skipRest();
    void skipToLineEnd();

    [[nodiscard]] NodeId parseUse();

    Scanner scanner_;
    Token tok_{};
    Token peek_tok_{};
    bool has_peek_ = false;
    FlatAst ast_;
};

// 解析 program：前导空行、use、空行、其余顶层跳过、EOF。
[[nodiscard]] FlatAst parseProgram(std::string_view src);

} // namespace rd

#endif // RIU_LANG_RD_PARSER_H

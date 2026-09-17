// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V scanner（MIT）。词法规则跟 riuLexer.g4（`;` 注释、LineEnd、StrTpl），不搬 V 的 ASI / 关键字。

#ifndef RIU_LANG_RD_SCANNER_H
#define RIU_LANG_RD_SCANNER_H

#include "ast/rd/token.h"

#include <string_view>

namespace rd {

class Scanner {
public:
    explicit Scanner(std::string_view src);

    // 下一 token。rd.1 骨架只返回 Eof；rd.2 起按 g4 扫描。
    [[nodiscard]] Token next();

    [[nodiscard]] std::string_view src() const { return src_; }

private:
    [[nodiscard]] Token makeEof() const;

    std::string_view src_;
};

} // namespace rd

#endif // RIU_LANG_RD_SCANNER_H

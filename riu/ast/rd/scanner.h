// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V scanner（MIT）。词法规则跟 riuLexer.g4（`;` 注释、LineEnd、StrTpl），不搬 V 的 ASI / 关键字。

#ifndef RIU_LANG_RD_SCANNER_H
#define RIU_LANG_RD_SCANNER_H

#include "ast/rd/token.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace rd {

class Scanner {
public:
    explicit Scanner(std::string_view src);

    // 下一 default 通道 token（跳过 Space / LineComment / LineEndComment）。
    [[nodiscard]] Token next();

    [[nodiscard]] std::string_view src() const { return src_; }

private:
    enum class Mode : std::uint8_t { Default, StrTpl };

    [[nodiscard]] Token makeEof() const;
    [[nodiscard]] Token scanOne();
    [[nodiscard]] Token emit(Kind kind, size_t start_byte, i32 start_cp, i32 start_line, i32 start_col);
    void consumeCp();
    void advanceBytes(size_t n);
    void pushMode(Mode m);
    void popMode();
    void applySideEffects(Kind kind);
    [[nodiscard]] bool atEnd() const { return byte_pos_ >= src_.size(); }

    std::string_view src_;
    size_t byte_pos_ = 0;
    i32 cp_pos_ = 0;
    i32 line_ = 1;
    i32 column_ = 0;
    bool hit_eof_ = false;
    Mode mode_ = Mode::Default;
    std::vector<Mode> mode_stack_;
    std::vector<int> interp_brace_depth_;
};

} // namespace rd

#endif // RIU_LANG_RD_SCANNER_H

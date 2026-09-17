// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V scanner（MIT）。语言可见 token 跟 riu（关键字、`;` 注释、StrTpl），
// 扫描按 UTF-8 字节分流，不模拟 ANTLR 最长匹配 / code point 下标。

#ifndef RIU_LANG_RD_SCANNER_H
#define RIU_LANG_RD_SCANNER_H

#include "ast/rd/token.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace rd {

class Scanner {
public:
    // errors 非空时词法错误写入该表（parser 试探回退会按 Snapshot 截断）。
    explicit Scanner(std::string_view src, std::vector<ParseError>* errors = nullptr);

    // 下一 default 通道 token（跳过 Space / LineComment / LineEndComment）。
    [[nodiscard]] Token next();

    [[nodiscard]] std::string_view src() const { return src_; }
    [[nodiscard]] const std::vector<ParseError>& errors() const { return *diags_; }

    void setErrors(std::vector<ParseError>* errors);

    // 词法位置快照，给 parser 试探 lambda / 结构体字面量失败时回退。
    struct Snapshot {
        size_t byte_pos = 0;
        i32 line = 1;
        i32 column = 0;
        bool hit_eof = false;
        std::uint8_t mode = 0;
        std::vector<std::uint8_t> mode_stack;
        std::vector<int> interp_brace_depth;
        size_t diag_count = 0;
    };
    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot& s);

private:
    enum class Mode : std::uint8_t { Default, StrTpl };

    [[nodiscard]] Token makeEof() const;
    [[nodiscard]] Token scanOne();
    [[nodiscard]] Token scanStrTpl();
    [[nodiscard]] Token emit(Kind kind, size_t start_byte, i32 start_line, i32 start_col);
    [[nodiscard]] Token scanIdent(size_t start_byte, i32 start_line, i32 start_col);
    [[nodiscard]] Token scanNumber(size_t start_byte, i32 start_line, i32 start_col);
    void consumeIdentRest();
    [[nodiscard]] bool tryRawString();
    [[nodiscard]] bool tryCodePoint();

    void skipTrivia();
    void skipLineComment();
    void skipLineEndComment();
    void consumeNewline();
    void adv(size_t n);
    void consumeInt10();
    void consumeGrouped(bool (*ok)(unsigned char));
    bool consumeIntSuffix();
    bool consumeFloatSuffix();

    [[nodiscard]] unsigned char ch() const;
    [[nodiscard]] unsigned char ch(size_t n) const;
    [[nodiscard]] bool atEnd() const { return byte_pos_ >= src_.size(); }

    void pushMode(Mode m);
    void popMode();
    void applySideEffects(Kind kind);
    void pushLexerError(i32 line, i32 col, std::string_view text);
    [[nodiscard]] Token skipIllegal(size_t start_byte, i32 start_line, i32 start_col);

    std::vector<ParseError> owned_diags_;
    std::vector<ParseError>* diags_ = &owned_diags_;
    std::string_view src_;
    size_t byte_pos_ = 0;
    i32 line_ = 1;
    i32 column_ = 0;
    bool hit_eof_ = false;
    Mode mode_ = Mode::Default;
    std::vector<Mode> mode_stack_;
    std::vector<int> interp_brace_depth_;
};

} // namespace rd

#endif // RIU_LANG_RD_SCANNER_H

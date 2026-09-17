// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/scanner.h"

namespace rd {

Scanner::Scanner(std::string_view src) : src_(src) {}

Token Scanner::next() {
    // TODO(rd.2): 空白、`;` 注释、关键字、Unicode ID、数字、r"..."、c'...'、LineEnd
    return makeEof();
}

Token Scanner::makeEof() const {
    Token tok;
    tok.kind = Kind::Eof;
    i32 line = 1;
    i32 column = 0;
    for (char c : src_) {
        if (c == '\n') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }
    const auto n = static_cast<i32>(src_.size());
    tok.pos = Pos{.offset = n, .end = n, .line = line, .column = column};
    tok.text = {};
    return tok;
}

} // namespace rd

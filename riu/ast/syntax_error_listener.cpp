// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/syntax_error_listener.h"

#include "ast/syntax_diag.h"

#include "Lexer.h"
#include "TokenStream.h"

#include <utility>

SyntaxErrorListener::SyntaxErrorListener(string sourcePath, std::ostream& out)
    : _sourcePath(std::move(sourcePath)), _out(out) {}

void SyntaxErrorListener::syntaxError(antlr4::Recognizer* recognizer, antlr4::Token* offending, size_t line,
                                      size_t charPositionInLine, const std::string& msg, std::exception_ptr /*e*/) {
    ++_errorCount;

    // recognizer 是 Lexer → 词法（E1001）；否则视为文法（E1002）。
    const bool isLexer = dynamic_cast<antlr4::Lexer*>(recognizer) != nullptr;

    // 记录首个错误码与位置，供调用方（如 _parseFile）构造 RiuError
    if (_errorCount == 1) {
        _firstErrorCodeDef = isLexer ? &ErrorCode::E1001 : &ErrorCode::E1002;
        _firstErrorLine = static_cast<int>(line);
        _firstErrorCol = static_cast<int>(charPositionInLine) + 1;
    }

    SyntaxDiag d;
    d.is_lexer = isLexer;
    d.file = _sourcePath;
    d.line = static_cast<int>(line);
    d.col = static_cast<int>(charPositionInLine) + 1; // ANTLR 0-based → 1-based
    d.message = msg;
    d.offending = offending ? offending->getText() : std::string();

    if (!isLexer && offending) {
        auto* ts = dynamic_cast<antlr4::TokenStream*>(recognizer->getInputStream());
        if (ts) {
            auto idx = offending->getTokenIndex();
            for (size_t i = 1; i <= 5 && idx >= i; ++i) {
                auto* tok = ts->get(idx - i);
                if (tok && tok->getChannel() == antlr4::Token::DEFAULT_CHANNEL) {
                    d.prev_text = tok->getText();
                    break;
                }
            }
        }
    }

    renderSyntaxDiag(_out, d);
}

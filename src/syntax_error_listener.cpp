// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "syntax_error_listener.h"

#include "diagnostic.h"
#include "error_code.h"

#include "Lexer.h"

#include <utility>

SyntaxErrorListener::SyntaxErrorListener(string sourcePath, std::ostream& out)
    : _sourcePath(std::move(sourcePath)), _out(out) {}

void SyntaxErrorListener::syntaxError(antlr4::Recognizer* recognizer,
                                      antlr4::Token* /*offending*/,
                                      size_t line, size_t charPositionInLine,
                                      const std::string& msg,
                                      std::exception_ptr /*e*/) {
    ++_errorCount;

    // recognizer 是 Lexer → 词法（E1001）；否则视为文法（E1002）。
    bool isLexer = dynamic_cast<antlr4::Lexer*>(recognizer) != nullptr;
    const ErrorCodeDef& ec = isLexer ? ErrorCode::E1001 : ErrorCode::E1002;

    Diagnostic d;
    d.severity = DiagSeverity::Error;
    d.code = ec.code;
    d.file = _sourcePath;
    d.line = static_cast<int>(line);
    d.col = static_cast<int>(charPositionInLine) + 1; // ANTLR 0-based → 1-based
    d.message = std::vformat(ec.message, std::make_format_args(msg));

    DiagnosticEngine::render(_out, d);
}

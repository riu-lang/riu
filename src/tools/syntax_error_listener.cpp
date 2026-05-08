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

    // 文法错误按 ANTLR 原始消息粗判几个常见形态，附加修复提示
    // 词法错误（E1001）通常是非法字符，hint 价值有限，暂不附
    if (!isLexer) {
        if (msg.find("';'") != std::string::npos || msg.find("missing ';'") != std::string::npos) {
            d.hints.push_back("语句末尾需要 `;`；表达式带 `;` 表示舍弃返回值，不带 `;` 才会作为返回值（参见 docs/基础语法.md）");
        } else if (msg.find("extraneous input") != std::string::npos
                   || msg.find("mismatched input") != std::string::npos
                   || msg.find("no viable alternative") != std::string::npos) {
            d.hints.push_back("检查空格规则：关键字后、二元运算符两侧、`,` 后必须有空格；`()` `[]` 内部不留空格");
        }
    }

    DiagnosticEngine::render(_out, d);
}

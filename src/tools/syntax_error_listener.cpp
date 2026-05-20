// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "syntax_error_listener.h"

#include "diagnostic.h"
#include "error_code.h"

#include "Lexer.h"

#include <unordered_set>
#include <utility>

SyntaxErrorListener::SyntaxErrorListener(string sourcePath, std::ostream& out)
    : _sourcePath(std::move(sourcePath)), _out(out) {}

namespace {
bool isYuxKeyword(const std::string& s) {
    static const std::unordered_set<std::string> kws = {
        "break", "catch", "draft", "elif", "else",
        "enum", "extern", "false", "fn", "if", "let", "loop", "match", "null",
        "ret", "struct", "true", "try", "use",
    };
    return kws.count(s) > 0;
}
}

void SyntaxErrorListener::syntaxError(antlr4::Recognizer* recognizer,
                                      antlr4::Token* offending,
                                      size_t line, size_t charPositionInLine,
                                      const std::string& msg,
                                      std::exception_ptr /*e*/) {
    ++_errorCount;

    // recognizer 是 Lexer → 词法（E1001）；否则视为文法（E1002）。
    bool isLexer = dynamic_cast<antlr4::Lexer*>(recognizer) != nullptr;

    // 抑制：lexer 被困在非默认 mode（如字符串插值未闭合）后, 对 '\n' '\r'
    // 触发的 "token recognition error" 几乎全是噪声 —— 真正的错误已由
    // E1002 报出. 仅在 offending 是纯空白/换行时跳过渲染.
    if (isLexer) {
        auto pos = msg.find("token recognition error at: '");
        if (pos != std::string::npos) {
            std::string at = msg.substr(pos + sizeof("token recognition error at: '") - 1);
            if (!at.empty() && at.back() == '\'') at.pop_back();
            // 转义 '\n' / '\r' / '\t' 文本形态 + 真实白空格
            bool whitespaceOnly = !at.empty();
            for (size_t i = 0; i < at.size(); ) {
                if (at[i] == '\\' && i + 1 < at.size()
                    && (at[i+1] == 'n' || at[i+1] == 'r' || at[i+1] == 't')) {
                    i += 2; continue;
                }
                if (at[i] == ' ' || at[i] == '\t' || at[i] == '\n' || at[i] == '\r') {
                    ++i; continue;
                }
                whitespaceOnly = false; break;
            }
            if (whitespaceOnly) return;
        }
    }

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
        // 优先：offending 是 yux 关键字 + 期望 ID → 关键字误作标识符
        std::string offText = offending ? offending->getText() : std::string();
        bool keywordAsId = !offText.empty() && isYuxKeyword(offText)
                           && msg.find("ID") != std::string::npos;
        if (keywordAsId) {
            d.hints.push_back("`" + offText + "` 是 yux 关键字，不能用作标识符；换一个名字（如 `" + offText + "_`）");
        } else if (msg.find("';'") != std::string::npos || msg.find("missing ';'") != std::string::npos) {
            d.hints.push_back("语句末尾需要 `;`；表达式带 `;` 表示舍弃返回值，不带 `;` 才会作为返回值（参见 docs/基础语法.md）");
        } else if (msg.find("extraneous input") != std::string::npos
                   || msg.find("mismatched input") != std::string::npos
                   || msg.find("no viable alternative") != std::string::npos) {
            d.hints.push_back("检查空格规则：关键字后、二元运算符两侧、`,` 后必须有空格；`()` `[]` 内部不留空格");
        }
    }

    DiagnosticEngine::render(_out, d);
}

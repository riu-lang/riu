// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/syntax_diag.h"

#include "error_code.h"

#include <format>
#include <ostream>
#include <unordered_set>
#include <utility>

namespace {

bool isRiuKeyword(const std::string& s) {
    static const std::unordered_set<std::string> kws = {
        "break", "catch", "continue", "draft", "elif",  "else", "enum", "extern", "false", "fn",  "for",
        "if",    "in",    "let",      "loop",  "match", "null", "ret",  "struct", "true",  "try", "use",
    };
    return kws.count(s) > 0;
}

// lexer 困在非默认 mode 后对 '\n' / '\r' 的 token recognition 几乎全是噪声。
bool isWhitespaceLexerNoise(const std::string& msg) {
    auto pos = msg.find("token recognition error at: '");
    if (pos == std::string::npos) return false;
    std::string at = msg.substr(pos + sizeof("token recognition error at: '") - 1);
    if (!at.empty() && at.back() == '\'') at.pop_back();
    bool whitespaceOnly = !at.empty();
    for (size_t i = 0; i < at.size();) {
        if (at[i] == '\\' && i + 1 < at.size() && (at[i + 1] == 'n' || at[i + 1] == 'r' || at[i + 1] == 't')) {
            i += 2;
            continue;
        }
        if (at[i] == ' ' || at[i] == '\t' || at[i] == '\n' || at[i] == '\r') {
            ++i;
            continue;
        }
        whitespaceOnly = false;
        break;
    }
    return whitespaceOnly;
}

void appendSyntaxHints(Diagnostic& d, const SyntaxDiag& in) {
    if (in.is_lexer) return;

    const std::string& msg = in.message;
    const std::string& offText = in.offending;
    const bool keywordAsId = !offText.empty() && isRiuKeyword(offText) && msg.find("ID") != std::string::npos;
    if (keywordAsId) {
        d.hints.push_back("`" + offText + "` 是 riu 关键字，不能用作标识符；换一个名字（如 `" + offText + "_`）");
        return;
    }
    if (offText == "ret" || msg.find("'ret") != std::string::npos) {
        d.hints.emplace_back("空返回 `ret;` 必须带 `;`（void 返回）；如需返回值，使用 `ret 表达式`");
        return;
    }
    if (offText == "break" || msg.find("'break") != std::string::npos) {
        d.hints.emplace_back("`break;` 必须带 `;`");
        return;
    }
    if (offText == "continue" || msg.find("'continue") != std::string::npos) {
        d.hints.emplace_back("`continue;` 必须带 `;`");
        return;
    }
    if (msg.find("';'") != std::string::npos || msg.find("missing ';'") != std::string::npos) {
        if (in.prev_text == "ret") {
            d.hints.emplace_back("空返回 `ret;` 必须带 `;`（void 返回）；如需返回值，使用 `ret 表达式`");
        } else if (in.prev_text == "break") {
            d.hints.emplace_back("`break;` 必须带 `;`");
        } else if (in.prev_text == "continue") {
            d.hints.emplace_back("`continue;` 必须带 `;`");
        } else {
            d.hints.emplace_back(
                "语句末尾需要 `;`；表达式带 `;` 表示舍弃返回值，不带 `;` 才会作为返回值（参见 docs/基础语法.md）");
        }
        return;
    }
    if (msg.find("extraneous input") != std::string::npos || msg.find("mismatched input") != std::string::npos ||
        msg.find("no viable alternative") != std::string::npos) {
        d.hints.emplace_back("检查空格规则：关键字后、二元运算符两侧、`,` 后必须有空格；`()` `[]` 内部不留空格");
    }
}

} // namespace

bool fillSyntaxDiagnostic(Diagnostic& out, const SyntaxDiag& in) {
    if (in.is_lexer && isWhitespaceLexerNoise(in.message)) return false;

    const ErrorCodeDef& ec = in.is_lexer ? ErrorCode::E1001 : ErrorCode::E1002;
    out = Diagnostic{};
    out.severity = DiagSeverity::Error;
    out.code = ec.code;
    out.file = in.file;
    out.line = in.line;
    out.col = in.col;
    const std::string msg = in.message;
    out.message = std::vformat(ec.message, std::make_format_args(msg));
    appendSyntaxHints(out, in);
    return true;
}

bool renderSyntaxDiag(std::ostream& out, const SyntaxDiag& in) {
    Diagnostic d;
    if (!fillSyntaxDiagnostic(d, in)) return false;
    DiagnosticEngine::render(out, d);
    return true;
}

std::string formatSyntaxDiagCompact(const SyntaxDiag& in) {
    Diagnostic d;
    if (!fillSyntaxDiagnostic(d, in)) return {};
    std::string out;
    out += d.code;
    out += ' ';
    out += std::to_string(in.line);
    out += ':';
    const int col0 = in.col > 0 ? in.col - 1 : 0;
    out += std::to_string(col0);
    out += "  ";
    out += d.message;
    out += '\n';
    for (const auto& h : d.hints) {
        out += "  = help: ";
        out += h;
        out += '\n';
    }
    return out;
}

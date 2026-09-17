// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 词法 / 文法诊断渲染（0 ANTLR）。ANTLR SyntaxErrorListener 与 rd Parser 共用。

#ifndef RIU_LANG_SYNTAX_DIAG_H
#define RIU_LANG_SYNTAX_DIAG_H

#include "ast/diagnostic.h"

#include <iosfwd>
#include <string>

// 一条 E1001 / E1002。line 1-based；col 1-based（与 Diagnostic 一致）。
// message 是错误码模板 `{}` 的实参（ANTLR 原文或 rd 生成的 mismatched / no viable）。
struct SyntaxDiag {
    bool is_lexer = false;
    std::string file;
    int line = 1;
    int col = 1;
    std::string message;
    std::string offending;
    std::string prev_text;
};

// 填 Diagnostic（含 hint）。词法空白噪声返回 false，不填 out。
bool fillSyntaxDiagnostic(Diagnostic& out, const SyntaxDiag& in);

// fill + DiagnosticEngine::render。空白噪声不输出，返回 false。
bool renderSyntaxDiag(std::ostream& out, const SyntaxDiag& in);

// 紧凑金样：`E1002 1:5  syntax error: ...`（列 0-based，与 rd Pos 一致），无路径/源码片段。
[[nodiscard]] std::string formatSyntaxDiagCompact(const SyntaxDiag& in);

#endif // RIU_LANG_SYNTAX_DIAG_H

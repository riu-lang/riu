// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 编译器侧 ANTLR 错误监听器
//
// 把 lexer / parser 的 syntaxError 即时渲染为统一格式的诊断（与
// DiagnosticEngine 同款 file:line:col [Exxxx] error: msg + 源码片段 + 插入符），
// 并累计错误数。调用方应当在解析后检查 hasErrors() 决定是否中止流程。

#ifndef YUX_LANG_SYNTAX_ERROR_LISTENER_H
#define YUX_LANG_SYNTAX_ERROR_LISTENER_H

#include "BaseErrorListener.h"
#include "error_code.h"
#include "types.h"

class SyntaxErrorListener : public antlr4::BaseErrorListener {
public:
    SyntaxErrorListener(string sourcePath, std::ostream& out);

    void syntaxError(antlr4::Recognizer* recognizer, antlr4::Token* offending, size_t line, size_t charPositionInLine,
                     const std::string& msg, std::exception_ptr e) override;

    [[nodiscard]] bool hasErrors() const { return _errorCount > 0; }
    [[nodiscard]] int errorCount() const { return _errorCount; }

    // 首个错误的 ErrorCodeDef 指针与位置；仅 syntaxError 至少调用一次后有效。
    // 用于调用方构造 YuxError 时复现正确的错误码与行号（而非万能 E5010）。
    [[nodiscard]] const ErrorCodeDef* firstErrorCodeDef() const { return _firstErrorCodeDef; }
    [[nodiscard]] int firstErrorLine() const { return _firstErrorLine; }
    [[nodiscard]] int firstErrorCol() const { return _firstErrorCol; }

private:
    string _sourcePath;
    std::ostream& _out;
    int _errorCount = 0;
    const ErrorCodeDef* _firstErrorCodeDef = &ErrorCode::E1002;
    int _firstErrorLine = 1;
    int _firstErrorCol = 1;
};

#endif // YUX_LANG_SYNTAX_ERROR_LISTENER_H

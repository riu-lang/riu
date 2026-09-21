// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "error_code.h"
#include "source_location.h"
#include "token.h"

#include <cassert>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

class RiuError : public std::runtime_error {
    size_t _line = 0;
    int _col = 0;                            // 0 表示列未知
    const char* _code = "E0000";             // 指向 ErrorCode 表中的静态字面量
    DiagSeverity _sev = DiagSeverity::Error; // 默认严重等级（来源于 ErrorCodeDef.defaultSev）
    vector<string> _hints;                   // 修复建议（"= help: ..."），可链式 withHint 追加
    vector<string> _notes;                   // 附加说明（"= note: ..."），可链式 withNote 追加
    string _file;                            // 出错节点所属源文件；空 = 由渲染入口 sourcePath 决定

public:
    explicit RiuError(const string& msg, size_t line) : runtime_error(msg), _line(line) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(size_t line, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(size_t line, int col, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line), _col(col) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(SourceLocation loc, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(loc.line), _col(loc.col) {
        assert(loc.line > 0 && "RiuError line must be > 0");
    }

    // ErrorCode 路径：模板取自 ec.message，code 取自 ec.code
    template <class... Types>
    explicit RiuError(size_t line, int col, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _col(col), _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    // 列未知场景的便利重载（驱动层 / 模块层 errorLine）
    template <class... Types>
    explicit RiuError(size_t line, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    template <class... Types>
    explicit RiuError(SourceLocation loc, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(loc.line),
          _col(loc.col), _code(ec.code), _sev(ec.defaultSev) {
        assert(loc.line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    void setLineNumber(size_t line) {
        assert(line > 0 && "RiuError line must be > 0");
        _line = line;
    }

    void setColumn(int col) { _col = col; }

    void setFile(string f) { _file = std::move(f); }

    [[nodiscard]] size_t getLineNumber() const { return _line; }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {static_cast<int>(_line), _col}; }

    [[nodiscard]] const char* getCode() const { return _code; }

    [[nodiscard]] DiagSeverity getSeverity() const { return _sev; }

    // 出错节点所属源文件。空 = 渲染时回退到入口传入的正在编译文件。
    [[nodiscard]] const string& file() const { return _file; }

    RiuError& withFile(string f) & {
        _file = std::move(f);
        return *this;
    }
    RiuError&& withFile(string f) && {
        _file = std::move(f);
        return std::move(*this);
    }

    // 链式追加 help / note：支持 `throw RiuError(...).withHint("...")` 形态
    RiuError& withHint(string h) & {
        _hints.push_back(std::move(h));
        return *this;
    }
    RiuError&& withHint(string h) && {
        _hints.push_back(std::move(h));
        return std::move(*this);
    }
    RiuError& withNote(string n) & {
        _notes.push_back(std::move(n));
        return *this;
    }
    RiuError&& withNote(string n) && {
        _notes.push_back(std::move(n));
        return std::move(*this);
    }

    [[nodiscard]] const vector<string>& hints() const { return _hints; }
    [[nodiscard]] const vector<string>& notes() const { return _notes; }

    // 显式声明拷贝 / 移动构造 noexcept：throw RiuError 在抛出栈展开期间不允许再次抛异常；
    // 真正的 OOM 走 std::terminate（语义上等价于 runtime_error 自身的承诺）
    // NOLINTBEGIN(bugprone-exception-escape)
    RiuError(const RiuError&) noexcept = default;
    RiuError(RiuError&&) noexcept = default;
    RiuError& operator=(const RiuError&) noexcept = default;
    RiuError& operator=(RiuError&&) noexcept = default;
    // NOLINTEND(bugprone-exception-escape)
};

inline void checkDiscardDeclName(string_view name, string_view kind, int line, int col) {
    if (isDiscardName(name)) {
        throw RiuError(line, col, ErrorCode::E3161, string(kind));
    }
}

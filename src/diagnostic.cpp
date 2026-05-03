// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 诊断引擎实现
//
// 本文件包含诊断渲染相关的实现：
// - YuxError → Diagnostic 转换
// - 源文件读取与按行切片（带缓存）
// - rustc 风格的 file:line:col + 源码片段 + 插入符渲染

#include "diagnostic.h"

#include <fstream>
#include <map>
#include <sstream>

// ==================== DiagPolicy 全局状态 ====================

namespace {
    // 单一全局策略；编译器进程内共享。LSP 多 session 用 reset() 清理。
    std::map<string, DiagSeverity>& policyOverrides() {
        static std::map<string, DiagSeverity> m;
        return m;
    }
    bool& policyWerrorRef() {
        static bool w = false;
        return w;
    }
}

bool DiagPolicy::setSeverityOverride(const string& code, DiagSeverity defaultSev, DiagSeverity newSev) {
    // 不可降级：默认就是 Error 的码不允许通过 --warn / --allow 改成更低
    if (defaultSev == DiagSeverity::Error && newSev != DiagSeverity::Error) {
        return false;
    }
    policyOverrides()[code] = newSev;
    return true;
}

void DiagPolicy::setWerror(bool on) {
    policyWerrorRef() = on;
}

bool DiagPolicy::werror() {
    return policyWerrorRef();
}

const DiagSeverity* DiagPolicy::findOverride(const string& code) {
    auto& m = policyOverrides();
    auto it = m.find(code);
    if (it == m.end()) return nullptr;
    return &it->second;
}

DiagSeverity DiagPolicy::effectiveSeverity(const string& code, DiagSeverity defaultSev) {
    DiagSeverity sev = defaultSev;
    if (auto* o = findOverride(code)) sev = *o;
    if (policyWerrorRef() && sev == DiagSeverity::Warning) sev = DiagSeverity::Error;
    return sev;
}

void DiagPolicy::reset() {
    policyOverrides().clear();
    policyWerrorRef() = false;
}

namespace {

// 源文件按路径缓存：路径 → 行内容数组（utf-8 字节）
// 仅在单次进程内、同一路径多次取行时复用；不监控文件变化。
std::map<string, vector<string>>& sourceCache() {
    static std::map<string, vector<string>> cache;
    return cache;
}

const vector<string>* loadSource(const string& path) {
    if (path.empty()) return nullptr;
    auto& cache = sourceCache();
    auto it = cache.find(path);
    if (it != cache.end()) return &it->second;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        cache[path] = {};
        return &cache[path];
    }
    vector<string> lines;
    string line;
    while (std::getline(f, line)) {
        // 去掉可能的 CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    cache[path] = std::move(lines);
    return &cache[path];
}

const char* severityLabel(DiagSeverity s) {
    switch (s) {
        case DiagSeverity::Note:    return "note";
        case DiagSeverity::Warning: return "warning";
        case DiagSeverity::Error:   return "error";
    }
    return "error";
}

} // namespace

void DiagnosticEngine::render(std::ostream& out, const Diagnostic& diagIn) {
    // 应用全局 severity 策略（CLI --warn / --allow / --deny / -Werror）
    Diagnostic diag = diagIn;
    diag.severity = DiagPolicy::effectiveSeverity(diag.code, diag.severity);

    // 头部：file:line:col [code] severity: message
    if (!diag.file.empty()) {
        out << diag.file;
        if (diag.line > 0) {
            out << ':' << diag.line;
            if (diag.col > 0) out << ':' << diag.col;
        }
        out << ' ';
    } else if (diag.line > 0) {
        out << "line " << diag.line;
        if (diag.col > 0) out << ':' << diag.col;
        out << ' ';
    }
    out << '[' << diag.code << "] " << severityLabel(diag.severity) << ": " << diag.message << '\n';

    // 源码片段（仅在 file + line + col 都有效时显示）
    if (!diag.file.empty() && diag.line > 0) {
        const auto* lines = loadSource(diag.file);
        if (lines && diag.line <= static_cast<int>(lines->size())) {
            const string& srcLine = (*lines)[diag.line - 1];
            string lineNoStr = std::to_string(diag.line);
            string gutter(lineNoStr.size(), ' ');

            out << gutter << " |\n";
            out << lineNoStr << " | " << srcLine << '\n';
            if (diag.col > 0) {
                out << gutter << " | ";
                // col 是 1-based 列号；按字节宽度对齐（中文等多字节会偏移，Phase 1 接受）
                int padding = diag.col - 1;
                if (padding < 0) padding = 0;
                out << string(padding, ' ') << "^\n";
            }
        }
    }

    // notes / hints
    for (auto& n : diag.notes) {
        out << "  = note: " << n << '\n';
    }
    for (auto& h : diag.hints) {
        out << "  = help: " << h << '\n';
    }
}

void DiagnosticEngine::renderYuxError(std::ostream& out,
                                      const string& sourcePath,
                                      const YuxError& err) {
    Diagnostic d;
    d.severity = err.getSeverity();
    d.code = err.getCode() ? err.getCode() : "E0000";
    d.file = sourcePath;
    d.line = err.getLineNumber();
    d.col = err.getColumn();
    d.message = err.what();
    d.notes = err.notes();
    d.hints = err.hints();
    render(out, d);
}

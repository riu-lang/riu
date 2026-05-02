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

void DiagnosticEngine::render(std::ostream& out, const Diagnostic& diag) {
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
    d.severity = DiagSeverity::Error;
    d.code = err.getCode() ? err.getCode() : "E0000";
    d.file = sourcePath;
    d.line = err.getLineNumber();
    d.col = err.getColumn();
    d.message = err.what();
    render(out, d);
}

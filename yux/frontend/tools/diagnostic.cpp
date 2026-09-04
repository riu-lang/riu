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
#include <set>
#include <utility>

#include "utf8.h"

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
} // namespace

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

// 估算单个 Unicode codepoint 在等宽终端中的显示列宽
//
// 简化的 East Asian Width 近似：
//   - 控制字符 / 组合标记 / 零宽字符 → 0
//   - CJK 表意 / 韩文音节 / 假名 / CJK 符号与标点 / 全角形式 / 常见 emoji → 2
//   - 其他 → 1
//
// 不依赖 ICU；够覆盖中文 + 常见 emoji 的诊断插入符对齐。
// 对罕见脚本（如阿拉伯文连字、泰文上下叠合）会有偏差，留待后续按需扩展。
int displayWidthOfCodepoint(char32_t cp) {
    // 控制字符
    if (cp < 0x20 || cp == 0x7F) return 0;

    // 组合标记 / 零宽
    if ((cp >= 0x0300 && cp <= 0x036F) ||                                   // Combining Diacritical Marks
        (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x200B && cp <= 0x200F) || // ZWSP / ZWNJ / ZWJ / LRM / RLM
        cp == 0x2028 || cp == 0x2029 || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || // Variation Selectors
        (cp >= 0xE0100 && cp <= 0xE01EF)) // VS Supplement
        return 0;

    // 全宽 / CJK / 韩文 / 假名 / 全角符号
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK Radicals / Kangxi / Symbols
        (cp >= 0x3041 && cp <= 0x33FF) ||   // Hiragana / Katakana / Bopomofo / Compat
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul Syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compat Ideographs
        (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK Compat Forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth Forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Signs
        (cp >= 0x20000 && cp <= 0x2FFFD) || // CJK Ext B-F
        (cp >= 0x30000 && cp <= 0x3FFFD))   // CJK Ext G+
        return 2;

    // 常见 emoji 段（粗略覆盖；不区分 text/emoji presentation）
    if ((cp >= 0x2600 && cp <= 0x27BF) ||   // Misc Symbols / Dingbats
        (cp >= 0x1F300 && cp <= 0x1F5FF) || // Misc Symbols and Pictographs
        (cp >= 0x1F600 && cp <= 0x1F64F) || // Emoticons
        (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport and Map
        (cp >= 0x1F700 && cp <= 0x1F77F) || (cp >= 0x1F780 && cp <= 0x1F7FF) || (cp >= 0x1F800 && cp <= 0x1F8FF) ||
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||                                   // Supplemental Symbols and Pictographs
        (cp >= 0x1FA00 && cp <= 0x1FA6F) || (cp >= 0x1FA70 && cp <= 0x1FAFF)) // Symbols and Pictographs Ext-A
        return 2;

    return 1;
}

// 把列号 col（1-based）转成插入符 padding 字符串
//
// `col` 与 ANTLR 的 `getCharPositionInLine() + 1` 同源，按 Unicode codepoint 计数
// （而非字节）。规则：扫描 srcLine 前 (col-1) 个 codepoint：
//   - tab 原样保留（让终端按与源码行一致的 tab stop 扩展）
//   - 其他按 displayWidthOfCodepoint 输出对应数量的空格
// 这样 `<srcLine>` 与下一行的 `<padding>^` 在终端里视觉对齐，
// 中文 / emoji / 全角符号都不会让 ^ 偏移（D.1.1 + D.7）。
//
// 解码失败（损坏的 utf-8 序列）按 1 codepoint / 1 字节跳过，避免抛异常打断诊断输出。
string caretPaddingFromCol(const string& srcLine, int col) {
    string out;
    if (col <= 1) return out;
    auto targetCp = static_cast<size_t>(col - 1);

    const char* it = srcLine.data();
    const char* end = it + srcLine.size();
    size_t cpCount = 0;
    while (it < end && cpCount < targetCp) {
        if (*it == '\t') {
            out.push_back('\t');
            ++it;
            ++cpCount;
            continue;
        }
        const char* prev = it;
        char32_t cp = 0;
        try {
            cp = utf8::next(it, end);
        } catch (...) {
            // 损坏字节：当作宽度 1 跳过 1 字节，继续渲染
            it = prev + 1;
            out.push_back(' ');
            ++cpCount;
            continue;
        }
        int w = displayWidthOfCodepoint(cp);
        for (int i = 0; i < w; ++i)
            out.push_back(' ');
        ++cpCount;
    }
    return out;
}

const char* severityLabel(DiagSeverity s) {
    switch (s) {
    case DiagSeverity::Note:
        return "note";
    case DiagSeverity::Warning:
        return "warning";
    case DiagSeverity::Error:
        return "error";
    }
    return "error";
}

// 跨文件 AST：出错节点自带路径时优先于正在编译的入口文件
string diagnosticFile(const string& sourcePath, const YuxError& err) {
    return err.file().empty() ? sourcePath : err.file();
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
        if (lines && std::cmp_less_equal(diag.line, lines->size())) {
            const string& srcLine = (*lines)[diag.line - 1];
            string lineNoStr = std::to_string(diag.line);
            string gutter(lineNoStr.size(), ' ');

            out << gutter << " |\n";
            out << lineNoStr << " | " << srcLine << '\n';
            if (diag.col > 0) {
                out << gutter << " | ";
                // col 仍为 1-based 字节列号；插入符 padding 按显示列宽换算，
                // 让中文 / emoji / 全角符号下的 ^ 与视觉位置对齐（D.1.1 + D.7）。
                out << caretPaddingFromCol(srcLine, diag.col) << "^\n";
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

namespace {
// emit 去重 set：(file, code, line, col, message)
// 同一站点可能从不同路径 emit；按 5 元组去重即可
using EmitKey = std::tuple<string, string, size_t, int, string>;
std::set<EmitKey>& emitDedup() {
    static std::set<EmitKey> s;
    return s;
}
} // namespace

void DiagnosticEngine::resetEmitDedup() {
    emitDedup().clear();
}

void DiagnosticEngine::emit(std::ostream& out, const string& sourcePath, const YuxError& err) {
    DiagSeverity defaultSev = err.getSeverity();
    // emit 仅服务 warning / note；默认 Error 的码应当 throw 走 renderYuxError 路径
    assert(defaultSev != DiagSeverity::Error &&
           "DiagnosticEngine::emit() called with default-Error code; use throw + renderYuxError instead");

    string code = err.getCode() ? err.getCode() : "E0000";
    DiagSeverity sev = DiagPolicy::effectiveSeverity(code, defaultSev);
    string path = diagnosticFile(sourcePath, err);

    EmitKey key{path, code, err.getLineNumber(), err.getColumn(), err.what()};
    bool firstSeen = emitDedup().insert(key).second;

    // 升级到 Error 时：不在 emit 内渲染, 直接 throw 让顶层 catch 走 renderYuxError
    // 流, 避免"emit 渲染一次 + 顶层 catch 再渲染一次"的双重输出.
    if (sev == DiagSeverity::Error) {
        throw err;
    }

    // Warning / Note 路径: 仅渲染一次 (按 5 元组去重)
    if (!firstSeen) return;

    Diagnostic d;
    d.severity = sev;
    d.code = code;
    d.file = path;
    d.line = err.getLineNumber();
    d.col = err.getColumn();
    d.message = err.what();
    d.notes = err.notes();
    d.hints = err.hints();
    render(out, d);
}

void DiagnosticEngine::emit(const string& sourcePath, const YuxError& err) {
    emit(std::cerr, sourcePath, err);
}

void DiagnosticEngine::renderYuxError(std::ostream& out, const string& sourcePath, const YuxError& err) {
    Diagnostic d;
    d.severity = err.getSeverity();
    d.code = err.getCode() ? err.getCode() : "E0000";
    d.file = diagnosticFile(sourcePath, err);
    d.line = err.getLineNumber();
    d.col = err.getColumn();
    d.message = err.what();
    d.notes = err.notes();
    d.hints = err.hints();
    render(out, d);
}

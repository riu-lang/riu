// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 诊断引擎
//
// 负责把 YuxError 渲染成统一格式的错误信息：
//   path/to/file.yux:12:5 [E0000] error: 主信息
//      |
//   12 | val x i32 = "hello"
//      |     ^
//
// Phase 1 仅落基础设施；错误码统一占位 E0000，按严重等级仅有 Error。
// 严重等级、错误码体系、warning/error 分级在 Phase 2-4 补齐。

#ifndef YUX_LANG_DIAGNOSTIC_H
#define YUX_LANG_DIAGNOSTIC_H

#include "types.h"
#include <ostream>

enum class DiagSeverity : u8 {
    Note,
    Warning,
    Error,
};

struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Error;
    string code = "E0000"; // Phase 2 起按类别分配
    string file;           // 源文件绝对/相对路径，可空
    int line = 0;          // 1-based；0 = 未知
    int col = 0;           // 1-based；0 = 未知
    string message;
    vector<string> notes;  // Phase 2+ 使用
    vector<string> hints;  // Phase 5 使用
};

class DiagnosticEngine {
public:
    // 把 YuxError 转成 Diagnostic 并渲染到 out。
    // sourcePath 决定文件名前缀；若为空，前缀只剩 line:col。
    // 引擎内部按 sourcePath 缓存源文件内容（同一个引擎实例多次调用同一文件不重复读）。
    static void renderYuxError(std::ostream& out,
                               const string& sourcePath,
                               const YuxError& err);

    // 通用渲染入口
    static void render(std::ostream& out, const Diagnostic& diag);
};

#endif // YUX_LANG_DIAGNOSTIC_H

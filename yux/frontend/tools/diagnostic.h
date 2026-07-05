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

// DiagSeverity 在 error_code.h 中定义（Note < Warning < Error）

struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Error;
    string code = "E0000"; // Phase 2 起按类别分配
    string file;           // 源文件绝对/相对路径，可空
    size_t line = 0;       // 1-based；0 = 未知
    int col = 0;           // 1-based；0 = 未知
    string message;
    vector<string> notes;  // Phase 2+ 使用
    vector<string> hints;  // Phase 5 使用
};

// CLI 严重度覆盖策略（Phase 4）
//
// 全局策略表：把单个错误码 / 整体 -Werror 等开关收纳进来，渲染时按"默认 sev → 覆盖 → Werror"的顺序计算最终 sev。
//
// 不可降级原则：默认 severity = Error 的错误码不能被 --warn / --allow 降级；尝试降级时
// setSeverityOverride 返回 false，调用方应打印拒绝信息并保留默认严重度。
class DiagPolicy {
public:
    // 注册对单个错误码的严重度覆盖。defaultSev 由调用方提供（来自 ErrorCodeDef.defaultSev）。
    // 拒绝条件：默认 sev = Error 但试图改成 Warning/Note；这种情况返回 false，不修改任何状态。
    // 允许条件：默认 sev <= Warning，可在 Note/Warning/Error 内任意调整；Werror 单独由 setWerror 控制。
    static bool setSeverityOverride(const string& code, DiagSeverity defaultSev, DiagSeverity newSev);

    // 启用 -Werror：所有最终 severity == Warning 的诊断升级为 Error。
    static void setWerror(bool on);

    // 是否启用 Werror。
    static bool werror();

    // 查询某码的覆盖（不存在返回 nullptr）。
    static const DiagSeverity* findOverride(const string& code);

    // 计算诊断的最终 severity：
    //   1) 起点 = defaultSev；
    //   2) 若该 code 有覆盖，使用覆盖值；
    //   3) 若 -Werror 且当前为 Warning，升级为 Error。
    static DiagSeverity effectiveSeverity(const string& code, DiagSeverity defaultSev);

    // 重置所有策略（测试 / LSP 重启场景使用）
    static void reset();
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

    // 非抛出发射通道（warning / note）
    //
    // 与 renderYuxError 的区别：renderYuxError 由顶层 catch 调用（错误已经把当前文件
    // 编译截断了），emit 是诊断的**生产者**直接调用 —— 用 YuxError 复用其消息模板
    // 与 SourceLocation，但**不**抛出（除非 DiagPolicy 把它升级为 Error）。
    //
    // 升级规则：当 err 的默认严重度为 Warning 且经 DiagPolicy::effectiveSeverity 计算
    // 后变为 Error（典型场景：-Werror 或 --deny=<code>），则按 Error 渲染**并**抛出
    // 同一 YuxError，让调用方走原有"首条 error 终止文件"协议。
    //
    // 重入安全：emit 内部对同一进程持有去重 set（path+code+line+col+message），同一
    // 站点只渲染一次。
    //
    // 不应用到 Error 默认严重度的 YuxError —— 那种应当 `throw`，由顶层 catch 走
    // renderYuxError。emit 内部 assert 默认严重度 ≤ Warning。
    static void emit(std::ostream& out,
                     const string& sourcePath,
                     const YuxError& err);

    // 便利重载：默认 std::cerr
    static void emit(const string& sourcePath, const YuxError& err);

    // 测试 / LSP 重启场景清理去重 set
    static void resetEmitDedup();
};

#endif // YUX_LANG_DIAGNOSTIC_H

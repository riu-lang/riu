// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `yux test` 子命令实现
//
// 从 src/main.cpp 抠出 (P1 Phase 1.b.ii): 收集 #Test 函数、LLJIT 装载、SEH
// 包裹 + 输出捕获、子进程隔离 (--isolate=process / #TestIsolate)。
// 仅项目模式; 详见 spec §11.3。

#pragma once

#include <string>
#include <vector>

namespace yux::cli {

struct TestCmdOptions {
    std::vector<std::string> selectors;
    bool verbose = false;
    std::string isolate = "none"; // "none" | "process"
    bool isolateChild = false;
    std::string captureFile;
    bool hasPositionalInput = false;
};

// 子进程模式 (--isolate-child) 下, 在任何输出前把 stdout/stderr 重定向到 capture
// 文件。返回 true 表示当前进程是 isolated child; false 表示正常进程。
// 失败 (例如无法打开 capture 文件) 时直接 _exit(2)。
bool maybeApplyChildRedirect(bool testCmdParsed, bool isolateChild, const std::string& captureFile);

// 运行 `yux test`。永不返回 (内部调 _exit)。
[[noreturn]] void runTestCommand(const TestCmdOptions& opts, bool isChildIsolated);

} // namespace yux::cli

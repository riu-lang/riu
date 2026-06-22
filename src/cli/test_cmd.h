// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `yux test` 子命令实现
//
// 流程：yux build --test → yux-test-runner 多线程加载 *.test.dll 执行
// DLL 协议见 src/tools/runner_main.cpp 文件头注释

#pragma once

namespace yux::cli {

struct TestCmdOptions {
    bool verbose = false;
    int threads = 0; // 线程数（0 = CPU 核数）
};

// 运行 `yux test`。永不返回 (内部调 _exit)。
[[noreturn]] void runTestCommand(const TestCmdOptions& opts);

} // namespace yux::cli

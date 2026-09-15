// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `riu test` 子命令实现
//
// 流程：riu build --test → riu-test-runner 多线程加载 *.test.dll 执行
// DLL 协议见 riu/test-runner/runner_main.cpp 文件头注释

#pragma once

#include <string>

namespace riu::cli {

struct TestCmdOptions {
    bool verbose = false;
    int threads = 0;     // 线程数（0 = CPU 核数）
    std::string testMod; // --test-mod：只编译/运行指定模块的 test
};

// 运行 `riu test`。永不返回 (内部调 _exit)。
[[noreturn]] void runTestCommand(const TestCmdOptions& opts);

} // namespace riu::cli

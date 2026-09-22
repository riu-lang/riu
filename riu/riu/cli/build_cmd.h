// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `riu build` 子命令实现
//
// 从 riu/riu/main.cpp 抠出 (P1 Phase 1.b.iii.b):
// - 项目模式: 加载 riu.toml 产物（[library] / [[executable]]）, 调度 SDK / lib / exe,
//   走 codegen → obj → LLD 链接 → exe / lib;
// - 包级缓存 (PkgCacheRegistry) 控制增量编译。

#pragma once

#include <string>
#include <vector>

namespace riu::cli {

struct BuildCmdOptions {
    bool projectMode = false;          // buildCmd->parsed()
    bool emitIr = false;               // --emit-ir
    bool testMode = false;             // --test：编译 .test.ut 为独立 test exe (build/tests/)
    std::vector<std::string> testMods; // --test-mod：可重复；只编译这些测试模块
    int threads = 0;                   // --threads：测试编译并行度（0 = CPU 核数；1 = 进程内串行）
    std::string emitIrDir;             // --emit-ir-dir (IR 输出目录, 默认 build/)
    std::string buildNameArg;          // `riu build <name>` 可选产出名
};

// 运行 build 流程。
// 错误路径返回非 0 (走正常析构); 成功路径内部直接 _exit(0) (跳过 LLVM 静态析构)。
int runBuildCommand(const BuildCmdOptions& opts);

} // namespace riu::cli

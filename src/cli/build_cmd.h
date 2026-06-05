// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `yux build` 子命令实现 (含单文件模式弃用路径)
//
// 从 src/main.cpp 抠出 (P1 Phase 1.b.iii.b):
// - 项目模式: 加载 yux.toml + entry, 调度 SDK / lib / exe 三条路径,
//   走 codegen → obj → LLD 链接 → exe / lib;
// - 单文件模式: 仍兼容 `yux <file.yux>`, 走 pid 隔离的 .tmp 中间目录;
// - --jit-run spike: 单文件模式可走 LLJIT 直接执行, 不写 obj 不调 LLD;
// - 包级缓存 (PkgCacheRegistry) 控制增量编译。

#pragma once

#include <string>

namespace yux::cli {

struct BuildCmdOptions {
    bool projectMode = false; // buildCmd->parsed()
    bool emitIr = false;      // --emit-ir
    bool jitRun = false;      // --jit-run (spike, 仅单文件)
    std::string emitIrDir;    // --emit-ir-dir (IR 输出目录, 默认 build/)
    std::string buildNameArg; // `yux build <name>` 可选名 (与 yux.toml.name 校验)
    std::string inputFile;    // positional input (单文件路径; 项目模式应空)
    std::string helpText;     // app.help() 文本, 单文件模式缺参时打印
};

// 运行 build / 单文件流程。
// 错误路径返回非 0 (走正常析构); 成功路径内部直接 _exit(0) (跳过 LLVM 静态析构)。
int runBuildCommand(const BuildCmdOptions& opts);

} // namespace yux::cli

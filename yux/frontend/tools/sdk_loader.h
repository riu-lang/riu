// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// SDK 加载工具：把 sdk/yux/src/yux/core 下的源文件 parse + ASTBuild 到 Yux 中。
// 0 LLVM 依赖, 主程序与 yux-check 共用。
//
// 错误处理: 解析失败抛 YuxError (路径见 sourcePath()), 调用方决定如何渲染 / exit.
//
// 与 main.cpp 旧版的差异:
// - 旧版在出错时直接 reportRuntimeError + exit(1)。lifted 后改为 throw, 让
//   main.cpp 自己 catch + reportRuntimeError + exit; yux-check 走 renderYuxError。

#pragma once

#include <map>
#include <string>

class Yux;

struct SdkPkgEntry {
    std::string moduleName;
    bool isFlat;
};

namespace sdk_loader {

// 定位 SDK 目录 (返回 .../sdk/yux/src/yux/core), 找不到返回 "". Windows 专用。
std::string findSdkPath();

// 读 <sdkDir>/pkg 文件, 解析为 stem → SdkPkgEntry。
std::map<std::string, SdkPkgEntry> readSdkPkg(const std::string& sdkDir);

// 给非平铺导出在 _sdkFile 上登记模块别名。
void registerSdkPkgAliases(Yux& yux, const std::map<std::string, SdkPkgEntry>& pkgMap);

// 把 sdkDir 下所有非 .test.yux 解析进 yux。出错抛 YuxError (含解析失败 / AST 错误)。
// 错误时附带的 sourcePath 是触发错误的具体 .yux 文件。
void parseSdkDir(const std::string& sdkDir, Yux& yux);

}  // namespace sdk_loader

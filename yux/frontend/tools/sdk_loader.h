// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// SDK 加载工具：把 sdk/yux/src/yux/core 下的源文件 parse + ASTBuild 到 Yux 中，
// 并加载独立包（`../io.yux` → `yux.io`，`../time.yux` → `yux.time`，不扁平进 core）。
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
#include <vector>

class Yux;

struct SdkPkgEntry {
    std::string moduleName;
    std::string exportName;
    bool isFlat;
    bool isPublic;
};

// core 同级独立文件模块（`io.yux` → `yux.io`，`time.yux` → `yux.time`）。
struct SdkExtraPkg {
    std::string absPath;
    std::string moduleName;
};

namespace sdk_loader {

// 定位 SDK 目录 (返回 .../sdk/yux/src/yux/core), 找不到返回 "". Windows 专用。
std::string findSdkPath();

// 读 <sdkDir>/pkg 文件（与 Yux::parsePkgFile 共用 parsePkgFileAt）。
// 按源模块 stem 建表；保留导出名、扁平形态与是否公开，供 SDK parent scope 注册使用。
std::map<std::string, SdkPkgEntry> readSdkPkg(const std::string& sdkDir);

// 在 _sdkFile 上登记默认已导入的 `yux.core` 路径前缀：
// - 仅公开清单项登记导出名别名（`map.Map` / `math.abs`）
// - 包根 `yux` 只挂公开的 `core.<导出名>` 子路径（`yux.core.map.Map`）
// 不把 `yux` 做成可点任意子包的根：未 use 的包（`yux.io` / `yux.time`）不能靠包根漏出来。
void registerSdkModulePaths(Yux& yux, const std::map<std::string, SdkPkgEntry>& pkgMap);

// core 的父目录下、已存在的独立包源文件（用于登记路径、加载、以及 SDK 自构建 freshness）。
std::vector<SdkExtraPkg> extraSdkPackages(const std::string& sdkDir);

// 把 sdkDir 下所有非 .test.yux 解析进 yux。出错抛 YuxError (含解析失败 / AST 错误)。
// 错误时附带的 sourcePath 是触发错误的具体 .yux 文件。
// allowDecl：true 时优先读 .decl（依赖方只要接口 + 泛型体）；false 时整文件 parse
// （SDK 自构建且 obj 过期，需要非泛型体去做 codegen）。
void parseSdkDir(const std::string& sdkDir, Yux& yux, bool allowDecl = true);

} // namespace sdk_loader

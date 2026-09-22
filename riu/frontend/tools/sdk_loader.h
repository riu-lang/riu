// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// SDK 加载：core 始终 parse（`sdk/core/src/riu/core`）；stdlib 仅在图中
// 出现（或 riu-check `; require-sdk-modules`）时按 `[library]` 加载。
// 0 LLVM 依赖, 主程序与 riu-check 共用。
//
// 错误处理: 解析失败抛 RiuError (路径见 sourcePath()), 调用方决定如何渲染 / exit.

#pragma once

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

struct SdkPkgEntry {
    std::string moduleName;
    std::string exportName;
    bool isFlat;
    bool isPublic;
};

class Riu;

namespace sdk_loader {

// 定位 core 源码目录（`.../sdk/core/src/riu/core`），找不到返回 ""。Windows 专用。
std::string findSdkPath();

// 工具链 `sdk/<id>/`（含 riu.toml）。找不到返回 ""。
std::string findSdkPackage(const std::string& id);

// 读 <sdkDir>/pkg 文件（与 Riu::parsePkgFile 共用 parsePkgFileAt）。
// 按源模块 stem 建表；保留导出名、扁平形态与是否公开，供 SDK parent scope 注册使用。
std::map<std::string, SdkPkgEntry> readSdkPkg(const std::string& sdkDir);

// 在 _sdkFile 上登记默认已导入的 `riu.core` 路径前缀：
// - 仅公开清单项登记导出名别名（`map.Map` / `math.abs`）
// - 包根 `riu` 只挂公开的 `core.<导出名>` 子路径（`riu.core.map.Map`）
// 不把 `riu` 做成可点任意子包的根：未进图的包（`riu.io` / `riu.time`）不能靠包根漏出来。
void registerSdkModulePaths(Riu& riu, const std::map<std::string, SdkPkgEntry>& pkgMap);

// 把 core 源码目录下所有非 .test.ut 解析进 riu。出错抛 RiuError。
// allowDecl：true 时优先读 .ud；false 时整文件 parse。
// forceFullParseAbs：这些源文件绝对路径跳过 .ud，整文件 parse（core 自构建、obj 过期）。
void parseSdkDir(const std::string& sdkDir, Riu& riu, bool allowDecl = true,
                 const std::unordered_set<std::string>* forceFullParseAbs = nullptr);

// 把一个 SDK/库项目的 `[library]` 源码加载进 riu（不扁平进 core）。已加载的模块跳过。
void parseSdkLibrary(const std::string& pkgRoot, const std::string& libMod, Riu& riu, bool allowDecl = true);

} // namespace sdk_loader

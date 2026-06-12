// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件
//
// 从 src/main.cpp 抠出 (P1 Phase 1.b): build 目录推导, 单文件 parse/compile,
// SDK 目录批量 compile 与产物路径推导, 运行时错误统一渲染。
// 由 main.cpp 与未来拆分出的 build_cmd / test_cmd / sdk_compile_cmd 共用。

#pragma once

#include "types.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <stdexcept>
#include <string>

class Yux;

namespace yux::cli {

std::string getBuildDir(const std::string& projectRoot);
void ensureBuildDir(const std::string& buildDir);

// 模块名 → 构建产物基础路径 (不含扩展名)
std::string moduleOutputBase(const std::string& buildDir, const std::string& projectName,
                             const std::string& moduleName);

bool compileIRToObj(llvm::Module* module, const std::string& outputPath);

struct IRResult {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

// 把 runtime_error 渲染成统一格式的诊断到 stderr。
// sourcePath 提供文件名 (可空), 用于源码片段查找与错误头打印。
// prefix 为可选前缀, 写在诊断头之前; 对非 YuxError 异常仅打印 prefix + msg。
void reportRuntimeError(const std::string& sourcePath, const std::runtime_error& e, const std::string& prefix = "");

void parseAST(const std::string& inputFile, Yux& yux, bool isSdk = false);
IRResult compileIR(const std::string& inputFile, Yux& yux, bool isSdk = false);

// SDK 构建路径 (与项目模式一致):
//   sdkRoot = <sdk-project-root> (含 yux.toml)
//   .lib    = sdkRoot/build/yux.lib                (最终产物, 去掉 <name>/ 子层)
//   .objDir = sdkRoot/build/src/yux/core/          (中间产物目录, 每 .yux 一个 .obj)
//   .irDir  = sdkRoot/build/src/yux/core/          (IR 输出目录)
// sdkPath 形如 .../sdk/yux/src/yux/core, 向上 3 级即 sdkRoot。
struct SdkPaths {
    std::string objDir;
    std::string libPath;
    std::string irDir;
};

SdkPaths sdkBuildPaths(const std::string& sdkPathAbs);
bool needRecompileSdkDir(const std::string& sdkDir, const std::string& sdkObjDir);

// 薄壳: 捕获 sdk_loader::parseSdkDir 的 YuxError 并 reportRuntimeError + exit(1)。
void parseSdkDirOrExit(const std::string& sdkDir, Yux& yux);

void compileSdkDir(const std::string& sdkDir, Yux& yux);

} // namespace yux::cli

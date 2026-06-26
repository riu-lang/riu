// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件
//
// build 目录推导, IR→Obj 编译, 运行时错误统一渲染。
// 由 main.cpp / build_cmd / test_cmd 共用。

#pragma once

#include "../types.h"

#include <llvm/IR/Module.h>

#include <stdexcept>
#include <string>

namespace yux::cli {

std::string getBuildDir(const std::string& projectRoot);
void ensureBuildDir(const std::string& buildDir);

// 模块名 → 构建产物基础路径 (不含扩展名)
std::string moduleOutputBase(const std::string& buildDir, const std::string& projectName,
                             const std::string& moduleName);

bool compileIRToObj(llvm::Module* module, const std::string& outputPath);

// 把 runtime_error 渲染成统一格式的诊断到 stderr。
// sourcePath 提供文件名 (可空), 用于源码片段查找与错误头打印。
// prefix 为可选前缀, 写在诊断头之前; 对非 YuxError 异常仅打印 prefix + msg。
void reportRuntimeError(const std::string& sourcePath, const std::runtime_error& e, const std::string& prefix = "");

} // namespace yux::cli

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件
//
// build 目录推导, IR→Obj 编译, 运行时错误统一渲染。
// 由 main.cpp / build_cmd / test_cmd 共用。

#pragma once

#include "types.h"

#include <llvm/IR/Module.h>

#include <stdexcept>
#include <string>

namespace riu::cli {

std::string getBuildDir(const std::string& projectRoot);
void ensureBuildDir(const std::string& buildDir);

bool compileIRToObj(llvm::Module* module, const std::string& outputPath);

// 把 runtime_error 渲染成统一格式的诊断到 stderr。
// sourcePath 提供正在编译的入口文件 (可空)；RiuError 若自带 file() 则优先用节点所属文件。
// prefix 为可选前缀, 写在诊断头之前; 对非 RiuError 异常仅打印 prefix + msg。
void reportRuntimeError(const std::string& sourcePath, const std::runtime_error& e, const std::string& prefix = "");

} // namespace riu::cli

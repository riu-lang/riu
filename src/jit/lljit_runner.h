// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 进程内 LLJIT 运行器
//
// 把用户 IR 模块与 sdk core.obj 装载进 ORC LLJIT, 解析 mainStartup 直接调用。
// 通过 makeYuxObjectLinkingLayer 注入 YuxSEHMemoryManager, 修复 Win64 SEH
// 跨帧 unwind (见 seh_memory_manager.h)。
//
// 用于 `yux test` 子命令的 in-process 测试运行; 绕过 obj 写盘 + LLD 链接。

#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <string>
#include <vector>

namespace yux::jit {

// 给 LLJITBuilder 用: 构造一个 RTDyldObjectLinkingLayer, 每个对象使用一个
// YuxSEHMemoryManager 实例 (用于 .pdata SEH 注册)。
llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> makeYuxObjectLinkingLayer(llvm::orc::ExecutionSession& ES);

// 装载用户主模块 + 额外模块 + sdk core.obj, lookup mainStartup 调用, 返回退出码。
int runViaJIT(std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::LLVMContext> ctx,
              const std::vector<std::unique_ptr<llvm::Module>>& extraMods,
              std::vector<std::unique_ptr<llvm::LLVMContext>>& extraCtxs, const std::string& sdkObjPath);

} // namespace yux::jit

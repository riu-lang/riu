// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LLJIT 运行器实现
//
// 见 lljit_runner.h 头注释; 本文件从 src/main.cpp 拆出, 不变动语义。

#include "lljit_runner.h"

#include "seh_memory_manager.h"

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>

namespace yux::jit {

// 给 LLJITBuilder 用: 构造一个 RTDyldObjectLinkingLayer, 每个对象使用一个
// YuxSEHMemoryManager 实例 (用于 .pdata SEH 注册)。
llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
makeYuxObjectLinkingLayer(llvm::orc::ExecutionSession& ES) {
    auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
        ES,
        [](const llvm::MemoryBuffer&) -> std::unique_ptr<llvm::RuntimeDyld::MemoryManager> {
            return std::make_unique<YuxSEHMemoryManager>();
        });
    // 与 LLJIT 默认 COFF 路径一致 (LLJIT.cpp::createObjectLinkingLayer)
    layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
    layer->setAutoClaimResponsibilityForObjectSymbols(true);
    return std::unique_ptr<llvm::orc::ObjectLayer>(std::move(layer));
}

// Phase 1 spike: build a user IR module and run via in-process LLJIT.
// 加载预编译 sdk core.obj 作为对象层符号源, 再加用户 IR; 用 process loader
// 兜底解析 kernel32 等动态库符号; 查 mainStartup 直接调用并返回退出码。
//
// 该路径绕过 obj 写盘 + LLD 链接, 单次成功用例从 ~2.1s 降到 IR 生成 + JIT 装载耗时。
// 仅供 Phase 1 验证; Phase 2 起会被 `yux test` 子命令收编。
int runViaJIT(std::unique_ptr<llvm::Module> mod,
              std::unique_ptr<llvm::LLVMContext> ctx,
              const std::vector<std::unique_ptr<llvm::Module>>& extraMods,
              std::vector<std::unique_ptr<llvm::LLVMContext>>& extraCtxs,
              const std::string& sdkObjPath) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jitOrErr = llvm::orc::LLJITBuilder()
        .setObjectLinkingLayerCreator(&makeYuxObjectLinkingLayer)
        .create();
    if (!jitOrErr) {
        llvm::errs() << "[jit] LLJIT create failed: "
                     << llvm::toString(jitOrErr.takeError()) << "\n";
        return 1;
    }
    auto& jit = *jitOrErr;
    auto& jd = jit->getMainJITDylib();

    // 进程内符号兜底 (kernel32: HeapAlloc, GetStdHandle, WriteFile, ...)
    auto procGen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!procGen) {
        llvm::errs() << "[jit] process generator failed: "
                     << llvm::toString(procGen.takeError()) << "\n";
        return 1;
    }
    jd.addGenerator(std::move(*procGen));

    // 加载 sdk core.obj
    if (!sdkObjPath.empty() && std::filesystem::exists(sdkObjPath)) {
        auto bufOrErr = llvm::MemoryBuffer::getFile(sdkObjPath);
        if (!bufOrErr) {
            llvm::errs() << "[jit] read sdk obj failed: " << sdkObjPath << "\n";
            return 1;
        }
        if (auto e = jit->addObjectFile(std::move(*bufOrErr))) {
            llvm::errs() << "[jit] addObjectFile failed: "
                         << llvm::toString(std::move(e)) << "\n";
            return 1;
        }
    } else {
        llvm::errs() << "[jit] warning: sdk obj not found at " << sdkObjPath << "\n";
    }

    // 用户主模块
    mod->setDataLayout(jit->getDataLayout());
    llvm::orc::ThreadSafeModule mainTsm(std::move(mod), std::move(ctx));
    if (auto e = jit->addIRModule(std::move(mainTsm))) {
        llvm::errs() << "[jit] addIRModule(main) failed: "
                     << llvm::toString(std::move(e)) << "\n";
        return 1;
    }

    // 用户导入模块
    for (size_t i = 0; i < extraMods.size(); ++i) {
        auto& m = const_cast<std::unique_ptr<llvm::Module>&>(extraMods[i]);
        if (!m) continue;
        m->setDataLayout(jit->getDataLayout());
        llvm::orc::ThreadSafeModule tsm(std::move(m), std::move(extraCtxs[i]));
        if (auto e = jit->addIRModule(std::move(tsm))) {
            llvm::errs() << "[jit] addIRModule(extra) failed: "
                         << llvm::toString(std::move(e)) << "\n";
            return 1;
        }
    }

    auto sym = jit->lookup("mainStartup");
    if (!sym) {
        llvm::errs() << "[jit] lookup mainStartup failed: "
                     << llvm::toString(sym.takeError()) << "\n";
        return 1;
    }
    auto fn = sym->toPtr<int (*)()>();
    return fn();
}

} // namespace yux::jit

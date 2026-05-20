// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Win64 SEH 修复: JIT 段 .pdata 注册
//
// 默认 RTDyldMemoryManager::registerEHFramesInProcess 只调用 __register_frame
// (libgcc DWARF unwind) 不调 RtlAddFunctionTable, 所以 RuntimeDyldCOFFX86_64
// 收集到的 .pdata 段从来没有真正注册到 OS。本类子类化 SectionMemoryManager
// 覆盖 registerEHFrames/deregisterEHFrames, 直接调 RtlAddFunctionTable。
// .pdata 是 RUNTIME_FUNCTION (3 个 DWORD RVA) 紧凑数组; ImageBase 取本对象
// 内已分配 section 的最低非零地址, 与 RuntimeDyldCOFFX86_64::getImageBase()
// 一致 (RTDyldObjectLinkingLayer 每次 emit GetMemoryManager(), 一实例一 obj)。

#pragma once

#include <windows.h>

#undef ERROR

#include <llvm/ExecutionEngine/SectionMemoryManager.h>

#include <cstdint>
#include <vector>

namespace yux::jit {

class YuxSEHMemoryManager : public llvm::SectionMemoryManager {
public:
    YuxSEHMemoryManager() = default;
    ~YuxSEHMemoryManager() override;

    uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                 llvm::StringRef SectionName) override;

    uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, llvm::StringRef SectionName,
                                 bool IsReadOnly) override;

    void registerEHFrames(uint8_t* Addr, uint64_t LoadAddr, size_t Size) override;
    void deregisterEHFrames() override;

private:
    std::vector<uint64_t> sectionAddrs;
    std::vector<PRUNTIME_FUNCTION> registeredTables;

    void recordSection(uint8_t* p);
};

} // namespace yux::jit

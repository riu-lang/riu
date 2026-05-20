// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// YuxSEHMemoryManager 实现
//
// 见 seh_memory_manager.h 头注释; 本文件仅放方法体, 不变动语义。

#include "seh_memory_manager.h"

#include <algorithm>
#include <limits>

namespace yux::jit {

YuxSEHMemoryManager::~YuxSEHMemoryManager() {
    for (auto* table : registeredTables) {
        ::RtlDeleteFunctionTable(table);
    }
}

uint8_t* YuxSEHMemoryManager::allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                                  unsigned SectionID,
                                                  llvm::StringRef SectionName) {
    auto* p = SectionMemoryManager::allocateCodeSection(
        Size, Alignment, SectionID, SectionName);
    if (p) recordSection(p);
    return p;
}

uint8_t* YuxSEHMemoryManager::allocateDataSection(uintptr_t Size, unsigned Alignment,
                                                  unsigned SectionID,
                                                  llvm::StringRef SectionName,
                                                  bool IsReadOnly) {
    auto* p = SectionMemoryManager::allocateDataSection(
        Size, Alignment, SectionID, SectionName, IsReadOnly);
    if (p) recordSection(p);
    return p;
}

void YuxSEHMemoryManager::registerEHFrames(uint8_t* Addr, uint64_t /*LoadAddr*/,
                                            size_t Size) {
    // .pdata 段必须是 RUNTIME_FUNCTION (12 字节) 的紧凑数组
    constexpr size_t kEntrySize = sizeof(RUNTIME_FUNCTION);
    if (Size == 0 || Size % kEntrySize != 0) return;

    uint64_t imageBase = std::numeric_limits<uint64_t>::max();
    for (uint64_t a : sectionAddrs) {
        if (a != 0) imageBase = std::min(imageBase, a);
    }
    if (imageBase == std::numeric_limits<uint64_t>::max()) return;

    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(Addr);
    DWORD count = static_cast<DWORD>(Size / kEntrySize);
    if (::RtlAddFunctionTable(table, count, imageBase)) {
        registeredTables.push_back(table);
    }
}

void YuxSEHMemoryManager::deregisterEHFrames() {
    for (auto* table : registeredTables) {
        ::RtlDeleteFunctionTable(table);
    }
    registeredTables.clear();
}

void YuxSEHMemoryManager::recordSection(uint8_t* p) {
    sectionAddrs.push_back(reinterpret_cast<uint64_t>(p));
}

} // namespace yux::jit

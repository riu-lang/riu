// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 目标 C ABI 布局（0 LLVM）：Win64 标量宽/对齐 + 用户 struct 的 #Packed / #Align。
// Sema（overlay / align_of 诊断）与 codegen（size_of / packed LLVM / byval align）共用。

#ifndef RIU_LANG_LAYOUT_H
#define RIU_LANG_LAYOUT_H

#include "types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class FileNode;
class StructDeclNode;

namespace layout {

struct AbiLayout {
    uint64_t size = 0;
    uint64_t align = 1;
    std::vector<uint64_t> fieldOffsets; // 与 StructDeclNode::fields() 等长；非 struct 为空
};

[[nodiscard]] inline uint64_t alignUp(uint64_t n, uint64_t a) {
    if (a <= 1) return n;
    return (n + a - 1) & ~(a - 1);
}

// 2 的幂且 1..2^28；非法返回 0。
[[nodiscard]] uint64_t parseAlignArg(std::string_view text);

[[nodiscard]] bool isPacked(const StructDeclNode* sd);
// struct 上 #Align(N)；无则 0。
[[nodiscard]] uint64_t structAlignN(const StructDeclNode* sd);
[[nodiscard]] bool hasCustomLayout(const StructDeclNode* sd);

// 算不出（未知名 / 形参 / 环）→ nullopt。
[[nodiscard]] std::optional<AbiLayout> tryAbiLayout(const TypeInfo& t, FileNode* file, FileNode* sdkFile);

// C-layout 白名单（§7.5.3.4）：标量无 bool / Ptr / 嵌套 C-layout / [T*N]。
[[nodiscard]] bool isCLayoutType(const TypeInfo& t, FileNode* file, FileNode* sdkFile);

} // namespace layout

#endif

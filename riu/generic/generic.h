// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 泛型单态化（`riu_generic`）。2.1 空壳：类型名占位，行为仍在 Compiler / SemaPass。
// 2.2 起迁替换栈与实例表。本库 0 LLVM，不 include SemaPass 实现。

#ifndef RIU_LANG_GENERIC_H
#define RIU_LANG_GENERIC_H

namespace generic {

struct StructInstance {};
struct FnInstance {};
struct SubstFrame {};

} // namespace generic

#endif // RIU_LANG_GENERIC_H

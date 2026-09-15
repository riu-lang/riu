// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_SEMA_NAME_RESOLVER_H
#define RIU_LANG_SEMA_NAME_RESOLVER_H

#include "ast/name_lookup.h"

namespace sema {

// 顶层别名一次性校验：E2017 名字冲突 + E2016 环 + fn 符号表归一化。
// SemaPass::run 起始处调一次；Compiler 不再双跑。查找实现见 ast/name_lookup.h。
void validateAliases(FileNode* file, FileNode* sdkFile = nullptr);

} // namespace sema

#endif // RIU_LANG_SEMA_NAME_RESOLVER_H

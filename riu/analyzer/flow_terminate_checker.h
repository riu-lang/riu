// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_FLOW_TERMINATE_CHECKER_H
#define RIU_LANG_FLOW_TERMINATE_CHECKER_H

#include "ast/node/fn_node.h"

// E7014 流终止检查（DRAFT-错误.md §8.3 / spec §11.5.1）：
// 仅对带 `#NoReturn` 注解的函数生效；要求 body 控制流必须以以下之一终止——
//   ret / ret void、loop {} 无 break、if-else 全分支终止、match 全 arm 终止、
//   或调用一个 `#NoReturn` 函数。
// SemaPass::run 之后由 driver 对每个 fn 调用一次（见 runFnCheckers），
// 与 borrow_checker 同档。
void checkFlowTerminate(FnNode* fn);

#endif // RIU_LANG_FLOW_TERMINATE_CHECKER_H

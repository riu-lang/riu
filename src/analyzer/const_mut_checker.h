// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_CONST_MUT_CHECKER_H
#define YUX_LANG_CONST_MUT_CHECKER_H

#include "ast/node/fn_node.h"

// DRAFT-const-mut §3 静态检查（P1-2 骨架）：
//
// 当前覆盖：
//   §3.3 局部 `cval` 初值约束（E3104）——
//     RHS 只允许由
//       基本数值 / `bool` / `null` / 字符串 / 码点字面量，
//       对已声明 `cval`（局部或全局）的引用，
//       以及上述组合而成的常量表达式（一元、加减乘除模、位运算、比较、逻辑、括号）
//     构成。其他形态（函数调用 / 构造 / 字符串模板 / 数组 / 成员访问 / if-else 等）一律拒绝。
//
// 暂未覆盖（按 CURRENT.md 阶段表，留 P1-3 / P1-4 / P1-5）：
//   §4 #Const fn 体内写操作约束；
//   §5 #Frozen 参数写约束与传染；
//   §6 字段 #Val / #Frozen。
//
// 在 compileFn / compileMethod 入口处调用一次，位置与 checkBorrows / checkFlowTerminate 同档。
void checkConstMut(p<FnNode> fn);

#endif // YUX_LANG_CONST_MUT_CHECKER_H

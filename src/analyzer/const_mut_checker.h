// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_CONST_MUT_CHECKER_H
#define YUX_LANG_CONST_MUT_CHECKER_H

#include "ast/node/fn_node.h"

// DRAFT-const-mut 静态检查（const_mut_checker）：
//
// 当前覆盖：
//   §3.3 局部 `cval` 初值约束（E3104）——
//     RHS 只允许由
//       基本数值 / `bool` / `null` / 字符串 / 码点字面量，
//       对已声明 `cval`（局部或全局）的引用，
//       以及上述组合而成的常量表达式（一元、加减乘除模、位运算、比较、逻辑、括号）
//     构成。其他形态（函数调用 / 构造 / 字符串模板 / 数组 / 成员访问 / if-else 等）一律拒绝。
//
//   §5.2 `#Frozen` 参数 / §5.4 传染所得 `#Frozen` 本地——
//     - 重赋（含整体 `=`）共享 E3093 路径（writeable=false，参数默认 val + frozen 标）。
//     - 字段写 `s.f = ...` / 数组写 `s[i] = ...` → E3106。
//     - 任何"可写槽位"承接 `#Frozen` 表达式（局部 var/val 声明 / 已声明可写局部重赋）
//       → E3107；唯一脱 const 出口为 builtin `copy_of:<T>(...)`，§5.3/§5.4。
//
// 暂未覆盖（按 CURRENT.md 阶段表，留 P1-3-followup / P1-4 / P1-5）：
//   §4 #Const fn 体内写操作约束；
//   §5.2.4 / §5.4 调用点：把 #Frozen 实参传给非 #Frozen 形参的拒收（要 callee 参数表）；
//   §6 字段 #Val / #Frozen。
//
// 在 compileFn / compileMethod 入口处调用一次，位置与 checkBorrows / checkFlowTerminate 同档。
void checkConstMut(p<FnNode> fn);

#endif // YUX_LANG_CONST_MUT_CHECKER_H

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_CONST_MUT_CHECKER_H
#define RIU_LANG_CONST_MUT_CHECKER_H

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
//     - 重赋（含整体 `=`）共享 E3093 路径（writeable=false，参数默认不可重赋 + frozen 标）。
//       DRAFT-let-unify §3：`let x` 默认走该路径（writeable=false），`#Mut let x` 才放宽；
//       因此局部不可重赋的诊断不需要 checker 显式触发，由 compiler_stmt.cpp 兜底。
//     - 字段写 `s.f = ...` / 数组写 `s[i] = ...` → E3106。
//     - 任何"可写槽位"承接 `#Frozen` 表达式（局部 var/val 声明 / 已声明可写局部重赋）
//       → E3107；唯一脱 const 出口为 builtin `copy_of:<T>(...)`，§5.3/§5.4。
//
//   §6.2 / §6.3 字段层 #Val / #Frozen 写白名单 ——
//     - 字段 #Val 浅、#Frozen 深；构造函数 (`fn StructName(...)`) 内可写，其他成员函数 / 自由函数禁写。
//     - 析构函数 (`fn ~()`) 不受 const-mut 约束（[#1.L]）。
//     - 写 `obj.f = ...`：subs.size()==1 时 #Val/#Frozen 均拒；subs.size()>1 时仅 #Frozen 拒（深传染）。
//     - 跨模块查 struct 走 FileNode::getStructOwner。
//
//   §4.2 `#Const fn` 体内写操作 / 调用约束（E3110 / E3111）——
//     被 #Const 标记的 fn / 方法体内禁止：
//       1. 写 `$.f` / `$[i]`（成员函数情形）—— E3110；
//       2. 写参数的字段 `p.f = ...` / `p[i] = ...` —— E3110；
//       3. 写非 `cval` 的全局符号（rebind 或字段写）—— E3110；
//       4. 调任何非 `#Const` 的自由函数 / 方法 —— E3111。
//     允许：读 $/参数/全局 val/cval；声明并写函数体内局部 var/val/cval；
//     调其它 #Const fn / #Const 方法。本 P1-5 仅识别:
//       - 自由函数调用 (callee 为简单标识符) → 当前 file 的 fnSymbols 查表;
//       - 方法调用 (callee 为 ExprDot, receiver 是已知变量) → 解析 receiver 类型,
//         查 `StructName.methodName`. 跨模块只走 file 自身 + wildcardImports 顺序。
//     无法解析的复杂 callee（链式调用、lambda 返回、enum ctor 等）暂按"未知"放行，
//     交由 P1-6+ 继续收紧。
//
// 暂未覆盖（按 CURRENT.md 阶段表）：
//   §5.2.4 / §5.4 调用点：把 #Frozen 实参传给非 #Frozen 形参的拒收（要 callee 参数表）；
//   §6.4 含 #Frozen 字段类型的传递约束（callsite 检查）；
//   §6.2 数组写 `obj[i] = ...` 形态（StatementSetNode 含字段链）；
//   字段深链 `$.f.g = ...` 中第二层及以后字段的 #Val/#Frozen 解析（需逐级类型推断）。
//
// SemaPass::run 之后由 driver 对每个 fn 调用一次（见 runFnCheckers），
// 位置与 checkBorrows / checkFlowTerminate 同档。
void checkConstMut(FnNode* fn);

#endif // RIU_LANG_CONST_MUT_CHECKER_H

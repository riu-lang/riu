// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_BORROW_CHECKER_H
#define YUX_LANG_BORROW_CHECKER_H

#include "node/fn_node.h"

// 借用寿命检查（spec §8.6.5 / §8.6.5.8）：块作用域栈，O(1) 局部规则；
// 借用期内根对象不可重赋（§8.6.5.5）。
// 在 compileFn / compileMethod 入口处调用一次。
// selfStructName 非空表示方法（注册 `$` 作为有效根对象名）。
void checkBorrows(p<FnNode> fn, const std::string& selfStructName = "");

#endif // YUX_LANG_BORROW_CHECKER_H

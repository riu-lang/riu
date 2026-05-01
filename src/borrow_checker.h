// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_BORROW_CHECKER_H
#define YUX_LANG_BORROW_CHECKER_H

#include "node/fn_node.h"

// Phase 4d 寿命检查（O(1) 静态规则）+ 借用期根对象不可重赋。
// 在 compileFn / compileMethod 入口处调用一次。
// selfStructName 非空表示方法（注册 `$` 作为有效根对象名）。
void checkBorrows(p<FnNode> fn, const std::string& selfStructName = "");

#endif // YUX_LANG_BORROW_CHECKER_H

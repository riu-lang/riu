// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_CTOR_DAA_H
#define YUX_LANG_CTOR_DAA_H

#include "ast/node/fn_node.h"

#include <string>
#include <vector>

// Phase 6 构造函数定性赋值分析（DAA，Definite Assignment Analysis）。
// 入口：fn 是构造函数（方法名 == structName 且非析构）。
// fieldNames 按声明顺序给出全部字段名。
// 检查规则见 DRAFT §8.2 / §8.3 / CURRENT.md Phase 6。
void checkConstructorDAA(p<FnNode> fn,
                         const std::string& structName,
                         const std::vector<std::string>& fieldNames);

#endif //YUX_LANG_CTOR_DAA_H

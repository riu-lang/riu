// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 拼写近似建议
//
// 根据当前作用域链中可见的符号 / 函数名，对未声明标识符给出最近 1–3 个候选，
// 用于 E3030 / E3031 / E3032 等"符号未找到"诊断的 help 提示。
//
// 距离阈值默认 Levenshtein ≤ 2；候选按 (距离, 名称) 升序排序，至多取 k 个。
// 无候选时返回空串/空 vector，由调用方决定是否附 hint。

#ifndef YUX_LANG_SYMBOL_SUGGEST_H
#define YUX_LANG_SYMBOL_SUGGEST_H

#include "../types.h"

class ScopeNode;

namespace SymbolSuggest {
    // 收集 scope 及父链所有可见符号 / 函数名，返回与 target 距离 ≤ maxDist 的最近 k 个
    vector<string> nearby(ScopeNode* scope, const string& target,
                          int maxDist = 2, int k = 3);

    // 一步到位：直接构造 hint 字符串（无候选返回空串）
    // 单候选："did you mean `foo`?"
    // 多候选："did you mean one of: `foo`, `bar`?"
    string buildHint(ScopeNode* scope, const string& target,
                     int maxDist = 2, int k = 3);

    // 抛"符号未找到"诊断的便利函数：自动附拼写近似 hint（无候选时不附）
    // 用于 E3030 / E3031 / E3032 等单参数错误码的统一抛点
    [[noreturn]] void throwSymbolNotFound(ScopeNode* scope,
                                          int line, int col,
                                          const ErrorCodeDef& ec,
                                          const string& name);
}

#endif // YUX_LANG_SYMBOL_SUGGEST_H

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 拼写近似建议实现
//
// 本文件包含 E3030 / E3031 / E3032 等"符号未找到"诊断的 hint 候选生成:
// - Levenshtein 编辑距离（带 cutoff 提前剪枝）
// - 沿 ScopeNode 父链汇总可见变量与函数名
// - 按 (距离, 名称) 排序后取前 k 个

#include <algorithm>
#include "ast/node/node.h"
#include <set>
#include "symbol_suggest.h"
#include <utility>

namespace {

    // 计算编辑距离；超过 cutoff 直接返回 cutoff + 1（用于剪枝，调用方仅关心 ≤ cutoff 的值）
    int levenshtein(const string& a, const string& b, int cutoff) {
        int n = static_cast<int>(a.size());
        int m = static_cast<int>(b.size());
        if (std::abs(n - m) > cutoff) return cutoff + 1;
        if (n == 0) return m;
        if (m == 0) return n;

        vector<int> prev(m + 1), cur(m + 1);
        for (int j = 0; j <= m; ++j) prev[j] = j;
        for (int i = 1; i <= n; ++i) {
            cur[0] = i;
            int rowMin = cur[0];
            for (int j = 1; j <= m; ++j) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                int v = prev[j - 1] + cost;
                int del = prev[j] + 1;
                int ins = cur[j - 1] + 1;
                if (del < v) v = del;
                if (ins < v) v = ins;
                cur[j] = v;
                if (v < rowMin) rowMin = v;
            }
            // 整行最小值已超 cutoff，后续不可能更优
            if (rowMin > cutoff) return cutoff + 1;
            std::swap(prev, cur);
        }
        return prev[m];
    }

} // namespace

vector<string> SymbolSuggest::nearby(ScopeNode* scope, const string& target,
                                     int maxDist, int k) {
    if (!scope || target.empty() || k <= 0) return {};

    vector<pair<int, string>> scored;
    std::set<string> seen;

    // 沿父链遍历；同名优先采用更内层（先访问到的）作用域，故已 seen 的跳过
    for (ScopeNode* s = scope; s != nullptr; s = s->parentScope()) {
        for (auto& kv : s->localSymbols()) {
            const string& name = kv.first;
            if (name == target) continue;
            if (!seen.insert(name).second) continue;
            int d = levenshtein(target, name, maxDist);
            if (d <= maxDist) scored.emplace_back(d, name);
        }
        for (auto& kv : s->localFnSymbols()) {
            const string& name = kv.first;
            if (name == target) continue;
            if (!seen.insert(name).second) continue;
            int d = levenshtein(target, name, maxDist);
            if (d <= maxDist) scored.emplace_back(d, name);
        }
    }

    std::ranges::sort(scored,
              [](const pair<int, string>& a, const pair<int, string>& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });

    vector<string> result;
    for (auto& p : scored) {
        if (std::cmp_greater_equal(result.size(), k)) break;
        result.push_back(p.second);
    }
    return result;
}

string SymbolSuggest::buildHint(ScopeNode* scope, const string& target,
                                int maxDist, int k) {
    auto cands = nearby(scope, target, maxDist, k);
    if (cands.empty()) return {};
    if (cands.size() == 1) {
        return "did you mean `" + cands[0] + "`?";
    }
    string s = "did you mean one of: ";
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i) s += ", ";
        s += "`" + cands[i] + "`";
    }
    s += '?';
    return s;
}

void SymbolSuggest::throwSymbolNotFound(ScopeNode* scope,
                                        int line, int col,
                                        const ErrorCodeDef& ec,
                                        const string& name) {
    auto err = YuxError(line, col, ec, name);
    auto h = buildHint(scope, name);
    if (!h.empty()) err.withHint(std::move(h));
    throw err;
}

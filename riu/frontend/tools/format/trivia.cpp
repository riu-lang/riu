// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// trivia 扫描实现
//
// 线性扫描 rd Scanner::nextRaw()：
// - hidden 的 LineComment / LineEndComment 收集为 TriviaComment；
//   行尾注释 (LineEndComment, 紧跟在某 default token 同一行后) 挂到该 default
//   token 的 trailing 桶；行首注释挂到"下一个 default token"的 leading 桶；
// - 通过相邻两个 default token 之间出现的换行数 (>=2) 判定空行，写入
//   blankBefore[nextDefaultIdx] = true。

#include "tools/format/trivia.h"

#include "ast/rd/scanner.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace riu::format {

namespace {

bool isHidden(rd::Kind k) {
    return k == rd::Kind::Space || k == rd::Kind::LineComment || k == rd::Kind::LineEndComment;
}

// 把 lexer 抓到的注释原文规整成"纯注释主体"：剥去前导空格 (LineComment 词法
// 规则把行首空格也吞进 token，列对齐由格式化器外层负责) 和尾随的 \r\n
// (LineComment 末尾固定有一个 LineEnd)
std::string trimCommentText(std::string_view s) {
    std::size_t a = 0;
    while (a < s.size() && s[a] == ' ')
        ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == '\n' || s[b - 1] == '\r'))
        --b;
    return std::string(s.substr(a, b - a));
}

} // namespace

TriviaScan scanTrivia(std::string_view src) {
    TriviaScan out;
    rd::Scanner sc(src);

    auto lastDefault = static_cast<std::size_t>(-1);
    std::size_t lastDefaultLine = 0;
    std::vector<TriviaComment> pendingLeading;

    for (;;) {
        rd::Token tk = sc.nextRaw();
        if (tk.kind == rd::Kind::Eof) break;
        const auto type = tk.kind;
        const std::string_view rawText = tk.text;

        if (!isHidden(type) && type != rd::Kind::Invalid) {
            const auto idx = static_cast<std::size_t>(tk.index);
            out.defaultToks.push_back(tk);
            // LineEnd 自身也走 default 通道（每行一个），不能用它更新行号锚，
            // 否则会把空行的 LineEnd 当作"上一行"把空行吃掉
            if (type == rd::Kind::LineEnd) continue;

            std::size_t prevLine = pendingLeading.empty() ? lastDefaultLine : pendingLeading.back().line;
            if (std::cmp_not_equal(lastDefault, -1) && static_cast<std::size_t>(tk.pos.line) > prevLine + 1) {
                out.map.blankBefore[idx] = true;
            }
            if (!pendingLeading.empty()) {
                if (static_cast<std::size_t>(tk.pos.line) > pendingLeading.back().line + 1) {
                    out.map.blankAfterLeading[idx] = true;
                }
                out.map.leadingByTokenIndex[idx] = std::move(pendingLeading);
                pendingLeading.clear();
            }
            lastDefault = idx;
            lastDefaultLine = static_cast<std::size_t>(tk.pos.line);
            continue;
        }

        if (type == rd::Kind::LineEndComment) {
            if (std::cmp_not_equal(lastDefault, -1) && std::cmp_equal(tk.pos.line, lastDefaultLine)) {
                out.map.trailingByTokenIndex[lastDefault].push_back({.text = trimCommentText(rawText),
                                                                     .isLineComment = true,
                                                                     .line = static_cast<std::size_t>(tk.pos.line),
                                                                     .blankBefore = false});
            } else {
                std::size_t prevLine = pendingLeading.empty() ? lastDefaultLine : pendingLeading.back().line;
                bool blank = (std::cmp_not_equal(lastDefault, -1) || !pendingLeading.empty()) &&
                             static_cast<std::size_t>(tk.pos.line) > prevLine + 1;
                pendingLeading.push_back({.text = trimCommentText(rawText),
                                          .isLineComment = true,
                                          .line = static_cast<std::size_t>(tk.pos.line),
                                          .blankBefore = blank});
            }
        } else if (type == rd::Kind::LineComment) {
            std::size_t prevLine = pendingLeading.empty() ? lastDefaultLine : pendingLeading.back().line;
            bool blank = (std::cmp_not_equal(lastDefault, -1) || !pendingLeading.empty()) &&
                         static_cast<std::size_t>(tk.pos.line) > prevLine + 1;
            pendingLeading.push_back({.text = trimCommentText(rawText),
                                      .isLineComment = true,
                                      .line = static_cast<std::size_t>(tk.pos.line),
                                      .blankBefore = blank});
        }
    }

    return out;
}

TriviaMap buildTrivia(std::string_view src) {
    return scanTrivia(src).map;
}

} // namespace riu::format

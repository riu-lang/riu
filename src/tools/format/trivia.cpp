// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// trivia 扫描实现
//
// 线性扫描 CommonTokenStream 的所有 token：
// - HIDDEN 通道的 LineComment / LineEndComment 收集为 TriviaComment；
//   行尾注释 (LineEndComment, 紧跟在某 default token 同一行后) 挂到该 default
//   token 的 trailing 桶；行首注释挂到"下一个 default token"的 leading 桶；
// - 通过相邻两个 default token 之间出现的换行数 (>=2) 判定空行，写入
//   blankBefore[nextDefaultIdx] = true。

#include "tools/format/trivia.h"

#include <cstddef>
#include <string>
#include <vector>

#include "antlr4-runtime.h"
#include "yux/yuxLexer.h"

namespace yux::format {

namespace {

// 把 lexer 抓到的注释原文规整成"纯注释主体"：剥去前导空格 (LineComment 词法
// 规则把行首空格也吞进 token，列对齐由格式化器外层负责) 和尾随的 \r\n
// (LineComment 末尾固定有一个 LineEnd)
std::string trimCommentText(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && s[a] == ' ') ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

} // namespace

TriviaMap buildTrivia(antlr4::CommonTokenStream& stream) {
    TriviaMap map;
    stream.fill();
    const auto& toks = stream.getTokens();

    std::size_t lastDefault = static_cast<std::size_t>(-1);
    std::size_t lastDefaultLine = 0;
    std::vector<TriviaComment> pendingLeading;

    for (auto* tk : toks) {
        if (tk->getType() == antlr4::Token::EOF) break;
        const auto channel = tk->getChannel();
        const auto type = tk->getType();
        const auto& rawText = tk->getText();

        if (channel == antlr4::Token::DEFAULT_CHANNEL) {
            std::size_t idx = tk->getTokenIndex();
            // LineEnd 自身也走 default 通道（每行一个），不能用它更新行号锚，
            // 否则会把空行的 LineEnd 当作"上一行"把空行吃掉
            if (type == yuxLexer::LineEnd) {
                continue;
            }
            // 行号差 >= 2 视为存在空行；若中间有 leading 注释 (pendingLeading
            // 非空)，应从最后一条 leading 注释的行号起算，否则一行注释会被
            // 当成"空行"误触发 blankBefore
            std::size_t prevLine = pendingLeading.empty()
                ? lastDefaultLine
                : pendingLeading.back().line;
            if (lastDefault != static_cast<std::size_t>(-1)
                && tk->getLine() > prevLine + 1) {
                map.blankBefore[idx] = true;
            }
            if (!pendingLeading.empty()) {
                // 检测最后一条 leading 注释与该 token 之间是否有空行
                if (tk->getLine() > pendingLeading.back().line + 1) {
                    map.blankAfterLeading[idx] = true;
                }
                map.leadingByTokenIndex[idx] = std::move(pendingLeading);
                pendingLeading.clear();
            }
            lastDefault = idx;
            lastDefaultLine = tk->getLine();
            continue;
        }

        // HIDDEN 通道：注释 / 空白
        if (type == yuxLexer::LineEndComment) {
            // 行尾注释：挂到上一个 default token 的 trailing
            if (lastDefault != static_cast<std::size_t>(-1) && tk->getLine() == lastDefaultLine) {
                map.trailingByTokenIndex[lastDefault].push_back(
                    {trimCommentText(rawText), true, tk->getLine(), false});
            } else {
                std::size_t prevLine = pendingLeading.empty()
                    ? lastDefaultLine
                    : pendingLeading.back().line;
                bool blank = (lastDefault != static_cast<std::size_t>(-1)
                              || !pendingLeading.empty())
                             && tk->getLine() > prevLine + 1;
                pendingLeading.push_back({trimCommentText(rawText), true, tk->getLine(), blank});
            }
        } else if (type == yuxLexer::LineComment) {
            std::size_t prevLine = pendingLeading.empty()
                ? lastDefaultLine
                : pendingLeading.back().line;
            bool blank = (lastDefault != static_cast<std::size_t>(-1)
                          || !pendingLeading.empty())
                         && tk->getLine() > prevLine + 1;
            pendingLeading.push_back({trimCommentText(rawText), true, tk->getLine(), blank});
        }
        // 其它 hidden token (Space 等) 当前不需要记录
    }

    return map;
}

} // namespace yux::format

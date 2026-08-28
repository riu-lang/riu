// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化 trivia (注释 / 空行) 预扫描
//
// ANTLR 把行注释 / 块注释 / 空白放进 HIDDEN 通道，AST 只看 default 通道。
// 我们用一次线性扫描把 trivia 按"出现在第几行 / 紧跟在哪个 default token 之后"
// 索引起来；printer 在拼 Doc 时按需查询，把注释和空行映射回输出。
//
// 当前规模较小，仅暴露最小接口；Phase 2+ 会逐步增加查询方法。

#ifndef YUX_LANG_FORMAT_TRIVIA_H
#define YUX_LANG_FORMAT_TRIVIA_H

#include <string>
#include <unordered_map>
#include <vector>

namespace antlr4 {
class CommonTokenStream;
}

namespace yux::format {

struct TriviaComment {
    std::string text;   // 原始文本，含 `;` 引导
    bool isLineComment; // 现在仅有行注释；保留字段供后续扩展
    std::size_t line;   // 1-based 源码行号
    bool blankBefore;   // 该注释前在源码里是否存在空行（与上一注释或上一 default token 对比）
};

class TriviaMap {
public:
    // 用 default 通道的 tokenIndex 作为锚；行尾注释挂在"该行最后一个 default token"
    // 后面，独立行注释挂在"下一个 default token 之前"。
    std::unordered_map<std::size_t, std::vector<TriviaComment>> trailingByTokenIndex;
    std::unordered_map<std::size_t, std::vector<TriviaComment>> leadingByTokenIndex;

    // 在 default token tokenIndex 之前，源码上是否存在 >=1 个空行（指 item
    // 与上一 item / 文件起点之间，不含 leading 注释带来的间隔）
    std::unordered_map<std::size_t, bool> blankBefore;

    // 在 default token tokenIndex 自身与"它前面最后一条 leading 注释"之间
    // 是否存在空行（用于保留 `; ...\n\nfn ...` 这种"标题注释 + 空行 + 项"形态）
    std::unordered_map<std::size_t, bool> blankAfterLeading;
};

// 扫描 token stream 的 HIDDEN 通道，构建 TriviaMap
TriviaMap buildTrivia(antlr4::CommonTokenStream& stream);

} // namespace yux::format

#endif // YUX_LANG_FORMAT_TRIVIA_H

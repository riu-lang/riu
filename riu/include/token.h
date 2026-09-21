// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "primitives.h"

#include <vector>

// Token 文本 intern：空串进程单例；其余挂当前 StringIntern（Riu 构造 push）。
[[nodiscard]] inline const string& emptyTokenText() {
    static const string kEmpty;
    return kEmpty;
}
[[nodiscard]] const string& internTokenText(string_view s);

// 恰好 `_`：丢弃槽（[#20]）。`_foo` 仍是普通私有名。
[[nodiscard]] inline bool isDiscardName(string_view name) noexcept {
    return name.size() == 1 && name.front() == '_';
}

[[nodiscard]] inline bool lastPathSegIsDiscard(string_view dotted) noexcept {
    auto pos = dotted.rfind('.');
    auto last = pos == string_view::npos ? dotted : dotted.substr(pos + 1);
    return isDiscardName(last);
}

class TokenInfo {
    const string* _text = nullptr;
    size_t _line = 0;
    size_t _charPositionInLine = 0;
    size_t _tokenIndex = 0;
    size_t _startIndex = 0;
    size_t _stopIndex = 0;

public:
    TokenInfo() = default;

    // 合成 Token：解糖时没有源 token 的节点（如 T? → Nullable<T> 的 "Nullable"）
    TokenInfo(string_view text, size_t line) : _text(&internTokenText(text)), _line(line) {}

    // 从 rd::Pos 填行列与字节区间。charPositionInLine 是 0-based 行内 UTF-8 字节
    // （rd::Pos.column）；start/stop 为 UTF-8 字节，stop 是闭区间（半开 end-1）。
    TokenInfo(string_view text, size_t line, size_t charPositionInLine, size_t tokenIndex, size_t startIndex,
              size_t stopIndex)
        : _text(&internTokenText(text)), _line(line), _charPositionInLine(charPositionInLine), _tokenIndex(tokenIndex),
          _startIndex(startIndex), _stopIndex(stopIndex) {}

    TokenInfo(const TokenInfo& other) = default;
    TokenInfo(TokenInfo&& other) noexcept = default;
    TokenInfo& operator=(const TokenInfo& other) = default;
    TokenInfo& operator=(TokenInfo&& other) noexcept = default;

    [[nodiscard]] const string& getText() const { return _text ? *_text : emptyTokenText(); }
    [[nodiscard]] size_t getLine() const { return _line; }
    [[nodiscard]] size_t getCharPositionInLine() const { return _charPositionInLine; } // 0-based 行内 UTF-8 字节
    [[nodiscard]] size_t getTokenIndex() const { return _tokenIndex; }
    [[nodiscard]] size_t getStartIndex() const { return _startIndex; }
    [[nodiscard]] size_t getStopIndex() const { return _stopIndex; }

    [[nodiscard]] bool empty() const { return !_text || _text->empty(); }
    [[nodiscard]] bool valid() const { return !empty() || _line > 0; }

    explicit operator bool() const { return valid(); }

    bool operator==(const TokenInfo& other) const { return getText() == other.getText() && _line == other._line; }
    bool operator!=(const TokenInfo& other) const { return !(*this == other); }
};

using Token = TokenInfo;

// 源码限定类型路径 `a.b.T`（riu.bnf typePath）。身份是 TypeInfo.ownerModule + 短名；
// TypeInfo.name 只用末段短名，路径不进 name。
// 一段名（`i32`）不进 heap：混测几乎全是裸名，vector<Token> 在 debug CRT 上按节点一份头。
struct TypePath {
    Token first;
    vector<Token> rest; // segs[1..]

    TypePath() = default;
    explicit TypePath(Token bare) : first(bare) {}
    explicit TypePath(vector<Token> s) {
        if (s.empty()) return;
        first = s[0];
        if (s.size() > 1) {
            rest.assign(s.begin() + 1, s.end());
        }
    }

    [[nodiscard]] bool empty() const { return first.empty() && rest.empty(); }
    [[nodiscard]] size_t size() const { return empty() ? 0 : 1 + rest.size(); }
    [[nodiscard]] bool isBare() const { return rest.empty() && !first.empty(); }
    [[nodiscard]] const Token& last() const { return rest.empty() ? first : rest.back(); }
    [[nodiscard]] const Token& operator[](size_t i) const { return i == 0 ? first : rest[i - 1]; }
    [[nodiscard]] const string& lastName() const {
        static const string kEmpty;
        return empty() ? kEmpty : last().getText();
    }
    [[nodiscard]] string dotted() const {
        if (empty()) return {};
        string s = first.getText();
        for (const auto& t : rest) {
            s += '.';
            s += t.getText();
        }
        return s;
    }
    void push_back(Token t) {
        if (empty())
            first = t;
        else
            rest.push_back(t);
    }
    void pop_back() {
        if (!rest.empty())
            rest.pop_back();
        else
            first = {};
    }
};

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化器模块
// 
// 本文件实现 yux 代码格式化功能:
// - 基于 Token 流重排实现格式化
// - 支持关键字后空格、运算符两边空格、逗号后空格等规则
// - 支持 2 空格缩进
// - 支持大括号独占行

#ifndef YUX_LANG_FORMATTER_H
#define YUX_LANG_FORMATTER_H

#include <string>
#include <vector>
#include <sstream>
#include "yux/yuxLexer.h"

namespace yux {

// 格式化配置
struct FormatConfig {
    size_t indentSize = 2;          // 缩进空格数
    bool insertSpaceAfterKeyword = true;  // 关键字后插入空格
    bool insertSpaceAroundOperator = true; // 运算符两边插入空格
    bool insertSpaceAfterComma = true;    // 逗号后插入空格
    bool braceOnNewLine = true;     // 大括号独占行
};

// 格式化器类
class Formatter {
public:
    explicit Formatter(const std::string& source, const FormatConfig& config = FormatConfig{});
    
    // 执行格式化，返回格式化后的代码
    std::string format();
    
private:
    std::string source_;
    FormatConfig config_;
    std::vector<antlr4::Token*> tokens_;
    size_t indentLevel_ = 0;
    bool atLineStart_ = true;
    
    // 收集所有 Token（包括 HIDDEN 通道的）
    void collectTokens(antlr4::CommonTokenStream& tokenStream);
    
    // 格式化单个 Token
    std::string formatToken(size_t index);
    
    // 判断是否需要在 Token 前插入空格
    bool needSpaceBefore(size_t tokenType, size_t prevTokenType, 
                         const std::string& prevText, size_t nextTokenType);
    
    // 判断是否需要在 Token 后插入空格
    bool needSpaceAfter(size_t tokenType, size_t nextTokenType, const std::string& nextText);
    
    // 发射缩进
    void emitIndent(std::stringstream& out);
    
    // 发射换行
    void emitNewline(std::stringstream& out);
    
    // 判断是否是关键字
    bool isKeyword(size_t tokenType) const;
    
    // 判断是否是运算符
    bool isOperator(size_t tokenType) const;
    
    // 判断是否是二元运算符
    bool isBinaryOperator(size_t tokenType) const;
    
    // 判断是否是类型关键字
    bool isTypeKeyword(size_t tokenType) const;
    
    // 判断 Token 是否会增加缩进（BlockStart）
    bool isIndentIncrease(size_t tokenType) const;
    
    // 判断 Token 是否会减少缩进（BlockEnd）
    bool isIndentDecrease(size_t tokenType) const;
};

} // namespace yux

#endif //YUX_LANG_FORMATTER_H

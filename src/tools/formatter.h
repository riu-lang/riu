// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化器模块
// 
// 本文件实现 yux 代码格式化功能:
// - 基于 Token 流重排实现格式化
// - 支持列宽配置（默认 120）
// - 支持函数参数多行格式化
// - 支持链式 `.` 智能换行
// - 保留用户手动换行
// - 支持 2 空格缩进

#ifndef YUX_LANG_FORMATTER_H
#define YUX_LANG_FORMATTER_H

#include <string>
#include <vector>
#include <sstream>
#include <set>
#include "yux/yuxLexer.h"

namespace yux {

// 格式化配置
struct FormatConfig {
    size_t indentSize = 2;          // 缩进空格数（固定为 2）
    size_t lineWidth = 120;         // 列宽阈值
    bool insertSpaceAfterKeyword = true;  // 关键字后插入空格
    bool insertSpaceAroundOperator = true; // 运算符两边插入空格
    bool insertSpaceAfterComma = true;    // 逗号后插入空格
};

// Token 上下文信息
struct TokenContext {
    size_t index;           // 在 token 列表中的索引
    size_t tokenType;       // token 类型
    std::string text;       // token 文本
    size_t line;            // 原始行号
    size_t column;          // 原始列号
    bool hasLineEndBefore;  // 前面是否有换行（用户手动换行）
    int lineEndCountBefore; // 前面连续换行数
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
    std::vector<TokenContext> contexts_;
    size_t indentLevel_ = 0;
    size_t currentColumn_ = 0;
    bool atLineStart_ = true;
    
    // 用户手动换行的位置（token 索引集合）
    std::set<size_t> manualLineBreaks_;
    
    // 收集所有 Token（包括 HIDDEN 通道的）
    void collectTokens(antlr4::CommonTokenStream& tokenStream);
    
    // 构建上下文信息
    void buildContexts();
    
    // 检测用户手动换行
    void detectManualLineBreaks();
    
    // 格式化入口
    std::string formatTokens();
    
    // 格式化单个 Token
    void formatToken(size_t index, std::stringstream& out);
    
    // 判断是否需要在 Token 前插入空格
    bool needSpaceBefore(size_t tokenType, size_t prevTokenType, 
                         const std::string& prevText, size_t nextTokenType);
    
    // 发射缩进
    void emitIndent(std::stringstream& out);
    
    // 发射换行
    void emitNewline(std::stringstream& out);
    
    // 判断是否是关键字
    bool isKeyword(size_t tokenType) const;
    
    // 判断是否是二元运算符
    bool isBinaryOperator(size_t tokenType) const;
    
    // 判断 Token 是否会增加缩进（BlockStart）
    bool isIndentIncrease(size_t tokenType) const;
    
    // 判断 Token 是否会减少缩进（BlockEnd）
    bool isIndentDecrease(size_t tokenType) const;
    
    // ==================== 列宽相关 ====================
    
    // 计算从指定位置到行尾的宽度
    size_t measureLineWidth(size_t startIndex);
    
    // 判断函数参数是否需要多行格式化
    bool shouldFormatFnParamsMultiLine(size_t parStartIndex);
    
    // 判断链式 `.` 是否需要换行
    bool shouldBreakDotChain(size_t dotIndex);
    
    // 检查从指定位置开始的链式 `.` 是否有用户手动换行
    bool hasManualBreakInDotChain(size_t startIndex);
    
    // ==================== 特殊结构格式化 ====================
    
    // 格式化函数参数（可能多行）
    void formatFnParams(size_t parStartIndex, std::stringstream& out);
    
    // 格式化链式 `.` 访问
    void formatDotChain(size_t dotIndex, std::stringstream& out);
    
    // 格式化数组/下标访问
    void formatArrayAccess(size_t getStartIndex, std::stringstream& out);
    
    // 格式化数组字面量
    void formatArrayLiteral(size_t getStartIndex, std::stringstream& out);
    
    // 格式化函数调用
    void formatCallExpr(size_t parStartIndex, std::stringstream& out);
    
    // ==================== 辅助函数 ====================
    
    // 查找下一个非 HIDDEN token
    size_t findNextVisibleToken(size_t startIndex);
    
    // 查找前一个非 HIDDEN token
    size_t findPrevVisibleToken(size_t startIndex);
    
    // 查找匹配的右括号/右中括号/右大括号
    size_t findMatchingEnd(size_t startIndex, size_t startType, size_t endType);
    
    // 获取 token 类型
    size_t getTokenType(size_t index);
    
    // 获取 token 文本
    std::string getTokenText(size_t index);
    
    // 检查是否在用户手动换行位置
    bool isManualLineBreak(size_t index);
    
    // 计算当前行的缩进字符串
    std::string getIndentStr();
};

} // namespace yux

#endif //YUX_LANG_FORMATTER_H

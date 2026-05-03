// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化器实现
// 
// 本文件实现 yux 代码格式化功能:
// - 基于 Token 流重排实现格式化
// - 支持列宽配置（默认 120）
// - 支持函数参数多行格式化
// - 支持链式 `.` 智能换行
// - 保留用户手动换行

#include "formatter.h"
#include <antlr4-runtime.h>
#include <algorithm>

namespace yux {

using namespace antlr4;

Formatter::Formatter(const std::string& source, const FormatConfig& config)
    : source_(source), config_(config) {}

std::string Formatter::format() {
    ANTLRInputStream input(source_);
    yuxLexer lexer(&input);
    
    CommonTokenStream tokenStream(&lexer);
    tokenStream.fill();
    
    collectTokens(tokenStream);
    buildContexts();
    detectManualLineBreaks();
    
    return formatTokens();
}

void Formatter::collectTokens(CommonTokenStream& tokenStream) {
    tokens_.clear();
    for (auto* token : tokenStream.getTokens()) {
        tokens_.push_back(token);
    }
}

void Formatter::buildContexts() {
    contexts_.clear();
    contexts_.reserve(tokens_.size());
    
    for (size_t i = 0; i < tokens_.size(); i++) {
        TokenContext ctx;
        ctx.index = i;
        ctx.tokenType = tokens_[i]->getType();
        ctx.text = tokens_[i]->getText();
        ctx.line = tokens_[i]->getLine();
        ctx.column = tokens_[i]->getCharPositionInLine();
        ctx.hasLineEndBefore = false;
        ctx.lineEndCountBefore = 0;
        
        // 检查前面是否有换行 token
        if (i > 0) {
            int lineEndCount = 0;
            for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                size_t t = tokens_[j]->getType();
                if (t == yuxLexer::LineEnd) {
                    lineEndCount++;
                } else if (tokens_[j]->getChannel() == Token::DEFAULT_CHANNEL) {
                    break;
                }
            }
            if (lineEndCount > 0) {
                ctx.hasLineEndBefore = true;
                ctx.lineEndCountBefore = lineEndCount;
            }
        }
        
        contexts_.push_back(ctx);
    }
}

void Formatter::detectManualLineBreaks() {
    manualLineBreaks_.clear();
    
    // 检测链式 `.` 中的手动换行
    for (size_t i = 0; i < tokens_.size(); i++) {
        size_t tokenType = tokens_[i]->getType();
        
        // 检查 `.` 或 `?.` 前是否有手动换行
        if (tokenType == yuxLexer::SymbolDot || tokenType == yuxLexer::SymbolQuest) {
            if (i > 0 && contexts_[i].hasLineEndBefore) {
                // 标记这个位置为手动换行
                manualLineBreaks_.insert(i);
            }
        }
        
        // 检查函数参数中的手动换行
        if (tokenType == yuxLexer::ParStart) {
            size_t parEnd = findMatchingEnd(i, yuxLexer::ParStart, yuxLexer::ParEnd);
            if (parEnd != i) {
                for (size_t j = i + 1; j < parEnd; j++) {
                    if (contexts_[j].hasLineEndBefore) {
                        // 函数参数中有手动换行，标记整个参数列表为多行
                        manualLineBreaks_.insert(i);
                        break;
                    }
                }
            }
        }
    }
}

std::string Formatter::formatTokens() {
    std::stringstream out;
    indentLevel_ = 0;
    currentColumn_ = 0;
    atLineStart_ = true;
    
    for (size_t i = 0; i < tokens_.size(); i++) {
        size_t tokenType = tokens_[i]->getType();
        
        // 跳过 HIDDEN 通道的 Token（空格、空行等）
        if (tokens_[i]->getChannel() != Token::DEFAULT_CHANNEL) {
            continue;
        }
        
        // 跳过 EOF token
        if (tokenType == Token::EOF) {
            continue;
        }
        
        // 处理行注释
        if (tokenType == yuxLexer::LineComment) {
            if (!atLineStart_) {
                out << " ";
                currentColumn_++;
            }
            out << tokens_[i]->getText();
            currentColumn_ += tokens_[i]->getText().length();
            continue;
        }
        
        // 处理行尾注释
        if (tokenType == yuxLexer::LineEndComment) {
            out << " " << tokens_[i]->getText();
            currentColumn_ += 1 + tokens_[i]->getText().length();
            continue;
        }
        
        // 处理换行
        if (tokenType == yuxLexer::LineEnd) {
            emitNewline(out);
            continue;
        }
        
        // 处理缩进减少（BlockEnd）
        if (isIndentDecrease(tokenType)) {
            if (indentLevel_ > 0) {
                indentLevel_--;
            }
        }
        
        // 特殊结构处理
        // 函数声明参数：fn name( 后的参数列表
        if (tokenType == yuxLexer::ParStart) {
            // 检查是否是函数声明的参数列表
            size_t prev = findPrevVisibleToken(i);
            if (prev < tokens_.size()) {
                size_t prevType = tokens_[prev]->getType();
                // fn name( 或 extern 块中的函数头
                if (prevType == yuxLexer::ID) {
                    size_t prevPrev = findPrevVisibleToken(prev);
                    if (prevPrev < tokens_.size() && tokens_[prevPrev]->getType() == yuxLexer::Fn) {
                        // 这是函数声明的参数列表
                        size_t parEndIndex = findMatchingEnd(i, yuxLexer::ParStart, yuxLexer::ParEnd);
                        formatFnParams(i, out);
                        // 跳过参数列表中的所有 token
                        i = parEndIndex;
                        continue;
                    }
                }
            }
        }
        
        // 格式化当前 Token
        formatToken(i, out);
        
        // 处理缩进增加（BlockStart）
        if (isIndentIncrease(tokenType)) {
            indentLevel_++;
        }
    }
    
    std::string result = out.str();
    
    // 确保结果以换行结尾
    if (!result.empty() && result.back() != '\n') {
        result += '\n';
    }
    
    return result;
}

void Formatter::formatToken(size_t index, std::stringstream& out) {
    antlr4::Token* token = tokens_[index];
    size_t tokenType = token->getType();
    std::string text = token->getText();
    
    // 在行首时先发射缩进
    if (atLineStart_) {
        emitIndent(out);
    }
    
    // 查找前一个非 HIDDEN Token
    size_t prevTokenType = Token::INVALID_TYPE;
    std::string prevText;
    size_t prevIndex = findPrevVisibleToken(index);
    if (prevIndex < tokens_.size()) {
        prevTokenType = tokens_[prevIndex]->getType();
        prevText = tokens_[prevIndex]->getText();
    }
    
    // 查找后一个非 HIDDEN Token
    size_t nextTokenType = Token::INVALID_TYPE;
    size_t nextIndex = findNextVisibleToken(index);
    if (nextIndex < tokens_.size()) {
        nextTokenType = tokens_[nextIndex]->getType();
    }
    
    // 判断是否需要在前方插入空格
    if (!atLineStart_ && needSpaceBefore(tokenType, prevTokenType, prevText, nextTokenType)) {
        out << " ";
        currentColumn_++;
    }
    
    out << text;
    currentColumn_ += text.length();
    atLineStart_ = false;
}

bool Formatter::needSpaceBefore(size_t tokenType, size_t prevTokenType, 
                                 const std::string& prevText, size_t nextTokenType) {
    // 左括号、左中括号前不需要空格
    if (tokenType == yuxLexer::ParStart || tokenType == yuxLexer::GetStart) {
        return false;
    }
    
    // 点号前不需要空格
    if (tokenType == yuxLexer::SymbolDot) {
        return false;
    }
    
    // 逗号、分号前不需要空格
    if (tokenType == yuxLexer::SymbolComma || tokenType == yuxLexer::SymbolSemicolon) {
        return false;
    }
    
    // 右括号、右中括号前不需要空格
    if (tokenType == yuxLexer::ParEnd || tokenType == yuxLexer::GetEnd) {
        return false;
    }
    
    // 大括号前需要空格（除非在行首）
    if (tokenType == yuxLexer::BlockStart) {
        if (prevTokenType == yuxLexer::ID || prevTokenType == yuxLexer::ParEnd) {
            return true;
        }
        return false;
    }
    
    // BlockEnd 前不需要空格（由缩进处理）
    if (tokenType == yuxLexer::BlockEnd) {
        return false;
    }
    
    // 如果当前 token 是运算符，前面需要空格
    if (isBinaryOperator(tokenType)) {
        // 特殊处理：* 作为通配符时（前面是 .）不需要空格
        if (tokenType == yuxLexer::SymbolMul && prevTokenType == yuxLexer::SymbolDot) {
            return false;
        }
        return config_.insertSpaceAroundOperator;
    }
    
    // 如果当前 token 是 =，前面需要空格
    if (tokenType == yuxLexer::SymbolEq) {
        return config_.insertSpaceAroundOperator;
    }
    
    // 如果前面是左括号、左中括号，不需要空格
    if (prevTokenType == yuxLexer::ParStart || prevTokenType == yuxLexer::GetStart) {
        return false;
    }
    
    // 如果前面是点号，不需要空格
    if (prevTokenType == yuxLexer::SymbolDot) {
        return false;
    }
    
    // 如果前面是逗号，需要空格
    if (prevTokenType == yuxLexer::SymbolComma) {
        return config_.insertSpaceAfterComma;
    }
    
    // 如果前面是关键字，需要空格
    if (isKeyword(prevTokenType)) {
        return config_.insertSpaceAfterKeyword;
    }
    
    // 如果前面是 ID，需要判断是否需要空格
    if (prevTokenType == yuxLexer::ID) {
        // ID 后面跟着 ID 需要空格（变量声明：v i32）
        if (tokenType == yuxLexer::ID) {
            return true;
        }
        // ID 后面跟着泛型 < 不需要空格
        if (tokenType == yuxLexer::SymbolLt) {
            return false;
        }
        // ID 后面跟着 BlockStart 需要空格（struct N {）
        if (tokenType == yuxLexer::BlockStart) {
            return true;
        }
    }
    
    // 如果前面是右括号，后面跟着 ID 需要空格（返回类型：fn name() i32）
    if (prevTokenType == yuxLexer::ParEnd) {
        if (tokenType == yuxLexer::ID) {
            return true;
        }
    }
    
    // 如果前面是运算符，需要空格
    if (isBinaryOperator(prevTokenType)) {
        return config_.insertSpaceAroundOperator;
    }
    
    // 如果前面是 =，需要空格（赋值）
    if (prevTokenType == yuxLexer::SymbolEq) {
        return config_.insertSpaceAroundOperator;
    }
    
    return false;
}

void Formatter::emitIndent(std::stringstream& out) {
    std::string indent = getIndentStr();
    out << indent;
    currentColumn_ = indent.length();
}

void Formatter::emitNewline(std::stringstream& out) {
    out << "\n";
    currentColumn_ = 0;
    atLineStart_ = true;
}

bool Formatter::isKeyword(size_t tokenType) const {
    switch (tokenType) {
        case yuxLexer::Break:
        case yuxLexer::DeclKey:
        case yuxLexer::Elif:
        case yuxLexer::Else:
        case yuxLexer::Extern:
        case yuxLexer::False:
        case yuxLexer::Fn:
        case yuxLexer::If:
        case yuxLexer::Loop:
        case yuxLexer::Null:
        case yuxLexer::Ret:
        case yuxLexer::Struct:
        case yuxLexer::True:
        case yuxLexer::Use:
            return true;
        default:
            return false;
    }
}

bool Formatter::isBinaryOperator(size_t tokenType) const {
    switch (tokenType) {
        case yuxLexer::SymbolAdd:
        case yuxLexer::SymbolSub:
        case yuxLexer::SymbolMul:
        case yuxLexer::SymbolDiv:
        case yuxLexer::SymbolMod:
        case yuxLexer::SymbolAnd:
        case yuxLexer::SymbolOr:
        case yuxLexer::SymbolXor:
        case yuxLexer::SymbolLt:
        case yuxLexer::SymbolMt:
        case yuxLexer::SymbolExcl:
            return true;
        default:
            return false;
    }
}

bool Formatter::isIndentIncrease(size_t tokenType) const {
    return tokenType == yuxLexer::BlockStart;
}

bool Formatter::isIndentDecrease(size_t tokenType) const {
    return tokenType == yuxLexer::BlockEnd;
}

// ==================== 列宽相关 ====================

size_t Formatter::measureLineWidth(size_t startIndex) {
    // 计算从 startIndex 到下一个换行符或 EOF 的宽度
    size_t width = currentColumn_;
    bool firstToken = true;
    
    for (size_t i = startIndex; i < tokens_.size(); i++) {
        size_t tokenType = tokens_[i]->getType();
        
        // 遇到换行符停止
        if (tokenType == yuxLexer::LineEnd) {
            break;
        }
        
        // 跳过 HIDDEN 通道
        if (tokens_[i]->getChannel() != Token::DEFAULT_CHANNEL) {
            continue;
        }
        
        // 跳过 EOF
        if (tokenType == Token::EOF) {
            break;
        }
        
        std::string text = tokens_[i]->getText();
        
        // 第一个 token 不加前导空格
        if (!firstToken) {
            // 简单估算：大多数情况需要空格
            width++;
        }
        firstToken = false;
        
        width += text.length();
    }
    
    return width;
}

bool Formatter::shouldFormatFnParamsMultiLine(size_t parStartIndex) {
    // 如果用户手动换行，保留多行格式
    if (isManualLineBreak(parStartIndex)) {
        return true;
    }
    
    size_t parEndIndex = findMatchingEnd(parStartIndex, yuxLexer::ParStart, yuxLexer::ParEnd);
    if (parEndIndex == parStartIndex) {
        return false;
    }
    
    // 计算参数数量
    int paramCount = 0;
    for (size_t i = parStartIndex + 1; i < parEndIndex; i++) {
        if (tokens_[i]->getChannel() != Token::DEFAULT_CHANNEL) continue;
        if (tokens_[i]->getType() == yuxLexer::SymbolComma) {
            paramCount++;
        }
    }
    // 最后一个参数后面没有逗号
    bool hasContent = false;
    for (size_t i = parStartIndex + 1; i < parEndIndex; i++) {
        if (tokens_[i]->getChannel() == Token::DEFAULT_CHANNEL && 
            tokens_[i]->getType() != yuxLexer::LineEnd) {
            hasContent = true;
            break;
        }
    }
    if (hasContent) paramCount++;
    
    // 1 个参数：强制单行
    if (paramCount <= 1) {
        return false;
    }
    
    // >1 个参数：检查列宽
    size_t lineWidth = measureLineWidth(parStartIndex);
    return lineWidth > config_.lineWidth;
}

bool Formatter::shouldBreakDotChain(size_t dotIndex) {
    // 如果用户手动换行，保留
    if (isManualLineBreak(dotIndex)) {
        return true;
    }
    
    // 检查整个链式调用的宽度
    // 向前找到链的起点
    size_t chainStart = dotIndex;
    while (chainStart > 0) {
        size_t prev = findPrevVisibleToken(chainStart);
        if (prev >= tokens_.size()) break;
        
        size_t prevType = tokens_[prev]->getType();
        if (prevType == yuxLexer::ID || prevType == yuxLexer::ParEnd || prevType == yuxLexer::GetEnd) {
            // 检查 prev 前面是否是 . 或 ?.
            size_t prevPrev = findPrevVisibleToken(prev);
            if (prevPrev < tokens_.size()) {
                size_t ppType = tokens_[prevPrev]->getType();
                if (ppType == yuxLexer::SymbolDot || ppType == yuxLexer::SymbolQuest) {
                    chainStart = prevPrev;
                    continue;
                }
            }
        }
        break;
    }
    
    // 向后找到链的终点
    size_t chainEnd = dotIndex;
    while (chainEnd < tokens_.size() - 1) {
        size_t next = findNextVisibleToken(chainEnd);
        if (next >= tokens_.size()) break;
        
        size_t nextType = tokens_[next]->getType();
        if (nextType == yuxLexer::SymbolDot || nextType == yuxLexer::SymbolQuest) {
            size_t nextNext = findNextVisibleToken(next);
            if (nextNext < tokens_.size() && tokens_[nextNext]->getType() == yuxLexer::ID) {
                chainEnd = nextNext;
                continue;
            }
        }
        break;
    }
    
    // 计算链的宽度
    size_t width = currentColumn_;
    bool first = true;
    for (size_t i = chainStart; i <= chainEnd && i < tokens_.size(); i++) {
        if (tokens_[i]->getChannel() != Token::DEFAULT_CHANNEL) continue;
        size_t t = tokens_[i]->getType();
        if (t == Token::EOF || t == yuxLexer::LineEnd) break;
        
        if (!first) width++;
        first = false;
        width += tokens_[i]->getText().length();
    }
    
    return width > config_.lineWidth;
}

bool Formatter::hasManualBreakInDotChain(size_t startIndex) {
    // 向前找到链的起点
    size_t chainStart = startIndex;
    while (chainStart > 0) {
        size_t prev = findPrevVisibleToken(chainStart);
        if (prev >= tokens_.size()) break;
        
        size_t prevType = tokens_[prev]->getType();
        if (prevType == yuxLexer::ID || prevType == yuxLexer::ParEnd || prevType == yuxLexer::GetEnd) {
            size_t prevPrev = findPrevVisibleToken(prev);
            if (prevPrev < tokens_.size()) {
                size_t ppType = tokens_[prevPrev]->getType();
                if (ppType == yuxLexer::SymbolDot || ppType == yuxLexer::SymbolQuest) {
                    if (isManualLineBreak(prevPrev)) return true;
                    chainStart = prevPrev;
                    continue;
                }
            }
        }
        break;
    }
    
    // 向后检查
    size_t chainEnd = startIndex;
    while (chainEnd < tokens_.size() - 1) {
        size_t next = findNextVisibleToken(chainEnd);
        if (next >= tokens_.size()) break;
        
        size_t nextType = tokens_[next]->getType();
        if (nextType == yuxLexer::SymbolDot || nextType == yuxLexer::SymbolQuest) {
            if (isManualLineBreak(next)) return true;
            size_t nextNext = findNextVisibleToken(next);
            if (nextNext < tokens_.size() && tokens_[nextNext]->getType() == yuxLexer::ID) {
                chainEnd = nextNext;
                continue;
            }
        }
        break;
    }
    
    return false;
}

// ==================== 特殊结构格式化 ====================

void Formatter::formatFnParams(size_t parStartIndex, std::stringstream& out) {
    size_t parEndIndex = findMatchingEnd(parStartIndex, yuxLexer::ParStart, yuxLexer::ParEnd);
    if (parEndIndex == parStartIndex) {
        formatToken(parStartIndex, out);
        return;
    }
    
    bool multiLine = shouldFormatFnParamsMultiLine(parStartIndex);
    
    // 输出 (
    if (atLineStart_) {
        emitIndent(out);
    }
    out << "(";
    currentColumn_++;
    atLineStart_ = false;
    
    // 收集参数列表中的所有可见 token
    std::vector<size_t> visibleTokens;
    for (size_t i = parStartIndex + 1; i < parEndIndex; i++) {
        if (tokens_[i]->getChannel() == Token::DEFAULT_CHANNEL &&
            tokens_[i]->getType() != yuxLexer::LineEnd) {
            visibleTokens.push_back(i);
        }
    }
    
    if (visibleTokens.empty()) {
        // 空参数列表
        out << ")";
        currentColumn_++;
        return;
    }
    
    if (multiLine) {
        // 多行格式：每参一行
        indentLevel_++;
        
        // 找出参数边界（逗号分隔）
        std::vector<std::pair<size_t, size_t>> paramRanges;
        size_t paramStart = 0;
        for (size_t i = 0; i < visibleTokens.size(); i++) {
            if (tokens_[visibleTokens[i]]->getType() == yuxLexer::SymbolComma) {
                paramRanges.push_back({paramStart, i});
                paramStart = i + 1;
            }
        }
        // 最后一个参数
        if (paramStart < visibleTokens.size()) {
            paramRanges.push_back({paramStart, visibleTokens.size()});
        }
        
        // 输出每个参数
        for (size_t p = 0; p < paramRanges.size(); p++) {
            auto& range = paramRanges[p];
            
            emitNewline(out);
            emitIndent(out);
            
            // 输出参数 tokens
            for (size_t i = range.first; i < range.second; i++) {
                size_t idx = visibleTokens[i];
                size_t t = tokens_[idx]->getType();
                std::string text = tokens_[idx]->getText();
                
                // 参数名后加空格（ID 后跟 ID 是类型）
                if (i > range.first && t == yuxLexer::ID) {
                    size_t prevIdx = visibleTokens[i - 1];
                    size_t prevType = tokens_[prevIdx]->getType();
                    if (prevType == yuxLexer::ID) {
                        out << " ";
                        currentColumn_++;
                    }
                }
                
                out << text;
                currentColumn_ += text.length();
                atLineStart_ = false;
            }
            
            // 添加尾随逗号
            out << ",";
            currentColumn_++;
        }
        
        indentLevel_--;
        
        // ) 单独一行
        emitNewline(out);
        emitIndent(out);
        out << ")";
        currentColumn_++;
        atLineStart_ = false;
    } else {
        // 单行格式
        for (size_t i = 0; i < visibleTokens.size(); i++) {
            size_t idx = visibleTokens[i];
            size_t t = tokens_[idx]->getType();
            std::string text = tokens_[idx]->getText();
            
            // 逗号后加空格
            if (t == yuxLexer::SymbolComma) {
                out << ", ";
                currentColumn_ += 2;
            } else {
                // 参数名后加空格（ID 后跟 ID 是类型）
                if (i > 0) {
                    size_t prevIdx = visibleTokens[i - 1];
                    size_t prevType = tokens_[prevIdx]->getType();
                    if (prevType == yuxLexer::ID && t == yuxLexer::ID) {
                        out << " ";
                        currentColumn_++;
                    }
                }
                out << text;
                currentColumn_ += text.length();
            }
        }
        
        out << ")";
        currentColumn_++;
    }
}

void Formatter::formatDotChain(size_t dotIndex, std::stringstream& out) {
    // 简化实现：按普通 token 处理
    // 完整实现需要检测链式调用并决定是否换行
    formatToken(dotIndex, out);
}

void Formatter::formatArrayAccess(size_t getStartIndex, std::stringstream& out) {
    // 简化实现：按普通 token 处理
    formatToken(getStartIndex, out);
}

void Formatter::formatArrayLiteral(size_t getStartIndex, std::stringstream& out) {
    // 简化实现：按普通 token 处理
    formatToken(getStartIndex, out);
}

void Formatter::formatCallExpr(size_t parStartIndex, std::stringstream& out) {
    // 简化实现：按普通 token 处理
    formatToken(parStartIndex, out);
}

// ==================== 辅助函数 ====================

size_t Formatter::findNextVisibleToken(size_t startIndex) {
    for (size_t i = startIndex + 1; i < tokens_.size(); i++) {
        if (tokens_[i]->getChannel() == Token::DEFAULT_CHANNEL) {
            return i;
        }
    }
    return tokens_.size();
}

size_t Formatter::findPrevVisibleToken(size_t startIndex) {
    if (startIndex == 0) return tokens_.size();
    for (int i = static_cast<int>(startIndex) - 1; i >= 0; i--) {
        if (tokens_[i]->getChannel() == Token::DEFAULT_CHANNEL) {
            return static_cast<size_t>(i);
        }
    }
    return tokens_.size();
}

size_t Formatter::findMatchingEnd(size_t startIndex, size_t startType, size_t endType) {
    int depth = 0;
    for (size_t i = startIndex; i < tokens_.size(); i++) {
        size_t t = tokens_[i]->getType();
        if (t == startType) {
            depth++;
        } else if (t == endType) {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }
    return startIndex; // 未找到匹配
}

size_t Formatter::getTokenType(size_t index) {
    if (index >= tokens_.size()) return Token::INVALID_TYPE;
    return tokens_[index]->getType();
}

std::string Formatter::getTokenText(size_t index) {
    if (index >= tokens_.size()) return "";
    return tokens_[index]->getText();
}

bool Formatter::isManualLineBreak(size_t index) {
    return manualLineBreaks_.find(index) != manualLineBreaks_.end();
}

std::string Formatter::getIndentStr() {
    return std::string(indentLevel_ * config_.indentSize, ' ');
}

} // namespace yux

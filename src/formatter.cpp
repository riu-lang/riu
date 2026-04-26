// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化器实现
// 
// 本文件实现 yux 代码格式化功能:
// - 基于 Token 流重排实现格式化
// - 支持关键字后空格、运算符两边空格、逗号后空格等规则
// - 支持 2 空格缩进
// - 支持大括号独占行

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
    
    std::stringstream out;
    indentLevel_ = 0;
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
            }
            out << tokens_[i]->getText();
            continue;
        }
        
        // 处理行尾注释
        if (tokenType == yuxLexer::LineEndComment) {
            out << " " << tokens_[i]->getText();
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
        
        // 格式化当前 Token
        std::string formatted = formatToken(i);
        out << formatted;
        atLineStart_ = false;
        
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

void Formatter::collectTokens(CommonTokenStream& tokenStream) {
    tokens_.clear();
    for (auto* token : tokenStream.getTokens()) {
        tokens_.push_back(token);
    }
}

std::string Formatter::formatToken(size_t index) {
    std::stringstream out;
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
    for (int j = static_cast<int>(index) - 1; j >= 0; j--) {
        if (tokens_[j]->getChannel() == Token::DEFAULT_CHANNEL) {
            prevTokenType = tokens_[j]->getType();
            prevText = tokens_[j]->getText();
            break;
        }
    }
    
    // 查找后一个非 HIDDEN Token
    size_t nextTokenType = Token::INVALID_TYPE;
    std::string nextText;
    for (size_t j = index + 1; j < tokens_.size(); j++) {
        if (tokens_[j]->getChannel() == Token::DEFAULT_CHANNEL) {
            nextTokenType = tokens_[j]->getType();
            nextText = tokens_[j]->getText();
            break;
        }
    }
    
    // 判断是否需要在前方插入空格
    if (!atLineStart_ && needSpaceBefore(tokenType, prevTokenType, prevText, nextTokenType)) {
        out << " ";
    }
    
    out << text;
    
    return out.str();
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
        // struct N { 或 if cond { 或 fn name() {
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

bool Formatter::needSpaceAfter(size_t tokenType, size_t nextTokenType, const std::string& nextText) {
    // 关键字后需要空格
    if (isKeyword(tokenType)) {
        return config_.insertSpaceAfterKeyword;
    }
    
    // 逗号后需要空格
    if (tokenType == yuxLexer::SymbolComma) {
        return config_.insertSpaceAfterComma;
    }
    
    // 二元运算符后需要空格
    if (isBinaryOperator(tokenType)) {
        return config_.insertSpaceAroundOperator;
    }
    
    // 赋值符号后需要空格
    if (tokenType == yuxLexer::SymbolEq) {
        return config_.insertSpaceAroundOperator;
    }
    
    // 点号后不需要空格
    if (tokenType == yuxLexer::SymbolDot) {
        return false;
    }
    
    // 左括号后不需要空格
    if (tokenType == yuxLexer::ParStart || tokenType == yuxLexer::GetStart) {
        return false;
    }
    
    return false;
}

void Formatter::emitIndent(std::stringstream& out) {
    for (size_t i = 0; i < indentLevel_ * config_.indentSize; i++) {
        out << " ";
    }
}

void Formatter::emitNewline(std::stringstream& out) {
    out << "\n";
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

bool Formatter::isOperator(size_t tokenType) const {
    return isBinaryOperator(tokenType);
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

bool Formatter::isTypeKeyword(size_t tokenType) const {
    // yux 没有内置类型关键字，类型都是 ID
    return false;
}

bool Formatter::isIndentIncrease(size_t tokenType) const {
    return tokenType == yuxLexer::BlockStart;
}

bool Formatter::isIndentDecrease(size_t tokenType) const {
    return tokenType == yuxLexer::BlockEnd;
}

} // namespace yux

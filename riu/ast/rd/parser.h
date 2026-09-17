// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V parser（MIT）。产生式跟 riuParser.g4，不搬 V 的 ASI / import。
// rd.8 起 FileNode 主路走本栈；format / LSP 仍 ANTLR。

#ifndef RIU_LANG_RD_PARSER_H
#define RIU_LANG_RD_PARSER_H

#include "ast/rd/flat.h"
#include "ast/rd/scanner.h"
#include "ast/rd/token.h"

#include <string>
#include <string_view>
#include <vector>

namespace rd {

struct ParseResult {
    FlatAst ast;
    std::vector<ParseError> errors;
};

class Parser {
public:
    explicit Parser(std::string_view src);

    // Node.value 指向 src；调用方须在 dump / 使用树期间保持源存活。
    [[nodiscard]] ParseResult parse();

private:
    struct Mark {
        Token tok;
        std::vector<Token> peeked;
        Scanner::Snapshot scan;
    };

    void next();
    [[nodiscard]] const Token& la(int n);
    [[nodiscard]] bool at(Kind k) const { return tok_.kind == k; }
    [[nodiscard]] bool peekIs(Kind k, int n = 1);
    bool eat(Kind k);
    void skipLineEnds();
    void skipToLineEnd();
    void errorSyntax(std::string message);
    void errorExpected(Kind k);
    void errorUnknown();
    [[nodiscard]] Mark mark() const;
    void rewind(const Mark& m);

    [[nodiscard]] Pos spanTok(const Token& first, const Token& last) const;
    [[nodiscard]] Pos spanPos(const Pos& start, const Pos& last) const;
    [[nodiscard]] std::string_view sliceTokens(const Token& first, const Token& last) const;

    [[nodiscard]] NodeId parseItem();
    [[nodiscard]] NodeId parseUse();
    [[nodiscard]] NodeId parseAnno();
    [[nodiscard]] NodeId parseFn(std::vector<NodeId> annos);
    [[nodiscard]] NodeId parseFnClean();
    [[nodiscard]] NodeId parseExtern(std::vector<NodeId> annos);
    [[nodiscard]] NodeId parseLet(std::vector<NodeId> annos, bool global);
    [[nodiscard]] NodeId parseAlias();
    [[nodiscard]] NodeId parseEnum();
    [[nodiscard]] NodeId parseStruct(std::vector<NodeId> annos);
    [[nodiscard]] NodeId parseField(std::vector<NodeId> annos);

    void parseFnParams(std::vector<NodeId>& out);
    void parseFnParam(std::vector<NodeId>& out);
    void parseLambdaParams(std::vector<NodeId>& out);
    [[nodiscard]] NodeId parseOptionalFnRet();
    [[nodiscard]] NodeId parseFnBody();
    [[nodiscard]] NodeId parseBlock();
    [[nodiscard]] NodeId parseStatement();
    [[nodiscard]] NodeId parseLoop();
    [[nodiscard]] NodeId parseForIn();

    [[nodiscard]] bool looksLikeType() const;
    [[nodiscard]] bool aheadIsLambda();
    [[nodiscard]] bool aheadIsStructLit();
    [[nodiscard]] bool aheadIsEnumCtor();
    [[nodiscard]] bool aheadIsTrailingLambda();
    [[nodiscard]] NodeId parseType(int min_prec = 0);
    [[nodiscard]] NodeId parseTypePrimary();
    [[nodiscard]] NodeId parseTypePath();
    [[nodiscard]] NodeId parseGenericDef();
    [[nodiscard]] NodeId parseGenericArgs();
    [[nodiscard]] Kind eatTrailingAnd();

    [[nodiscard]] NodeId parseExpr(int min_bp = 0, bool allow_brace = true);
    [[nodiscard]] NodeId parsePrefix(bool allow_brace);
    [[nodiscard]] NodeId parsePostfix(NodeId left, bool allow_brace);
    [[nodiscard]] NodeId parseLiteral();
    [[nodiscard]] NodeId parseStringTpl();
    [[nodiscard]] NodeId parseCall(NodeId left, bool allow_brace);
    [[nodiscard]] NodeId parseGet(NodeId left);
    [[nodiscard]] NodeId parseTrailingLambda();
    [[nodiscard]] NodeId parseEnumCtor(NodeId lhs, bool lhs_is_self);
    [[nodiscard]] NodeId parseStructLit();
    [[nodiscard]] NodeId parseIf();
    [[nodiscard]] NodeId parseMatch();
    [[nodiscard]] NodeId parseTryCatch();
    [[nodiscard]] NodeId parseArrayOrInit();
    [[nodiscard]] NodeId parseParenLambdaOrTuple();
    void parseArgList(std::vector<NodeId>& args, Kind closer);

    std::vector<ParseError> errors_;
    std::string prev_text_;
    Scanner scanner_;
    Token tok_{};
    std::vector<Token> peeked_;
    FlatAst ast_;
};

// 解析 program：前导空行、use、顶层声明、EOF。错误表可空。
[[nodiscard]] ParseResult parseProgram(std::string_view src);

} // namespace rd

#endif // RIU_LANG_RD_PARSER_H

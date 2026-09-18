// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V token/position（MIT）。Kind 名沿用 g4 规则名，不是 V 关键字。
// rd.9 起编译 / check 主路；rd.10 format；rd.11 LSP。

#ifndef RIU_LANG_RD_TOKEN_H
#define RIU_LANG_RD_TOKEN_H

#include <cstdint>
#include <string>
#include <string_view>

namespace rd {

using i32 = std::int32_t;

// 词法规则名 → Kind。第二列是 dump 用的 symbolic name（EOF / INVALID 例外）。
#define RD_KIND_LIST(X)                                                                                                \
    X(Invalid, "INVALID")                                                                                              \
    X(Eof, "EOF")                                                                                                      \
    X(LineComment, "LineComment")                                                                                      \
    X(LineEndComment, "LineEndComment")                                                                                \
    X(Space, "Space")                                                                                                  \
    X(LineEnd, "LineEnd")                                                                                              \
    X(Break, "Break")                                                                                                  \
    X(Catch, "Catch")                                                                                                  \
    X(Continue, "Continue")                                                                                            \
    X(Elif, "Elif")                                                                                                    \
    X(Else, "Else")                                                                                                    \
    X(Enum, "Enum")                                                                                                    \
    X(Extern, "Extern")                                                                                                \
    X(False, "False")                                                                                                  \
    X(Fn, "Fn")                                                                                                        \
    X(For, "For")                                                                                                      \
    X(If, "If")                                                                                                        \
    X(In, "In")                                                                                                        \
    X(Let, "Let")                                                                                                      \
    X(Loop, "Loop")                                                                                                    \
    X(Match, "Match")                                                                                                  \
    X(Null, "Null")                                                                                                    \
    X(Ret, "Ret")                                                                                                      \
    X(SelfType, "SelfType")                                                                                            \
    X(Struct, "Struct")                                                                                                \
    X(True, "True")                                                                                                    \
    X(Try, "Try")                                                                                                      \
    X(TypeKw, "TypeKw")                                                                                                \
    X(Use, "Use")                                                                                                      \
    X(SymbolAdd, "SymbolAdd")                                                                                          \
    X(SymbolAddEq, "SymbolAddEq")                                                                                      \
    X(SymbolAnd, "SymbolAnd")                                                                                          \
    X(SymbolAndAnd, "SymbolAndAnd")                                                                                    \
    X(SymbolAt, "SymbolAt")                                                                                            \
    X(SymbolColon, "SymbolColon")                                                                                      \
    X(SymbolColonColon, "SymbolColonColon")                                                                            \
    X(SymbolComma, "SymbolComma")                                                                                      \
    X(SymbolDiv, "SymbolDiv")                                                                                          \
    X(SymbolDivEq, "SymbolDivEq")                                                                                      \
    X(SymbolDot, "SymbolDot")                                                                                          \
    X(SymbolEq, "SymbolEq")                                                                                            \
    X(SymbolEqEq, "SymbolEqEq")                                                                                        \
    X(SymbolEqMt, "SymbolEqMt")                                                                                        \
    X(SymbolExcl, "SymbolExcl")                                                                                        \
    X(SymbolExclEq, "SymbolExclEq")                                                                                    \
    X(SymbolHash, "SymbolHash")                                                                                        \
    X(SymbolLt, "SymbolLt")                                                                                            \
    X(SymbolLtSub, "SymbolLtSub")                                                                                      \
    X(SymbolMod, "SymbolMod")                                                                                          \
    X(SymbolModEq, "SymbolModEq")                                                                                      \
    X(SymbolMt, "SymbolMt")                                                                                            \
    X(SymbolMul, "SymbolMul")                                                                                          \
    X(SymbolMulEq, "SymbolMulEq")                                                                                      \
    X(SymbolOrOr, "SymbolOrOr")                                                                                        \
    X(SymbolQuest, "SymbolQuest")                                                                                      \
    X(SymbolRev, "SymbolRev")                                                                                          \
    X(SymbolSemicolon, "SymbolSemicolon")                                                                              \
    X(SymbolSub, "SymbolSub")                                                                                          \
    X(SymbolSubEq, "SymbolSubEq")                                                                                      \
    X(SymbolThis, "SymbolThis")                                                                                        \
    X(ParStart, "ParStart")                                                                                            \
    X(ParEnd, "ParEnd")                                                                                                \
    X(GetStart, "GetStart")                                                                                            \
    X(GetEnd, "GetEnd")                                                                                                \
    X(BlockStart, "BlockStart")                                                                                        \
    X(BlockEnd, "BlockEnd")                                                                                            \
    X(ID, "ID")                                                                                                        \
    X(INT, "INT")                                                                                                      \
    X(FLOAT, "FLOAT")                                                                                                  \
    X(STR_TPL_OPEN, "STR_TPL_OPEN")                                                                                    \
    X(STR_LINE_RAW, "STR_LINE_RAW")                                                                                    \
    X(CODE_POINT, "CODE_POINT")                                                                                        \
    X(INT_SUFFIX, "INT_SUFFIX")                                                                                        \
    X(INT_10, "INT_10")                                                                                                \
    X(INT_2, "INT_2")                                                                                                  \
    X(INT_8, "INT_8")                                                                                                  \
    X(INT_16, "INT_16")                                                                                                \
    X(FLOAT_SUFFIX, "FLOAT_SUFFIX")                                                                                    \
    X(FLOAT_DOT, "FLOAT_DOT")                                                                                          \
    X(FLOAT_EXP, "FLOAT_EXP")                                                                                          \
    X(NUN_SIGN, "NUN_SIGN")                                                                                            \
    X(DOT_NUM, "DOT_NUM")                                                                                              \
    X(STR_TPL_INTERP_OPEN, "STR_TPL_INTERP_OPEN")                                                                      \
    X(STR_TPL_DOLLAR_ID, "STR_TPL_DOLLAR_ID")                                                                          \
    X(STR_TPL_CLOSE, "STR_TPL_CLOSE")                                                                                  \
    X(STR_TPL_TEXT, "STR_TPL_TEXT")

enum class Kind : std::uint8_t {
#define RD_KIND_ENUM(name, dumpName) name,
    RD_KIND_LIST(RD_KIND_ENUM)
#undef RD_KIND_ENUM
};

// 半开区间 [offset, end)，UTF-8 字节下标。line 1-based；column 0-based（行内字节）。
// Token.text 是源上的 UTF-8 切片。
struct Pos {
    i32 offset = 0;
    i32 end = 0;
    i32 line = 1;
    i32 column = 0;
};

struct Token {
    Kind kind = Kind::Eof;
    Pos pos;
    std::string_view text; // 指向 Scanner 持有的源；Eof 为空
    i32 index = 0;         // default 通道连续下标（hidden 不占）；Eof 为 0
};

// 一条词法（E1001）或文法（E1002）错误。message 是错误码模板 `{}` 的实参。
// pos.column 0-based；主路渲染时 +1。
struct ParseError {
    bool is_lexer = false;
    Pos pos;
    std::string message;
    std::string offending;
    std::string prev_text;
};

[[nodiscard]] std::string_view kindName(Kind k);

// 精确匹配 g4 关键字；非关键字返回 Invalid。
[[nodiscard]] Kind keywordKind(std::string_view ident);

// dump 一行：KIND  line:col  offset-end  "escaped"\n（offset 为字节）。
[[nodiscard]] std::string formatTokenLine(std::string_view kind, Pos pos, std::string_view text);
[[nodiscard]] std::string formatTokenLine(const Token& tok);

} // namespace rd

#endif // RIU_LANG_RD_TOKEN_H

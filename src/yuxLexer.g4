// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

lexer grammar yuxLexer;

@header {
#include <vector>
}

@members {
public:
    // 字符串模板插值跟踪：每进入一次 ${ 压一个深度计数；
    // 在该计数内 { ++、} 先 --，到 0 表示遇到匹配 ${...} 的收尾 }，popMode。
    std::vector<int> interp_brace_depth;
}


LineComment
    : {getCharPositionInLine()==0}? Space* SymbolSemicolon ~[\r\n]* LineEnd -> channel(HIDDEN)
    ;

LineEndComment
    : Space+ SymbolSemicolon ~[\r\n]* -> channel(HIDDEN)
    ;

Space : ' ' -> channel(HIDDEN);
LineEnd : '\r'? '\n' | '\n' | EOF;
//WhiteSpace : ~[\P{White_Space} \t\r\n]+ -> channel(HIDDEN);

Break : 'break';
Catch: 'catch';
Draft: 'draft';
Elif : 'elif';
Else : 'else';
Enum : 'enum';
Extern : 'extern';
False : 'false';
Fn : 'fn';
If : 'if';
Let : 'let';
Loop: 'loop';
Match : 'match';
Null : 'null';
Ret : 'ret';
SelfType : 'Self';
Struct : 'struct';
True : 'true';
Try: 'try';
Use : 'use';

SymbolAdd: '+';
SymbolAddEq: '+=';
SymbolAnd: '&';
SymbolAndAnd: '&&';
SymbolColon: ':';
// 静态成员也考虑用此
SymbolColonColon: '::';
SymbolComma: ',';
SymbolDiv: '/';
SymbolDivEq: '/=';
SymbolDot: '.';
SymbolEq: '=';
SymbolEqEq: '==';
// match arm 箭头：`pattern => body`
SymbolEqMt: '=>';
SymbolExcl: '!';
SymbolExclEq: '!=';
SymbolHash: '#';
SymbolLt: '<';
SymbolMod: '%';
SymbolModEq: '%=';
SymbolMt: '>';
SymbolMul: '*';
SymbolMulEq: '*=';
SymbolOr: '|';
SymbolOrOr: '||';
SymbolQuest: '?';
fragment SymbolQuote2: '"';
fragment SymbolQuote: ['];
SymbolRev: '~';
SymbolSemicolon: ';';
SymbolSub: '-';
SymbolSubEq: '-=';
// 当前作用域（同级的对象，相当于$所在代码中上一级的对象，像this）
SymbolThis: '$';
SymbolXor: '^';
SymbolXorEq: '^=';

ParStart: '(';
ParEnd: ')';
GetStart: '[';
GetEnd: ']';
BlockStart: '{' { if (!interp_brace_depth.empty()) interp_brace_depth.back()++; };
BlockEnd: '}' {
    if (!interp_brace_depth.empty()) {
        if (interp_brace_depth.back() == 0) {
            interp_brace_depth.pop_back();
            popMode();
        } else {
            interp_brace_depth.back()--;
        }
    }
};


fragment ID_HEAD : ~[\u0021-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}];
fragment ID_TAIL : ~[\u0021-\u002F\u003A-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}];
ID : ID_HEAD ID_TAIL*;
INT : NUN_SIGN? (INT_10|INT_2|INT_8|INT_16) INT_SUFFIX?;
FLOAT : NUN_SIGN? (INT_10|FLOAT_DOT|FLOAT_EXP) FLOAT_SUFFIX?;
STR_TPL_OPEN : SymbolQuote2 -> pushMode(StrTpl);
STR_LINE_RAW : 'r' SymbolQuote2 ~[\r\n]*? SymbolQuote2;
CODE_POINT: 'c' '\''
    ( '\\' [bnrtv0\\']
    | ~[\r\n\\']
    )
    '\'';

// 低优先级

INT_SUFFIX: [iu]('8'|'16'|'32'|'64');

INT_10: [0-9]('_'?[0-9]+)*;
INT_2: '0b'[01]('_'?[01]+)*;
INT_8: '0o'[0-7]('_'?[0-7]+)*;
INT_16: '0x'[0-9a-fA-F]('_'?[0-9a-fA-F]+)*;

FLOAT_SUFFIX: 'f' ('32'|'64');
FLOAT_DOT: INT_10 DOT_NUM;
FLOAT_EXP: FLOAT_DOT 'e' '-'? INT_10;

NUN_SIGN: '-'|'+';

DOT_NUM: SymbolDot INT_10 ;

mode StrTpl;
STR_TPL_INTERP_OPEN : '${' { interp_brace_depth.push_back(0); } -> pushMode(DEFAULT_MODE);
STR_TPL_DOLLAR_ID : '$' ~[\u0021-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}] ~[\u0021-\u002F\u003A-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}]*;
STR_TPL_CLOSE : '"' -> popMode;
STR_TPL_TEXT : ('\\' . | ~["\\$\r\n])+;

grammar yux;

options {
    language=Cpp;
}

program
   : (fn|comment|codeLineEnd)* EOF;

comment
    : LineComment
    ;

codeLineEnd: LineEndComment? LineEnd;

///////////
// 字面量
///////////

literal:
    num=number #literalNumber
    | (True|False) #literalBool
    // 变量等
    | name=ID #literalObj
    ;

number: numInt|numFloat;
// 整数
numInt: INT;
// 浮点数
numFloat: FLOAT;

///////////
// 函数
///////////

// fn name() {}
fn: fnHeader Space fnBody;

// fn name() 空返回
// fn name() type 返回 type
fnHeader: Fn Space name=ID ParStart
    (params+=fnParam (SymbolComma Space params+=fnParam)* )?
    ParEnd (Space retType=ID)?
    ;

fnParam: name=ID Space type=ID;

fnBody: fnExprkBody | fnBlockBody;

fnExprkBody: LineEnd?
    SymbolEq Space expr codeLineEnd?
    ;

fnBlockBody: statementBlock;

///////////
// 表达式
///////////

expr:
    // ( e )
      ParStart expr ParEnd # exprParen
    // if e {
    // ...
    // } elif e {
    // ...
    // } else {
    // ...
    // }
    | If Space condition=expr Space
          statementBlock
          (elifs+=exprElIf)*
             exprElse? # exprIfElse
    // e() e(e) e(e,e)
    | left=expr ParStart
        ( args+=expr
          (SymbolComma Space args+=expr)*
        )?
      ParEnd # exprCall
    // 判断
    | left=expr Space op=(SymbolEqEq|SymbolMt|SymbolMtEq|SymbolLt|SymbolLtEq) Space right=expr # exprCompare
    // a.b ...
    | left=expr SymbolDot member+=ID # exprDot
    | left=expr Space op=(SymbolAdd|SymbolSub) Space right=expr # exprAddSub
     // e * e e / e
    | left=expr Space op=(SymbolMul|SymbolDiv|SymbolMod) Space right=expr # exprMulDivMod
    | literal # exprLiteral;

// elif {
// ...
// }
exprElIf : Space Elif Space condition=expr Space statementBlock;
// else {
// ...
// }
exprElse : Space Else Space statementBlock;


///////////
// 语句
///////////

statement:
    // var name = expr
    // var name type = expr
     DeclKey Space name=ID Space (type=ID Space)? SymbolEq Space expr codeLineEnd #statementDeclareAssign
    // obj = expr
    | obj=ID Space SymbolEq Space expr codeLineEnd #statementAssign
    // 尾随;表示空类型（void）
    | expr SymbolSemicolon? codeLineEnd # statementExpr
    // ret value
    | Ret expr codeLineEnd # statementRet
    ;

statementBlock:
    BlockStart codeLineEnd
        (Space+|statement|comment|codeLineEnd)*
    BlockEnd;

//

LineComment
    : {getCharPositionInLine()==0}? Space* SymbolDiv ~[\r\n]* LineEnd
    ;

LineEndComment
    : Space SymbolSemicolon ~[\r\n]*
    ;

Space : ' ';
LineEnd : '\r'? '\n' | '\n' | EOF;
EmptyLine : {getCharPositionInLine()==0}? [ \t]*  LineEnd -> channel(HIDDEN);
//WhiteSpace : ~[\P{White_Space} \t\r\n]+ -> channel(HIDDEN);

Elif : 'elif';
Else : 'else';
False : 'false';
Fn : 'fn';
If : 'if';
Null : 'null';
Ret : 'ret';
True : 'true';
DeclKey: 'va'[rl] | 'cval';

SymbolAdd: '+';
SymbolArrow: '->';
SymbolComma: ',';
SymbolDiv: '/';
SymbolDot: '.';
SymbolEq: '=';
SymbolEqEq: '==';
SymbolExcl: '!';
SymbolExclEq: '!=';
SymbolLt: '<';
SymbolLtEq: '<=';
SymbolMod: '%';
SymbolMt: '>';
SymbolMtEq: '>=';
SymbolMul: '*';
SymbolQuest: '?';
SymbolQuote2: '"';
SymbolQuote: ['];
SymbolSemicolon: ';';
SymbolSub: '-';

ParStart: '(';
ParEnd: ')';
GetStart: '[';
GetEnd: ']';
BlockStart: '{';
BlockEnd: '}';


ID : ~[\u0021-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}] ~[\u0021-\u002F\u003A-\u0040\u005B-\u005E\u0060\u007B-\u007F\p{White_Space}]*;
INT : NUN_SIGN? (INT_10|INT_2|INT_8|INT_16) INT_SUFFIX?;
FLOAT : NUN_SIGN? (INT_10|FLOAT_DOT|FLOAT_EXP) FLOAT_SUFFIX?;
STR_LINE : SymbolQuote2 ('\\'.|~[\r\n\\])*? SymbolQuote2;
STR_LINE_RAW : 'r' SymbolQuote2 ~[\r\n]*? SymbolQuote2;

// 低优先级

INT_SUFFIX: [iu]('8'|'16'|'32'|'64')?;

INT_10: [0-9]('_'?[0-9]+)*;
INT_2: '0b'[01]('_'?[01]+)*;
INT_8: '0o'[0-7]('_'?[0-7]+)*;
INT_16: '0x'[0-9a-fA-F]('_'?[0-9a-fA-F]+)*;

FLOAT_SUFFIX: 'f' ('32'|'64');
FLOAT_DOT: INT_10 '.' INT_10;
FLOAT_EXP: FLOAT_DOT 'e' '-'? INT_10;

NUN_SIGN: '-'|'+';



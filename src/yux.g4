grammar yux;

options {
    language=Cpp;
}

program:
   (
    fn
   | externDelc
   | globalConst
   | structDecl
   | structImpl
   | comment
   | codeLineEnd
   )* EOF;

comment
    : LineComment
    ;

codeLineEnd: LineEndComment? LineEnd;

// 外部声明
// extern {
//  函数头
// }
externDelc: Extern Space BlockStart
    (
     Space* fnHeader
     | comment
     | codeLineEnd
    )*
    BlockEnd
    ;

// cval a i32 = 1
globalConst: 'cval' Space name=ID Space type Space SymbolEq Space literal;

///////////
// 字面量
///////////

literal:
    num=number #literalNumber
    | (True|False) #literalBool
    // 变量等
    | name=ID #literalObj
    | STR_LINE # literalStringLine
    | Null # literalNull
    ;

number: numInt|numFloat;
// 整数
numInt: INT;
// 浮点数
numFloat: FLOAT;

type:
    ID # typeNormal
   // | type SymbolQuest # typeNullable
   | ID SymbolLt types+=type (SymbolComma Space types+=type)* SymbolMt # typeGeneric
    // [ type * count ]
   | GetStart type Space SymbolMul Space INT GetEnd # typeArray;


///////////
// 函数
///////////

// fn ~()
fnClean: Fn Space SymbolRev ParStart ParEnd Space
      fnBody
      ;

// fn name() {}
fn: fnHeader Space fnBody;

// fn name() 空返回
// fn name() type 返回 type
fnHeader: Fn Space name=ID ParStart
    (params+=fnParam (SymbolComma Space params+=fnParam)* )?
    ParEnd (Space retType=type)?
    ;

fnParam: name=ID Space type;

fnBody: fnExprkBody | fnBlockBody;

fnExprkBody: LineEnd?
    SymbolEq Space expr codeLineEnd?
    ;

fnBlockBody: statementBlock;

///////////
// 结构体
///////////

// struct A
// struct A<T1, T2>
structDecl: Struct Space name=ID (SymbolLt types+=type (SymbolComma Space types+=type)* SymbolMt)? Space BlockStart
   (
      (Space* filedDecl codeLineEnd?)
     | comment
     | codeLineEnd
   )*
   BlockEnd
   ;

structImpl: name=ID (SymbolLt types+=type (SymbolComma Space types+=type)* SymbolMt)? Space BlockStart
     codeLineEnd
    (Space* fnClean)?
    (
      ( Space* fn)
     | comment
     | codeLineEnd
    )*
    BlockEnd
    ;

filedDecl: name=ID Space type;

///////////
// 表达式
///////////

expr:
    // ( e )
      ParStart expr ParEnd # exprParen
    // &a.b => Ref<T>
    | SymbolAnd obj=ID (SymbolDot subs+=ID)* # exprGetRef
    // e[a, b, c] 实际应为成员函数get的快捷调用
    | expr
        GetStart
            args+=expr
            (SymbolComma Space args+=expr)*
        GetEnd # exprGet
    // if 1 { 1 } else { 2 }
    | If Space condition=expr Space BlockStart Space trueValue=expr Space BlockEnd
        Space Else Space BlockStart Space falseValue=expr Space BlockEnd # exprOneLineIfElse
    // true if condition else false 类python
    | trueValue=expr Space If Space condition=expr Space
        Else Space falseValue=expr # exprIfElsePreValue
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
    // [0 ...] [1u8 ... u8] 填充数组
    | GetStart value=literal Space SymbolDotDotDot (Space type)?  GetEnd # exprArrayInit
    // a.b ...
    | left=expr SymbolDot member+=ID # exprDot
    // [e1, e2]
    | GetStart (velues+=expr (SymbolComma Space velues+=expr)* )? GetEnd # exprArray
    // e() e(e) e(e,e)
    | left=expr ParStart
        ( args+=expr
          (SymbolComma Space args+=expr)*
        )?
      ParEnd # exprCall
    // !e ~e -e 没有空格，低于成员访问优先级
    | op=(SymbolSub|SymbolRev|SymbolExcl) right=expr # exprUnary
    // e & e | e ^ e
    | left=expr Space op=(SymbolAnd|SymbolOr|SymbolXor|SymbolLtLt|SymbolMtMt) Space right=expr # exprBinOp
     // e * e e / e
    | left=expr Space op=(SymbolMul|SymbolDiv|SymbolMod) Space right=expr # exprMulDivMod
    | left=expr Space op=(SymbolAdd|SymbolSub) Space right=expr # exprAddSub
    // 判断
    | left=expr Space op=(SymbolEqEq|SymbolExclEq|SymbolMt|SymbolMtEq|SymbolLt|SymbolLtEq|SymbolOrOr|SymbolAndAnd) Space right=expr # exprCompare
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
     DeclKey Space name=ID Space (type Space)? SymbolEq Space expr codeLineEnd #statementDeclareAssign
    // e[a, b, c] = e 实际应为成员函数set的快捷调用
    | obj=expr GetStart
          args+=expr
          (SymbolComma Space args+=expr)*
        GetEnd Space SymbolEq Space value=expr # statementSet
    // 循环
    | Loop Space statementBlock # statementLoop
    // obj.member = expr
    | obj=ID (SymbolDot subs+=ID)* Space SymbolEq Space expr codeLineEnd #statementAssign
    // 尾随;表示空类型（void）
    | expr SymbolSemicolon? codeLineEnd # statementExpr
    // ret value
    | Ret Space expr codeLineEnd # statementRet
    // ret; 返回空，强制尾随;表示空返回
    | Ret SymbolSemicolon# statementRetVoid
    // break; 强制尾随;不返回任何值
    | Break SymbolSemicolon # statementBreak
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

Break : 'break';
DeclKey: 'va'[rl] | 'cval';
Elif : 'elif';
Else : 'else';
Extern : 'extern';
False : 'false';
Fn : 'fn';
If : 'if';
Loop: 'loop';
Null : 'null';
Ret : 'ret';
Struct : 'struct';
True : 'true';

SymbolAdd: '+';
SymbolAnd: '&';
SymbolAndAnd: '&&';
SymbolArrow: '->';
SymbolComma: ',';
SymbolDiv: '/';
SymbolDot: '.';
SymbolDotDot: '..';
SymbolDotDotDot: '...';
SymbolEq: '=';
SymbolEqEq: '==';
SymbolExcl: '!';
SymbolExclEq: '!=';
SymbolLt: '<';
SymbolLtEq: '<=';
SymbolLtLt: '<<';
SymbolMod: '%';
SymbolMt: '>';
SymbolMtEq: '>=';
SymbolMtMt: '>>';
SymbolMul: '*';
SymbolOr: '|';
SymbolOrOr: '||';
SymbolQuest: '?';
SymbolQuote2: '"';
SymbolQuote: ['];
SymbolRev: '~';
SymbolSemicolon: ';';
SymbolSub: '-';
SymbolXor: '^';

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



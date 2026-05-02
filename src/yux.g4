// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

grammar yux;

// 内涵c++代码，其它目标需转义，重写目标在命令行控制
options {
    language=Cpp;
}

program:
   comment*
   imports*
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

/////////
// 导入
/////////

imports: Use pkgs+=ID (SymbolDot pkgs+=ID)* (SymbolDot useAll=SymbolMul)? codeLineEnd;

// 外部声明
// extern {
//  函数头
// }
// 预留注解，未来可用于指定链接哪个库
externDelc: (buildAnnos+=buildAnno)* Extern BlockStart
    (
     fnHeader
     | comment
     | codeLineEnd
    )*
    BlockEnd
    ;

// cval a i32 = 1
globalConst: (buildAnnos+=buildAnno)* 'cval' name=ID type SymbolEq literal;

//////////////
// 构建注解
//////////////

// #Name
buildAnno:
    SymbolHash
    name=ID
    codeLineEnd
    ;

///////////
// 字面量
///////////

literal:
    num=number #literalNumber
    | (True|False) #literalBool
    // 变量等
    | name=ID #literalObj
    | STR_LINE # literalStringLine
    | STR_LINE_RAW # literalStringLineRaw
    // 编译为u32
    | CODE_POINT # literalCodePoint
    | Null # literalNull
    ;

number: numInt|numFloat;
// 整数
numInt: INT;
// 浮点数
numFloat: FLOAT;

type:
    ID #typeNormal
   | type SymbolQuest #typeNullable
    // A<T> B<T1, T2>
   | ID genericDef #typeGeneric
    // [ type * count ]
   | GetStart type SymbolMul INT GetEnd #typeArray;

typeWithRef:
    ID SymbolAnd? #typeNormalWithRef
    | type SymbolQuest SymbolAnd? #typeNullableWithRef
     // A<T> B<T1, T2>
    | ID genericDefWithRef SymbolAnd? #typeGenericWithRef
     // [ type * count ]
    | GetStart typeWithRef SymbolMul INT GetEnd SymbolAnd? #typeArrayWithRef;

// 共享
genericDef: SymbolLt types+=type (SymbolComma types+=type)* SymbolMt;

genericDefWithRef: SymbolLt types+=typeWithRef (SymbolComma types+=typeWithRef)* SymbolMt;

///////////
// 函数
///////////

// fn ~()
fnClean: Fn SymbolRev ParStart ParEnd
      fnBody
      ;

// fn name() {}
// 只有函数头的必须要有构建注解（比如用代码生成函数体，未来实现）
fn: fnHeader fnBody?;

// fn name() 空返回
// fn name() type 返回 type
// fn <T> some() T
fnHeader: (buildAnnos+=buildAnno)*
    Fn genericDef? name=ID ParStart
    fnParams?
    ParEnd (retType=type)?
    ;

fnParams: fnParam (SymbolComma params+=fnParam)* ;

fnParam: fnParamStd | fnParamGroup;

// a i32
fnParamStd: name=ID typeWithRef;

// a, b i32
// a, b, c i32
fnParamGroup: (names+=ID SymbolComma)* names+=ID typeWithRef;

fnBody: fnExprkBody | fnBlockBody;

fnExprkBody: LineEnd?
    SymbolEq expr codeLineEnd?
    ;

fnBlockBody: statementBlock;

///////////
// 结构体
///////////

// struct A
// struct A<T1, T2>
structDecl: (buildAnnos+=buildAnno)*
   Struct name=ID (SymbolLt types+=type (SymbolComma types+=type)* SymbolMt)? BlockStart
   (
      (filedDecl codeLineEnd?)
     | comment
     | codeLineEnd
   )*
   BlockEnd
   ;

structImpl: (buildAnnos+=buildAnno)* name=ID (SymbolLt types+=type (SymbolComma types+=type)* SymbolMt)? BlockStart
     codeLineEnd
    fnClean?
    (
      fn
     | comment
     | codeLineEnd
    )*
    BlockEnd
    ;

filedDecl: name=ID type;

///////////
// 表达式
///////////

expr:
    // ( e )
      ParStart expr ParEnd # exprParen
    // &a.b => T&
    | SymbolAnd obj=(ID|SymbolThis) (SymbolDot subs+=ID)* # exprGetRef
    // e[a, b, c] 实际应为成员函数get的快捷调用
    | expr
        GetStart
            args+=expr
            (SymbolComma args+=expr)*
        GetEnd # exprGet
    // if 1 { 1 } else { 2 }
    | If condition=expr BlockStart trueValue=expr BlockEnd
        Else BlockStart falseValue=expr BlockEnd # exprOneLineIfElse
    // true if condition else false 类python
    | trueValue=expr If condition=expr
        Else falseValue=expr # exprIfElsePreValue
    // if e {
    // ...
    // } elif e {
    // ...
    // } else {
    // ...
    // }
    | If condition=expr
          statementBlock
          (elifs+=exprElIf)*
             exprElse? # exprIfElse
    // [0 ...] [1u8 ... u8] 填充数组
    | GetStart value=literal SymbolDot SymbolDot SymbolDot type?  GetEnd # exprArrayInit
    // a.b
    // a?.b
    | left=expr SymbolQuest? SymbolDot member+=ID # exprDot
    // [e1, e2]
    | GetStart (velues+=expr (SymbolComma velues+=expr)* )? GetEnd # exprArray
    // e() e(e) e(e,e) e<T>()
    | left=expr (SymbolColon genericDef)? ParStart
        ( args+=expr
          (SymbolComma args+=expr)*
        )?
      ParEnd # exprCall
    // !e ~e -e 没有空格，低于成员访问优先级
    | op=(SymbolSub|SymbolRev|SymbolExcl) right=expr # exprUnary
    | left=expr opShift right=expr # exprShift
    // e & e | e ^ e
    | left=expr op=(SymbolAnd|SymbolOr|SymbolXor) right=expr # exprBinOp
     // e * e e / e
    | left=expr op=(SymbolMul|SymbolDiv|SymbolMod) right=expr # exprMulDivMod
    | left=expr op=(SymbolAdd|SymbolSub) right=expr # exprAddSub
    // 判断
    | left=expr opCompare right=expr # exprCompare
    // == 需要小于比大小
    | left=expr opEq right=expr # exprEq
    // 布尔
    | left=expr opBool right=expr # exprBool
    | literal # exprLiteral
    // e ?? e
    | expr SymbolQuest SymbolQuest expr #exprNullElse
    | SymbolThis #exprThis
    ;

// elif {
// ...
// }
exprElIf : Elif condition=expr statementBlock;
// else {
// ...
// }
exprElse : Else statementBlock;

// 移位操作符: << >>
opShift: SymbolLt SymbolLt | SymbolMt SymbolMt;

// 比较操作符: > >= < <=
opCompare:
      SymbolMt
    | SymbolMt SymbolEq
    | SymbolLt
    | SymbolLt SymbolEq
    ;

// 布尔操作符: || &&
opBool:
      SymbolOrOr
    | SymbolAndAnd
    ;

// == !=
opEq:
      SymbolEqEq
    | SymbolExclEq
    ;

// 赋值操作符: = += -= *= /= %= >>= <<=
opAssign:
      SymbolEq
    | SymbolAddEq
    | SymbolSubEq
    | SymbolMulEq
    | SymbolDivEq
    | SymbolModEq
    | SymbolMt SymbolMt SymbolEq
    | SymbolLt SymbolLt SymbolEq
    ;

///////////
// 语句
///////////

statement:
    // val a i32
     DeclKey name=ID type codeLineEnd #statementDeclare
    // var name = expr
    // var name type = expr
    | DeclKey name=ID typeWithRef? SymbolEq expr codeLineEnd #statementDeclareAssign
    // e[a, b, c] = e 实际应为成员函数set的快捷调用
    | obj=expr GetStart
          args+=expr
          (SymbolComma args+=expr)*
        GetEnd SymbolEq value=expr # statementSet
    // 循环
    | Loop statementBlock # statementLoop
    // obj.member = expr
    | obj=(ID|SymbolThis) (SymbolDot subs+=ID)*
        opAssign
        expr codeLineEnd #statementAssign
    // 尾随;表示空类型（void）
    | expr SymbolSemicolon? codeLineEnd # statementExpr
    // ret value
    | Ret expr codeLineEnd # statementRet
    // ret; 返回空，强制尾随;表示空返回
    | Ret SymbolSemicolon codeLineEnd # statementRetVoid
    // break; 强制尾随;不返回任何值
    | Break SymbolSemicolon codeLineEnd # statementBreak
    ;

statementBlock:
    BlockStart codeLineEnd
        (statement|comment|codeLineEnd)*
    BlockEnd;

//

LineComment
    : {getCharPositionInLine()==0}? Space* SymbolSemicolon ~[\r\n]* LineEnd
    ;

LineEndComment
    : Space+ SymbolSemicolon ~[\r\n]*
    ;

Space : ' ' -> channel(HIDDEN);
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
Use : 'use';

SymbolAdd: '+';
SymbolAddEq: '+=';
SymbolAnd: '&';
SymbolAndAnd: '&&';
SymbolColon: ':';
SymbolComma: ',';
SymbolDiv: '/';
SymbolDivEq: '/=';
SymbolDot: '.';
SymbolEq: '=';
SymbolEqEq: '==';
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
SymbolQuote2: '"';
SymbolQuote: ['];
SymbolRev: '~';
SymbolSemicolon: ';';
SymbolSub: '-';
SymbolSubEq: '-=';
// 当前作用域
SymbolThis: '$';
SymbolXor: '^';
SymbolXorEq: '^=';

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
CODE_POINT: 'c' '\''
    ( '\\' [bnrtv0\\']
    | ~[\r\n\\']
    )
    '\'';

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



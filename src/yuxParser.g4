// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

parser grammar yuxParser;

options {
    tokenVocab=yuxLexer;
}


program:
    LineEnd*
    imports*
    (
          fn
        | externDelc
        | globalConst
        | aliasDecl
        | enumDecl
        | draftDecl
        | structDecl
        | structImpl
        | LineEnd
    )* EOF
    ;

/////////
// 导入
/////////

// use a.b.c.*
imports:
    Use pkgs+=ID (SymbolDot pkgs+=ID)*
    (SymbolDot useAll=SymbolMul)?
    LineEnd
    ;

// 外部声明
// extern {
//  函数头
// }
// 预留注解，未来可用于指定链接哪个库
externDelc: (buildAnnos+=buildAnno)* Extern BlockStart LineEnd
    (
          (fnHeader LineEnd)
        | LineEnd
    )*
    BlockEnd LineEnd
    ;

// cval a i32 = 1
globalConst:
    (buildAnnos+=buildAnno)*
    Cval name=ID type SymbolEq literal
    LineEnd
    ;

// Arr<T> = type
aliasDecl:
    ID genericDef?
    SymbolEq
    type
    LineEnd
    ;

//////////////
// 构建注解
//////////////

// #Name
buildAnno:
    SymbolHash
    name=ID
    (
        ParStart
          arg=ID
        ParEnd
    )?
    LineEnd
    ;

///////////
// 字面量
///////////

literal:
      num=number        #literalNumber
    | (True|False)      #literalBool
    // 变量等
    | name=ID           #literalObj
    | stringTemplate    # literalStringTpl
    | STR_LINE_RAW      # literalStringLineRaw
    // 编译为u32
    | CODE_POINT        # literalCodePoint
    | Null              # literalNull
    ;

// 字符串模板（Kotlin 风，无前缀）。空模板等价于旧 STR_LINE。
// $ident 复用 ID 字符集；${expr} 内允许任意表达式（含块）。
stringTemplate:
    STR_TPL_OPEN
    templatePart*
    STR_TPL_CLOSE
    ;

templatePart:
      STR_TPL_TEXT                                # tplText
    | STR_TPL_DOLLAR_ID                           # tplDollarId
    | STR_TPL_INTERP_OPEN expr BlockEnd           # tplInterp
    ;

number: numInt|numFloat;
// 整数
numInt: INT;
// 浮点数
numFloat: FLOAT;

type:
      ID #typeNormal
    | type SymbolQuest        #typeNullable
    // A<T> B<T1, T2>
    | ID genericDef           #typeGeneric
    // [ type * count ]
    | GetStart
        type SymbolMul INT
      GetEnd                   #typeArray
    // (T1, T2)
    | ParStart types+=type
        (SymbolComma types+=type)+
      ParEnd                   #typeTuple
    // fn(T)R / fn?(T)R / fn() / 名可省 / 组糖 a, b T
    | Fn SymbolQuest?
      ParStart
        fnTypeParams?
      ParEnd
      retType=typeWithRef?     #typeFn
    ;

typeWithRef:
    // 注意：typeNullableWithRef 必须排在 typeNormalWithRef 前面 ——
    // 否则 `T?` 会被 typeNormalWithRef 吃掉 `T` 后把 `?` 漏给外层 typeNullable，
    // 导致 fn 类型字面量的 retType `i32?` 被错切成 `(fn(...)i32)?`（违反 [#24]）
      type SymbolQuest SymbolAnd?       #typeNullableWithRef
    | ID SymbolAnd? #typeNormalWithRef
    // A<T> B<T1, T2>
    | ID genericDefWithRef SymbolAnd?   #typeGenericWithRef
    // [ type * count ]
    | GetStart
        typeWithRef SymbolMul INT
      GetEnd
      SymbolAnd?                        #typeArrayWithRef
    // (T1, T2)
    | ParStart
        types+=typeWithRef
        (SymbolComma types+=typeWithRef)+
      ParEnd                            #typeTupleWithRef
    // 函数类型出现在 typeWithRef 位（fn 形参 / 返回值）；外层 & 借用一个 fn 值
    | Fn SymbolQuest?
      ParStart
        fnTypeParams?
      ParEnd retType=typeWithRef?
      SymbolAnd?                        #typeFnWithRef
    ;

// fn 类型字面量参数：名可省（仅文档），允许组糖 a, b T；类型用 typeWithRef（支持 T&）
fnTypeParams:
    fnTypeParam
    (SymbolComma LineEnd* fnTypeParam)*
    SymbolComma?
    LineEnd*
    ;

fnTypeParam:
      (names+=ID SymbolComma LineEnd*)+
      names+=ID typeWithRef         # fnTypeParamGroup
    | name=ID typeWithRef           # fnTypeParamNamed
    | typeWithRef                   # fnTypeParamUnnamed
    ;

// typeParam: 单个类型形参 / 类型实参槽位。
// 仅在**声明位**（fn / struct / draft 的 genericDef 槽位）允许 `:` 边界；
// 类型引用位（如 Box<T>）与调用点 turbofish 处必须无 bounds，由 semantic 层拒绝。
typeParam:
    type
    (
        SymbolColon bounds+=type
        (SymbolAdd bounds+=type)*
    )?
    ;

// 共享
genericDef:
    SymbolLt
        params+=typeParam
        (SymbolComma params+=typeParam)*
    SymbolMt
    ;

genericDefWithRef:
    SymbolLt
        types+=typeWithRef
        (SymbolComma types+=typeWithRef)*
    SymbolMt
    ;

///////////
// 枚举
///////////

// enum E {
//   V1
//   V2(T1, T2)
//   ...
// }
// variant 一行一个，行尾不写 `,`；空 enum 由语义层拒绝
enumDecl:
    Enum name=ID BlockStart
    (
          LineEnd
        | variants+=enumVariant
    )*
    BlockEnd
    ;

// 短名 + 可选 tuple-style payload；零参 variant 不写括号
enumVariant:
    name=ID
    (
        ParStart
        payloads+=type
        (SymbolComma payloads+=type)*
        ParEnd
    )?
    LineEnd
    ;

///////////
// 函数
///////////

// fn ~()
fnClean:
    Fn SymbolRev
    ParStart ParEnd
    fnBody
    ;

// fn name() {}
// 只有函数头的必须要有构建注解（比如用代码生成函数体，未来实现）
fn: fnHeader fnBody?;

// fn name() 空返回
// fn name() type 返回 type
// fn some<T>() T
// retType 仅 #CompilerInner baked builtin 允许含 `&`（spec §8.9 例外、§8.3.5.5 as_ref）；
// 用户代码 retType 含 `&` 由 semantic 层拒绝
fnHeader:
    (buildAnnos+=buildAnno)*
    Fn name=ID genericDef?
    ParStart LineEnd*
        fnParams?
    ParEnd
    (retType=typeWithRef)?
    ;

// 单行 a i32, b i32
// 多行：每参一行、强制尾随 `,`（由格式化器保证；语法上尾逗号可选）
fnParams:
    fnParam
    (
        SymbolComma LineEnd*
        params+=fnParam
    )*
    SymbolComma?
    LineEnd*
    ;

fnParam: fnParamStd | fnParamGroup;

// 参数注解：#Frozen 等；LineEnd 可选支持 inline 与换行两种形态。
// 与 buildAnno 分离：参数注解零实参槽，行尾换行非强制。
paramAnno:
    SymbolHash
    name=ID
    LineEnd?
    ;

// #Frozen a i32
// a i32
fnParamStd:
    (paramAnnos+=paramAnno)*
    name=ID typeWithRef
    ;

// a, b i32
// a, b, c i32
// #Frozen a, b i32  （注解共享给组内所有名字）
fnParamGroup:
    (paramAnnos+=paramAnno)*
    (names+=ID SymbolComma)*
    names+=ID
    typeWithRef
    ;

// lambda 形参：类型可省（由上下文推断）；允许组糖 a, b T
lambdaParams:
    lambdaParam
    (
        SymbolComma LineEnd* lambdaParam
    )*
    SymbolComma?
    LineEnd*
    ;

lambdaParam:
      (names+=ID SymbolComma LineEnd*)+
      names+=ID typeWithRef?          # lambdaParamGroup
    | name=ID typeWithRef?            # lambdaParamStd
    ;

// lambda 单表达式体的非左递归包装：迫使内部 expr 以新优先级 0 启动，
// 否则 `(a, b) => a + b` 会被 ANTLR 切成 `((a, b) => a) + b`
lambdaBody: expr;

// 尾随 lambda 调用糖（§4.5）：仅块形 lambda 可作尾随
trailingLambda:
      BlockStart LineEnd*
        lambdaParams SymbolEqMt
        LineEnd*
        (statement|LineEnd)*
      BlockEnd                # trailingLambdaBlock
    | BlockStart LineEnd
        (statement|LineEnd)*
      BlockEnd                # trailingLambdaZeroBlock
    ;

fnBody: fnExprkBody | fnBlockBody;

fnExprkBody:
    LineEnd?
    SymbolEq
    expr
    LineEnd?
    ;

fnBlockBody: statementBlock;

///////////
// draft 待中文命名
///////////

draftType:
    name=ID
    (
        SymbolLt
            types+=type
            (SymbolComma types+=type)*
        SymbolMt
    )?
    ;

draftDecl:
    (buildAnnos+=buildAnno)*
    Draft draftType
    BlockStart
        (
            (fnHeader LineEnd)
          | LineEnd
        )*
    BlockEnd
    ;

///////////
// 结构体
///////////

structType:
    name=ID
    (
        SymbolLt
            types+=type
            (SymbolComma types+=type)*
        SymbolMt
    )?
    ;

// struct A
// struct A<T1, T2>
structDecl:
    (buildAnnos+=buildAnno)*
    Struct structType
    BlockStart
        (filedDecl|LineEnd)*
    BlockEnd
    ;

structImpl:
    (buildAnnos+=buildAnno)* structType
    (SymbolColon (drafts+=draftType (SymbolAdd drafts+=draftType)*)?)?
    BlockStart LineEnd
    fnClean?
    (fn|LineEnd)*
    BlockEnd
    ;

filedDecl: name=ID type LineEnd;

///////////
// 表达式
///////////

expr:
    // 单参裸形 lambda: x => expr （单 ID 不与 fn 类型字面量歧义）
    // body 走 lambdaBody 包装规则：避免 ANTLR4 左递归把 `x => a + b` 误切成 `(x => a) + b`
      name=ID SymbolEqMt body=lambdaBody  # exprLambdaSingle
    // 括参形 lambda: (args) RetT? => expr
    | ParStart lambdaParams? ParEnd
      retType=typeWithRef?
      SymbolEqMt body=lambdaBody          # exprLambdaParen
    // 块形 lambda: { args => stmts }
    | BlockStart LineEnd*
        lambdaParams
        SymbolEqMt
        (statement|LineEnd)*
      BlockEnd                            # exprLambdaBlock
    // 0 参块 lambda: { stmts } —— 禁写 =>
    | BlockStart LineEnd
        (statement|LineEnd)*
      BlockEnd                            # exprLambdaZeroBlock
    // ( e )
    | ParStart expr ParEnd                # exprParen
    // &a.b => T&
    | SymbolAnd obj=(ID|SymbolThis)
      (LineEnd* SymbolDot subs+=ID)*      # exprGetRef
    // e[a, b, c] 实际应为成员函数get的快捷调用
    | expr
        GetStart LineEnd*
            args+=expr
            (SymbolComma LineEnd* args+=expr)*
            SymbolComma? LineEnd*
        GetEnd                            # exprGet
    // try {
    // } catch e E1 {
    // } catch e E2 {
    // }
    | Try tryBlock=statementBlock
      (catchs+=catchArm)+                 # exprTryCatch
    // if 1 { 1 } else { 2 }
    | If condition=expr
      BlockStart trueValue=expr
      BlockEnd Else BlockStart falseValue=expr
      BlockEnd                            # exprOneLineIfElse
    // true if condition else false 类python
    | trueValue=expr If condition=expr
      Else falseValue=expr                # exprIfElsePreValue
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
      exprElse?               # exprIfElse
    // match e {
    //   enum => expr
    //   else => {
    //    ...
    //   }
    // }
    | Match expr BlockStart LineEnd
        (
           (arms+=matchArm LineEnd)
          |LineEnd
        )+
      BlockEnd                #exprMatch
    // 枚举构造：E::V / E::V() / E::V(a, b, ...)
    // 零参 variant 写带不带括号等价；类型别名 C 处亦合法（C::V 解析期等价 E::V）
    | enumName=ID SymbolColonColon variant=ID
      (
        ParStart LineEnd*
            (
                args+=expr
                (SymbolComma LineEnd* args+=expr)*
                SymbolComma?
                LineEnd*
            )?
        ParEnd
      )?                      # exprEnumCtor
    // [0 ...] [1u8 ... u8] 填充数组
    | GetStart
        value=literal
        SymbolDot SymbolDot SymbolDot
         type?
      GetEnd                  # exprArrayInit
    // a.b
    // a?.b
    // 链式：`.` 前允许换行（a\n  .b\n  .c）
    | left=expr LineEnd* SymbolQuest? SymbolDot member+=ID    # exprDot
    // a.0
    | left=expr LineEnd* member=DOT_NUM                       # exprTupleMember
    // [e1, e2]
    | GetStart LineEnd*
        (
            velues+=expr
            (SymbolComma LineEnd* velues+=expr)*
            SymbolComma? LineEnd*
        )?
      GetEnd                                                  # exprArray
    // e() e(e) e(e,e) e<T>() / 尾随块 lambda： e(args) { ... } 或 e { ... }（唯一实参时省括号）
    | left=expr (SymbolColon genericDef)? ParStart LineEnd*
        ( args+=expr
            (SymbolComma LineEnd* args+=expr)*
            SymbolComma? LineEnd*
        )?
        ParEnd
        trailing=trailingLambda?
        errPropagate=SymbolExcl?                              # exprCall
    | left=expr
        (SymbolColon genericDef)?
      trailing=trailingLambda
      errPropagate=SymbolExcl?                                # exprCallTrailingOnly
    // !e ~e -e 没有空格，低于成员访问优先级
    | op=(SymbolSub|SymbolRev|SymbolExcl) right=expr          # exprUnary
    | left=expr opShift right=expr # exprShift
    // e & e | e ^ e
    | left=expr op=(SymbolAnd|SymbolOr|SymbolXor) right=expr  # exprBinOp
    // e * e e / e
    | left=expr op=(SymbolMul|SymbolDiv|SymbolMod) right=expr # exprMulDivMod
    | left=expr op=(SymbolAdd|SymbolSub) right=expr           # exprAddSub
    // 判断
    | left=expr opCompare right=expr # exprCompare
    // == 需要小于比大小
    | left=expr opEq right=expr      # exprEq
    // 布尔
    | left=expr opBool right=expr    # exprBool
    | literal                        # exprLiteral
    // e ?? e
    | expr
      SymbolQuest SymbolQuest
      expr                           #exprNullElse
    // (e1, e2)
    | ParStart
        values+=expr
        (SymbolComma values+=expr)+
      ParEnd                         #exprTuple
    | SymbolThis                     #exprThis
    ;

// elif {
// ...
// }
exprElIf : Elif condition=expr statementBlock;
// else {
// ...
// }
exprElse : Else statementBlock;

// catch e T {}
catchArm:
    Catch err=ID type
    statementBlock
    ;

// 移位操作符: << >>
opShift:
      SymbolLt SymbolLt
    | SymbolMt SymbolMt
    ;

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

// match arm: 模式 => 单表达式体
// v1 仅支持单表达式体；多语句体押后（spec §5.5 statement 形态留给后续）
matchArm: pattern=enumPattern SymbolEqMt body=expr;

// match 模式（v1 子集）：
//   E::V             零参 variant
//   E::V()           零参 variant（与上等价）
//   E::V(a, b, ...)  按位置绑定 payload 元素到不可变名
//   else             兜底分支，必须出现在最后一条
enumPattern:
    enumName=ID SymbolColonColon variant=ID
        (
          ParStart
             binds+=ID (SymbolComma binds+=ID)*
          ParEnd
        )?         #patternEnum
    | Else         #patternElse
    ;

///////////
// 语句
///////////

statement:
    // val a i32
      DeclKey name=ID type LineEnd                        #statementDeclare
    // var name = expr
    // var name type = expr
    | DeclKey name=ID typeWithRef? SymbolEq expr LineEnd  #statementDeclareAssign
    // cval name type = expr （局部编译期常量；类型必填）
    | Cval name=ID typeWithRef SymbolEq expr LineEnd      #statementCvalDeclAssign
    // var (a, b) = e
    | DeclKey
      ParStart
        names+=ID (SymbolComma names+=ID)+
      ParEnd
      typeWithRef?
      SymbolEq expr LineEnd                               #statementDeclareAssignTuple
    // e[a, b, c] = e 实际应为成员函数set的快捷调用
    | obj=expr
      GetStart
          args+=expr
          (SymbolComma args+=expr)*
      GetEnd
      SymbolEq
      value=expr
      LineEnd                         # statementSet
    // 循环
    | Loop statementBlock LineEnd     # statementLoop
    // obj.member = expr
    | obj=(ID|SymbolThis)
      (SymbolDot subs+=ID | subs+=DOT_NUM)*
      opAssign
      expr LineEnd                    #statementAssign
    // 尾随;表示空类型（void）
    | expr SymbolSemicolon? LineEnd   # statementExpr
    // ret value
    | Ret expr LineEnd                # statementRet
    // ret; 返回空，强制尾随;表示空返回
    | Ret SymbolSemicolon LineEnd     # statementRetVoid
    // break; 强制尾随;不返回任何值
    | Break SymbolSemicolon LineEnd   # statementBreak
    ;

statementBlock:
    BlockStart LineEnd
        (statement|LineEnd)*
    BlockEnd
    ;


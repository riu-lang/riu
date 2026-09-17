// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

parser grammar riuParser;

options {
    tokenVocab=riuLexer;
}

@members {
    // `{` 后若是 `(` 则不是 struct lit，让给尾随 lambda `f { () => e }`。
    // 必须在备选入口判定，否则 typePath `{` 已匹配、无法回退。
    bool aheadIsStructLit() {
        int i = 1;
        int t = _input->LT(i)->getType();
        if (t == SelfType) {
            i = 2;
        } else if (t == ID) {
            i = 2;
            while (_input->LT(i)->getType() == SymbolDot && _input->LT(i + 1)->getType() == ID) {
                i += 2;
            }
        } else {
            return false;
        }
        if (_input->LT(i)->getType() != BlockStart) return false;
        return _input->LT(i + 1)->getType() != ParStart;
    }
}


program:
    LineEnd*
    imports*
    (
          fn
        | externDelc
        | letGlobal
        | aliasDecl
        | enumDecl
        | structDecl
        | LineEnd
    )* EOF
    ;

/////////
// 导入
/////////

// use a.b.c / use a.b.c.* / use a.b.MyType
// 末段是否类型由 sema 分流（先当模块路径，否则当父模块里的类型）；产生式与 typePath 同形
imports:
    Use typePath
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

// DRAFT-let-unify §3 全局 let 形态。RHS 接 expr 以支持 const-eval（DRAFT-const-eval Phase 2）
// 与 static-vars（DRAFT-static-vars Phase 1）；非 const-evaluable 在 sema 期由 ConstEvaluator
// 判定后报 E3140。其他档位由 ast_builder 拒。type / init 缺失由 ast_builder 报 E3113 / E3114。
letGlobal:
    (letAnnos+=letAnno)*
    Let name=ID type? (SymbolEq expr)? LineEnd
    ;

// let 声明专用注解（无单参槽）：LineEnd 可选——
// 约定 inline 用于局部 / 参数位、顶行换行用于全局 / 字段 / fn 位，
// 但语法层接受两种形态；具体场景的形态合法性由格式化器/lint 后置（非 P1.a 范围）。
letAnno:
    SymbolHash name=ID LineEnd?
    ;

// type Name = T（左侧无 genericDef；右侧 type 仍可写 Pair<A, B> / T?）
aliasDecl:
    TypeKw name=ID
    SymbolEq
    type
    LineEnd
    ;

//////////////
// 构建注解
//////////////

// #Name / #Name(ID) / #Name(ID<T>) / #Name("str") / #Name(42) / #Name(type)
// annoArg 是注解参数的多态形态（spec §11.1.1.1 子集扩展）：
//   - ID + 可选 turbofish：兼容 #Fallible(SomeErr) / #Impl(Spec<T>) 等
//   - 数字字面量：INT / FLOAT
//   - 字符串：双引号无插值纯文本 "..." 或 raw string r"..."
//   - type：完整类型引用（含泛型、nullable 等）
buildAnno:
    SymbolHash
    name=ID
    (
        ParStart
          annoArg
        ParEnd
    )?
    LineEnd
    ;

annoArg:
      arg=ID genericDef?                       // #Impl(Spec<T>), #Fallible(E)
    | argNum=INT                               // #Align(8)
    | argNum=FLOAT                             // #Version(1.0)
    | argStr=STR_LINE_RAW                      // r"..."
    | argTPL=STR_TPL_OPEN argText+=STR_TPL_TEXT* STR_TPL_CLOSE  // "no-interpolation"
    | argType=type                             // #Impl(Rc<String>)
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

// 限定类型路径：裸名 `Map` 或 `riu.core.map.Map`。身份解析在 sema（T2）。
typePath:
    segs+=ID (SymbolDot segs+=ID)*
    ;

// 唯一类型产生式：尾部可选 `&`（原 type）。持有位（字段 / 别名 / 全局 /
// enum payload）的裸 T& / Array<T&> / [T& * N] 由 sema E4039 拒，不在语法层拆。
// 选择支顺序：
//   - typeFallible 在 typeNullable 之前，以支持 `i32? ! E`
//   - typeNullable 在 typeNormal 之前，否则 `T?` 被 typePath 吃掉 T 后把 `?` 漏掉
type:
    // T ! E（Function<..., T ! E> 末位等）；base 走 type 以支持 T? ! E
      base=type SymbolExcl errType=type SymbolAnd? #typeFallible
    // T? / T?&
    | type SymbolQuest SymbolAnd?       #typeNullable
    // A<T> B<T1, T2>；可带路径前缀 `riu.core.map.Map<i32>`
    // 实参槽接 genericDefWithRef：允许 `Function<i32&, ()>` / `Array<i32&>` 进 AST。
    | typePath genericDefWithRef SymbolAnd?   #typeGeneric
    | typePath SymbolAnd?              #typeNormal
    // [PROBE static-fn] Self / Self&
    | SelfType SymbolAnd?             #typeSelf
    // [ type * count ] / [T& * N] / 外层再 &
    | GetStart
        type SymbolMul INT
      GetEnd
      SymbolAnd?                       #typeArray
    // () → unit 类型（不接尾部 &）
    | ParStart ParEnd                  #typeUnit
    // (T1, T2)；元素可带 &，元组本身不接尾部 &
    | ParStart types+=type
        (SymbolComma types+=type)+
      ParEnd                           #typeTuple
    ;

// typeParam: 声明位类型形参（fn / struct / #Spec 头部 genericDef）。
// 类型引用与调用点 turbofish 走 genericDefWithRef（可含 T&），不再走 typeParam；
// 声明位 `:` 边界仍只出现在本产生式。
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
        types+=type
        (SymbolComma types+=type)*
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
// enum E<T> { V(T) } / enum E<T, U> { ... }：genericDef 与 fn / alias 同槽。
// 头上 `<T : D>` 语法收下、语义拒（#3 简单切片）。variant 一行一个，行尾不写 `,`；空 enum 由语义层拒绝
enumDecl:
    Enum name=ID genericDef? BlockStart
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
// retType 含 `&` 合法；合法性由 §8.6.10 溯源约束在 semantic 层保证
// 用户代码可返回 T&，源须来自 T& 形参（外发借用）或全局/静态/cval（静态借用）
fnHeader:
    (buildAnnos+=buildAnno)*
    Fn name=ID genericDef?
    ParStart LineEnd*
        fnParams?
    ParEnd
    ( retType=type (SymbolExcl errType=type)?
    | SymbolExcl errType=type
    )?
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
    name=ID type
    ;

// a, b i32
// a, b, c i32
// #Frozen a, b i32  （注解共享给组内所有名字）
fnParamGroup:
    (paramAnnos+=paramAnno)*
    (names+=ID SymbolComma)*
    names+=ID
    type
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
      names+=ID type?          # lambdaParamGroup
    | name=ID type?            # lambdaParamStd
    ;

// lambda 单表达式体的非左递归包装：迫使内部 expr 以新优先级 0 启动，
// 否则 `(a, b) => a + b` 会被 ANTLR 切成 `((a, b) => a) + b`
lambdaBody: expr;

// 尾随 lambda（仅函数调用，Kotlin 风）：foo(args){ (params) Ret? => stmts }
// 形参列表必须带括号（0 参写 ()）；Ret 可省，由上下文 / 体推断。
trailingLambda:
    BlockStart LineEnd*
      ParStart lambdaParams? ParEnd
      ( retType=type (SymbolExcl errType=type)?
      | SymbolExcl errType=type
      )?
      SymbolEqMt
      LineEnd*
      (statement|LineEnd)*
    BlockEnd
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
// 结构体 待中文命名
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

// struct A { fields*; fnClean?; fns* }
// struct A<T1, T2> { ... }
// 声明合一（spec-unify v1）：字段段在前、fn 段在后；
// 析构 `fn ~()` 居中；spec 实现关系由顶行 `#Impl(Spec)` 注解承载，不入头部槽。
structDecl:
    (buildAnnos+=buildAnno)*
    Struct structType
    BlockStart LineEnd
        (filedDecl|aliasDecl|LineEnd)*
        fnClean?
        (fn LineEnd | LineEnd)*
    BlockEnd
    ;

// DRAFT-static-vars Phase 4: 静态字段通过 #Static 注解 + 可选 init 表达。
// 语法层统一走 filedDecl（实例字段 / 静态字段共用），sema 层按注解分流。
filedDecl:
    (buildAnnos+=buildAnno)*
    name=ID type (SymbolEq init=expr)? LineEnd
    ;

// [PROBE static-fn] 字段字面量项：.name = expr LineEnd
fieldInit:
    SymbolDot name=ID SymbolEq value=expr LineEnd
    ;

///////////
// 表达式
///////////

expr:
    // 前缀 lambda: (args) RetT? => expr  或  (args) RetT? => { stmts }
    // body 走 lambdaBody 包装规则：避免 ANTLR4 左递归把 `(a, b) => a + b` 误切成 `((a, b) => a) + b`
    // 语句体走 statementBlock；`{ stmts }` 不再单独作为 lambda 表达式（尾随见 trailingLambda）
      ParStart lambdaParams? ParEnd
      ( retType=type (SymbolExcl errType=type)?
      | SymbolExcl errType=type
      )?
      SymbolEqMt
      (statementBlock | body=lambdaBody)  # exprLambdaParen
    // [PROBE static-fn] 结构体字段字面量：Self { \n .x = e \n .y = e \n }
    // DRAFT-const-eval Phase 5: LHS 放宽到通用 ID（如 Point { .x = 1 .y = 2 }）
    // 限定路径：riu.core.map.Map { ... }；命名形态多行强制；`.field=` 前缀标明字段项
    // 单字段简写（仅单行）：Type{ expr } / Self{ expr }，位置填充到唯一实例字段
    // `{` 后若是 `(` 不走简写，避免吃掉尾随 lambda `f { () => e }`
    | {aheadIsStructLit()}? (selfLhs=SelfType | typeName=typePath) BlockStart
        ( LineEnd (fieldInits+=fieldInit|LineEnd)*
        | positional=expr
        )
      BlockEnd                            # exprStructLit
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
    //   else => { stmts }
    // }
    | Match expr BlockStart LineEnd
        (
           (arms+=matchArm LineEnd)
          |LineEnd
        )+
      BlockEnd                #exprMatch
    // 路径调用：承载两种语义，由 sema 按 LHS 类型分流
    //   - 枚举构造：E::V / E::V() / E::V(args) / path.E::V
    //   - 静态函数调用：Type::name(args) / Type:<T>::name:<U>(args) / Self::name(args)
    // 零参 variant 写带不带括号等价；类型别名 C 处亦合法（C::V 解析期等价 E::V）
    // [PROBE static-fn] LHS 加 Self 入口；前后各加可选 turbofish
    | (enumName=typePath | selfLhs=SelfType)
      (SymbolColon lhsGenerics=genericDefWithRef)?
      SymbolColonColon variant=ID
      (SymbolColon rhsGenerics=genericDefWithRef)?
      (
        ParStart LineEnd*
            (
                args+=expr
                (SymbolComma LineEnd* args+=expr)*
                SymbolComma?
                LineEnd*
            )?
        ParEnd
      )?
      errPropagate=SymbolExcl? # exprEnumCtor
    // [0 ...] [1u8 ... u8] 填充数组
    | GetStart
        value=literal
        SymbolDot SymbolDot SymbolDot
         type?
      GetEnd                  # exprArrayInit
    // a.b
    // a?.b
    // 链式：`.` 前允许换行（a\n  .b\n  .c）
    | left=expr LineEnd* SymbolQuest? SymbolDot member+=ID
        (SymbolAt specQual=ID)?                               # exprDot
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
    // e() e(e) e(e,e) e<T>() / 尾随 lambda： e(args){ (params) => stmts } 或 e { () => stmts }（唯一实参时省括号）
    | left=expr (SymbolColon genericDefWithRef)? ParStart LineEnd*
        ( args+=expr
            (SymbolComma LineEnd* args+=expr)*
            SymbolComma? LineEnd*
        )?
        ParEnd
        trailing=trailingLambda?
        errPropagate=SymbolExcl?                              # exprCall
    | left=expr
        (SymbolColon genericDefWithRef)?
      trailing=trailingLambda
      errPropagate=SymbolExcl?                                # exprCallTrailingOnly
    // !e -e 没有空格，低于成员访问优先级（按位取反改走 .inv()）
    | op=(SymbolSub|SymbolExcl) right=expr                    # exprUnary
    // e * e e / e
    | left=expr op=(SymbolMul|SymbolDiv|SymbolMod) right=expr # exprMulDivMod
    | left=expr op=(SymbolAdd|SymbolSub) right=expr           # exprAddSub
    // 判断
    | left=expr opCompare right=expr # exprCompare
    // == 需要小于比大小
    | left=expr opEq right=expr      # exprEq
    // 布尔
    | left=expr opBool right=expr    # exprBool
    // e ?? e —— 右结合；高于 <-（`x <- y ?? z` = `x <- (y ?? z)`）
    | <assoc=right> expr
      SymbolQuest SymbolQuest
      expr                           #exprNullElse
    // a <- b  —— 移出旧值、替换新值、返回旧值（表达式，右结合，二元最低）
    | <assoc=right> left=expr SymbolLtSub right=expr  # exprMoveAssign
    | literal                        # exprLiteral
    // () → unit 值
    | ParStart ParEnd               #exprUnit
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

// 赋值操作符: = += -= *= /= %=
opAssign:
      SymbolEq
    | SymbolAddEq
    | SymbolSubEq
    | SymbolMulEq
    | SymbolDivEq
    | SymbolModEq
    ;

// match arm: 模式 => 单表达式体 或 语句块（块可单行，与 lambda / if 同）
matchArm: pattern=enumPattern SymbolEqMt (statementBlock | body=expr);

// match 模式（v1 子集）：
//   E::V             零参 variant
//   E::V()           零参 variant（与上等价）
//   E::V(a, b, ...)  按位置绑定 payload 元素到不可变名
//   else             兜底分支，必须出现在最后一条
enumPattern:
    enumName=typePath SymbolColonColon variant=ID
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
    // DRAFT-let-unify §3：let 局部声明（注解严格 inline）
    // 默认 let → val 语义；#Mut → var；#Cval → cval；#Frozen → val + frozen
    // type / init 同时缺失由 ast_builder 报 E3113；type 在但 init 缺由 E3114
    // #Mut 例外：允许 `#Mut let x T` 无 init（延后赋值，等价旧 `var x T`）
      (letAnnos+=letAnno)*
      Let name=ID type? (SymbolEq expr)? LineEnd?   #statementLet
    // DRAFT-let-unify §3：let 元组解构。注解语义与 statementLet 同（默认→val / #Mut→var / #Cval→cval）。
    | (letAnnos+=letAnno)*
      Let
      ParStart
        names+=ID (SymbolComma names+=ID)+
      ParEnd
      type?
      SymbolEq expr LineEnd?                               #statementLetTuple
    // e[a, b, c] = e 实际应为成员函数set的快捷调用
    | obj=expr
      GetStart
          args+=expr
          (SymbolComma args+=expr)*
      GetEnd
      SymbolEq
      value=expr
      LineEnd?                         # statementSet
    // Type::FIELD = expr 静态字段写（DRAFT-static-vars Phase 5）；LHS 可为 typePath
    | typeName=typePath SymbolColonColon fieldName=ID
      SymbolEq
      value=expr
      LineEnd?                         # statementStaticFieldSet
    // 循环
    | (ID SymbolColon)? Loop loopInit? statementBlock LineEnd?     # statementLoop
    // label: for a in e
    | (ID SymbolColon)? For ID In expr statementBlock LineEnd?     # statementForIn
    // obj.member = expr
    | obj=(ID|SymbolThis)
      (SymbolDot subs+=ID | subs+=DOT_NUM)*
      opAssign
      expr LineEnd?                    #statementAssign
    // 尾随;表示空类型（void）；LineEnd 可省（块末 `}` 充当终结）
    | expr SymbolSemicolon? LineEnd?   # statementExpr
    // ret value
    | Ret expr LineEnd?                # statementRet
    // ret; 返回空，强制尾随;表示空返回
    | Ret SymbolSemicolon LineEnd?     # statementRetVoid
    // break; 强制尾随;不返回任何值
    | Break (SymbolAt ID)? SymbolSemicolon LineEnd?     # statementBreak
    | Continue (SymbolAt ID)? SymbolSemicolon LineEnd?  # statementContinue
    | aliasDecl                                         # statementAlias
    ;

// `{` 后 / 语句后 / `}` 前换行均可省：`fn f() { ret 1 }`、`if c { a } else { b }` 合法。
statementBlock:
    BlockStart LineEnd*
        (statement|LineEnd)*
    BlockEnd
    ;

// loop init 子句：loop name = expr { } / loop (a, b) = expr { }
// 不以 LineEnd 结尾 —— '{' 充当终结符（类似 if cond { 中的 cond）
loopInit:
    // 单变量：loop i = 0 { } / loop i i64 = 0 { }
    name=ID type? SymbolEq expr
    |
    // tuple 解构：loop (i, n) = (0, arr.len()) { }
    ParStart names+=ID (SymbolComma names+=ID)+ ParEnd
    type?
    SymbolEq expr
    ;

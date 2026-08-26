# 附录 B：语法汇总

> 权威来源：[`yux/ast/yuxParser.g4`](../../yux/ast/yuxParser.g4)。本附录是规范化摘录（简化记号见 §2.1.1），不得与 `.g4` 冲突；当 `.g4` 与本附录不一致时**应当**修订本附录。

## B.1 顶层

```
program        ::= comment*
                   imports*
                   ( fn
                   | externDecl
                   | globalConst
                   | aliasDecl
                   | enumDecl
                   | structDecl
                   | comment
                   | codeLineEnd
                   )*
                   EOF

comment        ::= LineComment
codeLineEnd    ::= LineEndComment? LineEnd

imports        ::= 'use' ID ('.' ID)* ('.' '*')? codeLineEnd

externDecl     ::= buildAnno* 'extern' '{'
                       ( fnHeader | comment | codeLineEnd )*
                   '}'

globalConst    ::= buildAnno* 'let' ID typeWithRef? '=' expr codeLineEnd
                   ; 按注解分三档：无注解=val（运行期 init）/ #Mut=var（运行期 init，可变）/ #Cval=const（编译期求值）
                   ; 三者互斥（E3115）；缺 init→E3154（val/#Mut）或 E3114（#Cval）
                   ; RHS expr 语义按档位分流（#Cval→常量表达式，val/#Mut→任意 expr，§5.1.4）

aliasDecl      ::= ID genericDef? '=' type codeLineEnd

buildAnno      ::= '#' ID ( '(' annoArg ')' )? codeLineEnd
annoArg        ::= ID ('<' typeParam (',' typeParam)* '>')?
                 | INT | FLOAT
                 | STR_LINE_RAW
                 | '"' STR_TPL_TEXT* '"'
                 | type
                 ; 参数接受 ID（含 turbofish）/ 数字 / 字符串（无插值） / type 引用
```

## B.2 类型

```
type           ::= ID                              # typeNormal
                 | type '?'                        # typeNullable
                 | ID genericDef                   # typeGeneric
                 | '[' type '*' INT ']'            # typeArray
                 | '(' ')'                        # typeUnit
                 | '(' type (',' type)+ ')'        # typeTuple

typeWithRef    ::= type '?' '&'?                   # typeNullableWithRef
                 | ID '&'?                         # typeNormalWithRef
                 | ID genericDefWithRef '&'?       # typeGenericWithRef
                 | '[' typeWithRef '*' INT ']' '&'?# typeArrayWithRef
                 | '(' ')'                        # typeUnitWithRef
                 | '(' typeWithRef (',' typeWithRef)+ ')' # typeTupleWithRef

genericDef        ::= '<' typeParam        (',' typeParam)*        '>'
genericDefWithRef ::= '<' typeParamWithRef (',' typeParamWithRef)* '>'

typeParam         ::= type        (':' draftBound ('+' draftBound)*)?
typeParamWithRef  ::= typeWithRef (':' draftBound ('+' draftBound)*)?
draftBound        ::= modulePath? ID genericDef?     # 例：ToString / pkg.Display / To<i32>
```

约束：

- `typeWithRef` 仅出现在函数参数与局部变量声明位置（§3.2 / §8.3.1）；其它位置只能用 `type`。
- `typeParam` 的 spec 边界仅出现在**声明位**（`fn` / `struct` / `#Spec struct` 头部的 `genericDef` 槽位）；调用点 turbofish `f:<T>(args)` 处**不得**写边界（§6.4.4.3）。

> 上述边界产生式 spec-unify v1 已落地 `yux/ast/yux.g4`；`draftBound` 产生式名沿用历史 token 名，语义为"spec 边界"（§12）。

## B.2a `Dyn<D>` / `Dyn<D&>`（v0.5+）

`Dyn` 是编译器内置类型名（非关键字）。`Dyn<D>` 与 `Dyn<D&>` 作为 `typeGeneric` / `typeGenericWithRef` 形态出现；语义见 §12.9。约束：

- `Dyn<D&>` 中 `&` 仅在 `genericDefWithRef` 实参槽合法（即 `typeWithRef` 位）；
- `Dyn<...>` 不得嵌套 `Dyn` / `Rc<Dyn>` / `Weak<Dyn>` / `Dyn<D>?`（语义层拒绝，E1132 / E1135）。

构造形态走 `exprCall` 的 turbofish 形：`Dyn:<D>(box_u)` / `Dyn:<D&>(u_ref)`；无 `:` 写法 `Dyn<D>` 仅在类型位有效。

## B.2b `Function<P..., Ret>`（函数类型）

`Function` 是编译器内置类型名（非关键字），走 `typeGeneric` / `typeGenericWithRef`。语义见 §3.11。

- 末位类型实参永远是返回类型；至少 1 个实参。`Function<()>` = 0 参 unit 返回。
- 可空走标准 `?`：`Function<i32, i32>?`。布局仍是 16 字节 fat-ptr（`fn_ptr == null` 表空），不套 `Nullable` 外壳。
- `Weak<Function<...>>` 禁。`extern fn` 形参 / 返回禁。
- 含 `T&` 的类型实参仅 `genericDefWithRef` 槽合法（与 `Dyn<D&>` 同）。

## B.3 字面量

```
literal        ::= number              # literalNumber
                 | ('true' | 'false')  # literalBool
                 | ID                  # literalObj
                 | STR_LINE            # literalStringLine
                 | STR_LINE_RAW        # literalStringLineRaw
                 | CODE_POINT          # literalCodePoint
                 | 'null'              # literalNull

number         ::= numInt | numFloat
numInt         ::= INT
numFloat       ::= FLOAT
```

> `literalObj` 让 `#Cval let NAME T = X` 中的 `X` 可以是另一个 `#Cval let` 名（编译期常量传播）。

## B.4 函数

```
fnClean        ::= 'fn' '~' '(' ')' fnBody

fn             ::= fnHeader fnBody?

fnHeader       ::= buildAnno*
                   'fn' genericDef? ID '(' LineEnd*
                       fnParams?
                   ')' (retType=type)?

fnParams       ::= fnParam (',' LineEnd* fnParam)* ','? LineEnd*
fnParam        ::= fnParamStd | fnParamGroup
fnParamStd     ::= paramAnno* ID typeWithRef
fnParamGroup   ::= paramAnno* (ID ',')* ID typeWithRef
paramAnno      ::= '#' ID LineEnd?

fnBody         ::= fnExprkBody | fnBlockBody
fnExprkBody    ::= LineEnd? '=' expr codeLineEnd?
fnBlockBody   ::= statementBlock
```

## B.5 结构体

spec-unify v1（2026-05-19）将 struct 声明与方法块合一为单一 `structDecl`；spec（接口契约）由 `#Spec` 顶行注解承载；spec 实现关系由 `#Impl(D)` 顶行注解承载（§12.1 / §12.2）。

```
structDecl     ::= buildAnno*                              ; 顶行可含 #Spec / #Impl(D) / #Builtin 等
                   'struct' ID ('<' type (',' type)* '>')? '{'
                       ( filedDecl | staticFieldDecl | LineEnd )*
                       fnClean?
                       ( fn LineEnd | LineEnd )*
                   '}'

filedDecl      ::= buildAnno* ID type LineEnd

staticFieldDecl ::= buildAnno* ID type '=' expr LineEnd
                    ; 必须含 #Static，可叠 #Mut；v1 必须 init（E3150）
                    ; 泛型 struct 上禁（E3157）
```

约束（语义层）：

- `#Spec` 形态下 body 内只允许 `fn` 签名（无 body），不允许 `filedDecl` / `fnClean`（§12.1.1.1 / §11.4.1）。
- 非 spec 形态可含字段（实例 `filedDecl` + 静态 `staticFieldDecl`）、`fnClean`（析构 `fn ~()`，居于字段之后、其它 `fn` 之前）、实例方法 / 静态工厂（`#Static fn`）；构造函数形态已删除（§7.3.1.1），构造唯一通道为 `#Static fn` + `Self { ... }` 字段字面量。
- `#Impl(D)` 接受单参数糖 `(ID genericDef?)`，可重复出现，宣告该 struct 实现 D。

## B.5b 枚举（v0.x）

```
enumDecl       ::= 'enum' ID '{'
                       ( enumVariant | comment | codeLineEnd )*
                   '}'

enumVariant    ::= ID ( '(' type (',' type)* ')' )?
```

- variant 一行一个、行尾**不写** `,`（§3.10.2.2）。
- payload 类型用 `type`（不接 `typeWithRef`，§3.10.3.2）；零参 variant 不写括号。
- 空 enum（无 variant）由语义层拒绝（§3.10.2.5）。
- enum 值的读取仅经 `match`（B.6 `exprMatch`）；构造仅经 B.6 `exprEnumCtor`。

## B.5a spec（v0.5+）

spec 声明形态共用 §B.5 `structDecl`，由 `#Spec` 顶行注解切换：

```yux
#Spec
struct D {
  fn m1(...) R
  fn m2(...) R
  ; ...
}
```

约束：

- spec body 内**只允许 `fn` 签名**（不带函数体），违反报 `E1139`；字段 / 析构 `fn ~()` 禁用，违反报 `E2011`；详见 §12.1.1.1。
- spec 签名集**允许为空**（§12.1.1.2）。
- spec 自身可携带 `genericDef`，但 spec 体内单个 `fn` **不得**再引入泛型形参（§12.3.2 / `E1104`）。
- spec 声明上**禁带** `#Impl(D)`（§11.4.1.4）。

历史 `draftDecl` 产生式 spec-unify v1 已删除（参 [CHANGELOG 2026-05-19](CHANGELOG.md)）。

## B.6 表达式（按 `yux.g4` 中 `expr` 的分支顺序，决定优先级）

```
expr ::=
    '(' expr ')'                                                 # exprParen
  | '&' (ID | '$') (LineEnd* '.' ID)*                            # exprGetRef
  | expr '[' LineEnd* expr (',' LineEnd* expr)* ','? LineEnd* ']' # exprGet
  | 'if' expr '{' expr '}' 'else' '{' expr '}'                   # exprOneLineIfElse
  | 'if' expr statementBlock exprElIf* exprElse?                 # exprIfElse
  | 'match' expr '{' codeLineEnd
        ((matchArm codeLineEnd) | comment)+
    '}'                                                          # exprMatch
  | 'try' statementBlock catchArm+                               # exprTryCatch  ; DRAFT-错误.md §5
  | ID '::' ID ( '(' codeLineEnd*
                     (expr (',' LineEnd* expr)* ','? codeLineEnd*)?
                 ')' )?                                          # exprEnumCtor
  | '[' literal '.' '.' '.' type? ']'                            # exprArrayInit
  | expr LineEnd* '?'? '.' ID ('@' ID)?                          # exprDot  ; `@ID` = spec 默认体消歧后缀（§12.10.8）
  | expr LineEnd* DOT_NUM                                        # exprTupleMember
  | '(' ')'                                                       # exprUnit
  | '(' expr (',' expr)+ ')'                                     # exprTuple
  | typeName '{' LineEnd ( fieldInit | LineEnd )* '}'            # exprStructLit
  | '[' LineEnd* (expr (',' LineEnd* expr)* ','? LineEnd*)? ']'  # exprArray
  | expr (':' genericDef)? '(' LineEnd*
        (expr (',' LineEnd* expr)* ','? LineEnd*)? ')'
        trailingLambda?
        '!'?                                                     # exprCall  ; 末尾 `!` = 错误传播（DRAFT-错误.md §4.2）
  | expr (':' genericDef)? trailingLambda
        '!'?                                                     # exprCallTrailingOnly  ; 末尾 `!` 同 `exprCall`
  | '(' lambdaParams? ')' (retType=typeWithRef)? '=>'
        (statementBlock | lambdaBody)                            # exprLambdaParen
  | ('-' | '~' | '!') expr                                       # exprUnary
  | expr opShift expr                                            # exprShift
  | expr ('&' | '|' | '^') expr                                  # exprBinOp
  | expr ('*' | '/' | '%') expr                                  # exprMulDivMod
  | expr ('+' | '-') expr                                        # exprAddSub
  | expr opCompare expr                                          # exprCompare
  | expr opEq expr                                               # exprEq
  | expr opBool expr                                             # exprBool
  | literal                                                      # exprLiteral
  | expr '?' '?' expr                                            # exprNullElse
  | expr '<' '-' expr                                            # exprMoveAssign
  | '$'                                                          # exprThis

exprElIf       ::= 'elif' expr statementBlock
exprElse       ::= 'else' statementBlock

catchArm       ::= 'catch' ID type statementBlock              ; DRAFT-错误.md §5.1；type 必须是已声明 enum（语义层校验）

opShift        ::= '<' '<' | '>' '>'
opCompare      ::= '>' | '>' '=' | '<' | '<' '='
opEq           ::= '==' | '!='
opBool         ::= '||' | '&&'

opAssign       ::= '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '^=' | '|=' | '&='
moveAssign     ::= '<-'                                          ; 表达式级移入赋值（§4.13），不在 opAssign 中
                 | '>' '>' '=' | '<' '<' '='

matchArm       ::= enumPattern '=>' (statementBlock | expr)

lambdaBody     ::= expr                                          ; 非左递归包装：迫使内部 expr 以新优先级启动
lambdaParams   ::= lambdaParam (',' LineEnd* lambdaParam)* ','? LineEnd*
lambdaParam    ::= (ID ',' LineEnd*)+ ID typeWithRef?            # lambdaParamGroup
                 | ID typeWithRef?                               # lambdaParamStd

trailingLambda ::= '{' LineEnd*
                       '(' lambdaParams? ')' (retType=typeWithRef)? '=>'
                       LineEnd*
                       (statement | comment | codeLineEnd)*
                   '}'                                           ; 仅挂在调用上（§4.8.4）
enumPattern    ::= ID '::' ID ( '(' ID (',' ID)* ')' )?          # patternEnum
                 | 'else'                                        # patternElse
```

`exprMatch` / `exprEnumCtor` / `matchArm` / `enumPattern` 见 §3.10 与草案 [draft/DRAFT-枚举.md](draft/DRAFT-枚举.md) §4 / §5。

`exprTryCatch` / `catchArm`、以及 `exprCall` / `exprCallTrailingOnly` 末尾的 `'!'?` 槽（错误传播）见草案 [draft/DRAFT-错误.md](draft/DRAFT-错误.md) §4 / §5；语义层约束（穷尽性 / 类型一致性 / 跨类型 E7004 / 冗余 E7016）由编译器分析。

- 后缀 `!` **仅**附着在 `exprCall` / `exprCallTrailingOnly` 末尾（产生式内嵌槽 `errPropagate=SymbolExcl?`），不构成独立产生式；非调用位置出现的 `!` 由 `exprUnary` 解析为布尔取反，不进入错误传播路径。
- `f(a){ (x) => body }!` 与 `f { () => body }!` 合法（trailing lambda 与 `!` 槽并存于产生式末尾），详见 DRAFT-错误.md §4.4。
- `!` 与 `=` / `==` 之间需空白或换行（避免被吞为 `SymbolExclEq`）。

- `exprEnumCtor`：`E::V` 与 `E::V()` 等价；类型别名 `C = E` 后 `C::V` 在解析期归一为 `E::V`。
- `exprMatch`：arm 体为 `=> expr` 或 `=> { stmts }`（块可单行，值规则同 §5.4.4）；arm 顺序对穷尽语义无影响，仅 `else` **应当**为最后一条；穷尽性 / binding arity / 重复 variant 由语义层校验。
- `enumPattern` 的 binding 位仅接受 ID（不可变值绑定）；不支持 `_` 通配、字面量、嵌套、多模式合并 `|`、守卫 `if`、`@` 绑定（§3.10 / 草案 §5.3）。

> 优先级与结合性由 ANTLR4 在 `expr` 中按分支出现顺序自上而下决定。规范层语义见 §4。

## B.7 语句

```
statement ::=
    letAnno* 'let' ID typeWithRef? ('=' expr)? codeLineEnd?               # statementLet
  | letAnno* 'let' '(' ID (',' ID)+ ')' typeWithRef? '=' expr codeLineEnd? # statementLetTuple
  | expr '[' expr (',' expr)* ']' '=' expr codeLineEnd?                   # statementSet
  | ID '::' ID '=' expr codeLineEnd?                                      # statementStaticFieldSet
  | (ID ':')? 'loop' loopInit? statementBlock codeLineEnd?                # statementLoop
  | (ID | '$') ('.' ID | DOT_NUM)* opAssign expr codeLineEnd?   # statementAssign
  | expr ';'? codeLineEnd?                                      # statementExpr
  | 'ret' expr codeLineEnd?                                     # statementRet
  | 'ret' ';' codeLineEnd?                                      # statementRetVoid
  | 'break' ('@' ID)? ';' codeLineEnd?                          # statementBreak

statementBlock ::= '{' LineEnd*
                       (statement | comment | codeLineEnd)*
                   '}'
                   ; `{` 后 / 语句后 / `}` 前换行均可省（§2.3.2.3）

letAnno        ::= '#' ID codeLineEnd?   ; #Mut / #Cval / #Frozen（let 声明专用，无单参槽）
```

> `statementLet` / `statementLetTuple` 由 let-unify 统一局部声明形态（注解严格 inline）；旧 `var` / `val` / `cval` 关键字已从 lexer 移除（附录 A §A.1.1），`#Mut let x T`（无 init）保留旧 `var x T` 的延后赋值语义（§5.1.1.3）。顶层 `globalConst` RHS 从 `literal` 升为 `expr`（常量表达式，const-eval 落地，§5.1.4.1.3）。

## B.8 词法 token（节录）

完整定义见 `yux.g4` 末尾。下表只列规范常引用的 token：

| token | 形态 |
|---|---|
| `ID` | 见 §1.4，Unicode 标识符 |
| `INT` | `NUN_SIGN? (INT_10\|INT_2\|INT_8\|INT_16) INT_SUFFIX?` |
| `FLOAT` | `NUN_SIGN? (INT_10\|FLOAT_DOT\|FLOAT_EXP) FLOAT_SUFFIX?` |
| `DOT_NUM` | `'.' INT_10` —— 元组成员后缀 token；优先于 `FLOAT_DOT`，使 `t.0.0` 不被切成浮点 |
| `STR_LINE` | `"..."`，支持 `\<char>` 转义 |
| `STR_LINE_RAW` | `r"..."`，无转义 |
| `CODE_POINT` | `c'<char>'`，类型 `u32` |
| `LineComment` | 行首列起的 `;...` 注释，含 `LineEnd` |
| `LineEndComment` | 代码后空格起的 `;...` 注释，不含 `LineEnd` |
| `LineEnd` | `\r\n` / `\n` / `\r` / `EOF` |
| `Space` | `' '`，HIDDEN 通道 |
| `EmptyLine` | 行首列的纯空白行，HIDDEN 通道 |

## B.9 与 `yux.g4` 不一致时

- §B 中条目过时**应当**修订本附录，**不得**反向修改 `yux.g4`（参见 `RULES.md` 的规则）。
- 出现产生式新增 / 重命名时，请在 §B 对应小节追加，并同步 §2.5 的总览列表。

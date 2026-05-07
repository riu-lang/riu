# 附录 B：语法汇总

> 权威来源：[`src/yux.g4`](../../src/yux.g4)。本附录是规范化摘录（简化记号见 §2.1.1），不得与 `.g4` 冲突；当 `.g4` 与本附录不一致时**应当**修订本附录。

## B.1 顶层

```
program        ::= comment*
                   imports*
                   ( fn
                   | externDecl
                   | globalConst
                   | aliasDecl
                   | enumDecl
                   | draftDecl
                   | structDecl
                   | structImpl
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

globalConst    ::= buildAnno* 'cval' ID type '=' literal

aliasDecl      ::= ID genericDef? '=' type codeLineEnd

buildAnno      ::= '#' ID codeLineEnd
```

## B.2 类型

```
type           ::= ID                              # typeNormal
                 | type '?'                        # typeNullable
                 | ID genericDef                   # typeGeneric
                 | '[' type '*' INT ']'            # typeArray
                 | '(' type (',' type)+ ')'        # typeTuple

typeWithRef    ::= ID '&'?                         # typeNormalWithRef
                 | type '?' '&'?                   # typeNullableWithRef
                 | ID genericDefWithRef '&'?       # typeGenericWithRef
                 | '[' typeWithRef '*' INT ']' '&'?# typeArrayWithRef
                 | '(' typeWithRef (',' typeWithRef)+ ')' # typeTupleWithRef

genericDef        ::= '<' typeParam        (',' typeParam)*        '>'
genericDefWithRef ::= '<' typeParamWithRef (',' typeParamWithRef)* '>'

typeParam         ::= type        (':' draftBound ('+' draftBound)*)?
typeParamWithRef  ::= typeWithRef (':' draftBound ('+' draftBound)*)?
draftBound        ::= modulePath? ID genericDef?     # 例：ToString / pkg.Display / To<i32>
```

约束：

- `typeWithRef` 仅出现在函数参数与局部变量声明位置（§3.2 / §8.3.1）；其它位置只能用 `type`。
- `typeParam` 的 draft 边界仅出现在**声明位**（`fn` / `struct` / `draft` 头部的 `genericDef` 槽位）；调用点 turbofish `f:<T>(args)` 处**不得**写边界（§6.4.4.3）。

> 上述边界产生式为 §12 引入的形态（v0.5+）；待与用户确认后回写 `src/yux.g4`，按 CLAUDE.md 项目约束。本附录文本与 `.g4` 暂不同步时，以草案 `draft/DRAFT-draft.md` §10.3 为准。

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

> `literalObj` 让 `cval NAME T = X` 中的 `X` 可以是另一个 `cval` 名（编译期常量传播）。

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
fnParamStd     ::= ID typeWithRef
fnParamGroup   ::= (ID ',')* ID typeWithRef

fnBody         ::= fnExprkBody | fnBlockBody
fnExprkBody    ::= LineEnd? '=' expr codeLineEnd?
fnBlockBody   ::= statementBlock
```

## B.5 结构体

```
structDecl     ::= buildAnno*
                   'struct' ID ('<' type (',' type)* '>')? '{'
                       ( filedDecl codeLineEnd? | comment | codeLineEnd )*
                   '}'

structImpl     ::= buildAnno* ID ('<' type (',' type)* '>')?
                       (':' draftBound ('+' draftBound)*)? '{'
                       codeLineEnd
                       fnClean?
                       ( fn | comment | codeLineEnd )*
                   '}'

filedDecl      ::= ID type
```

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

## B.5a draft（v0.5+）

```
draftDecl      ::= buildAnno* 'draft' genericDef? ID '{'
                       ( fnSig | comment | codeLineEnd )*
                   '}'

fnSig          ::= buildAnno* 'fn' ID '(' fnParams? ')' (retType=type)?
```

- draft 体内**只允许签名**（`fnSig`），不得带函数体（§12.1.1.1）。
- 签名集**允许为空**（§12.1.1.2 / §12.7.2 内置 `Any`）。
- draft 自身可携带 `genericDef`，但 draft 体内单个 `fn` **不得**再引入泛型形参（§12.3.2）。

> 同 §B.2 末尾说明：`draftDecl` 为 §12 引入的形态，回写 `src/yux.g4` 前需用户确认。

## B.6 表达式（按 `yux.g4` 中 `expr` 的分支顺序，决定优先级）

```
expr ::=
    '(' expr ')'                                                 # exprParen
  | '&' (ID | '$') (LineEnd* '.' ID)*                            # exprGetRef
  | expr '[' LineEnd* expr (',' LineEnd* expr)* ','? LineEnd* ']' # exprGet
  | 'if' expr '{' expr '}' 'else' '{' expr '}'                   # exprOneLineIfElse
  | expr 'if' expr 'else' expr                                   # exprIfElsePreValue
  | 'if' expr statementBlock exprElIf* exprElse?                 # exprIfElse
  | 'match' expr '{' codeLineEnd
        ((matchArm codeLineEnd) | comment)+
    '}'                                                          # exprMatch
  | ID '::' ID ( '(' codeLineEnd*
                     (expr (',' LineEnd* expr)* ','? codeLineEnd*)?
                 ')' )?                                          # exprEnumCtor
  | '[' literal '.' '.' '.' type? ']'                            # exprArrayInit
  | expr LineEnd* '?'? '.' ID                                    # exprDot
  | expr LineEnd* DOT_NUM                                        # exprTupleMember
  | '(' expr (',' expr)+ ')'                                     # exprTuple
  | '[' LineEnd* (expr (',' LineEnd* expr)* ','? LineEnd*)? ']'  # exprArray
  | expr (':' genericDef)? '(' LineEnd*
        (expr (',' LineEnd* expr)* ','? LineEnd*)? ')'           # exprCall
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
  | '$'                                                          # exprThis

exprElIf       ::= 'elif' expr statementBlock
exprElse       ::= 'else' statementBlock

opShift        ::= '<' '<' | '>' '>'
opCompare      ::= '>' | '>' '=' | '<' | '<' '='
opEq           ::= '==' | '!='
opBool         ::= '||' | '&&'

opAssign       ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                 | '>' '>' '=' | '<' '<' '='

matchArm       ::= enumPattern '=>' expr
enumPattern    ::= ID '::' ID ( '(' ID (',' ID)* ')' )?          # patternEnum
                 | 'else'                                        # patternElse
```

`exprMatch` / `exprEnumCtor` / `matchArm` / `enumPattern` 见 §3.10 与草案 [draft/DRAFT-枚举.md](draft/DRAFT-枚举.md) §4 / §5。

- `exprEnumCtor`：`E::V` 与 `E::V()` 等价；类型别名 `C = E` 后 `C::V` 在解析期归一为 `E::V`。
- `exprMatch`：v1 arm 体仅单表达式（多语句体押后）；arm 顺序对穷尽语义无影响，仅 `else` **应当**为最后一条；穷尽性 / binding arity / 重复 variant 由语义层校验。
- `enumPattern` 的 binding 位仅接受 ID（不可变值绑定）；不支持 `_` 通配、字面量、嵌套、多模式合并 `|`、守卫 `if`、`@` 绑定（§3.10 / 草案 §5.3）。

> 优先级与结合性由 ANTLR4 在 `expr` 中按分支出现顺序自上而下决定。规范层语义见 §4。

## B.7 语句

```
statement ::=
    'va'[rl] ID type codeLineEnd                                # statementDeclare
  | 'va'[rl] ID typeWithRef? '=' expr codeLineEnd               # statementDeclareAssign
  | 'va'[rl] '(' ID (',' ID)+ ')' typeWithRef? '=' expr codeLineEnd # statementDeclareAssignTuple
  | expr '[' expr (',' expr)* ']' '=' expr                      # statementSet
  | 'loop' statementBlock                                       # statementLoop
  | (ID | '$') ('.' ID | DOT_NUM)* opAssign expr codeLineEnd    # statementAssign
  | expr ';'? codeLineEnd                                       # statementExpr
  | 'ret' expr codeLineEnd                                      # statementRet
  | 'ret' ';' LineEnd                                           # statementRetVoid
  | 'break' ';' codeLineEnd                                     # statementBreak

statementBlock ::= '{' codeLineEnd
                       (statement | comment | codeLineEnd)*
                   '}'
```

> `'va'[rl]` 是 `DeclKey` 中 `var` / `val` 两支；`globalConst` 单独使用 `'cval'`。

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

- §B 中条目过时**应当**修订本附录，**不得**反向修改 `yux.g4`（参见 `CLAUDE.md` 的规则）。
- 出现产生式新增 / 重命名时，请在 §B 对应小节追加，并同步 §2.5 的总览列表。

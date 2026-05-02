# 附录 B：语法汇总

> 权威来源：[`src/yux.g4`](../../src/yux.g4)。本附录是规范化摘录（简化记号见 §2.1.1），不得与 `.g4` 冲突；当 `.g4` 与本附录不一致时**应当**修订本附录。

## B.1 顶层

```
program        ::= comment*
                   imports*
                   ( fn
                   | externDecl
                   | globalConst
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

buildAnno      ::= '#' ID codeLineEnd
```

## B.2 类型

```
type           ::= ID                              # typeNormal
                 | type '?'                        # typeNullable
                 | ID genericDef                   # typeGeneric
                 | '[' type '*' INT ']'            # typeArray

typeWithRef    ::= ID '&'?                         # typeNormalWithRef
                 | type '?' '&'?                   # typeNullableWithRef
                 | ID genericDefWithRef '&'?       # typeGenericWithRef
                 | '[' typeWithRef '*' INT ']' '&'?# typeArrayWithRef

genericDef        ::= '<' type        (',' type)*        '>'
genericDefWithRef ::= '<' typeWithRef (',' typeWithRef)* '>'
```

约束：`typeWithRef` 仅出现在函数参数与局部变量声明位置（§3.2 / §8.3.1）；其它位置只能用 `type`。

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
                   'fn' genericDef? ID '('
                       fnParams?
                   ')' (retType=type)?

fnParams       ::= fnParam (',' fnParam)*
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

structImpl     ::= buildAnno* ID ('<' type (',' type)* '>')? '{'
                       codeLineEnd
                       fnClean?
                       ( fn | comment | codeLineEnd )*
                   '}'

filedDecl      ::= ID type
```

## B.6 表达式（按 `yux.g4` 中 `expr` 的分支顺序，决定优先级）

```
expr ::=
    '(' expr ')'                                                 # exprParen
  | '&' (ID | '$') ('.' ID)*                                     # exprGetRef
  | expr '[' expr (',' expr)* ']'                                # exprGet
  | 'if' expr '{' expr '}' 'else' '{' expr '}'                   # exprOneLineIfElse
  | expr 'if' expr 'else' expr                                   # exprIfElsePreValue
  | 'if' expr statementBlock exprElIf* exprElse?                 # exprIfElse
  | '[' literal '.' '.' '.' type? ']'                            # exprArrayInit
  | expr '?'? '.' ID                                             # exprDot
  | '[' (expr (',' expr)*)? ']'                                  # exprArray
  | expr (':' genericDef)? '(' (expr (',' expr)*)? ')'           # exprCall
  | ('-' | '~' | '!') expr                                       # exprUnary
  | expr ('&' | '|' | '^') expr                                  # exprBinOp
  | expr opShift expr                                            # exprShift
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
```

> 优先级与结合性由 ANTLR4 在 `expr` 中按分支出现顺序自上而下决定。规范层语义见 §4。

## B.7 语句

```
statement ::=
    'va'[rl] ID type codeLineEnd                                # statementDeclare
  | 'va'[rl] ID typeWithRef? '=' expr codeLineEnd               # statementDeclareAssign
  | expr '[' expr (',' expr)* ']' '=' expr                      # statementSet
  | 'loop' statementBlock                                       # statementLoop
  | (ID | '$') ('.' ID)* opAssign expr codeLineEnd              # statementAssign
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

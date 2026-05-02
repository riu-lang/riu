# 附录 A：保留字

> 与 [`src/yux.g4`](../../src/yux.g4) 的 lexer token 一致；冲突以 `.g4` 为准。本附录是规范化摘录。

## A.1 关键字

下表列出 `yux.g4` 中作为独立 lexer token 声明的关键字（按字母序）：

| 关键字 | g4 token | 说明 | 章节 |
|---|---|---|---|
| `break` | `Break` | 跳出 `loop` | §5.5 |
| `cval` | `DeclKey` 一支 | 全局常量声明 | §5.1 |
| `elif` | `Elif` | 多分支条件 | §4.9 / §5.4 |
| `else` | `Else` | 条件分支兜底 | §4.9 / §5.4 |
| `extern` | `Extern` | 外部声明块 | §6.6 |
| `false` | `False` | 布尔字面量 | §1.6.4 |
| `fn` | `Fn` | 函数声明 | §6 |
| `if` | `If` | 条件表达式 / 语句 | §4.9 / §5.4 |
| `loop` | `Loop` | 循环 | §5.5 |
| `null` | `Null` | 空字面量 | §3.6.1.3 / §1.6.7 |
| `ret` | `Ret` | 返回语句 | §5.6 |
| `struct` | `Struct` | 结构体声明 | §7.1 |
| `true` | `True` | 布尔字面量 | §1.6.4 |
| `use` | `Use` | 模块导入 | §10.2 |
| `val` | `DeclKey` 一支 | 不可变局部声明 | §5.1 |
| `var` | `DeclKey` 一支 | 可变局部声明 | §5.1 |

§A.1.1 `cval` / `val` / `var` 在 lexer 层共用一个 `DeclKey` token（产生式 `'va'[rl] | 'cval'`），在 parser 层按字面值分支。

§A.1.2 关键字**不得**用作 `ID`（标识符）。`yux.g4` 的 lexer 优先级保证关键字命中先于 `ID`。

§A.1.3 v1 **没有** `continue` 关键字；循环只能通过 `break` 中断（§5.5）。

## A.2 上下文标识符

下列名称在 `yux.g4` 中**不是**关键字（即可作 `ID` 出现），但在特定上下文具有语言层语义；用户**应当**避免在普通命名中复用：

| 名称 | 上下文 | 章节 |
|---|---|---|
| `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` | 内置整数类型名 | §3.2.1 / §9.1.1 |
| `f32` `f64` | 内置浮点类型名 | §3.2.1 / §9.1.2 |
| `bool` | 内置布尔类型名 | §3.2.1 / §9.1.3 |
| `Box` `Weak` `Array` `String` `StringBuilder` `Nullable` `Ref` `Ptr` | 内置泛型 / 堆句柄 / 引用类型 | §3.3 / §9 |
| `Self` | 当前结构体类型（隐式） | §7.2.2 |
| `$` | 方法接收者 / 当前实例 | §7.2.2 / §B.6 `exprThis` |

§A.2.1 上述名称由 `base.yux` 注册；用户重新定义同名顶层符号**应当**编译报错（§10.4.3.2）。

§A.2.2 `$` 是 lexer token `SymbolThis`，不是 `ID`；语法层只能在 `exprThis` / `exprGetRef` 头位 / `statementAssign` 头位出现。

## A.3 注解名

下列名称作为构建注解（§11）使用，受 `buildAnno: '#' ID` 语法约束。注解 ID 与普通 `ID` 共享 lexer 形态，但语义层只接受白名单内的注解名。

| 注解 | 用途 | 章节 |
|---|---|---|
| `#CompilerInner` | 编译器内部合成实现 | §11.2 |

§A.3.1 v1 仅 `#CompilerInner` 一种正式注解；其它注解形态属预留（§11.3.2）。

## A.4 运算符与符号 token

下表是 `yux.g4` 中的符号 lexer token，按字母序列出。规范在 §1.7 给出语义概览，本表只列形态与 token 名。

| 符号 | token |
|---|---|
| `+` | `SymbolAdd` |
| `+=` | `SymbolAddEq` |
| `&` | `SymbolAnd` |
| `&&` | `SymbolAndAnd` |
| `:` | `SymbolColon` |
| `,` | `SymbolComma` |
| `/` | `SymbolDiv` |
| `/=` | `SymbolDivEq` |
| `.` | `SymbolDot` |
| `=` | `SymbolEq` |
| `==` | `SymbolEqEq` |
| `!` | `SymbolExcl` |
| `!=` | `SymbolExclEq` |
| `#` | `SymbolHash` |
| `<` | `SymbolLt` |
| `%` | `SymbolMod` |
| `%=` | `SymbolModEq` |
| `>` | `SymbolMt` |
| `*` | `SymbolMul` |
| `*=` | `SymbolMulEq` |
| `\|` | `SymbolOr` |
| `\|\|` | `SymbolOrOr` |
| `?` | `SymbolQuest` |
| `~` | `SymbolRev` |
| `;` | `SymbolSemicolon` |
| `-` | `SymbolSub` |
| `-=` | `SymbolSubEq` |
| `$` | `SymbolThis` |
| `^` | `SymbolXor` |
| `^=` | `SymbolXorEq` |
| `(` `)` | `ParStart` `ParEnd` |
| `[` `]` | `GetStart` `GetEnd` |
| `{` `}` | `BlockStart` `BlockEnd` |

§A.4.1 多字符运算符（如 `<<` / `>>` / `<=` / `>=` / `<<=` / `>>=`）由两个或多个上表 token 拼装；具体规则见 §2.4 与 `opShift` / `opCompare` / `opAssign`。

§A.4.2 `^=` 在 lexer 层有 token，但 `opAssign` 产生式 v1 **未**包含；按位异或赋值 `^=` v1 不可用。Open Issue：是否补入 `opAssign`。

## A.5 词法 token 摘录

完整定义见 `yux.g4` 末尾。下表只列规范层常引用的 token：

| token | 形态 | 章节 |
|---|---|---|
| `ID` | Unicode 标识符（§1.4） | §1.4 |
| `INT` | `NUN_SIGN? (INT_10 \| INT_2 \| INT_8 \| INT_16) INT_SUFFIX?` | §1.6.1 |
| `FLOAT` | `NUN_SIGN? (INT_10 \| FLOAT_DOT \| FLOAT_EXP) FLOAT_SUFFIX?` | §1.6.2 |
| `STR_LINE` | `"..."`，支持 `\<char>` 转义 | §1.6.5 |
| `STR_LINE_RAW` | `r"..."`，无转义 | §1.6.5 |
| `CODE_POINT` | `c'<char>'`，类型 `u32` | §1.6.5 |
| `LineComment` | 行首列起的 `;...` 注释，含 `LineEnd` | §1.3 |
| `LineEndComment` | 代码后空格起的 `;...` 注释，不含 `LineEnd` | §1.3 |
| `LineEnd` | `\r\n` / `\n` / `\r` / `EOF` | §1.2 |
| `Space` | `' '`，HIDDEN 通道 | §1.2 |
| `EmptyLine` | 行首列的纯空白行，HIDDEN 通道 | §1.2 |

## A.6 预留但未启用

§A.6.1 `extern` 块与 `cval` 的 `buildAnnos` 在语法层已预留，v1 **不**赋予语义（§11.3.2）。

§A.6.2 v1 **没有**以下名称对应的 token / 关键字（如有需要由用户自由用作 `ID`）：

- `pub` / `priv` / `private` / `internal`：可见性修饰符（§10.3.1.2 不引入）；
- `mut` / `const`：可变性修饰符（v1 用 `var` / `val`）；
- `trait` / `impl` / `where`：v1 无 trait bound（§7.1.2.3）；
- `match` / `case`：v1 无模式匹配；
- `async` / `await`：v1 无并发原语；
- `for` / `while` / `do`：v1 循环只用 `loop`（§5.5）；
- `continue`：v1 不提供（§A.1.3）。

§A.6.3 上述名称未来如引入**应当**升格为关键字并同步更新本附录与 `yux.g4`。

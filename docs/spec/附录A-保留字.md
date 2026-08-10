# 附录 A：保留字

> 与 [`yux/ast/yuxLexer.g4`](../../yux/ast/yuxLexer.g4) 的 lexer token 一致；冲突以 `.g4` 为准。本附录是规范化摘录。

## A.1 关键字

下表列出 `yux.g4` 中作为独立 lexer token 声明的关键字（按字母序）：

| 关键字 | g4 token | 说明 | 章节 |
|---|---|---|---|
| `break` | `Break` | 跳出 `loop` | §5.5 |
| `catch` | `Catch` | 错误路由块的捕获子句（草案） | DRAFT-错误.md §5 |
| `elif` | `Elif` | 多分支条件 | §4.9 / §5.4 |
| `else` | `Else` | 条件分支兜底 / `match` 兜底 | §4.9 / §5.4 / §3.10 |
| `enum` | `Enum` | 枚举声明 | §3.10 |
| `extern` | `Extern` | 外部声明块 | §6.6 |
| `false` | `False` | 布尔字面量 | §1.6.4 |
| `fn` | `Fn` | 函数声明 | §6 |
| `if` | `If` | 条件表达式 / 语句 | §4.9 / §5.4 |
| `let` | `Let` | 局部 / 全局变量声明（含 `#Mut` / `#Cval` / `#Frozen` 注解修饰档位） | §5.1 |
| `loop` | `Loop` | 循环 | §5.5 |
| `match` | `Match` | 模式匹配（仅 enum） | §3.10 / §4 |
| `null` | `Null` | 空字面量 | §3.6.1.3 / §1.6.7 |
| `ret` | `Ret` | 返回语句 / 错误抛出（草案） | §5.6 / DRAFT-错误.md §4.1 |
| `Self` | `SelfType` | 当前结构体类型字面量（含 `Self&`、`Self { ... }`、`Self::name(...)`） | §7.10 / §7.2.2 / DRAFT-static-fn.md |
| `struct` | `Struct` | 结构体声明 | §7.1 |
| `true` | `True` | 布尔字面量 | §1.6.4 |
| `try` | `Try` | 错误路由块开始（草案） | DRAFT-错误.md §5 |
| `use` | `Use` | 模块导入 | §10.2 |

§A.1.1 `let` 是局部 / 全局变量声明的唯一引入符（[DRAFT-let-unify](draft/DRAFT-let-unify.md)）；可变性 / 编译期常量 / 深不可变档位由 inline 注解 `#Mut` / `#Cval` / `#Frozen` 修饰，详见 §5.1。旧 `var` / `val` / `cval` 关键字已在 let-unify 落地时从 lexer 移除；字段段保留独立形态（不在 let-unify 范围）。

§A.1.2 关键字**不得**用作 `ID`（标识符）。`yux.g4` 的 lexer 优先级保证关键字命中先于 `ID`。

§A.1.3 v1 **没有** `continue` 关键字；循环只能通过 `break` 中断（§5.5）。

## A.2 上下文标识符

下列名称在 `yux.g4` 中**不是**关键字（即可作 `ID` 出现），但在特定上下文具有语言层语义；用户**应当**避免在普通命名中复用：

| 名称 | 上下文 | 章节 |
|---|---|---|
| `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` | 内置整数类型名 | §3.2.1 / §9.1.1 |
| `f32` `f64` | 内置浮点类型名 | §3.2.1 / §9.1.2 |
| `bool` | 内置布尔类型名 | §3.2.1 / §9.1.3 |
| `Rc` `Weak` `Array` `String` `StringBuilder` `Nullable` `Ref` `Ptr` | 内置泛型 / 堆句柄 / 引用类型 | §3.3 / §9 |
| `$` | 方法接收者 / 当前实例 | §7.2.2 / §B.6 `exprThis` |

§A.2.1 上述名称由 `base.yux` 注册；用户重新定义同名顶层符号**应当**编译报错（§10.4.3.2）。

§A.2.2 `$` 是 lexer token `SymbolThis`，不是 `ID`；语法层只能在 `exprThis` / `exprGetRef` 头位 / `statementAssign` 头位出现。

## A.3 注解名

下列名称作为构建注解（§11）使用，受 `buildAnno: '#' ID` 语法约束。注解 ID 与普通 `ID` 共享 lexer 形态，但语义层只接受白名单内的注解名。

| 注解 | 用途 | 章节 |
|---|---|---|
| `#Builtin` | 编译器内部合成实现 | §11.2 |
| `#Test` | 单元测试函数（仅 `*.test.yux`） | §11.3 |
| `#Spec` | 把 struct 声明转为 spec（仅签名集合） | §11.4 / §12.1 |
| `#Impl(D)` | 宣告 struct 实现 spec `D`（单参数糖） | §11.4 / §12.2 |
| `#Const` | `#Const fn`：编译期常量函数（不改外部状态） | §11.6 |
| `#Frozen` | 参数 / 字段深不可变；局部 `#Frozen let` 同语义 | §11.7 / §5.1.1 |
| `#Val` | 字段浅不可变 | §11.8 |
| `#Mut` | 局部 / 全局 `let` 可变档位修饰；struct 静态字段可变修饰（与 `#Static` 组合） | §11.9 / §5.1.1 / §5.1.4 / §7.11 |
| `#Cval` | `let` 编译期常量档位修饰（局部 / 全局） | §11.10 / §5.1.4 / §5.1.5 |
| `#Static` | struct body 内方法（关联函数）+ 字段（静态字段） | §11.11 / §7.10 / §7.11 |
| `#Fallible(E)` | 失败声明：函数可能以错误 enum `E` 失败（草案，单参数糖） | DRAFT-错误.md §3 |
| `#NoReturn` | 不返回声明：函数永不正常返回（草案，零参数） | DRAFT-错误.md §8.3 |

§A.3.1 v1 正式注解：`#Builtin` / `#Test` / `#Spec` / `#Impl(D)` 已落地；DRAFT-错误.md 引入 `#Fallible(E)` / `#NoReturn`（草案，单参数糖于 §11.1.1.1 同步解禁）；其它注解形态属预留（§11.5.2）。早期 `#DraftLike` 已废弃（§11.4.3）。

## A.4 运算符与符号 token

下表是 `yux.g4` 中的符号 lexer token，按字母序列出。规范在 §1.7 给出语义概览，本表只列形态与 token 名。

| 符号 | token |
|---|---|
| `+` | `SymbolAdd` |
| `+=` | `SymbolAddEq` |
| `&` | `SymbolAnd` |
| `&&` | `SymbolAndAnd` |
| `:` | `SymbolColon` |
| `::` | `SymbolColonColon` |
| `,` | `SymbolComma` |
| `/` | `SymbolDiv` |
| `/=` | `SymbolDivEq` |
| `.` | `SymbolDot` |
| `=` | `SymbolEq` |
| `==` | `SymbolEqEq` |
| `=>` | `SymbolEqMt` |
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

§A.4.1 多字符运算符（如 `<<` / `>>` / `<=` / `>=` / `<<=` / `>>=`）由两个或多个上表 token 拼装；具体规则见 §2.4 与 `opShift` / `opCompare` / `opAssign`。`^=` 为单 token（`SymbolXorEq`），直接由 `opAssign` 包含。

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

§A.6.1 `extern` 块的 `buildAnnos` 在语法层已预留，v1 **不**赋予语义（§11.3.2）。全局 `let`（含 `#Cval` / `#Mut` / `#Frozen` 档位）的注解语义见 §5.1.4。

§A.6.2 v1 **没有**以下名称对应的 token / 关键字（如有需要由用户自由用作 `ID`）：

- `pub` / `priv` / `private` / `internal`：可见性修饰符（§10.3.1.2 不引入）；
- `mut` / `const`：可变性修饰符（v1 用 `let` 默认不可变 + `#Mut` 注解放宽，见 §5.1.1）；
- `var` / `val` / `cval`：旧声明关键字（已被 let-unify 移除，由 `let` + `#Mut` / `#Cval` 取代）；
- `trait` / `impl` / `where`：v1 用 `#Spec struct` + `#Impl(D)` 顶行注解替代，无 `where` 子句（§12 / §6.4.4）；
- `draft`：v0.x 接口契约关键字，spec-unify v1（2026-05-19）删除；由 `#Spec` 注解承载（§12.1）；
- `case`：v1 `match` 用 `=>` + `else` 兜底，无 `case` 关键字（§3.10）；
- `async` / `await`：v1 无并发原语；
- `for` / `while` / `do`：v1 循环只用 `loop`（§5.5）；
- `continue`：v1 不提供（§A.1.3）。

§A.6.3 上述名称未来如引入**应当**升格为关键字并同步更新本附录与 `yux.g4`。

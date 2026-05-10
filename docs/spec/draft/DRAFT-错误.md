# 草案：yux 错误处理

状态：**草案 / 决议已收齐，待固化**。日期：2026-05-09。
作用：把"yux 函数如何声明 / 抛出 / 传播 / 捕获错误，以及不可恢复终止（panic）的语义边界"这一组决策固化为单一规范，作为修改 `docs/spec/03-类型系统.md` / `04-表达式.md` / `06-函数.md` / `11-编译期注解.md` / 附录 A / B / D 与 `CURRENT.md` 实施计划的依据。

> **yux 不引入异常机制**：错误是值返回（无 unwind / SEH personality / DWARF cleanup landingpad）。本草案沿用 `try` / `catch` 关键字（Zig 路线下的业界约定），但其语义是"错误路由块"而非"异常处理器"。"异常"一词在本文中仅在与 Java/C++ 路线对比时出现，不作为 yux 概念。

> 本草案的所有规则均已逐条决议（见末尾"决议日志"，引 `CURRENT-错误-v1.md` 决议编号 [#1]-[#9] + [#R]）。后续若有反复，请在日志里追加修订记录，不要直接覆盖正文。

涉及章节（预估）：§3 类型系统、§4 表达式、§6 函数、§11 编译期注解、§7 FFI（推迟）、附录 A 保留字、附录 B 语法汇总、附录 D 诊断。

---

## 1. 目标

- 错误以**值返回**形式传递；编译器在 LLVM IR 层**不**引入栈展开 / cleanup landingpad / personality function / SEH。
- 失败声明在签名层显式（`#Fallible(E)` 注解）；调用点显式（`!` 后缀 / `try-catch` 块）；禁止"沉默失败"。
- 错误类型 = 普通 enum；不引入 Result 根类型、不引入"错误种类"、不引入子类型化。
- 跨类型错误转换走显式 `try { ... } catch e Type { ... }` 块；不做自动包装、不做"错误→null"折叠。
- 简化关键字 / 符号面：复用 `ret` 抛出错误；新增 `try` / `catch` 两个硬关键字；新增表达式后缀 `!`；不引入 `throw` / `raise` / `err` / `finally` / `defer`。
- panic 走 abort-only 路径，与错误通道完全分离；可被 `#Test` SEH wrapper 接住但**不**被用户 `try-catch` 捕获。

## 2. 核心模型与全景

| 概念 | 写法 | 语义 / 形态 | 备注 |
|---|---|---|---|
| 失败声明 | `#Fallible(E) fn f(...) T { ... }` | 注解，单参数为已声明的具体 enum；运行期等价于"错误通道返回 E，成功通道返回 T" | 不挤占类型语法；返回类型 `T` 仍是成功值类型 |
| 错误类型 | `enum E { V1, V2(T1, T2), ... }` | 普通 enum，复用 §3.10 既有机制 | 与普通 enum 在类型系统层不可区分；区别仅在出现位置 |
| 抛出 | `ret E::V` / `ret E::V(args)` | `ret` 后跟 `E` 类型值即进入错误通道 | 由编译器按表达式类型分流；不引入 `throw` 关键字 |
| 传播（同类型） | `expr!` | 后缀 `!`：callee 错误类型 == 当前 fn 的 `#Fallible` E → 直接透传 | 仅在 `#Fallible` fn 内或 `try` 块内合法 |
| 捕获 / 跨类型转换 | `try { ... } catch e E { ... }` | 块构造，`catch` 子句按 enum 类型路由；`catch e Type` 后置类型与 yux 声明风格一致 | 必须穷尽；至少一个 catch；类型一致性同 if-else / match |
| panic 入口（用户） | `panic(msg String)` | stdlib `#NoReturn` 函数；不进错误通道 | 不可被 `try-catch` 接 |
| 不返回声明 | `#NoReturn fn f(...) { ... }` | 注解；编译器视调用点为流终止（与 `ret` 同档），不参与表达式类型合并 | 与 `#Fallible` 互斥 |
| 进程退出（用户） | `exit(code u32)` | stdlib `#NoReturn` 函数；用户主动终止 | 与 panic 区分：业务终止 vs bug 终止 |
| main 错误退出 | `_exit(1)` + stderr `error: ...` | 仅当 main 标 `#Fallible(E)` 且未处理错误时 | stderr 文本无 ANSI；payload 走 §12.7.1 ToString |

要点（后续节展开）：

- **不引入** `throw` / `raise` / `err` / `try?` / `try!` / `Result<T,E>` / `From<E>` / `errdefer` / `defer` / `finally` / `!!`（force-unwrap）。
- **不引入** unwind / 栈展开 / DWARF personality；panic 实施层可复用 §11.3.5.3 的 SEH RaiseException 机制（普通 build 无 handler → 进程终止；`#Test` 模式 SEH wrapper 接住，不构成用户可见 unwind）。
- 析构 / 构造**不**参与错误传播：析构出错 → panic；构造失败 → 改写为返回错误 enum 的工厂函数（普通方法 / `init` 不允许 `#Fallible`）。
- 每个函数最多一个 `#Fallible(E)`；多类错误聚合用嵌套 enum + try-catch 显式 wrap。

## 3. 失败声明：`#Fallible(E)` 注解

### 3.1 形态

```yux
enum ParseErr { Empty, Invalid(String) }

#Fallible(ParseErr)
fn parse_int(s String) i32 { ... }
```

`#Fallible(E)` 是注解，**不**改函数返回类型语法 —— `T` 仍是成功值类型。错误通道独立由 `#Fallible(E)` 承载。

### 3.2 出现位置（白名单）

| 位置 | v1 行为 |
|---|---|
| 顶层 `fn` | ✅ |
| `structImpl` 内方法 | ✅ |
| `draft` 内 `fn` 签名 | ✅（实现侧 `#Fallible` 必须与声明一致；§12.3.1 签名一致性） |
| `extern` 块内 `fnHeader` | ❌（推 §7 FFI 边界，整体推迟；见 §7） |
| `#CompilerInner` 函数 | ❌ 互斥 |
| `#Test` 函数 | ❌ 互斥（§11.3.2 / §11.3.5.3） |

### 3.3 参数约束

`#Fallible` 严格一个参数，必须是已声明 enum 类型：

| 形态 | 行为 |
|---|---|
| `#Fallible(E)` 且 E 已声明 enum | ✅ |
| `#Fallible()` / 缺参数 | ❌ |
| `#Fallible(E1, E2)` | ❌（多错误禁止；用嵌套 enum 表达） |
| `#Fallible(NonEnumType)` | ❌（结构体 / 标量都不行） |
| 同函数多次 `#Fallible(...)` | ❌（重复声明） |
| `#Fallible(GenericParam)` | ❌（v1 错误类型必须是具体 enum） |

### 3.4 与函数其余部分的关系

- 不写 `T!E`、不写 `T throws E`、不写 `Result<T,E>` 类型形态。错误通道**不挤占**类型语法。
- 普通函数 = "无错误"。**不**引入 `#Fallible()` 形态（无错误就不写注解）。
- 声明 `#Fallible(E)` 但函数体内未实际产生错误：**允许**（接口先行 / 未来兼容），不报错。

### 3.5 注解参数语法解禁（§11 同步）

本草案附带解禁 §11.1.1.1 的**单参数注解糖**：

```
buildAnno ::= '#' ID ( '(' ID ')' )? codeLineEnd
```

仅解禁单 ID 参数；不解禁多参数、不解禁字面量 / 表达式参数。多参数 / 字面量留 §11.5.2.4 后续课题（O14）。

`#Fallible` / `#NoReturn` 的引入路径仍走"编译器内置名集合"（与 `#CompilerInner` / `#Test` 同档）；用户自定义注解（§11.6）的引入路径不变。

## 4. 抛出与传播：`ret E::V` + 后缀 `!`

### 4.1 抛出 = `ret ErrEnumValue`

函数体内通过 `ret`（yux 关键字，§5）后跟错误 enum 值进入错误通道：

```yux
#Fallible(ParseErr)
fn parse_int(s String) i32 {
  if s.is_empty() { ret ParseErr::Empty }              ; 类型 ParseErr → 错误通道
  if !valid(s)    { ret ParseErr::Invalid(s) }
  ret atoi(s)                                          ; 类型 i32 → 成功通道
}
```

**类型分流前置约束**：函数成功值类型 `T` 与 `#Fallible(E)` 的 `E` **不得**相等（否则 `ret e` 歧义无法分流）。编译期检查 → E7008。该约束对泛型函数在**单态化阶段**重跑：若实例化时 `T` 解为 `E`，触发 E7008 并附实例化栈。

### 4.2 传播 = 表达式后缀 `!`

| 出现位置 | 行为 |
|---|---|
| **try block 内 + 裸调用 `#Fallible` 函数（无 `!`）** | ✅ callee 错误自动路由到本 try 的 catch 子句；try 块边界本身即"调用点显式"标记 |
| **try block 内 + 写了 `!`** | ⚠ 行为与裸调用等价（编译器视为同义），但 `!` 在 try 域内冗余 → 警告 E7016，fix = 移除 `!` |
| try block 外 + callee 错误类型 == 当前 fn `#Fallible(E)` 的 E | ✅ `!` 直接透传到错误通道，零包装 |
| try block 外 + callee 错误类型 ≠ 当前 fn `#Fallible(E)` 的 E | ❌ E7004；用户必须用 try-catch 块显式转换 |
| try block 外 + 无 `#Fallible(E)` 的函数内 + 写了 `!` | ❌ E7001 |
| try block 外 + 调用 `#Fallible` 函数但未加 `!` | ❌ E7006 |

### 4.3 心智模型：`!` 与 `try-catch` 是同一机制的两种形态

`!` 本身就是"如果错误就 catch + 同类型 rethrow"的糖。`try { } catch ... { }` 是显式 catch 域。所以：

- **try 域内的可失败调用** —— 错误的"出口"已被 catch 子句承接，调用本身**不需要** `!` 重复声明。`!` 写出来语义上等价于"嵌套一层 try-catch 自抛自捕"，是冗余表达。
- **try 域外的可失败调用** —— 错误的"出口"是外层 `#Fallible(E)` 通道；必须用 `!` 把调用点标显式。

### 4.4 词法 / 优先级 / trailing lambda 协同

§4.4.1 **`!` 是 `exprCall` 的内嵌后缀槽**（产生式末尾 `errPropagate=SymbolExcl?`），不是独立 `expr '!'` 产生式。语法层意味着：

- `!` **只能**附在调用末尾；非调用位置写的 `!` 由 `exprUnary` 解析为布尔取反（`!cond`），不进入错误传播路径。`(a + b)!` / `arr[i]!` / `obj.field!` 等形态语法层就拒绝（不构成 `exprCall`，前置 `!` 也会被解析为 `exprUnary`）。
- 优先级与 `exprCall` / `[]` 同档（最高一档）；高于一切二元 / 三元 / `??` coalesce。例：`call() ?? -1` 是纯调用 + coalesce；`call()! ?? -1` 是错误透传后 nullable 与 -1 折叠。
- 与 `=` / `==` 之间**强制空白或换行**，否则被贪婪匹配为 `SymbolExclEq`。`x!=0` 永远是"不等于"；写"传播后比较"用 `x! == 0` 或先绑变量。

§4.4.2 **trailing lambda 协同**：`exprCall` 与 `exprCallTrailingOnly` 两条产生式都带 `errPropagate=SymbolExcl?` 槽，因此 trailing lambda 形态与 `!` 可同时出现：

```yux
#Fallible(IOErr)
fn read_each(path String, f fn(line String)) {
    ; 假设 read_lines: #Fallible(IOErr) fn(path String) Array<String>
    val lines = read_lines(path)!                ; 标准调用 + !
    each(lines) { line =>                        ; 尾随 lambda
        f(line)
    }
}

; 调用方
fn use(path String, sink fn(s String)) {
    read_each(path, sink)!                       ; 标准调用 + !
    process(...) { x => x.trim() }!              ; trailing-lambda 形态 + !
    process { x => x.trim() }!                   ; trailing-only 形态 + !
}
```

§4.4.3 **lambda body 内的 `#Fallible` 调用**：v1 的 `fn(T)R` 函数类型（§3.11）**不**承载 `#Fallible`（与 extern fn 同档推迟，§7）。lambda 字面量隐式视为不可失败函数；其 body 内部对 `#Fallible(E)` 函数的调用**应当**由 lambda 内**自带 `try-catch`** 处理，**不**得用 `!` 把失败传播到 lambda 外层（外层已是 fn 值，无 `#Fallible(E)` 通道）：

```yux
each(lines) { line =>
    parse(line)!                  ; ❌ E7001：lambda body 不在任何 #Fallible 函数体内
}

each(lines) { line =>
    try { parse(line) }
    catch e ParseErr { log(e) }   ; ✅ 在 lambda 内收口
}
```

外层 `each(lines) { ... }!` 中的 `!` 仅传播 `each` 自身的失败（若 `each` 是 `#Fallible`），与 lambda body 内部的失败无关。

§4.4.4 **try block 内的 trailing lambda**：try 域只覆盖 try block **直接**包含的可失败调用，**不**穿透 lambda 边界。试图通过尾随 lambda 把可失败调用"塞"进 try：

```yux
try {
    each(lines) { line => parse(line) }   ; lambda body 内 parse 的失败 ≠ try 接管
}
catch e ParseErr { ... }                  ; ❌ 不会捕获 parse 的错误（lambda 边界阻断）
```

`each` 自身若是 `#Fallible(IOErr)`，则 IOErr 走 try 域；ParseErr 仍由 lambda 内自处理。规则：**try 域穷尽性按当前块内可失败调用的 callee 错误类型集合计算，不下钻 lambda body**。这与 §4.4.3 的"fn 值不承载 #Fallible"一致。

### 4.5 显式不做

- ❌ Rust 风格 `From<E>` 协议驱动错误传播。
- ❌ 自动深嵌套穿透 / 自动 variant 包装。
- ❌ 子类型化 / 协变错误类型（必须严格类型相等）。
- ❌ `?` 后缀（属 nullable 家族；详见 §4.6）。
- ❌ force-unwrap `!!`（不存在；nullable 取值仍走 `Nullable<T>.get()` → panic 路径）。
- ❌ `throw` / `raise` / `err` 关键字；零新关键字（除 `try` / `catch`）。

### 4.6 显式不做"错误 → null"折叠

v1 **不**提供"出错则返回 `null`"的传播形态。结构性证明：

```yux
#Fallible(IoErr)
fn read_optional_int() i32? { ... }    ; 三种结局：Some(n) / None / IoErr::*

; 假设我们提供了 call()? 把错误折成 null：
var v = read_optional_int()? ?? -1     ; v == -1 同时对应：
                                       ;   1) callee 成功 None（业务"未找到"，正常）
                                       ;   2) callee 错误 IoErr::NotFound（文件不存在）
                                       ;   3) callee 错误 IoErr::Denied（权限拒绝）
                                       ; 调试时不可分辨
```

这等价于 catch-all-ignore，与"禁止沉默失败 / 不可 catch-all"直接冲突。**v1 不开放、v2 不预承诺**。

`T?` × `#Fallible(E)` 组合本身**允许**：成功通道返回 `T?`、错误通道返回 `E`；三种结局通过通道分流可分辨，不与上述折叠规则冲突。`call()!?.method()` 形态合法（`!` 走错误通道，`?.` 在 nullable 上做安全导航）。

## 5. try-catch 块：跨类型错误形态

### 5.1 形态

```
tryExpr ::= 'try' '{' stmt* '}' ( catchClause )+
catchClause ::= 'catch' ID typeRef '{' stmt* '}'
```

`catch <绑定名> <错误 enum 类型> { ... }` —— 错误类型**后置**，与 yux `var x Type` / `fn f(x Type)` 声明风格一致。

不允许 `try { } finally { }`、不允许 `try { } catch e { }` 缺类型、不允许 `try { } catch IoErr { }` 缺绑定、不允许"裸 expr catch"。

### 5.2 示例

```yux
#Fallible(AppErr)
fn run() i32 {
  ; try block 内**裸调**可失败函数；错误按类型自动路由到对应 catch 子句
  var n = try {
    var s = read_file("a.txt")     ; 错误 IoErr → 自动路由到 catch e IoErr
    parse_int(s)                    ; 错误 ParseErr → 自动路由到 catch e ParseErr
    parse_int(s)                    ; 表达式值即整个 try block 的值（最后一句作为表达式）
  } catch e IoErr {
    ret AppErr::IoFailed             ; e 是绑定名；arm body 用 ret 走外层 fn 错误通道
  } catch e ParseErr {
    match e {
      ParseErr::Empty       => ret AppErr::ParseEmpty,
      ParseErr::Invalid(s)  => ret AppErr::ParseBad(s),
    }
  }
  ret n
}

; 反例（写法等价但触发警告）：
;   var s = read_file("a.txt")!    ; ⚠ E7016：try 域内 `!` 冗余；fix = 去掉 `!`
```

### 5.3 核心规则

| 规则 | 说明 |
|---|---|
| 关键字 | `try` / `catch` 升 v1 硬关键字；附录 A 增补两条；与 `else` / `match` 同级 |
| try block 体 | 普通 block；可含任意 stmt + 表达式；末尾表达式作为 try block 的值 |
| try block 内可失败调用 | 裸调用 `#Fallible` 函数即可，错误自动路由到匹配 catch；写 `!` 触发 E7016 |
| catch 头部 | `catch <ID> <Type>`：绑定名 + 错误 enum 类型（后置）；类型必须是已声明的具体 enum；多个 catch 子句的类型两两不同；绑定名作用域仅 catch body 内部 |
| catch body 形态 | 普通 block；可对绑定变量 `e` 进行任意操作，含 `match e { ... }` 分流 |
| catch body 终结行为 | 必须以以下之一结尾：(1) `ret X` 早返外层 fn；(2) `panic` 类不可恢复终止；(3) 表达式值（类型必须等于 try block 的值类型） |
| 穷尽性 | try block 内**所有对 `#Fallible(E)` 函数的调用**的 callee 错误类型集合，必须被 catch 子句声明的类型集合严格覆盖；不穷尽 → E7002。**不下钻 lambda body**：传给函数的 trailing / 标准 lambda 字面量内部的 `#Fallible` 调用不计入此集合（fn 值不承载 `#Fallible`，§4.4.3 / §4.4.4） |
| catch 子句多余 | catch 子句类型不在 try block 错误集合内 → E7015（不阻止编译） |
| 类型一致性 | try block 末尾表达式值类型 + 所有 catch arm 表达式值类型严格一致；不一致 → 编译错（与 if-else / match 同档） |
| 必须有 catch | `var x = try { ... }` 不带 catch → E7009；try 至少跟一个 catch |
| 嵌套合法 | try 内允许嵌套 try-catch；外层 try 不会"穿越"内层已捕获的错误 |
| 与外层 `#Fallible(E)` 的关系 | try-catch 块**完全消化**所捕获的错误类型；外层 fn 的 `#Fallible(E)` **仅**接收 catch arm 内 `ret` 出去的错误 |
| `!` 在 try 外 | 行为同 §4.2（同类型透传 / 跨类型 E7004）；try-catch 不影响 try 域外 `!` 语义 |
| try block 内 `!` 冗余 | E7016 "`!` is redundant inside `try` block"；fix = 移除 `!` |
| try block 全成功 | try block 内不含任何 `#Fallible` 调用 → E7017 |
| catch arm 直接 panic | catch arm body 唯一终结操作是 `panic(...)` → E7018（可 suppress） |

### 5.4 路由模型（informative）

```yux
try { make_error().member } catch e E { HANDLE }
```

`make_error()` 类型 = `#Fallible(E)`；编译器在该调用点生成"若错误则跳到本 try 的 catch 路由"控制流，等价于把每个对 `#Fallible` 函数的调用包成"local 同类型透传，目标是本 try 域而非外层 fn"。实施层不强制嵌套 IR，可扁平化。

### 5.5 与表达式跳出的关系

`var n = try { ... } catch e E { ret X }` —— catch arm 以 `ret`（或 `panic` 等 `#NoReturn` 调用）结尾时该路径流终止，不参与 try 表达式类型合并；`n` 在该路径不被赋值。这与 `var n = if c { a } else { ret X }`、`var n = match e { ... => ret X }` 行为一致（§4.9 / §3.10），不是 try-catch 独有现象。

### 5.6 多类错误的标准写法

每函数最多一个 `#Fallible(E)`；多类错误用嵌套 enum + try-catch 显式 wrap：

```yux
enum ProcessErr { Io(IoErr), Parse(ParseErr) }

#Fallible(ProcessErr)
fn process(path String) i32 {
  var s = try { read_file(path) } catch e IoErr { ret ProcessErr::Io(e) }
  ret try { parse_int(s) } catch e ParseErr { ret ProcessErr::Parse(e) }
}
```

verbosity 是有意代价（避 Java `throws E1, E2, E3` 爆炸）。**未来 generic enum 落地后** wrapper enum 写法会变轻；嵌套通常已够用。本草案不为多类错误聚合预留糖。

## 6. main 出口与诊断

### 6.1 main 错误运行时呈现

main 标 `#Fallible(E)` 且未在 main 内处理：

- 退出码：固定 `_exit(1)`。理由：与 yux 既有"不可恢复终止"路径（`Nullable<T>.get()`、`Array.at()` 越界）一致；不引入"错误 variant → exit code"映射（顺序耦合、ABI 漂移）。用户要区分错误码 → 在更外层 `#Fallible(E_outer)` 函数自行 `match` 后调 stdlib `exit(code)`；main 不再 `#Fallible`。
- stderr 输出：`error: <enum 限定名>::<variant>[(<payload.to_string()>)]\n`
  - 限定名：跟随 §10 模块路径（`.`） + §3.10.4.1 enum variant 限定（`::`）。例：`mymath.ParseErr::Empty`、`app.io.IoErr::NotFound("path")`。
  - payload 走 §12.7.1 `ToString`：实现 → 打 `(payload.to_string())`；未实现 → 省略括号部分。
  - 多行 payload：原样输出（不转义）。
  - 彩色：纯文本，无 ANSI。本输出是用户**程序运行期**的 stderr，混入彩色会污染管道。

### 6.2 诊断段位

错误模型独占段位 **E7xxx**（附录 D 既有 E1-E6 / E11xx；E7-E10 当前空闲；本特性 + 未来 Phase 7 FFI / panic / defer 共享 E7xxx）。警告级码沿用附录 D 既有 `EXXXX + 严重度=warning` 约定（D.1.1 / D.5），不引入 `W` 前缀。

| 错误码 | 措辞模板 |
|---|---|
| **E7001** | `` `!` used outside of `#Fallible(E)` function and outside of `try` block — wrap call in `try { ... } catch e E { ... }` or declare the enclosing function with `#Fallible(E)` `` |
| **E7002** | `` non-exhaustive `try` block: error type `{E}` thrown by callee `{callee}` is not handled by any `catch` clause — add `catch e {E} { ... }` `` |
| **E7003** | `` redundant `catch` clause: error type `{E}` cannot be thrown by any call in the `try` block — promoted to error if `--strict-catch`，否则同 E7015 `` |
| **E7004** | `` cannot propagate error of type `{callee_err}` through `!`: caller declares `#Fallible({E})`, types differ — wrap the call in `try { ... } catch e {callee_err} { ret {E}::Variant... }` `` |
| **E7005** | `` duplicate `catch` clause: error type `{E}` is handled by more than one `catch` in the same `try` — merge into a single `catch e {E} { match e { ... } }` `` |
| **E7006** | `` call to fallible function `{callee}` outside `try` block must propagate via `!` (same error type) — bare call is forbidden outside `try` (inside `try`, bare call is correct; `!` would be redundant) `` |
| **E7007** | `` `ret` of error type `{got}` does not match `#Fallible({expected})` — wrap the error in a `{expected}` variant or change the function's `#Fallible` `` |
| **E7008** | `` function return type `{T}` cannot equal its `#Fallible` type `{E}` (the compiler cannot disambiguate `ret` between success and error channels) — split into two enums and rethrow / wrap explicitly. v1 does not provide a `throw` keyword to disambiguate (reserved for future revision). [generic instantiation]: in `{fn}<{T_arg}>` instantiated at {site}, type parameter `{T}` resolved to `{E}` `` |
| **E7009** | `` `try` block must be followed by at least one `catch` clause — bare `try { ... }` is forbidden `` |
| **E7010** | `` `catch` body must end with `ret`, `panic`-class terminator, or an expression of the same type as the `try` block (got `{T_catch}` vs `{T_try}`) `` |
| **E7011** | `` `catch e {E}` type `{E}` must be a declared enum; got `{actual}` `` |
| **E7012** | `` `#NoReturn` function `{fn}` cannot declare a return type — remove the return type or remove `#NoReturn` `` |
| **E7013** | `` `#NoReturn` and `#Fallible({E})` are mutually exclusive on the same function — a non-returning function cannot also propagate errors `` |
| **E7014** | `` `#NoReturn` function `{fn}` may reach end of body — control flow must terminate via `panic`-class call, another `#NoReturn` call, or unconditional infinite loop `` |
| **E7015** | `` redundant `catch` clause: no call in `try` block can throw `{E}` declared by `catch e {E}` — remove this `catch` clause `` |
| **E7016** | `` `!` is redundant inside `try` block: bare call to `#Fallible({E})` function `{callee}` already routes to the matching `catch e {E}` clause — remove `!` `` |
| **E7017** | `` redundant `try-catch`: no call in `try` block can throw any error — remove the entire `try` and use a plain block `` |
| **E7018** | `` `panic` in `catch` arm converts a recoverable error to abort — consider `ret` with an error variant or `exit(code)` if termination is intended `` |

诊断回归用例：沿用 §11.3.5 / 附录 D 的 `tests/cases/diag_*.yux` 模板，落到 `tests/cases/diag_throw_*.yux`，每条 E7xxx 至少一例。

## 7. FFI / `extern` 边界（v1 整体推迟）

`extern fn` 上的 `#Fallible(E)` 与 FFI 错误翻译 v1 **不开窗口**。两项前置工程：

1. **extern 使用限制收紧**：当前 §7 `extern` 块对 C ABI 签名约束较松；要在 FFI 边界检测 / 翻译错误，需先把"什么 extern 签名合法"明确，否则错误翻译规则的适用面无法定义。
2. **函数类型 `fnType` 形态**：v1 尚无函数类型（callback / 函数指针 / lambda 全空）。将来 `fnType` 加入时，错误类型应是其参数之一（例如 `fn(i32) i32 #Fallible(E)` 或等价形态），与 `#Fallible` 注解保持一致。

任一前置未完成，FFI 错误翻译形态都会反复返工。

**v1 立场（用户视角）**：

| 场景 | 写法 |
|---|---|
| 调 C 库返回 errno | `extern fn read_file(...) i32`；yux 侧 wrapper 检 errno 后 `ret YuxIoErr::...` |
| 给 C 库传 yux callback | v1 无函数类型，整个场景写不出来；非错误模型课题 |
| 在 yux 库里把"可能失败的 C 调用"暴露给上层 | 写一层 yux wrapper：`#Fallible(E) fn yux_read(...) Result { var r = c_read(...); if r < 0 { ret E::IoFail }; ret ok(r) }` |
| 在 `extern fn` 头写 `#Fallible(E)` | ❌（沿用 §3.2 表既存推迟项） |

## 8. panic / 不可恢复

### 8.1 立场：abort-only，与错误通道完全分离

panic = "程序碰到 bug / 不变式破坏，无法继续"。**不**走 `#Fallible(E)` 错误通道，**不**可被 `try-catch` 捕获，**不**可被 `match` 处理。

| 触发源 | 路径 |
|---|---|
| `Nullable<T>.get()` 在 `null` 上 | panic（既有，§3.10.2） |
| `Array.at(idx)` 越界 | panic（既有） |
| 整数除零 / 算术溢出（已有运行时检查的） | panic（沿用既有路径） |
| 用户显式 `panic("...")` | panic（§8.2） |
| OOM（`Box::new` / `Array` 扩容失败） | panic（§8.5） |
| 析构函数抛出 | panic（§1 已定，沿用） |
| `#Fallible(E)` 函数 `ret E::V(...)` | **不是 panic**，是错误通道 |

**与 try-catch 的隔离**：

```yux
try {
  some_fn()         ; 正常错误：可被 catch 接住
  arr.at(999)       ; panic：穿透 try-catch，直接 _exit(1)
  panic("bug")      ; panic：穿透 try-catch
} catch e SomeErr {
  ...               ; 永远接不到 panic
}
```

§5.3 表已规定 catch 类型必须是已声明 enum（E7011）—— panic 不带 enum 类型，从语法上不可能写出 catch panic 形态。零额外约束。

### 8.2 触发形态：stdlib 函数 `panic(msg)`

```yux
#NoReturn
fn panic(msg String) { ... }    ; stdlib，路径 yux.core.panic
```

**不**引入 `panic` 关键字。形态选择：函数调用形态自然 + 编译器仅需识别 `#NoReturn` 注解做控制流分析。yux 无宏系统，不立 `panic!("...")` 形态。

### 8.3 `#NoReturn` 注解

| 形态 | v1 行为 |
|---|---|
| `#NoReturn fn f(...) {}` | ✅ 函数永不返回；控制流分析将 `f()` 之后的代码视为不可达；返回类型必须省略 |
| `#NoReturn fn f(...) i32 {}` | ❌ E7012 |
| `#NoReturn` + `#Fallible(E)` 同函数 | ❌ E7013（互斥：既然 panic 终止，错误通道无意义） |
| `#NoReturn fn` 内未触发终止就到达函数末尾 | ❌ E7014 |

**控制流分析**：`#NoReturn` 调用点视为流终止（与 `ret` 同档），不参与表达式类型合并。`if c { panic(...) } else { 42 }` 整体类型为 `i32`：`panic(...)` 所在 arm 流终止，被排除出"两 arm 类型须一致"的统一计算（§4.9.1.2）。同机制覆盖 `match` arm 与 `try`/`catch` arm（§4.9 / §3.10 / DRAFT §5）。yux 不引入独立 `Never` / `Bottom` 类型名；`#NoReturn` 函数签名仍写空 retType（"无返回值"），永不返回是注解层属性、不是类型层属性。

### 8.4 assert 分流

| 场景 | 路径 |
|---|---|
| 普通 yux 代码内 `assert(cond)` / `assert(cond, msg)` | stdlib 函数；条件不成立 → 调 `panic("assertion failed: <msg>")`；走 §8.1 路径 |
| `#Test` 函数内 assert | 沿用 §11.3.5.3 既有 SEH 路径（`_yux_test_assert_failed()` → `RaiseException(0xE0FA17ED)` → SEH wrapper 接住、记录失败、继续下个用例）。本草案**不动**该路径，仅复述其为 `#Test` 专属机制 |

### 8.5 OOM 决策

`Box::new` / `Array` 扩容 / `String` 扩容等内存分配失败 → **panic**。

理由：让每个 `Box::new` 返回 `Box<T>?` 或 `#Fallible(AllocErr)` 会污染**所有**类型签名与所有调用点；大多数业务代码碰到 OOM 无法有意义恢复。需要可恢复 alloc 的场景，stdlib 后续提供 `try_alloc<T>(...) Box<T>?` 类显式 API（v1 不做）。

### 8.6 main 退出码 / stderr

| 终止源 | 退出码 | stderr 模板 |
|---|---|---|
| `#Fallible` main 错误未处理 | `_exit(1)` | `error: <enum 限定名>::<variant>[(<payload.to_string()>)]\n` |
| panic（含 assert / 越界 / OOM / 用户 `panic("...")`） | `_exit(1)` | `panic: <msg>\n` |
| 用户显式 `exit(code)` | `_exit(code)` | 无（用户自决） |

panic stderr **不**追加调用栈 / 文件 + 行号 / 符号信息：yux 当前无调试基础设施（无 DWARF / PDB / 符号表保留约定 / stack walker），强写只能输出 raw 地址。**例外**：编译器内置 panic（越界 / null get）可在现场附 `file:line`（不走 stack walker，低成本），是否落地实施层决。完整 stack trace 等调试基础设施成熟后追加。

### 8.7 公开 `exit(code u32)` stdlib 函数

```yux
#NoReturn
fn exit(code u32) { ... }    ; stdlib，路径 yux.core.exit
```

参数类型 = **u32**：负退出码无意义（POSIX 截到 0-255 unsigned；Windows ExitProcess 取 UINT）。yux 已支持无后缀整数字面量类型推断，调用形态 `exit(0)` / `exit(2)` 自然。

| 区分 | `panic("msg")` | `exit(code)` |
|---|---|---|
| 语义 | bug / 不变式破坏 | 业务正常终止 |
| stderr 自动模板 | `panic: <msg>\n` | 无（用户自决） |
| 退出码 | 固定 `_exit(1)` | 用户指定 |

**`exit` vs `_exit` 内部名**：`_exit` 是 yux 编译器内部约定（`#CompilerInner`），用户不可调；`exit` 是公开 stdlib 函数。实施层可共用同一底层 syscall（POSIX `_exit(2)` / Windows `ExitProcess`）；命名分裂仅区分"内部触发 vs 用户主动"。若未来引入 atexit / RAII 全局析构，再考虑分裂为 `exit` (clean) / `_exit` (immediate)。

### 8.8 panic 实施层（informative）

panic 实施可复用 §11.3.5.3 SEH RaiseException 机制：

- `panic(msg)` → 调内部 `_yux_panic_failed(msg)` → `RaiseException(<panic_code>)`（panic 用的 SEH exception code 可与 assert `0xE0FA17ED` 同可不同；实施时决）
- `#Test` 模式：现有 SEH wrapper 接住 panic（与 assert 失败一致归为 FAIL，继续下个用例），自然落实"#Test 内 panic 由 SEH 接"
- 普通 `yux build`：无 SEH wrapper → 未匹配 RaiseException → OS 直接终止进程 = `_exit(1)` 等价

**与"无 unwind"立场的关系**：SEH RaiseException 在普通 build 模式下**没有匹配 handler** —— OS 直接终止进程，不走任何 IR 层 cleanup landingpad / DWARF personality / 用户可见 catch。`#Test` 模式的 SEH wrapper 是 `yux test` runner 私有机制，不是用户可写的 catch；不暴露给用户语义，故不违反"无 unwind"立场。

### 8.9 显式不做

- ❌ 可恢复 panic（任何形态的 catch panic / on-panic hook）。
- ❌ panic propagation 跨线程（v1 无并发模型）。
- ❌ `#[no_panic]` 类编译期可达性证明（工具链课题，非错误模型）。
- ❌ panic message 结构化 payload（仅 String；不引入 panic enum / panic info struct）。

## 9. defer / finally：v1 不引入

**不引入** `defer` / `errdefer` / `finally` 关键字或块构造。

### 9.1 取舍

| 场景 | 现状 |
|---|---|
| 既存 RC 资源类型释放（File / Mutex / Socket / Box / Array / String） | RC 析构覆盖：作用域结束自动 release，错误路径同样走 RC 释放。`var f = open(...)`，`ret` / `ret Err::X` / catch arm `ret` 三条路径都正确关 |
| panic 路径上的清理 | **不**运行 —— panic = abort-only（§8.1）；引入 defer 也救不了，且与"不引入栈展开"基调冲突。析构在 panic 路径同样不执行；defer ≈ 析构在这一点上**等价**，不是 defer 独有缺陷 |
| Zig 风格 `errdefer`（仅错误路径清理 / 提交-或-回滚事务） | 用 try-catch + 显式 `match` 写：成功路径调 `commit()`，catch arm 调 `rollback()`。代码长一点但**显式** |
| Java `finally` 释放非 RC 资源 | yux stdlib 立场是所有资源都 RC 化（裸句柄 / FD 由用户 wrap） |
| 用户自写 RAII Guard（C++ ScopeGuard 模式：捕获局部变量 / 闭包，作用域退出运行任意代码） | v1 写起来不简单：需要自定义 struct + 实现析构 + 把要捕获的局部变量"装进" Guard，牵涉 yux 的栈/堆 + `T&` / `Box` 借用-移动语义。**理论上可写，实务上门槛高**；不是"defer 的天然替代" |
| 业务上的 "scope-exit 任意代码"（例：函数退出前打日志 / 累计性能计数器 / 还原全局状态） | v1 无简洁形态；用户在每条退出路径手动重复调用。代码冗余的代价存在 |

### 9.2 用户面对"清理"场景的 v1 写法

1. **RC 析构**（默认）：

   ```yux
   #Fallible(IoErr)
   fn read_first_line(path String) String {
     var f = open(path)!         ; f 是 RC 类型，作用域结束自动 close
     var line = f.read_line()!
     ret line                    ; ret / ret Err 都释放 f
   }
   ```

2. **错误专属清理（事务 / 回滚）** —— 显式 try-catch：

   ```yux
   #Fallible(TxErr)
   fn do_tx() i32 {
     var tx = db.begin()!
     var n = try {
       step1(tx)                  ; try 域内裸调；错误路由到 catch
       step2(tx)
     } catch e StepErr {
       tx.rollback()              ; 错误路径回滚
       ret TxErr::StepFailed
     }
     tx.commit()                  ; 成功路径提交
     ret n
   }
   ```

3. **必须在所有路径执行的非清理副作用**：v1 没有专用语法；用户用 (1) + (2) 组合或显式封装函数。

### 9.3 与 RC release 顺序

错误路径与成功路径的 RC 释放顺序应**一致**（按声明逆序，作用域 LIFO）。在 try-catch 块中：

- try block 内已声明的 RC 值进入 catch arm 时**必须已释放**（因为 try block 已退出）。
- catch arm 内新声明的 RC 值在 arm 退出时释放。

具体 IR 时序属实施层课题，本草案不强制 IR 形态。

### 9.4 未来加入 defer 的触发条件

将来若用户实践产生**反复出现**的痛点，重启该议题。触发条件（满足任一）：

- 出现至少一类无法 RC 化的资源；或
- 用户自写 RAII Guard 类型在 lambda + 借用-移动语义稳定后**仍**门槛高（典型证据：≥3 个常用清理模式被反复手写）；或
- try-catch 编排回滚类逻辑出现 ≥3 次相同模式且用户反馈强烈；或
- panic 模型放松（v1 不预承诺）。

defer 与析构在"不允许抛出"这一点上**对等**；defer 的真正价值在**降低 ergonomic 门槛** —— 不需要为每个清理动作定义 Guard struct + 析构 + 处理捕获的借用/移动。重启该议题时这是核心评估维度，不是"能不能跑、跑得对不对"。

## 10. 不在范围

显式列出本草案**不解决**的问题：

- Java `throws E1, E2, E3` 列表式签名（多错误聚合走嵌套 enum）
- Rust `Result<T, E>` / `From<E>` 协议
- 错误"种类" / 错误专属类型构造器 / 内置 Result 根类型
- `?` 后缀作为错误传播运算符（属 nullable 家族，与"错误→null"折叠等价，禁）
- force-unwrap `!!`
- catch-all（`catch _ { ... }` / `catch e Any { ... }`）
- builder 链式 try-catch（`try {}.catch<E>{}.catch<F>{}`）
- generic enum / 关联类型作错误类型
- `errdefer` / `defer` / `finally` / scoped guard 关键字
- 跨线程 panic propagation（v1 无并发模型）
- panic stack trace / file:line 自动附加（待调试基础设施落地）
- FFI 边界错误翻译（推 §7 待 extern 限制 + fnType 完成）
- `extern fn` 上的 `#Fallible(E)`
- 用户自定义注解形态（沿用 §11.6 现有窗口）
- panic message 结构化 payload

## 11. 迁移面（粗估）

### 11.1 编译器（`src/`）

- 新增 `#Fallible(E)` 注解登记 + 单参数糖（沿用 `#Test` / `#CompilerInner` 路径）
- 新增 `#NoReturn` 注解 + 控制流可达性分析
- 新增 `tryExpr` AST 节点 + 类型路由（穷尽性 / 类型一致性）
- 新增表达式后缀 `!` 优先级 = `exprCall` 同档
- 新增类型分流约束（`T ≠ E` 同函数 / 单态化阶段重跑）
- 新增 stdlib `panic(msg)` / `exit(code)` 内部识别（`#NoReturn` 路径 + 实施层 SEH 复用）
- 诊断码 E7001-E7014 + E7015-E7018 实现
- main 错误退出 stderr 模板生成（含 `ToString` 调用合成）

### 11.2 SDK / runtime（`sdk/`）

- `sdk/yux/src/yux/core/panic.yux`（新建）：`panic(msg String)` / `_yux_panic_failed(msg)` 实现，SEH RaiseException 路径
- `sdk/yux/src/yux/core/exit.yux`（新建）：`exit(code u32)` 实现
- `sdk/yux/src/yux/core/assert.yux`：补充用户面 `assert(cond, msg)` 形态（与 `#Test` 内 `assert_*` 区分）

### 11.3 语法（`src/yux.g4`）

> 改语法属高风险动作，按 CLAUDE.md 项目约束需先与用户确认；本节只列要改什么。

- `buildAnno` 产生式：解禁单参数糖 `'#' ID ( '(' ID ')' )?`
- 新增 `tryExpr ::= 'try' '{' stmt* '}' (catchClause)+`
- 新增 `catchClause ::= 'catch' ID typeRef '{' stmt* '}'`
- 新增 `exprErrPropagate ::= postfix '!'`（加在 `exprCall` 同档）
- 保留字表新增 `try` / `catch`
- 词法 / lexer：后缀 `!` 与 `=` / `==` 之间空白要求（边角约束）

### 11.4 测试（`tests/`）

- `tests/cases/diag_throw_*.yux`：每条 E7001-E7014 + E7015-E7018 至少一例
- `tests/cases/throw_*.yux`：合法形态覆盖（同类型透传 / try-catch 路由 / 嵌套 / `T?` 组合 / 泛型单态化）
- `tests/cases/panic_*.yux`：panic 路径 + assert + exit
- `tests/cases/nofallible_extern_*.yux`：extern fn 上 `#Fallible` 应拒（推 §7 完成时）

### 11.5 规范文档（`docs/spec/`）

- ✅ `docs/spec/04-表达式.md`：§4.12 错误处理表达式（`exprErrPropagate` / `exprTry`）；§4.2.1 优先级表追加；§4.9.1.4 / §4.9.3.5 流终止注释（2026-05-10 落）
- ✅ `docs/spec/06-函数.md`：§6.7 失败声明 `#Fallible(E)`（2026-05-10 落）
- ✅ `docs/spec/11-编译期注解.md`：§11.1.1.1 单参数糖解禁；§11.5.1 表追加 `#Fallible` / `#NoReturn`；§11.5.1.1 计数更新（2026-05-10 落）
- 附录 A：保留字表新增 `try` / `catch`；符号表 `!` 后缀语义补一行
- 附录 B：`exprUnary` 后增 `exprErrPropagate`；新增 `exprTry` / `catchClause`
- 附录 D：新增段位 E7xxx 表（共 18 条；E7001-E7014 默认严重度 = error，E7015-E7018 默认严重度 = warning）
- `docs/spec/CHANGELOG.md`：顶部追加一条（日期 + 摘要 + 影响章节）

### 11.6 用户教程（`docs/`）

- 新增 `docs/<NN>-错误处理.md`：用户视角的简化叙述（`#Fallible` + `ret Err::V` + `!` + try-catch + panic / exit），可在 spec 落定后再写

---

## 决议日志

详细决议讨论保存在 `CURRENT-错误-v1.md`（本地，不入 git）。条目编号 [#1]-[#9] + [#R] 与本草案章节对应关系：

- **[#1]**（Phase 1）→ §1 / §2：值返回 only；防滥用三条硬约束（签名层显式 / 调用点显式 / 不可 catch-all）。
- **[#2]**（Phase 2 + Phase 5 修订撤销 [#2.C]）→ §3 / §4：错误类型 = 普通 enum；每函数最多一个；无自动包装。
- **[#3]**（Phase 3）→ §3：`#Fallible(E)` 注解形态 + §11.1.1.1 单参数糖解禁。
- **[#4]**（Phase 4 + Phase 6 修订加入 try-catch）→ §4 / §5：抛出 `ret E::V`；传播后缀 `!`；try-catch 块作为跨类型形态；显式拒绝 `?` (error→null)。
- **[#5]**（Phase 5）→ §6.1 / §6.2：main 出口 `_exit(1)` + stderr 模板；E7xxx 段位与措辞；draft 内 `#Fallible` 解禁。
- **[#6]**（Phase 6 子项 D / E / F）→ §4.6 / §4.1：`T?` × `#Fallible(E)` 组合允许；泛型 `T` 上 `ret E::V` 单态化阶段重跑 E7008。A/B/C/G 推 Phase 10 实施层（IR 层课题）。
- **[#7]**（Phase 7）→ §7：FFI 边界整体推迟；记两项前置工程。
- **[#8]**（Phase 8）→ §8：panic abort-only；`panic(msg)` stdlib 函数；`#NoReturn` 注解；assert 分流；OOM = panic；main 退出码 `_exit(1)`；`exit(code u32)` 公开 API；不附加 stack trace；实施层复用 §11.3.5.3 SEH。
- **[#9]**（Phase 9）→ §9：defer / errdefer / finally v1 不引入；清理走 RC 析构 + 显式 try-catch；承认 RC 析构非通用 defer 替代（用户自写 Guard 门槛高）；未来重启触发条件四项。
- **[#R]**（整体复审）→ §2 / §4 / §5 / §8：`#Throw` → `#Fallible` 改名（97 处替换）；`try` / `catch` 关键字保留；多类错误用嵌套 enum；`var n = try {} catch {ret X}` 跳赋值同 if/match；单行 if-else **不**加 elif；后缀 `!` 优先级 = `exprCall` 同档高于 `??`；E7018 catch arm 直接 panic 警告；编译器内置注解命名空间不立项（注解未来 = 结构体 build-time only）；panic 实施 = SEH RaiseException 复用。

每个 Phase 的 Open Issues（O1-O36）在 CURRENT 文件中各 Phase 末尾收尾段标注闭合状态。

---

## 定型与归宿

草案定型后按以下步骤拆分迁入正式文档：

1. **§11 列出的每个 spec 章节**逐条改写，引用本草案条目编号（如 [#4.2] / [#5.3]）保留可追溯性。
2. **`docs/spec/CHANGELOG.md`** 顶部追加一条，摘要 = "错误模型 + panic + try-catch v1 落地"，影响章节 = §3 / §4 / §6 / §11 / 附录 A / B / D，日期为合并日。
3. **附录 A / B / D** 按 §11 同步对齐 `src/yux.g4`（前提：用户确认改语法文件）。
4. **`CURRENT.md`** 的 Phase 列表从 §11 派生；本草案对应的 `CURRENT-错误-v1.md` 在落地后归档至 `docs/dev/error-model-impl-log.md`（剔除人名 / 路径 / 行号 / 测试计数）。
5. 处置本 DRAFT：保留为历史档，在头部加一句「已落地，见 §3 / §4 / §6 / §8 / §11」。

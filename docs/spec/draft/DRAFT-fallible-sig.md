# 草案：失败签名 `T ! E`

状态：**草案 / F1 规范草稿**（不动编译器）。日期：2026-09-03。
作用：把 `#Fallible(E)` 注解形态迁移为**签名后缀** `T ! E`，作为修改 `docs/spec/03-类型系统.md` / `04-表达式.md` / `06-函数.md` / `11-编译期注解.md` / 附录 A / B / D 与 `docs/错误处理.md` 的依据；实施计划见 `CURRENT.md` Phase F1–F7。

> **语义 / ABI 不变**：`T ! E` 仅是表面语法；编译器内部仍填 `FnSymbolInfo.fallibleErrType`；LLVM 返回类型仍为 `wrapFallibleRetType` 产出的 `{ i1, T_ok?, E }` struct。`ret` 分流、`!` 传播、try-catch、main wrapper、E7001–E7014 逻辑不改。

> **与 DRAFT-错误.md 的关系**：DRAFT-错误 §3 决议「错误通道不挤占类型语法」；本草案 **废止** 该条（见 §9 / 决议 [#F1.1]），改为签名同行 `T ! E`。DRAFT-错误 其余章节（抛出 / 传播 / try-catch / panic / E7xxx）保持有效，仅把文中 `#Fallible(E)` 引用替换为 `T ! E` 等价表述。

涉及章节（回写面）：§3.11 · §4.11 · §4.12 · §6.7 · §11.5.1 · 附录 A · 附录 B · 附录 D · `docs/错误处理.md`。

---

## 1. 目标

- 错误类型写进**签名同行**，便于搜索与阅读；告别「注解在上一行、返回类型在下一行」的双行形态。
- **双轨共存** → 弃用警告 → 删除 `#Fallible`（Phase F5–F6）；新代码只写 `T ! E`。
- 三处表面语法**同形**：`fn` 声明、lambda 字面量、`Function<..., T ! E>` 类型位。
- 不引入 `Result<T,E>`、不改 ABI、不改 mangle 除「把 `!E` 编入类型串」外的规则。

## 2. 全景

| 概念 | 旧写法（过渡） | 目标写法 | 内部槽位 |
|---|---|---|---|
| 失败声明 | `#Fallible(E) fn f(...) T` | `fn f(...) T ! E` | `retType=T`, `fallibleErrType=E` |
| void 成功 | `#Fallible(E) fn main()` | `fn main() ! E` | `retType=()`（省略）, `fallibleErrType=E` |
| lambda | `(s String) i32 => ...`（靠推断 + body） | `(s String) i32 ! E => ...` | 同 fn |
| 函数类型 | `Function<P, T>` + 注解（无） | `Function<P, T ! E>` | 末位 `elementType` + `fallibleErr` |
| 传播 | `expr!` | `expr!` | 不变 |
| 捕获 | `try { } catch e E { }` | 同左 | 不变 |

要点：

- `!` 在**类型位**读作「可能失败，错误类型为右侧 `E`」；在**表达式位**仍为传播后缀（DRAFT-错误 §4.2），二者共用 `SymbolExcl` token，由产生式位置消歧。
- `E` 必须是已声明的具体 `enum`（与现 `#Fallible(E)` 约束相同）。
- 每个函数最多一个 `E`；多类错误仍用嵌套 enum + try-catch。

---

## 3. 语法产生式（概念形，F2 落地 g4 前须用户确认）

### 3.1 `fn` 声明 `fnHeader`

在现有 `')' (retType=typeWithRef)?` 之后追加可选失败后缀：

```
fallibleSuffix ::= SymbolExcl errType=type

fnHeader       ::= buildAnno*
                   'fn' ID genericDef? '(' LineEnd* fnParams? ')'
                   ( retType=typeWithRef fallibleSuffix?
                   | fallibleSuffix                          ; void 成功：仅 `! E`
                   )
```

示例：

```yux
fn parse_int(s String) i32 ! ParseErr { ... }
fn main() ! AppErr { ... }
fn ok() i32 { ... }                              ; 普通函数，无 `!`
```

约束：

- `retType` 与 `fallibleSuffix` 至少其一出现（`fn f()` 合法；`fn f() ! E` 合法；`fn f() i32` 合法）。
- `errType` 走 `type`（非 `typeWithRef`）：错误 enum 按值返回，不可写 `ParseErr&`。
- `retType` 为 `typeWithRef`：成功返回可含 `T&`（沿用 §8.6.10）。

### 3.2 lambda 字面量 `exprLambdaParen` / `trailingLambda`

```
exprLambdaParen ::= '(' lambdaParams? ')'
                    ( retType=typeWithRef fallibleSuffix?
                    | fallibleSuffix
                    )? '=>' (statementBlock | lambdaBody)

trailingLambda  ::= '{' LineEnd*
                    '(' lambdaParams? ')'
                    ( retType=typeWithRef fallibleSuffix?
                    | fallibleSuffix
                    )? '=>' ...
                  '}'
```

示例：

```yux
let f Function<String, i32 ! ParseErr> = (s String) i32 ! ParseErr => parse_int(s)!
parse { (s String) i32 ! ParseErr => ... }
```

### 3.3 类型位 `type` — `typeFallible`

供 `Function<..., T ! E>` 末位（及嵌套）使用：

```
typeFallible   ::= base=type SymbolExcl errType=type
type           ::= ... | typeFallible
```

`typeFallible` 的 `base` 侧允许 `typeNullable` 展开后的形态（见 §4 `?` 叠层）。

**禁止**：`typeFallible SymbolQuest`（即 `T ! E?`）——见 §4.3。

### 3.4 与 `#Fallible(E)` 互斥

| 形态 | 行为 |
|---|---|
| 仅 `T ! E` | ✅ |
| 仅 `#Fallible(E)` | ✅（过渡期） |
| 两者同函数 | ❌ **E7019** |
| `#NoReturn` + `! E` | ❌ **E7013**（沿用） |

`ast_builder` 两路均解析到 `fallibleErrType` 槽；若槽已被 `#Fallible` 填充且 `fallibleSuffix` 亦出现 → E7019。

### 3.5 `g4` 改动摘要（F2，须用户确认）

| 产生式 | 改动 |
|---|---|
| `fnHeader` | 增 `fallibleSuffix?`；`retType` 改为二选一分支（见 §3.1） |
| `exprLambdaParen` | 同 `fnHeader` 后缀 |
| `trailingLambda` 内 lambda 头 | 同左 |
| `type` | 增 `#typeFallible` 分支：`type SymbolExcl type` |

`SymbolExcl` 已存在（`!`）；`!=` 仍为 `SymbolExclEq`，类型位 `!` 后紧跟类型名，不与 `!=` 冲突。

---

## 4. `?` 与 `!` 叠层

### 4.1 原则

- `?`（可空）修饰**紧邻左侧**的类型单元。
- `! E`（可失败）修饰**紧邻左侧**的成功类型单元。
- 先可空、后失败：`T? ! E`。
- 函数类型末位：`Function<P..., (T? ! E)>` 写作 `Function<P..., T? ! E>`（`typeFallible` 的 `base` 可为 nullable 类型）。

### 4.2 合法组合表

| 写法 | 含义 |
|---|---|
| `i32 ! E` | 成功 `i32`，失败 `E` |
| `i32? ! E` | 成功 `i32?`，失败 `E` |
| `Function<i32, i32 ! E>` | 0 参函数，返回 fallible `i32` |
| `Function<i32, i32? ! E>` | 返回「可空成功值」的 fallible |
| `Function<i32, i32 ! E>?` | 函数值本身可空 |
| `Function<i32, i32? ! E>?` | 函数值可空，且返回 fallible 可空成功 |
| `() ! E` | 成功 unit，失败 `E`（`fn f() ! E`） |
| `Function<(), () ! E>` | 0 参，返回 unit 的 fallible |

### 4.3 非法组合

| 写法 | 原因 | 诊断 |
|---|---|---|
| `i32 ! E?` | 错误类型不可空 | **E2xxx**（复用类型非法形态，或新增 E2xxx 专码 — F2 实施时定） |
| `i32 ! E? ! F` | 双重失败 | 解析/语义拒绝 |
| `#Fallible(E) fn f() T ! E` | 双轨重复声明 | **E7019** |

### 4.4 与 `T?` × `#Fallible` 既有决议对齐

DRAFT-错误 §4.6 / 决议 [#6]：`T?` 与 fallible **允许**组合。迁移后表达到 `T? ! E`，语义不变。

---

## 5. 语义映射（编译器内部，F2+）

### 5.1 AST → `FnSymbolInfo`

| 源码 | `retType` | `fallibleErrType` |
|---|---|---|
| `fn f() i32 ! E` | `i32` | `E` |
| `fn f() ! E` | `()`（void / unit） | `E` |
| `#Fallible(E) fn f() i32` | `i32` | `E`（旧路径，F6 删除） |
| `fn f() i32` | `i32` | `""` |

`visitProgram` 预扫、`structImpl` 方法、lambda `emitLambdaFunction` **统一**读两槽，不再要求 `#Fallible` 注解存在。

### 5.2 `Function<..., T ! E>` → `TypeInfo`

末位类型实参解析为：

- `elementType` = 成功类型 `T`（含 `T?`）
- `fallibleErr` = `E`（新增字段或 `TypeKind::Fallible` 包装 — F3 实施择一）

结构等同：`Function<P, T ! E>` ≡ `Function<Q, S ! F>` 当形参逐位等同且 `T≡S`、`E≡F`。

`rebuildFnName` / 类型打印输出与源码同形：`i32!ParseErr`、`Function<String,i32!ParseErr>`（无空格，与 §3.11.2.4 mangle 风格一致）。

### 5.3 诊断文案迁移（F5 起）

E7001 / E7004 / E7006 / E7007 / E7008 / E7013 等消息中的 `#Fallible(E)` 措辞改为「声明了 `T ! E`」或「enclosing function returns `T ! E`」；码号与触发条件不变。

| 码 | F5+ 典型文案方向 |
|---|---|
| E7001 | `` `!` used outside of a `T ! E` function and outside of `try` block `` |
| E7004 | `` cannot propagate error of type `{}` through `!`: caller returns `T ! {}`, types differ `` |
| E7006 | `` call to `T ! E` function `{}` outside `try` must use `!` or `try-catch` `` |
| E7008 | `` success type `{}` cannot equal error type `{}` in `T ! E` signature `` |
| **E7019** | `` `#Fallible({})` and `T ! E` are mutually exclusive on the same function — use `T ! E` only `` |
| **E7020** | `` `#Fallible({})` is deprecated: use `T ! E` in the signature instead ``（warning，F5） |

---

## 6. Mangle 与符号名

### 6.1 规则

- **ABI 不变**：LLVM 函数类型仍由 `wrapFallibleRetType` 决定，**不**把 `!E` 编进 LLVM struct 字段名。
- **符号 / 调试名 / `Function<...>` 类型串**：与**源码同形**，在成功类型后紧接 `!` + 错误类型名（无空格）。

| 源码签名 | mangle 片段（示例） |
|---|---|
| `fn parse_int(s String) i32 ! ParseErr` | `mod.parse_int(String,i32!ParseErr)` |
| `fn main() ! AppErr` | `mod.main(!AppErr)` 或 `mod.main(!AppErr)`（void 成功位省略 `()`，与现 unit 规则对齐 — F4 实施时与 `rebuildFnName` 统一） |
| `Function<String, i32 ! ParseErr>` | `Function<String,i32!ParseErr>` |
| `Function<i32, i32? ! ParseErr>?` | `Function<i32,i32?!ParseErr>?` |

### 6.2 区分性

同名 `T` 与 `T ! E` 的函数 **必须** mangle 不同（F4 烟测）。现实现若仅按 `retType` mangle，F4 须在 `fallibleErrType` 非空时追加 `!E`。

### 6.3 与 `#Fallible` 过渡期

双轨解析到同一 `fallibleErrType` 时，**mangle 输出一致**（均带 `!E`），不论用户写的是注解还是后缀。

---

## 7. 过渡与删除时间表

| Phase | 内容 | 用户可见 |
|---|---|---|
| **F1** | 本草案 + 回写面清单 | 无 |
| **F2** | g4 + AST 双轨解析 | 可写 `T ! E` |
| **F3** | `TypeInfo` / `Function<..., T ! E>` | 类型检查 / 打印 |
| **F4** | mangle `!E` | 链接符号变化（仅 fallible fn） |
| **F5** | `#Fallible` → **E7020** warning；文档 / 测试迁移 | 旧写法警告 |
| **F6** | 删除 `#Fallible`；**E2005** 或专用码 | 旧写法错误 |
| **F7** | fallible lambda / fn-value 闭合 | 与 `DRAFT-closure-capture.md` §9.1 对齐 |

**本线不做**：`extern fn ... ! E`；`#Fallible(GenericParam)`；`#Fallible(E1,E2)`。

---

## 8. 示例对照

### 8.1 声明

```yux
enum ParseErr { Empty, Invalid(String) }

; 目标
fn parse_int(s String) i32 ! ParseErr {
  if s == "" { ret ParseErr::Empty }
  ret 0i32
}

fn main() ! ParseErr {
  let n = parse_int("42")!
  ret ()
}

; 过渡期仍合法（F6 前）
#Fallible(ParseErr)
fn parse_int_old(s String) i32 { ... }
```

### 8.2 函数类型与 lambda

```yux
let f Function<String, i32 ! ParseErr> = (s String) i32 ! ParseErr => parse_int(s)!

fn apply(op Function<i32, i32 ! ParseErr>, x i32) i32 ! ParseErr {
  ret op(x)!
}
```

### 8.3 try-catch（不变）

```yux
fn load() i32 ! IoErr {
  let code = try {
    read_file("x.txt")
  } catch e IoErr {
    ret e
  }
  ret code.len()
}
```

---

## 9. Spec 回写清单（F6 前可分步推进）

| 文档 | 章节 | 改动摘要 |
|---|---|---|
| `03-类型系统.md` | §3.11 | `Function<..., T ! E>` 产生式、等同、mangle、`?` 叠层表 |
| `04-表达式.md` | §4.11 | lambda `Ret ! E`；§4.12 诊断措辞 |
| `06-函数.md` | §6.7 | 标题改为「失败签名 `T ! E`」；删 `#Fallible` 为主形态 |
| `11-编译期注解.md` | §11.5.1 | `#Fallible` 标 deprecated → F6 删除行 |
| `附录A-保留字.md` | §A.3 | 增 `T ! E` 说明；`#Fallible` 标过渡 |
| `附录B-语法汇总.md` | B.4 / B.6 / type | `fallibleSuffix`、`typeFallible` |
| `附录D-诊断.md` | E7xxx | E7019 / E7020；E7001–E7008 文案 |
| `docs/错误处理.md` | 全文 | 示例与概览表改为 `T ! E` |
| `DRAFT-错误.md` | §3.4 | 追加修订：废止「不挤占类型语法」 |
| `DRAFT-closure-capture.md` | §9.1 | `#Fallible` lambda → `T ! E` 表述 |

实施日志（F2 起）：`docs/dev/fallible-syntax-impl-log.md`（F6 完成后归档）。

---

## 10. Open Issues

| # | 问题 | 倾向 |
|---|---|---|
| O1 | void 成功 mangle：`mod.main(!E)` vs `mod.main(!AppErr)` 是否显式写 `()` | 与现 `Function<()>` unit 规则对齐，省略 `()` |
| O2 | `T ! E?` 专用诊断码 vs 泛型类型错误 | F2 实施时定 |
| O3 | spec / draft 签名 `#Spec` 内是否写 `T ! E` | ✅ 与 `fn` 声明同形 |
| O4 | `typeFallible` 与 `typeNullable` 优先级 | `i32? ! E` 解析为 `(i32?) ! E`；`typeFallible` 的 `base` 含 `typeNullable` 分支 |

---

## 决议日志

- **[#F1.1]**（2026-09-03）失败声明从 `#Fallible(E)` 注解迁移为签名后缀 `T ! E`；废止 DRAFT-错误 §3.4「不挤占类型语法」。ABI / sema / codegen 不变。
- **[#F1.2]** 三处同形：`fnHeader` · lambda · `Function<..., T ! E>`。
- **[#F1.3]** void 成功：`fn main() ! E`（无 `retType`）。
- **[#F1.4]** `T? ! E` 合法；`T ! E?` 非法。
- **[#F1.5]** `#Fallible` 与 `T ! E` 互斥 → E7019；过渡弃用 → E7020。
- **[#F1.6]** mangle 与源码同形：`i32!ParseErr`；`g4` 改动须用户确认（RULES.md）。

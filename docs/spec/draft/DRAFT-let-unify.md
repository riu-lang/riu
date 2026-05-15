; 草案：yux let 声明形态统一

# 草案：yux let 声明形态统一

状态：**草案 / 讨论中**。日期：2026-05-15（v3：默认全不可变 + `#Mut` 显式 + 注解 inline 唯一）。
作用：把**局部和全局**变量声明形态收敛到单一引入符 `let`，把"可变 / 编译期常量 / 深不可变"三档全部移到注解侧。注解形态：局部 `let` / 参数走 **inline**，字段 / `fn` / `struct` 走**顶行**（保留 const-mut P1 现状）。

> **范围缩水声明**：v3 早版本曾打算把字段段也统一到 `let` + 注解；本版决定**字段保留现状**（`var f T` / `val f T` / `cval f T` / `#Val 字段` / `#Frozen 字段`），不引入 `let` 字段形态。理由：字段块本身的 `var/val/cval/注解 + 字段` 形态已经清晰，强制 `let` 让字段块更冗长；SDK 字段 audit 工作量过大；本草案只承担"局部声明统一"这一件事。`#1.D`/`#1.E`/`#1.G` 决议相应作废或限定到局部范围。

> 本草案与 `DRAFT-const-mut.md` 的关系：保留 const-mut 的 `#Frozen` / `#Cval` / `#Const fn` 语义，但**反转字段默认**（const-mut P1-4 字段默认可变 / `#Val` 收紧 → 本草案字段默认不可变 / `#Mut` 放宽），并重写声明形态。落地后 const-mut 草案 §3 / §5 / §6 / §7 全部需要重写；P1-4 的 `#Val` 字段注解被吸收为"默认形态"，注解本身可删。

涉及章节（预估）：§05 语句与控制流、§06 函数、§07 结构体、§11 编译期注解、附录 A 保留字（删 `var` / `val` / `cval`，加 `let` / `#Mut`）。

---

## 1. 目标

- **局部 + 全局**变量声明统一到 `let`：消除局部 `var x` / `val x` / `cval x` 三套；全局常量改 `#Cval let NAME T = literal`。
- 默认安全：局部 `let x` **默认浅不可变**（= 旧 `val` 语义），要可变要显式 `#Mut`。
- 关键字净 -2：加 `let` 一个、删 `var` / `val` / `cval` 三个。
- 注解承担"档位修饰"，**按声明位拆分形态**：局部 `let` / 参数 inline；字段 / `fn` / `struct` 顶行（与 const-mut P1 形态一致，**保留不动**）。
- **字段不在本草案范围**：字段段保留现状 `var/val/cval f T` + 注解形态，未来若需统一另起草案。
- 与 `Self` 类型关键字独立——本草案只动局部 / 全局声明引入符，不动类型层、不动字段层。

## 2. 档位总表（局部 / 全局 `let`）

| 名称 | 写法 | 重绑定 | 子字段写 | 出现位置 |
|---|---|---|---|---|
| **默认（浅不可变）** | `let x T = ...` | ❌ | ✅ | 局部 / 全局 |
| 可变 | `#Mut let x T = ...` | ✅ | ✅ | 局部（全局可变值非本草案讨论） |
| 深不可变 | `#Frozen let x T = ...` | ❌ | ❌ | 局部 / 全局 |
| 编译期常量 | `#Cval let x T = ...` | ❌ | ❌ | 局部 / 全局 |
| 函数副作用纯 | `#Const fn` | —— | —— | 函数声明（独立维度） |

要点：

- 局部 / 全局 `let` 是**唯一**声明引入符（替代 `var/val/cval`）。
- `#Mut` / `#Frozen` / `#Cval` 三注解**互斥**。
- 字段段保留现状，与本表无关；字段层的 `#Val` / `#Frozen` / `#Const fn` 沿用 const-mut 草案不变。

## 3. 子特性 A — 局部 `let`

### 3.1 语法形态

```yux
let x i32 = 1                  ; 默认浅不可变
#Mut let x i32 = 1             ; 可变
#Frozen let x i32 = 1          ; 深不可变（含字段时禁子字段写）
#Cval let x i32 = 1            ; 编译期常量
let y                          ; ❌ 必须有 type 或 = expr（推断）
let y = 1                      ; ✅ 类型推断为 i32
let y i32                      ; ❌ 必须有初值（除非显式 `#Uninit`，本草案不引入）
```

### 3.2 参数声明：`let` 在参数位省略

```yux
fn add(a i32, b i32) i32           ; 默认浅不可变（与现状 E3093 一致）
fn add(#Mut a i32, b i32) i32      ; 参数 a 可重赋
fn dump(#Frozen s String)
```

参数位 `let` 省略。默认与现状一致（E3093 即"参数默认浅不可变"的现行实现），本草案落地后语义不变，**只是统一了概念表述**。

### 3.3 字段声明（**不在本草案范围**）

字段段**保留现状**：

```yux
struct Counter {
  var count i32          ; 可变字段（现状）
  #Val
  val cfg Config         ; 浅不可变（const-mut P1-4 现状）
  #Frozen
  val frozen Config      ; 深不可变（const-mut P1-4 现状）
}
```

未来若决定字段也统一 `let`，另起草案处理。本草案落地后字段层完全不动。

### 3.4 静态检查规则

**唯一规则**：对名字 `x` 的赋值 `x = ...` / `x op= ...`，看声明上注解集合：

| 注解集 | 允许重赋 |
|---|---|
| 含 `#Mut` | ✅ |
| 空（默认） / `#Frozen` / `#Cval` | ❌ |

`#Mut` / `#Frozen` / `#Cval` 三者**互斥**，编译期报错。

**附加规则**：

- `let x` 缺 type **且**缺 `= expr` → checker 报 **E3113**（"`let` 声明必须提供类型或初值"），不在 g4 层强制，便于给出友好诊断。
- `let x` 含 type 但缺 `= expr`（如 `let y i32`）→ checker 报 **E3114**（"`let` 声明必须有初值"），不引入 `#Uninit`。

## 4. 子特性 B — 注解形态按声明位拆分

### 4.1 决议

注解形态**按声明位**拆分两套：

| 声明位 | 注解形态 | 单行写法 |
|---|---|---|
| 局部 `let` | **inline** | `#Mut let x i32 = 1` |
| 参数 | **inline** | `fn f(#Frozen s String)` |
| 字段 `let` | **顶行** | `#Mut`<br>`let count i32 = 0` |
| `fn` / 方法 | **顶行** | `#Const`<br>`fn add(a i32, b i32) i32` |
| `struct` | **顶行** | `#Reflect`<br>`struct Point { ... }` |

理由：

- **单行声明（局部 / 参数）**：声明本身一行就写完，强制把注解再换出去 → 一个声明占 2-3 行，视觉负担过重；inline 紧贴是自然写法。
- **多行声明（字段块 / fn 体 / struct 体）**：声明本身已经多行，顶行注解"挂在最顶上"反而让多个注解能整齐排列，inline 把一行挤到 40-60 列后就读不下去了。
- 这与现状的形态接近一致：const-mut P1 落地的 `#Const fn` / `#Val 字段` / `#Frozen 字段` 写法全部沿用，**不需要迁移**；唯一改动是局部 `let` + 参数位明确"必须 inline"。

### 4.2 多注解写法

- 局部 / 参数：`#Mut #Reflect let counter i32 = 0`，多注解 inline 平铺。
- 字段 / fn / struct：每个注解独占一行，从上到下堆叠：
  ```yux
  #Const
  #Reflect
  fn pure_helper(a i32) i32 = a * 2
  ```

### 4.3 反例

```yux
; ❌ 局部 let 走顶行
#Mut
let a i32 = 1

; ❌ 参数走顶行（参数本就单行，顶行无意义）

; ❌ 字段走 inline
struct S { #Mut let count i32 = 0 }

; ❌ fn 走 inline
#Const fn add(a i32, b i32) i32 = a + b
```

## 5. 不在范围

- 类型推断规则（`let x = expr` 何时合法）：沿用现状，本草案不变。
- 字段层 `#Cval` 静态成员语义：归入独立 `DRAFT-data-struct.md`。
- 成员函数默认是否 `#Const`：**取"不动"**。绝大多数方法读写 $.f + 调其它方法，默认 `#Const` 会让 90% 方法需 `#Mut fn` 解除，比现状更啰嗦。`#Const fn` 仍是独立"完全纯"标记。
- "$ 只读"维度（弱于 `#Const fn`，仅禁写 $.f）：本草案不引入。若未来需要可单独草案。
- `#Const fn` / `#Frozen` 传递规则：沿用 `DRAFT-const-mut.md` §4 §5 §6 §7，本草案只重写**声明形态 + 字段默认**，不动调用 / 借用语义。
- `Self` 类型关键字：独立草案 `DRAFT-spec-unify.md`。
- 析构上下文可变性：仍归 §8 所有权与引用。构造 / 析构内字段写白名单不变。

## 6. 迁移面（粗估）

### 6.1 编译器（`src/`）

- `src/ast/ast_builder.cpp`：删除 `var` / `val` / `cval` 三个 `DeclKey` 分支，新增 `let` 单分支 + inline `buildAnno` 集合解析。删除顶行注解解析路径。
- `src/analyzer/const_mut_checker.{h,cpp}`：
  - 字段默认翻转：检查规则从"默认可写 + `#Val` 收紧"改为"默认不可写 + `#Mut` 放宽"。E3109（字段写）触发面扩大。
  - 局部默认翻转：原 `var` 路径删除，所有局部声明走"默认不可写 + `#Mut` 放宽"。原 E3093（参数重赋）扩展到局部。
  - 输入从"DeclKey enum + 顶行注解列表"改为"inline 注解集合"。
- LSP / formatter / IntelliJ / tmLanguage：高亮规则按 `let` + inline 注解集重写；`var` / `val` / `cval` keyword 从所有词表删；加 `let` keyword、`#Mut` 注解染色。

### 6.2 SDK / runtime（`sdk/`）

- 全仓 `*.yux` 一次性脚本迁移：
  - `var x ...` → `#Mut let x ...`
  - `val x ...` → `let x ...`
  - `cval x ...` → `#Cval let x ...`
- 字段层：
  - 现状 `f T`（默认可变）→ 审视：若该字段实际只在构造期写 → `let f T`；若运行期需写 → `#Mut let f T`。**这是主要工作量**：SDK 字段需逐个 audit。
  - `#Val f T` → `let f T`（默认即浅不可变）
  - `#Frozen f T` → `#Frozen let f T`
- 注解形态：
  - 字段 / `fn` / `struct` 已是顶行：**保留不动**（与 §4 决议一致）。
  - 参数位本就 inline：不动。
  - 局部 `let` 若历史上有顶行 `#X\nlet ...` 写法：改 inline `#X let ...`（实测罕见，可能无现存代码）。

### 6.3 语法（`src/yux.g4`）

> 改语法属于高风险动作，需先与用户确认；本节只列方向。

- 删除 `Var` / `Val` / `Cval` token；新增 `Let` keyword token、`Mut` 作为已有注解 ID（不需要新 token，`#` + `Mut` ID 已可解析）。
- `statement` / `filedDecl` / `fnParamStd`：
  - 删除 `DeclKey` rule。
  - 新增 `letDecl` rule 替代。
  - 参数位 `let` 可省。
- `globalConst` 一并改：`#Cval let NAME T = literal` 顶层形态。
- 注解形态：
  - `letDecl`（局部）/ `fnParamStd`（参数）：仅允许 inline `anno+` 紧贴声明前。
  - `fieldDecl` / `fnDecl` / `structDecl`：仅允许顶行 `anno LineEnd` 堆叠形态。
  - g4 上需要两套产生式区分；ast_builder 在两侧分别合并到统一 `AnnoSet`。

### 6.4 测试（`tests/`）

- 全仓 `*.yux` 用例一次性脚本迁移（同 §6.2）。
- 新增 `tests/cases/diag_let_*.yux` 覆盖：
  - `let x` 缺初值
  - 普通 `let x = 1` 重赋拒收 → 新 E3093 路径
  - 普通 `let f T` 字段写拒收 → 新 E3109 路径
  - `#Mut` + `#Frozen` 同标 → 报错
  - 局部 `let` 用顶行注解 → 报错（"局部 let 需用 inline 注解"）
  - 字段 / fn 用 inline 注解 → 报错（"字段/fn 注解需顶行换行"）

### 6.5 规范文档（`docs/spec/`）

- `05-语句与控制流.md`：声明产生式重写。
- `06-函数.md`：参数声明形态；成员函数默认形态条款保留。
- `07-结构体.md`：字段声明形态 + 默认翻转说明 + 迁移指引。
- `11-编译期注解.md`：
  - 新增 `#Mut` 章节。
  - 删除 `#Val` 章节（被默认吸收）。
  - `#Frozen` / `#Cval` / `#Const fn` 章节注明 inline-only。
  - 新增"注解形态：inline 唯一"小节。
- 附录 A 保留字：删 `var` / `val` / `cval`，加 `let`。
- 附录 B 语法汇总：同步。
- `CHANGELOG.md` 顶部追加一条。

### 6.6 用户教程（`docs/`）

- `基础语法.md`：完全重写"变量"小节。
- `结构体.md`：字段段重写，强调"默认不可变 + #Mut 放宽"。
- 所有教程内嵌示例一次性迁移。

---

## 决议日志

- **[#1.A]** 单 `let` + 注解优于 `val` / `var` / `cval` 三关键字。理由：三处声明位同形态，眼前噪音降到最低；保留字净 -2。
- **[#1.B v3]** **默认全不可变（局部 / 参数 / 字段统一）**，`#Mut` 显式可变。**反转 v2 决议**。理由：与"明确"哲学一致；与参数现状（E3093）一致；字段实际"构造后只读"是多数形态，默认不可变匹配现实；注解集净 0（删 `#Val`、加 `#Mut`）。代价：现 SDK 95% `var` 字段需翻新为 `#Mut let`，是一次性脚本工作量。
- **[#1.C]** 参数位 `let` 省略。
- **[#1.D 作废]** v3 早版本曾决议"字段强制 `let`"；现版本**字段不在本草案范围**，决议作废。
- **[#1.E 作废]** v3 早版本曾决议"`#Val` 注解删除"；字段保留 `#Val` 不动，决议作废。
- **[#1.F]** `#Cval` / `#Frozen` / `#Const fn` 语义沿用 const-mut，**仅形态改变**（局部场景）。
- **[#1.G v2]** 局部 + 全局迁移以 P1.a 并存阶段 + P1.b 一次性脚本完成；字段不动。
- **[#1.H]** 成员函数默认**不动**（仍可读写 $.f）。理由：默认 `#Const`（完全纯）会让 90% 方法需 `#Mut fn`，比现状更啰嗦；"$ 只读"是与 `#Const fn` 独立的弱维度，本草案不引入。
- **[#1.I v2]** 注解形态**按声明位拆分**：局部 `let` / 参数 → inline；字段 / `fn` / `struct` → 顶行。**反转 v1 "inline 唯一" 决议**。理由：单行声明（局部 / 参数）inline 不增行数；多行声明（字段块 / fn / struct）顶行注解更整齐、多注解可堆叠。这与 const-mut P1 落地的形态（`#Const\nfn` / `#Val\n字段`）一致，**字段 / fn 注解无需迁移**，仅局部 / 参数现存的顶行写法（少量）需 inline 化。

---

## 定型与归宿

草案定型后：

1. 与 `DRAFT-const-mut.md` 合并：const-mut 草案的 §3 / §5 / §6 / §7 改为引用本草案的 §2 / §3 / §4；其余（`#Const fn` 检查清单、`#Frozen` 传染规则、构造期白名单）保留。注解形态全改 inline。
2. 按 §6.5 拆分迁入 spec。
3. CHANGELOG 顶部追加 v0.X let-unify 主题条目，特别注明"字段默认翻转 + 顶行注解删"两个破坏性变化的迁移路径。
4. `CURRENT.md` 列 Phase（渐进 P1.a + P1.b，避免一次性 1000+ 文件改动）：
   - **P1.a** g4 加 `Let` token + `letDecl` 产生式（并存阶段，旧 `DeclKey` 路径暂留），ast_builder 把 `let` 当 `val` 处理；先验证 `let x = 1` 可 parse + 编译。
   - **P1.b** SDK + tests 全仓脚本迁移 `var/val/cval` → `let` + 注解，迁移完才删旧 token；字段 audit 同步进行。
   - P2 const_mut_checker 适配字段默认翻转 + 局部默认翻转，E3093 / E3109 新覆盖路径加诊断用例。
   - P3 教程 + spec 改写 + CHANGELOG。
5. 完成后归档 `docs/dev/let-unify-impl-log.md`，原 DRAFT 头部标"已落地"。

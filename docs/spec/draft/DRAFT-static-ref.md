; 草案：T& 返回 + 静态引用

状态：**已落地（Phase 1–2，2026-06-10）**。草案日期：2026-06-09。
实施日志：`docs/dev/static-ref-impl-log.md`。
作用：把「放行 `T&` 作函数返回值 + `&` 接全局/静态变量」这一组决策固化为单一规范，作为修改 `docs/spec/08-所有权与引用.md` §8.6 / §8.6.10 / §8.9 与 `docs/spec/03-类型系统.md` §3.2.3.2 的依据。

> 本草案不引入新类型修饰。`T&` 还是 `T&`——根来自形参还是全局是编译器内部信息，不暴露到类型层。

涉及章节（预估）：§3.2.3（借用类型）、§8.6（借用 T&）、§8.6.10（返回引用的溯源约束）、§8.9（禁忌一览）、附录 D（诊断）。

---

## 1. 目标

- 放行 `T&` 作函数返回值（去掉 E2009），让编译器按 ret 表达式根来源自动判定合法性
- `&` 可接全局变量 / `#Static FIELD` / `cval`
- 不动 type 系统：不引入 `#Static T&` 类型修饰、不要子类型关系
- 调用点不区分"引用来自形参还是全局"——统一当普通 `T&`，寿命走现有 §8.6.5 ⊇ 规则
- v1 保留单源约束（§8.6.10.3），多源返回推 v2
- struct 含 T& 推 v2

## 2. 核心模型

### 2.1 两类合法源

函数返回 `T&` 时，编译器对每条 `ret expr` 追溯引用根。合法根分两类：

| 类别 | 根来源 | 寿命 | 例 |
|------|--------|------|----|
| **外发借用** | 函数声明的 `T&` 形参 | ≤ 实参根寿命 | `fn first(a i32&) i32& = a` |
| **静态借用** | 全局变量 / `#Static FIELD` / `cval` | 程序全程 | `fn config() Config& = &GLOBAL_CONFIG` |

两类可以在同一函数体内出现——只要每条 `ret` 分支的根在合法源集内。

要点：

- `T&` 形参和全局/静态在允许源集里**不互斥**：有 1 个 `T&` 形参时，既可以返回该形参，也可以返回全局/静态引用
- `&` 取址是静态借用的唯一产生途径；现有 `as_ref` / 拷贝绑定 / 函数返回 `T&` 的语义不变
- 静态借用不改变被引用对象的可变性——写入权限由 const-mut 规则（`#Mut` / val / `#Frozen`）独立决定

### 2.2 调用点统一

不管返回的 `T&` 从哪来，调用点**统一当普通 `T&`**：

```yux
fn get_counter() i32& = &COUNTER          ; 静态借用

fn main() {
  let r i32& = get_counter()              ; r 是普通 i32&
  r = 5                                   ; 写入 COUNTER
}
```

寿命走现有 §8.6.5 ⊇ 规则：
- 静态借用：全局作用域 ⊇ 任何局部 → 自动通过
- 外发借用：走 §8.6.10.4 调用点根扩散 → 结果借用的根 = 实参的根

## 3. `&global_var`（静态借用基础）

### 3.1 形态

`&` 可以接全局变量 / `#Static FIELD` / `cval`：

```yux
#Mut
let COUNTER i32 = 0

fn main() {
  let r i32& = &COUNTER     ; ✅ 结果类型 i32&
  r = 1                     ; ✅ COUNTER 变 1
}

let DEFAULT_PORT i32 = 8080

fn get_port() i32& = &DEFAULT_PORT   ; ✅ 静态借用，返回 i32&
```

### 3.2 阻塞点

语法层 `exprGetRef ::= SymbolAnd obj=(ID|SymbolThis) (SymbolDot subs+=ID)*` 已可匹配全局标识符——**不需改 g4**。阻塞点在 codegen 和 borrow checker：

- `compileGetRefExpr`（`src/compiler/expr/expr_unary.cpp`）仅查 `_localVarPtrs`，未命名的全局变量需 fallback 到 `_module->getGlobalVariable(Mangler::global(...))`
- `borrow_checker::rootFromRefInit` 全局根识别 + 根重赋禁豁免

`&Type::STATIC_FIELD`（如 `&Counter::DEFAULT_STEP`）的语法层暂不支持——`exprGetRef` 的 `obj` 不接受 `Type::ID` 路径。先做 `&global_var` / `&cval`（均为简单 `ID`），`&Type::FIELD` 后续单独 MR。

### 3.3 寿命

全局/静态/cval 的声明作用域是模块顶层 ⊇ 任何局部作用域 → §8.6.5.1 ⊇ 规则自动满足。

`&` 接 `#Static FIELD`（如 `&Counter::DEFAULT_STEP`）与接全局变量同规则。

### 3.4 根重赋禁的调整

§8.6.5.5「T& 存活期内根对象不可重赋」对**静态根放宽**：

- 局部根 / T& 形参根：T& 存活期内根不可重赋（现状保留）
- 全局/静态/cval 根：地址固定，T& 存活期内允许修改全局的值

注意：若借的是 `Rc<T>` 全局句柄本身（`&GLOBAL_RC` → `Rc<T>&`），重赋该句柄会 release 旧 payload。这是 `Rc<T>&` 语义的通用行为，与静态无关。

## 4. `T&` 作返回值（外发借用 + 静态借用）

### 4.1 放行 E2009

现状 E2009 阻止非 `#CompilerInner` 函数的 retType 含 `&`。改为：retType 含 `&` 不再无条件拒绝；合法性由 ret 表达式根检查（§4.2）保证。

### 4.2 溯源约束

对返回 `T&` 的函数，编译器对每条 `ret expr` 检查：

- 追溯 `expr` 的引用根（同 §8.6.5.7 根判定算法）
- 要求根 ∈ 允许源集

**允许源集**（替代 §8.6.10.2）：

- 方法（receiver 在 scope 内为 `$`，类型 `Self&`）：允许源集 = `{ $ } ∪ { 全局/静态/cval }`
- 自由函数：
  - 恰好 1 个 `T&` 形参：允许源集 = `{ 该形参 } ∪ { 全局/静态/cval }`
  - 0 个 `T&` 形参：允许源集 = `{ 全局/静态/cval }`（即只能静态借用）
  - ≥2 个 `T&` 形参 → 报 E4021（单源约束保留，§4.2.2）

**v1 单源约束**（§8.6.10.3 保留）：允许源集中恰好一个 T& 形参元素。≥2 个 `T&` 形参时报 E4021，即使不同分支分别只用其中一个也不行——v2 再放松。

例：

```yux
; ✅ 1 个 T& 形参：可外发也可静态
fn first_or_default(a i32&) i32& {
  if some_condition {
    ret a                   ; 外发借用，根 = a
  } else {
    ret &DEFAULT_VALUE      ; 静态借用
  }
}

; ❌ 2 个 T& 形参 → E4021
fn pick(a i32&, b i32&) i32& = a   ; E4021

; ✅ 0 个 T& 形参：只能静态借用
fn always_default() i32& = &DEFAULT_VALUE

; ❌ 0 个 T& 形参 + ret 局部 → 报错
fn bad() i32& {
  let x = 5
  ret &x     ; ❌ x 是局部，非全局/静态/cval，非 T& 形参
}
```

### 4.3 调用点根扩散（外发借用）

方法调用 `recv.method(args)`：根 = `recv` 的根。
自由函数调用 `f(args)`：根 = 函数唯一 `T&` 形参对应实参的根。

调用点结果借用的存活期受 §8.6.5 全部规则约束——等同在调用点直接写 `&<根表达式>`。

### 4.4 `Self&` 等价性

方法返回 `Self&` 与方法返回 `Self`（按值复制）语义不同；两者**不可**混用，类型必须严格匹配（与 §8.6.10.6 一致）。

## 5. 不在范围

- `#Static` 类型修饰（已否决——不需要，type 不动）
- `T&` 在结构体字段（v2，需配合生命周期标注 / pinned 语义）
- `T&` 在容器内层（同上）
- 多源 `T&` 形参返回（v2，放松单源约束 E4021）
- 借用检查器升级 / 生命周期标注（v1 不在范围，§8.10）

## 6. 迁移面（粗估）

### 6.1 编译器（`src/`）— 实际实施

- ✅ `src/compiler/expr/expr_unary.cpp`：`compileGetRefExpr` —— `_localVarPtrs` 找不到时 fallback 到 `_module->getGlobalVariable(Mangler::global(...))`
- ✅ `src/analyzer/borrow_checker.cpp`：
  - `rootFromRefInit` 识全局/静态根 → 返回 immortal 哨兵 `"$rodata"`
  - `_returnAllowedSources` 始终含 `"$rodata"`（全局/静态永不过期）
  - E4021 从 `!= 1` 放宽为 `> 1`（0 T& 形参 + 仅静态借用 → 合法）
  - 调用站自由函数零 T& 实参返回 T& → 溯源到 `"$rodata"`
- ❌ `src/sema/sema_pass.cpp`：E2009 无需调整（已在 v0.15 移除）
- ❌ `src/ast/ast_builder_expr.cpp`：无需改动（`&` 接全局 ID 的 AST 构建路径已通）

### 6.2 语法（`src/yux.g4`）

- ❌ 不需改 g4。`exprGetRef` 已可匹配全局标识符，`typeWithRef` 已在 Phase 4a 支持。

### 6.3 测试（`tests/`）

- `borrow_return_static_*`：静态借用返回 + 调用点写入
- `borrow_return_param_*`：外发借用返回 + 根扩散
- `borrow_return_mixed_*`：同函数内静态 + 外发两分支
- `diag_borrow_return_*`：返回局部引用被拒、≥2 T& 形参 E4021、非静态非 T& 形参根被拒

### 6.4 规范文档（`docs/spec/`）

- §3.2.3.2（禁止列表）：删除「函数返回值 → T&」条目
- §8.6（借用 T&）：新增「静态借用」子条款（根为全局/静态时寿命自动通过）
- §8.6.5.5（根重赋禁）：静态根豁免
- §8.6.10（返回引用的溯源约束）：修订允许源集，纳入静态借用路径；外发借用规则保持
- §8.9（禁忌一览）：删除「函数返回值 T&」条目
- 附录 D：E2009 行为变更说明

### 6.5 SDK / runtime

- 无需改动
- SDK 后续可利用此特性（暴露内部全局的引用视图等）

---

## 7. 与其他草案的关系

- **`DRAFT-static-vars.md`（已落地）**：全局 `let` / `#Static FIELD` / `cval` 作为 `&` 的合法根
- **`DRAFT-const-mut.md`（已落地）**：通过 `T&` 写入的权限由被引用对象的 `#Mut` / val 状态决定
- **§8.6.10（现有规范）**：本草案是该条款的落地——当前只有 `#CompilerInner` 能走，本草案放行用户函数
- **v0.16 闭包捕获**：含 T& 捕获的闭包若返回 T&，走本草案同一套溯源规则（§8.7.6.7 已预留衔接点）

---

## 决议日志

- **[#1]** 不引入 `#Static T&` 类型修饰。type 不动。合法根判定是编译器内部信息，不暴露到类型层。
- **[#2]** 合法源 = T& 形参 ∪ 全局/静态/cval。struct 字段 T& → v2。
- **[#3]** v1 保留单源约束（允许源集中恰好一个 T& 形参）。≥2 个 T& 形参 → E4021。
- **[#4]** 根重赋禁对静态根放宽——全局变量地址固定，不会因重赋导致 dangling。
- **[#5]** struct 含 T& → v2。

---

## 定型与归宿

草案定型后按 `_模板.md` 末尾流程拆分迁入 spec、CHANGELOG、附录，最后归档到 `docs/dev/static-ref-impl-log.md`。

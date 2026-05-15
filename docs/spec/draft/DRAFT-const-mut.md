# 草案：yux 可变性与常量体系（const-mut）

状态：**已落地（2026-05-15）**。原草案保留为历史档；规范落点见 §5.1.5 / §6.1.2a / §6.2.2a / §7.1.4 / §11.6 / §11.7 / §11.8；附录 D §D.3.3 E3104–E3111。实施日志见 [`../../dev/const-mut-impl-log.md`](../../dev/const-mut-impl-log.md)。

----

原状态：**草案 / 讨论中**。日期：2026-05-12。
作用：把"局部 / 参数的可变性档位 + 函数纯洁性 + 编译期常量局部化"这一组决策固化为单一规范，作为修改 `docs/spec/` 中变量、函数相关章节与 `CURRENT.md` 实施计划的依据。

> 命名于 [#1.J] 锁定：`#Const`（函数）/ `#Frozen`（参数+字段，深不可变）/ `#Val`（字段，浅不可变）/ `#Pure`（保留名，本轮不实施，见 [#1.K]）。

涉及章节（预估）：变量与常量、函数、附录 A（关键字）、附录 B（注解）。

---

## 1. 目标

- 把"不可变"细分为两档：运行时一次性绑定 (`val`) vs. 编译期常量 (`cval`)。
- 让 `cval` 突破"仅全局"的现状，可在局部使用。
- 参数默认安全（不能被随手重赋），但不强制深不可变，避免与现有大量代码冲突。
- 为函数提供纯洁性标记 `#Const`，作为深不可变参数 `#Frozen` 与局部 `cval` 求值的依据。
- 不引入新关键字；所有限定符走 `#` 注解，与 `#Test` / `#NoReturn` / `#Fallible` 一致。
- 渐进可落地：四档语义可分四个 Phase 实施，互不阻塞。

## 2. 档位总表

| 名称 | 写法 | 重绑定 | 字段写 | 方法调用 | 初值约束 | 出现位置 |
|---|---|---|---|---|---|---|
| 可变变量 | `var x ...` | ✅ | ✅ | ✅ | 运行时 | 局部 |
| 不可变变量 | `val x ...` | ❌ | ✅ | ✅ | 运行时 | 局部 / 参数（默认） |
| 编译期常量 | `cval x ...` | ❌ | ❌ | 仅 `#Const fn` | 编译期可求值 | 全局 / 局部 |
| 冻结参数 / 字段 | `#Frozen` + 参数 / 字段 | ❌ | ❌ | 仅 `#Const fn` | —— | 参数 / 字段 |
| 浅不可变字段 | `#Val` + 字段 | 构造期内 ✅ / 之后 ❌ | ✅ | ✅ | —— | 仅字段 |
| 不改外部 | `#Const fn` | —— | —— | —— | —— | 函数声明 |
| 纯函数（保留）| `#Pure fn` | —— | —— | —— | —— | 函数声明，本轮不实施 |

要点：

- `val` 是浅不可变（C++ `T* const p` / Kotlin `val`）。"借出去能被改，但你换不了手里这个引用本身。"
- `#Frozen` 是深不可变（C++ `const T&`）。"借出去看一眼，拿回来一模一样。"
- `cval` 等价于 `#Frozen` + 编译期可求值。
- 三种"不可变"档位的检查规则可以共享同一通道，仅在"允许的写操作集合"与"初值约束"上区分。

## 3. 子特性 A — `var` / `val` / `cval` 三档

### 3.1 语法

`val` / `var` 已在 `statement` 文法中存在，本草案不改。`cval` 文法需要扩展：

```
; 现状
globalConst: ... Cval name=ID type SymbolEq literal LineEnd ;  ; 仅顶层、RHS 仅 literal

; 新增（保留 globalConst 作为顶层规则别名）
statement
    : ... 
    | Cval name=ID type SymbolEq expr LineEnd     #statementCvalDeclAssign   ; 局部 cval
    ;
```

顶层 `globalConst` 暂时维持 `literal` RHS（保守），是否放宽到任意 `const`-evaluable expression 由 P4 决定。

### 3.2 静态检查

**唯一规则**：对名字 `x` 形如 `x = ...` / `x op= ...` 的赋值语句，按下表判断：

| 声明形态 | 允许重赋 |
|---|---|
| `var x` | ✅ |
| `val x` / 参数 / `cval x` / `#Frozen` 参数 | ❌ |

字段写 `x.f = ...` 的限制按 §4、§5 单独处理。

### 3.3 cval 局部初值约束（P1）

P1 只允许：基本数值 / `bool` / `null` / 字符串字面量 / 由这些组成的常量表达式（含算术、位运算、比较、逻辑），以及对已声明 `cval` 的引用。

不允许（P1）：`Rc` / `Array` / `String` 构造、结构体构造、任何函数调用。

P4 放宽：允许调 `#Const fn`，前提是所有实参也是常量。

### 3.4 参数默认 val

```yux
fn add(a i32, b i32) i32 = a + b   ; a, b 是 val：函数体内不能 a = ...
```

参数声明在 `SymbolInfo` 中 `writeable=false`。若确需在函数体内重赋，写法待定（候选：`var` 前缀，或新注解 `#Mut`；P1 阶段不放开，保持简单）。

## 4. 子特性 B — `#Const fn`

### 4.1 标记

```yux
#Const
fn add(a i32, b i32) i32 = a + b
```

`#Const` 出现在函数头注解位置，由现有 `buildAnno` 文法承载，不改 g4。

### 4.2 检查清单

`#Const fn` 函数体内禁止：

1. 写 `$.f`（成员函数情形）；
2. 写任何参数的字段：`p.f = ...`；
3. 写非 `cval` 的全局变量；
4. 调任何非 `#Const fn`（包括 SDK / extern）；
5. 调任何对 `$` / 参数有写效果的方法（由 1–4 递归保证）。

允许：

- 读 `$` / 参数 / 全局 `cval` / 全局 `val`；
- 声明并写局部 `var` / `val`（函数体内部的局部状态不算"外部上下文"）；
- 调其它 `#Const fn`。

### 4.3 推断 vs. 显式

P2 阶段不做推断：必须显式标 `#Const`，缺标即视为非 const。未来可加"自动推断 + `#Const` 仅作断言"的语义。

## 5. 子特性 C — `#Frozen` 参数

### 5.1 标记

```yux
fn dump(#Frozen s String) {
  ; s.push(...) ; ❌ String.push 不是 #Const fn
  println(s.size().to_string())
}
```

`#Frozen` 出现在参数位置，紧贴参数。具体语法形态在草案敲定时与 §4 一并和 g4 团队对齐（可能复用 `buildAnno`，可能新增 paramAnno）。

### 5.2 检查清单

对标 `#Frozen` 的参数 `p`，函数体内：

1. 不可重赋（已由 §3.2 覆盖）；
2. 不可写 `p.f = ...`、`p[i] = ...`；
3. 不可调任何非 `#Const fn` 的方法；
4. 不可把 `p` 传给非 `#Frozen` / 非 `val` 的参数位（避免"借出去就被偷写"）。

### 5.3 传递规则（C++ 风格 const 传染）

总原则：**const 可加不可去**（同 C++ 引用语义）。

- `#Frozen` 位置允许接收 `var` / `val` 实参 —— 收紧能力，安全。
- 非 `#Frozen` 位置（`val` / `var` 参数）**禁止**接收 `#Frozen` 实参 —— 等于脱 const。
- 唯一脱 const 出口：**显式深拷贝**，复用既有 builtin `copy_of:<T>(x T&) T`（§11.2.3.2 / §12.7.3）。`copy_of` 接受 `#Frozen` 形态时按 `T&` 看待——**语义重载**，不引入第二条声明，base.yux 中签名仍为 `fn copy_of<T>(x T&) T`，调用点凭实参档位决定走 T& 路径还是 #Frozen 路径。返回独立 owned 值，不带 #Frozen 痕迹。

### 5.4 传染到局部绑定

`#Frozen` 表达式（取自 #Frozen 参数 / #Frozen 字段 / 通过返回值传出的 #Frozen 视图）只能绑定到 `#Frozen val`：

```yux
fn f(#Frozen o Outer) {
  val tmp = o.inner            ; ❌ 类型推导出 #Frozen，必须显式标 #Frozen val
  #Frozen val tmp = o.inner  ; ✅
  var tmp = o.inner            ; ❌ #Frozen 与 var 互斥（语义层报错）
  val fresh = copy_of(o.inner) ; ✅ 显式拷贝，fresh 不带 #Frozen
}
```

传染只作用于"含可写内部"的类型：`struct` / `Rc<T>` / `Array<T>` / `Ref<T>` / `String`。原子类型（`i8`–`u64`、`f32`/`f64`、`bool`、`null`）按值拷出后不挂 #Frozen（标了也无意义）。

## 6. 子特性 D — 结构体字段修饰

### 6.1 三档（默认 var）

| 写法 | 重赋字段 | 改子字段 | 通过该字段调方法 |
|---|---|---|---|
| `f T`（无修饰）| ✅ | ✅ | ✅ |
| `#Val` + `f T` | 构造期内 ✅ / 之后 ❌ | ✅ | ✅ |
| `#Frozen` + `f T` | 构造期内 ✅ / 之后 ❌ | ❌（传染）| 仅 `#Const fn` |

字段修饰一律走注解，注解写在字段上一行：

```yux
struct Outer {
  inner Inner            ; var 默认
  #Val
  tag i32                ; 浅不可变
  #Frozen
  config Config          ; 深不可变（传染）
}
```

变量层用关键字 `var` / `val` / `cval`，字段层用注解 `#Val` / `#Frozen`；这是声明位的天然不对称，由 g4 现状决定（变量层有 `DeclKey` token，字段层没有）。

### 6.2 构造期白名单

`#Val` / `#Frozen` 字段**仅在构造函数 `fn StructName(...)` 内**可被写；其它成员函数禁写。

析构函数（`fn ~()`）的可变性归入 §8（所有权与引用），**不属于本草案**——析构本质需要修改 `$`（如 `conn.close()`），不能套 const-mut 那套规则。

`#Frozen` 字段构造期写入时 RHS 自身必须是 owned（可以是 `copy_of(...)`、新构造、移动），不能是另一个 #Frozen 借用。

### 6.3 字段修饰与外层声明的优先级

字段级修饰**凌驾**外层变量声明：

```yux
struct Outer {
  #Frozen cfg Config
  tag i32
}

var o Outer = Outer(...)
o.tag = 1            ; ✅ tag 是 var 字段
o.cfg = Config(...)  ; ❌ 字段是 #Frozen，外层 var 救不了
o.cfg.x = 1          ; ❌ 同上 + 深传染
```

### 6.4 含 #Frozen 字段的类型在传递中的约束

若类型 `T` 的字段表中有任何 `#Frozen` 标注：

- `T` 类型的值**不能**作为非 `#Frozen` 引用参数的实参（同 §5.3 总原则的实例化）。
- 绑定到本地名只能是 `#Frozen val`（同 §5.4）。
- 脱 const 出口：`copy_of:<T>` 显式深拷贝。

### 6.5 不在本草案范围（移至独立草案）

- 字段层 `#Cval` / "关联常量 / 静态成员" —— 含 per-instance vs static 的归属问题，归入未来 `DRAFT-data-struct.md`。
- data class 自动派生（auto ctor / eq / hash / copy 等）—— 同上。

## 7. 不在范围

- 借用检查器升级、生命周期标注。
- 闭包捕获的可变性传递（lambda 内对外层 var 的写）。
- 多线程下的 `Frozen` 与不变性约束。
- `#Const fn` 的自动推断 / 调用图分析。
- `cval` 中允许堆分配类型（Rc/Array/String）的常量求值。
- 字段层 `#Cval` / 关联常量 / data class 等（独立 `DRAFT-data-struct.md`）。

## 8. 迁移面（粗估）

### 8.1 编译器（`src/`）

- `src/ast/ast_builder.cpp`：参数 `SymbolInfo.writeable` 默认改为 false；`#Const` / `#Frozen` / `#Val` 注解读取。
- `src/ast/node/statement_node.h`：保留 `DeclareType`；考虑新增 `LValMutability` 描述字段写权限 + #Frozen 痕迹位。
- `src/ast/node/`：字段节点扩字段修饰位。
- 新文件 / 既有文件：`src/analyzer/const_mut_checker.{h,cpp}`，承载 §3.2 / §4.2 / §5.2 / §6 检查。
- `src/compiler/`：纯检查器，不影响 IR 生成；`copy_of` 已存在，无需新 builtin。

### 8.2 SDK / runtime（`sdk/`）

- P2 先放开用户代码可标 `#Const`；SDK 内化（`Array.size` / `String.size` / 数值 `to_*` 等明显纯方法）放 P2 末尾或 P3 时一并补。

### 8.3 语法（`src/yux.g4`）

> 改语法属于高风险动作，需先与用户确认。

- `statement` 新增 `cval` 局部声明产生式（P1）。
- `filedDecl` 接 `(buildAnno)*` 允许字段注解（P3）。
- 评估参数注解形态：复用 `buildAnno` 还是新增 `paramAnno`（P3）。

### 8.4 测试（`tests/`）

- 新增 `tests/cases/diag_const_mut_*.yux` 系列：重赋 val / 写 cval / 写 #Val 字段构造期外 / 写 #Frozen 字段 / #Frozen 传给 val 参数 / #Const fn 调非纯函数。
- `sdk/yux/src/yux/core/` 下补正面 `#Test` 用例：cval 局部、参数 val、`copy_of` 脱 #Frozen。

### 8.5 规范文档

- §05（语句与控制流）/ §07（结构体）/ §06（函数）章节：新增 `val` / `cval` 局部条款、参数默认 val、字段修饰、`#Const`。
- §11（编译期注解）：新增 `#Const` / `#Frozen` / `#Val` 三个条款。
- §12.7.3 `copy_of`：补一句"接受 `#Frozen` 形态时按 `T&` 看待"。
- 附录 A（保留字）：无新增（一切走注解）。
- 附录 B（语法汇总）：同步 `filedDecl` / `statement` 中 cval 产生式。
- 附录 D（诊断）：新增 const-mut 系列诊断码。
- `docs/spec/CHANGELOG.md` 顶部追加。

### 8.6 用户教程

- `docs/基础语法.md`：参数默认 val、cval 局部示例。
- `docs/函数.md`：`#Const`。
- `docs/结构体.md`：字段 `#Val` / `#Frozen`。

---

## 决议日志

- **[#1.A]** `val` 选择浅不可变（仅禁重绑定）。理由：与 Kotlin `val` 一致、迁移成本低；深不可变交给 `#Frozen`。
- **[#1.B]** `cval` 应支持局部，否则与 `val` 无差异化。第一阶段初值限基本类型 + 字面量算术。
- **[#1.C]** 参数默认 `val`（不可重绑定），与 Kotlin 一致；强不可变需显式 `#Frozen`。
- **[#1.D]** `#Frozen` 取深不可变语义：禁重绑定 + 禁字段写 + 仅可调 `#Const fn`。
- **[#1.E]** 修饰一律走 `#` 注解，不引入新关键字。
- **[#1.F]** `#Const fn` 与 `#Frozen` 有依赖（深不可变要看方法是否纯）；按 P1 → P2 → P3 → P4 顺序实施。
- **[#1.G]** `#Frozen` 表达式传染到本地绑定时**必须**落在 `#Frozen val` 上：`var tmp = readonly_expr` 语义层报错；`val tmp = readonly_expr` 也报错（要求显式 `#Frozen val`）。`#Frozen var` 组合禁用（语义层报错，不动 g4）。传染只对"含可写内部"的类型生效（struct / Rc / Array / Ref / String），原子类型按值拷出脱锁。
- **[#1.H]** 结构体字段三档：默认 `var`、`#Val`（浅）、`#Frozen`（深）。修饰走注解、写在字段上一行。字段修饰**凌驾**外层声明（外层 `var o` 救不了 #Frozen 字段）。`#Val` / `#Frozen` 字段仅在构造函数内可写，其它成员函数禁写。字段层 `#Cval` / 关联常量 / data class 等推迟到 `DRAFT-data-struct.md`。
- **[#1.I]** const 传递取 C++ 风格——可加不可去。`#Frozen` 实参不可传给非 #Frozen 参数；唯一脱 const 出口为现成 builtin `copy_of:<T>(x T&) T`（§11.2.3.2 / §12.7.3），无需新 builtin。含 #Frozen 字段的类型自动继承该约束。
- **[#1.J]** 注解命名最终化：`#Const`（函数，不改外部状态）/ `#Frozen`（参数+字段，深不可变，传染）/ `#Val`（字段，浅不可变，仅禁构造期外重赋）/ `#Pure`（函数，完全独立、零捕获、零外部读，保留名）。`#Frozen` 优于 `#Readonly`：① 字面意思更紧（"冻住"vs"只读"）；② 与 `#Val` 浅深分层清晰；③ 与 `#Const`（函数侧）词不重复，避免"#Const 是浅是深"的歧义；④ 更短，符合 yux 简短明确显式风格。
- **[#1.K]** `#Pure` 本轮不实施，仅在草案中占名。落脚场景：P4 `cval` 局部初值的编译期求值出口、未来 SIMD / 自动并行候选。`#Pure ⊂ #Const`（`#Pure fn` 自动满足任何要求 `#Const fn` 的位置，不需重复标）。
- **[#1.L]** 析构函数 `fn ~()` **不**受 const-mut 体系约束。析构本质需要修改 `$`（如 `conn.close()` 显然非 #Const），强加纯洁性约束会与现实需求冲突。析构上下文的别名与寿命限制归入 §8（所有权与引用），不属于本草案。
- **[#1.M]** 结构体字段层的 `#Cval` / 关联常量 / 静态成员，**不复用** `#Cval` 这个名字，命名与语义留到后续独立草案（暂用工作名 `DRAFT-data-struct.md` 或 `DRAFT-static-member.md`，最终待定）。

---

## 定型与归宿

草案定型后按 `_模板.md` 末尾流程拆分迁入 spec、CHANGELOG、附录、CURRENT.md Phase 列表，最后归档到 `docs/dev/const-mut-impl-log.md`。

; 草案：yux 堆类型重组（Box→Rc 改名 + Heap 引入 + Arc 占名）

# 草案：yux 堆类型重组

状态：**草案 / 讨论中**。日期：2026-05-15。
作用：重组 yux 的堆相关类型族——把当前误名为 `Box` 的 RC 形态改名为 `Rc`、引入作用域绑定的 `Heap<T>` 形态作为零 RC 中间档、为多线程主题占名 `Arc<T>`、统一深拷贝入口为 `copy_of`。

> 当前 `Box<T>` 的语义（Block 含 strong/weak、可多 owner、`var b2 = b` 走 RC+1）一比一对齐 Rust 的 `Rc<T>`，叫 Box 是命名误导。本草案先正名，再补充"堆但无 RC"的中间档。

涉及章节（预估）：§03 类型系统、§07 结构体（字段持 Heap）、§08 所有权与引用、§09 内置类型、§11 编译期注解（`#OneShot` 占位）、附录 A 保留字、附录 B 语法汇总、附录 C 术语表、附录 D 诊断。

---

## 1. 目标

- 命名诚实：当前 `Box<T>` ≡ Rust `Rc<T>` 一比一，改名修正。
- 完成"栈 or 堆"语义矩阵：当前缺"堆但无 RC、零开销"中间档，补 `Heap<T>`。
- 多线程留口：`Arc<T>` 名字占住，v1.x 落地时不与现存类型撞名。
- 深拷贝唯一入口：`copy_of:<T>(x T&) T`，所有"显式独立拷贝"走它，不挂 `.copy()` 方法。
- 不引入 unique `Box<T>` 单独类型：被 `Heap<T>?` + B 档 nullable move 完全覆盖，避免概念冗余。

## 2. 堆类型矩阵总表

| 类型 | RC 头 | 多 owner | move | 释放点 | 借用检查 | 多线程 |
|---|---|---|---|---|---|---|
| `Rc<T>` | strong+weak | ✅ | (共享) | RC=0 | 已有 scope 栈 | ❌ v1.0 |
| `Arc<T>`（占名）| atomic strong+weak | ✅ | (共享) | RC=0 | 同 Rc | ✅ v1.x |
| `Heap<T>` | 无 | ❌ | ❌（非空形态） | 声明作用域尾 | 已有 scope 栈 | ❌（不可传） |
| `Heap<T>?` | 无 | ❌ | ✅（B 档隐式 null 写回） | 接管者作用域尾 | 同 Heap + nullable flow | ❌（不可传） |
| `Ptr` | 无 | —— | —— | 不管 | FFI 边界 | —— |

要点：

- `Heap<T>` 与 `Heap<T>?` 是**两种形态的一族类型**，用 nullable `?` 区分"是否参与 move"：非空形态钉死作用域、可空形态允许调用点移动。
- 不存在 unique `Box<T>` 单独类型——它的全部能力被 `Heap<T>?` 覆盖。
- `Ptr` 维持现状作 FFI 边界形态，**不**与 `Heap<T>` 合并，避免管理 / 不管理语义混淆。

## 3. 子特性 A — `Box` → `Rc` 改名

### 3.1 范围

`Box<T>` 一切出现处全量改 `Rc<T>`，**零语义变化**：

- ast 内置类型表 / 编译器 baked 类型映射
- SDK `sdk/yux/src/yux/core/*.yux`
- `tests/cases/`、`tests/projects/`
- `docs/`（教程 / spec / draft 全部）
- `examples/`
- 编辑器插件 keyword / semantic token / tmLanguage / IntelliJ 配色（`Box` → `Rc`）
- 错误码诊断信息（"box of type X" → "rc of type X"）

### 3.2 Block layout 不变

```
Rc<T> Block: { i32 strong, i32 weak, T data }    ; 与现状一致
```

`Weak<T>` 维持配对：未来 unique heap（即 `Heap<T>`）不带 Weak，明文写入 spec。

### 3.3 改名时机

**先于 `Heap<T>` 落地**——`Heap` 类型的引入要确保 `Box` 名字不再混淆。改名是独立 phase，可一次性脚本 + CI 通过验证。

## 4. 子特性 B — `Heap<T>` 非空形态

### 4.1 语义

- 堆分配 + 单所有者 + 作用域绑定释放。
- **不可 move**（这是非空形态的核心约束）。
- 没有 Block 头 / 没有 RC / 没有 Weak 配对——layout 等于裸 `T*`，零开销 vs C `malloc`/`free`。
- 借用 `H&`，寿命 ≤ h 所在作用域，沿用现有 scope 栈。

### 4.2 出现位置

| 位置 | 允许 | 备注 |
|---|---|---|
| 局部声明 | ✅ | `let h Heap<T> = Heap:<T>(...)` |
| 函数形参 | ✅（按值 = 接管所有权）| 但 caller 无法 move，所以等价"传值 → 拷一份"——见 §4.4 |
| 函数返回 | ✅（A 档 NRVO）| §6 |
| struct 字段 | ✅ | ctor 内构造，dtor 内释放 |
| `Rc<Heap<T>>` / `Array<Heap<T>>` | ❌ | 等价 escape，编译错 |
| 跨线程 | ❌ | 与 Rc 同 |

### 4.3 构造语法

复用 turbofish 形态（与 `Dyn:<D>(x)` 一致）：

```yux
let h Heap<T> = Heap:<T>(args_for_T_ctor)
```

或借助类型推断：

```yux
let h Heap<Buffer> = Heap:<Buffer>(4096)
```

### 4.4 借用 vs 按值

- `fn f(h Heap<T>&)`：借用形参，h 寿命来自 caller，f 内不能 move 走。
- `fn f(h Heap<T>)`：按值形参——但因为 `Heap<T>` 不能 move，caller 端实参必须经 `copy_of` 拷贝传入；callee 接管自己那份的作用域释放。
  - 编译期提示：`Heap<T>` 按值传通常意味着写错了，编译器给 warning 建议改 `Heap<T>&` 或 `Heap<T>?`。

### 4.5 静态检查规则

**唯一规则**：`Heap<T>` 值不可出现在以下位置：
- 任何 escape：return、装容器、struct 字段以外的赋值。
- 任何 move 起点：`let x Heap<T> = other_heap` 编译错。

诊断码：`E4023`（Heap escape）、`E4024`（Heap move / lambda 捕获非空 Heap，§7）、`E4025`（Heap 跨容器）、`E4026`（NRVO 不可消解，§6.2）、`E4027`（Heap widen to nullable，§8a.2）、`E4028`（Heap 跨 FFI，§8b）。原占位 `Exxx4`（lambda 借用捕获 move-out）**已收回**——该场景由现有 E4022 覆盖（§7.3）。号段由附录 D 正式分配（2026-05-16）。

## 5. 子特性 C — `Heap<T>?` 可空 + B 档 move

### 5.1 语义

- 同 `Heap<T>` 的分配 / 释放规则。
- 状态扩 `null`，参与 yux 现有 nullable flow-sensitive 分析。
- **支持调用点移动**：传 `Heap<T>?` 给按值形参 `Heap<T>?`，调用后实参自动写 null。

### 5.2 B 档移动规则

```yux
fn consume(h Heap<T>?) { ... }    ; 按值，接管所有权

fn caller() {
  let a Heap<T>? = Heap:<T>(...)
  consume(a)              ; 调用后编译器 emit: a = null
  if a != null { ... }    ; flow: a 必 null
}
```

触发条件（全部满足才触发）：

1. 形参类型 = `Heap<T>?`（按值）。
2. 实参表达式是 `Heap<T>?` 类型的左值（局部变量、字段、容器槽）。
3. 调用上下文不是借用模式。

**不**触发的情况：

- 借用 `fn f(h Heap<T>?&)`：保留借用语义，caller 实参不变。
- 实参是临时值（函数返回值直接传）：根本无地址可写 null，按 NRVO 直传。
- 实参是 `Heap<T>` 非空形态：类型不匹配，编译错（要 `consume` 收非空就该写 `consume(h Heap<T>)`，但那条路因为不可 move 通常用借用）。

### 5.3 字段 move-out

```yux
struct Builder { let buf Heap<u8>? }
struct Built { let buf Heap<u8>? }

fn build(b Builder) Built {            ; b 按值接收
  ret Built { buf: b.buf }             ; b.buf move 走 → null；b 即将销毁，析构看到 null 跳过 free
}
```

字段 move-out 写入 null：与局部变量同规则。要求：

- 字段类型必须是 `Heap<T>?`（带 `?`）。`Heap<T>` 非空字段**不可 move-out**，编译错。
- struct 析构器看到 `Heap<T>?` 字段为 null 时自然跳过 free。

### 5.4 分支汇合

```yux
let a Heap<T>? = Heap:<T>(...)
if cond { consume(a) }      ; 此分支后 a == null
                            ; else 分支后 a 保持
println(a.field)            ; ❌ 编译错: 可空类型不可直接 `.` 访问
println(a?.field)           ; ✅ 安全调用，结果 Field?
if a != null {              ; ✅ 缩窄后 a 在块内为 Heap<T>
  println(a.field)
}
```

沿用 yux 现有 nullable 规则：**可空类型只能 `?.` 调用，或经 `if x != null` 缩窄后用 `.`**。本草案不增加新规则。`a` 本身在 B 档 move 后形态仍是 `Heap<T>?`（只是 flow 上必 null），访问规则不变。

## 6. 子特性 D — A 档 NRVO（返回 Heap）

### 6.1 触发与机制

```yux
fn make() Heap<T> {
  let a Heap<T> = Heap:<T>(...)
  ret a                ; A 档触发: a 即将走出作用域，把指针直接写到 caller 返回槽
}
```

编译器 pass：

1. 识别 `ret a` 中 a 是局部 `Heap<T>` 且 fn 返回 `Heap<T>` / `Heap<T>?`。
2. 折叠"分配 a + 拷给 caller + 释放 a"为"分配直写 caller 槽 + 跳过释放"。
3. caller 接管释放责任。

### 6.2 不触发的情况

- a 是借用结果：`let a Heap<T> = ...some_borrow.into_heap()` —— 不属于 a 的所有权，编译错。
- 多候选 / 多 ret：函数体内出现 `ret a` 与 `ret b` 两条以上、且来源不是同一局部 Heap 槽 —— A 档暂不合并候选（理论可行，留 P5 优化）。**这种情况直接报 `E4026`（Heap return: NRVO 不可消解），不退化为 §4.5 的 Heap escape 错**——同样诊断码会让用户误以为问题在 `ret` 本身。
- a 在分支后部分初始化（某分支未赋值）：编译错（既有"未初始化使用"诊断覆盖）。
- 返回类型不是 `Heap<T>` / `Heap<T>?`：编译错（类型不匹配）。

约束：函数体内若同时存在"A 档候选 `ret a`" 与 "对同一 a 的借用 `&a` 已发布到外层"——前者必须放弃 NRVO 改报 `E4026`。借用提前结束（作用域闭合）后允许 NRVO 触发。

### 6.4 与析构钉死的关系

A 档把"分配 + free"折叠成"分配 + 接管"——意味着 `a` 走出作用域时**不**调用 free（已转移给 caller 返回槽）。codegen 实现侧需要在该局部槽位上标记 `moved_out`，析构 IR pass 据此跳过 free。该标记与 §5 B 档 nullable null 写回**复用同一 slot bit**（codegen 内部约定），不引入两套 sentinel。

### 6.3 与 B 档的关系

`ret a` 中 a 为 `Heap<T>?` 时：

- 如果 a 是局部：走 A 档 NRVO，转移所有权。
- 如果 a 是借用 / 字段引用：走 B 档移动规则，源置 null。
- 两档同一表达式不重叠触发（编译期决定哪条路径）。

## 7. 子特性 E — Lambda 返回

Lambda 与普通 fn 同形——A / B 档规则同等适用，**外加捕获维度**。本节沿用 DRAFT-lambda 已落地的 yux 字面量语法（`=>` 标记，0 参 `() => expr` 或 `{ body }`、≥1 参 `(a T) => expr` 或 `{ a T => body }`），捕获自动推断、无显式 `[move]` / `||` 列表。

新增捕获模式补到 DRAFT-lambda §6.2 "按外层类型选择捕获语义" 既有表：

| 外层类型 | 新增的捕获语义 |
|---|---|
| `Heap<T>` | **禁止捕获**——非空形态不可 move（§4.5），lambda 创建即"取走 outer 所有权"在源头就违反约束。报错 `E4024`。 |
| `Heap<T>?` | **按所有权捕获 + 调用点 null 写回**（B 档变体，详 §7.2） |
| `Heap<T>?&` | 走既有"`T&` 借用透传"——含 `T&` 捕获的 lambda 不可逃逸（E4022），自动覆盖"借用捕获 + return-move"组合，无新规则 |

### 7.1 lambda 体内分配，return 出来

A 档 NRVO 不区分 fn vs lambda：

```yux
let f fn() Heap<u8> = () Heap<u8> => {
  let buf = Heap:<u8>(...)
  buf                                ; A 档: lambda 返回槽直写 caller 槽
}
```

0 参带返回类型的块体 lambda 写 `() Heap<u8> => { ... }`；返回类型可由上下文 `fn() Heap<u8>` 反推时简写 `() => { let buf = ...; buf }`（DRAFT-lambda §4）。**禁写** `|| ...` / `|outer&| ...` Rust 风格——yux 字面量统一走 `=>`，捕获自动推断。

### 7.2 捕获 `Heap<T>?` 并 move-out

```yux
let outer Heap<u8>? = Heap:<u8>(...)
let f fn() Heap<u8>? = () => outer     ; outer 被引用 → 自动按所有权捕获
                                       ; lambda 创建点: outer → null（B 档写回）
f()                                    ; 第一次调用: 返回那块 heap，capture 包字段 → null
f()                                    ; 第二次调用: 返回 null（capture 已空）
```

机制：

- 捕获 outer 时 lambda 的 capture 包内建字段 `cap_outer Heap<u8>?`，**按 B 档移动写入**——outer 在 lambda 创建点变 null。
- lambda 体内 `outer` 引用 capture 包字段。
- `ret outer` 触发 B 档：capture 字段值返回、写 null。
- 多次调用安全：第二次起返 null，nullable 类型契合。

**关键**：one-shot lambda 不需要 `FnOnce / FnMut / Fn` 类型层次——nullable + 隐式 null 写回足够表达。可选 `#OneShot` 注解为编译期强制单次调用提供出口（独立小特性，本草案占名）。

### 7.3 借用形态的 Heap 捕获

```yux
fn caller(outer Heap<u8>?&) {
  let f fn() Heap<u8>? = () => outer  ; outer 形参是 Heap<u8>?& → 按 "T& 借用透传" 捕获
  ; ❌ E4022：含 T& 捕获的 lambda 不能 ret 出去
}
```

借用捕获走 DRAFT-lambda §6.4 既有的 E4022 通道（含 `T&` 捕获的 lambda 不可逃逸），**不**需要本草案新加诊断码——原 §7.3 占用的 `Exxx4` **收回**，附录 D 不分配；§4.5 诊断列表里 `Exxx4` 也同步删除。

### 7.4 类型契合

捕获 `Heap<T>?` 并 return 时，lambda 返回类型必须是 `Heap<T>?`（带 `?`）。写 `Heap<T>` 编译错——第二次调用必返 null，类型不诚实。

按 DRAFT-lambda §4 推断规则，`let f fn() Heap<u8>? = () => outer` 中 lambda 返回类型由 `fn() Heap<u8>?` 反推；省略 lambda 显式返回类型完全合法。

## 8. 子特性 F — 深拷贝唯一入口 `copy_of`

### 8.1 签名

```yux
fn copy_of<T>(x T&) T                ; base.yux baked
```

`copy_of` 是 yux 内 **唯一**的深拷贝入口，覆盖所有"产生独立 owned 拷贝"的语义需求。**不挂** `.copy()` / `.clone()` 方法（避免 spec 实现与 struct 用户方法的命名碰撞）。

### 8.2 多形态匹配

按实参类型自动匹配语义（语义重载，单签名）：

| 实参形态 | 行为 | 结果 |
|---|---|---|
| `T&` 普通借用 | 递归字段深拷 | 新 owned `T` |
| `#Frozen` 形态 | 同上，脱 `#Frozen` 痕迹 | const-mut §5.3 |
| `Heap<T>&` | malloc 新块 + 字段深拷 | 新 `Heap<T>` |
| `Heap<T>?&`，源 null | 直接 null | `Heap<T>?` |
| `Heap<T>?&`，非 null | 同 `Heap<T>&` | `Heap<T>?` |
| `Rc<T>` 走深拷而非 retain | `copy_of(as_ref(rc))` | 新 owned `T`（不是新 Rc，是拆出来的 owned） |

### 8.3 与 spec Clone 的关系

`#Spec Clone { fn clone() Self }` 与 `copy_of` 关系：

- `copy_of` 是 baked 内置函数，编译器按 T 实际字段表展开拷贝代码——**编译期反射**路径（见 DRAFT-spec-unify §6）。
- `Clone` spec 仅在用户需要**自定义拷贝逻辑**（例如拷贝时打日志、注册到 registry、修改某字段）时使用。
- 默认情况下，**绝大多数类型不需要实现 `Clone`**——直接 `copy_of(x)` 就行。
- 实现了 `Clone` 的类型上调 `copy_of`：编译器优先走 `Clone.clone()` 而非默认字段拷贝（确保用户自定义逻辑生效）。

### 8.4 RC 类型的拷贝歧义

`Rc<T>` 上不要写 `copy_of(rc)`——歧义。两种语义：

- "想要新 Rc，共享同一底层"：直接 `let r2 = r1`，retain 即可。**不需要 copy_of**。
- "想要新 Rc，底层独立"：`Rc:<T>(copy_of(as_ref(r1)))`——显式拆 + 重新打包，意图明确。

spec 写明：**`copy_of` 不接受 `Rc<T>` 按值/借用形态**，编译错引导用户选明意图。

## 8a. 子特性 F2 — 解引用、形态转换、相等

### 8a.1 payload 借用 `as_ref(h)` 与 `h.field`

`Heap<T>` / `Heap<T>?` **复用 §8.3.5 现有 `as_ref` 形态**，不引入新的解引用语法 `*h` / `&*h`：与 `Rc<T>` 对齐，"无隐式降级"原则继续生效。

| 形式 | `Heap<T>` | `Heap<T>?` |
|---|---|---|
| `as_ref(h)` baked | ✅ 结果 `T&`（寿命 = h 所在作用域，根 = h 自身） | ❌ 类型层报错——`as_ref` 仅接非空形态，与 §8.3.5.5 对 `Rc<T>?` 的拒绝一致 |
| `h.field`（T 是 struct） | ✅ 自动解引用，结果 `Field&`，根 = h | 需 nullable flow 缩窄到 `Heap<T>` 后才允许；否则写 `h?.field` 返回 `Field?` |
| `&h` | ✅ 结果 `Heap<T>&`（借用句柄本身，与 §3.4 `&box` 同形，根 = h 局部槽） | ✅ 结果 `Heap<T>?&` |
| `&h.field` | ✅ 结果 `Field&`，根 = h | 需缩窄后才允许 |
| `*h` / `&*h` | ❌ 不存在该语法（与 §8.3.5.6 对 Rc 的禁止一致） | ❌ 同上 |

借用 `Heap<T>&` 与借用 `Rc<T>&` 在借用检查器中走**同一根注册路径**（句柄本身是根）；`as_ref(h)` 站点的根追溯沿用 §8.6.5.7 对 `as_ref(box)` 的规则——根 = h 自身，借用作用域内 h 不可重赋。

实现侧：`as_ref:<T>(h Heap<T>) T&` 走 §11.2.3 的 `#CompilerInner` baked dispatch，与 `as_ref(Rc<T>)` 同一入口，仅 GEP 偏移不同（Heap 无 RC 头，偏移 0；Rc 跳过 8 字节 RC 头）。

### 8a.2 `Heap<T>` ↔ `Heap<T>?` 形态转换

- **`Heap<T>` → `Heap<T>?`**：**禁止隐式 widening**。理由：`Heap<T>` 非空形态的核心约束是"不可 move"，一旦 widen 到 `Heap<T>?` 即落入 B 档 move 路径——隐式 widen 会让 §4.5 的"不可 move"在调用点静默失效。需要 widen 时只能改 `let` 类型为 `Heap<T>?`，或者一开始就声明 `Heap<T>?`。诊断码 `E4027`（Heap widen to nullable: 需显式重声明）。
- **`Heap<T>?` → `Heap<T>`**：需先 nullable 缩窄（`if h != null { ... }` 块内 h 类型为 `Heap<T>`）。块内对该绑定 `move-out` 仍然受限——因为缩窄只换类型不换所有权流，B 档 move 仍按原始 `Heap<T>?` 形态生效。
- **`Heap<T>` ↔ `Rc<T>`**：**永久禁止隐式互转**。需要的话写 `Rc:<T>(copy_of(as_ref(h)))` 或 `Heap:<T>(copy_of(as_ref(rc)))`，意图显式（与 §8.4 `copy_of` 对 Rc 的拒绝同源）。

### 8a.3 `Weak<T>` 配对

`Heap<T>` 与 `Heap<T>?` **没有 `Weak` 配对类型**。理由：

- `Heap` 无 RC 头，无引用计数语义，弱引用机制（清零策略）无处挂。
- 单所有者 + 作用域绑定释放，已经天然杜绝循环引用——`Weak` 的核心用途不存在。

要弱引用语义请使用 `Rc<T>` + `Weak<T>`。spec §9 显式写明本条。

### 8a.4 相等性

- **`==`**：`Heap<T>` / `Heap<T>?` 上的 `==` **递归到 `*h`**（即对 payload 走 §7 的字段级 eq）。两个 `null` 相等、一 null 一非 null 不等。
- **地址相等**：`same_ref:<Heap<T>>(a, b)` 与 `same_ref:<Heap<T>?>(a, b)` 比较句柄内的裸指针，与 `same_ref` 对 `Rc<T>` 的形态一致。
- 不引入 `==` 的"地址相等" fast-path：`Heap<T>` 的语义点是单所有者，两个独立 `Heap<T>` 绝不可能持同一指针；同一指针的"两份" `Heap` 是 §4.5 直接禁止的状态。`Heap<T>?` 同理（只有一份持有，另一份必 null）。

## 8b. 子特性 F3 — FFI 边界

`Heap<T>` 不是 ABI 稳定形态，**不可**出现在 `extern fn` 的参数或返回类型上：

| 位置 | 允许 | 备注 |
|---|---|---|
| `extern fn(... h Heap<T> ...)` | ❌ | 编译错 `E4028`（Heap 不可跨 FFI） |
| `extern fn() Heap<T>` | ❌ | 同上 |
| `extern fn(... p Ptr ...)` | ✅ | `Ptr` 是 FFI 边界形态、**无泛型**（DRAFT-所有权 §2） |

互转手段（沿用 §9.7 `ptr_of` 既有命名约定，turbofish 走容器类型而非元素类型）：

- **`Heap<T>` → `Ptr`**：baked `ptr_of:<Heap<T>>(h) Ptr`，**交出所有权**——h 在调用点失效（按 §4.5 "Heap 不可被复用"，事实上是把作用域释放责任移交 C 侧）。编译器在该位置 emit `moved_out` 标记（与 §6.4 NRVO 复用同一 slot bit），跳过作用域尾 free。
- **`Ptr` → `Heap<T>`**：baked `Heap:<T>(p Ptr) Heap<T>`，**接管所有权**——caller 承诺该指针由 yux 兼容的 allocator 分配（默认 `__yux_heap_alloc`），作用域尾走 `__yux_heap_free` 释放。**未对齐 allocator 是未定义行为**，spec 不兜底。

这两条转换属于 `unsafe` 语义但 v1 yux 无 unsafe 关键字，故仅在 `extern` 块或带 `#FFI` 注解的上下文允许使用，否则报警告。

注：`ptr_of` 已在 §11.2.3 / §9.7 注册为 `Rc` / `Array` / `String` 的 FFI 出口；本草案只追加 `Heap<T>` 重载，**且增加"交出所有权"语义**——其他 `ptr_of:<Rc<T>>(rc)` 形态依然是只读出指针不动 RC，不受影响。

## 9. 子特性 G — `Arc<T>` 占名

不实现，仅占名，spec §3 / §9 写：

- `Arc<T>` 名字保留，多线程主题（v1.x）落地时引入。
- 预设 Block layout：`{ atomic_i32 strong, atomic_i32 weak, T data }`。
- 预设跨线程行为：`Send` / `Sync`（届时命名）可携带。
- 与 `Rc<T>` 共用 spec 接口但 ABI 不同——届时由专属草案决议是否能透明替换。

**作用**：阻止社区代码 / SDK 在 v1.x 前占用 `Arc` 名字。

## 9a. 边界与未定义行为

### 9a.1 OOM

`Heap:<T>(...)` 与 `Rc:<T>(...)` 在分配失败时的行为统一：**调用 `_exit(1)` 终止进程**，不抛、不返 null。与 `Array.push` 扩容 OOM、§9 现存堆句柄保持一致。可恢复 OOM 不在 v1 范围。

### 9a.2 零大小类型 `Heap<()>` / `Heap<EmptyStruct>`

- 允许声明。
- codegen 仍走真分配（1 字节 / 实现选定的 sentinel），保证每个 `Heap<()>` 句柄地址不同；与 `same_ref` 语义一致。
- 不做 ZST 特化优化，理由：节省的内存极小、且引入 sentinel 后 `Ptr` 互转语义会变复杂。

### 9a.3 大对象与对齐

- `Heap<T>` 的分配对齐 = `alignof(T)`，与 `Rc<T>` payload 对齐规则一致。
- T 超过实现侧栈分配阈值（用户层不可观测）时，编译器仍按 Heap 规则处理，不退化为 alloca。

### 9a.4 自引用 / 循环

- `struct Node { let next Heap<Node>? }`：允许。链状结构天然不构成循环（单所有者）。
- 互引用：因没有 `Weak<Heap<T>>`（§8a.3），互指字段必须经 `Rc<T>` + `Weak<T>`，不能用两边互持 `Heap`——后者构造期就违反"单所有者"。

## 10. 不在范围

- 真正实现多线程语义、`Send` / `Sync` spec、原子 RC ABI：v1.x 异步主题。
- 闭包捕获包的 movable / pinnable 细节：与 v0.8 lambda 主题共同决议（lambda 已规划在路线图，本草案仅触及 Heap 涉及部分）。
- `FnOnce` / `FnMut` / `Fn` 类型层次：不引入。
- unique `Box<T>` 独立类型：永久从路线图划掉。
- struct 字段 move-out 在 spec 默认体 unroll 内的语义：与 DRAFT-spec-unify 共同决议。
- 跨 `Heap<T>` 与其它 owned 形态的隐式转换：永久禁。

## 11. 迁移面（粗估）

### 11.1 编译器（`src/`）

- 类型名重写：`Box` → `Rc`（lexer keyword、ast 类型映射、`isBoxType()` → `isRcType()`）。
- 新增 `Heap<T>` 类型形态：
  - ast / Sema 识别
  - codegen 走 `malloc` + 作用域尾 `free`（不是 `__yux_rc_release`）
  - 借用规则复用 `borrow_checker.cpp`，仅注册新形态对应的根
- A 档 NRVO pass：识别 `ret <local Heap>`，elide free + slot 直写。
- B 档 nullable move：
  - 调用点参数评估后 emit null 写回
  - 字段 move-out 同形
  - flow-sensitive nullable 分析复用现有 pass
- `copy_of` 已存在（const-mut 已用）；扩展支持 `Heap<T>&` / `Heap<T>?&` 实参。
- 诊断码新增：Heap escape / Heap move / Heap into container / lambda borrow-capture move-out 等。

### 11.2 SDK / runtime（`sdk/`）

- 一次性脚本 `Box` → `Rc`：全仓 yux 文件。
- 评估 SDK 中 Heap 适用场景的迁移：
  - IO buffer：`Heap<[u8 * N]>` 替代 `Rc<Array<u8>>` 评估
  - 临时大对象工厂：返回值改 `Heap<T>?` + NRVO
- runtime helper：`__yux_heap_alloc<T>` / `__yux_heap_free<T>` 入口。

### 11.3 语法（`src/yux.g4`）

> 改语法属于高风险动作，需先与用户确认。

- `Heap` 不需要新 keyword（类型名走泛型实例化路径）。
- 但 `Heap<T>?` 需要确保 nullable `?` 与泛型实参解析无歧义——现有 `Box<T>?` 已是同形，无新风险。
- `Heap:<T>(...)` 构造形态复用现有 turbofish 规则。
- `#OneShot` 注解（如果引入）走现有 buildAnno。

### 11.4 测试

- 全仓 `Box` → `Rc` rename：一次性脚本。
- 新增 `tests/cases/heap_*.yux` 与 `diag_heap_*.yux`：
  - heap_local_alloc / heap_borrow / heap_field_init / heap_field_release
  - heap_nrvo_return / heap_nullable_move / heap_field_move_out
  - heap_lambda_capture_move / heap_lambda_borrow_capture_move_fail
  - heap_no_rc_wrap (诊断) / heap_no_array_wrap (诊断) / heap_no_move (诊断)
- 性能微基准：`Heap<[u8 * 4096]>` vs `Rc<Array<u8>>` 分配 / 释放 / 借用读写。

### 11.5 规范文档

- `09-内置类型.md`：拆分 `Rc` / `Heap` / `Ptr` / `Arc` 占名四节，layout / 协议 / 互操作。
- `08-所有权与引用.md`：新增 §8.X "Heap<T> 作用域绑定"小节。
- `11-编译期注解.md`：`#OneShot` 占名（若引入）。
- 附录 A 保留字：无变化（类型名不是保留字）。
- 附录 B 语法汇总：无变化。
- 附录 C 术语表：新增"Heap"、"NRVO（A 档）"、"nullable move（B 档）"、"capture move"。
- 附录 D 诊断：分配 E4023–E4028（原 Exxx4 收回，由现有 E4022 覆盖）。
- `CHANGELOG.md` 顶部追加。

### 11.6 教程

- `内置类型.md` / `结构体.md` / 新增 `内存与所有权.md` 教程：讲清 Rc vs Heap 选择决策树。
- `Lambda与闭包.md`：覆盖 §7 lambda 捕获 Heap 场景。

---

## 决议日志

- **[#1.A]** `Box<T>` → `Rc<T>` 改名独立先做。理由：当前语义一比一对应 Rust Rc，命名误导是认知税最大项；为 Heap / Arc 腾出名字空间。
- **[#1.B]** 引入 `Heap<T>` 作为"堆但无 RC"中间档，补完"栈 or 堆"语义矩阵。
- **[#1.C]** Heap 不引入 move 概念（非空形态彻底无 move），借用检查器保持现状。理由：const-mut / spec-unify / let-unify 已在排队，避免叠加借用检查器升级。
- **[#1.D]** `Heap<T>?` 引入 nullable + 隐式 null 写回作为受控移动机制。move-out 后源置 null，由现有 nullable flow 分析管理。理由：用已有概念白嫖 move 语义、零新检查通道、单回放 unique-Box 整套语义。
- **[#1.E]** 永久不做 unique `Box<T>` 独立类型。理由：`Heap<T>?` 完全覆盖其能力且更便宜；保持类型族小。
- **[#1.F]** A 档 NRVO（return Heap）独立 phase，编译期 pass，无语法变化。
- **[#1.G]** B 档 nullable move 在形参 `Heap<T>?` + 实参左值条件下自动触发。借用形态不触发。
- **[#1.H]** lambda 与 fn 同形——A / B 档同等适用。捕获 `Heap<T>?` 的 lambda 走 capture-field move 路径，多次调用返 null，one-shot 语义由 nullable 自然表达，不引入 `FnOnce` 类型族。
- **[#1.I]** lambda 借用捕获 + return-move = 编译错。
- **[#1.J]** `copy_of:<T>(x T&) T` 统一深拷入口，覆盖 #Frozen / Heap / Heap?，不挂 `.copy()`。
- **[#1.K]** `Rc<T>` 不接受 `copy_of` 直传——歧义。要拆请 `Rc:<T>(copy_of(&*rc))` 显式。
- **[#1.L]** `Arc<T>` 占名不实现，spec 占位。
- **[#1.M]** `Ptr` 维持 FFI 边界形态，不与 Heap 合并。
- **[#1.N]** `Heap<T>` → `Heap<T>?` **禁止隐式 widening**（§8a.2）。理由：widening 会让"不可 move"在调用点被 B 档静默打破——把核心约束架空。需要 nullable 形态必须从声明处写 `Heap<T>?`。
- **[#1.O]** `Heap<T>` 上 `==` 走 payload 递归 eq；地址相等走 `same_ref`（§8a.4）。理由：与 `Rc<T>` 在 §9 / §8.2 的相等约定一致，避免出现"句柄相等" vs "值相等"两套语义。
- **[#1.P]** `Heap<T>` / `Heap<T>?` 不可跨 FFI（§8b）。互转走 `Ptr:<T>(h)` 交所有权 / `Heap:<T>(p)` 接管所有权，且只在 `extern` / `#FFI` 上下文允许。理由：所有权交接不是 ABI 概念，extern 边界硬要传 Heap 会出现"谁释放"歧义。
- **[#1.Q]** `Heap<T>` 没有 `Weak` 配对（§8a.3）。理由：无 RC 头、无循环、单所有者——`Weak` 的三个前提都不成立。
- **[#1.R]** OOM 一律 `_exit(1)`（§9a.1）。与 v1 现有堆句柄行为对齐，可恢复 OOM 留 v2。
- **[#1.S]** 多候选 / 多 ret NRVO 不合并，单独诊断码 `E4026`（§6.2）。理由：退化为 §4.5 escape 错会让用户排查方向偏到"为什么不能返回 Heap"上，与实际"NRVO 不可消解"无关。
- **[#1.T]** A 档 NRVO 与 B 档 move 在 codegen 复用同一 slot `moved_out` 位（§6.4）。避免两套 sentinel + 析构 pass 重复扫描。
- **[#1.U]** `Heap<T>` payload 借用复用 §8.3.5.5 既有 `as_ref` baked，**不**引入 `*h` / `&*h` 解引用语法（§8a.1）。理由：与 `Rc<T>` 的"无隐式降级"原则对齐——v1 已经收口"唯一 payload 借用入口"；为 Heap 重开第二种语法会反复打破该原则，且和 §8.3.5.6 显式禁掉的 `&*box` 直接冲突。
- **[#1.W]** §7 lambda 全部改写为 yux 字面量语法（`=>` 标记 + 捕获自动推断），禁 Rust 风格 `||` / `[move]` / `|x&|`。理由：DRAFT-lambda 已落地、`docs/Lambda与闭包.md` 已发布，多套语法并存会让用户对 spec 失信。捕获模式补到 DRAFT-lambda §6.2 既有表，作为 v1 lambda 表的增量条目；原 `Exxx4`（lambda 借用捕获 move-out）收回——`Heap<T>?&` 形参捕获走既有 E4022（含 T& 捕获不可逃逸），新规则不必要。
- **[#1.V]** `Ptr` **无泛型**（DRAFT-所有权 §2），矩阵表与 FFI 章节统一写 `Ptr` 而非 `Ptr<T>`；`Heap<T>` ↔ `Ptr` 互转 builtin 命名沿用既有 `ptr_of:<HandleType>(handle)` 约定，turbofish 取容器类型（§8b）。理由：另起 `Ptr:<T>(h)` 形会让用户误以为 `Ptr` 是泛型类型；沿用 `ptr_of` 也保持 FFI 出口 builtin 命名一致。`Heap<T>` 重载的"交出所有权"语义是新增的，不影响 `Rc` / `Array` / `String` 既有 `ptr_of` 行为。

---

## 定型与归宿

草案定型后按以下步骤拆分迁入：

1. **Phase 1（独立先做、阻塞 P2+）**：Box → Rc 一次性 rename，全仓脚本 + CI 验证。**必须先于** Heap 落地完成，否则 Heap 与误名的 Box 在 lexer / 诊断中会冲突。归档为 `docs/dev/rc-rename-impl-log.md`。
2. **Phase 2**：`Heap<T>` 非空 + 局部 + 借用 + struct 字段（**不含返回**）。本阶段 ret Heap 暂时报 `E4023`，A 档 NRVO 留 P3。
3. **Phase 3**：A 档 NRVO（return Heap）。完成后 Phase 2 的 `E4023` 在 ret 位置改触发 NRVO 路径。
4. **Phase 4**：`Heap<T>?` + B 档 nullable move（局部 + 形参 + 字段 move-out）。依赖现有 nullable flow，先确认 flow 分析支持"调用点改写"——若不支持，本 Phase 拆为 4a（flow 扩展）+ 4b（B 档接入）。
5. **Phase 5**：lambda 捕获 Heap 全场景（§7）。依赖 v0.8 lambda 主题落地——若 lambda 未到位，本 Phase 阻塞。
6. **Phase 6**：`copy_of` 扩展 Heap 形态支持 + spec Clone 优先级（依赖 DRAFT-spec-unify Phase ≥ Spec 形态可识别）。
7. **Phase 7**：`Arc<T>` 占名条款写入 spec（纯文档，可与任何 Phase 并行）。
8. **Phase 8**：FFI 边界 `Ptr:<T>(h)` / `Heap:<T>(p)` 互转 builtin（§8b）。可与 P5 / P6 并行。
9. 每 Phase 独立 `xmake test` + `yux test` 双回归；diag 用例放 `tests/cases/diag_heap_*`，分组自动落到 `yux/diag`。
10. 全部完成后归档 `docs/dev/heap-types-impl-log.md`，剔除人名 / 私人路径 / 行号 / 测试计数，保留：Block layout（Heap 无 Block）、A/B 档 codegen 协议、NRVO pass 入口、`__yux_heap_alloc` / `__yux_heap_free` runtime 入口。

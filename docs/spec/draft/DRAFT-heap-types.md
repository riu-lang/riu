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
| `Ptr<T>` | 无 | —— | —— | 不管 | FFI 边界 | —— |

要点：

- `Heap<T>` 与 `Heap<T>?` 是**两种形态的一族类型**，用 nullable `?` 区分"是否参与 move"：非空形态钉死作用域、可空形态允许调用点移动。
- 不存在 unique `Box<T>` 单独类型——它的全部能力被 `Heap<T>?` 覆盖。
- `Ptr<T>` 维持现状作 FFI 边界形态，**不**与 `Heap<T>` 合并，避免管理 / 不管理语义混淆。

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
- 借用 `&*h` 给 `T&`，寿命 ≤ h 所在作用域，沿用现有 scope 栈。

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

诊断码（待编号）：`Exxx1`（Heap escape）、`Exxx2`（Heap move）、`Exxx3`（Heap 跨容器）。

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
println(a.field)            ; 编译错 / 警告: a 可能 null，需 a? / if 检查
```

沿用 yux 现有 nullable flow-sensitive 分析，本草案不增加新规则。

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
- 多候选：`ret cond ? a : b` —— A 档暂不处理三元 NRVO（理论可行，但增加诊断复杂度，留 P5 优化）。
- a 在分支后部分初始化：编译错。

不触发时退化为：`Heap<T>` 不能返回的报错（同 §4.5），引导用户改写为 `Heap<T>?` 或 `Rc<T>`。

### 6.3 与 B 档的关系

`ret a` 中 a 为 `Heap<T>?` 时：

- 如果 a 是局部：走 A 档 NRVO，转移所有权。
- 如果 a 是借用 / 字段引用：走 B 档移动规则，源置 null。
- 两档同一表达式不重叠触发（编译期决定哪条路径）。

## 7. 子特性 E — Lambda 返回

Lambda 与普通 fn 同形——A / B 档规则同等适用，**外加捕获维度**：

### 7.1 lambda 体内分配，return 出来

A 档 NRVO 不区分 fn vs lambda：

```yux
let f fn() Heap<u8> = || {
  let buf = Heap:<u8>(...)
  buf                      ; A 档: lambda 返回槽直写 caller 槽
}
```

### 7.2 捕获 `Heap<T>?` 并 move-out

```yux
let outer Heap<u8>? = Heap:<u8>(...)
let f fn() Heap<u8>? = || { outer }   ; 按所有权捕获 outer
                                       ; lambda 创建点: outer → null
f()                                    ; 第一次调用: 返回那块 heap，capture 包内字段 → null
f()                                    ; 第二次调用: 返回 null（capture 已空）
```

机制：

- 捕获 outer 时 lambda 的 capture 包内建字段 `cap_outer Heap<u8>?`，**按 B 档移动写入**——outer 在 lambda 创建点变 null。
- lambda 体内 `outer` 引用 capture 包字段。
- `ret outer` 触发 B 档：capture 字段值返回、写 null。
- 多次调用安全：第二次起返 null，nullable 类型契合。

**关键**：one-shot lambda 不需要 `FnOnce / FnMut / Fn` 类型层次——nullable + 隐式 null 写回足够表达。可选 `#OneShot` 注解为编译期强制单次调用提供出口（独立小特性，本草案占名）。

### 7.3 借用捕获 + return-move

```yux
let outer Heap<u8>? = Heap:<u8>(...)
let f fn() Heap<u8>? = |outer&| { outer }   ; 借用捕获
f()                                          ; ❌ 编译错: 借用不可 move-out
```

借用捕获不持所有权，return-move 无来源。报错诊断码（待编号）：`Exxx4`。

### 7.4 类型契合

捕获 `Heap<T>?` 并 return 时，lambda 类型必须是 `fn() Heap<T>?`（带 `?`）。
写 `fn() Heap<T>` 编译错——第二次调用必返 null，类型不诚实。

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
| `Rc<T>` 走深拷而非 retain | `copy_of(&*rc)` | 新 owned `T`（不是新 Rc，是拆出来的 owned） |

### 8.3 与 spec Clone 的关系

`#Spec Clone { fn clone() Self }` 与 `copy_of` 关系：

- `copy_of` 是 baked 内置函数，编译器按 T 实际字段表展开拷贝代码——**编译期反射**路径（见 DRAFT-spec-unify §6）。
- `Clone` spec 仅在用户需要**自定义拷贝逻辑**（例如拷贝时打日志、注册到 registry、修改某字段）时使用。
- 默认情况下，**绝大多数类型不需要实现 `Clone`**——直接 `copy_of(x)` 就行。
- 实现了 `Clone` 的类型上调 `copy_of`：编译器优先走 `Clone.clone()` 而非默认字段拷贝（确保用户自定义逻辑生效）。

### 8.4 RC 类型的拷贝歧义

`Rc<T>` 上不要写 `copy_of(rc)`——歧义。两种语义：

- "想要新 Rc，共享同一底层"：直接 `let r2 = r1`，retain 即可。**不需要 copy_of**。
- "想要新 Rc，底层独立"：`Rc:<T>(copy_of(&*r1))`——显式拆 + 重新打包，意图明确。

spec 写明：**`copy_of` 不接受 `Rc<T>` 按值/借用形态**，编译错引导用户选明意图。

## 9. 子特性 G — `Arc<T>` 占名

不实现，仅占名，spec §3 / §9 写：

- `Arc<T>` 名字保留，多线程主题（v1.x）落地时引入。
- 预设 Block layout：`{ atomic_i32 strong, atomic_i32 weak, T data }`。
- 预设跨线程行为：`Send` / `Sync`（届时命名）可携带。
- 与 `Rc<T>` 共用 spec 接口但 ABI 不同——届时由专属草案决议是否能透明替换。

**作用**：阻止社区代码 / SDK 在 v1.x 前占用 `Arc` 名字。

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
- 附录 D 诊断：新增 Exxx1..Exxx4 等。
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
- **[#1.M]** `Ptr<T>` 维持 FFI 边界形态，不与 Heap 合并。

---

## 定型与归宿

草案定型后按以下步骤拆分迁入：

1. **Phase 1（独立先做）**：Box → Rc 一次性 rename，全仓脚本 + CI 验证。归档为 `docs/dev/rc-rename-impl-log.md`。
2. **Phase 2**：`Heap<T>` 非空 + 局部 + 借用 + struct 字段，A 档 NRVO。
3. **Phase 3**：`Heap<T>?` + B 档 nullable move（局部 + 形参）。
4. **Phase 4**：lambda 捕获 Heap 全场景（§7）。
5. **Phase 5**：`copy_of` 扩展 Heap 形态支持 + spec Clone 优先级。
6. **Phase 6**：`Arc<T>` 占名条款写入 spec。
7. 每 Phase 独立 `xmake test` + `yux test` 双回归。
8. 完成后归档 `docs/dev/heap-types-impl-log.md`。

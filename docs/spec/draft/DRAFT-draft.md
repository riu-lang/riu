; 草案：yux `draft`（接口/约束）+ `#DraftLike`

状态：**草案 / 决议已收齐，待固化进文档与实施**。日期：2026-05-04。
作用：把 v0.5 引入的"接口/约束"机制（显式 `:` 实现 + `#DraftLike` 结构化匹配 + 泛型边界 `<T : D1 + D2>` + 单态化分发 + 内置 `ToString`）这一组决策固化为单一规范，作为修改 `docs/spec/06-函数.md` `docs/spec/07-结构体.md` `docs/spec/11-编译期注解.md` 与新增 draft 专章、以及 `CURRENT.md` 实施计划的依据。

> 本草案的所有规则均按"决议日志"逐条收齐。后续若有反复，请在日志里追加修订记录，不要直接覆盖正文。

涉及章节（预估）：§6 函数、§7 结构体、§8.6.7 借用与边界（已存在，需衔接）、§11 编译期注解、新增 §12（或并入 §7 子节，依 §10 决定）、附录 A / B / C、`CHANGELOG.md`。

参考输入：
- `MILESTONE.md` v0.5 范围与退出标准
- `docs/spec/draft/DRAFT-所有权与引用.md`（已落地，§8.6.7 边界匹配规则）

---

## 1. 目标

- **显式优先**：默认 draft 必须由 `Type : D { ... }` 显式实现；结构化匹配仅在 draft 显式 `#DraftLike` 时开放（[#A.3] / [#C.4]）。
- **零运行时开销**：v0.5 纯单态化静态分发，不引入虚表 / `dyn` / 隐式签名表参数；mangling 与签名为未来 `dyn Draft` 留位（[#B.3]）。
- **与现有借用模型自然衔接**：复用 §8.6.7 已有规则——T 实参仍 owned，`T&` 自动可调，`Box<U>` 上方法分发归一为 `U&`，不为 Box 引入独立 forward 机制（[#B.2] / [#D.3]）。
- **少特例**：实现块仅合并块且穷尽匹配（[#A.2]）；边界仅内联无 `where`（[#B.1]）；签名等价严格不变（[#C.1]）；orphan 同 rust 禁跨外部包实现（[#C.5]）。
- **为 v0.6 字符串模板提供"可插值"约束**：`ToString` 写在 `base.yux`，**不**标 `#DraftLike`，避免 debug-string 误命中模板（[#D.1]）。
- **链路自洽**：补齐 `T& → T` 反向口子 `copy_of`（[#D.5]），与现有 `as_ref`（Box → T&）形成对称。

## 2. 概念全景

| 名称 | 写法 | 语义 | 关键决议 |
|---|---|---|---|
| draft 声明 | `draft Name genericDef? { fnSig* }` | 一组方法签名（允许零签名） | [#A.1] / [#A.1 微补] / [#B.1 修订] |
| 实现块 | `Type : D1 + D2 { fn... }` | 块内方法**穷尽且不多余**地实现列出 draft | [#A.2] |
| `#DraftLike` 开关 | 标注在 draft 声明 | 开放结构化匹配（按签名集相等） | [#A.3] / [#C.1] |
| 泛型边界 | `<T : D1 + D2>` | 内联在 `genericDef`，无 `where` / 无 or | [#B.1] |
| 调用 ABI | 单态化静态分发 | `<Type>__<DraftPath>__<method>`，无 vtable | [#B.3] |
| 借用 ↔ owned | `as_ref` / `copy_of` | Box→T& / T&→T 两个方向显式 builtin | [#D.5] |
| 内置 `Any` draft | `#DraftLike draft Any { }` | 所有类型自动满足（∅ 签名集） | [#D.4] |
| 内置 `ToString` | `base.yux` 显式 `i32 : ToString` 等 | 方法体走 `#CompilerInner`；不标 `#DraftLike` | [#D.1] |
| orphan 限制 | 实现块只能在 Type 包或 D 包 | 跨外部包绑定走 `#DraftLike` 结构化匹配 | [#C.5] |

## 3. 子特性 A — 显式实现 `:`

### 3.1 声明形态

由 [#A.1] + [#A.1 微补] 决议：draft 体只允许**方法签名**，不允许默认方法体；**允许零签名**（空 draft 合法，用于 [#D.4] `Any`）。

```yux
draft ToString {
  fn to_string() String
}

draft Any { }                 ; 合法：空 draft（[#D.4] / [#A.1 微补]）
```

- 体内的 `fn` 行只能是签名（参数列表 + 返回类型，**无函数体**）；带 `{ ... }` 的形态由语法或语义层拒绝。
- 当前实例引用约定按 §7：方法体内用 `$` / `$.field` / `$.method(args)`（无 `self` 关键字）；draft 内只有签名，因此 §3.1 不出现 `$`。

### 3.2 实现块写法

由 [#A.2] 决议：**仅允许合并块**，块内方法必须**穷尽且不多余**地实现列出的所有 draft 签名。

```yux
struct Counter { value i32 }

; 普通方法块（非 draft）
Counter {
  fn Counter(v i32) { $.value = v }
  fn bump()         { $.value = $.value + 1 }
}

; draft 实现块；列出的 draft 签名必须全部实现，不允许多余方法
Counter : ToString + Hash {
  fn to_string() String { $.value.to_string() }
  fn hash() i64         { $.value.to_i64() }
}
```

规则：

- `Type { ... }`（无 `:`）= 普通方法块，可定义构造 / 析构 / 普通方法。
- `Type : D1 + D2 { ... }` = draft 实现块；块内方法集**必须等于** D1+D2 签名集（缺则报"未实现"，多则报"非 draft 方法不得入此块"）。
- 同一 `Type : D` 不允许出现多次（无论同包跨包；跨包是否允许整体由 §6 orphan 规则决定）。
- 辅助方法去普通块；不要混入 draft 实现块。

### 3.3 出现位置

- 顶层；不允许嵌套
- 同模块内；跨包规则见 §6

## 4. 子特性 B — 泛型边界 `<T : D1 + D2>` 与单态化

### 4.1 边界写法

由 [#B.1] 决议：内联在 `genericDef` 中，单形参写 `T : D1 + D2`；多形参各自带边界，逗号分隔；**不引入 `where` 子句**。

```yux
fn print<T : Display>(x T) { ... }
fn merge<T : Display + Hash, U : Display>(a T, b U) { ... }
struct Cache<K : Hash, V> { ... }
```

产生式调整（草案；实际改 `yux.g4` 前需用户确认 — CLAUDE.md 项目约束）：

```
genericDef        ::= '<' typeParam        (',' typeParam)*        '>'
genericDefWithRef ::= '<' typeParamWithRef (',' typeParamWithRef)* '>'
typeParam        ::= type        (':' draftBound ('+' draftBound)*)?
typeParamWithRef ::= typeWithRef (':' draftBound ('+' draftBound)*)?
draftBound       ::= modulePath? ID genericDef?    ; 例如 ToString / pkg.Display / To<i32> / pkg.Map<K, V>
```

边界条目规则：

- `D` 必须是已声明的 draft（含模块路径解析后），否则报"未知 draft"。
- 同一形参的多约束用 `+` 连接，**无序**；重复 `D + D` 报错。
- v0.5 **不支持** or 约束（`D1 | D2`）、不支持 `?` 修饰、不支持关联类型等高级形态。
- 边界出现在 `genericDef` 槽位（函数 / 结构体定义处），**不出现**在调用点的 `genericDef`（`f:<T>(x)` 形态，§4.6.3）—— 调用点只填实参类型，不填边界。

### 4.2 与 §8.6.7（借用边界）的衔接

由 [#B.2] 决议（Q1–Q5）：

- **Q1 / T 仍须 owned**：`T : D` 不放宽 §8.6.7.1，T 实参仍须为 owned 类型（值类型 / 堆句柄 / `Ptr`），**不接** `T&`。要在借用上调 `D` 的方法，靠 §8.6.7.3 receiver 归一规则。
- **Q2 / D 实现位点单一**：`D` 的实现绑在 owned 类型 T 上即可；`T&` 不需要单独实现 `D`。`obj: T&` 调 `obj.m()` 与 `obj: T` 调一致（方法分发归一，§8.6.7.3）。
- **Q3 / Box forward 等价于现有规则**：§8.6.7.3 保证 `Box<U>` 上 `obj.method` receiver 归一为 `U&`，因此 `Box<U>` 调 `D` 方法**不需要新增 forward 机制**；草案 §8 的 "Box 自动 forward" 在 spec 层等价于"自动解引用 + 方法分发归一"。
- **Q4 / 内置堆句柄可作为 T 实参**：`Array<T>` / `String` / `Weak<T>` 是 owned 类型，可作 `<T : D>` 的 T 实参；其 `D` 实现优先放 `base.yux` 走 `#Builtin`，用户不应在同包再次实现（按 [#C.2] 报错）。
- **Q5 / `T&` 不能写 draft 实现块**：`i32& : ToString { ... }` 之类形态语义层报错；draft 实现绑定到 owned 类型。

```yux
fn show<T : ToString>(x T&) String {     ; ✅ T 是 owned；签名内 T& 来自 §8.6.7.5
  x.to_string()                          ; receiver 归一为 Self&，等价于在 T 上调
}

fn bad<T : ToString>(x T&) ... { ... }
fn pass(b Box<MyStruct>) {
  show(as_ref(b))                        ; ✅ Box→T& 走 §8.3.5.5 + §8.6.7.4
}
```

### 4.3 调用 ABI / 分发

由 [#B.3] 决议：**v0.5 一律纯单态化、静态分发**；不生成 vtable / 不传隐式签名表参数；同时**为未来 `dyn Draft` 留位置**（不实现），约束体现在 mangling 与方法签名上。

**v0.5 落地形态**：

```
fn show<T : ToString>(x T&) String { x.to_string() }

; 调用：
show(&counter)        ; counter: Counter
show(&str)            ; str: String

; 单态化产物（伪 IR）：
define show__Counter (x: Counter*) -> String*  { call Counter__ToString__to_string(x) }
define show__String  (x: String*)  -> String*  { call String__ToString__to_string(x) }
```

- 方法分发：直接 `call <Type>__<DraftPath>__<method>`，无 vtable 间接跳转。
- 泛型函数实例化沿用现有路径（每个 T 一份 IR），边界匹配在单态化前完成。
- `<T : D>` 函数与现有 `<T>` 一样要求调用方可见函数体（不跨编译单元裸导出），非新增限制。

**为 `dyn Draft` 预留**（v1.x 候选，不在 v0.5 范围）：

- 方法 mangling 命名 `<TypePath>__<DraftPath>__<method>` 与签名（receiver + args，**无隐式 vtable 参数**）需保证未来直接作为 vtable 表项使用，不需要回头改 mangling。
- 未来 `dyn Draft` 形态拟为 `(object_ptr, vtable_ptr)` fat pointer；语法层届时新增（候选 `dyn Draft` 关键字），与当前 `T : D` 边界不冲突。
- 当前单态化产生的每个 `<Type>__<DraftPath>__<method>` 函数应能直接填入未来 vtable 表项（无 ABI 改造）。

**mangling 草案**（实施细节，spec 不约束 mangle 命名格式，仅供 LLVM 层参考）：

```
<TypePath>__<DraftPath>__<method>
;  Counter         __ ToString  __ to_string  →  Counter__ToString__to_string
;  yux.core.String __ pkg.Show  __ show       →  yux_core_String__pkg_Show__show
```

边界泛型实例化沿用现有 mangler 规则，把每个 T 实参拼到调用者名字里（与现有 `<T>` 一致）。

## 5. 子特性 C — `#DraftLike` 结构化匹配

### 5.1 标注位置

由 [#A.3] 决议：`#DraftLike` **只能标在 draft 声明上**，使用处（泛型边界、参数类型等）不允许再加。

```yux
#DraftLike
draft Display {
  fn show() String
}

fn print<T : Display>(x T) { ... }
;       ↑ 不允许再写 #DraftLike，draft 是否结构化匹配由声明决定
```

- "是否开放结构化匹配"是 draft 的**语义属性**，不在调用点改变，避免同一 draft 在不同位置行为不一致。
- 未标 `#DraftLike` 的 draft 必须显式 `:` 实现；调用方不能绕过。

### 5.2 签名等价判定

由 [#C.1] 决议：

| 维度 | 规则 |
|---|---|
| 方法名 | 完全相同（大小写敏感） |
| 参数个数 | 相同 |
| 参数类型 | 逐一按 §3 类型相等判定（不协变、不变） |
| 参数名 | 不参与判定 |
| 返回类型 | 相同（不协变） |
| receiver 形态 | 不参与（按 §8.6.7.3 归一为 `Self&`） |
| `T` vs `T&` 参数 | 必须一致（不互通） |
| draft 体内方法的本地泛型 | **禁止**（draft 体内 `fn` 签名不得引入新的 `<...>` 形参） |
| 注解（如 `#Inline`） | 不参与 |
| 可见性 | 必须按 §10 可见 |

draft 自身**允许**带泛型形参（详见 [#B.1 修订]）：

```yux
draft To<T> {
  fn to() T
}

Counter : To<String> {
  fn to() String { $.value.to_string() }
}

Counter : To<i32> {                       ; ✅ 同一类型可对不同 T 实参分别实现
  fn to() i32 { $.value }
}
```

但 draft 体内单个 `fn` 不得再引入新的泛型形参（即 `fn map<U>(...)` 之类禁止）。需要这种能力时，写在外层泛型函数 `fn map<T : Mappable, U>(...)` 中。

**收集策略**：按需匹配。出现 `<T : D>` 调用、T 实参为具体 `Counter` 时，在编译单元内检查 `Counter` 是否存在等价方法集；不预扫全包"所有 `#DraftLike` 命中"。

### 5.3 显隐优先级与冲突

由 [#C.2] 决议（结合 [#C.5] 禁 orphan，跨包显隐共存的情形 C 不存在）：

- **情形 A — 同包**（用户自己包内同时写普通方法和 `Type : D` 显式实现，两者同签名）：**error**。报"同签名方法在普通块与 draft 实现块重复定义"。
  - 根本原因：`counter.show()` 调用点会面临"调普通块的还是 draft 块的"分发歧义；不允许写出来。
  - 即便 D 没标 `#DraftLike`（只是显式实现），也按本规则报错（实现块本身已穷尽 D 签名，普通块再定义同名同签名属重复定义）。
- **情形 B — 跨包**（D 在外部包，且标 `#DraftLike`；类型在本包或第三方包，仅有同签名普通方法）：**结构化匹配自动命中**，无 warning。这是 `#DraftLike` 的初衷。
- **情形 C — 跨包显隐共存**：由 [#C.5] 禁止 orphan，此情形不存在。

```yux
#DraftLike
draft Display { fn show() String }

struct Counter { value i32 }

Counter {
  fn show() String { ... }    ; (1)
}

Counter : Display {           ; (2) ❌ 与 (1) 同签名 → error（情形 A）
  fn show() String { ... }
}
```

### 5.4 多包同名 draft 命中

由 [#C.3] 决议：**不引入新规则**。理由：调用点用哪个 draft 由 `D` 完全限定名（§10 名字解析）确定，结构化匹配是"`Counter` 是否满足这个具体 D"的判断，与"是否还有别的同名 D 存在"无关。

具体行为：

- `<T : a.Show>` 与 `<T : b.Show>` 是两个独立约束，分别独立判断 `Counter` 是否结构匹配；两者并存不冲突。
- import 同名（`use a.Show; use b.Show`）按 §10 已有名字冲突规则处理，与 draft 无关；可用 `as` 别名（如 §10 支持）或完全限定名消歧。
- 普通成员调用 `x.show()` 始终走普通方法分发（§4 / §6），**不**经 draft 结构化匹配；只有 `<T : D>` 边界场景才触发结构化命中。
- 因此 DRAFT.md 原文「匹配多包同名 Draft 报错」在本模型下不构成独立规则。

### 5.5 滥用边界

由 [#C.4] 决议，写入未来 spec §12.X.Y（编号待 Phase 2 定）：

1. draft **默认**为封闭契约：未标 `#DraftLike` 的 draft 仅接受显式 `Type : D { ... }` 实现块。
2. `#DraftLike` 是 draft 声明的"开放结构化匹配"开关；标注后该 draft 在 `<T : D>` 场景允许结构化命中（§5.2）。
3. 标注 `#DraftLike` **应当**有明确语义动机；仅出于"少写 `:` 实现"目的开放属于反模式，编译器不强制阻止，规范层标记为不推荐。
4. 编译器**应当**在以下场景报「`#DraftLike` 误用」诊断：
   - 标在 draft 之外的声明上（fn / struct / global / 实现块 …）；
   - 标在带默认体的 draft 上（与 [#A.1] 矛盾，本版本默认体即被禁，对应诊断仍保留作前向兼容）；
   - 标在体内带方法本地泛型形参的 draft 上（与 [#C.1] 矛盾）。

错误码：暂定 `E1100` 段（draft 类）；具体编号在 Phase 2 回写 spec 与附录 D 时确定。

## 6. 跨包规则（orphan 规则）

由 [#C.5] 决议：**禁止为外部包类型实现外部包 draft**（同 rust orphan rule）。`Type : D { ... }` 实现块只能写在以下位置之一：

- `Type` 所在的包，或
- `D` 所在的包。

否则编译期报错。

效果：
- 避免外部库被第三方"污染"实现，破坏可推断性。
- 让 [#C.2] 情形 C（跨包显隐共存）不存在，简化优先级模型。
- `#DraftLike` 是跨包"无显式实现也能匹配"的唯一通道；想要跨外部包绑定行为只能走结构化匹配。

## 7. 内置 `ToString` 与 `#Builtin`

### 7.1 `base.yux` 中的声明

由 [#D.1] 决议：`ToString` 写在 `sdk/yux/src/yux/core/base.yux`，**不标** `#DraftLike`：

```yux
draft ToString {
  fn to_string() String
}
```

各内置类型（`i8`–`u64` / `f32` / `f64` / `bool` / `String` 等）在 `base.yux` 内显式 `:` 实现，方法体走 `#CompilerInner`（沿用 §11.2.3，**不引入新注解** `#Builtin`）：

```yux
i32 : ToString {
  #CompilerInner
  fn to_string() String       ; 体由编译器在调用点合成（按 §11.2.3）
}
```

### 7.2 不标 `#DraftLike` 的理由

- `ToString` 是 v0.6 字符串模板 `"$expr"` 的"可插值"约束，属**强契约**：用户类型必须显式声明"我支持被插值"才会进入模板路径。
- 若标 `#DraftLike`，任何长得像 `fn to_string() String` 的随机方法都会被自动当作"可插值"，存在 debug-string 被误用作显示文本的风险。
- 与"明确的使用 / 显式优先"基调一致；用户给自己类型加 `ToString` 多写一行 `:` 实现块，接受。

### 7.3 复用 `#CompilerInner`

由 [#D.1] 决议：**不引入** `#Builtin`，复用 §11.2 `#CompilerInner`。形态与现有内置 `to_<type>` / `upgrade` / `same_ref` / `assert_eq` 完全一致；编译器对 `#CompilerInner` 修饰的 `to_string` 方法在调用点合成 IR。

注：MILESTONE / TARGETS 文本中的"`#Builtin` 注解"实指 `#CompilerInner`，措辞由本草案统一。

## 8.附 借用 ↔ owned 链路：`as_ref` 与 `copy_of`

由 [#D.5] 决议，v0.5 把"借用 ↔ owned"两个方向都补齐：

| builtin | 签名 | 方向 | 语义 | 章节 |
|---|---|---|---|---|
| `as_ref` | `as_ref:<T>(box Box<T>) T&` | Box → T& | 零拷贝借用视图；锁住 `box` 在借用期内不可重赋 | §8.3.5.5 |
| `copy_of` | `copy_of:<T>(x T&) T` | T& → T | 显式拷贝；返回栈上 owned 副本，按 T 档位调 RC | §8.附（本节） |

`copy_of` 规则：

- T 受 §8.6.7.1 owned 限制；`x` 是 T 的借用，结果是 T 的栈值。
- 复制语义按 T 档位：
  - **值类型**（`i32` / `bool` / 用户 struct）：memcpy + 字段级 RC retain（含 `Box<U>` 字段按 §7.4.4 retain）。
  - **堆句柄**（`Box<U>` / `Array<U>` / `String` / `Weak<U>`）：句柄复制 + RC retain（同 callee-clean 协议，§8.2）。
- `x` 的借用根（`box` / `local`）在 `copy_of` 调用语句结束后仍可正常使用（临时借用 + 立即释放，按 §8.6.5 临时消费）。
- 与 `as_ref` 不互锁：原 `x` 视图与返回的 owned 副本各自独立，析构按 §8.5 各自走。
- 不接受 `Ptr`、不接受 v0.5 之外的可空形态；turbofish 可省，T 由实参推断。

```yux
fn use_owned<T : D>(x T) { ... }

fn caller(box Box<MyType>) {
  use_owned(copy_of(as_ref(box)))   ; T& → T 显式拷贝后传入 owned 形参
}
```

## 8. `Box<T>` 自动 forward

由 [#D.3] 决议：v0.5 **不为 `Box<U>` 引入独立的 forward 机制**；草案的"Box 自动 forward"在 spec 层等价于 §8.6.7.3 自动解引用 + 方法分发归一，用户视角"`box.method()` 调到 U 上的 D 方法"已自然成立。澄清边界：

1. **forward 实例方法（自然行为）**：`Box<U>` 上 `box.method(args)` 按 §8.6.7.3 解引用为 `method($: U&, args)`，等价于对 U 调用，无新增机制。
2. **`<T : D>` 边界与 Box 的关系**：T 实参遵循 §8.6.7.1（必须 owned）。
   - 把 `Box<U>` 传给 `fn show<T : D>(x T)` → T 实例化为 `Box<U>`，要求 **`Box<U>` 自身**实现 D。U 实现 D 不会自动让 `Box<U>` 实现 D。
   - 把 `as_ref(box)` 传给 `fn show<T : D>(x T&)` → T 实例化为 U，要求 U 实现 D。这是 §8.6.7.4 已有路径，不是新规则。
   - 当前借用链是单向的：`Box<U> → as_ref → U&`；**没有** `T& → T` 的反向降级（也不应有，借用不能升级为 owned）。
3. **不 forward 关联函数**：v0.5 draft 体内只有实例方法签名（[#A.1]），不存在关联函数概念。
4. **不 forward 给其它堆句柄**：`Array<T>` / `String` / `Weak<T>` 不参与"自动解引用调元素方法"，与 §8.4 / §8.5 / §9 一致。
5. **forward 不绕过 orphan**：`Box<U>` 能 forward 的方法集 = U 已经实现的 D；[#C.5] 限制不被 Box 削弱。

## 9. 不在范围

由 [#E.1] 决议，v0.5 **明确不做**：

1. **`dyn Draft` / 运行时多态**（[#B.3]，纯单态化；mangling 留位但不实现）
2. **draft 默认方法体**（[#A.1]）
3. **draft 体内方法本地泛型**（[#C.1]；draft 自身可泛型，[#B.1 修订]）
4. **关联类型 / 关联常量**
5. **跨外部包为外部类型实现外部 draft（orphan）**（[#C.5]）
6. **操作符 draft**（`Add` / `Eq` / `Index` 等语法糖绑定；留 v0.7+）
7. **运行时反射 / `is` / `as` 类型测试**
8. **draft 继承 / super-trait**（需"D2 蕴含 D1"时直接在边界写 `<T : D1 + D2>`）
9. **协变 / 逆变返回或参数**（[#C.1] 签名等价不变）
10. **draft 上独立的可见性修饰**（沿用 §10 `_` 前缀私有）
11. **`<T : D>` 在调用点 `f:<T>(x)` 处回写边界**（[#B.1]，调用点只填实参类型）
12. **`#DraftLike` 部分匹配 / 子集匹配**（[#C.1] 必须穷尽匹配 D 全部签名）

注：`where` 子句、`T : D1 | D2` or 约束已在 [#B.1] 直接说明不引入，不在本节重复。

## 10. 迁移面

> CURRENT.md Phase 2–4 从本节派生。每条标"决议依据"。

### 10.1 编译器（`src/`）

- **AST 层**（`src/node/`）
  - 新增 `draft_node.{cpp,h}`：draft 声明（含 `genericDef?` + `fnSig*`）。 [#A.1] / [#B.1 修订]
  - `struct_node` 实现块带 draft 列表（普通块 vs draft 块用 `draftRefs` 字段区分；穷尽性在语义阶段校验）。 [#A.2]
  - `type_node` / `genericDef`：携带可选 `draftBound*` 列表。 [#B.1] / [#B.1 修订]
- **语义层**（`src/compiler.cpp` / 新建 `draft_resolver.{cpp,h}`）
  - draft 注册表（按完全限定名）+ `#DraftLike` 标志位。 [#A.3] / [#C.1]
  - 显式实现登记（`(TypePath, DraftPath) → impl block`），含穷尽性 + 不多余检查。 [#A.2]
  - 结构化匹配：按需查询（`(TypePath, DraftPath, isDraftLike)`）；空签名集 → 总是真（自然涵盖 `Any`）。 [#C.1] / [#D.4]
  - 同包显隐冲突 / orphan 校验。 [#C.2] / [#C.5]
- **泛型 / 单态化**（`src/compiler.cpp` 现有泛型路径）
  - 实例化期校验 `<T : D>` 边界；mangle 名 `<TypePath>__<DraftPath>__<method>`。 [#B.3]
  - 与 §8.6.7.3 receiver 归一复用，无独立 Box forward。 [#B.2] / [#D.3]
- **builtin**（`src/compiler.cpp` baked 表）
  - 新增 `copy_of:<T>(x T&) T`：值类型 memcpy + 字段级 retain；堆句柄复制 + retain（按 §8.2 callee-clean）。 [#D.5]
  - 现有 `as_ref` 不变。
- **诊断**（`src/compiler.cpp` 错误发出 + 文档同步附录 D）
  - 错误码段 `E1100`：未实现 / 显隐冲突 / `#DraftLike` 误用（4 类）/ 实现穷尽性（缺/多）/ orphan 违反 / 多包同名（按 §10 名字解析报错） 等。 [#C.4]

### 10.2 SDK / runtime（`sdk/`）

- `sdk/yux/src/yux/core/base.yux`
  - 新增 `draft Any { }`（标 `#DraftLike`）。 [#D.4]
  - 新增 `draft ToString { fn to_string() String }`（不标 `#DraftLike`）。 [#D.1]
  - 各内置类型实现块：`i8 : ToString { #CompilerInner fn to_string() String }` 等（覆盖 `i8`–`u64` / `f32` / `f64` / `bool` / `String`）。 [#D.1]
  - 新增 `copy_of` 占位签名（`#CompilerInner`），与 `as_ref` 同处。 [#D.5]
- 不改 runtime helper 集；调用约定与 §8.5 完全兼容。

### 10.3 语法（`src/yux.g4`）

> **不要先动**。本节列出"待改产生式"，按 CLAUDE.md 项目约束，定型后与用户确认再改。

- 新增顶层 `draftDecl`：`'draft' genericDef? ID '{' fnSig* '}'`。 [#A.1] / [#B.1 修订]
- 实现块在现有 `structImpl` 基础上扩展：`structImpl ::= ID (':' draftBound ('+' draftBound)*)? '{' members '}'`。 [#A.2]
- `genericDef` 槽位扩展：`type` 槽位包外 `(':' draftBound ('+' draftBound)*)?` 后缀；`draftBound ::= modulePath? ID genericDef?`。 [#B.1] / [#B.1 修订]
- 不引入 `where` / `dyn` / `|` token。 [#B.1] / [#B.3]
- 附录 A 新增保留字 `draft`。

### 10.4 测试（`tests/`、`sdk/yux/`）

- **行为用例**（`sdk/yux/src/yux/core/draft_*.test.yux`，由 `yux test` 跑）
  - `draft_explicit_basic.test.yux`：`Counter : ToString` 显式实现 + 调用
  - `draft_like_match.test.yux`：未 `:` 但方法名匹配的类型走 `#DraftLike`
  - `draft_bound_call.test.yux`：`<T : ToString + Hash>` 边界 + 单态化分发
  - `draft_any.test.yux`：`<T : Any>` 接任意类型
  - `draft_box_forward.test.yux`：`box.to_string()` 调到 payload 实现
  - `draft_copy_of.test.yux`：`copy_of(as_ref(box))` 链路 + RC 守恒
  - `draft_generic_draft.test.yux`：`draft To<T> { fn to() T }` + `Counter : To<String>` / `Counter : To<i32>` 同时存在
- **诊断用例**（`tests/cases/diag_draft_*.{yux,expected_err}`，由 `xmake test -g yux/diag` 跑）
  - `diag_draft_unimpl.yux`：`<T : D>` 接未实现 D 的类型 → `E11xx`
  - `diag_draft_explicit_implicit_clash.yux`：同包普通块 + draft 块同签名 → `E11xx`（[#C.2] 情形 A）
  - `diag_draft_orphan.yux`：跨外部包实现外部 draft → `E11xx`（[#C.5]）
  - `diag_draft_like_misplace.yux`：`#DraftLike` 标在 fn / struct → `E11xx`（[#C.4] 第 4 条）
  - `diag_draft_impl_block_extra.yux`：实现块多余非 draft 方法 → `E11xx`（[#A.2]）
  - `diag_draft_impl_block_missing.yux`：实现块缺方法 → `E11xx`（[#A.2]）
  - `diag_draft_method_local_generic.yux`：draft 体内 `fn map<U>(...)` → `E11xx`（[#C.1]）
  - `diag_draft_ref_impl.yux`：`T& : D { ... }` → `E11xx`（[#B.2] Q5）

### 10.5 规范文档（`docs/spec/`）

- **§6.4** 函数 / 泛型：在 §6.4.1.2 把"`v0.5 引入 draft 后启用` trait bounds"改为正文，引用新 §12。
- **§7** 结构体实现块：在 §7.5 增子节"`Type : D1 + D2 { ... }` draft 实现块"，含穷尽性 + 不多余 + orphan 限制。 [#A.2] / [#C.5]
- **§8.3.5 / §8.6** 借用：追加 `copy_of` 章（与 `as_ref` 并列）。 [#D.5]
- **§8.6.7** 借用与边界：追加交叉引用到新 §12，规则本身不改。 [#B.2]
- **§9.10**（如存在）/ §11：builtin 列表追加 `copy_of`；`#CompilerInner` 用例追加 `to_string` / `copy_of`。 [#D.1] / [#D.5]
- **§11.4** 注解：新增 `#DraftLike` 条款（§11.X，对应 [#C.4] 4 条措辞）。
- **新增 §12 `draft`**（独立章节）：把草案 §3 / §4 / §5 / §6 / §7 / §8 内容固化为 §12.1–§12.7。
- **附录 A**：保留字加 `draft`。
- **附录 B**：同步 §10.3 待改产生式（仅文本，不动 `yux.g4` 直至用户确认）。
- **附录 C**：术语加 `draft` / `#DraftLike` / 结构化匹配 / 单态化分发。
- **附录 D**：`E1100` 段错误码定义。
- **`CHANGELOG.md`** 顶部追加："v0.5 引入 draft + `#DraftLike` + `<T : D>` 边界 + `copy_of`"。

### 10.6 用户教程（`docs/`）

spec 落地后写：

- `docs/接口与约束.md`（暂名）：面向用户的 `draft` 教程，含 `Counter : ToString` 案例 + `<T : Display>` 边界 + `Any` / `copy_of` 用例。

---

## 决议日志

按讨论顺序追加，标 `[#编号]`。每次反复或修订也追加新条目，不要覆盖。

编号约定：A = 基础语法，B = 单态化与边界，C = `#DraftLike` 行为，D = 衍生（内置 / Box forward），E = 不在范围。

- **[#D.5]** 新增 builtin **`copy_of:<T>(x T&) T`**：补齐 `T& → T` 链路（与 `as_ref` 反向对称）。值类型 memcpy + 字段级 retain；堆句柄复制 + retain。命名沿用 yux 现有 `_of` / `_ref` builtin 格律（避免 `deref_copy` 的 deref 概念，避免 `clone` 的 Rust trait 包袱）。
  - 影响章节：草案 §8.附，未来 spec §8.3.5 / §8.6 / §9.10（builtin 列表）。

- **[#D.4]** 在 `base.yux` 内置 `#DraftLike draft Any { }`（空体）；由 [#C.1] 穷尽匹配 + ∅ 签名集，**所有类型自动满足 `Any`**，无需新规则。v0.5 仅作约束最弱形态；不引入"`Any` + 反射"组合。
  - 影响章节：草案 §2 / §7（base.yux 内置清单），未来 spec §11 / §12。

- **[#A.1 微补]** draft 体**允许零方法签名**（空 draft 合法）；原 [#A.1] 主结论（无默认体）不变。前置于 [#D.4]。

- **[#E.1]** v0.5 不在范围（详见 §9）：`dyn Draft` / 默认方法体 / 方法本地泛型 / 关联类型 / orphan / 操作符 draft / 反射 / draft 继承 / 协变 / 独立可见性修饰 / 调用点回写边界 / `#DraftLike` 部分匹配。`where` 子句、or 约束已在 [#B.1] 说明，不在 §9 重复列出。

- **[#D.3]** `Box<T>` 自动 forward：v0.5 **无独立机制**，等价于 §8.6.7.3 自动解引用 + 方法分发归一。澄清：
  - `<T : D>` 接 `Box<U>` 实参 → T = `Box<U>`，要求 Box<U> 自身实现 D；不会自动跳到 U（借用链单向：`Box<U> → as_ref → U&`，**无** `T& → T` 反向降级）。
  - 关联函数不 forward（v0.5 draft 无此概念）；其它堆句柄（Array / String / Weak）不参与；不绕过 [#C.5] orphan。
  - 影响章节：草案 §8，未来 spec §8.6.7（追加交叉引用）/ §12；不改 §8.3.5。

- **[#D.1]** 内置 `ToString` 写在 `base.yux`，**不标** `#DraftLike`（强契约，避免 debug-string 误命中字符串模板）；各内置类型显式 `i32 : ToString { ... }`；方法体复用 §11.2 `#CompilerInner`，**不引入** `#Builtin` 注解（MILESTONE / TARGETS 中的"`#Builtin`"措辞按 `#CompilerInner` 统一）。
  - 影响章节：草案 §7，未来 spec §11.2.3（追加 `to_string` 列入内置 `to_<type>`）/ §12 / `base.yux`。

- **[#C.4]** `#DraftLike` 滥用边界条款（拟入 §12.X.Y）：默认严格（未标 = 仅显式实现）；显式开放 = `#DraftLike` 仅在 draft 声明上；语义动机要求 = 风格指引不上诊断；硬性误用诊断 = 标错位置 / 标在带默认体 draft / 标在带方法本地泛型 draft。错误码暂占 `E1100` 段，Phase 2 与附录 D 一起定。
  - 影响章节：草案 §5.5，未来 spec §11 / §12 / 附录 D。

- **[#C.3]** 多包同名 draft 同时命中：**不需要新规则**。每个 `<T : D>` 按 D 的完全限定名独立判断结构化匹配；import 名字冲突按 §10 处理；普通调用 `x.m()` 不经 draft 分发。DRAFT.md 原文"匹配多包同名 Draft 报错"在本模型下退化为名字解析问题，不在 draft 章节出特殊规则。
  - 影响章节：草案 §5.4，未来 spec §10（名字解析）/ §12。

- **[#C.2]** 显隐冲突优先级：
  - 同包同签名共存（普通块 + draft 实现块）→ **error**（理由：`counter.show()` 调用点会出现普通方法 vs draft 方法的分发歧义，不允许写出来）；
  - 跨包结构化匹配（`#DraftLike` D 在外部包、类型在本包或第三方包，仅有同签名普通方法）→ **自动命中，无警告**；
  - 跨包显隐共存（情形 C）由 [#C.5] 禁 orphan 而不存在。
  - 影响章节：草案 §5.3，未来 spec §11 / §12 / 附录 D（错误码）。

- **[#C.5]** 禁止 orphan：`Type : D { ... }` 只能写在 `Type` 所在包或 `D` 所在包；否则编译期报错。`#DraftLike` 是跨包绑定行为的唯一通道。
  - 正方：避免外部库被第三方污染实现；让 [#C.2] 情形 C 不存在；与 rust 一致，工程化经验已验证。
  - 反方：失去"为外部类型实现外部 draft"的灵活性；接受（这正是 `#DraftLike` 设计目的）。
  - 影响章节：草案 §6 / §10，未来 spec §10（模块系统）/ §12 / 附录 D。

- **[#C.1]** `#DraftLike` 签名等价：方法名 / 参数个数 / 逐一参数类型 / 返回类型必须完全相同；参数名、receiver 形态、注解不参与；`T` vs `T&` 不互通；**draft 体内单个 `fn` 不得再引入泛型形参**（draft 自身可带泛型形参，见 [#B.1 修订]）；按需匹配，不预扫全包。
  - 正方：与显式实现的等价规则一致；按需匹配复用现有 per-call 分发模型；禁止方法本地泛型避免高阶 + 边界传递的复杂度。
  - 反方：未来想要协变返回 / 关联类型时再开；接受。
  - 影响章节：草案 §5.2，未来 spec §11 / §12。

- **[#B.1 修订]** 相对 [#B.1]：补充 draft 自身允许带泛型形参（如 `draft To<T> { fn to() T }`），即 `draftBound` 可携带 `genericDef`。修订后的产生式：
  ```
  draftBound ::= modulePath? ID genericDef?      ; 例：To<i32> / pkg.Map<K, V>
  ```
  draft 声明侧形态：`draft Name genericDef? { fnSig* }`；实现块 `Counter : To<String> { ... }` 把不同 T 实参视为**不同实现**，可同时存在。draft 体内 `fn` 仍**不允许**引入新泛型形参（详见 [#C.1]）。
  - 不影响 [#B.1] 其它结论（仍内联、无 `where`）。

- **[#B.3]** 调用 ABI：v0.5 **纯单态化静态分发**，无 vtable、无隐式签名表参数；方法 mangling 与签名（receiver + args）按"未来可直接作为 vtable 表项"约束，**为 `dyn Draft` 留位置**（v0.5 不实现）。
  - 正方：与现有泛型 codegen 路径一致；零运行时开销；与"明确的使用 / 知道每一步"基调一致；预留 vtable 改造路径，避免未来推倒重来。
  - 反方：每 T 一份代码，二进制变大；不能跨编译单元裸导出泛型函数；接受（与现有 `<T>` 一致）。
  - 影响章节：草案 §4.3 / §10，未来 spec §6.4 / §12 / 附录 D（诊断）；未触及 §8.6.7。

- **[#B.2]** 边界与 §8.6.7 衔接：
  - T 仍须 owned（沿用 §8.6.7.1，`T&` 不能作 T 实参）；
  - D 实现绑在 owned 类型 T 上，`T&` 自动可调（沿用 §8.6.7.3 方法分发归一）；
  - `Box<U>` 调 D 方法走自动解引用 + 归一，**无需新增 forward 机制**（草案 §8 与 [#D.3] 直接以此结论收口）；
  - `Array<T>` / `String` / `Weak<T>` 可作 T 实参；`D` 实现优先走 `base.yux` + `#Builtin`，用户同包再次实现按 [#C.2] 报错；
  - `T&` 上**不允许**写 `T& : D { ... }` 实现块。
  - 影响章节：草案 §4.2 / §8，未来 spec §8.6.7（追加交叉引用，不改既有规则）/ §6.4 / §12。

- **[#B.1]** 泛型边界**内联**写在 `genericDef`，形态 `T : D1 + D2`，多形参各自带边界并以 `,` 分隔；**不引入 `where` 子句**；调用点 `genericDef` 不允许写边界（仍只填实参类型）。
  - 正方：与现有 `genericDef` 最小冲突；与"少特例"基调一致；签名单行可读，未发生需要 `where` 才能容下的场景。
  - 反方：未来复杂边界要回头加 `where`；接受，加 `where` 是兼容扩展。
  - 影响章节：草案 §4.1，未来 spec §6.4 / §7 / 附录 B（产生式）。

- **[#A.3]** `#DraftLike` **只能标在 draft 声明上**，使用处不允许再加；是否结构化匹配为 draft 的语义属性，不可在调用点变更。
  - 正方：单一行为，便于静态推理；与"显式 > 隐式 + 默认严格"一致；库作者掌握封闭/开放选择。
  - 反方：第三方未标的 draft 无法结构化匹配；接受 —— 要绕需显式 `:` 实现。
  - 影响章节：草案 §5.1，未来 spec §11（注解）/ §12。

- **[#A.2]** 实现块**仅允许合并块** `Type : D1 + D2 { ... }`，块内方法集**必须等于** 列出 draft 的签名集（缺/多均报错）；普通方法走 `Type { ... }` 块；同一 `Type : D` 不允许出现多次。
  - 正方：与「统一用法 / 少特例」基调一致；语义单一，IDE / 诊断容易实现；避免分块带来"重复实现 / 顺序影响 / 跨块辅助方法归属"等附加规则。
  - 反方：rust 分块灵活度更高；跨包追加场景需求未明，待 [#C.5] 决定后回看。
  - 兼容性：从"仅合并"放宽到"允许分块"是兼容变更，反向不兼容，先严。
  - 影响章节：草案 §3.2，未来 spec §7（结构体实现块）/ §12。

- **[#A.1]** draft 体内**只允许方法签名，不允许默认方法体**。
  - 正方：v0.5 退出标准为"最小可用"；默认体会引入 draft 内 `$.其它签名` 的分发上下文，与 §8.6.7 边界匹配交叉，复杂度陡增；`#DraftLike` 命中"未实现该方法"是否回退默认体也要新规则。
  - 反方：rust trait 默认体复用强；牺牲一定便利。
  - 兼容性：从"不允许"放宽为"允许"是兼容变更，留 v0.7+ 再加；反向不兼容，故先严。
  - 影响章节：草案 §3.1，未来 spec §6 / §12（draft 专章）。

---

## 定型与归宿

草案定型后按以下步骤拆分迁入正式文档：

1. §10.5 列出的每个 spec 章节逐条改写，引用本草案条目编号（如 [#A.2]）保留可追溯性。
2. `docs/spec/CHANGELOG.md` 顶部追加一条，摘要 + 影响章节，日期为合并日。
3. 附录 A / B / C 按 §10 同步；如需动 `src/yux.g4`，**先与用户确认**再改，按 CLAUDE.md 项目约束。
4. `CURRENT.md` 的 Phase 列表从 §10 派生，每完成一个阶段就地更新；全部完成后归档到 `docs/dev/draft-impl-log.md`（剔除人名/路径/行号/测试计数）。
5. 处置本文件：要么删除，要么在头部加一句「已落地，见 §N.M」并保留为历史档。

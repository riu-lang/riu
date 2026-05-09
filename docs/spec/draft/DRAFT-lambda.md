# 草案：yux Lambda 与函数类型

状态：**已落地（spec 层）**，2026-05-08。回写章节：§3.11 / §4.8.4 / §4.11 / §6.1.1.6 / §6.5.5 / §6.6.2.1 / §8.1.1.1 / §8.1.2.5 / §8.7.6 / §8.9 / §8.10。CHANGELOG 同日条目已加。

> 编译器实现 / `src/yux*.g4` 改动 / `tests/cases/lambda_*` 用例 / 用户教程 `docs/Lambda与闭包.md` 按 §10 迁移面分阶段后续落，本草案保留为决议史档。

日期：2026-05-08。
作用：把"函数作为一等值（类型 + 字面量 + 闭包）"这一组决策固化为单一规范，作为修改 `docs/spec/03-类型系统.md`、`docs/spec/04-表达式.md`、`docs/spec/06-函数.md`、`docs/spec/08-所有权与引用.md` 与 `CURRENT-lambda.md` 实施计划的依据。

> 本草案的所有规则均已逐条决议（见末尾"决议日志"）。后续若有反复，请在日志里追加修订记录，不要直接覆盖正文。

涉及章节（预估）：§3、§4、§6、§8、附录 A / B / C。

---

## 1. 目标

- 函数作为一等值：类型字面量 + lambda 字面量 + 闭包，三件一并落地。
- 与 `docs/spec/06-函数.md` 中 fn 声明形态对齐；零新关键字。
- 与 §3.9 类型别名透明衔接，无新机制：`Callback = fn(s String)bool`。
- 与 §8 RC / 借用模型正交：函数值的所有权协议复用既有档位（值类型 / 堆句柄之一，Phase 3 决）。
- 显式记号 `=>` 标记 lambda；不引入 `it` 隐式名；不引入"块表达式"专属形态。
- v1 必含闭包（v2 错误模型 try/catch handler 必须能访问外层变量；同步解禁 §8.1.2.5 中"闭包不在 v1 范围"条目）。

## 2. 全景

| 名称 | 写法 | 语义 | 决议 Phase |
|---|---|---|---|
| 函数类型字面量 | `fn(a T, b U) R` | 一等类型，结构等同 | **Phase 1（本草案 §3）** |
| 函数类型别名 | `Callback = fn()bool` | 透明 type alias，复用 §3.9 | Phase 1 |
| 表达式 lambda（多参） | `(a T, b T) => expr` | 单表达式体 | Phase 2（§4） |
| 表达式 lambda（单参省括号） | `x => expr` | 类型由上下文推断 | Phase 2 |
| 块 lambda（≥1 参） | `{ a T, b T => body }` | `=>` 分隔参列与体 | Phase 2 |
| 块 lambda（0 参） | `{ body }` | **禁写** `=>` | Phase 2 |
| 函数值 RC 协议 | Block layout 待 §5 决 | 与 §8.5 callee-clean 对接 | Phase 3（§5） |
| 闭包 | 同 lambda + 自由变量捕获 | v1 必需 | Phase 4（§6） |
| FFI / extern 上的 fn 指针 | extern 边界限零捕获 | 与 C 函数指针 ABI 对齐 | Phase 5（§7） |

要点：

- **函数类型按结构等同判等**（与 §3.8 元组同档，与 struct 名义判等不同）。
- 函数类型字面量的形态严格等同 fn 声明的 `(' fnParams? ')' retType?` 段，仅去函数名。
- 函数类型字面量带的参数名**仅作文档**，不参与类型相等。
- 0 参 lambda **禁写** `=>`；`=>` ⇔ "≥1 参 lambda"。

---

## 3. 函数类型 `fn(A) R`（Phase 1 主体）

### 3.1 出现位置（白名单）

作为 `type` 的一个产生式选项出现，可在以下位置使用：

- 局部变量类型：`var f fn()i32 = ...`
- 函数形参 / 返回值（任意嵌套）：`fn map(f fn(i32)i32)`、`fn make_counter() fn()i32`
- 类型别名右侧（§3.9）：`Callback = fn(s String)bool`
- 元组元素：`(fn()i32, i32)`
- ~~`T&` 借用：`(fn(A) R)&`~~（**v1 不支持**：g4 缺"括号包类型"规则，且 `typeFnWithRef` 末尾 `&` 在有非借用 R 时被 retType 贪心吃掉，无语法表达手段；§3.6 复述）

字段位置 / `Box<fn(...)>` / `Array<fn(...)>` / `Weak<fn(...)>` 等容器内层是否合法 → 决于 Phase 3 档位决议（值类型 vs 堆句柄）；本节不收口。

### 3.2 形态

形态等同函数声明 `fnHeader` 的 `(' fnParams? ')' retType?` 段，仅去函数名，**且 `fn` / 可选 `?` / `(` 之间不带空格**，`)` 与 retType 之间也不带空格：

```
fnType       ::= 'fn' '?'? '(' fnTypeParams? ')' retType?
fnTypeParams ::= fnTypeParam (',' fnTypeParam)* ','?
fnTypeParam  ::= ID? type             ; 名可省；省名时直接写类型
```

可选的 `?` 紧贴 `fn`（**[#24]** nullable 函数值紧凑形）：`fn?(T)R` 表示"整个函数值可空"。这与 retType 上的 `?` 视觉分立（详见 §3.7）。

**两条紧凑特例**（与 `docs/基础语法.md` 通用规则相反，仅 `fnType` 产生式特例）：

- [#16] `fn` 关键字后**不带**空格（违反"关键字后必须有空格"）。
- [#23] `)` 与 retType 之间**不带**空格（违反 fn 声明的 `) RetT` 风格）。

紧凑写作 `fn(T)R`，整体视觉成为单一单元。理由：fn 类型典型出现在嵌套位置（fn 类型作另一 fn 的形参 / 返回值 / 容器内层），紧凑版避免 `i32) i32) i32` 这类返回类型与外层 `)` 视觉粘连。fn 声明 `fn add(a i32, b i32) i32 = ...` 仍按通用规则带空格 —— 因声明在顶层语句位置不嵌套，可读性优先。

g4 实际产生式由 Phase 1 落地时同步（按 CLAUDE.md 须用户拍板，本草案仅给概念形）。

### 3.3 写法示例

```yux
; 0 参 / void
fn()
fn()i32

; 单 / 多参（带名仅作文档）
fn(i32)i32
fn(a i32)i32
fn(a, b i32)i32                   ; 参数组糖（与 §6 一致）
fn(s String, n i32)bool
fn(b Box<T>, r T&)T?              ; T& 形参合法

; 嵌套 / 高阶（紧凑形不再视觉粘连）
fn op(f fn(a, b i32)i32) i32 = f(1, 2)        ; f 为函数值形参；外层 op 是 fn 声明，按通用规则空格
fn make_adder(n i32) fn(i32)i32 { ... }       ; 返回函数值

; 与 alias / 容器 / nullable 组合
Callback = fn(s String)bool
var p (fn()i32, i32) = ...        ; 元组成员

; nullable —— [#24] 紧凑形 fn?
var f fn?()                       ; nullable 0 参 void fn 值
var g fn?()i32                    ; nullable fn 值，返回 i32
var h fn(s String)i32?            ; ⚠️ retType nullable：fn 值不可空，返回 i32?
var k fn?(s String)i32?           ; 双层 nullable：fn 值可空，且返回 i32?
; (fn()i32)? 形态禁；统一紧凑形 fn?()i32
```

**两类 `?` 视觉分立**（[#24]）：

| 写法 | 含义 |
|---|---|
| `fn?(T)R` | 整个函数值可空；`?` 紧贴 `fn` |
| `fn(T)R?` | 函数值不可空，返回 `R?`；`?` 紧贴 retType |
| `fn?(T)R?` | 双层：函数值可空，且返回 `R?` |
| `(fn(T)R)?` | ❌ 禁；语义等价 `fn?(T)R`，但风格分裂；规范统一到紧凑形 |

紧凑规则只约束 `fn?` / `)` 两处零空格；retType 后的 `?` 仍按 §3.6 typeNullable 通用规则。`fn?` 在词法层是 `fn` + `?` 两 token，`fnType` 产生式专门吸收，不污染其它表达式位置。`(fn(T)R)?` 形态禁——`fnType` 不进入 §3.6 `typeNullable` 的 `?` 后缀候选，理由是两形并存损害可读性，规范统一到紧凑形。

### 3.4 类型相等

**结构等同**（与 §3.8 元组同档）：

`fn(P1, ..., Pn) R` ≡ `fn(Q1, ..., Qm) S` 当且仅当：

1. arity 相等：`n = m`；
2. 各位置参数类型 §3.4.1 等同：`Pᵢ ≡ Qᵢ`（递归判等，含别名透明替换）；
3. 返回类型 §3.4.1 等同：`R ≡ S`（含 void 缺省）；
4. **参数名不参与判等**（仅文档作用）；
5. 参数组糖与展开形等同：`fn(a, b i32) R` ≡ `fn(a i32, b i32) R` ≡ `fn(i32, i32) R`。

Spec 落地点：§3.4.1 在元组结构等同（§3.8.1.3）旁加一档"函数类型按结构等同"。

### 3.5 与 §3.9 类型别名协作

直接复用 §3.9 透明替换，**无新规则**：

```yux
Callback = fn(s String)bool
Predicate<T> = fn(x T)bool       ; 泛型别名（§3.9.2）

var p Callback = ...              ; 与 fn(s String)bool 等同（§3.9.1.2）
fn each<T>(arr Array<T>, p Predicate<T>) { ... }
```

§3.9.3.4（别名右侧不得含 `T&`）对函数类型别名"返回类型 `T&`"的处理由 §8 主导（§3.6）；本草案不重复定义。

### 3.6 与借用 `T&` / `Ref<T>`

**形参位置 `T&`**（§3.2.3.1）：合法。`fn(b Box<T>, r T&)T?` 与 fn 声明同形态。

**返回值 `T&`**：沿用 §3.2.3.2 + §8.6.10「返回引用的溯源约束」。函数类型字面量本身不重新定义溯源规则；写得出 `fn(r T&)T&` 时，使用点（lambda 字面量 / fn 声明）须满足 §8.6.10 的"允许源集"与"v1 单源"约束（§8.6.10.2 / §8.6.10.3）。lambda 字面量的 `$` 不存在（除非 Phase 4 决议允许 lambda 内 `$` 透传），故 lambda 的允许源集仅靠"`T&` 形参"获得。

**`Ref<T>` 自动取引用**（§9）：调用函数值时，若形参为 `Ref<T>` 而实参为 `T`，自动取引用规则照旧，无差异。

**借用一个函数值** `(fn(A) R)&`：**v1 不支持**。g4 当前没有"括号包类型"规则，`typeFnWithRef` 末尾的 `SymbolAnd?` 在有非借用 R 时被 retType=typeWithRef 的贪心 `&` 吃掉（`fn(i32)i32&` → 返回 `i32&`，外层 `&` 永远轮不上），无语法表达手段。Phase 3d 决议直接弃，不改 g4。如未来需求出现，需先在 g4 加 paren-type 规则（或重新设计借用语法）再回填 §5.5 表。

### 3.7 与 nullable / 容器 / FFI

- `fn?(...)R`：nullable 函数值（[#24] 紧凑形，§3.3），✅ 合法（[#17]，§5.5）。`(fn(...)R)?` ❌ 禁（统一紧凑形，§3.3）。
- `Array<fn(...)>` / `Box<fn(...)>` / 字段位置：✅ 合法（值类型 + 字段级 RC，§5.5）。
- `Weak<fn(...)>`：❌ 禁（函数值不是堆句柄无 RC 头，§5.5）。
- FFI 边界（`extern fn`、`Ptr` 转换）：推到 Phase 5（§7）+ 错误模型 v1 [#7] 合并讨论。

### 3.8 ABI（Phase 3 已落地，2026-05-09）

仅占位记号：函数类型在调用点按 callee-clean §8.5 协议传值；零捕获 vs 含捕获是否走同一 ABI、是否引入 fat-pointer / Box-wrapped 形态，留 Phase 3 决。

---

## 4. Lambda 字面量（Phase 2）

### 4.0 形态总览

```yux
; --- 表达式形（单表达式体；表达式含 if / match，[#15]） ---
x => expr                          ; 裸参：类型 + 返回类型均从上下文推断
(x i32) => expr                    ; 括参 + 无返回标注 → void
(x i32) i32 => expr                ; 括参 + 显式返回类型
(a, b) => expr                     ; 多参裸列：上下文推断；仅在表达式形且无返回标注时合法
(a, b) i32 => expr                 ; 多参括参 + 显式返回类型
(a i32, b i32) i32 => expr         ; 全显式

; --- 块形（语句序列体） ---
{ x => body }                      ; 裸单参：推断
{ a, b => body }                   ; 裸多参：推断
{ a i32, b i32 => body }           ; 全显式（参数组糖同 §6：{ a, b i32 => body }）
{ (a, b) i32 => body }             ; 块形也可用括参 + 显式返回类型
{ body }                           ; 0 参块；禁写 =>
```

### 4.1 形参类型推断（[#10]）

lambda 形参形态：`name` 或 `name type`。**类型可省时由"期望函数类型"反推**：

```yux
fn op(fun fn(a, b i32)i32) i32 = fun(1, 2)

; 期望类型 fn(a, b i32)i32 → a / b 推断为 i32
op({ a, b =>
    a + b
})
```

类型上下文来源（与 §3.4 / §3.6 现有推断风格一致）：

- 赋值的变量类型：`var f fn(i32)i32 = x => x + 1` ✅
- 形参传入的类型：上例的 `op({...})` ✅
- 返回值类型：`fn make() fn(i32)i32 { ret (x i32) i32 => x + 1 }` ✅（裸参 `x => x + 1` 在 ret 位置类型上下文也成立）
- 无上下文：`var f = x => x + 1` ❌ 编译错（与 §3.6.1.3 `null` 字面量同构）

可混写：`{ a, b i32 => ... }` 合法（与 §6 fn 声明的参数组糖正交）。

### 4.2 返回类型规则（[#12][#14]）

| 形态 | 参数列表形式 | 返回类型 | 行为 |
|---|---|---|---|
| `x => expr` | 裸（无括号） | 从上下文推断 | 推断成功即合法；否则 [E?]推断失败 |
| `(x i32) => expr` | 括号（任意 arity） | **默认 void**（不写就是 void） | body `expr` 被视作语句位置丢弃 |
| `(x i32) i32 => expr` | 括号 | 显式 `i32` | body 求值结果 § 类型相等 i32 |
| `{ a, b => body }` | 裸 | 从上下文推断 | 同表达式形 |
| `{ a, b i32 => body }` | 裸 | 从上下文推断 | 类型混写允许 |
| `{ (a, b) i32 => body }` | 括号 + 显式返回 | 显式 `i32` | 同表达式形 |
| `{ (a, b) => body }` | 括号 + 无返回 | void | 同表达式形 |
| `{ body }` | 0 参（`=>` 禁写） | 从上下文推断 | |

**为何裸 vs 括号差异化**：括号形态是"完整声明形"（自描述）—— 不写返回类型就视作刻意 void；裸形态是"轻量推断形"，留空让上下文驱动。这避免了"括号但缺信息"的歧义。

### 4.3 实参 / 字段值位置：参数必须括起来（[#13]）

当 lambda 出现在**调用实参 / 字段赋值 / 别名右侧**等"已有外层 `(...)`"位置时，lambda 自身的参数列表**必须括起来**，否则与 fn 声明 / fn 类型字面量歧义：

```yux
; ✅ 合法
fn a(p (a, b) i32 => i32 + i32) ...   ; （示意，实际不会写这种）
op((a, b) i32 => a + b)
op({ (a, b) i32 => a + b })

; ❌ 与 fn 类型字面量歧义
op(a, b => a + b)                      ; 看起来像 fn(a, b => a + b)
fn a(a, b i32, fn())                   ; 与 fn(f fn(a, b i32)i32) 形态相互混淆
```

裸参 `x => expr` 在实参位置仍合法（单 ID 不与多参 fn 类型歧义），但建议风格上统一括起来。

块形 `{ ... }` 自带定界符，参数列表仍可裸：`op { a, b => a + b }` 无歧义（[#11] 尾随调用糖）。

### 4.4 表达式体可含 `if` / `match`（[#15]）

yux 中 `if` / `match` 是表达式（决议日志见 [#15]）。lambda 表达式形的 `expr` 可直接包含：

```yux
; if 表达式
val classify = (x i32) String => if x > 0 { "pos" } else if x < 0 { "neg" } else { "zero" }

; match 表达式
val tag = (e MyErr) i32 => match e {
    MyErr::Empty   => 0,
    MyErr::Bad(_)  => 1,
}
```

具体 `if` / `match` 的表达式形态（各分支类型如何归一、是否需要 `else` 兜底等）由 §4 / §5 既有条款约束；lambda 不引入新规则。

### 4.5 尾随 lambda 调用糖（[#11]）

原 v2 错误阻塞清单 P3 已折入本草案。

当函数调用的**最后一个位置实参是块形 lambda** 时，该 lambda 可移出 `(...)`：

```yux
; 等价形态对
f(a, b, { x => body })       ; 标准
f(a, b) { x => body }        ; 尾随移出

f({ x => body })             ; 唯一实参的标准
f { x => body }              ; 唯一实参 + 省 (...)
```

规则：

- 仅**块形** lambda（`{ args => body }` / `{ body }`）可作尾随；表达式形 `(x) => expr` 不可（与表达式后缀链冲突）。
- 当尾随 lambda 是唯一实参时，`()` 可整体省略；多实参时保留 `(...)` 包裹其余实参。
- 与 `?` / `??` / `?.` 等表达式后缀相容（lambda 在调用语义层是普通实参，糖只影响 parse）。
- 与 `:<T>` turbofish 相容：`f:<i32>(args) { lambda }` / `f:<i32> { lambda }`。
- 函数签名层无影响：`fn op(fun fn(a, b i32)i32) i32` 与 `fn op2(x i32, fun fn(...)i32) i32` 在调用形态上对称扩展。
- callee 与 `{` 之间空格：与现有 `f(...)` 调用同款（无强制要求；§4.6 留具体规则待 g4 与现有 `callExpr` 对齐）。

实施面：纯 parser 改动，AST 层改写为标准调用形；语义阶段无差异。

### 4.6 Phase 2 待决（剩余）

- `=>` 与现有 token 冲突评估：`=` / `==` / `>=` / `>` 按最长匹配不冲突；附录 A 加新 token。
- 多行块"换行强制"具体：`=>` 后允许立即换行进入语句序列（与 fn 声明 body `{` 后立即换行同款）。倾向：合法，与现有"语句以换行收尾"规则零冲突。
- 尾随调用 `f { ... }` 中 `f` 与 `{` 间空格规则与现有 `callExpr` 对齐。

---

## 5. 函数值 RC / ABI（Phase 3，[#17] C1）

> **状态**：Phase 3 (a/b/c) 已落地（2026-05-09）—— 字段级 RC 接入零捕获骨架、struct fn 字段、`Array<fn>` / `Box<fn>` 容器全部走通，xmake test 65/65 + sdk yux test 259/259 全绿。Phase 3d「`(fn(...) R)&` 借用」**v1 不支持**（详见 §3.6 / §5.5）。Phase 4 闭包未启动，含捕获路径仍为 informative。

### 5.1 档位归属

函数值 `fn(...) R` 是 **§3.1 值类型**（与元组 / enum 同档），但其内部含一个 `Box<...>?` 字段（捕获包），按 **§7.4 字段级 RC** 处理。

不新增"堆句柄"档位（§3.1 / §8.1.1.1 封闭集合保持）；零开销路径与含捕获路径在**类型层完全不可见**，仅运行时表示分两条。

### 5.2 数据布局

每个 `fn(...) R` 类型的值在栈上按以下伪 layout 表示：

```
struct FnValue {
  fn_ptr   : Ptr                ; 8 bytes (LP64)；指向匿名顶层 fn / trampoline
  captures : Box<CapturesT>?    ; 8 bytes；nullable 堆句柄；零捕获 lambda 为 null
}
```

总 size = 16 bytes（LP64）。`fn_ptr` 是裸函数指针，不参与 RC。`captures` 是 §3.6 nullable + §3.3 Box 堆句柄，按既有 §8.2 RC 协议管理。

`CapturesT` 是编译期为每个含捕获 lambda 单独合成的匿名 struct，字段为该 lambda 体内引用的非形参 / 非全局标识符（具体收集规则见 §6 闭包）；零捕获 lambda 不分配 `CapturesT`，`captures = null`。

### 5.3 调用 ABI（永远 fat 形态）

**调用约定统一**：调用 `f(args)` 翻译为：

```
f.fn_ptr(f.captures, args...)
```

`captures` 永远作为**隐式首参**传给 fn_ptr 指向的实体。零捕获 lambda 的 fn_ptr 指向一个忽略首参的实现（编译器合成的 trampoline 或直接生成签名忽略首参的 fn）。这避免了"调用点根据 captures 是否 null 分流"的两种调用约定，保证 `fn(...) R` 在 ABI 层是单一类型。

**与 §8.5 callee-clean 对接**：
- 函数值作实参传递：caller 按值复制 fat-ptr，按 §7.4 字段级 retain（`captures != null` 则 `Box::retain`）；callee 在出口或显式丢弃点按 §7.4 字段级 release。零捕获场景 retain / release 都是 nullable 早返，零开销。
- 函数值作返回值：callee 在 `ret` 处构造 fat-ptr，retain captures（若非空），按 §8.5 既定方向交出所有权。
- 函数值作字段：`struct S { handler fn() }` 合法；S 的析构按 §7.4 处理 handler 字段（release captures）。
- 函数值作元素：`Array<fn(...) R>` / `Box<fn(...) R>` 合法；容器元素 retain / release 走字段级 RC。

### 5.4 RC 操作伪码

```
fn_value_retain(v: FnValue):
  if (v.captures != null) box_retain(v.captures)

fn_value_release(v: FnValue):
  if (v.captures != null) box_release(v.captures)
  ; fn_ptr 不参与

fn_value_destruct(v: FnValue):
  fn_value_release(v)
```

**`Box<CapturesT>` 析构**：按 §8.2.2.3 + §7.4 字段级 release —— `CapturesT` 字段中的堆句柄 / `T&`（如允许，§6 决）按各自类型协议 release / 丢弃。

### 5.5 字段位置 / 容器内层 / nullable

依 5.1 档位归属（值类型 + 字段级 RC），下列形态合法性收口：

| 形态 | 合法性 | 备注 |
|---|---|---|
| `var f fn(...) R = ...` | ✅ 局部变量 | |
| `struct S { handler fn(...) }` | ✅ 字段位置 | 字段级 RC 处理 |
| `Array<fn(...) R>` | ✅ 容器元素 | 元素 size = 16 bytes |
| `Box<fn(...) R>` | ✅ | 双层句柄；语义合法不专门禁 |
| `fn?(...)R` | ✅ Nullable 紧凑形 | [#24]；`(fn(...)R)?` 禁 |
| `Weak<fn(...) R>` | ❌ 禁 | 函数值不是堆句柄，无 RC 头；§8.10 / §3.7 禁忌列表加一条 |
| ~~`(fn(...) R)&`~~ | ✗ v1 不支持 | g4 缺 paren-type 规则，retType 贪心吞 `&`，无语法表达手段；详见 §3.6 |

### 5.6 移动 / 复制 / 借用

- **复制**：按值拷贝 fat-ptr 16 bytes；retain captures（若非空）。与 §7.4 字段级复制语义一致。
- **移动**：通过 §8.5 callee-clean / 移动初始化等"已知所有权转移"路径，可省 retain / release 对消。零捕获场景 captures = null，retain / release 是早返 NOP，移动优化收益不显著但无损。
- ~~**借用** `(fn(...) R)&`~~：**v1 不支持**，详见 §3.6。函数值借用形态在 v1 缺位。

### 5.7 Open Issues

- **F1**：编译器是否对零捕获 lambda 走"fn_ptr 直指 lambda body 且首参签名带未使用 captures Ptr"形态，还是合成统一 trampoline？两者用户不可见；落实施时择一。
- **F2**：多个零捕获 lambda 字面量（同一签名）是否共享同一 fn_ptr / 是否做去重？纯优化，不进规范。
- **F3**：函数值的 `==` 仍按 §8 禁；地址相等不开口子（一致 [#8]）。
- **F4**：`Box<CapturesT>` 在 captures 含 `T&` 时不可堆化（与 §3.2.3.2 字段位置禁 `T&` 同源）—— 这把"捕获 `T&`"的 lambda 强制为"不可逃逸出借用源 scope"。Phase 4 闭包决议时与 §6 合并表述。

---

## 6. 闭包（Phase 4，v1 必需）

闭包是 lambda 字面量的语义升级：lambda 体引用了**非形参 / 非全局**的标识符，编译器自动收集为捕获。v1 落地必含（v2 错误模型 try/catch handler 要访问外层变量），同步**解禁 §8.1.2.5** 中"闭包不在 v1 范围"。

### 6.1 自由变量收集（[#18]）

编译期为每个 lambda 字面量计算自由变量集 `FV(λ)`：

```
FV(λ) = { 标识符 x 在 λ 体内被引用 } \ ( λ 形参 ∪ 顶层全局 / cval / 顶层 fn 名 )
```

`FV(λ)` 在词法作用域内解析，不跨越 fn 声明边界向上越级（除非外层 fn 本身是 lambda，从而构成嵌套闭包）。捕获是**逐变量**而非"整个外层 frame"。

`CapturesT`（§5.2）字段为 `FV(λ)` 中每个变量在外层的类型 `Tx`，按 §6.2 规则映射成 captures 字段类型。

### 6.2 捕获模式（[#18]，默认推断，无显式覆写）

默认按外层变量类型自动选：

| 外层变量类型 `Tx` | captures 字段类型 | 捕获语义 |
|---|---|---|
| 标量（`i32` / `bool` / `f64` / 等 §3.2.1） | 同 `Tx`（按值复制） | 一次性复制；闭包后续不感知外层变化 |
| 用户 struct / 元组（值类型） | 同 `Tx`（字段级复制 + 字段级 retain，§7.4） | 同上 |
| 堆句柄（`Box<T>` / `Array<T>` / `String` / `StringBuilder` / `Weak<T>`） | 同 `Tx`（句柄复制 + retain） | 共享句柄；与赋值语义一致 |
| `T&` | 同 `T&`（栈嵌入，**不可** Box 化） | 借用透传，§6.3 |
| enum（值类型） | 同 `Tx`（tag dispatch 字段级 retain） | 与 §7.4 一致 |

不引入显式捕获列表语法（如 Rust `move` / C++ `[=]` / `[&]`）。理由：

- 类型即文档：`Box<T>` 必 retain、`T&` 必借用、标量必复制 —— 捕获语义由变量类型唯一确定，无歧义。
- 显式覆写在多数场景是噪音；少数需要"复制堆句柄给闭包独立持有"的需求可在闭包外手动 `var snap = box` 后捕获 `snap`。
- 与 §8 整体"无显式 retain / release / move"风格一致。

### 6.2.1 捕获变量在 lambda 体内只读（[#26]）

lambda 体**不得**对捕获变量赋值（含 `=` / 复合赋值 `+= -= *= /= ...` / 重新绑定 `Box` 句柄等）。违者编译错。

```yux
var a i32 = 0
val f = () => { a = 1 }     ; ❌ 编译错：不可对捕获变量赋值

var b Box<i32> = 0
val g = () => { b = 5 }     ; ❌ 编译错：重新绑定捕获句柄
```

理由：

- v1 标量按值复制（§6.2），赋值仅影响 captures 内副本，对外层变量**静默无效** —— 与用户直觉冲突，必须显式禁。
- 给"自动升级为 `T&` 借用"开口子需要外层 DAA 透传 / `#CallOnce` 注解 / `#Inline` 等机制（详见 §9 不在范围 [#28]）；v1 不做。
- AST 层：lambda 字面量视作普通 fn 实参，无 inline / 无 callonce 特殊语义（[#27]）。

**用户绕道**（v1 表达"想在 lambda 内修改外层状态"的合法路径）：

- **改外层堆对象**：通过堆句柄的方法接口修改对象内部状态（如 `Array::push` / 用户 struct 的 mutator 方法），不重新绑定句柄本身。
- **传 `T&` 形参**：把外层借用作为 lambda 形参显式传入，体内通过形参 `T&` 修改（不走捕获通道）。
- **等 v0.x+1**：`#Inline` 上线后，inline lambda 内对外层 `var` 的赋值直通栈上原变量。

### 6.3 含 `T&` 捕获：captures 栈化 + 不可逃逸（[#19]）

由 §3.2.3.2（字段位置禁 `T&`）+ §5.2（captures 是 `Box<CapturesT>`）推导：

`CapturesT` 含 `T&` 字段时，`Box<CapturesT>` **不可构造**（§3.2.3.2 直接拒绝）。因此含 `T&` 捕获的 lambda **不能**走标准 §5.2 形态。

替代落地：

- **栈嵌入**：`CapturesT` 不进 Box，直接在 lambda 字面量出现的 frame 上分配；fat-ptr 的 `captures` 字段改为 `&CapturesT`（栈借用）。
- **不可逃逸**：含 `T&` 捕获的 lambda（即 `CapturesT` 含 `T&` 字段）按"广义 `T&`"处理：
  - 不可作 `ret` 表达式（除非满足 §8.6.10 溯源约束，§6.5）
  - 不可入字段 / 容器 / `Box<...>`
  - 不可赋给寿命外延的变量（§8.6.5 既有规则）
- **类型层不可见**：`fn(...) R` 仍是单一类型；含 `T&` 捕获 vs 不含的差异由 §8.6 借用寿命规则承担，不暴露到类型相等（§3.4）。

实施侧：含 `T&` 捕获的 lambda 在 IR 层走"栈分配 captures + fat-ptr 指向栈"路径；其余走 §5.2 标准 `Box<CapturesT>` 路径。两条路径在 ABI 层（§5.3）一致：fn_ptr 的隐式首参签名都是 `Ptr`（一个指向栈，一个指向堆）。

**编译期判定 lambda 是否含 `T&` 捕获 = `FV(λ)` 中是否存在 `T&` 类型变量** —— O(1)，无数据流分析。

### 6.4 lambda 内 `$`（[#20]）

> **Phase 4d 已落地（2026-05-09）**：`$` 在 ast_builder 处登记为 `Ref<Self>`（`src/ast/ast_builder.cpp:989` / `:1115`），lambda 体引用 `$` 时 FV 通路按 `T&` 处理，复用 §6.3 的栈嵌入路径，无需新代码。`$.field = ...` / `$.field += ...` 直接受 §6.2.1 [#26] 约束被 E2030 拒（视作捕获 binding 写入）；用户改 `$` 内部状态走 §6.2.1 "用户 struct 的 mutator 方法"绕道。

方法体内的 lambda 引用 `$` / `$.field` / `$.method()` **合法**：编译器把 `$` 视作隐式 `Self&` 形参，按 §6.2 表 "`T&` → 栈嵌入借用透传"处理。

```yux
struct Counter {
  value i32
}

Counter {
  fn each_op(ops Array<fn()i32>) Array<i32> {
    ; 闭包内 $ 是 Counter& 借用，自动捕获
    ops.map({ op =>
      $.value + op()
    })
  }
}
```

约束：

- `$` 捕获即"含 `T&` 捕获" → §6.3 不可逃逸；闭包不能被返回出方法 / 不能存进字段 / 容器。
- 与 §8.6 借用寿命规则一致；闭包寿命 ≤ `$` 借用寿命 = 方法 frame。
- v1 不允许 lambda 内重新指代另一个 receiver（即 lambda 内 `$` **永远**是外层方法的 `$`，不被 lambda 自身重新绑定）。

### 6.5 lambda 返回 `T&` 与 §8.6.10 溯源衔接（[#21]）

> **Phase 4e 已落地（2026-05-09）**：`src/analyzer/borrow_checker.cpp` 在 `visitExpr` 中递归 `LambdaExprNode`：保存外层 ret-ref 状态、push lambda 形参 scope、注册 T& 形参；若 lambda retType 是 T& 且 T& 形参数 ≠ 1 → E4021；body 单表达式视作隐式 ret，根经 `rootFromRetExpr` 推导，∉ 允许源集 → E4020。捕获 T& 在外层 `_refToRoot` 中保留，按外层根名解析，自然不属于 lambda 自身允许源。

由 §3.6（lambda 作 fn 字面量返回 `T&` 沿用 §8.6.10）+ §6.1（捕获是逐变量解析）：

**lambda 的允许源集**（§8.6.10.2 风格）= **形参为 `T&` 者**。

**捕获的 `T&` 不进允许源集**。理由：lambda 出现位置与"被调用位置"可能跨函数，§8.6.10 的根判定算法对捕获来的根**不能**给出稳定的调用点扩散语义（§8.6.10.4 的"调用点根扩散"假设根来自调用 site 的实参，不能假设来自 lambda 字面量出现处的栈）。

```yux
; ✅ 合法：返回的 T& 根来自形参 r
fn op(p (r String&) String& => r) ...

; ❌ 编译错：返回的 T& 根来自捕获 s（不在允许源集）
fn make() ... {
  var s String = "hi"
  ret (() String& => &s)        ; E4020：根 s 不在允许源集
}
```

捕获 `T&` 的 lambda 自身**不可逃逸**（§6.3）这条已经覆盖了大多数误用；本节进一步约束"lambda 体内 `ret expr`（即单表达式体或块体的尾值）的 `T&` 路径"，与现有 §8.6.10 形成同构规则。

### 6.6 解禁 §8.1.2.5

`docs/spec/08-所有权与引用.md` §8.1.2.5 现列"闭包"为 v1 不在范围；本草案落地后从清单移除。CHANGELOG 同步记录。

### 6.7 Open Issues

- **F4'**（继承自 §5.7 F4）：含 `T&` 捕获 lambda 的具体 IR 落地形态（栈分配 + alloca 后地址传 fn_ptr 首参，还是 frame pointer 偏移）—— 实施细节，不进规范。
- **F5**：嵌套闭包（lambda 体内再写 lambda）的 `FV` 推导是否需要"二级捕获"递推？倾向：是，按词法作用域逐层解析；嵌套 lambda 把内层 `FV` 再展开到外层 `FV`。具体形式 v1 可不写到 spec，留实施。
- ~~**F6**~~：已收口为 [#26]，lambda 体不可对捕获变量赋值，含 `+= -= ` 等复合赋值与堆句柄重新绑定（§6.2.1）。
- **F7**：堆句柄 captures 在 lambda 多次调用间是否共享？默认**共享**（`Box<CapturesT>` retain 后多 fat-ptr 指同一 captures）；零拷贝是预期形态。

---

## 7. FFI / extern 边界（Phase 5，[#22]）

> **Phase 4f 已落地（2026-05-09）**：`src/ast/ast_builder.cpp visitExternDelc` 在 paramTypes / retType 计算后扫描 Fn 类型，命中即抛 `E2031`。零捕获 / fat-ptr 拆字段 / `ptr_of:<fn>` 等未来路径仍按本节"informative"留 v0.x+1。

**v1 显式不支持**函数类型跨 FFI 边界：

- `extern fn` 形参 / 返回值**不得**出现 `fn(...)` 类型；语义层报错。
- 不提供 `Ptr` ↔ 函数值的转换 builtin（无 `ptr_of:<fn(...)>` / `to_fn:<fn(...)>`）。
- yux 函数值在 v1 不与 C 函数指针互通；C API 中需要 callback 的接口（如 Win32 `EnumWindowsProc`、`qsort` 比较器）在 v1 无法直接对接。

理由：

- 含捕获 lambda 的 fat-ptr（§5.2）与 C 函数指针不 ABI 兼容；零捕获 lambda 虽可单独取 `fn_ptr` 字段，但需引入静态"零捕获"判定 + 调用约定差异（C 调用无隐式 captures 首参），实施复杂度与 v1 收益不匹配。
- 错误模型 v1 [#7] FFI 边界 `#Throw` 也未收口；两者同步推到 v0.x+1 一并讨论。

**未来路径（informative，不在 v1 承诺）**：

- 引入"零捕获"静态判定 + 编译器 baked builtin `ptr_of:<fn(...)>(f)` / `to_fn:<fn(...)>(p)` 对零捕获 fn 值生效。
- `extern fn` 形参 / 返回值上 `fn(...)` 类型在编译期强制检查实参为零捕获；调用约定按 C 函数指针落地（无隐式 captures 首参，编译器为零捕获 lambda 单独生成"无 captures 首参"的导出符号）。

具体形态留 v0.x+1 草案。

---

## 8. 比较 / 相等性

- `==` / `!=`：函数值不实现内容相等；语义上"相等"无良定义。规范层禁止 `==` 用于函数类型（语义层报错）。
- 地址相等：**不**提供 `same_fn:<...>(a, b)` builtin；避免暴露实现细节（fn 指针、捕获包地址、共用 trampoline 等）。

---

## 9. 不在范围

- ~~尾随 lambda 调用语法 `f { ... }`~~ —— **已折入本草案 §4.3**（[#11]）。
- generic lambda `<T>(x T) => x` 与 generic fn 类型字面量 `fn<T>(T)T`（[#25]）—— 推到 v0.x+1。多数泛型需求由"外层泛型 fn + 内层单态 lambda" + §3.9.2 泛型类型别名 `Predicate<T> = fn(x T)bool` 已覆盖；真正多态 fn 值（rank-n / HRTB）成本不匹配 v1。
- 函数值上的 method（`.compose(g)` / `.curry()` 等）—— 走库函数，不进语言核心。
- 命名捕获参数 `it`（[#7]）。
- 块表达式形态 —— 走 stdlib `fn run<R>(block fn()R) R`，配合 P3 尾随 lambda 调用得到 `run { ... }` 写法（[#4]）。
- 协程 / 异步 lambda。
- **跨 FFI 边界的函数类型**（[#22]，§7）：`extern fn` 形参 / 返回值不接受 `fn(...)`；`Ptr` ↔ 函数值转换 builtin 不提供。v1 显式不支持，推到 v0.x+1。
- **lambda 内对捕获变量的赋值（mutation-through-capture）**（[#26][#28]）：v1 lambda 体不可对捕获变量赋值（§6.2.1）。`#Inline` 注解 + inline 编译期展开 + DAA 透传等机制推到 v0.x+1；届时 inline lambda 内的赋值可直通外层 `var`。当前用户需求走"堆对象 mutator 方法"或"`T&` 形参显式传入"两条合法路径。
- **`#Inline` / `#CallOnce` 注解**（[#28]）：v1 不引入。`try` / `run` / `catch` / `each` 等 stdlib callonce builder 在 v1 走普通 fat-ptr lambda 路径，性能上有一份 captures 分配 + fn_ptr 间接调用开销，但语义正确。v0.x+1 引入 `#Inline` 后再做性能升级与 mutation-through-capture 解锁。

### 9.1 v2 错误模型 try-builder 草图（informative，本草案不承诺）

错误模型 v2 的 try-builder 在本草案 [#27] AST 层"普通 fn"假设下落地形态（不在本草案承诺，留 v2 错误模型立项时决）：

```yux
fn try<R>(block fn()R) A           ; A 是 builder 类型；E 由 block 调用的 fn 的 #Throw enum 推断
                                    ; 链式 catch<E> 用方法本地泛型（v2 阻塞清单 P4）

try {
    ; ... 一组可能 #Throw(E1) / #Throw(E2) / 嵌套 #Throw(E12) 的调用
}.catch<E1> { e1 =>
    ; 处理 E1
}.catch<E2> { e2 =>
    match e2 { ... }
}
```

**前置依赖**：
- 本草案 [#1]–[#28] 全部落地；
- v2 阻塞清单 **P4 方法本地泛型**（`catch<E>` 的 `<E>`）—— 独立立项；
- v2 错误模型立项决定 builder 形态（`A` 的具体类型 / `E` 推断规则 / 错误穿透 / 终态值取出 `value:<T>()`）。

**本草案约束下的限制**：
- block 体内**不可**对外层 `var` 赋值（[#26]）；通过结构化返回值 `ret` 把结果交给 builder。
- catch handler 体内同上限制；通过 `match` 拆解 `e` 后构造新值返回。
- 多次 `.catch<E>` 链式调用是普通方法链（不依赖 inline 机制）。

---

## 10. 迁移面

> 本节是"实施分解的种子"。`CURRENT-lambda.md` 的 Phase 子任务从这里派生。
> 写"面"（哪些文件 / 哪些模块要改），不写逐函数 TODO。

### 10.1 编译器（`src/`）

- 类型系统：新增 `FnType` 节点；`Compiler::resolveAlias` / `applySubst` 接入函数类型透明替换（§3.9.3.1 路径已建好）。
- Parser：lambda 字面量 → AST 节点 `LambdaExpr`；表达式形与块形归一。尾随 lambda 调用糖（§4.3）→ AST 改写为标准调用 `Call(callee, args + [LambdaExpr])`。
- 闭包分析：自由变量收集；捕获包构造（Phase 4）。
- IR 生成：lambda → 顶层匿名 fn + 调用点构造捕获包；零捕获 / 含捕获两条路径（Phase 3 形态决）。

### 10.2 SDK / runtime（`sdk/`）

- 新增 `sdk/yux/core/fn.yux`（如需，依 Phase 3 档位决议）：函数值 RC 协议 / 捕获包析构。
- `run` / `try` 等 stdlib builder 函数（[#4]）—— 与本草案并行立项，不阻塞。

### 10.3 语法（`src/yux.g4`）

> 改语法属高风险动作，按 CLAUDE.md 须用户确认；本节只列要改什么，不动手。

- 新增 `fnType` 产生式，挂入 `type` 选择支。
- 新增 `lambdaExpr` 产生式（表达式形 + 块形），挂入 `expr` 选择支。
- 调用产生式扩展尾随 lambda：`callExpr ::= ... ('(' args? ')')? lambdaBlock?`（具体形态 Phase 2 落地时与 g4 现有 `expr` / `callExpr` 对齐）。
- 新增 token `=>`（最长匹配，与 `=` / `==` 不冲突）。
- 附录 A 加 `=>` 与 `fn`（关键字 `fn` 已存在，仅扩用法说明）；附录 B 同步 `fnType` / `lambdaExpr`。

### 10.4 测试（`tests/`）

- 新增前缀 `lambda_*`（合法用例）/ `tests/cases/error/lambda_*`（非法用例）；按 `tests/xmake.lua` `categorize` 入 `yux/lambda` 分组。
- 覆盖矩阵：每条 §3 / §4 / §5 / §6 规则至少一合法 + 一非法。

### 10.5 规范文档（`docs/spec/`）

- §3.2.2 复合类型表加一行"函数类型 `fn(...) R`"。
- §3.4.1 加结构等同条款（与 §3.8.1.3 元组并列）。
- §3.7 评估禁忌（如 `Weak<fn(...)>` 是否禁，依 Phase 3 决）。
- §4 表达式新增 lambda 节（表达式形 + 块形 + 0 参形）。
- §6 函数：与 lambda 字面量的差异说明（具名 fn 进顶层符号表 vs lambda 是值）。
- §8.1.2.5 移除"闭包"条目；§8.5 调用 ABI 节增函数值传递；§8.6 借用规则增 lambda 捕获 `T&` 寿命条款。
- 附录 A / B / C 同步。
- `docs/spec/CHANGELOG.md` 顶部追加一条。

### 10.6 用户教程（`docs/`）

- 新增 `docs/Lambda与闭包.md`（spec 落定后写）；`docs/index.md` 加链接；`docs/函数.md` 末尾追"函数值与 lambda"小节交叉引用。

---

## 决议日志

- **2026-05-08 [#1]** 函数类型字面量 = fn 声明形态去函数名（§3.2 / §3.3）。
- **2026-05-08 [#2]** 类型相等 = 结构等同；参数名不参与；参数组糖与展开形等同（§3.4）。
- **2026-05-08 [#3]** 函数类型走 §3.9 透明类型别名，无新机制（§3.5）。
- **2026-05-08 [#4]** 块表达式不另立形态；走 stdlib `fn run<R>(block fn()R) R` + 尾随 lambda 调用（§9）。命名取自 Kotlin `run`（0 参体）；不用 `let`（Kotlin `let` 是带 `it` 的单参体，会误导）。
- **2026-05-08 [#5]** lambda 形态：表达式 `(args) => expr` / 单参 `x => expr` / 块 `{ args => body }` / 0 参块 `{ body }`（§4）。
- **2026-05-08 [#6]** 0 参 lambda **禁写** `=>`；`=>` ⇔ "≥1 参"（§4）。
- **2026-05-08 [#7]** 不引入 `it` 隐式参数名（§9）。
- **2026-05-08 [#8]** 单参 lambda 可省括号（`x => expr`，§4）。
- **2026-05-08 [#9]** 闭包 v1 必需（v2 错误模型阻塞依赖；同步解禁 §8.1.2.5；§6）。
- **2026-05-08 [#10]** lambda 形参类型可推断时省略（§4.1）。无上下文则编译错；可与显式类型混写。
- **2026-05-08 [#11]** 尾随 lambda 调用糖（原 v2 阻塞清单 P3）折入本草案 §4.5。仅块形 lambda 可尾随；唯一实参时省 `(...)`。
- **2026-05-08 [#12]** 允许显式返回类型标注：`(a i32) i32 => expr`。仅在参数列表带括号时可写（§4.2）。
- **2026-05-08 [#13]** lambda 出现在已有外层 `(...)` 的实参 / 字段值位置时，参数列表必须括起来；解决与 fn 声明 / fn 类型字面量的歧义（§4.3）。
- **2026-05-08 [#14]** 返回类型规则：裸参 → 推断；括号参 + 无返回标注 → void；括号参 + 显式标注 → 显式 RetT（§4.2）。
- **2026-05-08 [#15]** `if` / `match` 是表达式；lambda 单表达式体可直接含（§4.4）。
- **2026-05-08 [#16]** `fn(...)` 类型字面量中 `fn` 与 `(` 之间不带空格；与 fn 声明"关键字后必须有空格"规则相反，仅本产生式特例。理由：嵌套 fn 类型作实参时空格爆炸（§3.2）。
- **2026-05-08 [#17]** 函数值 RC / ABI 选 **C1**：永远 fat-ptr `{ fn_ptr, captures Box<CapturesT>? }`，零捕获时 captures = null。函数值是值类型 + 字段级 RC（不进堆句柄档位）；调用约定统一（captures 永远作隐式首参传 fn_ptr）。详见 §5。
- **2026-05-08 [#18]** 闭包捕获模式默认按外层变量类型自动选（§6.2）：标量 / struct 复制；堆句柄 retain；`T&` 借用透传。**不引入**显式捕获列表（如 `[move x]`）。
- **2026-05-08 [#19]** 含 `T&` 捕获的 lambda：`CapturesT` 不进 `Box`（§3.2.3.2 字段禁 `T&`），改栈嵌入；lambda 自身按"广义 `T&`"处理 —— 不可逃逸出借用源 scope（§6.3）。
- **2026-05-08 [#20]** 方法体内 lambda 引用 `$` 合法，按隐式 `Self&` 形参捕获处理（§6.4）；触发 [#19] 不可逃逸约束。
- **2026-05-08 [#21]** lambda 返回 `T&` 的允许源集 = 形参为 `T&` 者；**捕获来的 `T&` 不进允许源集**（§6.5）。与 §8.6.10 同构。
- **2026-05-08 [#22]** v1 显式不支持函数类型跨 FFI 边界：`extern fn` 不接受 `fn(...)`；不提供 `Ptr` ↔ 函数值转换 builtin。理由：含捕获 lambda 与 C 函数指针 ABI 不兼容；零捕获静态判定 + 调用约定差异化实施复杂度与 v1 收益不匹配。推 v0.x+1（§7）。
- **2026-05-08 [#23]** `fn` 类型字面量采用紧凑形 `fn(T)R`：`)` 与 retType 之间**不带**空格（与 [#16] "fn 后无空格"配套，仅 `fnType` 产生式特例）。理由：嵌套时 `fn(T)R` 视觉成单一单元，避免 `i32) i32) i32` 粘连。`?` nullable 仍按 §3.6 规则贴 retType（即 `fn(T)R?` = 返回 `R?`）。fn 声明保持通用空格规则。
- **2026-05-08 [#24]** nullable 函数值紧凑形 `fn?(T)R`：`?` 紧贴 `fn` 关键字，表整个函数值可空；与 retType 上的 `?` 视觉分立（`fn?(T)R` vs `fn(T)R?` vs `fn?(T)R?`）。词法层 `fn` + `?` 两 token，`fnType` 产生式吸收，不污染其它位置。**`(fn(T)R)?` 禁**：`fnType` 不进入 §3.6 `typeNullable` 的 `?` 后缀候选，规范统一到紧凑形；理由是两形并存让 `fn?` 与 `(fn)?` 在 review / search / 文档间交替出现，损害可读性（§3.3）。
- **2026-05-08 [#25]** v1 不实现"泛型 lambda 字面量"（`<T>(x T) => x`）与"泛型 fn 类型字面量"（`fn<T>(T)T`）。理由：（a）多数需求由"外层泛型 fn + 内层单态 lambda" + 现有 §3.9.2 泛型类型别名 `Predicate<T> = fn(x T)bool` 已覆盖；（b）真正的多态 fn 值（rank-n / HRTB）实施成本（类型多态推断 + IR 单态化追踪 + RC 协议在多态值上的行为）与 v1 收益不匹配；（c）lambda 字面量无具名锚点，turbofish `:<T>` 实例化路径不直接适用。推 v0.x+1 草案。§9 不在范围扩补。
- **2026-05-08 [#26]** lambda 体**不得**对捕获变量赋值（含 `=` / `+= -= ` 等复合赋值 / 重新绑定堆句柄）。违者编译错。理由：标量按值复制后赋值仅影响 captures 副本，对外层静默无效，与用户直觉冲突。要修改外层状态走"堆对象 mutator 方法"或"`T&` 形参显式传入"。详见 §6.2.1。
- **2026-05-08 [#27]** AST 层：lambda 字面量视作普通 fn 实参；`try` / `run` / `catch` 等 stdlib callonce builder 是普通 fn，不享有 inline / callonce 特殊语义。v2 错误模型 try-builder（草图见下）走此模型。
- **2026-05-08 [#28]** v1 不引入 `#Inline` / `#CallOnce` 注解。`#Inline` 上线后将解锁：lambda 内对外层 `var` 的赋值直通、性能（无 fat-ptr 调用 + 无 captures 分配）、非局部返回 / break-continue 穿透。推 v0.x+1。

---

## Open Issues

- ~~**L1**~~：已收口为 [#15]，`if` / `match` 是表达式；§4.4 收纳。
- ~~**L2**~~：已收口为 [#17] C1；§5.5 表锁字段 / 容器 / nullable / Weak / 借用 合法性。
- ~~**L3**~~：已收口为 [#18]，无显式捕获列表（§6.2）。
- ~~**L4**~~：已收口为 [#20]，lambda 内 `$` 合法（§6.4）。
- **L5**：方法本地泛型（`v2 阻塞清单 P4`）是否阻塞本草案 → 不阻塞 lambda 本身；阻塞 v2 try-builder。
- ~~**L6**~~：已收口为 [#11]，折入 §4.3。

---

## 定型与归宿（template tail）

定型后按 `_模板.md` 末尾流程拆分：

1. §3 / §4 / §6 / §8 / 附录 A / B / C 条款逐项落 `docs/spec/` 正文；
2. `docs/spec/CHANGELOG.md` 顶部追加一条；
3. `src/yux*.g4` 同步（按 CLAUDE.md 用户拍板后改）；
4. 用户教程 `docs/Lambda与闭包.md` 写齐；
5. 本 DRAFT 头部标注「已落地，见 §3 / §4 / §6 / §8」并保留为历史档，或删除（二选一）。

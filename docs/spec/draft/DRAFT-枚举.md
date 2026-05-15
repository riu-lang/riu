# 草案：yux 枚举（enum + match）

状态：**草案 / 讨论中**。日期：2026-05-06。
作用：把"如何在 yux 中表达**带命名变体的代数数据类型**及其**模式匹配读取通道**"这一组决策固化为单一规范，作为修改 `docs/spec/03-类型系统.md` / `04-表达式.md` / `05-语句与控制流.md` / `07-结构体.md` 周边章节，以及 `CURRENT.md` 实施计划的依据。

> 本草案与"错误处理"子项目解耦。错误处理**不**走 enum + Result 形态；本草案只承担"命名变体 + 模式匹配"语言面，不为错误处理预留专用语义。Error.code 字段最终采用 enum，是对本特性的一个普通使用，不构成额外约束。

涉及章节（预估）：§3 类型系统、§4 表达式、§5 语句与控制流、§7 结构体（邻接节点）、附录 A 保留字、附录 B 语法汇总、附录 C 术语表。

---

## 1. 目标

- 提供"命名变体 + 可选 tuple-style payload"的代数数据类型，覆盖状态机 / 离散选择 / 分类数据建模。
- 与 `match` 表达式同档落地：enum 的读取通道**只有 match**，不暴露 tag 整数。
- 与现有结构体路径形态对齐：值类型、栈布局、`Rc<E>` 才上堆；payload 含 RC 字段时按 tag 正确析构。
- 保持 v1 范围最小：**不**做泛型 enum、**不**做独立 `impl` 块、**不**做 discriminant 显式赋值、**不**做 struct-style payload。
- 与既有内置类型并行：不把 `T?` / `Rc<T>` / `Array<T>` 重写为 enum；这些是已固化的内置形态。

## 2. 核心模型与全景

| 概念 | 写法 | 语义 / 存储 | 备注 |
|---|---|---|---|
| enum 声明 | `enum E { V1, V2(T1, T2), ... }` | 声明类型 `E`，登记其全部 variant | 顶层声明，与 `struct` 同级 |
| 零参 variant | `V1` | tag = i，无 payload | 构造写 `E::V1` 或 `E::V1()`，二者等价 |
| tuple-payload variant | `V2(T1, T2, ...)` | tag = i，payload 为 anonymous tuple `(T1, T2, ...)` | 至少一个元素；零参用零参 variant 表达 |
| 构造表达式 | `E::V` / `E::V(e1, e2, ...)` | 产出 `E` 类型值 | **永远全限定**，禁止裸 `V` |
| 类型出现位置 | 与 struct 类型相同 | 变量 / 字段 / 形参 / 返回值 / 泛型实参（仅当外层泛型实例化时） | enum 自身 v1 不可声明类型形参 |
| 读取通道 | `match e { ... }` | 唯一稳定通道 | 不允许 `e.tag`、不允许 `as i32` |
| 存储布局 | `{ tag: i32, union<最大 payload> }` | 值类型，按 tag 选择 payload；含 RC 字段时按 tag dispatch retain/release | 详见 §6（待写） |

要点（在后续节里展开）：

- enum 是**封闭**的：所有 variant 在声明处一次列全；不允许跨文件 / 跨模块追加。
- enum **不暴露内部 tag**：v1 不允许 `as i32`、不允许 `Red = 1` 等显式 discriminant。tag 是实现细节，可在未来版本变更（i8 / i16 / i32）。
- variant **永远以 `E::V` 形式出现**：声明侧用短名 `V`，构造 / match 模式侧必须 `E::V`。这避免 variant 名与全局符号冲突，也让 IDE 跳转更确定。
- 不引入 `dyn Enum` / 类型擦除；enum 是单一具体类型。
- enum 不参与隐式转换；`E1` 与 `E2` 是无关类型，即便 variant 名相同。

## 3. enum 声明

### 3.1 出现位置（白名单）

- 文件顶层：与 `struct` / `fn` 同级，作为模块的可见声明。
- 不允许：函数体内、struct 体内、enum variant 体内（v1 不做嵌套声明）。

### 3.2 声明语法（拟）

```yux
enum Color {
  Red
  Green
  Blue
}

enum Shape {
  Point              ; 零参 variant
  Circle(f64)        ; 单元素 tuple payload
  Rect(f64, f64)     ; 多元素 tuple payload
}
```

约定：

- variant **一行一个**，以换行分隔，**不写 `,`**。多个 variant 写在同一行属语法错误。
- variant 名采用 PascalCase（与类型名同规约；附录 C 给词法建议，非硬性）。
- variant 名在同一 enum 内**不得重复**；不同 enum 之间互不干扰。
- payload 类型与"普通类型出现位置"一致：可以是基础类型、struct、`Rc<T>`、`Array<T>`、`String`、`T?`、tuple `(T1, T2)`，以及类型别名。
- payload 中**不允许**出现 `T&`（与 struct 字段同规约，§8.6 已禁止 `T&` 进字段）。
- payload tuple 内部用 `,` 分隔元素（这是 tuple 自身规则，非 variant 分隔符）。

### 3.3 零参 variant 与 tuple-payload variant 的关系

- 零参 variant `V` **是** tuple-payload variant 的退化形式（零元 tuple）。
- 构造侧 `E::V` 与 `E::V()` 等价，AST 归一为零参 ctor，不区分两形态。
- match 模式侧同理：`E::V` 与 `E::V()` 等价（细节进 §5 match 章节）。

### 3.4 命名与可见性

- enum 类型本身遵循当前模块导出规则（与 struct 一致）。v1 不引入"variant 单独可见性"。
- enum 名进类型命名空间；variant 名**不**进顶层命名空间，必须经 `E::` 限定访问（语义等同 C++ `enum class`）。
- 同一文件内禁止 enum 名与 struct / 类型别名 / 已知内置类型撞名（沿用 §3 现有规则）。
- **类型别名透明传递 variant 访问路径**：`type C = Color` 后，`C::Red` 与 `Color::Red` 等价，AST 在解析期完成透明替换（沿用元组 Phase 2b 既有别名机制）。别名同样不破坏全限定要求 —— 仍必须写 `C::Red`，禁止裸 `Red`。

### 3.5 与现有特性的相容性

| 特性 | 行为（v1） |
|---|---|
| `Rc<E>` | 允许；按现有 Rc 协议 retain/release，析构时按 tag dispatch payload 析构 |
| `Array<E>` | 允许；元素按值存储，移动 / 拷贝行为遵循 enum 自身规则 |
| `E?` | 允许；按现有 Nullable 规则展开 |
| 泛型 enum `enum E<T> { ... }` | **不允许**（v1 之外） |
| enum 上的方法 / `impl` 块 | **不允许**（v1 之外） |
| draft 实现 `E : Draft { ... }` | **不允许**（v1 之外） |
| discriminant 显式值 `Red = 1` | **不允许**（v1 之外） |
| `as i32` / 转 tag 整数 | **不允许**（v1 之外） |
| variant 名省略 `E::` | **不允许** |

> v1 之外的项是有意识地砍掉的最小集，目的是先把"声明 / 构造 / match"三件事走通；后续档位（v0.x+1）再一次性补"泛型 + draft + 方法"。

### 3.6 g4 改动方向（**不在本节落实**）

预期改动点（高风险，进入 Phase 2 时再逐处与用户确认）：

- `yuxParser.g4`：新增顶层 `enumDecl`；新增 `enumVariant`（短名 + 可选 tuple payload）。
- `yuxParser.g4`：构造表达式 `E::V` / `E::V(args)` 复用既有 `::` 路径还是新增产生式，待 Phase 2 评估。
- `yuxParser.g4`：`match` 表达式 + `matchArm` + 模式（pattern）—— 与 §5 一并做。
- `yuxLexer.g4`：是否需要新 keyword `enum` / `match`，待查现有保留字表（附录 A）。

---

## 4. 构造表达式

### 4.1 形态

| 形式 | 适用 | AST 归一 |
|---|---|---|
| `E::V` | 零参 variant | `EnumCtor(E, V, [])` |
| `E::V()` | 零参 variant | `EnumCtor(E, V, [])` —— 与上行**等价** |
| `E::V(e1, e2, ...)` | tuple-payload variant | `EnumCtor(E, V, [e1, e2, ...])` |

零参 variant 写带不带括号编译器一视同仁；ToString 等下游不应观测到差异。

### 4.2 类型与参数规则

- 实参个数应当与 variant 声明的 payload 元素个数严格一致。
- 实参类型应当与对应 payload 元素类型严格匹配；**不做隐式转换**（沿用 yux 全局规则）。
- 实参求值顺序与函数调用一致（左至右）。
- 构造表达式整体类型为 `E`，是值类型，存活与移动语义遵循 §6（待写）。

### 4.3 与类型别名

`type C = E` 后，`C::V` / `C::V()` / `C::V(args)` 三种形式与 `E::V*` 等价，由解析期透明替换处理（详见 §3.4 [#3.F]）。

### 4.4 不允许形式

| 形式 | 原因 |
|---|---|
| 裸 `V(args)` 或 裸 `V` | 必须 `E::` 全限定（[#3.B]） |
| `E::V { f: e }` | struct-style payload 押后（[#3.D]） |
| `E::V(...).0` 等 tuple 索引 | enum 不暴露 payload；唯一读取通道是 match（[#2.A]） |
| `E::V as i32` | 不暴露 tag（[#3.C]） |

---

## 5. match（最小子集）

### 5.1 形态

```yux
var area = match s {
    Shape::Point => 0.0
    Shape::Circle(r) => 3.14 * r * r
    Shape::Rect(w, h) => w * h
}
```

约定：

- match 箭头一律 `=>`（`->` 留给后续 lambda）。
- arm **一行一个**，行尾不写 `,`，与 §3 variant 同规约。
- arm 顺序对穷尽语义无影响；唯一例外是 `else`，必须是最后一条。
- match **是表达式**（与 if-else 同档）；作为语句使用时各 arm 体可为空返回（带尾随 `;` 的表达式）。

### 5.2 模式形态（v1）

| 模式 | 含义 |
|---|---|
| `E::V` | 匹配零参 variant |
| `E::V()` | 与 `E::V` 等价 |
| `E::V(name1, name2, ...)` | 匹配 tuple-payload variant，**每个 payload 元素绑定到一个名字** |
| `else` | 兜底分支，匹配尚未显式列出的所有 variant |

绑定规则：

- 绑定名是普通标识符，遵循变量命名规则；同一模式内**不得重复**。
- 绑定为不可变值绑定（与 `var` 不同：不允许重新赋值），作用域为该 arm 的体（含其块）。
- 绑定的类型为对应 payload 元素的类型（按位置）。RC 字段（`String` / `Rc<T>` / `Array<T>` 等）按值绑定时遵循现有 retain/move 规则（细节进 §6 ABI）。
- 绑定**未被使用**不产生警告（等同 v1 不实施 `_` 通配）。

### 5.3 v1 不支持的模式

- 字面量模式：`Shape::Circle(0.0)`（不允许，元素位只能是绑定名）
- 嵌套模式：`Some(Some(x))` 之类（v1 无嵌套 enum，且暂不支持）
- 范围模式 / 守卫 `if`
- `_` 通配（任意位置）—— 走 `else` 兜底；payload 元素位必须给名字
- 多模式合并（`A | B => ...`）
- 绑定后再约束（`@` 模式）

### 5.4 穷尽性

- 编译器**强制**穷尽性检查。
- 满足穷尽的两种合法形态：
  1. 显式列出 enum 的**所有 variant**，每个**至多出现一次**；
  2. 列出**任意子集**（含空集）后以 `else` 兜底。
- 重复 variant（同一 variant 在 arms 中出现多次）应当报错。
- 缺失 variant 且无 `else` 应当报错，错误信息列出未覆盖的 variant 名。
- `else` 出现在非末尾位置应当报错。

### 5.5 类型与值规则

- match 作为表达式时，所有 arm 体类型应当**严格一致**（无隐式转换）。
- match 作为语句使用（不取值）时，各 arm 体可为表达式语句、块或控制流终结（`ret;` / `break;`）。
- 被 match 的标量（scrutinee）只求值一次。
- scrutinee 类型应当是 enum 类型 `E` 或其类型别名（解析期已替换）；非 enum 类型在 v1 不允许进入 match（不做整型 / 字符串 match）。

### 5.6 g4 改动方向（**不在本节落实**，进入 Phase 2 时确认）

- `yuxParser.g4`：新增 `matchExpr`、`matchArm`、`enumPattern`（含 `else` 分支）。
- 与既有 `expression` / `statement` 入口的接驳点（特别是 expression-as-statement 路径）。
- `yuxLexer.g4`：`match` 是否需新增为 keyword；`else` 既有保留字直接复用。

---

## 6. 运行时 / 内存模型 / RC

### 6.1 数据布局（伪代码）

```
struct EnumValue<E> {
    tag:     i32        ; 0..N-1，按声明顺序编号
    payload: union {    ; 大小 = max(sizeof(variant_i 的 tuple payload))
        v0: tuple(...)
        v1: tuple(...)
        ...
    }
}
```

- enum 是**值类型**，默认栈上，遵循 §7 结构体的"按值布局"族。
- 零参 variant 在 union 槽内占 0 字节，但 enum 整体大小仍取所有 variant payload 中的最大值（含对齐）。
- 整体对齐取 `max(align(i32), max align of payloads)`。

### 6.2 tag 编码

- tag 类型固定 **`i32`**。理由：与默认整型对齐友好；i8 / i16 节省的字节通常被 payload 对齐填回去，不划算。
- variant 按声明顺序从 0 起编号；用户代码**不可观测** tag 数值（§3 [#3.C]）。
- tag 编码 / 类型 / niche 优化等均为实现细节，未来版本可调整，**不构成 ABI 承诺**（§6.10）。

### 6.3 析构（按 tag dispatch）

- 编译器为每个**含 RC payload 字段**的 enum 合成 `__enum_drop_<E>(p: *E)`：

  ```
  __enum_drop_E(p):
    switch p->tag {
      case 0: drop variant_0 payload fields   ; 逐字段按既有 RC 规则 release
      case 1: drop variant_1 payload fields
      ...
    }
  ```

- 全部 variant 都是 POD（无 String / Rc / Array / 含 RC 的 struct / `T?` 含 RC 内层）的 enum：**不**合成 drop，按平凡值处理。
- drop 触发点与 struct 一致：变量退出作用域、函数返回前的非 returned 值、被覆盖赋值的旧值、复合表达式的临时寿命终点。

### 6.4 拷贝与移动

- 默认按值拷贝：tag 直接复制；payload 按当前 tag dispatch 到对应 variant 的字段拷贝（RC 字段 retain）。
- 全 POD enum 走 `memcpy` 路径；含 RC 时合成 `__enum_copy_<E>(dst, src)` 按 tag dispatch。
- move-return / 实参移动 / 链式 `+` 等沿用 callee-clean retain/release 协议（与 struct 完全一致），不引入 enum 专用协议。

### 6.5 构造 codegen

- `E::V(args...)`：在结果槽写 tag = V 的序号；把 args 按位置写入 payload 对应 variant 的字段位（RC 字段按 callee-clean 入参规则）。
- 零参 variant `E::V` / `E::V()`：仅写 tag，payload 槽不动（语义上不可观测内容）。

### 6.6 match codegen

- scrutinee 求值一次，存到 match 临时槽（寿命覆盖整个 match 表达式，含所有 arm 体）。
- 走 LLVM `switch` over tag → 每个 case 跳转到对应 arm 块；`else` arm 接 default case。
- arm 块内：按位置从 payload 槽取出绑定（含 RC 字段绑定**按值绑定**时 retain；arm 退出时 release）。
- match 作为表达式时，所有 arm 体把结果写入统一的 match 结果槽。
- match 结束后，scrutinee 临时按 §6.3 dispatch drop（未发生整体移动的情形下）。

### 6.7 `Rc<E>` 与 `Array<E>`

- **`Rc<E>`**：复用现有 Block layout `{strong, weak, payload: E}`；Block 的 strong 归零时先调 `__enum_drop_<E>(&payload)`（若有），再走 Block 自身释放路径。等同 `Rc<Struct>`，无新协议。
- **`Array<E>`**：元素按值存储；Array 析构遍历元素 dispatch drop。等同 `Array<Struct>`。
- 这两条共用一句话：**enum 在 RC 容器中的地位与 struct 完全一致**，不引入新的容器入口。

### 6.8 空 enum 与单 variant enum

- **`enum E { }`**（无 variant）：**编译期拒绝**。理由：没有值能构造它；保留 uninhabited 类型在 yux 没场景，徒增"何时该报错"的边界。
- **`enum E { Only }`**（单 variant）：合法。虽无实际意义，但不专门拒绝；后续可作为新增 variant 前的占位写法。

### 6.9 临时值寿命

- 与 struct 临时一致：表达式语句末尾、`;` 边界释放。
- match scrutinee 临时寿命覆盖整个 match 表达式；arm 体内绑定独立分配，arm 退出时释放（绑定是值绑定，含 RC 时 retain/release 成对）。
- `match expr { ... }` 整体作为右值参与外层表达式时，scrutinee 临时寿命到外层表达式语句末尾。

### 6.10 ABI 稳定性

- v1 **不**承诺跨编译器版本的 enum ABI 稳定（与 struct 一致）。
- tag 编码顺序、payload 内部布局、niche optimization、是否提取至单独段等均为实现细节。
- 语言面仅规定：enum 是值类型，含 RC payload 时正确按 tag dispatch retain/release，与 struct / Rc / Array 协议一致。

---

## 7. 不在范围

- 泛型 enum `enum E<T> { ... }`
- enum 上的方法 / 独立 `impl` 块
- enum 实现 draft（`E : Draft { ... }`）
- struct-style payload `V { f: T }`
- discriminant 显式赋值 `Red = 1`
- `as i32` / tag 整数互转
- 嵌套模式、字面量模式、范围模式、守卫、`|` 多模式、`@` 绑定、`_` 通配
- 整型 / 字符串 match
- variant 跨文件 / 跨模块追加（enum 是封闭的）
- v1 之后版本可能补的：`if let` / `while let` 等单分支糖

---

## 8. 迁移面

### 8.1 编译器（`src/`）

- `include/types.h`：`TypeKind` 增 `Enum` 档位；`TypeInfo` 携带 enum decl 引用。
- `src/node/`：新增 `enum_node.{cpp,h}`（声明节点）、`enum_variant_node.{cpp,h}`（variant 节点）；表达式侧扩 `expr_node` 加 `EnumCtorExpr` 与 `MatchExpr` 形态（具体落点待 Phase 1 评估，可能复用现有 ctor 路径）。
- `src/ast_builder.cpp`：enum 声明 / 构造 / match 的 ANTLR → AST 路径。
- `src/compiler.cpp` / `compiler_call.cpp`：构造调用分发；类型检查接入。
- `src/compiler_match.{cpp,h}`（新增）：match 的 codegen（基于 switch on tag）、绑定槽位生成、穷尽性检查。
- `src/borrow_checker.cpp`：match arm 中的绑定按值绑定语义对接现有借用规则；payload 含 `Rc<T>` / `Array<T>` 等 RC 类型时按 tag dispatch retain/release。
- `src/mangler.cpp`：variant ctor 的 mangling 方案（建议：`<EnumMangled>::V`，与 struct 方法 mangling 同策）。

### 8.2 SDK / runtime（`sdk/`）

- v1 不提供内置 enum；`base.yux` 不动。
- 后续版本（押后）补 `Option<T>` / `Result<T, E>` 等需待泛型 enum 落地。

### 8.3 语法（`src/yux*.g4`）

> 高风险，按 CLAUDE.md 项目约束需逐处与用户确认；本节只列"要改什么"。

- `yuxParser.g4`：新增 `enumDecl`（顶层）、`enumVariant`（短名 + 可选 tuple payload）、`matchExpr` + `matchArm` + `enumPattern` + `else` 分支。
- `yuxParser.g4`：构造表达式 `E::V(args)` 与既有 `::` 路径的接驳。
- `yuxLexer.g4`：评估是否需要将 `enum` / `match` 设为新关键字（沿用既有保留字风格）。

### 8.4 测试（`tests/`）

- `sdk/yux/src/yux/core/enum.test.yux`（新增）：声明 / 构造 / match / 类型别名透传 / 含 RC payload 析构。
- `tests/cases/diag_enum_*.{yux,expected_err}`：缺失 variant、重复 variant、`else` 非末尾、payload 元数不符、裸 variant、`as i32`、`E::V.0` 等诊断回归。
- 借用 / 析构相关行为用例（含 `Rc<E>`、`String` payload）落到 `sdk/yux/src/yux/core/enum.test.yux`，走 JIT。

### 8.5 规范文档（`docs/spec/`）

- §3 类型系统：新增 enum 类型条款。
- §4 表达式：新增构造表达式 + match 表达式条款。
- §5 语句与控制流：match 作为语句使用的规则。
- §7 结构体：与 enum 的对照说明（值类型族邻居）。
- §11 编译期注解：检查是否需要新增（v1 不引入）。
- 附录 A 保留字：`enum` / `match` 入表（如 g4 引入）。
- 附录 B 语法汇总：与 `yux.g4` 对齐。
- 附录 C 术语表：variant、payload、scrutinee、穷尽性、绑定。
- 附录 D 诊断：新增 enum / match 相关错误码段。
- `docs/spec/CHANGELOG.md`：合并日追加一条。

### 8.6 用户教程（`docs/`）

- 新增 `docs/枚举与匹配.md`，加入 `docs/index.md` 索引。

---

## 决议日志

---

## 决议日志

- **[#1.A]** 形态采用 Rust 风 ADT（tuple-style payload），C-like 命名常量集合作为"全部 variant 都是零参"的退化形式落到同一套机制下，避免双轨制。
- **[#1.B]** 错误处理走 Swift 风 `throws / try / catch`（见 `TARGETS.md`），enum 不承担 Result 角色；本草案不为错误处理预留专用语义。Error.code 用 enum 是普通使用场景。
- **[#2.A]** match 与 enum 同档落地：v1 enum 的读取通道**只有 match**，不暴露 tag 整数。理由：若不做 match，enum 写得出读不出，毫无价值；提供 `e.tag` / `as i32` 又会反过来固化 tag 类型选择，未来难以调整。
- **[#3.A]** 零参 variant 的构造形态 `E::V` 与 `E::V()` 等价。理由：C-like 习惯写 `E::V`，函数式 / Rust 习惯写 `E::V()`；二者等价比强制其一更省心，AST 归一为零参 ctor 不引入额外档位。
- **[#3.B]** variant 永远全限定 `E::V`，禁止裸 `V`。理由：避免与全局符号 / 函数名 / 常量名冲突；保留未来引入"局部 use" 的余地，但 v1 不做。
- **[#3.C]** discriminant 完全不暴露。理由：tag 类型（i8/i16/i32）应是后端可调的实现细节；一旦允许 `as i32` 或 `Red = 1`，就把 ABI 选择固化到了语言面。
- **[#3.D]** 泛型 enum、独立 `impl` 块、draft 实现、struct-style payload 全部押后到下一档。理由：错误处理已不依赖 enum，去掉了"必须一次到位"的压力；先把声明 + 构造 + match 三件事跑通更稳。
- **[#3.E]** variant 一行一个、换行分隔、行尾不写 `,`。理由：与 yux 整体"少标点"风格一致（已有 `;` 注释、强制空格、换行作为语句边界）；tuple payload 内部仍用 `,` 分隔，是 tuple 自身规则，二者不冲突。
- **[#3.F]** 类型别名透明传递 variant 访问路径：`type C = Color` 后 `C::Red ≡ Color::Red`。理由：别名机制已在元组 Phase 2b 落地为"AST 解析期透明替换"，enum 沿用同一通道无新增规则；别名仍受全限定约束，不为裸 `Red` 开口子。
- **[#5.A]** match 箭头一律 `=>`。理由：`->` 留给后续 lambda（Kotlin 风），避免两套表达式形态共用同一符号造成歧义。
- **[#5.B]** 兜底走 `else`，**不**引入 `_` 通配（任意层）。理由：`else` 已是保留字，复用零代价；`_` 一旦引入需要在多处规则（标识符 / 模式 / 表达式占位）开口子，v1 想保持小。代价是 payload 元素位必须显式给名字（含未使用情况），通过"绑定未使用不警告"消化。
- **[#5.C]** 模式中 payload 元素直接绑定为不可变值，写作 `E::V(a, b)`。理由：与 Rust / Swift 主流一致；不引入额外修饰符；含 RC payload 按现有 retain/move 协议处理（细节进 §6）。
- **[#5.D]** match 是表达式，与 if-else 同档。理由：值导向的"按变体取值"是 enum 最常见用法；作为语句使用时各 arm 走 `;` 路径，无需独立形态。
- **[#5.E]** 必须穷尽（Kotlin 风）：列全所有 variant 或带 `else` 兜底，二者择一。理由：穷尽性是 ADT 的核心安全保证；让编译器把"忘了一个 variant"挡在编译期。`else` 必须为最后一条，避免不可达分支。
- **[#6.A]** tag 类型固定 i32。理由：与默认整型对齐友好；i8 / i16 节省的字节通常被 payload 对齐填回去；tag 类型对用户不可观测，未来仍可调整。
- **[#6.B]** payload 内联为 union，与 tag 同槽存储（值类型）；不做"payload 永远上堆"的间接版。理由：与 struct 值类型族一致；要上堆用 `Rc<E>`，与既有协议复用。
- **[#6.C]** 析构 / 拷贝 / 移动按 tag dispatch；编译器合成 `__enum_drop_<E>` / `__enum_copy_<E>`。全 POD enum 走平凡 memcpy / 无 drop，避免不必要开销。
- **[#6.D]** `Rc<E>` / `Array<E>` 走与 `Rc<Struct>` / `Array<Struct>` 完全相同的 RC 入口，不引入容器级 enum 专用协议。
- **[#6.E]** 空 enum `enum E { }` 编译期拒绝；单 variant `enum E { Only }` 合法（占位 / 后续扩展用）。
- **[#6.F]** v1 不承诺 enum ABI 跨版本稳定，与 struct 同档。在 v1.0 候选档（规范定稿 + ABI 冻结）一并定。

; 草案：yux 静态函数（关联函数）

# 草案：yux 静态函数（`#Static` + `Type::fn`）

状态：**已落地（P1 + Phase 6A–6E）**，规范见 [docs/spec/07-结构体.md §7.10](../07-结构体.md) 与 [docs/spec/11-编译期注解.md §11.11](../11-编译期注解.md)；实施记录见 [docs/dev/static-fn-impl-log.md](../../dev/static-fn-impl-log.md)。日期：2026-05-17 起草，2026-05-18 收尾。

> 范围已扩为"构造模型重构（砍 ctor）"——P1 落静态函数 + `Self` + `Self { ... }` 字段字面量三件套，Phase 6A–6D 砍同名 `fn TypeName(...)` ctor 通道（sema E3130 拦截定义、SDK / tests / examples / docs 全量迁移、C++ 端 ctor 残余清理）。构造唯一通道收敛到 `#Static fn` + `Self { ... }`。本文件保留作历史档；正文不再变更。
> 已知遗留：泛型 struct + `#Static fn` + `Self { }` codegen 路径未通（BUGS #3 / Phase 6E.4 B-E），非阻塞。
作用：在 v1 结构体方法模型（§7.2，全方法隐式注入 `$: Self&`）之外，新增一类"不带接收者"的函数（关联函数 / 静态函数），挂在类型命名空间下，调用语法 `Type::name(...)`。同时为函数体引入 `Self` 类型关键字。

> 这是 §7.3.2 / §7.3 周边 Open Issue 的一部分，但与"结构体字面量构造"**独立**，后者不在本草案范围（见 §9）。
> `DRAFT-spec-unify.md` 也碰 `Self`，本草案先行落 `Self` 在静态函数位上的最小语义；spec-unify 落地时统一收口。

涉及章节（预估）：§07 结构体（新增 §7.10 或合并入 §7.2）、§06 函数（提及 `Self`）、§11 编译期注解（新增 `#Static` 条目）、附录 A（视决议是否加 `Self` 为保留字）、附录 B 语法汇总、附录 D 诊断。

---

## 1. 目标

- 提供与现有"实例方法 + 同名构造 fn"正交的"关联函数"通道，覆盖工厂 / 转换器 / 类型级常量构造场景。
- 调用语法**显式标记**类型来源（`Type::fn`），与实例方法 `obj.fn(...)` 视觉上区分；不复用 `.`。
- 标记机制**显式**（注解），不做按签名隐式推断。
- 与现有构造函数 `Foo(x, y)` **共存**，**不取消**也不重写既有用例。
- 函数体允许 `Self` 作为类型字面量（返回 / turbofish / 局部声明）。
- 不引入字段字面量构造、不动 `Foo(...)` ctor 语义、不引入按类型路径的 const 访问（`Type::CONST` 留给以后）。

## 2. 全景表

| 概念 | 写法 | 语义 | 备注 |
|---|---|---|---|
| 静态函数声明 | `#Static\nfn name(...) RT { ... }`（在 structImpl 块内） | 无 `$` 接收者；挂在 `Type` 命名空间下 | `#Static` 必须紧贴 fn 头；与其它 `buildAnno` 可叠加 |
| 静态调用 | `Type::name(args)` | 调用 `Type` 上声明的 `#Static fn name` | 复用既有 `SymbolColonColon` token |
| 泛型静态调用 | `Type::name:<T>(args)` | turbofish 给静态 fn 的类型参数 | 与现有泛型 fn 调用语法一致 |
| 泛型 struct 上的静态调用 | `Vec:<i32>::with_capacity(8)` | 类型参数挂在 struct，再调静态 fn | 见 §3.5 |
| `Self` 类型 | `Self`（仅静态 fn 体内 + 已有的 ctor / 实例方法上下文） | 等同所属结构体类型（含泛型实参） | 静态 fn 内不可写 `$`，故 `Self` 是访问 receiver-side 类型的唯一通道 |

要点：

- `#Static` 是**注解**（`buildAnno`），不是关键字；不增加保留字。
- `Self` **加为保留字**（**[#1.A]**），与 `null` / `true` / `false` 同档；lexer 新增一个 token，附录 A 同步。
- `ID :: ID` 在 parser 层与现有 `exprEnumCtor` 同形——**parser 不分叉**，sema 按 LHS 类型分流（enum / struct）。AST 节点 `ExprEnumCtorNode` 改名为 `ExprPathCallNode`（**[#4.A]**），enum 路径不动语义。

## 3. 子特性 A — 语法形态

### 3.1 声明位置

- 静态函数**只能**在 `structImpl` 块内声明，且**必须**带 `#Static` 注解；其它位置（顶层 / draft 实现块 / enum 上）**不允许**。**[#1.B]**
- `#Static` 与其它注解的相对顺序：`#Static` 应当紧贴 `fn` 头（最末一行注解），实现上不强制，sema 只检"注解集合是否含 `#Static`"。
- 同一 structImpl 块内**允许**静态 fn 与实例 fn / ctor / dtor 混合书写；语法层无顺序要求。
- 同一结构体内静态 fn 与实例 fn 可以同名（**[#2.A]**），但 sema **抛 warning**（诊断码 W3xxx，可读性提醒；不阻断编译）。两者在调用语法层天然区分：`Type::foo(...)` 走静态、`obj.foo(...)` 走实例，无解析歧义。
- 静态 fn 与构造函数同名（即与结构体同名）**禁止**——`Point::Point(...)` 在调用形态上与既有 `Point(...)` ctor 入口冲突，无法区分。

### 3.2 调用语法

```yux
struct Point {
  x f64
  y f64
}

Point {
  fn Point(x f64, y f64) {     ; 普通构造，不变
    $.x = x; $.y = y
  }

  #Static
  fn origin() Self {            ; 静态工厂
    Point(0.0, 0.0)
  }

  #Static
  fn from_pair(p (f64, f64)) Self {
    Point(p.0, p.1)
  }
}

fn main() {
  val p1 = Point::origin()
  val p2 = Point::from_pair((1.0, 2.0))
}
```

泛型示例：

```yux
struct Vec<T> { ... }

Vec<T> {
  #Static
  fn with_capacity(n usize) Self { ... }

  #Static
  fn of:<U>(x U) Vec<U> { ... }     ; 静态 fn 自身也可带类型参数
}

fn main() {
  val v1 = Vec:<i32>::with_capacity(8)
  val v2 = Vec::of:<String>("hi")    ; 类型参数全在静态 fn 上时，struct 可省略
}
```

### 3.3 语法层禁止

- 静态 fn 体内**禁写** `$`、`$.field`、`&$`（语法层不感知 `$`，由 sema 报错；诊断码新增 E3xxx）。
- 静态 fn **不可**作为运算符重载方法（运算符方法约定接收者 `Self&`，§7.2.3.3）。
- 静态 fn **不可**与 `fn ~()` 同义（析构是接收者绑定的）。
- 静态 fn **不可**在 draft 实现块内（§7.8.2.1 既有约束扩展：辅助 fn / 静态 fn / ctor / dtor 均不混入 draft 块）。**[#1.C]**
- 静态 fn **不可**标 `#Frozen`（`#Frozen` 是字段 / 形参专用注解，**[#5]**）。

### 3.4 `Self` 在 structImpl 体内

- `Self` 等同所属结构体类型（含其泛型实参的当前绑定）：
  - `Vec<T>` 的 impl 块内 `Self ≡ Vec<T>`；
  - `Point` 的 impl 块内 `Self ≡ Point`。
- `Self` 是**类型字面量**，进入 `type` 产生式作为一条新 alt（与 `ID` / `ID genericDef` 并列）；同时进 `typeWithRef`（`Self SymbolAnd?` 形态）。
- 允许位置（v1）：
  - **类型位**：fn 返回类型、fn 形参类型（含实例方法，如 `fn clone() Self` / `fn merge(other Self&) Self`）、局部变量类型标注、turbofish 实参（`Foo:<Self>(...)`）；
  - **调用位**：`Self::name(...)` —— 在 structImpl 体内（含实例方法体、静态 fn 体、ctor 体）调用本类型静态 fn 的糖（**[#3.A]**）；语义等同当前类型名做 LHS。
- 不允许位置（v1）：**字段类型**——§7.1.3 既有规则：字段不能写 `Self`（栈上自引用直接被拒绝），自引用须 `Rc<Self>?` / `Weak<Self>?`。`Rc<Self>` / `Weak<Self>` 中 `Self` 作为泛型实参出现于字段位**允许**（§3.7 既有形态）。

### 3.5 与 turbofish / 泛型的组合

调用形态完整 BNF（口头）：

```
staticCall ::= (typeName | 'Self') (':' genericDef)? '::' ID (':' genericDef)? '(' args? ')'
```

LHS 可以是 struct 名（带可选 turbofish）或保留字 `Self`（仅在 structImpl 体内合法；体外写 `Self::...` 由 sema 报错）。

- 第一段 `(':' genericDef)?` —— struct 的类型参数；
- 第二段 `(':' genericDef)?` —— 静态 fn 自身的类型参数；
- 至少一段；两段可同时出现也可只出现一段；类型参数推断规则与现有泛型 fn 一致（若能从实参推出则可省）。

### 3.6 ABI

- 静态 fn 在 IR 层就是顶层 fn，不注入 `$` 形参。
- mangle：`Type::name` 形参表前置 struct 的 mangle 段（含泛型实参），与实例方法 mangle 同前缀但**不含 receiver 形参槽位**。具体方案见 §5。
- 泛型 struct 的静态 fn 跟随结构体单态化：`Vec<i32>::with_capacity` 与 `Vec<String>::with_capacity` 是不同实例。

## 4. 子特性 B — sema 分流

`ID :: ID ( :<T> )? ( ( args ) )?` 在 parser 输出为同一 AST 节点（建议复用 `ExprEnumCtorNode` 并改名为 `ExprPathCallNode`，或新增 `ExprStaticCallNode` 并把 enum 路径迁过来 —— **[#4.A] 待定**）。

sema 在该节点上的判定（按 LHS 名查找）：

1. LHS 解析为 **enum 类型** → 走 §3.10 既有 `enumCtor` 路径；
2. LHS 解析为 **struct 类型**，RHS 在该 struct 的方法集中找到一个**带 `#Static` 注解的 fn** → 走静态调用路径；
3. LHS 解析为 **struct 类型**，RHS 找到的是**实例 fn / ctor**（无 `#Static`）→ 报 E3xxx "实例方法不能用 `::` 调用，请用 `obj.method(...)` 或在声明上加 `#Static`"；
4. LHS 无法解析为 enum / struct，或 RHS 找不到匹配 → 报 E3xxx "未找到关联函数"。

判定 1/2 之间不存在冲突（enum 与 struct 是不同名义类型）。

## 5. 运行时 / mangle

- 静态 fn 没有 receiver，**不**走字段级 retain / release 派生（§7.4.3 与本特性无关）。
- mangle 形态草案（与 mangler.cpp 现状对齐时再细化）：
  - 实例方法当前形态：`<Type>::<name>(<Self&>, <args...>)`
  - 静态 fn 形态：`<Type>::<name>(<args...>)`（receiver 槽位缺省）
  - 关键：mangle name 要包含 `static` 标记或通过缺省 receiver 槽位天然区分，**避免**与同名实例方法在 IR 层冲突（实际 §3.1 已禁同名，但 mangle 层加保险）。

## 6. 构造与初始化

不涉及 DAA（§7.3.3）—— 静态 fn 不操作 receiver。
静态 fn **可以**调用 ctor 来构造 `Self`：`Self::origin() { Point(0, 0) }` 体内 `Point(0, 0)` 是普通 ctor 调用，照走 §7.3。

## 7. FFI / extern

- `#Static` 与 `extern fn` 不组合（extern 块在 structImpl 之外，§6.6）。
- 静态 fn 自身可以调用 `extern fn`，无特殊约束。

## 8. 比较 / 相等性

不适用。

## 9. 不在范围

- **任意 struct 名字面量 `Other { .x = ... }`**：v1 仅放开 `Self { ... }` 形态（**[#8]**）；跨类型构造走 `Other::new(...)` 等静态调用。
- **关联常量** `Type::CONST` —— 需要在 structImpl 内支持 `cval` 声明，本草案不引入；可作为后续草案。
- **`Self::name(...)` 在实例方法体内调用同类型静态 fn** —— §3.4 已列入"允许位置"但未细化语义（recursive call / vtable 类问题）；**[#3.A]** 视实施进度决定 v1 是否启用。
- **静态 fn 出现在 enum / draft 上** —— enum 当前不支持方法（§3.10.2.6）；draft 是否引入静态契约方法留给 `DRAFT-spec-unify`。
- **多 structImpl 块跨文件汇总** —— 沿用 §7.2.1.1 既有约定，本草案不改。

## 10. 迁移面

### 10.1 编译器（`src/`）

- `src/ast/ast_builder.cpp`：识别 `#Static` 注解、记录到 fn 节点；扩展 `ID::ID` parse-tree 处理路径。
- `src/sema/sema_pass.cpp`：新增 sema 分流（§4）；`Self` 类型在 fn 体内的绑定与查找；同名冲突诊断；新增诊断码更新 `kMigratedCodes`。
- `src/sema/call_resolve.cpp`：静态调用 overload resolution（不带 receiver 的版本）。
- `src/ast/mangler.cpp`：静态 fn mangle（§5）。
- `src/compiler/compiler_call.cpp`（或同等位置）：静态调用 codegen 走顶层 fn 路径，不传 `$`。
- `src/compiler/compiler_types.cpp`：`Self` 类型解析为当前 impl 块的所属类型。

### 10.2 SDK / runtime（`sdk/`）

- 试点：`String::empty()` / `Array::with_capacity(n)` / `Rc::of:<T>(x)` 等少量工厂（先**新增**，不重写既有 ctor 调用点）。
- `sdk/yux/src/yux/core/*.test.yux`：加 `#Test` 覆盖。

### 10.3 语法（`src/yux.g4`）

> 改语法属于**高风险动作**，按 CLAUDE.md 项目约束需先与用户确认；本节只列要改什么，不动手。

- `exprEnumCtor` 产生式扩展（或并入新规则）：在 `ID :: ID` 后加 `(SymbolColon genericDef)?` 与 `(ParStart ... ParEnd)`，让静态调用与 enum 构造共用同一节点；LHS 增加 `Self` 入口。
  - 现状：`enumName=ID SymbolColonColon variant=ID (ParStart args ParEnd)?`
  - 草案：`lhs=(ID|SelfTok) (SymbolColon lhsGenerics=genericDef)? SymbolColonColon rhs=ID (SymbolColon rhsGenerics=genericDef)? (ParStart args ParEnd)?`
- `type` / `typeWithRef` 增加 `Self` alt：
  - `type`：`... | SelfTok #typeSelf`
  - `typeWithRef`：`... | SelfTok SymbolAnd? #typeSelfWithRef`
- token 新增：`SelfTok: 'Self';`（**[#1.A]**）；`SymbolColonColon` 已存在不动。
- `Self` 加入附录 A 保留字表。

### 10.4 测试（`tests/`）

- 成功用例落 `sdk/yux/src/yux/core/<topic>.test.yux`（`yux test` 走 JIT）。
- 诊断用例：
  - `tests/cases/diag_static_self_in_body.yux` —— 静态 fn 体内写 `$`
  - `tests/cases/diag_static_dup_name.yux` —— 静态 fn 与实例 fn 同名
  - `tests/cases/diag_static_call_instance_fn.yux` —— `Type::instance_method()` 错调
  - `tests/cases/diag_static_not_found.yux` —— 未声明的静态 fn
  - `tests/cases/diag_static_in_draft_block.yux` —— 静态 fn 写在 draft 实现块内
- 各用例命名前缀 `diag_` 自动落到 `yux/diag` 分组。

### 10.5 规范文档（`docs/spec/`）

- `docs/spec/07-结构体.md`：新增 §7.10 "静态函数（关联函数）"；§7.2.1 / §7.2.2 / §7.3.2 各处加交叉引用。
- `docs/spec/06-函数.md`：在 fn 注解一节列 `#Static`；提一下 `Self` 类型。
- `docs/spec/11-编译期注解.md`：新增 `#Static` 条目。
- `docs/spec/附录A`：如决议把 `Self` 加为保留字，同步附录 A。
- `docs/spec/附录B`：`structImpl` / `expr` 产生式同步。
- `docs/spec/附录D-诊断.md`：新增诊断码（E3xxx 段位待定）。
- `docs/spec/CHANGELOG.md`：顶部追加一条，日期为合并日。

### 10.6 用户教程（`docs/`）

- `docs/结构体.md`：新增 "静态函数 `#Static` 与 `Type::fn` 调用"。
- `docs/函数.md`：交叉引用。

---

## 决议日志

- **[#0.A]** 识别方式选**注解 `#Static`**，不选隐式签名识别。理由：和 `#Frozen` / `#Const` 一致，单一通道；隐式识别会让"加/删一行 `$`"改变 fn 性质，太脆。
- **[#0.B]** 调用语法选 `::`，不选 `.`。理由：`SymbolColonColon` 已在 lexer，与 enum 路径形态统一；与 `obj.method` 视觉分明；turbofish 不与 `::` 冲突。
- **[#0.C]** 静态 fn 体内允许 `Self`。范围：返回 / 局部 / turbofish；不放 fn 形参 / 字段。
- **[#1.A]** `Self` **加保留字**（附录 A 同步）。与 `null` / `true` / `false` 同档"语法保留特殊记号"。lexer 新增一个 token；普通 ID 不得再用 `Self`。
- **[#1.B]** 静态 fn 仅在 structImpl 内，不开顶层 / draft / enum。enum 留给后续草案（v1 enum 不带方法块）。
- **[#1.C]** 静态 fn 不进 draft 实现块（§7.8.2.1 扩展）。draft 契约仅含实例方法签名。
- **[#2.A]** 静态 fn 与实例 fn 允许同名，sema 抛 warning（不阻断）。理由：`Type::foo` / `obj.foo` 调用语法天然分明，硬禁过严；warning 提示可读性问题已足够。与 ctor（即与结构体同名）仍禁，因为调用形态冲突。
- **[#5]** `#Frozen` 是字段 / 形参专用注解，**不可**标在 fn 上（含静态 fn）；这是 §7.1.4 / §6 既有约束的明确化，本草案不引入"`#Static` + `#Frozen`"组合议题。`#Static` 与 `#Const`（编译期常量函数）的组合**允许**，语义为"无副作用的工厂"。
- **[#3.A]** 实例方法体内 `Self::static_fn(...)` 调用糖：**v1 启用**。语义等同 `<当前类型>::static_fn(...)`；含泛型 struct 时 `Self` 绑当前单态化实参。
- **[#6]** `Self` 同时进入 **type 产生式**与**调用 LHS**：`type` 与 `typeWithRef` 各加 `Self` alt；`exprPathCall` 的 LHS 接受 `ID | Self`。`Self` 在 structImpl 体外出现一律 sema 报错。
- **[#7]** 范围扩展：砍同名 ctor（`fn StructName(...)` 形态废除），构造唯一通道为 `#Static fn` 返回 `Self`，内部用 `Self { .field = value }` 字段字面量产值。不留过渡期。本草案改名为 `DRAFT-construction-model.md`，§7.3 重写。
- **[#8]** 字段字面量 LHS 仅限 `Self`（v1）：`Self { .x = ... }` 合法；`Other { .x = ... }` 不合法，跨类型构造一律走 `Other::new(...)`。理由：把字面量当低层原语收紧到当前类型作用域，避免泄漏到普通表达式语法。
- **[#9]** α 形态定稿（probe 通过）：`Self { LineEnd (fieldInit | LineEnd)* }`，`fieldInit ::= . name = expr LineEnd`。多行强制 + `.` 前缀强制。
- **[#4.A]** AST 节点：**复用 `ExprEnumCtorNode` 改名为 `ExprPathCallNode`**，承载 `enum::variant` 与 `Type::static_fn` 两条 sema 分流。enum 路径不动语义，仅跟随节点改名。

---

## 定型与归宿

草案定型后按 §10.5 列出的章节回写 spec；CHANGELOG 顶部追加一条；`CURRENT.md` 的 Phase 列表从 §10 派生，全部完成后归档到 `docs/dev/static-fn-impl-log.md`（剔除人名 / 路径 / 行号 / 测试计数）。本 DRAFT 文件保留为历史档并在头部标注「已落地，见 §7.10」。

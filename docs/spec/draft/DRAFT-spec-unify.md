; 草案：yux spec / 声明合一 / 编译期反射

# 草案：yux spec 形态、声明合一、编译期反射

状态：**v1 已落地（2026-05-19）**。权威规范见 `docs/spec/12-spec.md`；实施记录见 `docs/dev/spec-unify-impl-log.md`。本草案保留为历史档 + 占位章节随各独立草案（`DRAFT-spec-default-body.md` / `DRAFT-spec-reflect.md` / `DRAFT-spec-fields.md`）落地后逐步处置。原始日期：2026-05-18（基于 2026-05-15 初稿刷新；v1 范围收紧见 [#1.AD]）。
作用（**v1 收紧**）：**替代 `draft` 关键字**——用 `#Spec` 注解的 struct 描述接口、用 `#Impl(Spec)` 顶行注解声明实现、把方法体并入 struct 声明体（取消独立 impl 块）、删 `#DraftLike` / spec 体内追加 `Self` 抽象变量语义。**默认方法体 / 反射 / `#Reflect` / `#Impl` fall-through 全部形态占位、不实施**，分别独立草案落地（[#1.AD]）。

> 本草案是 v0.5 `draft` + v0.11 `Dyn<D>` 的**重新组织**，**不取消语义**：现有"显式 `: Draft` 实现、单态化分发 + Dyn fat pointer"两条路径全部保留，只重写声明形态。
>
> **与 static-fn 已落地协议的交接**：`Self` 关键字、`Self { ... }` 字段字面量、`Type::name(...)` 静态调用、构造唯一通道 `#Static fn make/from/new + Self { ... }`（详见 `docs/dev/static-fn-impl-log.md` 与 `07-结构体.md` §7.10）已是基线，本草案 §5（`Self` 关键字）只追加"在 spec 内的抽象类型变量语义"，不改 §7.10 既有规则。本草案落地后 spec 实现者**不**通过同名 ctor 满足契约——所有"返回 Self 的工厂"都是 `#Static fn`。

涉及章节（预估）：§07 结构体、§12 draft、§11 编译期注解、附录 A 保留字（加 `Self`）、附录 B 语法汇总。

---

## 1. 目标

**v1 范围收紧（[#1.AD]）**：本草案 v1 = **替代 draft 关键字**。默认方法体、自动 fall-through、反射、`#Reflect` 注解全部**降级为形态占位**，独立草案分别落地。

v1 实施：

- 接口 / 抽象规范用 `#Spec` 注解的 struct 表达，砍掉 `draft` 关键字 / `Draft` token / `draftDecl`。
- 实现声明用 `#Impl(Spec)` 注解（顶行）；spec 内声明的方法实现者**必须全部写体**（v1 无 fall-through）。
- 声明 + 实现合一：struct 体内字段段在前、方法段在后，**取消独立 impl 块**（`structImpl`）。
- 砍掉 `#DraftLike` 结构化匹配——一种通道、一种语义。
- spec 体内追加 `Self` 抽象类型变量语义（`Self` 关键字 token 已由 static-fn 落地）。

v1 仅锁形态 / 不实施（**占位章节**）：

- §4 spec 默认方法体 → [#1.AD] / `DRAFT-spec-default-body.md`
- §6 内置 spec `Reflect` + 运行时反射 → [#1.AD] / `DRAFT-spec-reflect.md`
- §7 `#Reflect` 强制 emit 注解 → 同上
- §8 `#Impl` 无方法体即 fall-through（[#1.AA]）→ 依赖 §4，同期延后

**永不引入**（[#1.AE]）：

- `#Inline for` 编译期循环 unroll + 任何 IR-before unroll pass —— 增加编译器复杂度，性价比低
- 异构类型按字段递归的"自动 derive" 默认体（如 `ToJson.to_json` 自动按字段）—— 实现者必须**手写体**，或用 `Self::fields` runtime 数组 + `Dyn<ToString>` 等同质化形态自己写循环

v1 落地后 yux 还**不能**："给 spec 写默认实现"、"反射一个类型拿字段表"、"用 `#Impl(ToJson)` 不写体自动 derive"。但语法 / 概念面已就位，后续草案不再动 g4 / 大改 sema。

## 2. 三块概念总表

| 块 | 现状 | 草案 v1 | 状态 |
|---|---|---|---|
| 接口声明 | `draft Foo { fn m() }` | `#Spec struct Foo { fn m() }`（v1 仅 fn 段，[#1.Q]） | **v1 实施** |
| 类型方法 | `struct C { ... }` + `C : Foo { fn m() {...} }` | `#Impl(Foo)` + `struct C { f T / fn m() {...} }` 单体合一 | **v1 实施** |
| Self 类型（spec 体内）| 仅 `Dyn` 受限场景 | spec 体内抽象类型变量；其它位 static-fn 已落地 | **v1 实施** |
| 结构化匹配 | `#DraftLike` | **删除** | **v1 实施** |
| 默认实现 | 无 | spec 内 `fn m() body { ... }` | 占位 / [#1.AD] |
| 编译期反射 | 无 | 内置 spec `Reflect`（仅类型形）| 占位 / [#1.AD] |
| 运行时反射 emit 强度 | 无 | `#Reflect` 防 DCE | 占位 / [#1.AD] |

## 3. 子特性 A — `#Spec struct` 与 `: Spec` 实现

### 3.1 spec 声明

```yux
#Spec
struct ToString {
  fn to_string() String
  fn to_debug() String {                ; 带 body = 默认实现
    "<" + $.to_string() + ">"
  }
}

#Spec
struct Cloneable {
  fn clone() Self                       ; Self 返回，见 §5
}

#Spec
struct Iter<T> {                        ; 泛型 spec
  fn next() T?
}

#Spec
struct Default {                        ; spec 含 #Static fn 工厂签名
  #Static
  fn default() Self
}
```

约束：

- `#Spec struct` 不可被实例化、不可装 `Rc` / `Array` / `Dyn`（除 `Dyn<Spec>` 这一形态）。
- v1 spec body 内**只有 fn 段**（[#1.Q]），不接受字段段——含字段段的 spec（结构匹配语义）独立草案 v1.x 处理；字段抽象当前由 `Self::fields` 编译期反射（§6）覆盖。
- 方法段：仅签名 = 必须实现；签名 + body = 默认实现（实现者可覆盖）。
- spec 内**不**声明析构函数（语法层拒绝 `fn ~()`）。
- spec 内**允许** `#Static fn` 签名作工厂契约（如 `default() Self`），与 static-fn 协议对齐——构造唯一通道走 `#Static fn make/from/new` 形态。spec 内 `#Static fn` 体不可用 `Self { ... }` 字段字面量（spec 无字段段），带 body 的 `#Static fn` 默认实现必须通过调用其它 spec 方法或转调具体类型 `Self::xxx(...)` 完成构造。

### 3.2 实现：`#Impl(Spec)` 顶行注解 + 声明合一

```yux
#Impl(Cloneable)
#Impl(ToString)
struct Counter {
  ; ===== 字段段（必须在最前）=====
  value i32
  #Mut
  step i32

  ; ===== 方法段 =====
  fn ~() {
    ; ...
  }

  #Static
  fn make(v i32) Self {
    Self {
      .value = v
      .step = 1
    }
  }

  fn increment() {
    $.step = $.step + 1
  }

  fn clone() Self {
    Counter::make($.value)
  }

  fn to_string() String {
    ; ...
  }

}
```

多 spec 一律**顶行堆叠**（`buildAnno` 单参形态，见 §10.3 / [#1.O]）：

```yux
#Impl(Cloneable)
#Impl(ToString)
struct Counter { ... }
```

不支持单注解多参 `#Impl(Cloneable, ToString)`——未来 anno-struct 草案（[#1.P]）若决议放开 named-args / 多字段注解，再行考量；spec-unify v1 仅堆叠。

g4 改动方向：

```
structDecl:
    Struct name=ID generics? StructStart
        filedDecl*           ; 字段段
        fnDecl*              ; 方法段（含 #Static fn / dtor / spec 实现）
    StructEnd
    ;
```

—— `: Spec` 头从 g4 删除；spec 实现关系由 `#Impl(...)` 顶行注解承载（解析期由 `buildAnno` 收 + sema 期入 spec registry）。字段后首次遇到 `fn` 后，再出现字段是语法错。

### 3.3 单一实现源 + extension blocks 形态预留

**v1 决议**：每个类型的全部行为在它出生的文件 / struct 体内写完。取消 v0.5 已有的"`Counter : ToString { ... }` 独立 impl 块"形态。后果：

- 不能给 SDK 的 `Array` / `i32` 在外部源文件实现"自定义 spec"。
- 想给外部类型加行为：用 wrapper struct 包一层（newtype + 零开销字段透传）。

**v1.x 形态预留**：未来包管理落地后，引入 `extend` 顶层声明开放"外部 spec 实现外部类型"路径，形态如下，**本草案锁形态、不实施**：

```yux
#Impl(MyDisplay)
extend Array<i32> {
  fn display() String {
    ; ...
  }
}
```

要点：

- `extend` 是新增顶层声明，**不是** struct body 的延伸——与"声明合一"决议不冲突。
- `#Impl(...)` 顶行注解形态与 §3.2 完全一致——`#Impl` 注解的语义对 `struct` 和 `extend` 头通用。
- 关键字净 +1（`extend`）。本草案不引入 `extend` token、不写 g4，但在附录 A 预留为**未来保留字**，避免有人现在用 `extend` 命名标识符。
- 未来 orphan rule（如"`#Impl` 注解的 spec 或 `extend` 目标类型，至少一方在本包"）等包管理落地再加约束；当前 v1 阶段连 `extend` 语法本身都不通过，问题不存在。
- 独立草案占位：`DRAFT-extension-blocks.md`（v1.x 启动时建）。

### 3.4 结构化匹配 `#DraftLike` 删除

`#DraftLike` 与"明确"冲突——类型可以"无心 satisfy"一个它从不知道的 spec。取消。

- 唯一实现路径：显式 `#Impl(Spec)`。
- 不显式声明 = 不算实现，即使方法签名碰巧匹配。
- 内置 `ToString` 等 spec 在 base.yux 内显式 `#Impl(ToString)` + `struct i32 { ... }` 形态（SDK 内对内置类型走相同形态；具体写法在 SDK 迁移期定型，可能需要为内置标量类型先创建宿主 struct）。

## 4. 子特性 B — 默认方法体（**v1 占位 / 不实施**，[#1.AD]）

> **v1 状态**：spec body 内方法**仅签名**——`fn m() ReturnType LineEnd`；带 body 的形态 g4 同期保留产生式槽位（fn body 可选）但 sema 期拒收（E1138 占位）。
> 实施留给独立草案 `DRAFT-spec-default-body.md`（涵盖默认体 typecheck 时机、`Self` 抽象变量在默认体内的替换、组合 spec 默认体冲突 E3132、[#1.S] / [#1.T]）。
> 后果：v1 阶段 `#Impl(Ord)` 实现者必须**全部写体**（cmp / lt / le / gt / ge 五个），无 fall-through。

未来形态（**不实施**，仅占位示意）：spec 内方法可带 body，等价于"实现方可继承的默认实现"。

```yux
#Spec
struct Ord {
  fn cmp(other Self&) i32
  fn lt(other Self&) bool = $.cmp(other) < 0
  fn le(other Self&) bool = $.cmp(other) <= 0
  fn gt(other Self&) bool = $.cmp(other) > 0
  fn ge(other Self&) bool = $.cmp(other) >= 0
}
```

实现方只需写 `cmp`，其余四个自动 fall-through 到默认体。覆盖时只需在 struct 体内重写同名 fn。

约束：

- 默认体内能引用的：`$`（receiver）、其它 spec 内方法签名（含尚未实现的，因为是抽象类型变量）、`Self::type` / `Self::fields` 等编译期反射静态字段（类型形访问，无 `()`）。
- 默认体内**不能**引用：`$.fields` / `$.type` 等"实例形反射"（实例不能调静态字段 / 静态函数，[#1.AB]）；spec 内字段的具体 layout（offset / size）在表达式位（仅 `f.offset` 编译期常量可用）。

## 5. 子特性 C — `Self` 类型关键字（spec 语义追加）

> **`Self` 关键字本身已落地**（static-fn Phase 1.A，附录 A.1）。本节只追加"spec 体内的抽象类型变量"语义，不重复 `07-结构体.md` §7.10.3 已收的条款。

### 5.1 语义（spec 内追加）

`Self` 是类型位关键字。已有语义：在 `structImpl` 体内绑定为所属结构体类型（含泛型实参当前绑定）。本草案追加：

- 在 `#Spec struct` 体内：**抽象类型变量**，每个实现者替换为它自己。
- 在 `fn<T : S>` 函数体内：不直接出现，应写 `T`（既有规则）。
- `: Spec` 头被 `#Impl(Spec)` 注解取代后，sema 把 spec 体内 `Self` 的抽象类型变量按"哪些 struct `#Impl(this_spec)`"逐个特化展开（默认体合成路径，见 §4）。

### 5.2 合法位置

| 位置 | mono `<T:S>` | `Dyn<S>` |
|---|---|---|
| receiver | ✅ | ✅ |
| 参数 `fn eq(other Self&) bool` | ✅ | ❌ E1134（解锁待 thunk） |
| 返回 `fn clone() Self` | ✅ | ❌ E1134（解锁待 thunk） |
| 字段类型 `Rc<Self>` | ✅ | ❌ |
| `Self::fields` 等反射静态字段 | ✅ | ✅（vtable 携带 type 元数据指针）—— 形态占位，反射 v1 不实施 |

`Dyn<S>` 上的 `Self` 限制沿用 v0.11 已有规则（E1134），解锁路径见 MILESTONE 后续主题。

## 6. 子特性 D — 反射（**v1 占位 / 不实施**，[#1.AD]）

> **v1 状态**：编译器不生成 `Reflect` spec / 不隐式 `#Impl(Reflect)` / 不 emit `.rodata` Type 节点。`Counter::type` / `Self::fields` 等访问在 v1 报"未声明"普通名解析错（与未定义符号一致），不引入专门错码。
> 实施留给独立草案 `DRAFT-spec-reflect.md`（涵盖 `Reflect` spec 声明、`#Static #Frozen` 字段子集、`Type` / `Field` / `Method` / `Variant` 内置数据类型、`.rodata` emit、`#Reflect` 防 DCE、实例形访问 E1137、`Field.value` sema 期改名、[#1.Y] / [#1.Z] / [#1.AB] / [#1.AE]）。
> 形态在本章节锁住，不留漂移空间。

> **形态决议**（[#1.Y] / [#1.Z] / [#1.AB]）：反射通过内置 spec `Reflect` 暴露；编译器为每个类型隐式 `#Impl(Reflect)`；**仅类型形** `Counter::type` / `Self::fields` 访问（无 `()`），实例形 `c.fields` / `$.fields` 不支持（[#1.AB]：实例不能调静态字段 / 静态函数）。数据落 `.rodata`。**不引入** `fields_of(...)` 等内置函数。

### 6.1 API（Reflect spec）

`Reflect` 的 spec body 只声明 **`#Static #Frozen` 字段**。形态与普通用户 spec 一致（[#1.Y]）——spec body 允许 `#Static` 字段是通用规则（[#1.Q] 例外条款，[#1.Z]），不是 Reflect 专用。区别只在编译器为每个类型隐式 `#Impl(Reflect)` 并自动填充这 4 个字段；用户 spec 仍走显式 `#Impl` 路径。

```yux
; base.yux
#Spec
struct Reflect {                          ; 编译器隐式为每个类型 #Impl(Reflect)，自动填充
  #Static
  #Frozen
  type Type&                              ; 当前类型的 Type 节点
  #Static
  #Frozen
  fields [Field& * 0]&                    ; Field& 元素指向 rodata；0 = 编译期占位，按 implementer 自动填长度
  #Static
  #Frozen
  methods [Method& * 0]&                  ; 0 占位，同上
  #Static
  #Frozen
  variants [Variant& * 0]&                ; 0 占位；仅 enum；非 enum 访问 → E3135
}
```

使用形态（**仅类型形**，[#1.AB]）：

| 场景 | 写法 |
|---|---|
| 拿 Type 节点 | `Counter::type` |
| 遍历字段（runtime 数组）| `Counter::fields` / `Self::fields` —— 普通 `[Field& * N]&` 数组，可索引 / 取 len / `for` 遍历 |
| 在方法体内 | `Self::type` / `Self::fields`（**不**写 `$.type` / `$.fields`） |
| `Field.value` 改名 | `Counter::fields[0].value` → `$.<f.name>`（sema 改写，f 必须编译期可定，[#1.AE]） |

合法性规则：

- ✅ `Counter::type` / `Self::type`（类型形 static field 访问）
- ✅ `Counter::fields` / `Self::fields` —— 类型为 `[Field& * N]&`，常规 array 形态可索引、可取 len
- ❌ `c.type` / `c.fields` / `$.type` / `$.fields` —— 实例上调静态字段 → E1137（[#1.AB]）
- ✅ `#Mut let t Type& = Counter::type`（runtime ref 变量；`#Frozen` 由静态字段类型保证 deep-immutable）
- ✅ `fn dump(#Frozen t Type&) { ... }`（runtime 形参，`#Frozen` 形参对接 `#Frozen` 字段）
- ❌ `#Mut let t Type = Counter::type`（按值取 rodata 单例，复制无意义；E3136 占位）
- ✅ `if Counter::type == OtherType::type { ... }`（编译期 if，`==` 由 Type 内置 `#Const fn`）

### 6.2 Type / Field / Method / Variant 形状

`Type` / `Method` / `Variant` 是普通 struct（不是 `#Spec`——它们是反射数据载体，要可被实例化在 rodata 上），可纯 yux 写在 base.yux：

```yux
struct Type {
  #Frozen
  name String                            ; rodata 字面量串
  #Frozen
  fields [Field& * 0]&                   ; 0 = 编译期占位，按具体类型字段数填
  #Frozen
  methods [Method& * 0]&                 ; 0 占位，同上
}
```

`Field` 是**唯一编译器内置反射类型**（[#1.X]）——`.value` 字段类型按迭代位 + 是否绑 receiver 动态决定（dependent-typed phantom field），yux 类型系统当前无法表达，因此 base.yux 仅留 `#CompilerInner` stub：

```yux
#CompilerInner          ; 编译器内置：.value phantom 由 sema 注入
struct Field {
  #Frozen
  name String
  #Frozen
  type Type&                              ; 该字段的类型节点
  #Frozen
  offset usize                            ; layout 内省
  ; value <field 实际类型>                ; phantom：仅当 f 编译期可定时可用（如 Counter::fields[0].value）
  ;                                       ; 由 sema 改写为 $.<f.name>
  ;                                       ; 形态锁、本草案 v1 不实施；详见 §6.4 / [#1.W] / [#1.AE]
}
```

注：`Field.name` 等 String 字段是 .rodata 中的常量串（用 `#Frozen` 标记 + 编译器在 emit 时分配 rodata 槽）。本草案不引入 `cval String` 区分（[#1.V] 收回——既然反射数据全 .rodata 落地为 `#Frozen`，编译期可读 / 运行期不可写已由 const-mut 保证，`cval` 修饰多余）。`#Const fn` 仍是函数纯洁性独立维度，与本节无关。

### 6.3 字段遍历机制：runtime 反射 + sema 期 `f.value` 改名（[#1.AE]）

**核心决议**：反射 = **运行时反射**——`Self::fields` / `Counter::fields` 是普通 `[Field& * N]&` runtime 数组；用普通 `for` 遍历、可索引、可取 len。**不引入** `#Inline for` 编译期 unroll / IR-before pass。

`Field.value` 的特殊性是 **sema 期纯改名**（不引入运行时机制）：当 `f` 是编译期可确定的具体 Field 引用（如 `Counter::fields[0]`，下标位字面量）时，`f.value` 被 sema 改写为 `<receiver>.<f.name>`。否则报错——`f` 运行期才能定时，`.value` 类型无法静态确定（异构）。

后果（与"自动 derive"形态的关系）：

- ✅ 手写每字段访问的 spec 实现（`Counter::fields[0].value` / `[1].value` ...）—— 实现者麻烦但可行
- ✅ `Reflect` 静态字段 API 完整可用，工具链 / debug / dump 等读 `Counter::fields[0].name` 之类编译期常量访问形态可用
- ✅ 同质字段类型场景（所有字段都 `Dyn<ToString>` 或 `i32` 等）可走普通 `for` 遍历 + `.value` 必须仍在编译期下标位
- ❌ "异构类型 + 单遍循环"的自动 derive（`ToJson.to_json` 自动按字段递归调 `to_json`）—— `for f in Self::fields { f.value.to_json() }` 报错，因为 `f` 运行期才知道是哪个字段，`.value` 类型不定
- 因此 5 件套中 `ToJson.to_json` / `Eq.eq` / `Clone.clone` 等按字段递归形态的默认体**永不引入** —— 实现者手写

### 6.4 字段访问 `f.value` —— sema 期改名（[#1.W] / [#1.AE]）

**形态**：通过 `Field.value` 访问当前位置字段的实际值，**无新语法**——按普通字段访问规则解析。

`f.value` 的 sema 规则：

| 上下文 | `f.value` 解析 |
|---|---|
| `f` 是编译期可确定的具体 `Field&`（如 `Counter::fields[0]`、`Self::fields[N]` 下标位字面量） | 改写为 `$.<f.name>`；类型 = 该字段实际类型 |
| `f` 是 runtime 变量（如普通 `for f in Self::fields` 内的循环变量） | E3133"`Field.value` 要求 f 编译期可定" |
| 在普通方法体外（无 `$` receiver 上下文）| E3134"`Field.value` 改写无 receiver 绑定" |
| 写 `f.value = expr` | 等价 `$.<f.name> = expr`，受常规 const-mut 检查（`#Mut` 字段才允许写）|

显式 receiver 形态（**可选扩展**，本草案锁形态不实施 / 待 reflect 草案讨论）：是否支持 `<other_receiver>::fields[0].value` → `<other_receiver>.<f.name>` 改写，留 reflect 草案落地时决定。v1 仅 `$`（方法体内）。

理由：

- 无新语法 = sema/parser 无需引入"编译期前缀符号在字段位"的特殊路径
- 无 IR-before pass = 编译器复杂度不上去（[#1.AE]）
- `Field.value` 是 sema 改写产物，不是真实运行时字段——base.yux 仍保留 `#CompilerInner` Field stub（[#1.X]），LSP 补全 / hover / doc 三套硬编码这一字段
- 其它三个反射类型（`Type` / `Method` / `Variant`）不受影响，可纯 yux 写

## 7. 子特性 E — `#Reflect` 注解（**v1 占位 / 不实施**，[#1.AD]）

> **v1 状态**：`#Reflect` 注解 lexer / parser 不识别（与未知 `#XXX` 注解一致报错）。形态与反射本体一同推到 `DRAFT-spec-reflect.md`。

形态决议变化（[#1.Y]）：反射数据**默认 emit + DCE 兜底**——`Counter::type` 若全程未被引用，链接期 `--gc-sections` 自动删 `.rodata` 项。`#Reflect` 注解的语义因此从"opt-in 开放反射"退化为"强制 emit + 防 DCE 误删"，适用于 dump / debug / serialize 等"工具按需读 type 元数据"场景。

```yux
#Reflect             ; 标该类型的反射数据强制 emit，链接期不被 DCE
struct User {
  name String
  age i32
}
```

`#Reflect` v1 零参；未来 anno-struct 落地后（[#1.P]）若决议精细化（`#Reflect(name)` 仅强制 emit name 字段、其它 DCE 可删），形态走 named-args bool 字段。spec-unify v1 不实施精细化。

`#NoReflect` 反向注解（强制不 emit，纯编译期）未在 v1 实施，未来按需引入。

约束：

- 默认所有类型反射数据 emit + `--gc-sections` DCE 兜底——未被引用的 Type / Field 节点链接期自动删。
- 标了 `#Reflect` 的类型，反射数据**防 DCE**——即使 `Counter::type` 全程未被 yux 源码引用，链接期保留。适用于 dump / debug / serialize 等"工具按 .rodata 符号名读"场景。
- 描述符走 linkonce_odr emit + 链接期 `--gc-sections` DCE 兜底。
- `#Reflect` 不打开 runtime 方法 dispatch——只保留方法名 / 签名元数据可读。runtime 调方法仍需通过 `Dyn<Spec>`。

**`#Reflect` 是 emit 强度选择**，不是反射 API 的开关——API 由内置 spec `Reflect` 隐式 `#Impl` 永远可用（[#1.Y]）。

## 8. 子特性 F — 唯一 `#Impl`，默认体自动 fall-through（[#1.AA]，**v1 占位 / 不实施**，[#1.AD]）

> **v1 状态**：v1 实现者必须**全部写体**——spec 里声明几个方法，`#Impl` 处就实现几个；漏一个报 E1136"spec 方法未实现"。**`#Derive` 永不引入**仍是 v1 决议（[#1.AA]）；fall-through 机制本身随 §4 默认体延后。

**不引入** `#Derive(Spec)` 独立注解——`#Impl(Spec)` 即承载两种用法：

```yux
#Impl(Ord)
struct Counter {
  value i32
  step i32

  fn cmp(other Self&) i32 {           ; 仅实现 cmp，其它 lt/le/gt/ge 走默认体 fall-through
    $.value - other.value
  }
}
```

vs 显式覆盖：

```yux
#Impl(Ord)
struct Counter {
  value i32
  step i32

  fn cmp(other Self&) i32 { $.value - other.value }
  fn lt(other Self&) bool { false }     ; 显式覆盖默认体
}
```

规则：

- 实现者写了同名 fn → 覆盖 spec 默认体
- 没写 → spec 默认体 fall-through（克隆 AST + 替换 Self + 走常规 typecheck，[#1.S]）
- spec 内只有签名无默认体 + 实现者没写 → 报 E1136"spec 方法未实现"

> base.yux 内置 5 件套（ToString / ToJson / Eq / Ord / Clone）的"无方法体即 derive"形态对**带默认体的方法**（如 `Ord.lt/le/gt/ge` 由 `cmp` 推、`Eq.ne` 由 `eq` 推）可工作；对**需遍历字段才能合成的方法**（`ToJson.to_json` / `Eq.eq` / `Clone.clone` 自动按字段递归）则需要 `#Inline for` + `f.value`，本草案 v1 不实施（[#1.AC]）。v1 阶段这些方法用户必须显式手写体，或等 `#Inline for` 草案落地后由 spec 默认体自动 fall-through。

## 9. 不在范围

- **v1 收紧后从本草案剥出的两块（[#1.AD]）**：
  - spec 默认方法体 + 实现者 fall-through → `DRAFT-spec-default-body.md`
  - 内置 spec `Reflect` + 运行时反射 + `Field.value` sema 改名 + `#Reflect` 注解 → `DRAFT-spec-reflect.md`
  
  本草案 §4 / §6 / §7 / §8 锁形态，不实施；上述两草案启动顺序：default-body → reflect。
- **`#Inline for` 编译期循环 unroll + IR-before unroll pass**（[#1.AE]）：**永不引入**——增加编译器复杂度，性价比低。反射走 runtime 数组形态（普通 `for` 遍历，§6.3）；`Field.value` 走 sema 期纯改名（编译期可确定的具体 Field 引用，§6.4）。副作用：异构类型按字段递归的自动 derive 默认体（`ToJson.to_json` 等）**永不引入**，实现者手写。
- **实例形反射 mirror**（`c.fields` / `$.fields` / `c.type`）：[#1.AB] 决议——实例不能调静态字段 / 静态函数；类型形 `Counter::type` / `Self::fields` 是唯一通道，不引入实例形 sema 注入。
- **extension blocks**（`extend Type { ... }` orphan 实现）：形态已预留（§3.3 / [#1.M]），独立草案 `DRAFT-extension-blocks.md`，v1.x 包管理时启动。
- **spec 含字段段**（[#1.Q]）：v1 spec 内**只允许 fn 段**，含字段段的 spec（§3.1 `Point<T>` 例已删）独立草案 `DRAFT-spec-fields.md`，v1.x 启动。字段抽象当前由 `Self::fields` 编译期反射（§6）覆盖。
- **关联类型**（[#1.R]）：v1 仅类型参数 spec（`#Spec struct Iter<T>` 形态），不引入 `type Item` 关联类型机制。同一类型可对不同 T 多次实现。独立草案 `DRAFT-assoc-types.md`，v1.x 启动。
- **注解 struct 体系 / 命名参数 / 字段默认值 / LSP 注解补全**（[#1.P]）：用户可定义注解 struct（`#Annotation struct Api { preffix String; ... }`）+ named-args（`#Api(preffix="/api")`）+ 字段默认值 + LSP 字段补全，是覆盖范围远超 spec-unify 的前置基础设施，独立草案 `DRAFT-anno-struct.md`。本草案锁前向兼容（[#1.O]：buildAnno 单参 + literal/type ref），未来 anno-struct 落地时仅 `#Reflect` 形态需迁，其它无破坏。
- mixin / 字段-方法包：spec 默认体 + 字段字面量已覆盖核心用例，不重复造概念。
- procedural macros / 任意编译期执行：永久禁。
- 编译期方法 dispatch（"调一个 cval Type 上的方法"）：需要 dependent typing 风元编程，暂不做。
- 字段名构造 `Self { .name = x, .age = y }` 语法：已由 static-fn 落地（[#1.K]），本草案不再独立设计。
- runtime 方法 dispatch（不通过 Dyn）：永不开放。
- `#Reflect(methods)` 是否包含默认体 signature：v1.x 决定。

## 10. 迁移面（粗估）

### 10.1 编译器（`src/`）

- `src/ast/`：`structImpl` 节点删除，并入 `structDecl`；字段后 fn 段在同一 body 内顺序解析。
- spec 实现关系从原 `: Spec` impl 头迁到 `#Impl(...)` 顶行注解；`buildAnno` 实参槽**仅扩单参 ID→literal/type**（[#1.O]），不引入多参。多 spec / 多 flag 一律顶行堆叠 `#Impl(A)`\n`#Impl(B)` / `#Reflect(name)`\n`#Reflect(fields)`。
- `#Spec` 注解的结构化语义：建表"哪些 struct 是 spec 形态"，spec 不进 codegen 的常规通路。
- `#Impl(Spec)` 注解的 sema 路径：将"struct → 它实现的 spec 列表"入 spec registry，复用原 `analyzer/draft_impl_checker.{h,cpp}` 大部分逻辑（重命名为 `spec_impl_checker`）。
- spec 默认体的语义抓取：当类型实现 spec 但未提供方法体时，从 spec body 提取默认体合成。
- 已完成（无需迁移）：`Self` lexer token、`SelfType` parser 产生式、`structImpl` 体内 `Self` 绑定、`Self::name(...)` 静态调用糖、E1134 对象安全。
- spec 体内 `Self` 抽象类型变量：sema 期单态化展开（每次为某具体类型合成默认体时替换）。
- 反射数据：编译器为每类型隐式生成 4 个 `#Static #Frozen` 字段（`type` / `fields` / `methods` / `variants`），值指向 .rodata 中 emit 的 Type / Field 节点。**不引入** `type_of` / `fields_of` 内置函数（[#1.Y]）。
- 实例形访问拦截：`expr.type` / `expr.fields` 等在 sema 期检查 LHS 为实例 + RHS 是 `#Static` 字段 → 报 E1137（[#1.AB]）；提示用 `<Type>::field` 形态。
- `Field.value` sema 期改名：识别 `<staticFieldsExpr>[<intLit>].value` → `<receiver>.<name>` 改写；`f` 非编译期可定 → E3133。**不引入** IR-before pass / unroll 机制（[#1.AE]）。
- `#Reflect(...)` 注解处理：emit .rodata 描述符表，linkonce_odr。
- 删除 `draft` 关键字、`Draft` token、`draftDecl` / `draftType` / `structImpl` 文法、`#DraftLike` 注解、`analyzer/draft_registry.h` / `ast/node/draft_node.h`（或归并入 spec 等价物）。

### 10.2 SDK / runtime（`sdk/`）

- 全仓 draft 改 `#Spec struct`：`draft ToString { ... }` → 顶行 `#Spec` + `struct ToString { ... }`。
- `: Draft1 + Draft2 { ... }` impl 块合并进 struct 声明体，头部改顶行 `#Impl(Draft1, Draft2)`（或两行 `#Impl(...)` 堆叠）。
- `#DraftLike` 注解全删，对应 spec 改为显式 `#Impl`（base.yux 内对内置类型的实现形态待定，可能引入"内置类型宿主 struct"或为 lexer 内建类型 token 开 sema 例外）。
- 默认体补全：`ToString.to_debug` / `Ord.lt|le|gt|ge` / `Eq.ne` 等等。

### 10.3 语法（`src/yux.g4`）

> 改语法属于高风险动作，需先与用户确认；本节只列方向。

- `structDecl`：合并 `structImpl`，body 内 `filedDecl*` 后接 `fnDecl*`；头部移除 `: specList` 槽位（实现关系搬到 `#Impl` 注解）。
- `draft` 关键字删除；`Draft` token 退役；`draftDecl` / `draftType` / `structImpl` 产生式删除。
- 已完成（无需再动）：`Self` 关键字 token、`SelfType` 在 type 位/字段字面量位/path-call LHS 位的认可。
- type 位无新增——`Reflect` API 全走 `T::name` 静态字段访问形态，复用 `pathCall` 既有产生式（无 paren 分支由 static-fn Phase 已落地）。
- 注解 `#Spec` / `#Impl(...)` / `#Reflect` / `#Inline` / `#Mut`：复用 `buildAnno`；扩展只动单参 arg 形态——`arg=ID` → `arg = literal | ID genericDef?`（[#1.O]），即接受 spec 含泛型 ref（`#Impl(Iter<i32>)`）与字面量（`#Api("/api")` 占位，主用例由 anno-struct 草案承接）。**不引入多参**；本草案不动 `annoArg` 之上的子产生式形态，未来 anno-struct 草案再扩 `kvArg` 等分支。`#Derive` 不在本草案（[#1.AA] 砍）。

  g4 草拟（参考形态，最终以 Phase 1 用户确认为准）：
  ```antlr
  buildAnno:
      SymbolHash name=ID
      (ParStart arg=annoArg ParEnd)?
      LineEnd
      ;

  annoArg:
        ID genericDef?
      | literal
      ;
  ```
- 附录 A 预留 `extend` 为未来保留字（不引入 token、不入 lexer，仅文档层声明）。

### 10.4 测试

- 全仓 yux 用例：`draft` → `#Spec struct`，impl 块合并。
- 新增 `tests/cases/diag_spec_*.yux`：字段后字段（语法错）、`#DraftLike` 残留（拒收）、`Self` 用错位置、实例形访问静态字段 `c.type` / `$.fields`（E1137）。
- 新增 `sdk/yux/src/yux/core/reflect.test.yux`：`Counter::fields` 索引访问 / `Counter::fields.len()` / `Counter::type.name` 编译期使用、`#Impl(Ord)` 仅写 `cmp` + `lt/le/gt/ge` fall-through 端到端、实例形 `c.type` 触发 E1137。**不**测 unroll（推到 inline-for 草案）。

### 10.5 规范文档

- `07-结构体.md`：声明 + 实现合一新形态。
- `12-draft.md`：整章重写为 spec 章节，或拆为 §12 spec + §13 反射。
- `11-编译期注解.md`：新增 `#Spec` / `#Impl` / `#Reflect` / `#Inline` / `#Mut`。
- 附录 A：删 `draft` / `var` / `val` / `cval`（与 let 草案联动），加 `Self` / `let`。
- 附录 B：同步语法。
- `CHANGELOG.md` 顶部追加。

### 10.6 教程

- `结构体.md` / `Lambda与闭包.md`（若涉及）：spec 形态新写法。
- 新增 `编译期反射.md`：`Reflect` spec / `Counter::type` / `Self::fields`（runtime 数组遍历）/ `Field.value` sema 改名 / `#Reflect` 用户视角。

---

## 决议日志

- **[#1.A]** spec 表达为 `#Spec struct`，砍掉 `draft` 关键字。理由：与"统一"哲学一致（接口和数据共用 struct 形态）；net -1 关键字；spec 内允许字段段统一接口与抽象数据。
- **[#1.B]** 声明 + 实现合一，取消独立 impl 块。理由：找类型行为只看一个地方；语法 forced fields-first 杜绝穿插 / 重复声明。
- **[#1.C]** 单一实现源——orphan 写法砍掉，外部类型靠 wrapper struct。理由：v1.0 之前生态尚未建立，orphan 用例 ≈ 0；先紧后松。
- **[#1.D]** `#DraftLike` 结构化匹配删除。理由：与"明确"冲突——类型可"无心 satisfy"未知 spec。一种通道、一种语义。
- **[#1.E]** spec 内方法允许带 body 作默认实现。理由：吃掉 75% mixin / derive 需求；不需要任何编译期 codegen 机制。
- **[#1.F]** `Self` 类型关键字引入，覆盖 spec 返回 / 参数 / `Self::FIELDS`。在 `Dyn<S>` 上的限制沿用 v0.11 E1134。
- **[#1.G]** 编译期反射 P1：通过内置 spec `Reflect` 隐式 `#Impl(Reflect)` + `#Static #Frozen` 字段（`type` / `fields` / `methods` / `variants`）暴露，仅类型形访问（[#1.AB]），数据落 `.rodata`。**形态演化**：原 `type_of(x)` / `fields_of(x)` 内置函数路线被 [#1.Y] / [#1.AB] 取代为 `T::name` 静态字段访问。理由：JSON / Eq / Hash / Clone 全可走编译期 unroll（机制延后到 inline-for 草案，[#1.AC]），runtime 反射真用例 < 1%。
- **[#1.H]** 运行时反射 P2：`#Reflect(...)` opt-in 子集（name / fields / methods）。emit .rodata，linkonce_odr + 链接期 DCE 兜底。理由：留逃生口给调试 / dump 工具，但默认关闭以避免 Java 反射元数据膨胀。
- **[#1.I] 收回**（被 [#1.AA] 取代）。原决议引入 `#Derive(Spec)` 作糖；现统一为 `#Impl(Spec)` 单注解 + 默认体自动 fall-through。
- **[#1.J] 收回**（被 [#1.AE] 取代）。原决议要求显式 `#Inline for`；现砍掉整个 inline-for 路线。
- **[#1.K]** 反序列化需要的字段名构造已由 static-fn 落地（`Self { .name = x, .age = y }` 字段字面量，spec §7.3.2 / §7.10.3.4）；不再需要独立 `DRAFT-named-ctor.md`。本草案的反射 unroll 路径（§6.3）在 `#Static fn` 体内可直接产 `Self { ... }`。
- **[#1.L]** 实现声明用 `#Impl(Spec)` 顶行注解，取代 `: Spec` 头部形态。理由：与 yux 注解派哲学（const-mut / let-unify / static-fn）一致；与 `<T : S>` 边界、`Dyn<S>` 擦除三处分形态，`:` 留给类型边界专用。`#Impl(Spec)` 单注解承担实现声明（写或不写方法体由 spec 默认体自动 fall-through 决定，[#1.AA]）。代价：SDK 全仓 `Type : Spec { ... }` impl 块改写顶行 `#Impl`，与"draft → #Spec struct"同期一次性完成。
- **[#1.M]** orphan / 外部类型实现外部 spec：v1 维持 wrapper struct 兜底；**形态预留** `#Impl(X) extend Type { ... }`，附录 A 把 `extend` 列为未来保留字，独立草案 `DRAFT-extension-blocks.md`（v1.x 包管理时启动）落地。理由：避免现在做的决策堵死未来路；当前阶段不增 g4 / token 工作量。
- **[#1.N]** `Self` 关键字、`Self { ... }` 字段字面量、`Type::name(...)` 静态调用、构造唯一通道（`#Static fn make/from/new + Self { ... }`）已由 static-fn 草案落地为基线（`docs/dev/static-fn-impl-log.md`、`07-结构体.md` §7.10、附录 A.1）。本草案不重做这些条款，只追加"spec 体内 Self 抽象类型变量"语义（§5.1）；spec 内允许声明 `#Static fn` 签名作工厂契约（§3.1）。
- **[#1.O]** `buildAnno` 实参槽**保持单参**，arg 形态从 `arg=ID` 扩到 `arg = literal | ID genericDef?`。多 spec / 多 flag 一律顶行堆叠（`#Impl(A)`\n`#Impl(B)` / `#Reflect(name)`\n`#Reflect(fields)`）。理由：注解 codegen 后端已取消，注解 = 编译器内置识别 + 未来用户可定义 struct 承载，单参形态对绝大多数注解够用；多参 / named-args 与"用户定义注解 struct"是同一盘大设计，剥离到独立草案（[#1.P]）。
- **[#1.P]** 注解 struct 体系——用户可定义注解（`#Annotation struct Api { ... }`）+ 命名参数（`#Api(preffix="/api")`）+ 字段默认值 + LSP 注解字段补全——剥离到独立草案 `DRAFT-anno-struct.md`。本草案锁前向兼容：单参 + literal/type ref 形态在 anno-struct 落地后兼容（只需扩 `annoArg` 子产生式，不破坏已有形态），唯 `#Reflect` 由 positional flag 集迁 named-args bool 字段。理由：注解 struct 涉及 `filedDecl` 默认值、命名参数调用语法、注解注册标记、字段类型限定、LSP 协议等，超出 spec-unify 单一职责。
- **[#1.Q]** spec body 内**instance 字段段** v1 不实施。理由：字段抽象由 `Reflect` 编译期反射（§6）覆盖；instance 字段段引入 vtable layout / accessor / 内置标量类型实现问题，复杂度高、收益低；独立草案 `DRAFT-spec-fields.md` v1.x 启动。代价：spec `Point<T> { x T; y T }` 形态当前写不出，需通过 fn 签名表达（`fn x() T` / `fn y() T`）。**例外**：`#Static` 字段段允许（[#1.Z] 通用扩展），承担 type-bound 契约——例如 Reflect spec 的 4 个 `#Static #Frozen` 字段；用户 spec 亦可用，如 `#Spec struct Bounded { #Static MIN i32; #Static MAX i32 }`。
- **[#1.R]** v1 仅类型参数 spec（`#Spec struct Iter<T>` 形态），不引入关联类型 / `type Item` 机制。同一类型可对不同 T 多次实现（`Counter` 可同时 `#Impl(Iter<i32>)` + `#Impl(Iter<String>)`）。独立草案 `DRAFT-assoc-types.md` v1.x 启动。理由：避免 +1 关键字、避免 sema 引入抽象类型成员表；类型参数 spec 已能表达迭代器、Future、Add 等核心场景。
- **[#1.S]** spec 默认体 typecheck **推迟到单态化时机**：sema 期只解析 + 占位符号校验，不做完整 typecheck；每次某具体类型 `#Impl(Spec)` 且未覆盖某方法时，克隆默认体 AST + 替换 `Self` + 入该类型方法表 + 走常规 typecheck。理由：避免引入"抽象 self table"机制；副作用是无人实现的 spec 默认体错误不报，与"无人调用的泛型函数体不报错"现状一致。
- **[#1.T]** spec 组合的默认体冲突——两个 spec 都带同名同 arity 方法 + 都带默认体时，实现者**必须显式覆盖以消歧**（fn body 内可写 `SpecA::default_method(...)` / `SpecB::default_method(...)` 选边）。不引入隐式优先级 / 顺序 / `use SpecA::method` 机制。冲突未覆盖 → E3132（占位，见 [#1.U]）。一种 spec 默认体覆盖另一种纯抽象签名不冲突。
- **[#1.U]** 错误码分配草表（具体措辞 Phase 实施时定）：
  - **E1135-E1140**（spec / draft 行为）：`#Impl` 注解校验（非 spec / 重复 / 错位置）、spec 方法未实现（E1136，[#1.AA]）、实例上调静态字段 / 静态函数（E1137，[#1.AB]）、spec 实例化拒收、`#Spec struct` 内 `~()` 拒收、spec 内 `#Static fn` 体引用具体类型 ctor 拒收、对象安全（沿用 E1134）。
  - **E3132-E3140**（sema 静态）：spec 默认体冲突（E3132，[#1.T]）、`Field.value` 改名时 f 非编译期可定（E3133，[#1.W] / [#1.AE]）、`Field.value` 改名无 receiver 绑定（E3134，[#1.W] / [#1.AE]）、`Reflect::variants` 在非 enum 上访问（E3135，[#1.Y]）、按值取 `Counter::type` 等 rodata 单例（E3136，[#1.Y]）、spec 实现者方法签名不匹配、spec 内 `#Static fn` 签名未实现。
- **[#1.V] 收回**（被 [#1.Y] 取代）。原决议"编译期字符串 = `cval String`"——既然反射数据全走 `#Frozen` + rodata，`cval` 修饰多余；反射用例下 String 字段就是 `#Frozen String`（rodata 字面量串）。`cval` 体系本身（局部 `#Cval let` / 全局 `#Cval let`）由 let-unify 维持，与本草案无关。`#Const fn` 函数纯洁性也仍是独立维度。
- **[#1.W]** 编译期字段访问形态 **`f.value`**：通过 `Field.value` 按普通字段访问语法读写，不引入新语法（如 `$.#f`）。`fields_of(instance)` 实例形态返回的 Field 绑 receiver，`f.value` 由 sema 替换为 `<instance>.<f.name>`；`fields_of(T)` 类型形态未绑 → `.value` 访问 E3134；出 `#Inline for` 上下文 → E3133。写 `f.value = expr` 受常规 const-mut 检查（`#Mut` 字段才允许）。理由：无新语法、与"普通字段访问"形态统一；代价见 [#1.X]。
- **[#1.X]** `Field` 是**唯一编译器内置反射类型**——`.value` 是 dependent-typed phantom（类型按迭代位 + 是否绑 receiver 动态），yux 类型系统当前无法表达。后果：base.yux 仅留 `#CompilerInner stub`；LSP 补全 / hover、文档、sema 三套硬编码该字段。其它三个反射类型（`Type` / `Method` / `Variant`）不受影响、可纯 yux 表达。已知代价，实施时如有更优方案再议。
- **[#1.Y]** 反射通过**内置 spec `Reflect` + 静态字段**暴露——编译器隐式为每个类型 `#Impl(Reflect)`、自动填充 `#Static #Frozen` 字段。不引入 `fields_of(...)` 内置函数通道；类型形 `Counter::type` 无 `()` 直接读 rodata；实例形 `c.type` / `c.fields` 由 sema 注入 mirror（不在 spec body 内声明），等价同名 static field 读 + `c.fields` 附 receiver binding 给 `.value` phantom。数据走 `#Frozen T&` 落 `.rodata`，不引入 `cval& T` 区分（[#1.V] 收回——`#Frozen` + rodata 已覆盖 const 流出 runtime 的需求，`cval` 修饰多余）。`#Reflect` 注解语义从"opt-in 反射"退化为"强制 emit + 防 DCE"，默认走 emit + 链接期 `--gc-sections` 兜底。
- **[#1.Z]** 反射所需的 **`#Static` 字段子集**（`#Static #Frozen` 字段 + 字段类型可为 `T&` + `Type::name` 无 `()` 访问形态）吸收进 spec-unify Phase (b)，**不前置 `DRAFT-data-struct.md` 全集**。子集明确不引入 `#Cval` / `#Mut` 静态字段等其它档位；data-struct 草案全集（含非反射场景的静态字段、`#Static let` 等）独立另起，与本草案约定不冲突即可。代价：未来 data-struct 落地时要 audit 一致性。同时收回方法形态（`#Static fn type() #Frozen Type&`）——用户决议"用方法多此一举"，全静态字段统一；额外好处：const-mut 返回值 `#Frozen` 缺口（[#1.V] 旁注）被 sidestep，spec-unify Phase 不依赖该缺口修复。spec body 内允许 `#Static` 字段是 [#1.Q] "spec 仅 fn 段" 的**通用扩展**（不仅 Reflect 专用）——type-bound 契约 vs instance 字段段，前者允许、后者仍禁；任何用户 spec 都可声明 `#Static` 字段作类型级常量契约。
- **[#1.AB]** **实例不能调静态字段 / 静态函数**——`c.type` / `c.fields` / `$.type` / `$.fields` 等"实例形访问静态成员"一律 E1137 拒收；类型形 `Counter::type` / `Self::fields` 是唯一通道。理由：与 yux 静态成员一致性（静态属类型不属实例）；避免 sema 引入"实例形 mirror"注入逻辑；用户在方法体内写 `Self::fields` 与裸 `Counter::fields` 形态对齐。代价：反射 API 表面少一种写法，对工具 / 模板代码无影响（`Self::` 同样指当前类型）。本草案 §6.1 反射 API 全面改类型形。
- **[#1.AC] 收回**（被 [#1.AE] 取代）。原决议将 `#Inline for` + `f.value` phantom 延后到 `DRAFT-inline-for.md`；现 [#1.AE] 永久砍 inline-for 路线。
- **[#1.AA]** 砍 `#Derive(Spec)` 独立注解，统一为 **`#Impl(Spec)` 单注解 + 默认体自动 fall-through**——实现者写了同名 fn → 覆盖；没写 → spec 默认体生效（[#1.S] 单态化时机克隆 AST 路径）；spec 仅签名无默认体 + 实现者没写 → E1136。理由：`#Derive` 本就是 `#Impl + 不写体` 的同义糖，分两个注解纯增概念；删 `#Derive` 后 base.yux 内置 5 件套（ToString / ToJson / Eq / Ord / Clone）的"derive 即可"形态仍工作（用户写 `#Impl(MySpec)` 不写体即得默认行为），无需任何编译器硬编码。[#1.I] 同步收回。`#Derive` 永不引入 = v1 决议；fall-through 机制本身随 §4 默认体延后（[#1.AD]）。
- **[#1.AD]** **v1 范围收紧为"替代 draft"** —— spec 默认方法体（§4 / [#1.S] / [#1.T]）、内置 spec `Reflect` 反射（§6 / [#1.Y] / [#1.Z] / [#1.X]）、`#Reflect` 注解（§7 / [#1.H]）、`#Impl` fall-through（§8 / [#1.AA]）四块**全部降级为形态占位**，独立草案逐个落地：
  - `DRAFT-spec-default-body.md`：spec 默认体 typecheck（[#1.S]）、组合默认体冲突（[#1.T] / E3132）、spec 内 `Self` 抽象变量替换路径
  - `DRAFT-spec-reflect.md`：`Reflect` spec 声明 + 隐式 `#Impl(Reflect)`、`Type` / `Field` / `Method` / `Variant` 数据类型、`.rodata` emit、`#Static #Frozen` 字段子集（[#1.Z]）、实例形访问 E1137（[#1.AB]）、`#Reflect` 防 DCE
  - 注：原计划的 `DRAFT-inline-for.md` 已**永久砍掉**（[#1.AE]）；`Field.value` 改名作为 sema 规则吸收进 spec-reflect 草案。
  
  v1 实施面 = **§3 + §5 + §10 中 draft 替换部分**：`#Spec struct` + `#Impl(Spec)` 顶行 + 声明合一 + 删 `draft` / `Draft` token / `draftDecl` / `structImpl` / `#DraftLike` + spec 体内 `Self` 抽象变量。spec body 内方法**仅签名**（带 body 报 E1138 占位）；实现者**必须全部写体**（漏一个 E1136）。
  
  理由：原 spec-unify 草案爆面太大（声明合一 + 反射 + 默认体 + 注解扩参 + 静态字段子集），强行打包 → 任一子任务卡住整盘卡。draft 替换是最小可独立交付切片——动作纯重命名 / 删旧 / 注解走单参形态，无需 sema 新机制；其余两块各自有 typecheck 时机 / `.rodata` emit 等大议题，独立落地不污染主路径。代价：v1 落地后 yux 仍写不出 `to_json` 默认体、读不到 `Counter::fields`，但 g4 / `#Spec` / `#Impl` 形态已稳定，后续草案不再动 g4 / 不破坏 SDK 迁移成果。

- **[#1.AE]** **永不引入 `#Inline for` 编译期循环 unroll** —— 增加编译器复杂度（新 IR-before pass / `#Inline` 通用语义 / unroll 上限 / IR 爆量风险），性价比低。替代路径：
  - **反射数组走 runtime**：`Self::fields` 是普通 `[Field& * N]&` 数组，普通 `for` 遍历，正常 codegen
  - **`Field.value` sema 期纯改名**：当 `f` 是编译期可确定的具体 Field 引用（如 `Counter::fields[0]` 下标位字面量）时，sema 改写为 `<receiver>.<f.name>`；`f` 运行期才能定 → E3133。无 IR-before pass，sema 复用既有字段访问通路
  - **异构按字段递归默认体永不引入**：`ToJson.to_json` / `Eq.eq` / `Clone.clone` 等自动 derive 形态——实现者手写体或显式覆盖
  
  代价：[#1.AC] 同步收回；[#1.W] / [#1.X] 保留并简化（不再依赖 unroll，只是 sema 改写规则）；base.yux 5 件套自动 derive 路径永久关闭。理由：yux 哲学倾向"显式 > 编译期魔法"——用户写 N 个字段就手写 N 行 `to_json`，与 macro-free / 无 procedural macros 决议一致。

---

## 定型与归宿

草案定型后：

1. 与 `DRAFT-let-unify.md` 协调：`Self` 关键字 + `let` 同时落地；声明形态完全统一。
2. 与 `DRAFT-const-mut.md` 协调：const-mut 注解（`#Mut` / `#Cval` / `#Frozen` / `#Const fn`）维持，声明形态走 let。
3. 按 §10.5 拆分迁入 spec。整章 §12 重写。
4. CHANGELOG 顶部追加 v0.X spec-unify 主题条目。
5. `CURRENT.md` 列 Phase（粗）：
   - **(a) g4 改造**：`structDecl` 合并 `structImpl` + 删 `draftDecl` / `Draft` token / `draftType` + `buildAnno` 单参 arg 升级到 `literal | ID genericDef?`（[#1.O]）。
   - **(b) `#Spec` / `#Impl(...)` 注解 sema 接管 + spec registry**：重命名 `draft_impl_checker` → `spec_impl_checker`，逻辑大体复用；spec 内方法 v1 仅签名（带 body → E1138 占位）；实现者漏方法 → E1136；spec 体内 `Self` 抽象变量识别（sema 期方法签名解析时把 `Self` 占位为该 spec 抽象 type var；本 phase 仅识别 + 单态化时机替换为具体 implementer，不做默认体合成）。
   - **(c) `#DraftLike` 删除 + SDK + tests 全仓迁移**：`draft` → `#Spec struct` / `: Spec` impl 块合并进 struct 体 + 顶行 `#Impl(...)` / `#DraftLike` 删 一次性切换；测试 / 教程同步。
   - 已落地不进 Phase：`Self` 关键字 / `Self { ... }` / `#Static fn` / let-unify。
   - **占位章节（不进 v1 Phase，独立草案落地）**：
     - spec 默认方法体 + fall-through → `DRAFT-spec-default-body.md`
     - `#Static #Frozen` 字段子集 + 内置 spec `Reflect`（runtime 反射数组）+ `Field.value` sema 改名 + `#Reflect` 防 DCE → `DRAFT-spec-reflect.md`
     - base.yux 内置 5 件套（ToString / ToJson / Eq / Ord / Clone）spec 声明 → v1 可写"仅签名"形态，实现者必须全写体；可推默认体（如 `Ord.lt/le/gt/ge` 由 `cmp` 推）待 default-body 落地；按字段递归默认体（`ToJson.to_json` 等）**永不引入**（[#1.AE]）。
   - **永不引入**（[#1.AE]）：`#Inline for` 编译期 unroll / IR-before unroll pass / 异构按字段递归自动 derive。
6. 完成后归档 `docs/dev/spec-unify-impl-log.md`。

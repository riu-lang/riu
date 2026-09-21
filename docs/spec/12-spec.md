# §12 spec（接口与约束）

本章规范 riu 的 **spec**（接口契约）机制：通过 `#Spec` 注解声明方法签名与静态字段契约、通过 `#Impl(D)` 顶行注解在 struct 声明上宣告实现关系、泛型边界 `<T : D1 + D2>`、跨包 orphan 限制，以及内置 `ToString` / `Number` / `copy_of` / `Indexed` / `Iter`。本章对应草案 [`draft/DRAFT-spec-unify.md`](draft/DRAFT-spec-unify.md)（v1 已落地）。

> 设计基调：**默认严格 / 显式优先 / 零运行时开销**。spec 是封闭契约，必须由 `#Impl(D)` 显式宣告并由 struct body 提供方法实现。v1 一律单态化静态分发，无 vtable、无隐式签名表参数。运行时多态形态另由 §12.9 `Dyn<D>` 提供。

> 与历史 `draft` 关键字的关系：v1 已合一为单一 `structDecl` 形态，`draft` 关键字与 `Type : D { ... }` 外置实现块已删除（见 [CHANGELOG 2026-05-19 spec-unify v1](CHANGELOG.md)、附录 A.6）。本章用语统一为 **spec**；早期 docs / 实现日志中残留的"draft" 字样仅作历史参照。

涉及交叉章节：§6.4（函数泛型）/ §7.1（结构体声明）/ §8.6.7（借用与边界）/ §11.4（`#Spec` / `#Impl` 注解）/ §附录 D（错误码）。

## §12.1 概念

### §12.1.1 spec 声明：`#Spec struct`

```
specDecl  ::= '#' 'Spec' codeLineEnd
              ( '#' ID ( '(' ID genericDef? ')' )? codeLineEnd )*
              'struct' ID genericDef? '{'
                  ( fnSig | specStaticField | comment | codeLineEnd )*
              '}'

fnSig     ::= buildAnno* 'fn' ID '(' fnParams? ')' (retType=type)?
specStaticField ::= fieldAnno* ID type codeLineEnd
```

§12.1.1.1 `#Spec` 顶行注解把一个 `structDecl` 转为一个 **spec**：一组方法签名（可附带默认体，见 §12.10）与静态字段契约。spec body 内**不得**出现实例字段、`#Static fn`、或析构函数 `fn ~()`；违反报 **E2011**。允许的字段形态为 `#Static #Frozen` 类型元数据（§13）和 `#Cval #Inline` 关联常量；在 spec 中它们只声明名称、类型与存储属性，不带初值。

§12.1.1.2 签名集**允许为空**（无方法的 spec 合法，但 v1 不再内置 `Any`——见 §12.7.2）。

§12.1.1.3 spec 自身**可以**带泛型形参；spec 体内单个 `fn` 签名**不得**再引入新的泛型形参（违反报 **E1104**，参见 §12.4.2）。

§12.1.1.4 `#Spec` 是构建注解（§11.1 / §11.4.1）；`struct` 是关键字（附录 A.1）。spec 名按 §10 名字解析；同模块内 spec 名与其它顶层符号共享命名空间。

§12.1.1.5 spec 是名义类型（§3.4.1），用于 `<T : D>` 边界、`#Impl(D)` 注解的实参、`Dyn<D>` 类型参数等位置；**不**可直接出现在值表达式位（spec 没有值，只是契约）。

```riu
#Spec
struct ToString {
  fn to_string() String
}

#Spec
struct To<T> {                ; 合法：spec 自身泛型
  fn to() T
}
```

### §12.1.2 spec 名字解析

§12.1.2.1 在 `<T : D>` 边界、`#Impl(D)` 注解、`Dyn<D>` 类型参数中，`D` **应当**按 §10 完全限定名解析。

§12.1.2.2 同名跨包 spec（如 `a.Show` 与 `b.Show`）按完全限定名独立判定，两者并存不冲突；import 名字冲突按 §10 已有规则处理。

## §12.2 实现宣告：`#Impl(D)`

### §12.2.1 形态

```
structDecl ::= buildAnno*                                ; 含可重复 #Impl(D)
               'struct' ID genericDef? '{'
                   (filedDecl | LineEnd)*
                   fnClean?
                   (fn LineEnd | LineEnd)*
               '}'

implAnno   ::= '#' 'Impl' '(' draftBound ')' codeLineEnd
draftBound ::= modulePath? ID genericDef?     ; 例：ToString / pkg.Display / To<i32>
```

§12.2.1.1 `#Impl(D)` 顶行注解附着在 `struct` 上，宣告该 struct 实现 spec `D`。可重复出现（多 spec 由多条 `#Impl(D)` 平铺），等价于旧形态 `Type : D1 + D2 { ... }` 的实现关系。

§12.2.1.2 `#Impl(D)` 不接受嵌套 spec 参数之外的形态；turbofish `#Impl(To<i32>)` 通过 g4 `buildAnno` 的 `arg=ID genericDef?` 槽承载（§11.1.1.1）。

§12.2.1.3 被宣告的 struct **应当**为 owned 名义类型；语法层不可能在 struct 头位写 `T&` / `T?`（与 §7.1 一致）。泛型 struct 可写 `#Impl(D) struct Foo<T> { ... }`；每个泛型实例化产出一份独立的实现位（与 §6.4.2.3 单态化路径一致）。`D` 带类型参数时，同一 `S` 对同一 spec **基名**只能挑下面三种之一（每种一条，§12.2.2.3）：

- 闭、非泛型：`#Impl(D<i32>) struct S` — 只这一份。
- 闭、泛型：`#Impl(D<i32>) struct S<T>` — 任意 `T` 的 `S<T>` 都实现 `D<i32>`（与 `#Impl(ToString) struct Foo<T>` 同一模型：impl 不提 `T`）。
- 开：`#Impl(D<T>) struct S<T>` / `#Impl(D<K>) struct Map<K, V>` — 每个单态一份；实参可以是实现者的任一形参，不必与 spec 形参同名。

无 `where`、不特化：不能写「仅当 `T : Foo`」或另开一份 `S<i32>` 更特殊的 `#Impl`。

§12.2.1.4 `D` 声明带 N 个形参时，`#Impl(D)` 无 turbofish、或实参数 ≠ N，报 **E1142**。实参是普通 `type`（标量 / 用户类型 / 实现者自己的形参名 / 嵌套泛型）；禁止 `T&`（**E4037**，与其它类型实参同规）。

### §12.2.2 穷尽性

§12.2.2.1 对 struct `S` 上每个 `#Impl(D)`：S body 内提供的成员**应当**覆盖 D 的全部方法签名与静态字段契约：

- 缺任一 D 签名 → 报 **E1137**（“未实现 spec 方法”）。
- 缺静态字段，或字段类型 / `#Cval` / `#Inline` 属性不满足 D 的要求 → 报 **E1141**。字段类型中的 `Self` 在比对时替换为 S。

§12.2.2.2 v1 **不再**对 "多余于 D 的方法" 报错——struct body 内未命中任何 `#Impl(D)` 的方法**应当**视为该类型自身的普通方法，照常存在并参与普通方法分发。早期 `E1102`（"多余"）已废弃；严格的 spec 隔离待 [`draft/DRAFT-extension-blocks.md`](draft/DRAFT-extension-blocks.md) 落地后回归。

§12.2.2.3 同一 `S` 上**不得**重复宣告同一 spec **基名** `D`：写两条 `#Impl(D)`，或 `#Impl(D<A>)` 与 `#Impl(D<B>)`，均报 **E1103**。身份键为 `(S, D)`，不含实参。`#Impl(D<i32>)` + `#Impl(Eq)` 合法（不同基名）。同一 `S` 上写多条不同基名的 `#Impl(D1) #Impl(D2) ... #Impl(Dn)` 合法，方法集需穷尽全部 Di 的并集（重复签名按 §12.3 等价判一致即可，无须重复实现）。不同实参并存（`#Impl(To<i32>)` + `#Impl(To<String>)`）本阶段不成立，见 [prop/16](prop/16-impl-split.md)。

### §12.2.3 方法分发

§12.2.3.1 对 `#Impl(D) struct S` 已实现的方法 `m`：

- `obj.m(args)`（`obj: S` 或 `S&`）按 §8.6.7.3 receiver 归一为 `Self&`，直接静态分发到 S body 内的方法实现。
- `Rc<S>` 上 `box.m(args)` 按 §8.6.7.3 自动解引用 + 归一，等价于在 S 上调用，无须独立 forward 机制（§12.6）。
- `Heap<S>` 上 `h.m(args)` 同款（§9.5a）。

§12.2.3.2 receiver `$` 在 struct body 内方法体的语义与 §7.2.2.1 一致（类型 `Self&`），不论该方法是否对应某个 `#Impl(D)` 的契约。

§12.2.3.3 v1 spec 与 struct 共用同一 method namespace：方法名 `m` 在 struct body 内只允许一份实现。若多条 `#Impl(Di)` 要求同名 `m` 且签名等价，只需一份实现即可同时满足。签名不等价的同名要求在 §12.3 等价判定下报 **E1137**（无法同时满足）。

### §12.2.4 实例形访问静态成员（误用拦截）

§12.2.4.1 spec body 内**不得**含 `#Static fn`；§12.1.1.1 所列静态字段契约不受此限。对 struct body 内通过实例形访问 `#Static fn`（如 `obj.factory()`、`$.factory()`）报 **E1138**——静态调用须走 `Type::factory(...)`（§7.10）。

## §12.3 签名等价

下列规则用于 §12.2.2 穷尽性检查。

### §12.3.1 等价判定

| 维度 | 规则 |
|---|---|
| 方法名 | 完全相同（大小写敏感） |
| 参数个数 | 相同 |
| 参数类型 | 逐一按 §3.4 类型相等判定（不变；不协变） |
| 参数名 | 不参与判定 |
| 返回类型 | 相同（不协变） |
| receiver 形态 | 不参与（按 §8.6.7.3 归一为 `Self&`） |
| `T` vs `T&` 参数 | **必须一致**（不互通） |
| spec 体内 `fn` 的本地泛型 | **禁止**（§12.3.2） |
| 注解 | 不参与 |
| 可见性 | 实现方法**应当**按 §10 在 spec 视角可见 |

### §12.3.2 禁止方法本地泛型

§12.3.2.1 spec 体内单个 `fn` **不得**再引入泛型形参：

```riu
#Spec
struct Bad {
  fn map<U>(x U) U      ; ❌ E1104
}
```

§12.3.2.2 需要这种能力时，写在外层泛型函数 `fn map<T : Mappable, U>(...)` 的 `genericDef` 中。

§12.3.2.3 spec 自身允许 `genericDef`（§12.1.1.3）。穷尽性把 spec 形参换成 `#Impl` 实参，再按 §12.3.1 比对；`Self` 仍换实现者。查找满足关系按 `(S, D+实参)`：`#Impl(D<i32>)` 的 `S` 满足 `D<i32>`，不满足 `D<String>`。宣告次数仍按 `(S, D)`（§12.2.2.3）；同一 `S` 上不同实参并存本阶段不成立，见 [prop/16](prop/16-impl-split.md)。

## §12.4 `#Spec` / `#Impl` 注解互锁

### §12.4.1 `#Spec`

§12.4.1.1 `#Spec` 仅可附着于 `structDecl`；附着于其它声明位（fn / extern / globalConst / let / 字段 / 参数）由 ast_builder 拒收报 **E1110**。

§12.4.1.2 `#Spec` 不接受参数；与 `#Impl(...)` **不得**在同一声明上共存（spec 自身不实现别的 spec；继承 / super-trait 由 §12.8 项 8 禁止）。

§12.4.1.3 `#Spec struct` body 内的方法**允许**带函数体；带体的方法成为**默认方法体**，由 §12.10 定义 fall-through 与组合冲突规则。`fnExprBody`（`= expr` 单表达式形）与 `fnBlockBody`（`{ ... }` 块形）均合法承载默认体。默认体内的占位符号校验（`$` / `$.method` 等）见 §12.10.3。

§12.4.1.4 `#Spec struct` body 内**禁止**字段 / 析构 `fn ~()`（违反报 **E2011**）。

### §12.4.2 `#Impl(D)`

§12.4.2.1 `#Impl(D)` 仅可附着于 `structDecl`（含 spec？否——见 §12.4.1.2）；附着于其它声明位由 ast_builder 拒收报 **E1110**。

§12.4.2.2 `#Impl(D)` **必须**带单参数 `(D)`，其中 D 由 `ID genericDef?` 形态表达（§11.1.1.1 单参数糖）。缺参 / 多参 / 字面量参报 **E2010**（注解参数形态不合）。

§12.4.2.3 同一 struct 上多条 `#Impl(D)` 顺序无语义；重复同名 **基名** D（含不同实参）按 §12.2.2.3 报 **E1103**。实参数与 spec 形参数不一致报 **E1142**。

§12.4.2.4 D 解析失败 / 非 spec（解析到其它顶层符号或解析到普通 struct）时报 **E1131**。

### §12.4.3 历史 `#DraftLike`（已废弃）

§12.4.3.1 `#DraftLike` 注解在 spec-unify v1 中**已删除**。其原语义"开放结构化匹配"在统一形态下由"宣告 `#Impl(D)` 即可"取代——结构化匹配不再是开放档位，所有满足关系一律走 `#Impl(D)` 显式宣告。

§12.4.3.2 早期代码 / 文档中残留的 `#DraftLike` 字样应迁删。`#DraftLike` 仍在注解白名单：非 `#Spec` 位已报 E1110；`#Spec struct` 上为 noop（§11.4.3.2）。

## §12.5 跨包规则（orphan）

### §12.5.1 限制

§12.5.1.1 `#Impl(D) struct S { ... }` 的 struct 声明**应当**写在以下两处之一：

- `S` 所在的包（即在定义 S 的同一文件 / 模块声明 S 并附 `#Impl(D)`），或
- `D` 所在的包（即在定义 D 的同一文件 / 模块以 newtype 或本地 struct 声明 S 并附 `#Impl(D)`）。

由于实现关系通过 struct 声明上的注解承载，"为外部类型实现外部 spec" 在 v1 语法层即不可表达——外部类型不能在本包重新声明 `struct`。该限制由 §10 名字解析自然落实，**不**单独引入 `E1120` 诊断点。

§12.5.2 设计动机（*informative*）：避免外部库类型被第三方"污染"实现而破坏可推断性；让 `#Impl` 是跨包绑定行为的唯一通道，且通道在 spec 与类型同包内对称。

§12.5.3 扩展实现块（v1 不引入）：v1 **不**引入 `extend` 顶层声明。为外部类型实现 spec、同包多 struct 共享 impl、用户对内置类型加方法，一律等 v1.x 包管理（形态预留见 [`draft/DRAFT-spec-unify.md`](draft/DRAFT-spec-unify.md) [#1.M]；独立草案 `DRAFT-extension-blocks.md` 届时再建）。`extend` 为未来保留字（附录 A.6.2），当前仍可作 `ID`。v1 只能在 S 或 D 所在包的 `structDecl` 上写 `#Impl(D)`（§12.5.1）。

## §12.6 堆句柄与 spec

§12.6.1 v1 **不**为 `Rc<U>` / `Heap<U>` 引入独立的 forward 机制；堆句柄上的方法调用经 §8.6.7.3 自动解引用 + 方法分发归一处理。

§12.6.2 边界与堆句柄实参的关系：

| 调用形态 | T 实例化 | 要求 D 实现位点 |
|---|---|---|
| `show(box)` 给 `fn show<T : D>(x T)` | T = `Rc<U>` | **`Rc<U>` 自身**附 `#Impl(D)` |
| `show(as_ref(box))` 给 `fn show<T : D>(x T&)` | T = U | U 附 `#Impl(D)` |

§12.6.3 边界泛型形参的实参档位仍受 §8.6.7.1 限制：T **不得**为 `T&`；调用方需经 `as_ref`（§8.3.5.5）/ `copy_of`（§12.7.3）显式做借用 ↔ owned 转换。

§12.6.4 堆句柄不绕过 §12.5 orphan：句柄类型与 spec 必须满足同包条件。

§12.6.5 v1 spec 体内只有实例方法签名（§12.1.1.1），**不**存在关联函数 forward；其它堆句柄（`Array<T>` / `String` / `Weak<T>` / `StringBuilder`）**不**参与"自动解引用调元素方法"。

## §12.7 内置 spec 与 builtin

### §12.7.1 `ToString`

§12.7.1.1 `sdk/riu/src/riu/core/base.ut` 内置 spec：

```riu
#Spec
struct ToString {
  fn to_string() String
}
```

`ToString` 是 v0.6 字符串模板 `"$expr"` 的"可插值"约束，属强契约；用户类型必须显式 `#Impl(ToString)` 才会进入插值路径，避免 debug-string 被误命中显示文本。

§12.7.1.2 各内置类型以 `#Impl(ToString) struct T { ... fn to_string() String { ... } ... }` 形态显式实现；方法体可走 `#Builtin`（§11.2）或直接 riu 实现，二者并存。数值类型在 `num.ut`，`String` 在 `string.ut`，`bool` 在 `base.ut`。当前实施：

```riu
#Impl(ToString)
#Builtin
struct i64 {
  ; ... 内置算术 / 转换方法 ...
  fn to_string() String { ... }   ; riu 实现
}

#Impl(ToString)
#Builtin
struct String {
  ; ... 内置方法 ...
  fn to_string() String { $ }     ; 恒等返回
}

; 同理覆盖 i8 / u8 / i16 / u16 / i32 / u32 / u64 / f32 / f64 / bool
```

§12.7.1.3 v1 中原注解 `#CompilerInner` 已于 2026-06-11 重命名为 `#Builtin`（§11.2）；原"不引入新注解 `#Builtin`"条款随此次重命名自然解除。

### §12.7.2 `Any`（已删除）

§12.7.2.1 v1 内置 spec `Any` **已删除**（spec-unify v1，2026-05-19）。早期形态：

```riu
#DraftLike
draft Any { }    ; v0.x，已废
```

§12.7.2.2 删除原因：unified `#Spec` + `#Impl` 形态下，所有 owned 类型"自动满足 Any"必须为每个类型写 `#Impl(Any)` 才能宣告，零意义；而无 `#DraftLike` 自动派生机制，"universal bound" 概念在 v1 不再成立。编译期反射已由 §13 `Reflect` 承接。运行时 `is` / `as` / 类型擦除 `AnyRef` 仍按 §12.8 项 7 留后续版本。

### §12.7.3 `copy_of`

§12.7.3.1 编译器 baked builtin（与 spec 系统不直接耦合，但 §12.6 借用 → owned 转换链路常用）：

| builtin | 签名 | 方向 | 章节 |
|---|---|---|---|
| `as_ref` | `as_ref:<T>(box Rc<T>) T&` | Rc → T& | §8.3.5.5 |
| `copy_of` | `copy_of:<T>(x T&) T` | T& → T | §12.7.3（本节） |

§12.7.3.2 `copy_of` 规则：

- T 受 §8.6.7.1 owned 限制；`x` 是 T 的借用，结果是 T 的栈上 owned 副本。
- 复制语义按 T 档位：
  - **值类型**（标量 / 用户 struct / `[T*N]`）：memcpy + 字段级 retain（§7.4.3 / §7.4.4）。
  - **堆句柄**（`Rc<U>` / `String` / `Weak<U>` / `StringBuilder`）：句柄复制 + RC retain，沿用 §8.5 callee-clean 协议。
  - **`Heap<T>` / `Heap<T>?`**：深拷——重新 `__riu_heap_alloc` + 写入 inner T + 递归 retain inner 子句柄字段（[`draft/DRAFT-heap-types.md`](draft/DRAFT-heap-types.md) Phase 6）。
  - **`Array<T>` / 其它 `#NoCopy`**：拒绝（E4031）。`Array<T>` 深拷贝走 `arr.clone()`（§9.2.2.3）。
- `x` 的借用根（如对应的 `box` / 局部变量）在 `copy_of` 调用语句结束后仍可正常使用（临时借用 + 立即释放，按 §8.8 临时帧）。
- 与 `as_ref` 不互锁：原 `x` 视图与返回的 owned 副本各自独立析构。
- **不**接受 `Ptr`；turbofish 可省，T 由实参推断。

§12.7.3.3 占位签名置于 `base.ut`，`#Builtin` 形态；编译器在调用点合成 IR（§11.2.3）。

```riu
fn use_owned<T : D>(x T) { ... }

fn caller(box Rc<MyType>) {
  use_owned(copy_of(as_ref(box)))   ; T& → T 显式拷贝后传入 owned 形参
}
```

### §12.7.3a `size_of` / `align_of` / `overlay`

§12.7.3a.1 编译器 baked `#Builtin`（`base.ut`）：

| builtin | 签名 | 说明 |
|---|---|---|
| `size_of` | `size_of<T>() u64` | ABI 字节数（含 `#Packed` / `#Align` 尾 pad） |
| `align_of` | `align_of<T>() u64` | ABI 对齐 |
| `overlay` | `overlay:<U>(x T&) U&` | 同一块 C-layout 内存的另一种视图 |

细则 §7.5.3.4 / §7.5.3.5。缺类型实参 → [E6018]；无布局 → [E6019]；非 C-layout → [E2039]；size / align 不符 → [E2040]。`overlay` 不是类型转换、不 copy；借用根与 `x` 相同。

### §12.7.4 `Eq` / `Ord` / `ToJson`

§12.7.4.1 SDK 另有三个内置 spec，声明在 `base.ut`，方法与默认体见 §12.10.6：

```riu
#Spec
struct Eq {
  fn eq(other Self&) bool
  fn ne(other Self&) bool = !$.eq(&other)
}

#Spec
struct Ord {
  fn cmp(other Self&) i32
  fn lt(other Self&) bool = $.cmp(&other) < 0
  ; le / gt / ge 同理由 cmp 默认体推导
}

#Spec
struct ToJson {
  fn to_json() String          ; 纯抽象；永不按字段递归自动 derive
}
```

用户结构体写 `#Impl(Eq)` / `#Impl(Ord)` / `#Impl(ToJson)` 即宣告。编译期反射 spec `Reflect` 见 §13。

§12.7.4.2 `Eq.eq` / `Eq.ne` / `Ord.lt` 等与 §7.2.3 运算符方法**同名**：`#Impl(Eq)` 的 `fn eq` 同时可作为 `==` 的重载入口。这不是操作符 spec——运算符仍按方法名分发，不因 `#Impl` 自动绑定其它算符。操作符 spec 语法糖（`Add` / `Index` 等）见 §12.8 项 6。

§12.7.4.3 v1 **不**引入独立 `Stringify` spec。`ToString` 只服务插值（§12.7.1）；断言失败打印实参值的窗口在 §11.3.5.4。

### §12.7.5 `Number`

§12.7.5.1 SDK 在 `base.ut` 声明 `Number` 作为所有内置数字类型的公共边界；`i8` / `u8` / `i16` / `u16` / `i32` / `u32` / `i64` / `u64` / `isize` / `usize` / `f64` / `f32` 均显式 `#Impl(Number)`。

```riu
#Spec
struct Number {
  #Cval
  #Inline
  MAX Self
  #Cval
  #Inline
  MIN Self

  fn to_i8() i8
  fn to_u8() u8
  fn to_i16() i16
  fn to_u16() u16
  fn to_i32() i32
  fn to_u32() u32
  fn to_i64() i64
  fn to_u64() u64
  fn to_f64() f64
  fn to_f32() f32

  fn plus(other Self) Self
  fn minus(other Self) Self
  fn mul(other Self) Self
  fn div(other Self) Self
  fn mod(other Self) Self
  fn eq(other Self) bool
  fn ne(other Self) bool
  fn lt(other Self) bool
  fn le(other Self) bool
  fn gt(other Self) bool
  fn ge(other Self) bool
  fn neg() Self
}
```

§12.7.5.2 数值运算与比较契约使用**按值** `Self` 参数，直接由 `num.ut` 的 `#Builtin` intrinsic 实现。这些方法不提供 `$ + other` / `$ == other` 形式的默认体：运算符本身就会分发到 `plus` / `eq` 等同名方法，默认体会形成递归。

§12.7.5.3 `to_bool` 不属于 `Number`；它是具体数字类型的便利转换，不作为数值泛型契约。`to_string` 由独立 `ToString` spec 承担；`to_bits` / `to_isize` / `to_usize` 及位运算方法不是全部内置数字的交集，也不进入 `Number`。

§12.7.5.4 在 `<T : Number>` 中可使用 `T::MIN` / `T::MAX`、上述转换方法及运算方法；单态化时 `Self` 替换为具体数字类型。

### §12.7.6 `Indexed` / `Iter` / `End` / `IterItem`

§12.7.6.1 SDK 在 `base.ut` 声明遍历契约（扁平进 `riu.core`，#2）。只有签名；**不**在 spec 上放 `filter` / `map` / `collect`（E1104）。`at` 不叫 `get`：`Map` 已有 `get(key K&) V?`。

```riu
#Spec
struct Indexed<T> {
  fn len() usize
  fn at(i usize) T&
}

enum End {
  End
}

enum IterItem<T, E> {
  Item(T)
  End
  Error(E)
}

#Spec
struct Iter<T, E> {
  fn next() IterItem<T, E>
}
```

§12.7.6.2 `Indexed`：按索引遍历。for-in 第二档认 `#Impl(Indexed<U>)`（§5.5.4.4）。`item` = `U&`；入口拍 `len`。无失败通道。

§12.7.6.3 `Iter`：消费型遍历。`next` 写游标，返回值枚举：`ret IterItem::Item(v)` 交出 `T`；`ret IterItem::End` 结束；`ret IterItem::Error(e)` 业务失败。`next` 本身**不是** `T ! E`（成功类型就是 `IterItem<T, E>`）。结束不是失败，不进 `!`。手写 `loop { match it.next() }` 与 for-in 同一条 `next`。不用 `T?` 当结束（`T = U?` 时是双层可空）。`#Impl(Iter<i32?, End>)` 合法：`null` 是 item。

§12.7.6.4 `E` **应当**是 enum（与 §6.7.1.1 同一档：`for` 透传 / 手写 `ret e` 要进失败通道）。无业务失败时用 SDK `End`。`enum End` 与 variant `IterItem::End` 不同名空间：前者是占位错误类型，后者是「没有下一项」。`E = End` 且实现不 `Error` 时，for-in **不可失败**，不必空 `catch End`。

§12.7.6.5 关联类型 / `IntoIter`：**不做**。短名走 §3.9 的文件 / 块内 `type`，不是 `Iter::Item`。不拆第二基名 `TryIter`。`#Impl` 按 `(S, D)` 基名（§12.2）：同一类型对 `Iter` 只能一条。`Dyn<Iter<T, E>>` 不是 for-in 目标（§5.5.4.2）。

## §12.8 不在范围

v1 / v0.5 **明确不做**：

1. ~~**`dyn Draft` / 运行时多态**（mangling 留位但不实现）。~~ → v0.5.x **已实现**，见 §12.9。
2. ~~**spec 默认方法体**（占位 → [`draft/DRAFT-spec-default-body.md`](draft/DRAFT-spec-default-body.md)）。~~ → 2026-05-22 **已落地**，见 §12.10。
3. **spec 体内方法本地泛型**（spec 自身可泛型）。
4. **关联类型 / 通用关联项语法**（占位 → [`draft/DRAFT-assoc-types.md`](draft/DRAFT-assoc-types.md)）。静态字段契约与 `#Cval #Inline` 关联常量子集已由 §12.1.1.1 / §12.2.2.1 落地。
5. **跨外部包为外部类型实现外部 spec**（§12.5；语法层自然落实）。
6. **操作符 spec**（`Add` / `Index` 等语法糖绑定；留后续版本；当前运算符走 §7.2.3 重载）。`Eq` / `Ord` 作为方法契约已落地（§12.7.4 / §12.10.6）；方法名与运算符重载重合，但运算符不因 `#Impl` 自动绑定。
7. ~~**运行时反射 / `is` / `as` 类型测试**~~ → 编译期反射已落地，见 §13。**运行时 `is` / `as` / 类型擦除 `AnyRef`** 仍留后续版本。
8. **spec 继承 / super-trait**（需"D2 蕴含 D1"时直接在边界写 `<T : D1 + D2>`）。
9. **协变 / 逆变返回或参数**（§12.3.1 签名等价不变）。
10. **spec 上独立可见性修饰**（沿用 §10 `_` 前缀私有）。
11. **`<T : D>` 在调用点 turbofish 处回写边界**（仅声明位允许）。
12. **结构化匹配 / `#DraftLike`**（§12.4.3 已废弃；所有满足关系均显式 `#Impl(D)`）。
13. **`where` 子句、`T : D1 | D2` or 约束**（§6.4.4.2）。
14. ~~**spec body 内字段 / 静态成员 / 关联常量**（占位）~~ → `#Static #Frozen` 字段段与 `#Cval #Inline` 关联常量已落地；`#Static fn` 与实例字段仍禁止（§12.1.1.1）。
15. **按字段递归的自动 derive 默认体**（如 `ToJson.to_json` 自动遍历字段）—— spec-unify v1 明确**永不引入**。
16. **`#Inline for` 编译期循环 unroll / IR-before unroll pass** —— spec-unify v1 明确**永不引入**。

## §12.9 `Dyn<D>` / `Dyn<D&>`（运行时多态形态）

§12.9 在 §12.1–§12.7 的**单态化 `<T : D>`** 之外，提供 spec `D` 的**运行时多态**形态：`Dyn<D>`（owned）与 `Dyn<D&>`（借用）。二者并存、互不替代。设计基调与决议详见草案 [`draft/DRAFT-dyn-draft.md`](draft/DRAFT-dyn-draft.md)（草案文件名保留历史 "draft" 字样，语义同 spec）。

### §12.9.1 类型档位

§12.9.1.1 两种形态均为 **fat pointer**，运行时表示为 `{ vtable_ptr, data_ptr }`（16 字节，sized）：

| 名称 | 写法 | data 端语义 | RC 行为 |
|---|---|---|---|
| owned dyn | `Dyn<D>` | 指向 `[RC head \| U 实例]`，与 `Rc<U>` 同源 | 标准 RC；强引用为 0 时调 `vtable[0]` 析构 |
| 借用 dyn | `Dyn<D&>` | 借自栈或堆，不持有所有权 | 不动 RC，按 §8.6 借用栈追踪 |

§12.9.1.2 `Dyn` 是编译器内置类型名（非关键字），**不**写在 `base.ut`；不引入 `dyn` 关键字（沿用 `Dyn<D>` 类型名形态，与 `Rc<T>` / `Weak<T>` 一致）。

§12.9.1.3 `Dyn<D>` 与 `Dyn<D&>` 不可互转，与 `T` ↔ `T&` 同理（§8.3）。同一 `U` 对不同 spec `D1` / `D2` 有**独立** vtable，互不复用。

### §12.9.2 出现位置

§12.9.2.1 `Dyn<D>` 可出现在：函数参数 / 返回类型、局部变量类型标注（带 / 不带初值）、结构体字段类型、`Array<Dyn<D>>` 元素类型。

§12.9.2.2 `Dyn<D&>` 仅出现在临时位（函数参数 / 返回类型、`let ... = ...` 初值形态）；**不**进结构体字段（字段不持借用，§7.4.4）、**不**进 `Array<...>` 元素（Array 为 owned 容器）。

§12.9.2.3 `Dyn<D&>` 在 owned 位（字段 / 别名 / 容器元素）由语义层拒绝（**E4038**）。`typeGeneric` 与 turbofish 接 `genericDefWithRef`，故 `Dyn:<D&>(x)` 与 `let r Dyn<D&> = …` 可写。

### §12.9.3 静态检查

§12.9.3.1 `Dyn<X>` 中 `X` **应当**解析到 `#Spec struct`（spec node）；否则报 **E1131**。

§12.9.3.2 **不**允许嵌套：`Dyn<Dyn<...>>`、`Rc<Dyn<...>>`、`Weak<Dyn<...>>` 报 **E1132**。

§12.9.3.3 **不**允许 `Dyn<D>?`（nullable dyn）报 **E1135**（v1 不引入）。

§12.9.3.4 spec `D` **应当对象安全**（§12.9.4），否则报 **E1134**。

§12.9.3.5 上述规则在所有声明位 `TypeNode` 上检查（free fn / impl 方法 / spec 签名 / struct 字段 / enum payload / 顶层类型别名）。

### §12.9.4 对象安全（object safety）

§12.9.4.1 spec `D` 在以下任一条件成立时**不对象安全**：

- spec 体内**任一**方法签名在 **receiver 之外**的位置出现 `Self` 类型；
- spec 体内**任一**方法签名在返回位置出现 spec 自身名（如 `#Spec struct D { fn dup() D }`）。

§12.9.4.2 v1 第一轮**不**为自反方法（`Self` / spec-name 返回）生成 thunk；用 `<T : D>` 单态化路径替代。Thunk 解锁留 v0.X+1。

§12.9.4.3 §12.3.2 已禁止 spec 方法本地泛型，§12.8 项 4 已禁止关联类型；静态字段契约不进入 vtable，不改变对象安全判定。

### §12.9.5 构造

§12.9.5.1 构造形态走 **turbofish** 类型构造：

```riu
let b Rc<U>      = U(...)
let d Dyn<D>     = Dyn:<D>(b)        ; Rc<U> → Dyn<D>，移交 RC
let r Dyn<D&>    = Dyn:<D&>(ref)     ; U& 或 Rc<U> → Dyn<D&>，借用
```

§12.9.5.2 调用站语法**应当**带 `:`（`Dyn:<D>(x)`）；无 `:` 写法 `Dyn<D>(x)` 仅在**类型位**有效（§B.2 / §B.2a）。`:` 前缀见 `riu.bnf` `exprCall` 形态。

§12.9.5.3 构造检查：

- `Dyn<D>(x)`：`x` **应当**为 `Rc<U>` 且 `U` 已通过 `#Impl(D)` 满足 `D`；否则报 **E1133**。
- `Dyn<D&>(x)`：`x` **应当**为 `U&` 或 `Rc<U>`，`U` 满足 `D`；结果为借用形态，按 §8.6 进借用栈。

§12.9.5.4 **不**走隐式 coercion，**不**引入 `as_dyn` builtin。

### §12.9.6 方法分派

§12.9.6.1 `d.m(args)`（`d : Dyn<D>` 或 `Dyn<D&>`）：

- D 中存在 `m` → 按 D 签名做参数 arity / 类型等价检查（§12.3.1）；缺失方法报 **E6016**，arity 不匹配报 **E6012**，参数类型不匹配报 **E6015**。
- 间接调用 `(ptr receiver, P1, ..., Pn) → R`，i 为 D 中 `m` 的声明序下标 + 1（跳过槽 0 的 dtor）。
- receiver：owned `Dyn<D>` 走 `data + sizeof(RCHeader)` 跳 RC 头；借用 `Dyn<D&>` 直接以 `data` 为实例指针。

§12.9.6.2 字段访问 `d.field` 在 dyn 形态下**不**暴露（dyn 不携带具体类型布局）。

§12.9.6.3 `==` / `same_ref` 等结构化相等 v1 **不**提供（用户契约自管）。

### §12.9.7 vtable 模型（*informative*）

§12.9.7.1 每个 `(具体类型 U, spec D)` 对生成一份静态 vtable：

```
__riu_vtable.<U全限定>.<D全限定>:
  [0] dtor:        fn(ptr) void          ; U 的类型特定析构
  [1] D.method_0:  fn(ptr, ...) -> R     ; 按 D 声明序
  ...
  [N] D.method_N-1
```

- 槽 0：`U` 需要析构时取 `~()`；否则 `null`。
- 槽 1..N：按 D 签名走 `Mangler::method`；找不到 fn 时 emit forward declare 占位。
- 内置 `U`（`i32` / `i64` / `bool` 等）receiver ABI 为 by-value，与 Dyn 调用站统一 `(ptr,...)` 派发不可调和；vtable 槽插入 `linkonce_odr` 适配 thunk：load primitive 后转发到 SDK fn。
- vtable 符号 `linkonce_odr`、`unnamed_addr`，每编译单元各发一份。

### §12.9.8 RC / 析构

§12.9.8.1 owned `Dyn<D>` 走 `_dyn_release(data, vtable)`：strong-- → 为 0 时 dispatch `vtable[0](data + sizeof(RCHeader))` → weak-- + free。借用 `Dyn<D&>` 释放 no-op。

§12.9.8.2 构造 `Dyn:<D>(b)` 的 RC 接管：源 `b` 是 fresh 临时（构造表达式直接消费）时偷取 +1；命名变量则 retain 拷 +1，源 `Rc` 仍按自身 scope 释放。Dyn 局部变量在 scope 退出走 `_dyn_release` 抵消。

### §12.9.9 调用约定 / ABI

§12.9.9.1 `Dyn<D>` / `Dyn<D&>` 作参数按 16 字节聚合传（与 `Rc<U> + ptr` 同形）；作返回值走 sret 形态。

§12.9.9.2 `Dyn<D&>` 借用与原 `U` 借用按 `data_ptr` 视作同一借用根，§8.6 借用栈复用。

### §12.9.10 FFI / `extern` 边界

§12.9.10.1 `Dyn<D>` / `Dyn<D&>` **不得**跨 `extern` 边界（vtable 布局是 riu 内部 ABI，不暴露给 C），报 **E1136**。

### §12.9.11 不在本节范围

承 §12.8：

- `Self` / spec-name 在返回位置的对象安全解锁（thunk 路径，留 v0.X+1）
- `Dyn<D>?` nullable 形态（§12.9.3.3 / `E1135` 占位）
- `Dyn<D>` ↔ `Rc<U>` 向下转型 / 反射 / `is` / `as`（§12.8 项 7）
- 多线程 vtable 跨线程引用（v1 单线程）
- 操作符 spec 的 dyn 化（§12.8 项 6）
- vtable 内联缓存 / devirtualization（性能任务）

## §12.10 默认方法体（默认实现 + fall-through）

§12.10 在 §12.1–§12.4 单态化 spec 体系之上，允许 spec body 内单个方法签名附带**默认方法体**：实现方未覆盖该方法时，由 spec 提供的默认体 fall-through 进入实现类型；多 spec 提供同名默认体冲突时，实现方**必须**显式覆盖以消歧。设计基调与决议详见草案 [`draft/DRAFT-spec-default-body.md`](draft/DRAFT-spec-default-body.md)（已落地）。

### §12.10.1 形态

§12.10.1.1 spec body 内每条方法签名**可选**附带函数体；带体即为该签名的"默认方法体"：

```riu
#Spec
struct Ord {
  fn cmp(other Self&) i32                             ; 仅签名 — 必须实现
  fn lt(other Self&) bool = $.cmp(other) < 0          ; fnExprBody 默认体
  fn le(other Self&) bool {                           ; fnBlockBody 默认体
    $.cmp(other) <= 0
  }
}
```

§12.10.1.2 默认体合法形态：

| 形态 | 例 | 备注 |
|---|---|---|
| 仅签名 | `fn cmp(other Self&) i32` | 必须由实现者写体 |
| `= expr` 单表达式体 | `fn lt(other Self&) bool = $.cmp(other) < 0` | 与 fn 通用 `fnExprBody` 一致 |
| `{ ... }` 块体 | `fn le(other Self&) bool { $.cmp(other) <= 0 }` | 可单行；多行时 `{` 后换行（§2.3.2.3） |

§12.10.1.3 默认体内 `Self` 视为**抽象类型变量**（具体化推迟到 §12.10.4 单态化时机）；`$` 在默认体内类型为 `Self&`（与 §12.2.3.2 一致）。

§12.10.1.4 默认体内**不得**引入方法本地泛型形参（沿用 §12.3.2 限制；违反报 **E1104**）。

### §12.10.2 不引入 `#Derive`

§12.10.2.1 v0.X **不引入** `#Derive(Spec)` 独立注解。`#Impl(D) struct S { ... }` 不写 `D` 的某方法即视为采用默认体（若该方法在 D 内有默认体）；该形态足够覆盖 SDK 内置 spec（§12.10.6）与用户场景。

§12.10.2.2 永远**不引入**"按字段递归自动 derive 默认体"（如 `ToJson.to_json` 自动遍历字段填 JSON）。这类需求由实现者手写体、或通过 §12.8 项 7 落地后的反射 API 显式展开。

### §12.10.3 sema 期占位校验

§12.10.3.1 默认体在 sema 期**仅做占位符号校验**，**不做完整 typecheck**（与 v1 泛型函数体"未实例化前不报体内类型错"的处理一致）：

- `$` 绑到 Self 抽象类型变量；
- `$.foo(args)` / 裸 `foo(args)` 中 `foo` **应当**为本 spec 内的某条签名（含本签名递归调用合法）；
- 引用 spec 外部符号（fn / 类型 / 常量）按 §10 名字解析正常查找。

§12.10.3.2 默认体引用本 spec 不存在的方法名 → 报 **E1140**（"spec `{}` default body references unknown method `$.{}`; must appear in this spec's signatures"）。

§12.10.3.3 默认体内类型不匹配 / 借用错误等深度检查推迟到 §12.10.4 单态化时机；无人实现的 spec 默认体错误**不报**（与"无人调用的泛型函数体不报错"现状对齐）。

### §12.10.4 fall-through 与单态化

§12.10.4.1 对 `#Impl(D) struct S { ... }`：S body 内方法集 + D 中**带默认体**的方法集合并后，**应当**覆盖 D 的全部签名。具体：

- 该方法在 S body 内已实现 → 实现覆盖默认体（默认体不参与单态化）；
- 该方法在 S body 内未实现 + D 中有默认体 → 默认体**fall-through**到 S，编译期合成一份属于 S 的方法实现；
- 该方法在 S body 内未实现 + D 中**无**默认体 → 报 **E1101**（"未实现 spec 方法"）。

§12.10.4.2 fall-through 实现的合成时机为类型层"实现宣告校验"通过后；其 typecheck / 借用检查 / IR 生成与实现者手写方法等价。

§12.10.4.3 默认体内 `Self` 在单态化时点替换为具体类型 `S`；`$` 类型固化为 `S&`。

### §12.10.5 组合冲突 E3132

§12.10.5.1 同一 `S` 上多条 `#Impl(D1) #Impl(D2) ...` 时，对每个 (方法名, arity) 二元组聚合各 spec 提供的签名条目，按下表判定：

| S body 实现 | 各 spec 默认体数 | 处理 |
|---|---|---|
| 已实现 | 任意 | 实现覆盖，OK |
| 未实现 | 0 | **E1101** 未实现 spec 方法 |
| 未实现 | 1 | 该默认体 fall-through 到 S |
| 未实现 | ≥2 | **E3132** 组合冲突，实现者必须显式覆盖以消歧 |

§12.10.5.2 "一种 spec 默认体 + 另一种纯抽象签名"**不**触发 E3132；默认体直接 fall-through，覆盖另一 spec 的抽象签名要求。

§12.10.5.3 v0.X **不引入**隐式优先级 / 顺序 / `use SpecA::method` 机制；冲突一律由实现者写显式覆盖解决。实现者写覆盖时如需 delegate 到任一 spec 的默认体，使用 `$.m@SpecA()` 形态的消歧调用语法（详见 §12.10.8）。

§12.10.5.4 E3132 消息：`Type `{}` inherits conflicting default bodies for method `{}` from specs {}; implementer must provide an explicit override`。

### §12.10.6 SDK 内置 spec

§12.10.6.1 `sdk/riu/src/riu/core/base.ut` 中 5 个内置 spec（`ToString` / `ToJson` / `Eq` / `Ord` / `Number`）的方法体策略如下：

- `Ord.lt` / `Ord.le` / `Ord.gt` / `Ord.ge` 由 `Ord.cmp` 默认体推导；
- `Eq.ne` 由 `Eq.eq` 默认体推导；
- `ToJson.to_json` / `Eq.eq` / `Ord.cmp` / `ToString.to_string` 维持**纯抽象签名**（按 §12.10.2.2 永不按字段递归自动 derive）。
- `Number` 的转换、运算与比较方法均为**纯抽象按值签名**，由数字类型的 `#Builtin` intrinsic 实现；不使用会回调同名运算方法的默认体（§12.7.5）。

§12.10.6.2 用户类型只需写 `#Impl(Ord) struct N { ... fn cmp(...) i32 { ... } }`，`lt/le/gt/ge` 自动 fall-through，无需重复实现。

### §12.10.7 不在本节范围

承 §12.8：

- ~~显式消歧调用语法（`a.SpecA::m(args)`）—— 留 v0.X+1。~~ → 2026-05-23 **已落地**（形态改为 `$.m@SpecA()` dot-call 后缀），见 §12.10.8。
- 按字段递归自动 derive 默认体（如 `ToJson.to_json` 字段遍历）—— §12.10.2.2 永不引入。
- `#Inline for` 编译期循环 unroll —— §12.8 项 16 永不引入。
- spec 体内的实例字段 / `#Static fn` / 关联类型 —— §12.1.1.1 / §12.8 项 4；静态字段契约和 `#Cval #Inline` 关联常量已落地。

### §12.10.8 消歧调用 `@SpecA` 后缀

§12.10.8.1 在 dot-call 的方法名后**可选**附加 `@ID` 后缀，显式指向某 spec 的默认方法体：

```
exprDot ::= expr LineEnd* '?'? '.' ID ('@' ID)? ...
```

调用形态：

| 写法 | 含义 |
|---|---|
| `$.m()` / `obj.m()` | 常规方法调用，dispatch 走 receiver 类型的方法表（spec 默认体已 fall-through） |
| `$.m@SpecA()` / `obj.m@SpecA()` | 显式指向 SpecA 的默认方法体 `m`；即便 receiver 类型覆盖了 `m`，仍走 spec 默认体 |

`@SpecA` 是**方法名上的标签**，`m@SpecA` 整体作为一个 callable name 解析，不是新的调用入口。`@` 后只接 spec 单名（按 §10 名字解析），**不**接路径前缀。

§12.10.8.2 sema 期判定（按序）：

1. SpecA 必须是 receiver 类型 `T` 自身 `#Impl` 列表里的 spec —— 若 T 未 `#Impl(SpecA)` → 报 **E1101**（与 §12.2.2.1 missing-impl 同语义类）。
2. SpecA 必须含名为 `m` 的签名 —— 否则报 **E1140**（消息按上下文区分"unknown method"）。
3. SpecA 中 `m` 必须带默认体 —— 纯抽象签名无法 disambiguate，报 **E1140**（消息按上下文区分"no default body"）。

§12.10.8.3 `@SpecA` 是 escape hatch：即便 T 覆盖了 `m`，`$.m@SpecA()` 仍指向 SpecA 的默认方法体。典型用法是在 §12.10.5 E3132 消歧覆盖体内 delegate 到 spec 默认体：

```riu
#Impl(A)
#Impl(B)
struct S {
  fn m() {                  ; 显式覆盖以消歧 E3132
    $.m@A()                 ; delegate 到 A 的默认体
    $.m@B()                 ; 再 delegate 到 B 的默认体
  }
}
```

若 T 未覆盖 `m` 且已 fall-through SpecA 的默认体，`$.m@SpecA()` 与 `$.m()` 行为一致（前者显式、后者由 fall-through 隐式选择）。

§12.10.8.4 `Dyn<D>` 上的形态：

- `d.m@D()` 合法且等价于 `d.m()`（dyn 携带 D 的 vtable，`@D` 仅作显式标注，仍按 vtable dispatch）。
- `d.m@OtherSpec()` 拒（dyn 只携带 D 的 vtable，无法 dispatch 到其它 spec）；复用 **E1101** 语义类（"Type 'Dyn' does not implement spec method"）。

§12.10.8.5 codegen：实现者类型 `S` 对每条 (spec, 带默认体的签名) 在 §12.10.4 fall-through 路径之外**额外合成一份** `S.m@<spec>` 符号；`$.m@SpecA()` / `obj.m@SpecA()` 调用点 codegen 期把 member 名重写为 `m@<spec>` 后走常规 dispatch。该额外符号仅在 `S` 实际参与 spec 实现宣告校验时合成，调用约定 / 借用语义与常规方法 + fall-through 等价（§8.6 / §8.5）。Mangler 不引入新规则——`@` 嵌在方法名里，沿用 `Mangler::method`，与源码 `@Spec` 同形。

§12.10.8.6 不在本节范围：

- `@` 后路径前缀 / 全限定形态（`@pkg.SpecA`）—— 留"统一路径形态"专项（与同期 `pkg.X<...>` turbofish / `#Impl(pkg.Spec)` 一并设计）。
- free fn / `Type::factory` / 构造调用上的 `@` 后缀 —— **不引入**；`@` 仅在 dot-call 出现。
- `@label` 用于 `break` / `ret` —— 同源 `@` 形态但分草案承担（`DRAFT-label-break.md` 或类似）；本节不绑死 label 语义。

## Open Issues

（无。操作符 spec 语法糖转后续版本，运算符维持 §7.2.3；`Eq` / `Ord` / `ToJson` / `Number` 方法契约已落地。`Stringify` 与 `assert_eq` 用户类型见 §11（v1 不引入；转后续版本）。跨编译单元边界 IR：v1 调用方需可见函数体（§6.4.2.3）。`Reflect` 编译期已落地（§13）；`is` / `as` / `AnyRef` 转后续版本。扩展实现块 / `extend` 转 v1.x 包管理。）

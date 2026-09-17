# 15 泛型 spec

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-13 |
| 旧档 | [DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) [#1.R]（类型参数 spec 已写入 §12，勿改草案） |

前置。[#2](2-iter.md) `Iter<T>` / `Indexed<T>` 依赖本条端到端可用。不绑版本。

## 提议

把 §12 已写的「spec 自身可带 `genericDef`」做成可编译、可校验的契约，而不是只存在于条文和 g4。

现状：

- 条文已允许 `#Spec struct To<T>`、`#Impl(To<i32>)`（§12.1.1.3 / §12.2.1.2）。g4 已收 `ID genericDef?`。
- §12.3.2.3 写了同一类型可并存 `#Impl(To<i32>)` + `#Impl(To<String>)`。**本条不落地该句**：struct 与方法仍合一、共用一个 method namespace（§12.2.3.3），两份 `to()` 签不同会 E1137；要并存得先把 impl 从 struct 里拆出去。
- SDK 与测试里**没有**泛型 spec。`ToString` / `Eq` / `Ord` / `Number` 全是零参。
- 实现半截：`SpecDeclNode` 存了形参；`#Impl` 只把 `typeNormal` 收进 `SpecRef.typeArgs`（注释写「复杂泛型实参押后」）；`<T : D<U>>` 边界只留 D 的基名，`boundSatisfied` 始终传空实参（`call_resolve.cpp` 注释 v0.5）。
- 不改 g4。不引入关联类型（§12.8 项 4）。spec 体内 `fn` 仍禁本地泛型（E1104）。

退出：用户能写 `#Spec struct D<T>`，在具体类型和泛型 struct 上**一条** `#Impl(D<…>)`，用 `<X : D<Y>>` 约束函数 / struct 形参；穷尽性、重复、边界、默认体 subst 与零参 spec 同一套规则。同一类型对同一 spec 基名不能写第二次。

## 评估

- 赞成：
  - Iter / Indexed / 转换类契约本来就要类型参数 spec，再拖会把 #2 做成编译器特判。
  - 形态已在 §12，本条补洞而不是另开语法。
  - 单态化静态分发即可；不碰 vtable。
  - 同类型只一条 `#Impl(D<…>)`：E1103 键维持 `(S, D)`，避开共享 namespace / 并存重叠。
- 反对：
  - `#Impl` / 边界目前只记基名，要改 AST 与 `.ud` 序列化，不是「打开开关」。
- 未决：无

## 决定

- 日期：2026-09-17
- 结论：实施
- 理由：条文已有形态；单 Impl、不做 Dyn；闭标在泛型 struct 允许。

## 规范要点

待实施后回写 §12 / §6.4.4 / 附录 D。下面是拟收口的洞（条文已有的不重复）。

- 语法：不改 g4。`#Spec struct D<T1, T2>`；`#Impl(D<A>)`；边界 `<X : D<A> + E>`。
- 单 Impl：同一类型对同一 spec **基名**只能一条 `#Impl`（`#Impl(D)` / `#Impl(D<A>)` / `#Impl(D<T>)` 都算）。E1103 键维持 `(S, D)`，与现在零参相同。`#Impl(To<i32>)` + `#Impl(To<String>)`、`#Impl(D<i32>)` + `#Impl(D<T>)` 一律拒。`#Impl(D<i32>)` + `#Impl(Eq)` 合法（不同基名）。
- 查找身份：满足关系仍按实参。`#Impl(D<i32>)` 的 `S` 满足 `D<i32>`，不满足 `D<String>`。边界查 `(S, D+实参)`；宣告次数查 `(S, D)`。
- 缺参 / 错元：`D` 声明带 N 个形参时，`#Impl(D)` 无 turbofish、或实参数 ≠ N → 编译期错误（现有 E6010/E6011 类，或专码；落地时定）。
- 实参形态：`#Impl` 与边界里的实参是普通 `type`（标量 / 用户类型 / 实现者自己的形参名 / 嵌套泛型）。禁止 `T&`（E4037，与其它类型实参同规）。
- 三种合法 `#Impl`（每种在同一 `S` 上对同一 `D` 只能挑一条）：
  - 闭、非泛型：`#Impl(D<i32>) struct S` — 只这一份。
  - 闭、泛型：`#Impl(D<i32>) struct S<T>` — 任意 `T` 的 `S<T>` 都实现 `D<i32>`（与现有 `#Impl(ToString) struct Foo<T>` 同一模型：impl 不提 `T`，每个单态一份相同契约）。
  - 开：`#Impl(D<T>) struct S<T>` / `#Impl(D<K>) struct Map<K, V>` — 每个单态一份；实参可以是实现者的任一形参，不必与 spec 形参同名。
- 无 `where`、不特化：不能写「仅当 `T : Foo`」或另开一份 `S<i32>` 更特殊的 `#Impl`。收窄只能自己再包一层 newtype。
- 签名 subst：穷尽性把 spec 形参换成 `#Impl` 实参，再比 §12.3.1。`Self` 仍换实现者（泛型则是 `S<T>`，单态后再换）。默认体克隆同一张 subst 表。
- 边界：`<X : D<A>>` 在单态化时查 `X` 是否有 `#Impl(D<A>)`（A 先 subst）。只查基名 `D` 不够。形参方法查找（`typeParamBoundHasMethod` 一类）同样要带实参，才能让 `<I : Iter<T>>` 里 `it.next()` 的返回是 `T?`。闭标在泛型 struct 上时，查表按 struct 裸名 + `D` 的闭实参，忽略 `X` 自己的类型实参（`S<String>` 仍满足 `D<i32>`）。开 impl 要把 `X` 的实参 subst 进 `#Impl` 实参再比，不能拿源码字面 `D<T>` 去跟 `D<i32>` 比字符串。
- 不做：关联类型；spec 方法本地泛型；super-trait；`where`；`Dyn<D<T>>`（零参 `Dyn<D>` 维持 §12.9，本条不扩展）；同一类型对同一 spec 基名多条 `#Impl`（含不同实参并存）。`$.m@D()` 仍按基名；本条不改 g4。
- 并存的前置：[16 分离 impl](16-impl-split.md)。落地本条时把 §12.3.2.3「并存」改成「本阶段不成立；见 #16」。

## 落地

- 2026-09-17（`notes/0.23`）：AST / `.ud` v3 边界带实参；E1103 维持 `(S, D)`；E1142 缺参错元；`boundSatisfied` subst 开 impl；check-cases + `tests/projects/spec_generic`；回写 §12 / §6.4.4 / 附录 D。不落地 Iter / 并存 / 分离 impl / Dyn。

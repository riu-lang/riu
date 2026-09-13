# 15 泛型 spec

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-13 |
| 旧档 | [DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) [#1.R]（类型参数 spec 已写入 §12，勿改草案） |

前置。[#2](2-iter.md) `Iter<T>` / `Indexed<T>` 依赖本条端到端可用。不绑版本。

## 提议

把 §12 已写的「spec 自身可带 `genericDef`」做成可编译、可校验的契约，而不是只存在于条文和 g4。

现状：

- 条文已允许 `#Spec struct To<T>`、`#Impl(To<i32>)`、同一类型并存 `#Impl(To<i32>)` + `#Impl(To<String>)`（§12.1.1.3 / §12.2.1.2 / §12.3.2.3）。g4 已收 `ID genericDef?`。
- SDK 与测试里**没有**泛型 spec。`ToString` / `Eq` / `Ord` / `Number` 全是零参。
- 实现半截：`SpecDeclNode` 存了形参；`#Impl` 只把 `typeNormal` 收进 `SpecRef.typeArgs`（注释写「复杂泛型实参押后」）；`<T : D<U>>` 边界只留 D 的基名，`boundSatisfied` 始终传空实参（`call_resolve.cpp` 注释 v0.5）。
- 不改 g4。不引入关联类型（§12.8 项 4）。spec 体内 `fn` 仍禁本地泛型（E1104）。

退出：用户能写 `#Spec struct D<T>`，在具体类型和泛型 struct 上 `#Impl(D<…>)`，用 `<X : D<Y>>` 约束函数 / struct 形参；穷尽性、重复、边界、默认体 subst 与零参 spec 同一套规则。

## 评估

- 赞成：
  - Iter / Indexed / 转换类契约本来就要类型参数 spec，再拖会把 #2 做成编译器特判。
  - 形态已在 §12，本条补洞而不是另开语法。
  - 单态化静态分发，不碰 vtable 模型也能先用（`Dyn<D<T>>` 可后切片）。
- 反对：
  - `#Impl` / 边界目前只记基名，要改 AST 与 `.decl` 序列化，不是「打开开关」。
- 未决：
  - `Dyn<D<T>>` 是否跟本条一起做（倾向：本条不做；vtable 按 `(U, D<T>)` 对，留给用到再开）。
  - `#Impl(D<i32>)` 标在 `struct S<T>` 上：表示「任意 `T` 的 `S<T>` 都实现 `D<i32>`」。允许。

## 决定

- 日期：
- 结论：实施 / 关闭
- 理由：

## 规范要点

待实施后回写 §12 / §6.4.4 / 附录 D。下面是拟收口的洞（条文已有的不重复）。

- 语法：不改 g4。`#Spec struct D<T1, T2>`；`#Impl(D<A>)`；边界 `<X : D<A> + E>`。
- 身份：`D<A>` 与 `D<B>` 是不同实现位。E1103 的键是 `(S, D+实参)`，不是裸 `D`。
- 缺参 / 错元：`D` 声明带 N 个形参时，`#Impl(D)` 无 turbofish、或实参数 ≠ N → 编译期错误（现有 E6010/E6011 类，或专码；落地时定）。
- 实参形态：`#Impl` 与边界里的实参是普通 `type`（标量 / 用户类型 / 实现者自己的形参名 / 嵌套泛型）。禁止 `T&`（E4037，与其它类型实参同规）。
- 两种合法 `#Impl`：
  - 闭：`#Impl(D<i32>) struct S` — 只这一份。
  - 开：`#Impl(D<T>) struct S<T>` / `#Impl(D<K>) struct Map<K, V>` — 每个单态一份；实参可以是实现者的任一形参，不必与 spec 形参同名。
- 签名 subst：穷尽性把 spec 形参换成 `#Impl` 实参，再比 §12.3.1。`Self` 仍换实现者。默认体克隆同一张 subst 表。
- 边界：`<X : D<A>>` 在单态化时查 `X` 是否有 `#Impl(D<A>)`（A 先 subst）。只查基名 `D` 不够。形参方法查找（`typeParamBoundHasMethod` 一类）同样要带实参，才能让 `<I : Iter<T>>` 里 `it.next()` 的返回是 `T?`。
- 不做：关联类型；spec 方法本地泛型；super-trait；`where`；本条范围内的 `Dyn<D<T>>`。

## 落地

- （未开始。建议切片：AST/边界带实参 → 穷尽性+E1103 闭包 → `<X : D<A>>` 单态化校验 → SDK 最小用例 `To<T>` 级测试。不在本条落地 Iter。）

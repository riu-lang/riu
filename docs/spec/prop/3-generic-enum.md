# 3 泛型 enum

| | |
|---|---|
| 状态 | 实施中 |
| 开 | 2026-09-13 |
| 旧档 | [DRAFT-枚举.md](../draft/DRAFT-枚举.md)（非泛型 enum + match 已落地，勿改） |

前置：无。[#2](2-iter.md) 的 `IterErr<E>` 只需要本条**简单切片**。不绑版本。

## 提议

给已落地的 enum / match 加上类型参数，单态化后与现在的非泛型 enum 同一套 tag + payload + match。

简单切片（#2 前置，**先做完再谈其余**）：

- `enum E<T>` / `enum E<T, U>`；payload 可用形参（`Fail(E)`）。头上**不加边界**。
- 使用点写出实参：`IterErr<NetErr>`；**每个单态一份**布局 / 拷贝 / 析构。
- 空 enum 仍拒（match 没有 variant 可用）。
- `match`、`E::V` / `E::V(payload)`、作 `T ! E` 的 E，与现在相同。

本条其余（后切片，不挡 #2）：enum 上方法 / `#Impl` / struct-style payload / 显式 discriminant。

**改 g4**：`enumDecl` 为 `enum ID genericDef? BlockStart`（复用 `genericDef`，不另开产生式）。spec 正文等简单切片完成再回写。

## 评估

- 赞成：
  - 错误包装 / Iter 结束与业务失败拆开，都要「带一个类型实参的 enum」，不是新枚举机制。
  - 布局与 match 已有；按实参 subst payload 再走现有路径。
- 反对：
  - 必改 g4。
  - 现失败通道多把 E 存成**名字字符串**（E7008、透传、mangle）。泛型 E 必须改成完整 `TypeInfo`（`IterErr<NetErr>` ≠ `IterErr<ParseErr>`），否则 #2 落不了。
- 未决：无

## 决定

- 日期：2026-09-17
- 结论：实施
- 理由：简单切片先落地；g4 复用 `genericDef`；头上边界语义拒。

## 规范要点

简单切片完成后回写 §3.10.2.6 / §3.10.1.1 / 附录 B。后切片另写，不提前改那些条。

### 语法

```riu
enum IterErr<E> {
  End
  Fail(E)
}
```

- `genericDef` 与 struct / fn 头同一槽：裸形参名，禁止 `T&`（E4037）。
- **不加边界**：简单切片 enum 头不许 `<T : D>`。g4 复用 `genericDef` 会收下，语义层拒（不另开无边界产生式）。与 struct 头对齐边界是后切片。
- 使用点必须带齐实参（E6010/E6011 类）。实参普通 `type`；禁止 `T&`。
- variant 仍一行一个、全限定 `IterErr::End` / `IterErr::Fail(e)`。match 穷尽按**该单态** subst 后的 variant 集（与现在相同，不因形参多分支）。
- **无空 enum**：零 variant 仍拒（§3.10.2.5），泛型同样。不引入 `Never`。没有 variant 则 match 穷尽 / 构造都没有合法臂。

### 语义

- 名义类型 + 实参：`IterErr<NetErr>` 与 `IterErr<ParseErr>` 不同。
- **每个单态一份**：`enum Box<T> { V(T) }` 的 `Box<i32>` / `Box<String>` 各一份布局与拷贝 / 析构。不做「只对 `Box<i32>` 特化」的第二份声明。
- 单态化：subst payload → 现有 tag/union 布局；RC payload 仍按 tag dispatch。
- 作 `fn f() T ! IterErr<E>` 的 E：E 仍须是 enum；比较 / 透传 / E7008 用完整类型，不用裸名。
- 形参出现在 payload 外（`enum E<T> { A }` 的 `T` 未用）：与未用的 struct 形参同一档，编译期可留、实例化仍要给实参。

### 明确不做（简单切片）

- enum 头 `<T : D>`；空 enum / `Never`；按实参特化第二份 `enum`。
- enum 体内 `fn` / `#Impl` / `#Spec`。
- struct-style payload、显式 discriminant、`as i32`。
- 改 match 语法；嵌套模式 / `|` / 守卫（仍无）。
- 把 `Nullable` / `T?` 重写成 enum。
- #1 的跨类型自动包装 / 内置 `Result<T, E>` 根类型。

## 落地

- 3.1：`enumDecl` + `genericDef?`；`EnumDeclNode` 形参 / 头上 `: D` 进 AST。`.ud` / 语义 / codegen 仍后切片。
- 3.2（2026-09-17）：使用点实参个数 E6011；头上 `<T : D>` → E2037；`E<T>` = 名义名 + `genericArgs`，lookup 仍裸名。
- 3.3（2026-09-17）：构造 `E:<T>::V` / `E:<T>::V(payload)` 按该次实参 subst payload；缺 turbofish → E6011。
- 3.4（2026-09-17）：match 穷尽按 variant 名；绑定类型按 scrut 的 genericArgs subst payload。
- 3.5（2026-09-17）：每单态一份 LLVM 类型（`generic::EnumInstance`）；拷贝 / 析构 tag dispatch 走 subst 后 payload。

# 16 分离 impl

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-17 |
| 旧档 | [DRAFT-draft.md](../draft/DRAFT-draft.md) `Type : D { }`；[DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) [#1.B] 合一 / [#1.M] `extend` |

[#15](15-spec-generic.md) 单 Impl 不依赖本条。本条是 15 里「不同实参并存」的前置。形态已拍（A）；改 `riu.bnf` 等本条 `待实施`。不绑版本。

## 提议

把 spec 实现从 `structDecl` 体里拆回独立顶层块，每块一个 `(类型, spec)`，自己的方法 namespace。

v0.5 本来就是分离的：`struct S { 字段 }` + `S { 固有方法 }` + `S : D { spec 方法 }`。2026-05-19 spec-unify [#1.B] 并进单一 `structDecl`，理由是「找类型行为只看一个地方」。AST 没真正合一——`visitStructDecl` 仍同时造 `StructDeclNode` + `StructImplNode`。合一的是文法表面和共享 method namespace（§7.8.2 / §12.2.3.3）。

[#15](15-spec-generic.md) 要把 `#Impl(To<i32>)` 与 `#Impl(To<String>)` 做成不同实现位。共享 namespace 下两份 `to()` 签不同 → E1137。所以 15 先单 Impl；并存要先拆块。

不引入 `impl` / `for` / `extend` 关键字（附录 A.6.2 明确不用）。不把 Rust 的 `impl Trait for Type` 套过来。

退出：用户能写独立 spec 块；同一 `S` 上 `#Impl(To<i32>)` 与 `#Impl(To<String>)` 各一块、各一份 `to()`；调用点能消歧。固有方法 / 析构 / `#Static` 仍跟类型走。orphan 仍是 S 包或 D 包。

## 评估

- 赞成：
  - 15 的并存只卡在共享 namespace，不是缺类型论。
  - AST 已有 `StructImplNode` + `specRefs`；表面语法搬回去，不是新机制。
  - 一条块一个 D，E1102 可以回来（块内只许该 spec 的方法），比现在「混在 struct 里当普通方法」干净。
  - 顶层块写在 D 包就能给 SDK 类型补本包 spec，[#1.M] `extend` 主场景被覆盖，不必另开关键字。
- 反对：
  - 推翻 [#1.B]。固有方法还在 struct 里，spec 实现要按 `(S, D)` 找块——这本来就是 registry 的键。
  - 必改 `riu.bnf`；SDK `#Impl(D) struct T` 全量迁一次。
- 未决：无（2026-09-17 已收：A / 不留糖 / `@D<A>` / `extend` 不动）

## 决定

- 日期：
- 结论：实施 / 关闭
- 理由：

## 规范要点

待实施后回写 §2.2 / §7.1 / §7.8 / §12.2 / §12.3.2.3 / §12.5.3 / 附录 B/D。不改附录 A 的 `extend` 预留。

### 形态 A

struct 留字段 + 固有方法 + 析构 + `#Static`。`#Impl` 不再贴 `struct`（无过渡糖）。spec 走独立块：一条 `#Impl(D)` / `#Impl(D<A>)` + 已声明类型名 + 方法体。类型形参从该 struct 声明带进块内，块头不写 `<T>`。一条块恰好一个 spec，不能 `#Impl(Eq) #Impl(Ord)` 堆叠。

```riu
#Spec
struct To<T> {
  fn to() T
}

struct Counter<N> {
  count N

  fn bump() {
    ; 固有
  }
}

#Impl(To<N>)
Counter {
  fn to() N {
    $.count
  }
}
```

`#Impl(To<i32>) struct Other { ... }` 非法，须拆成 `struct Other { ... }` + `#Impl(To<i32>) Other { fn to() i32 { ... } }`。

- 不恢复 `S { }` 无 `#Impl` 的固有块。
- 不恢复 `S : D` / `S : D1 + D2`。
- 不用 `impl` / `for` token。`extend` 预留本条不改。

### 语义

- 身份：E1103 在本条改成 `(S, D+实参)`。`#Impl(To<i32>)` 与 `#Impl(To<String>)` 可并存。`#Impl(To<i32>)` 与 `#Impl(To<N>)` 在 `Counter<N>` 上可合一 → 仍 E1103（与 15 的重叠规则接上）。同一 `(S, D+实参)` 两块 → E1103。
- 穷尽性：块内方法 + 该 spec 默认体，覆盖该 D 签名。缺 → E1101。块内出现非 D 方法 → E1102（收回）。
- 不从固有方法偷偷满足 spec（旧 E1105 显隐冲突维持）。要给 spec 用的方法写在 spec 块，或靠默认体。
- 块内 `Self` / `$` 与现在 struct 方法相同，绑定目标类型。
- orphan：块必须出现在 S 所在包或 D 所在包（§12.5 不变）。因此 `MyShow` 包里可以对 `Array` 写 `#Impl(MyShow) Array { ... }`，不需要 `extend`。
- 查找：`<X : D<A>>` / `boundSatisfied` 按块登记的 `(S, D+实参)` 查，与 15 同一套。

### 调用

- 仅固有、或仅一块 spec 提供 `m`：`obj.m()` 照旧。
- 两块 spec 都提供 `m`（典型：`To<i32>` 与 `To<String>` 的 `to`）：`obj.to()` 歧义，必须 `obj.to@To<i32>()`。`@` 后接 `ID genericDef?`（现 `@ID` 不够，本条改 `riu.bnf`）。
- `$.m@D()` 仍是 escape hatch，指向该 spec 默认体（§12.10.8）；带实参时指向那一份。

### 不做

- 不拆固有方法出 struct（不恢复 `S { }` 无 `#Impl` 块）。
- 不引入 `where`、impl 级额外形参、特化 `Counter<i32>` 另写一块。
- 不引入 `impl` / `for` token。
- 不改 `extend` 预留（后续另说）。
- 不在本条做 `Dyn<D<T>>`（15 已划掉）。
- 不在本条改包管理。

## 落地

- （未开始。建议切片：`riu.bnf` 收独立块 + 禁 `#Impl` 贴 struct → E1103 改键 → E1102 收回 → `@D<A>` → SDK 迁 ToString/Eq/Ord。15 的单 Impl 可先落地，本条再打开并存。）

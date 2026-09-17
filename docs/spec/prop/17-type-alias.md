# 17 类型别名 `type` 关键字 / 块作用域

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-17 |
| 旧档 | 无。现 §3.9 / g4 `aliasDecl`：顶层 `Name = type` / `Name<T> = type`，**无** `type` 关键字；不在 struct / 函数体。附录 A.6.2：`type` v1 不引入，可作 `ID`。 |

相关：[2 Iter](2-iter.md) 用块内短名写 `IterItem<…>`；**不挡** #2。本条不发明关联类型。反射字段改名与本条同切片（避免 `type` 既是关键字又是 `ID`）。

## 提议

给已落地的透明类型别名加上关键字 `type`，并放到任意 `statementBlock`（函数体 / `if` / `elif` / `else` / `loop` / `for` / `match` 臂 / `try`/`catch`）以及 `struct` 体。语义仍是 §3.9 透明替换，不是新名义类型。

`type` **升格为真正的关键字**（与 `fn` / `let` / `struct` 同档）。词法一层：`type` 永远是 keyword token，不是「有时 ID」。插件 / LSP 只加一词，不写上下文规则。

因此 Reflect 静态字段现名 `type`（`Counter::type`）改成 `type_info`。

```riu
type Item = IterItem<i32, End>
type P = Pair<A, B>

fn f() {
  type R = IterItem<u8, NetErr>
  loop {
    match it.next() {
      R::Item(v) { }
      R::End { break; }
      R::Error(e) { ret e }
    }
  }
}

let t Type = Counter::type_info
```

## 评估

- 赞成：
  - `IterItem<T, E>` 在方法里反复出现，文件顶层别名不够近。
  - 块内 `type A = T` 与赋值 `A = expr` 不打架。
  - 透明替换已有（`resolveAlias`、E2016、E2017）；本条是出现位 + 关键字 + 反射改名。
  - 关键字化比「关键字/标识符双关」更简单：g4 / tmLanguage / tree-sitter 都只加一个词。
- 反对：
  - **必改 g4**。
  - 现网顶层 `A = T` 全量加 `type`。
  - 第一刀左侧无泛型：现有 `Pair<T> = (T, T)` 两条测试要改写或挪后切片。
  - Reflect 用户 API `T::type` → `T::type_info`（源码面很小：两处正测 + 一条 E1138）。
- 未决：无

## 决定

- 日期：2026-09-17
- 结论：实施
- 理由：
  - 一律 `type Name = T`（顶层旧写法迁完删）。
  - 左侧第一刀无 `genericDef`：`type P = Pair<A, B>` 合法；`type Pair<T> = (T, T)` 后切片。
  - 右侧根类型不得裸 `&`（`type A = i32&` 仍 E4039）：用别名时看不出是不是借用。内层 `Function<i32&, bool>` 按现规，别名本身不是 `&`。
  - `type` 只当关键字。Reflect 字段改为 `type_info`（`Counter::type_info` 得 SDK `Type`）。不写 contextual keyword。
  - struct 体内 = 该声明范围内 typedef，不是 `Foo::Item` 关联类型。
  - 不挡 #2。

## 规范要点

待实施后回写 §3.9 / §13 / 附录 A/B。g4：

```
aliasDecl ::= 'type' ID '=' type LineEnd
```

- 顶层 + `statement` 一条 + `structDecl` 字段段可出现（仅该 struct 内可见）。
- 无左侧 `<T>`。右侧 `type` 产生式仍可写 `Pair<A, B>` / `T?`；根为 `T&` → E4039。
- 透明、成环 E2016、撞名 E2017。作用域与 `let` 同。
- lexer：`TypeKw : 'type';` 从 `ID` 划走。附录 A 从 A.6.2 挪到关键字表。

### Reflect `type_info`（与本条绑定）

现 `T::type` / 实例 `c.type`（E1138）改为 `T::type_info` / `c.type_info`。返回值仍是 SDK `#Builtin struct Type`（`.name`）。LLVM 全局 `__riu_reflect_<mod>_<name>__type` 与 intrinsic `__riu_reflect_type:<T>()` **不动**（内部符号）。

SDK **没有** `#Spec struct Reflect` 源文件；四个名字是编译器魔串。改名 = 换魔串 + 改调用点 + 回写 §13。须在 `type` 升关键字**之前**落地，否则 `Counter::type` 无法再当 `ID` 解析。

## 落地

- 17.1–17.5（2026-09-17）：Reflect `type_info`；`TypeKw` + `type Name = T`（无左侧泛型）；块 / struct 作用域；formatter / tmLanguage / LSP；回写 §3.9 / §13 / 附录 A/B。

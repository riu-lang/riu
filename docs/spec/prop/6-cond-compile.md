# 6 条件编译 + 用户注解

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-13 |
| 旧档 | `DRAFT-spec-unify.md` [#1.P] / 未落地的 `DRAFT-anno-struct.md`。反射 `#Reflect` 命名参数挂 [#4](4-reflect.md) |

不绑版本。语法要改 `riu.bnf`（`annoArg` / 命名参数 / 语句位注解），**拍板前不改 bnf**。派生走 [#7](7-codegen.md)。`Platform::*` 与包条件走 [#10](10-multi-target.md)。裸 `#Anno` 缺省字段走 [#23](23-field-default.md)。不定长列表用现行 `Array<T>`。本轮**仅内置**用这套形态；不开放用户 `#Anno struct`。用户注解执行也不做。

## 提议

注解是普通 struct，用 `#Anno` 登记。使用处 `#Name(...)` 按字段填编译期常量。条件编译是其中一条编译器认识的注解 `#If`，条件只能引用常量。代码块尾随 lambda（Kotlin `run { }`）**本提案不做**。

```riu
#Anno(on=[AnnoType::Code])
struct If {
  value bool          ; 单位置参数可省略 value=
}

#Anno
struct Anno {
  repeatable bool = false          ; 默认见 #23
  on Array<AnnoType> = []
}

#Anno(repeatable=true, on=[AnnoType::StructDecl])
struct Impl {
  value Type          ; 定义是 Type；实参写字符串 / 简单 ident
}

#Impl("Iter<i32>")    ; 含 < > 必须字符串，按 type 拼写再解析
#Impl(Eq)             ; 无尖括号的裸名仍可 ident

#If(COUNT==1)
println("")

#If(COUNT==1)
fn onlyWhen() { }
```

`Platform::win` 等目标开关留给 #10；本提案 `#If` 只接用户 `#Cval` / 字面量 / `#Const fn`。

## 评估

### 现状（2026-09-22）

编译器白名单，无用户入口。参数槽是**单参字符串**：

```
buildAnno ::= '#' ID ( '(' annoArg ')' )? LineEnd
annoArg   ::= ID genericDef? | INT | FLOAT | STR | type
letAnno / paramAnno ::= '#' ID          ; 零参，无括号
```

| 形态 | 有无 |
|---|---|
| `#Name` 零参 | ✅ `#Builtin` `#Test` `#Const` `#Mut` `#Cval` `#Frozen` `#Val` `#Static` `#Inline` `#Spec` `#Packed` `#NoCopy` `#Reflect` `#NoReturn` |
| 单参字面量 | ✅ `#Align(8)` `#CName("foo")` |
| 单参类型 | ✅ `#Impl(Iter<i32>)` |
| 表达式 | ❌ `#If(COUNT==1)` 过不了 parser（`==`） |
| 命名 / 多参 | ❌ `#Anno(repeatable=true, on=[...])` |
| 语句位 | ❌ `parseStatement` 只在后面是 `let` 时吃 `#`（`#Mut let`）；`#If` + `println` 会 rewind |
| 形参 | 仅零参档位 `#Frozen`（`paramAnno`） |
| 用户自定义 | ❌ 未知名 E2005 / E3112 |
| 实参 AST | ❌ `Annotated._annoArgs` 是 `vector<string>` |

const-eval 已有：字面量、`#Cval` 名、算术 / 比较 / `&&` `||`、`#Const fn`、struct 字面量。**没有**：`Type::FIELD`、enum 变体、数组字面量、字符串。

### `<` / `>` 实测（`riu-ast --rd`，现行 `parseAnno` = 非字面量走 `parseType`）

| 输入 | 注解槽（现行） | 表达式位 |
|---|---|---|
| `Foo<i32>` | ✅ `TypeGeneric` | `(Foo < i32) > ∅` 比较垃圾树 |
| `Foo<i32>::n` | ❌ `::` E1002（类型吃完 `Foo<i32>`） | 同上，`::` 被 prefix 静默吞掉 |
| `Foo<i32>::n > 1` | ❌ 同上 | 裂成比较 + 下一句 `n > 1` |
| `Foo:<i32>::n > 1` | ❌ `:` E1002 | ✅ `EnumCtor` 再 `>` |
| `COUNT==1` / `n > 1` | ❌ `==` / `>` E1002 | ✅ 比较 |
| `a<b`（无闭合 `>`） | ⚠️ 收成 `TypeGeneric a[b]`（`eat('>')` 失败不报） | ✅ `a < b` |

结论：语言里**没有** `Type<T>::static_var`。泛型静态是 `Type:<T>::static_var`。`#Impl(Foo<T>)` 与 `#If(Foo:<T>::x > n)` **不撞**：前者类型吃到 `)`，后者有 `:<` / `::`，类型吃不完。

真歧义只有 **`a<b`：类型实参 vs 小于**。不在注解槽里 parse `type` 之后这条消失：`Type` 字段的实参走表达式（字符串 / 裸 ident），`<` 只出现在字符串里或比较里。

### 赞成 / 反对

- 字面量 / 类型 / 表达式按字段类型填编译期值，`#If` / `#Impl` 同一套。
- `value=` 省略对齐 Kotlin。
- `#Mut` 等档位不必做成 struct。
- 比只扩 `annoArg ::= expr` 重；`Type` 字段名撞反射 `struct Type`。`on` 用现行 `Array<T>`（编译期常量列表，字面量仍是堆句柄形态）。

### 未决（改 bnf 前仍要拍）

10. ~~用户 `#Anno struct`~~ → **先仅内置**（2026-09-23）。不开放用户登记；未知名仍 E2005 / E3112。

## 决定

- 日期：2026-09-22；`on` 改回 `Array<T>`、未决 10：2026-09-23
- 结论：语义已定（状态仍 **提议**，未改 `待实施`）。形态给内置；用户 `#Anno struct` 本轮不开。**尚未实施。**

1. **产生式**：`buildAnno` 扩成可多参 / 命名：`annoArg ::= ID '=' annoVal | annoVal`，**`annoVal ::= expr`**（不再 `| type`）。顶层声明与**语句**都可挂 `buildAnno*`。`letAnno` 并进 `buildAnno`。**不在形参**：`paramAnno` 仍只零参档位（`#Frozen`）。
2. **`<` 歧义**：注解槽不 parse `type`，`<` 不是泛型。比较照常。泛型静态仍是 `Type:<T>::x`。不新增 `Type<T>::x`。
3. **`#If` 附着**：语句（`AnnoType::Code`）+ 声明（struct / spec / fn / extern / let / enum / 字段）。**不在形参**。无 `#Else` / `#Elif`。无尾随 `{ }`。假则丢掉目标。
4. **`Type` 字段**：定义仍 `value Type`（名义类型，不是改成 `String`）。实参是无插值字符串，内容按 `type` 再解析：`#Impl("Iter<i32>")`。裸 ident `#Impl(Eq)` 当类型名（无 `<` `>` `&` `?` `,`）。现有 `#Impl(Iter<i32>)` 迁成带引号。不要写 `Eq::type_info`。
5. **缺省实参**：不特判元注解。字段默认值是普通 struct 能力，见前置 [#23](23-field-default.md)（已落地；默认必须 **const**）。
6. **`on` / 不定长**：字段类型 `Array<AnnoType>`。实参 `[a, b]` / `[]`。不做切片。
7. **内置声明**：先在 SDK 写 `#Anno struct If` / `Impl` / `CName` / `Align` 等给用户看；编译器行为仍走现白名单，不读这些 struct。诊断可以引用 SDK 声明（「见 `If`」）。以后再迁到按声明驱动。档位 `#Mut` `#Cval` `#Builtin` 等仍不做成 struct。
8. **`Platform`**：留给 [#10](10-multi-target.md)。包 / 依赖条件同样跨平台，现在不做。
9. **假分支**：跟 Rust `#[cfg]`。目标先 parse（句法错误照报）；sema 前摘掉，不进 typeck / codegen。假支里未定义名、类型错、链不到的符号不报。同名两项一项假则只留真的。不挂在摘掉后句法不成立的位置（表达式中间）。
10. **用户 `#Anno struct`**：本轮不开。语法与字段规则按内置声明写；使用处只有编译器白名单里的名字。用户自己 `#Anno struct Foo` 不登记，`#Foo` 仍未知名。元数据 / 执行留给以后。

## 规范要点

拍板后写。草稿：

- **注解类型**：本轮仅内置。SDK 可写 `#Anno struct` 给人看（决定 7），编译器不按它登记。用户 `#Anno struct` 不生效；使用处未知名仍 E2005 / E3112。字段类型仅编译期可求值子集。无方法、无 `#Mut` 字段。
- **使用**：`#Name` / `#Name(实参)`。实参是常量表达式。`Type` 字段：无插值 `"Foo<T>"` 或裸 ident `Foo`。名为 `value` 的字段可省略字段名（仅一条位置实参）。其余 `字段=值`。
- **`repeatable`**：true 才允许叠同名（`#Impl`）。
- **`on`**：`Array<AnnoType>`。不在列表的位置报错。无 Param。
- **`#If`**：罩下一条声明或语句。条件 `bool` 常量。假：仍 parse，sema 前丢掉（Rust `#[cfg]`）。
- **常量**：字面量、`#Cval` / `#Inline #Cval`、`Type::` 静态 `#Cval`、`#Const fn`。禁止 `#Mut`、运行时调用。不含 `Platform::*`（#10）。
- **档位修饰**（`#Mut` `#Frozen` 形参等）保持现白名单。

## 落地

（未开始）

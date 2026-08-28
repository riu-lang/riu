# yux 语法

写 `*.yux` 前看。写完 `yux-check <file>`。细则 [docs/index.md](../docs/index.md)、[docs/spec/index.md](../docs/spec/index.md)。

- 注释 `;`（行首或 `空格+;`），不是 `//` `#`
- 关键字后、二元运算符两边、`,` 后有空格；`()` `[]` 内无空格
- 局部 `let name Type = expr`；struct 字段无 `let`
- `#Mut` / 注解顶行堆叠，每个 `#Anno` 单独一行在 fn/let 上
- 当前实例 `$`（`$.field` / `$.method()`），不是 self/this
- `{ }` 可单行（`fn f() { ret 1 }`）；match 臂一行一个，臂体 `=> expr` 或 `=> { stmts }`；别把 struct 字面量压成非法单行
- 单表达式体 `fn foo() i32 = 42`（`=` 与 expr 同行）
- 无独立裸 `{ }` 语句块；同块 `let` 不重名；if/loop 体内可遮蔽
- 数组 `[T * N]` 的 N 只能是 INT 字面量
- `heap:<T>(v)` / `rc:<T>(v)` 必须写 `:<T>`，不从参数推导
- `a <- b` 是表达式（返回旧值）；`a = b` 是语句
- 无隐式转换，用 `.to_<type>()`
- 无 `++` `--` `+=` `?:`；`&&` `||` 短路，位运算 `and` `or`

整数后缀 `42i8` `100u32` `0xFFu64` `42usize`。声明类型 / 对侧类型 / 非泛型参数已知可省略；方法参数、数组元素、无标注、泛型非 i32 必须加。

`T&` 在算术/比较两侧自动解引用；`Rc<T>` receiver 自动解引用。`let` / 实参 / `ret` 不解引用。

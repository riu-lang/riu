# yux 语法速查（简版）

写 `*.yux` 时**必看**。这里只列容易踩坑、与 C++/Rust/Go 不同的点；语义细节查 `docs/`（每条末尾给了入口）。

不要拿 Rust / C++ / Go 的直觉去套，遇到没列到的形态先翻 docs 再写。

## 1. 注释 / 空格

- `;` 行首或"空格 + `;`"为注释，**没有 `//` 或 `#`**。
- 关键字后、二元运算符两边、`,` 后必须有空格；`()` `[]` 内部无空格。空格是语法的一部分。
- 详见 [docs/基础语法.md](../../docs/基础语法.md)。

## 2. 变量

- **`let` 仅用于局部变量**；结构体字段段直接 `name Type`，**不带 `let`**。
- 默认浅不可变（旧 `val`）；要可变在前面顶行加 `#Mut`：

  ```yux
  let a i32 = 10
  #Mut let b i32 = 100
  ```

- 档位互斥：`#Mut` / `#Cval` / `#Frozen`。
- 详见 [docs/基础语法.md](../../docs/基础语法.md)。

## 3. 注解形态

- **inline `#Anno`**：仅用于局部 `let` 或单行形参。
- **顶行换行堆叠**：字段 / fn / struct / extend 上的注解，每条单独一行。

  ```yux
  #Mut
  #Inline
  fn step() { ... }
  ```

- 详见 [docs/构建注解.md](../../docs/构建注解.md)。

## 4. 当前实例用 `$`，**不是** `self`

```yux
extend Point {
  fn move(dx i32) {
    $.x = $.x + dx
    $.notify()
  }
}
```

裸 `$` 也合法（指当前实例）。

## 5. 显式构造堆句柄

堆句柄类型**一律显式 turbofish**，没有"非平凡 T 自动 Rc 包"：

```yux
let p = Heap:<Point>(1, 2)
let r = Rc:<String>("hi")
```

详见 [docs/内置类型.md](../../docs/内置类型.md)、[docs/类型系统.md](../docs/类型系统.md)。

## 6. 数组类型 `[T * N]`

- `N` **必须 INT 字面量**，不能是 `#Cval`、表达式或泛型参数。
- 占位 / 待定写 `[T * 0]` 并加注释。
- 详见 [docs/类型系统.md](../../docs/类型系统.md)、[docs/内置类型.md](../docs/内置类型.md)。

## 7. 移入赋值 `<-`

`a <- b`：移出 `a` 的旧值（返回旧值），写入新值 `b`。是**表达式**、最低优先级、右结合。区别于语句级 `a = b`（不产生值）。

```yux
let old = a <- b        ; old = 旧值，a 被重写为 b
let prev = $.buf <- Rc:<Buffer>(64)   ; 字段级移入
```

## 8. 块体强制换行

- 块 `{ ... }` 多行**强制**换行，不能压成一行。
- 仅 `= expr` 单表达式体可写单行：

  ```yux
  fn add(a i32, b i32) i32 = a + b

  fn run() {
    println("hi")
  }
  ```

- 详见 [docs/基础语法.md](../../docs/基础语法.md)、[docs/函数.md](../docs/函数.md)。

## 9. 不要碰语法文件

写 yux 时如果觉得语法不顺，**先暂停**告诉用户。`src/yux*.g4` 只读，详见 [behavior.md](../.claude/rules/behavior.md)。

## 想看完整语法

- [docs/index.md](../docs/index.md) — 教程入口（中文）
- [docs/spec/index.md](../docs/spec/index.md) — 规范（草案）
- [src/yuxParser.g4](../src/yuxParser.g4) / [src/yuxLexer.g4](../src/yuxLexer.g4) — 权威语法

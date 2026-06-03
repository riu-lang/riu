# Change Log

All notable changes to the "yux-vscode" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

## [0.9.0]

- 移除 `draft`（struct 关键字组）、`Ref`（泛型类型组）

## [0.8.0]

- 同步 let-unify：`var` / `val` / `cval` 三关键字已并入 `let` + `#Mut` / `#Cval` / `#Frozen` 注解
  - TextMate：`let` 加入 `keyword.declaration`；删除 `var|val|cval` 的 `storage.modifier` 规则
  - LSP 补全：去 `var` / `val` / `cval` 关键字与 snippet；新增 `let` 关键字 + `let` / `let-mut` / `let-cval` / `let-global` 四个 snippet
  - README 关键字 / snippet 表同步

## [0.7.0]

- 同步 v0.9 语法 + 修复参数 / 返回类型颜色丢失
  - 关键字补 `try` / `catch`
  - `E::V` 枚举构造 / 模式：左侧识别为枚举名（`entity.name.type.enum`），右侧识别为
    枚举成员（`variable.other.enummember`）；新增 `::` 路径访问符
  - 错误传播 `e()!` 单独命名为 `keyword.operator.error-propagate.yux`，便于配色
- LSP 侧：参数 / 返回 / 字段 / 局部变量声明里的类型（含 `i32` / `Rc<T>` / `T?` /
  `[T*N]` / `(T1, T2)` / `fn(...) R` 各形态）现在能正确染色，不再降级为 variable
- LSP 侧新增 `enum` / `enumMember` 两个 semantic token type，覆盖 enum 声明、
  variant、`E::V` 引用与 match 模式
- LSP：类型别名 `Pair<T> = (T, T)` 左侧名字现在按用户类型染色，不再降级为 variable
- 编译器附带：formatter `isKeyword` 补齐 `try` / `catch` / `match` / `draft` /
  `enum` / `cval`，修复这些关键字与后续 token 之间漏空格的问题
- LSP 补全：补齐 `try` / `catch` 关键字；新增 `try` / `match` / `enum` snippet
- 编译器附带：formatter 修复
    - 泛型 `Pair<T>` / `Rc<Map<i32, String>>` 不再被改成 `Pair < T >` 这种带空格
      形态；同时 `a < b` 比较仍正确加空格（按源码里 `<` 是否紧贴前一 token 区分）
    - `=` / `,` / 关键字 后跟 `(` 现在会补空格：`var a = (1, 2)` 不再被压成 `=(1, 2)`
    - `yux format <file>` CLI 在向上找 `yux.toml` 时遇到根目录会无限循环，已修

## [0.6.1]

- 添加`match` `enum` 关键字

## [0.6.0]

- 元组（tuple）与类型别名（type alias）相关高亮
  - 元组类型 / 构造表达式 `(T1, T2, ...)` / `(e1, e2, ...)`：复用现有标点与类型规则
  - 元组成员访问 `a.0` / `a.0.0`：`.N` 整体识别为 `meta.tuple.member.yux`，N 落到 `variable.other.tuple-member.yux`，避免被识别为 float 或纯数字常量
  - `var (a, b) = e` 解构、`obj.0 = e` 成员赋值：复用 `var` 关键字与上述元组成员规则
  - 顶层类型别名 `A = T` / `Pair<T> = (T, T)`：通过既有的大写标识符 / 泛型规则正确高亮

## [0.5.1]

- `draft` 关键字

## [0.5.0]

- 独立的`lsp`

## [0.4.3]

- `slef` -> `$`

## [0.4.2]

- 跟随语言：显式泛型调用改为 turbofish 形式 `name:<T>(args)`，更新对应的 TextMate 模式

## [0.4.1]

- 新增构建注解高亮

## [0.4.0]

- 新增 `Nullable<T>` / `T?` 可空类型相关高亮：`Nullable` 识别为内置泛型类型；`??`（默认值）和 `?.`（安全字段访问）作为独立运算符高亮

## [0.3.0]

- 切换为纯 LSP 客户端：删除内置 antlr-ng / antlr4ng / 自带 parser / scope / 跨文件索引
- 语言服务全部由编译器 `yux lsp` 子命令提供（stdio JSON-RPC）
- 新增配置项 `yux.executablePath` 指定 yux 可执行文件路径，留空走 PATH
- 当前由服务器提供能力：诊断、文档符号、格式化、静态补全（关键字 / 类型 / 内置函数 / snippet）
- 跳转 / hover / signatureHelp / 动态补全将在后续版本由服务器补齐

## [0.2.0]

- 用基于 `src/yux.g4` 生成的 antlr-ng parser（通过 `-D language=TypeScript` 覆盖 .g4 的 Cpp 目标）替换原正则方案
- 作用域感知：函数参数 / 函数体 / 嵌套 block 三级 scope，定义跳转与变量补全按可见性过滤
- 一层泛型壳剥离（`Ref<T>` / `Rc<T>` / `Ptr<T>` / `Array<T>`），多级点链 `a.b.c.` 按 struct 字段/方法下钻
- `self` 在 struct impl 方法内解析为当前 struct
- 跨文件跳转与补全：按 `yux.toml` 定位项目根，展开 `use a.b` / `use a.b.*`；sdk 从 PATH 上 `yux(.exe)` 所在目录的 `../sdk` 回退
- hover：显示变量 / 函数 / struct / 方法签名
- signatureHelp：函数调用 `f(|)` 显示参数列表并跟随 `,` 高亮当前参数
- 文件监听：`.yux` 保存 / 变更自动失效跨文件解析缓存

## [Unreleased]

- Initial release
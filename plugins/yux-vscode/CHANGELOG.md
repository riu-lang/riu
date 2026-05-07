# Change Log

All notable changes to the "yux-vscode" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

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
- 一层泛型壳剥离（`Ref<T>` / `Box<T>` / `Ptr<T>` / `Array<T>`），多级点链 `a.b.c.` 按 struct 字段/方法下钻
- `self` 在 struct impl 方法内解析为当前 struct
- 跨文件跳转与补全：按 `yux.toml` 定位项目根，展开 `use a.b` / `use a.b.*`；sdk 从 PATH 上 `yux(.exe)` 所在目录的 `../sdk` 回退
- hover：显示变量 / 函数 / struct / 方法签名
- signatureHelp：函数调用 `f(|)` 显示参数列表并跟随 `,` 高亮当前参数
- 文件监听：`.yux` 保存 / 变更自动失效跨文件解析缓存

## [Unreleased]

- Initial release
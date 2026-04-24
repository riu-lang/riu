# Change Log

All notable changes to the "yux-vscode" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

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
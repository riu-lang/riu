# 目录结构

```
yux-lang/
├── yux/              所有构建子系统（各自含 xmake.lua）
│   ├── yux/          主编译器 + LLVM codegen（yux.exe）
│   │   ├── compiler/  LLVM IR 生成
│   │   ├── cli/      CLI 子命令（build/test/format）
│   │   └── main.cpp  CLI 入口
│   ├── rt/           运行时 C99 静态库（yuxrt.lib）
│   ├── ast/          ANTLR4 + AST + parse tree 转储
│   │   ├── gen/yux/  ANTLR4 生成代码，不要手改
│   │   ├── node/     AST 节点定义
│   │   └── yux*.g4   语法文件（不要手改）
│   ├── analyzer/     语义分析器
│   ├── frontend/     前端静态库（sema + tools + formatter，0 LLVM）
│   ├── check/        yux-check（快速 sema，0 LLVM）
│   ├── lsp/          LSP 服务器
│   └── test-runner/  测试运行器（yux test 内部 spawn）
├── sdk/yux/          自举运行时，静态库链接到每个 yux 程序
│   └── src/yux/core/  SDK 源码 + *.test.yux 测试
├── docs/             语言参考文档（中文）
│   ├── spec/         语言规范（草案）
│   └── dev/          版本实施日志归档
├── tests/
│   ├── projects/     项目编译+运行+格式化用例（每目录一个 yux.toml）
│   ├── check-cases/  诊断用例（diag_*.yux，yux-check test）
│   └── xmake.lua     测试运行器
├── plugins/          编辑器插件（VSCode / IntelliJ / Claude Code）
├── .claude/
│   └── rules/        硬性规则（每会话自动加载）
├── rules/            按需读的手册与规则
│   └── manuals/      工程手册（各 exe 用法）
├── third_party/      依赖（由 sync-deps 拉取）
├── scripts/          构建/同步辅助脚本
└── CLAUDE.md / README.md
```

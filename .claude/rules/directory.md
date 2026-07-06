# 目录结构

```
yux-lang/
├── yux/              所有构建子系统（各自含 xmake.lua）
│   ├── yux/          主编译器入口 + LLVM codegen + zlib + llvm（yux.exe，直接编入无中间 .lib）
│   │   ├── compiler/  LLVM IR 生成
│   │   ├── cli/      CLI 子命令（build/test/format）
│   │   └── main.cpp  CLI 入口
│   ├── rt/           运行时 C99 静态库（yuxrt.lib，mem + math + port）
│   ├── ast/          ANTLR4 运行时 + AST 构建器 + parse tree 转储（antlr4_static.lib + yux_ast.lib + yux-ast）
│   │   ├── gen/yux/  ANTLR4 生成代码，不要手改
│   │   ├── node/     AST 节点定义
│   │   ├── yux*.g4   语法文件（不要手改）
│   │   └── xmake.lua
│   ├── analyzer/     语义分析器（yux_analyzer.lib）
│   ├── frontend/     前端静态库（yux_frontend.lib；sema + tools + formatter + SDK loader，0 LLVM）
│   ├── check/        快速语义检查工具（yux-check 可执行文件，0 LLVM）
│   ├── lsp/          LSP 服务器（yux-lsp 可执行文件，叶子节点）
│   └── test-runner/  测试运行器（yux-test-runner 可执行文件，0 依赖，叶子节点）
├── sdk/yux/          自举运行时（独立 yux 项目，编为静态库 yux.lib），链接到每个 yux 程序
│   └── src/yux/core/  SDK 源码 + *.test.yux 测试（#Test 注解，yux test 运行）
├── docs/             语言参考文档（中文）；入口 docs/index.md
│   ├── spec/         语言规范（草案中）
│   │   └── draft/    跨章节设计草案（`DRAFT-<特性>.md` + `_模板.md`），入库但不等同规范
│   └── dev/          已完成版本的实施日志归档（如 ownership-impl-log.md），内容为重大 / 重要变更实现，其它在 git 提交记录
├── examples/test/    示例项目，用作快速冒烟测试
├── tests/
│   ├── cases/        单文件用例（format_* / extern_* / ptr_*，xmake test）
│   ├── check-cases/  诊断用例（diag_*.yux，; check: EXXXX 注解，由 yux-check test 运行）
│   ├── projects/     项目模式用例（每目录一个 yux.toml + expected.txt）
│   └── xmake.lua     测试运行器（yux_tests target）
├── third_party/      依赖：antlr4, cli11, llvm, toml11, utfcpp, zlib（由 sync-deps 拉取）
├── bin/              二进制工具：antlr-4.13.2-complete.jar（由 sync-deps 拉取）
├── build/            xmake 产物目录（布局详见 yux-lang-dev 技能）
├── plugins/          编辑器 / 客户端插件（同时是 Claude Code marketplace 根，含 .claude-plugin/marketplace.json）
│   ├── yux-vscode/       VSCode 语法高亮插件（LSP 客户端走 `yux-lsp`）
│   ├── yux-idea/         IntelliJ 插件（通过 LSP4IJ 接入 `yux-lsp`，含语法高亮 / 配色 / 代码风格）
│   └── yux-claude-code/  Claude Code LSP 插件（marketplace 名 `yux-lang-lsp@yux-lang`，对接独立可执行 `yux-lsp-claude`）
├── scripts/          构建/同步辅助脚本
│   └── sync-deps.js     sync-deps 核心逻辑（读取 DEPS.json）
├── .claude/
│   ├── rules/        自动加载的项目规则（behavior、tasks-and-bugs、directory）
│   └── skills/       项目技能（仅 yux-lang-dev）
├── rules/            按需手动读的规则（不自动加载）：yux-syntax、sema-codegen、spec-writeback
├── xmake.lua         顶层构建脚本
├── yux.toml          仓库自身的 dogfood 项目配置
├── DEPS.json         第三方依赖声明
├── CURRENT.md        当前多步任务追踪，本地（不入 git）
├── CURRENT-*.md      其它任务，本地（不入 git）
├── BUGS.md           新发现的 bug 清单，本地（不入 git）
├── MILESTONE.md      里程碑，当前稳定版目标和已经实现的目标
└── CLAUDE.md / README.md
```

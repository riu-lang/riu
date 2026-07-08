# yux-lang 规则模块

本目录是 yux-lang 项目的**硬性规则**，每会话自动加载。所有工程环境、测试流程、手册入口均在此，无需手动调用 skill。

## 自动加载（每次会话）

| 文件 | 内容 |
|------|------|
| [behavior.md](behavior.md) | **决策框架**：核心循环、异常分支、强制触发、原则 |
| [directory.md](directory.md) | 仓库目录结构 |
| [engineering.md](engineering.md) | **工程环境与测试**：环境、构建、中途测试/结束回归流程、手册入口、C++ 规范 |

## 手册与专题规则（`rules/` 目录，按需读）

| 文件 | 内容 | 触发场景 |
|------|------|----------|
| `rules/manuals/manual-yux.md` | 构建/测试/format/IR 调试/lint | 最高频 |
| `rules/manuals/manual-yux-check.md` | 快速诊断 | 需要快速 sema |
| `rules/manuals/manual-yux-lsp.md` | LSP 服务 | 编辑器集成问题 |
| `rules/manuals/manual-yux-test-runner.md` | 测试运行器 | 测试崩溃排查 |
| `rules/manuals/manual-yux-ast.md` | AST 转储 | 语法有异议时（最低频） |
| `rules/yux-syntax.md` | yux 写前清单 | **编辑 .yux 前必须逐条过** |
| `rules/sema-codegen.md` | Sema/Codegen 协议 | 改 sema 或 compiler 目录 |
| `rules/spec-writeback.md` | 语言面变更回写 | 改语言特性/语法/ABI |

## 维护

- 规则文件**入 git**，与 spec 同等约束力；改前先与用户对齐。
- 行为类规则放 [behavior.md](behavior.md)；目录结构放 [directory.md](directory.md)。
- 手册内容保持实用、简短——讲"怎么用"而不是"是什么"。

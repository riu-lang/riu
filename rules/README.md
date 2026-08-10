# yux-lang 规则模块

本目录是 yux-lang 项目的**硬性规则**，入口为根目录 `RULES.md`。所有工程环境、测试流程、手册入口均在此，无需手动调用 skill。

## 自动加载（每次会话）

`RULES.md` 是基本规则文件，引用本目录的以下文件：

| 文件 | 内容 |
|------|------|
| [behavior.md](behavior.md) | **决策框架**：核心循环、异常分支、强制触发、原则 |
| [directory.md](directory.md) | 仓库目录结构 |
| [engineering.md](engineering.md) | **工程环境与测试**：环境、构建、中途测试/结束回归流程、手册入口、C++ 规范 |

## 手册与专题规则（按需读）

| 文件 | 内容 | 触发场景 |
|------|------|----------|
| `manuals/manual-yux.md` | 构建/测试/format/IR 调试/lint | 最高频 |
| `manuals/manual-yux-check.md` | 快速诊断 | 需要快速 sema |
| `manuals/manual-yux-lsp.md` | LSP 服务 | 编辑器集成问题 |
| `manuals/manual-yux-test-runner.md` | 测试运行器 | 测试崩溃排查 |
| `manuals/manual-yux-ast.md` | AST 转储 | 语法有异议时（最低频） |
| `yux-syntax.md` | yux 写前清单 | **编辑 .yux 前必须逐条过** |
| `sema-codegen.md` | Sema/Codegen 协议 | 改 sema 或 compiler 目录 |
| `spec-writeback.md` | 语言面变更回写 | 改语言特性/语法/ABI |

## 维护

- 规则文件**入 git**，与 spec 同等约束力；改前先与用户对齐。
- 行为类规则放 [behavior.md](behavior.md)；目录结构放 [directory.md](directory.md)。
- 手册内容保持实用、简短——讲"怎么用"而不是"是什么"。

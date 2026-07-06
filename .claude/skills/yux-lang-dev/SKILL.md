---
name: yux-lang-dev
description: 每会话开始时（需要看/改代码）调用——加载工程环境、构建/测试命令入口、指向手册与硬性规则。
---

# yux-lang-dev

yux 是自举编译器：`.yux → ANTLR4 → AST → LLVM IR → LLD → exe`。

## 环境

- Windows + Clang（无 MSVC），LLVM 在 PATH
- `build/windows/x64/debug/bin` 在 PATH，构建后直接 `yux ...`

## 必读规则

开始任何任务前，先读这两个（auto-load）：
- [.claude/rules/behavior.md](../../rules/behavior.md) — **决策框架**（核心循环 + 异常分支 + 强制触发）
- [.claude/rules/directory.md](../../rules/directory.md) — 目录结构

## 工程手册

按需读（强制触发条件见 behavior.md）：

| 手册 | 场景 |
|------|------|
| `rules/manuals/manual-yux.md` | 构建、测试、format、IR 调试、lint/format 包装器（最高频） |
| `rules/manuals/manual-yux-check.md` | 快速诊断、批量诊断测试 |
| `rules/manuals/manual-yux-lsp.md` | LSP 服务行为 |
| `rules/manuals/manual-yux-test-runner.md` | 测试运行器内部行为（崩溃排查） |
| `rules/manuals/manual-yux-ast.md` | AST 转储（语法有异议时，最低频） |

## 专题规则（按需）

| 文件 | 场景 |
|------|------|
| `rules/yux-syntax.md` | 编辑 `*.yux` 前必读（写前往下逐条勾） |
| `rules/sema-codegen.md` | 改 sema 或 compiler 目录前必读 |
| `rules/spec-writeback.md` | 改语言特性/语法/ABI 前必读 |

## 写 C++ 快记

- 注释用中文；`// ====` 分隔区域
- 未完成 / 潜在 bug / 待验证 → 写 `// TODO:`
- 提交前 `./lint.ps1` 必须 0 warnings
- hook 自动调 `clang-format -i`，无需手动 format

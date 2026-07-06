# yux-lsp 手册

LSP 服务器，0 LLVM 依赖。编辑器插件通过 stdio 接入。

## 启动方式

- **不直接调**。由编辑器插件自动启动：VSCode（`yux-vscode`）、IntelliJ（`yux-idea` 通过 LSP4IJ）、Claude Code（`yux-claude-code`）
- Claude Code LSP 插件对接独立可执行 `yux-lsp-claude`（构建后从 `yux-lsp` 自动复制）

## 构建

```powershell
xmake build yux-lsp
```

构建后 `build/windows/x64/debug/bin/yux-lsp.exe`，同时自动复制为 `yux-lsp-claude.exe`。

## 诊断

LSP 启动失败时检查：
1. 确认 `yux-lsp.exe` 在 PATH 中
2. 编辑器的 LSP 配置中可执行路径正确
3. 日志见编辑器 LSP 输出面板

语法高亮依赖 LSP 的 semantic tokens；补全/跳转/悬停走 project 模式（需要有 `yux.toml`）。

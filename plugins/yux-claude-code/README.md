; yux-claude-code

Claude Code 插件，让 Claude 编辑 `.yux` 文件时通过 LSP 实时看到诊断。

走的二进制是 `yux-lsp-claude`（不是 `yux-vscode` 用的 `yux-lsp`）。两个客户端一个 server 容易出怪问题（`clientInfo`、未实现 capability 的回退差异等），所以这边专门走一份**用户自行手动改名/单独构建**的副本。

## 前置

- 已构建 yux 工具链：`xmake build yux`
- 把 `yux-lsp` 二进制复制/重命名为 `yux-lsp-claude`（Windows：`yux-lsp-claude.exe`），放进 `PATH`
  - 例：`copy build\windows\x64\debug\yux-lsp.exe build\windows\x64\debug\yux-lsp-claude.exe`
- 本仓库默认产物目录 `build/windows/x64/debug` / `build/windows/x64/release` 已加进 `PATH` 时无需额外操作

## 安装

本仓库 `plugins/.claude-plugin/marketplace.json` 把本插件登记为 marketplace 条目，名字 `yux-lang-lsp@yux-lang`。

### 本地装载（直接指向工作树）

```
/plugin marketplace add E:\yux-lang\plugins
/plugin install yux-lang-lsp@yux-lang
/reload-plugins
```

### 通过 git 远端

仓库推到远端后：

```
/plugin marketplace add yux-lang/yux
/plugin install yux-lang-lsp@yux-lang
```

> Claude Code 会把插件目录拷到 `~/.claude/plugins/cache`，再从缓存里加载 `.lsp.json`。所以 `yux-lsp-claude` 必须在 `PATH`，不能用相对路径指。

## 工作机制

`.lsp.json` 告诉 Claude Code 用 `yux-lsp-claude` 处理 `.yux` 文件。Claude Code 在每次 Edit/Write 后通过 `textDocument/didChange` 把内容推给 LSP server，server 经由 `textDocument/publishDiagnostics` 回推诊断，Claude 在编辑返回里以 `<new-diagnostics>` 系统提示拿到错误，编译前就能感知。

## 注意事项

- Claude Code 在 LSP `initialize` 请求里带 `clientInfo`。如果 `yux-lsp-claude` 对未知 client 有特别处理，需要确认它能容忍非 vscode 客户端
- 这里只接 diagnostics；hover / completion / goto-def 对终端 LLM 收益小，目前不显式禁用，由 server 侧决定是否提供
- `yux-lsp-claude` 不在 `PATH` 时，Claude Code `/plugin` 的 Errors tab 会报 "Executable not found in $PATH"

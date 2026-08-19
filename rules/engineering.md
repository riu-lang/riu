# 工程环境与测试

yux 是自举编译器：`.yux → ANTLR4 → AST → LLVM IR → LLD → exe`。

## 环境

- Windows + Clang（无 MSVC 作为编译器；仍需 VS 的 Windows SDK / STL）
- `build/windows/x64/debug/bin` 在 PATH，构建后直接 `yux ...`
- 每个 exe 独立，改了代码要重编对应的：

| 修改的代码 | 需要重编 |
|------------|----------|
| `yux/yux/compiler/` | `./build.ps1 yux` |
| `yux/frontend/`（sema/AST） | `./build.ps1 yux yux-check` |
| `yux/ast/`（ANTLR4 运行时/AST 节点） | `./build.ps1 yux yux-check yux-ast` |
| `yux/lsp/` | `./build.ps1 yux-lsp` |
| `yux/test-runner/` | `./build.ps1 yux-test-runner` |
| 改 g4 | `./gen-antlr.ps1` → 上面全部 |

> 改了 `yux/frontend/` 只 `./build.ps1 yux`，然后用 `yux-check` 验证 → `yux-check` 没重编，跑的是旧代码。

## 测试

### 中途测试（改完代码立即验证）

```
yux test
```

在 `sdk/yux/` 下运行，跑 SDK 测试集（`sdk/yux/src/yux/core/*.test.yux`）。轻量、快速，覆盖纯逻辑 + 行为用例。

常用变体：
```
yux test --verbose                  ; 打印每个测试的 stdout/stderr
yux test --test-mod yux.core.array  ; 只跑指定模块
yux test --threads 4                ; 指定并行数
```

> 目前覆盖面还不全，但会逐步扩充。改完代码就跑 `yux test`，不跳过。

### 结束回归测试（任务收尾）

任务完结前跑完整回归：

```powershell
./build.ps1 yux-check               ; 确保 yux-check 是最新的
yux-check test tests/check-cases/   ; 诊断回归用例
./build.ps1 test                    ; 项目编译+运行 + 格式化回归（全量）
```

三项都过 → 任务验证完毕，可以提交。

### 调试测试崩溃

1. `yux test --verbose` 看哪个 DLL 没出摘要行
2. `yux build --test -d` 拿到完整 IR 调试输出
3. 单独跑：`yux test --test-mod <模块名> --verbose`

## 工程手册（按需读）

强制触发条件见 behavior.md。

| 手册 | 场景 |
|------|------|
| `rules/manuals/manual-yux.md` | 构建、测试、format、IR 调试、lint/format 包装器（最高频） |
| `rules/manuals/manual-yux-check.md` | 快速诊断、批量诊断测试 |
| `rules/manuals/manual-yux-lsp.md` | LSP 服务行为 |
| `rules/manuals/manual-yux-test-runner.md` | 测试运行器内部行为（崩溃排查） |
| `rules/manuals/manual-yux-ast.md` | AST 转储（语法有异议时，最低频） |

## 专题规则（按需读）

| 文件 | 场景 |
|------|------|
| `rules/yux-syntax.md` | 编辑 `*.yux` 前必读（写前往下逐条勾） |
| `rules/sema-codegen.md` | 改 sema 或 compiler 目录前必读 |
| `rules/spec-writeback.md` | 改语言特性/语法/ABI 前必读 |

## 写 C++

- 注释用中文；`// ====` 分隔区域
- 未完成 / 潜在 bug / 待验证 → 必须写 `// TODO:`，不假装没看见
- 提交前 `./lint.ps1`，必须 **0 warnings**
- 改完 C++ 后立即跑 `./format.ps1`，确保代码风格一致

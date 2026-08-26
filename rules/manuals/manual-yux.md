# yux 工程手册（最高频）

主编译器 `yux`。依赖 LLVM。用户直接调用，三个子命令：`build` / `test` / `format`。

## 首次环境

```powershell
./sync-deps.ps1              ; 拉取第三方依赖
./gen-antlr.ps1              ; 改 g4 后重新生成 parser
./build.ps1 yux              ; 编译编译器
```

日常重编译：`./build.ps1 yux`（增量，2min+ 耗时主要在链接阶段）。

**重编目标对照**（每个 exe 独立，改了代码要重编对应的）：

| 修改的代码 | 需要重编 |
|------------|----------|
| `yux/yux/compiler/` | `./build.ps1 yux` |
| `yux/frontend/`（sema/AST） | `./build.ps1 yux yux-check` |
| `yux/ast/`（ANTLR4 运行时/AST 节点） | `./build.ps1 yux yux-check yux-ast` |
| `yux/lsp/` | `./build.ps1 yux-lsp` |
| `yux/test-runner/` | `./build.ps1 yux-test-runner` |
| 改 g4 | `./gen-antlr.ps1` → 上面全部 |

> 改了 `yux/frontend/` 只 `./build.ps1 yux`，然后用 `yux-check` 验证 → `yux-check` 没重编，跑的是旧代码。

`build/windows/x64/debug/bin` 默认已配置到 PATH，构建后直接 `yux ...`。

## 全局选项（所有子命令前）

```
-d, --debug             输出编译 IR 调试信息
    --emit-ir           输出 .ll 文件
    --emit-ir-dir TEXT  指定 .ll 输出目录（默认 build/）
    --warn TEXT         将 EXXXX 视为警告（可重复）
    --allow TEXT        将 EXXXX 视为提示（可重复）
    --deny TEXT         将 EXXXX 视为错误（可重复）
    --Werror            所有警告视为错误
```

## yux build

```
yux build [OPTIONS] [name]
```

必须在含 `yux.toml` 的项目根目录运行。

| 选项 | 说明 |
|------|------|
| `name` | 项目名（可选，需匹配 yux.toml 里的 `name`） |
| `--emit-ir` | 输出 .ll 到 build/ |
| `--emit-ir-dir TEXT` | 指定 .ll 目录 |
| `--test` | 编译测试 DLL（`*.test.yux` → `build/tests/`） |
| `--test-mod TEXT` | 只编译指定测试模块（如 `yux.core.array`） |
| `-d` | 输出编译 IR 调试信息 |

**构建缓存**：`PkgCacheRegistry` 基于编译器指纹 + 源文件 mtime/size 判断 obj 新鲜度。`yux build` 与 `yux test` 共用缓存，编译器重编后指纹变化自动全体作废，无需手动清。

## yux test

```
yux test [OPTIONS]
```

流程：`yux build --test`（复用缓存）→ 并行 spawn `yux-test-runner` 子进程 → 每子进程加载一个 DLL，顺序跑其中 `#Test fn`，SEH 包裹异常。

| 选项 | 说明 |
|------|------|
| `-v, --verbose` | 打印每个测试捕获的 stdout/stderr（默认仅失败时） |
| `--threads INT` | 并行子进程数（默认 CPU 核数） |
| `--test-mod TEXT` | 只跑指定模块 |
| `-d` | 调试输出 |

测试写在 `*.test.yux`（**不能**挂在普通 `.yux`），断言用 `assert_eq` / `assert_true` / `fail`。SDK 测试集在 `sdk/yux/src/yux/core/*.test.yux`。

**摘要行**：每个 DLL 输出末尾必有一行 `<dll>: X passed, Y failed, [T]`。如果某个 DLL 末尾**没有**摘要行，说明进程意外退出（崩溃/被终止/死循环）。

常用：
```powershell
yux test                          ; 当前项目所有 #Test
yux test --verbose                ; 打印每个测试的 stdout/stderr
yux test --test-mod yux.core.array  ; 只跑指定模块
yux test --threads 4              ; 指定并行数
yux test -d                       ; 调试输出传给 yux build --test
cd sdk/yux && yux test            ; 主测试集
```

### 调试测试崩溃

1. `yux test --verbose` 看哪个 DLL 没出摘要行
2. `yux build --test -d` 拿到完整 IR 调试输出
3. 单独跑对应测试模块：`yux test --test-mod <模块名> --verbose`

## yux format

```
yux format [OPTIONS] [file]
```

| 选项 | 说明 |
|------|------|
| `-i, --in-place` | 原地编辑文件 |
| `--stdin` | 从 stdin 读（非文件） |
| `--line-width INT` | 行宽阈值（默认 120） |

## IR 调试

```powershell
yux build --emit-ir               ; 输出 .ll 到 build/
yux build --emit-ir --emit-ir-dir ir_out  ; 指定输出目录
yux build -d                      ; 编译全流程调试输出
```

常见调试链路：`-d` 看编译器内部状态 → `--emit-ir` 看生成的 LLVM IR → 对照 `yux/compiler/` 源码定位。

## BUG 排查

### 编译期崩溃（LLVM 断言 / 段错误）

`-d` 输出编译全流程的 IR 调试信息，**输出量很大**，需要过滤定位崩溃行：

```powershell
yux build -d 2>&1 | tail -100          ; 崩溃时最后 100 行（最常见）
yux build -d 2>&1 | grep "Emit\|error\|assert"  ; 过滤关键信号
yux build -d 2>&1 | grep "\.cpp:"      ; 只看出自哪个 C++ 文件的输出
```

步骤：
1. `yux build -d 2>&1 | tail -100` → 看崩溃前最后一条 IR dump，定位到哪个 pass/哪段代码
2. 如果输出太长，用 `grep` 收缩到 `.cpp` 文件名 → 确定崩溃所在的 compiler 源文件
3. 对照 `yux/yux/compiler/<源文件>` 定位逻辑

### 运行时错误（编译通过但运行崩溃 / 结果不对）

用 `--emit-ir` 输出 LLVM IR，**对照 .ll 和 yux 源码**检查 codegen 是否正确：

```powershell
yux build --emit-ir --emit-ir-dir ir_out
# 打开 ir_out/<name>.ll，搜索对应函数名
```

LLVM IR 里能看到每个函数的入口标签、alloca/load/store、call 等，直接对比 yux 源码逻辑判断 codegen 是否偏差。

### 测试崩溃

见上方「调试测试崩溃」段。关键信号：DLL 末尾无摘要行 = 进程意外退出。

## 项目/格式化回归

```powershell
./build.ps1 test                      ; 全部 tests/projects（默认并行，jobs = CPU 核数）
./build.ps1 test -Jobs 8              ; 指定并行用例数
./build.ps1 test -Jobs 1              ; 强制串行
./build.ps1 test -Group project       ; 项目编译+运行（含 expected.txt）
./build.ps1 test -Group format        ; 格式化回归（含 expected_format）
./build.ps1 test <name>               ; 跑单个用例（目录名）
```

## lint / format 包装器

```powershell
./lint.ps1              ; clang-tidy，仅 git 变动/未跟踪文件
./lint.ps1 --all        ; 全量
./lint.ps1 yux/x.cpp    ; 指定文件
./format.ps1            ; clang-format -i，仅 git 变动/未跟踪文件
./format.ps1 --all      ; 全量
./format.ps1 --check    ; dry-run + Werror（pre-commit）
```

提交门槛：`./lint.ps1` 必须 **0 warnings**。

> 已知未处理：未使用 include 清理（clangd `unused-includes` / IWYU），后续专项排。

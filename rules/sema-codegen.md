# Sema / Codegen 协议

项目正在长期推进 Sema/Codegen 两段分离（最终目标：`yux-check` 独立 exe，0 LLVM 依赖，与 `yux build` 错误覆盖等价）。当前是**半完成态**，写新 C++ 代码必须注意：

## `yux/frontend/sema/` = 0 LLVM 依赖

sema 相关代码进 `yux/frontend/sema/sema_pass.{h,cpp}` 与 `yux/frontend/sema/call_resolve.{h,cpp}`，由静态库 `yux_frontend` 提供。

不要在这两个文件里 `#include "llvm/..."` 或调 `IRBuilder` / `_module` 等 codegen 状态。

## 新加 `throw YuxError` 时

- 默认**不强求** sema 镜像；写在 `yux/yux/compiler/compiler_*.cpp` 里照常即可。
- 若顺手让 sema 接管（推荐对纯静态形态检查这么做），**必须同步更新 `yux/frontend/sema/sema_pass.cpp` 顶部的 `kMigratedCodes` 白名单**。
  - 漏更新 = sema 抛了又被自身的 try/catch 吞掉，**无测试能捕获**。
- sema 接管后 Compiler 端的原 inline throw / validate 调用**直接删除**（sema 已覆盖，codegen 正常路径不可达）；不再保留防御性双跑。

## 新加 AST 节点 / 表达式类

必须在 `SemaPass::visitExpr` (`yux/frontend/sema/sema_pass.cpp`) 加一条 dynamic_cast 分支，**哪怕只是 `return;` 占位**，否则 sema 会静默 skip 整个子树。

## sema 当前缺口（出错时不报，靠 codegen 兜底）

- 泛型 fn / impl 体
- 所有 statement（lambda body 内 statement 已解锁 v0.16）
- target-type 上下文驱动的类型检查
- `compiler_types.cpp` 的 alias 环检测

在这些路径里加 throw 时 sema 镜像不会生效，省事写 Compiler 端即可。

## 快速诊断

本地写 demo / 改代码后想快速跑诊断，用 `yux-check <file.yux>`（0 LLVM，编译几秒）；要完整覆盖才走 `yux build`。

**`yux-check` 报错是 `yux build` 报错的子集，不报错不代表无错**（缺口见上）。

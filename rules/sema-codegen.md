# Sema / Codegen

目标：`yux-check` 独立、0 LLVM，诊断与 `yux build` 等价。当前半完成。

- `yux/frontend/sema/`（`sema_pass` / `call_resolve`，库 `yux_frontend`）禁止 LLVM、`IRBuilder`、`_module`。
- 新 `throw YuxError` 默认写 Compiler。`getType()` 的 YuxError 由 SemaPass **默认重抛**；必须暂留 Compiler 的码加入 `kDeferredCodes`（现 E3009 / E3095），禁止静默吞。sema 已覆盖的 Compiler throw / validate **删掉**，不双跑。泛型 fn/impl 体 SemaPass 仍跳过，那些 throw 留到 Phase C。
- 新 AST / 表达式：`SemaPass::visitExpr` 必须加分支（可 `return;`），否则整棵子树被 skip。
- 缺口（sema 不报、靠 codegen）：泛型 fn/impl 体；statement（lambda body 内 stmt 已解锁）；target-type 上下文。这些路径的 throw 写 Compiler。

`yux-check <file>` 是 `yux build` 报错的**子集**。

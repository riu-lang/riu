# Sema / Codegen

目标：`yux-check` 独立、0 LLVM，诊断与 `yux build` 等价。当前半完成。

- `yux/frontend/sema/`（`sema_pass` / `call_resolve`，库 `yux_frontend`）禁止 LLVM、`IRBuilder`、`_module`。
- 新 `throw YuxError` 默认写 Compiler。`getType()` 的 YuxError 由 SemaPass **默认重抛**；必须暂留 Compiler 的码加入 `kDeferredCodes`（现 E3009 / E3095），禁止静默吞。sema 已覆盖的 Compiler throw / validate **删掉**，不双跑。
- 泛型 fn/impl 体已走 SemaPass：类型参数当不透明 TypeParam。形态检查（#NoCopy / 未定义符号 / arity）在模板体报；依赖 T 具体化的类型错仍留给实例化期 / Compiler。
- 新 AST / 表达式：`SemaPass::visitExpr` 必须加分支（可 `return;`），否则整棵子树被 skip。
- 缺口（sema 不报、靠 codegen）：statement 层残留 Compiler throw；target-type 上下文（E3009）。这些路径的 throw 写 Compiler。

`yux-check <file>` 是 `yux build` 报错的**子集**。

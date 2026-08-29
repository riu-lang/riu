# Sema / Codegen

目标：`yux-check` 独立、0 LLVM，诊断与 `yux build` 等价。当前半完成。

- `yux/frontend/sema/`（`sema_pass` / `call_resolve`，库 `yux_frontend`）禁止 LLVM、`IRBuilder`、`_module`。
- 新 `throw YuxError` 默认写 Compiler。`getType()` 的 YuxError 由 SemaPass **默认重抛**；必须暂留 Compiler 的码加入 `kDeferredCodes`（当前为空），禁止静默吞。方法点 callee 的 E3095 在 `visitExpr` 按形态吞掉（挡住 E1101/E1140）；ID-literal 的 E3095 由 SemaPass 重抛。sema 已覆盖的 Compiler throw / validate **删掉**，不双跑。
- 泛型 fn/impl 体已走 SemaPass：类型参数当不透明 TypeParam。形态检查（#NoCopy / 未定义符号 / arity）在模板体报；依赖 T 具体化的类型错仍留给实例化期 / Compiler。
- 数组 / 元组字面量：`visitExpr` 带 target-type（声明 / 赋值 / 返回 / 调用实参 / 字段 / 嵌套），E3009 / E3012 由 SemaPass 抛。
- 调用实参：仅唯一 arity 非泛型候选（及 Fn 值 / 非泛型方法 / `#Static fn`）在重载前带形参类型下钻；同 arity 重载不猜。
- lambda：`expected` 为 Fn 时 `setInferredFnType` + bodyScope 形参回填（声明 / 上述调用点）；表达式体带返回类型下钻并做 E3014；块体 `ret` / 末位无 `;` 的尾表达式按 lambda 标注或反推返回类型检查，不用外层 fn。
- 新 AST / 表达式：`SemaPass::visitExpr` 必须加分支（可 `return;`），否则整棵子树被 skip。
- 缺口（sema 不报、靠 codegen）：方法点 E3095（挡住更精确诊断时吞给 Compiler）；泛型实例化期类型错；部分 T& / Fallible；同 arity 重载实参靶向。这些路径的 throw 写 Compiler。

`yux-check <file>` 是 `yux build` 报错的**子集**。

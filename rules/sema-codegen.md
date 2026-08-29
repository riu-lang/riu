# Sema / Codegen

目标：`yux-check` 独立、0 LLVM，诊断与 `yux build` 等价。当前半完成。

- `yux/frontend/sema/`（`sema_pass` / `call_resolve`，库 `yux_frontend`）禁止 LLVM、`IRBuilder`、`_module`。
- 新 `throw YuxError` 默认写 Compiler。`getType()` 的 YuxError 由 SemaPass **默认重抛**；必须暂留 Compiler 的码加入 `kDeferredCodes`（当前为空），禁止静默吞。方法点 callee 的 E3095：`visitExpr` 先按形态记下（挡住 E1101/E1140），Dot 分支校验 `@Spec` 后——字段非 Fn 值直接报，其余延迟重抛。ID-literal 的 E3095 立即重抛。sema 已覆盖的 Compiler throw / validate **删掉**，不双跑。
- 泛型 fn/impl 体已走 SemaPass：类型参数当不透明 TypeParam。形态检查（#NoCopy / 未定义符号 / arity）在模板体报；依赖 T 具体化的类型错仍留给实例化期 / Compiler。
- 数组 / 元组字面量：`visitExpr` 带 target-type（声明 / 赋值 / 返回 / 调用实参 / 字段 / if·match·try 块末尾 / 嵌套），E3009 / E3012 由 SemaPass 抛。
- 调用实参：唯一 arity 非泛型候选（及 Fn 值 / 非泛型方法 / `#Static fn`）在重载前带形参类型下钻；同 arity 重载只对各位都相同的类型下钻，不一致的位置不猜。泛型 fn 显式 typeArgs（及推断完成后）按 `TypeInfo::substitute` 后的形参下钻 / E3014；泛型 struct `#Static` turbofish → E3131；接收者已带 typeArgs 的实例方法同款。
- lambda：`expected` 为 Fn 时 `setInferredFnType` + bodyScope 形参回填（声明 / 上述调用点）；表达式体带返回类型下钻并做 E3014；块体 `ret` / 末位无 `;` 的尾表达式按 lambda 标注或反推返回类型检查，不用外层 fn。
- `ret`：Fallible 成功/错误双通道、T& 内层类型、Nullable wrap、别名 `resolveAlias`、灵活整数推断 → E3014。非法 T& 源形态由 analyzer E4001/E4020 先报。T& 局部 `&expr` 内层 E3014。
- 赋值：RHS 相对存储槽 E3014。T& 局部 / `$`（Self&）store-through 比内层 T；Nullable wrap / Rc wrap / 灵活整数与 codegen 对齐。索引赋值同款。经 T& / `$` 的嵌套数组走靶向类型 E3009。
- match：各臂结果类型一致 → E3014（流终止臂跳过）。scrut 经别名 / Rc<E> / Heap<E> / E& 剥到 enum；临时 Rc/Heap 直接 match → E2022。if / match / try 块末尾值带靶向类型（嵌套数组 E3009）。
- 新 AST / 表达式：`SemaPass::visitExpr` 必须加分支（可 `return;`），否则整棵子树被 skip。
- 缺口（sema 不报、靠 codegen）：泛型**体**在实例化期的类型错（模板体内依赖 T 具体化的 ret / 赋值等）。调用点签名替换已由 SemaPass 做。这些路径的 throw 写 Compiler。

`yux-check <file>` 是 `yux build` 报错的**子集**。

# Sema / Codegen

目标：`yux-check` 独立、0 LLVM，诊断与 `yux build` 等价。当前半完成。

- `yux/frontend/sema/`（`sema_pass` / `call_resolve`，库 `yux_frontend`）禁止 LLVM、`IRBuilder`、`_module`。
- 新 `throw YuxError` 默认写 Compiler。`getType()` 的 YuxError 由 SemaPass **默认重抛**；必须暂留 Compiler 的码加入 `kDeferredCodes`（当前为空），禁止静默吞。方法点 callee 的 E3095：`visitExpr` 先按形态记下（挡住 E1101/E1140），Dot 分支校验 `@Spec` 后——字段非 Fn 值直接报，其余延迟重抛。ID-literal 的 E3095 立即重抛。sema 已覆盖的 Compiler throw / validate **删掉**，不双跑。
- 泛型 fn/impl 体已走 SemaPass：类型参数当不透明 TypeParam。形态检查（#NoCopy / 未定义符号 / arity / 已知 struct 缺字段）在模板体报；ret / 赋值 / 方法分派 / 内置运算符 / 一元运算符 / 索引 / 成员链 / 嵌套泛型推断在调用点或写出的 `S<Concrete>` 类型位置 `substitute` 后再查（E3014 / E3095 / E3001 / E3070 / E3071 / E3074 / E3062 / E3040 / E3041 / E3152 / E6012 / E6013）。
- 数组 / 元组字面量：`visitExpr` 带 target-type（声明 / 赋值 / 返回 / 调用实参 / 字段 / if·match·try 块末尾 / 嵌套），E3009 / E3012 由 SemaPass 抛。
- 调用实参：唯一 arity 非泛型候选（及 Fn 值 / 非泛型方法 / `#Static fn`）在重载前带形参类型下钻；同 arity 重载只对各位都相同的类型下钻，不一致的位置不猜。泛型 fn 显式 typeArgs（及推断完成后）按 `TypeInfo::substitute` 后的形参下钻 / E3014；无显式 typeArgs 时 infer 失败 → E6012（arity）/ E6013（无法反推），调用点与实例化后重抛；泛型 struct `#Static` turbofish → E3131；接收者已带 typeArgs 的实例方法同款。
- lambda：`expected` 为 Fn 时 `setInferredFnType` + bodyScope 形参回填（声明 / 上述调用点）；表达式体带返回类型下钻并做 E3014；块体 `ret` / 末位无 `;` 的尾表达式按 lambda 标注或反推返回类型检查，不用外层 fn。
- `ret`：Fallible 成功/错误双通道、T& 内层类型、Nullable wrap、别名 `resolveAlias`、灵活整数推断 → E3014。非法 T& 源形态由 analyzer E4001/E4020 先报。T& 局部 `&expr` 内层 E3014。
- 赋值：RHS 相对存储槽 E3014。T& 局部 / `$`（Self&）store-through 比内层 T；Nullable wrap / Rc wrap / 灵活整数与 codegen 对齐。索引赋值同款。经 T& / `$` 的嵌套数组走靶向类型 E3009。索引读 / 索引赋值：subst + peelRef 后非 `[N]T` / `Array<T>` → E3062（模板形参跳过）。成员赋值 / `&obj.field` / 读路径 `x.foo`：subst + peelAutoDeref 后缺字段 → E3040，非 struct → E3041，实例碰静态字段 → E3152（模板形参跳过；已知 struct 缺字段模板期也报）。方法调用 `x.foo()` 不走读路径。
- 声明：`Rc<T>` 同型或内层 T 包装 → E3014；`Weak<T>` 仅 Rc<T> / Weak<T> → E3016；`Array<T>` 非 Array 表达式且非字面量 → E3064。须在 visitExpr 带靶向类型之后查（if/match 块末尾先走 E3009）。Heap 非 `heap:<T>(...)` 由 borrow checker E4024 先报。模板形参等实例化后再查。
- 元组解构（`let (a, b) = expr` / `loop (a, b) = expr`）：标注优先否则 RHS，经 subst + `resolveAlias` 后非元组 → E3101；元素数 ≠ 名字数 → E3102。模板形参 T 跳过；`Array<T>` 等永远不是元组，模板期也报。
- match：各臂结果类型一致 → E3014（流终止臂跳过）。scrut 经别名 / Rc<E> / Heap<E> / E& 剥到 enum；临时 Rc/Heap 直接 match → E2022。if / match / try 块末尾值带靶向类型（嵌套数组 E3009）。
- 新 AST / 表达式：`SemaPass::visitExpr` 必须加分支（可 `return;`），否则整棵子树被 skip。
- 缺口（sema 不报、靠 codegen）：源码从未写出 `S<Concrete>` 且无调用时，泛型 struct 方法体内依赖 T 的类型错两边都不查（与未调用泛型 fn 一致）。未调用的泛型 fn 体内嵌套调用的 E6012/E6013 同样两边都不查。写出 `S<Concrete>` 的类型位置（形参 / 返回 / 字段 / let / 声明 / 全局常量）由 SemaPass 复查方法体。调用点签名替换、ret / 赋值、声明处 Rc / Weak / Array、元组解构（E3101 / E3102）、实例化后的方法分派 / 内置与一元运算符 / 索引（E3062）/ 成员链（E3040 / E3041 / E3152，含读路径）、以及调用点 / 实例化后的 typeArgs 推断（E6012/E6013）已由 SemaPass 做。其余依赖 T 具体化且未在调用点实例化的路径，throw 写 Compiler。

`yux-check <file>` 是 `yux build` 报错的**子集**。

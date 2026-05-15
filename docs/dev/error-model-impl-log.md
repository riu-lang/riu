# 错误模型 v1 实施日志

> 注：本日志使用旧名 Box / box；当前等同 Rc<T>。

本文件归档 yux 错误模型 v1 的实施记录：依赖图、各 Phase 决议（[#1]-[#9] + 复审 [#R]）、Phase 10 落稿子项 10a-10i。是后续回答"为什么 `#Fallible(E)` 不挤占返回类型"、"`!` 与 `try-catch` 的关系"、"main 错误退出 wrapper 在哪生成"、"为什么 panic 不进错误通道"等问题的事实来源。

- 规范条款见 `docs/spec/draft/DRAFT-错误.md`（草案落稿）+ spec 正文 §4.12 / §6.7 / §11
- 用户教程见 `docs/错误处理.md`
- 注解登记（`#Fallible` / `#NoReturn`）见 `docs/构建注解.md`
- SDK 测试集 `sdk/yux/src/yux/core/error_model.test.yux`（10g-8）

---

## 起草总目标（DRAFT-错误.md）

错误处理（值返回模型 + panic 终止）按依赖关系自上而下逐项决议，最终落到 `docs/spec/draft/DRAFT-错误.md`。yux **不引入异常机制**（无 unwind / SEH / personality），所以"异常"一词仅在比较 Java/C++ 路线时出现，不作为 yux 概念。

依赖图（上游→下游）：

- [x] **Phase 1 — 运行时载体**：值返回 only（Swift / Zig 路线）。**决议见下方 [#1]**。
- [x] **Phase 2 — 错误的"是什么"**：错误类型 = 普通 enum；每函数最多一个错误 enum；跨函数原"自动包装"规则在 Phase 5 撤销，改为跨类型必须显式 `match`。**决议见 [#2]**（含 Phase 5 撤销注释）。
- [x] **Phase 3 — 函数签名**：注解 `#Fallible(ErrEnum)`；§11 顺带解禁单参数注解糖。**决议见 [#3]**。
- [x] **Phase 4 — 抛出 / 传播语法**：抛出 = `ret ErrEnumValue`；传播 = 表达式后缀 `!`，仅同类型透传，跨类型必须 `match`；不做 [#2.C] 自动包装；不做"错误→null"。**决议见 [#4]**。
- [x] **Phase 5 — 传播糖与 main 出口**：v1 surface 锁定 + main 错误运行时呈现 + E7xxx 段位与措辞 + draft 内 `#Fallible` 解禁。**决议见 [#5]**。
- [/] **Phase 6 — 跨类型形态 + 类型推断边界 + RC/IR 实施**（拆为 A-G 七个子项）：
    - [x] **D**. 跨类型错误形态 → try-catch 块。**决议见 [#4.H]**（写在 Phase 4 决议块下，因与 `!` 紧耦合）。
    - [x] **E**. `T?` × `#Fallible(E)` 类型推断边界（O26）。**决议见 [#6.E]**。
    - [x] **F**. `ret ErrEnumValue` 在泛型 `T` 上的边界（O23）。**决议见 [#6.F]**。
    - [/] **A**. 错误通道 ABI 形态（tagged return / sret + i1 / 双返回）—— 草案 [#10.A] 推荐 anonymous struct `{ i1 isErr, T_ok, ErrEnum }`（候选 A），用户已确认 ✅；codegen 实施 10g。
    - [/] **B**. 错误返回路径的 RC 释放时序 —— 草案 [#10.B]：错误 ret 与成功 ret 共享同一段 `callDestructorsForScope()`，时序统一 [U1/U2/U3]；待 codegen 验证。
    - [/] **C**. 错误 enum payload 的 RC 协议（move vs retain）—— 草案 [#10.C]：透传 / catch 绑定 / wrap-rethrow / 成功透传 四种形态规约 [U4/U5/U6]；待 codegen 验证。
    - [ ] **G**. 析构 / panic 与错误通道交互 —— 与 Phase 8 panic 联动；推 Phase 8。
- [x] **Phase 7 — extern fn 边界**：**v1 整体推迟**。理由：外部（C ABI）函数没有 yux 错误通道概念；要让 `extern fn` 支持 `#Fallible(E)` / 错误翻译，需要先完成 (1) extern 使用限制（§7 当前接受 C 风格签名过于宽松，需收紧）+ (2) 函数类型 `fnType` / 参数承载错误类型的形态（v1 尚无函数类型）。**两项前置未完成前 Phase 7 不动**；v1 错误模型对 extern 的立场：`extern fn` 内**禁用** `#Fallible`（[#3.D] 表既存推迟项保持 ❌）；从 yux 调 extern 函数若失败，由 yux 包装层手动转 `#Fallible`。**决议见 [#7]**（仅记推迟理由 + 用户视角的临时写法）。
- [x] **Phase 8 — panic / 不可恢复**：abort-only；stdlib `panic(msg)` 函数 + `#NoReturn` 注解；不入错误通道；assert / OOM 走 panic。**决议见 [#8]**。
- [x] **Phase 9 — defer / finally**：v1 **不引入** defer / errdefer / finally。清理依赖 RC 析构 + 显式 try-catch 编排。**决议见 [#9]**。
- [/] **Phase 10 — 落稿**：DRAFT-错误.md 已起 ✅；附录 A / B / D 同步 ✅；CHANGELOG ✅；spec 正文 §4.12 / §6.7 / §11 修订 ✅（2026-05-10）。**§3 不动**：不引入独立 `Never` / `Bottom` 类型名，`#NoReturn` 函数签名仍写空 retType，控制流分析复用 `ret` arm 流终止语义。
    - [x] **10a** g4 改动：`buildAnno` 单参数糖、`exprTryCatch`、`catchArm`、`Try`/`Catch` lexer token、`exprCall`/`exprCallTrailingOnly` 末尾 `errPropagate=SymbolExcl?`（用户落 ✅；附录 B 同步对齐 ✅）
    - [x] **10b** 编译器注解白名单 + AST 扩展：(1) `ast_builder.cpp` 注解白名单加 `NoReturn`（零参） / `Fallible`（单参） ✅；(2) `Annotated` 类支持单参数糖（并行 `_annoArgs` 槽位 + `getAnnoArg(name)`） ✅；(3) `collectAnnos` / `ForDraft` / `NonFn` 改返回 `AnnoList { names, args }` + 形参/形参糖 arity 校验（多/缺参 → E2005） ✅；(4) 语义检查推 10d / 10e
    - [x] **10c** SDK：`sdk/yux/src/yux/core/exit.yux`（`#NoReturn fn exit(code u32)` 转发 `_exit(code.to_i32())`） ✅；`sdk/yux/src/yux/core/panic.yux`（`#NoReturn fn panic(msg String)` 调 `_yux_panic_failed(msg)`，写 stdout `panic: <msg>\n` 后 `RaiseException(0xE0FA1750)`，普通 build 无 SEH wrapper → OS 终止） ✅。烟测：`exit(7u32)` 退出码 7；`panic("boom")` 输出 `panic: boom\n` 后非零退出。TODO：stderr 通道（spec §8.6）待 STD_ERROR_HANDLE 接通。
    - [x] **10d** 编译器：`#NoReturn` 语义校验
        - [x] **10d-1** 头部级互斥（fn / structImpl 内方法）：E7012 retType 禁、E7013 与 `#Fallible` 互斥；`include/error_code.h` 注册 E7012/E7013/E7014；`tests/cases/diag_throw_e7012_*` + `diag_throw_e7013_*`（2026-05-10）
        - [x] **10d-2** 流终止分析：E7014（`#NoReturn` 函数体可达末尾），最小覆盖 ret / loop 无 break / if-else 全分支 / match 全 arm / 已知 `#NoReturn` callee 调用；`src/analyzer/flow_terminate_checker.{h,cpp}` 在 `compileFn` / `compileMethod` 入口随 `checkBorrows` 一同调用。`FnSymbolInfo` 新增 `isNoReturn` 槽，extern fnHeader 现允许 `#NoReturn`（`externFnAllowedAnnos = {CompilerInner, NoReturn}`），E7012 同步在 extern 上生效；E7013 暂不在 extern 上触发（`#Fallible` 仍排除在 extern 白名单外）。SDK：`extern ExitProcess` / `extern RaiseException` 标 `#NoReturn`；`exit(u32)` 直接调 `ExitProcess(code)`（去掉 `_exit` 间接层 + `to_i32/to_u32`）；`_exit(i32)` / `_yux_panic_failed(String)` 统一标 `#NoReturn`。`tests/cases/diag_throw_e7014_noreturn_falls_through` + `diag_throw_e7014_noreturn_loop_with_break`（2026-05-10）。备注：trailing void-typed 表达式（如末尾的无 `;` 形 `if cond { panic(...) }`）会被 ast_builder 合成为 `ret <expr>`，落入 `StatementRetNode` 路径——属预期行为，E7014 仅捕获**结构性 fall-through**。
    - [x] **10e** 编译器：`#Fallible(E)` 类型路由
        - [x] **10e-1** AST 扩展 `ExprCallNode._errPropagate`，ast_builder 从 g4 `errPropagate=SymbolExcl?` 槽读入（exprCall / exprCallTrailingOnly 两路）
        - [x] **10e-2** 声明侧：`FnSymbolInfo.fallibleErrType` 槽位；visitProgram 预扫 fn header 的 `#Fallible(E)` 单参；structImpl 方法同路径；E7008（成功值类型 == 错误类型）在头部注册时按字符串比较触发
        - [x] **10e-3** 调用侧：`compileCallExpr` 顶部插入 `checkErrPropagateForIdCall`，按 caller `_currentFnNode->header()` 的 #Fallible 注解 + callee FnSymbolInfo 的 `fallibleErrType` 决议 E7001 / E7004 / E7006。仅覆盖 ID-callee（最常见）；方法 / fn-value 调用的 `!` 校验留 10f / 10g 与 try-catch 一同决
        - [x] **10e-4** 测试：`tests/cases/diag_throw_e7001_bang_outside_fallible.{yux,expected_err}` / `_e7004_bang_type_mismatch` / `_e7006_bare_call_fallible` / `_e7008_success_eq_fallible`（2026-05-10）。当前 88 个 xmake 用例 + 259 个 SDK yux 用例全过
        - 备注：实际错误通道 codegen（`!` IR 路由 / `ret ErrEnumValue` 走错误返回 / `#Fallible` ABI）未在 10e 实施；推到 10g 与 try-catch 一并落 IR。10e 仅做语义校验，故 `#Fallible` 函数体内 `ret ErrEnumValue` 仍会触发 E3020（成功通道类型不符）——这是预期行为：用户暂不能写出能编译过的 `#Fallible` 函数（接口先行 / 返回成功值是允许的，[#3.F]）
    - [/] **10f** 编译器：`exprTryCatch` AST + 语义
        - [x] **10f-1** AST：`CatchArmNode`（ScopeNode，承载 `e` 绑定符号）+ `ExprTryCatchNode`（`tryBlock` + `catches`），`getType()` 取 try block 末表达式类型，流终止 arm 不参与（与 if-else / match 同档）
        - [x] **10f-2** ast_builder：`visitCatchArm`（先 push arm scope 再 visit body，binding 注册到 arm scope）+ `visitExprTryCatch`
        - [x] **10f-3** Compiler：`_tryCatchStack` 上下文栈；`compileCallExpr` 路径在栈非空时把 callee 的 `fallibleErrType` 追加到 ctx.seenErrTypes，并抑制 E7001 / E7006（裸调合法）；非 ID-callee / fn-value 路径同步抑制
        - [x] **10f-4 / 10f-5** 语义：`compileTryCatchExpr` 入口校验 E7011（catch 类型必须是已声明 enum），try block 编译完出栈后比对 `seenErrTypes` × `catchTypes` 触发 E7002（穷尽性），catch arm body 末表达式类型与 try block 不一致触发 E7010；穷尽性不下钻 lambda body（lambda emit 内独立编译流，与 `_tryCatchStack` 隔离）。E7015 / E7016 / E7017 / E7018 为 warning，编译器目前无独立 warning emit 通道，标 TODO 留给 spec §11 诊断分级落地后接入；E7009 由 g4 `(catchs+=catchArm)+` 强制，无运行时触发点
        - [x] **10f-6** codegen 占位：try block 当普通 block 编译，结果作为整体值；catch arms 编 dead block + unreachable 收尾（语义检查触发 + 类型一致性验证），错误通道实际 IR 路由推 10g
        - [x] **10f-tests** `tests/cases/diag_throw_e7011_catch_type_not_enum` / `_e7002_try_non_exhaustive` / `_e7010_catch_body_type_mismatch`（2026-05-10）；当前 91 个 xmake 用例 + 259 个 SDK yux 用例全过
        - 备注：实际错误通道 codegen（`!` IR 路由 / `ret ErrEnumValue` 走错误返回 / try-catch 路由）仍未在 10f 实施；推到 10g 与 #Fallible ABI 一并落 IR。10f 仅做静态语义校验 + 占位编译，故 try-catch 表达式在运行期不做错误分流（错误 variant 仍走当前的成功通道）；这是预期行为，diag 测试覆盖无运行期路径。E7016（`!` 在 try 内冗余）目前仅在 checkErrPropagateForIdCall 标 TODO，待诊断通道接入后启用警告输出
    - [/] **10g** codegen + 合法形态测试（按 [#10.A]/[#10.B]/[#10.C] 实施 IR）。子项：
        - [x] **10g-1** cache hash 字段校核：现有 `pkg_cache` 仅 hash 编译器指纹 + 各源 mtime+size，**完全不**做签名 hash；任何 fn 签名变更都会让 caller 模块 cache 误命中——这是编译器级既存限制，**不是 #Fallible 引入的新问题**，签名 hash 化推独立工程，不入 10g（O40 收口）。
        - [x] **10g-2** `wrapFallibleRetType` / `getFallibleRetStructType` helper（`compiler.h` + `compiler_types.cpp`）；wire 入 `getLLVMFunctionType` + 6 处 user-fn FunctionType::get 站点（compiler.cpp 泛型实例 / compiler_call.cpp getMethodFunction / 泛型 fn 调用 / sdk method / 方法 / fnSymbol fn）。`getMethodFunction` 增加 `fallibleErrType` 默认 ""。回归：91 xmake + 259 SDK 全过（2026-05-10）。
        - [x] **10g-3** `compileRetStatement` 错误 ret 分流（`compiler_stmt.cpp`）+ `compileRetVoidStatement` / `compileFn` / `compileMethod` 隐式 void-success 补尾。成功路径构 `{ false, retVal, zero }`、错误路径构 `{ true, zero, errVal }`；析构序与成功路径共享（[#10.B] U1）；E3020 措辞兼容"既不是成功也不是错误"。烟测 IR 形态正确（`{ i1, i32, %Enum }` + insertvalue）；91 xmake + 259 SDK 全过（2026-05-10）。
        - [x] **10g-4** `compileCallExpr` `!` 透传 IR：新增 `handleFallibleCallResult` helper（compiler.h + compiler_call.cpp）；callee 标 #Fallible 时 extract i1 → CondBr `fallible.err` / `fallible.ok`；err 分支取 ErrEnum 字段构外层 fn ret struct + `callDestructorsForScope` + ret；ok 分支 extract T_ok 继续。当前 wire 入 `compileKnownFunctionCall`（ID-callee 主路径，对应 10e 静态校验范围）；方法 / 泛型 / fn-value 路径推后续子项。烟测 IR 形态与设计 [#10.A] / [#10.C] 完全对齐：`extractvalue %r, 0` → isErr；err 分支 `insertvalue {true, 0, errVal}` + ret；ok 分支 `extractvalue %r, 1` 续编。回归：91 xmake + 259 SDK 全过（2026-05-10）。备注：try-catch 路由的 errBB 改写在 10g-5。
        - [x] **10g-5** try-catch 内调用路由 IR + catch arm `e` alloca：`TryCatchCtx` 增 `armEntryBBs` / `armEAllocas` 字段；`compileTryCatchExpr` 进入 try 前为每个 arm 预分配 entry BB + e alloca（类型 = arm errType 的 enum），推入栈；`handleFallibleCallResult` 错误分支优先在 `_tryCatchStack.back()` 查匹配 catchTypes，命中则 `store ErrEnum → eAlloca` + `br armEntryBB`（不退出当前 fn），未命中再走原透传路径；arm body 编译时把 `_localVarPtrs[errName] = eAlloca` 临时绑定，body 末尾 `br joinBB`。烟测：单 arm / 多 arm / arm 内 `match e { ... }` 解构均运行结果正确（1060 / -10 / -30）。回归：91 xmake + 259 SDK 全过（2026-05-10）。
        - [x] **10g-6** try-catch 表达式合并 (phi)：与 10g-5 一并落地。`compileTryCatchExpr` 用 `phiIncoming` 收集 try 成功路径末尾 + 各 arm 末尾的 (value, BB) 对（流终止分支不参与），joinBB 上 `phi T` 合并；纯 statement 形态（无 result）走 `ret nullptr`；全部分支流终止时 joinBB emit unreachable + return undef。回归：91 xmake + 259 SDK 全过（2026-05-10）。
        - [x] **10g-7** main `#Fallible` 出口 wrapper（spec §6.1）：`compiler.cpp` 主入口分支：main 标 `#Fallible(E)` 时改走 `Compiler::emitMainStartupFallible(E)`（compiler_types.cpp 末尾），否则走原 `runtime::emitMainStartup`。wrapper：调用 yux_main 拿 `{ i1 isErr, EnumLLVM err }`，extract isErr → CondBr `main.err` / `main.ok`；err 分支取 enum tag，按 `enumDecl->variants()` 顺序 switch 到 per-variant BB，每个 BB 写一段 `.rodata` 字符串 `"error: <module>.<EnumName>::<VariantName>[(...)]\n"` 到 stderr（GetStdHandle(-12) + WriteFile）后 ExitProcess(1) + unreachable；ok 分支 `ret 0`。runtime: `getOrCreateWindowsAPI` 加 GetStdHandle / WriteFile / ExitProcess（ExitProcess 标 NoReturn）。坑：`outWritten` alloca 必须在 CreateSwitch 之前 emit（switch 是 terminator，之后 IR 会破坏块结构 → LLVM "Dead.count(Pred)" 断言）。payload variant 当前打 "(...)" 占位（TODO §12.7.1 ToString 集成）。烟测：`run(2)!` 透传错误到 main → stderr "error: main.AppErr::Forbidden" + exit 1；payload variant 形态 "main.AppErr::Other(...)" 同样 exit 1。回归：91 xmake + 259 SDK 全过（2026-05-10）。
        - [x] **10g-8** SDK 合法形态测试 `sdk/yux/src/yux/core/error_model.test.yux`：11 个 `#Test` 覆盖（1）`#Fallible` 成功通道穿越 try block；（2）零参 variant `ret E::V` + catch 捕获；（3）payload variant `ret E::V(x)` + catch arm 内 `match e` 解构；（4）`!` 同类型成功 / 错误透传；（5）try 表达式 phi 合并（成功路径 vs catch 路径取值）；（6）多 catch arm 按类型路由（ParseErr / IoErr）；（7）跨类型显式包装（`#Fallible(AppErr)` helper 用 catch arm `ret AppErr::Parse(e)` / `ret AppErr::Io(e)` wrap 内层 ParseErr / IoErr，外层 try 解构 AppErr）；（8）catch arm 以 `ret` 流终止不参与 phi。回归：270 SDK 用例 + 91 xmake 用例全过（2026-05-10）。
    - [x] **10h** 用户教程 `docs/错误处理.md`（参 `docs/控制流.md` / `docs/枚举与匹配.md` 风格）：覆盖 `#Fallible(E)` 声明、`ret E::V` 抛错、`!` 后缀传播、`try-catch` 块（多 arm 路由 / payload 在 arm 内 `match e` / phi 合并 / 流终止）、跨类型 wrap-rethrow 标准写法、main 出口 wrapper 行为、`panic` / `#NoReturn`、v1 不支持清单 + 完整示例；`docs/index.md` 入门索引同步追加链接（2026-05-10）。
    - [ ] **10i** 归档：本文件迁 `docs/dev/error-model-impl-log.md`

每个 Phase 收束的产物：一段决议文字 + Open Issues。讨论收齐后再统一进 DRAFT 文件，避免边讨论边覆写。

---

### 决议 [#1] — 运行时载体：值返回 only（Swift / Zig 路线）

错误通过返回值传递。编译器在 LLVM IR 层**不**引入栈展开 / cleanup landingpad / personality function；可失败函数的调用走与普通函数一致的 callee-clean 协议（§8.5），错误值由调用方按枚举模式取出。

**形状参照**：Swift `throws` + `try` 调用 + `do/catch`；Zig `!T` error union + `try` / `catch` / `errdefer`。两者共同点是"语法看着像 Java/C++ 异常、ABI 是值返回"。yux 选这一路（语法形态在 Phase 4 决），但**不引入异常这一概念**——本草案后续不再使用"异常"作为 yux 术语，仅在与 Java/C++ 对比时出现。

**防滥用三条硬约束**（下游 Phase 必须遵守）：

1. **签名层显式**（Phase 3）：可能失败的函数**应当**在签名里标注（候选形态见 Phase 3）。未标注的函数**不得**失败传播，禁止"沉默失败"；也不允许 Java 风格的 `throws Exception` 顶层兜底签名。
2. **调用点显式**（Phase 5）：调用可失败函数的位置**应当**有可见标记（`?` / `try` 之类）。禁止隐式忽略；显式丢弃错误**应当**有专门形态（如 `try?` / discard 表达式），不允许"裸调用即吞错"。
3. **不可 catch-all**（Phase 4）：catch 段**应当**按错误 enum 的分支匹配，**不得**提供"捕获一切"语法（防 Java try-catch-ignore 反模式）。

**不做（终态）**：

- 不引入 unwind / 栈展开 / landingpad / SEH / DWARF personality。
- 不引入 Java 风格 checked exception 列表式签名（`throws E1, E2, E3` 是签名爆炸源）。
- 析构 / 构造函数**不**参与错误传播：析构出错 → abort；构造失败 → 改写为返回错误 enum 的工厂函数（普通方法 / `init` 不能失败）。

**Open Issues**：

- O1：`never` / `#NoReturn` 类型形态 —— Phase 8 panic / process-exit 入口需要；可能挪进 §3 类型系统而非本草案。
- O2：迭代器 / lazy seq 报错惯用模式（`Iter<Result<T,E>>` vs fail-fast）—— 推到 stdlib 设计阶段，本草案不收口。
- O3：OOM（`Box::new` / `Array` 扩容失败）走错误返回还是 abort —— 与 Phase 8 panic 联动。
- O4：是否引入 Zig `errdefer`（错误路径专属清理）—— 与 Phase 9 defer / finally 联动。
- O5：Phase 8 panic 是否完全 abort-only —— 若保留任何 catch 能力，等于以小子集形式重新引入 unwind，与本决议冲突，需在 Phase 8 显式重申。

---

### 决议 [#2] — 错误的"是什么"：普通封闭 enum + 嵌套包装

**[#2.A] 错误类型 = 普通 enum**。不引入"error 种类"，不引入运行时类型。完全复用 enum 草案现有机制（tuple-payload / 穷尽 match / RC 协议）。错误 enum 与普通 enum 在类型系统层不可区分；区别仅在它出现的"位置"（函数签名的错误通道，Phase 3 决形态）。

**[#2.B] 每个函数最多一个错误 enum**。一个 enum 内可任意多 variant。禁止 Java 风格 `throws E1, E2, E3` 列表（[#1.1] 推论）。

**[#2.C] ~~跨函数聚合 = enum 嵌套 + 自动包装~~ — 已撤销（Phase 5 修订）**

原决议为：caller 错误 enum 中找到与 callee 错误类型严格相等的唯一 variant payload → 编译器自动包装；歧义档编译错。

**撤销理由**：自动包装是隐式行为，与 [#1.1] "签名层显式" 基调有张力；写代码时 `call()!` 实际做了什么取决于 caller enum 内部结构，调用点信息不足。改为**跨类型必须显式 `match`**（[#4.B] 修订），零自动魔法。

**Phase 5 修订后规则**：

| callee 错误类型 vs caller `#Fallible(E)` 的 E | 行为 |
|---|---|
| 严格相等 | `call()!` 直接透传 |
| 不等 | `call()!` 编译错（E7004）；用户必须显式 `match` callee 错误后用 `ret E::V(...)` 构造 |

**显式不做**（同原决议，仍有效）：

- ❌ Rust 风格 `From<E>` 协议驱动错误传播。
- ❌ 自动深嵌套穿透。
- ❌ 子类型化 / 协变错误类型（必须严格类型相等）。
- ❌ "error 种类" / 错误专属类型构造器 / 内置 Result 根类型。

**Open Issues 更新**：

- O7 收口 ✅：跨函数传播规则定下（无自动包装；同类型透传 + 跨类型 match）。
- O9 收口 ✅（消解）：自动包装撤销后无"包装时 move vs retain"问题；用户显式 `ret E::V(payload)` 走普通 enum 构造路径。
- O10 收口 ✅（消解）：本地 catch v1 不做（[#4.C]），且无自动包装，"嵌套错误解构"不再是 Phase 4 课题。
- O11 收口 ✅：函数"无错误"形态 = 普通函数签名；不引入零错误 enum。
- O12 收口 ✅：跨类型显式糖（`!::V` / `as Variant`）整体不做；用户用 `match` 显式处理。

---

### 决议 [#3] — 函数签名：`#Fallible(E)` 注解，不动函数其余形态

**[#3.A] 失败声明 = `#Fallible(E)` 注解**。无该注解的函数**不**进入错误传播链；必然成功。零新关键字、零新符号。

```yux
enum ParseErr { Empty, Invalid(String) }

#Fallible(ParseErr)
fn parse_int(s String) i32 { ... }
```

**[#3.B] 严格一个参数，必须是已声明 enum 类型**：

| 形态 | 行为 |
|---|---|
| `#Fallible(E)` 且 E 已声明 enum | ✅ |
| `#Fallible()` / 缺参数 | ❌ |
| `#Fallible(E1, E2)` | ❌（[#2.B] 禁止多错误，本草案不解禁） |
| `#Fallible(NonEnumType)` | ❌（结构体 / 标量都不行） |
| 同函数多次 `#Fallible(...)` | ❌（重复声明） |
| `#Fallible(GenericParam)` | ❌（v1 错误类型必须是具体 enum，参 O13） |

**[#3.C] 函数其余部分完全不变**：错误通道**不挤占类型语法**。返回类型 `T` 仍是成功值类型。不写 `T!E`、不写 `T throws E`、不写 `Result<T,E>`。这是少符号原则在错误模型上的具体体现。

**[#3.D] 可附着位置**：

| 位置 | v1 行为 |
|---|---|
| 顶层 `fn` | ✅ |
| `structImpl` 内方法 | ✅ |
| `extern` 块内 `fnHeader` | 推到 Phase 7（FFI 边界）决 |
| `#CompilerInner` 函数 | ❌ 互斥（编译器 baked builtin 不进错误传播） |
| `#Test` 函数 | ❌ 互斥（断言走 SEH 路径，§11.3.5.3） |
| `draft` 内 `fn` 签名 | 暂 ❌；Phase 4/5 复审 |

### [#3.E]（前置依赖）§11 注解参数语法：仅解禁单参数糖

**注解机制方向性铺垫**（[#3.E.方向]）：`#Name` 概念上指向一个 `Name` struct；v1 仍是**编译器内置名集合**（`#CompilerInner` / `#Test` / `#DraftLike` / `#Fallible`），用户自定义注解（§11.6）的引入路径不变 —— 仍按 §11.6 Open Issue 推进。本草案**不**预先承诺用户自定义路径，仅把语法形态留出空间。

本草案落地时**同步修改**的规范条款（最小化）：

1. **§11.1.1.1** `buildAnno` 产生式扩展：仅解禁**单参数糖**形态：

   ```
   buildAnno ::= '#' ID ( '(' ID ')' )? codeLineEnd
   ```

   不解禁 `#Name(a, b, ...)`、不解禁字面量 / 表达式参数；这些留后续设计（O14）。

2. **§11.1.3.2** 改写为："`#Name(Arg)` 单参数糖在登记的注解上启用，未登记仍报错；多参数 / 字面量参数 v1 不接受。"

3. **§11.5.2.4** Open Issue "注解参数语法的设计窗口"由本草案部分收口（仅单参数 + 类型名 ID）；多参数 / 字面量留下一波。

4. **`src/yux.g4`**：`buildAnno` 产生式同步（按 CLAUDE.md 须用户确认后才动）。

### [#3.F] 与"无错误"形态的关系

普通函数 = "无错误"。**不**引入 `#Fallible()` 形态（呼应 O11；无错误就不写注解）。

声明 `#Fallible(E)` 但函数体内未实际产生错误：**允许**（接口先行 / 未来兼容）。因传播是显式（[#1.2]），不会蔓延。

### Open Issues

- O13：泛型函数 `#Fallible(T)` —— v1 禁止；理由是单态化每个实例化产生不同错误 enum，[#2.C] 包装查找规则会漂移。Phase 6 复审。
- O14：多参数注解 / 字面量参数（如 `#Link("kernel32")`）的设计窗口 —— 本草案不开启，留 §11.5.2.4 后续。
- O15：`extern fn` 上 `#Fallible(E)` 的语义 —— Phase 7 决。
- O16：方法重写 / draft 实现内方法的错误协变 —— v1 无方法重写，且 §12.3.1 签名不变，故无问题；仅作记录。

---

### 决议 [#4] — 抛出 / 传播语法：v1 同类型透传 `!` + 跨类型显式 `match`

**[#4.A] 抛出 = `ret ErrEnumValue`**。函数体内通过 `ret`（yux 关键字，`yuxLexer.g4:44 Ret : 'ret'`）后跟错误 enum 值进入错误通道，编译器按表达式类型分流到错误返回路径。

```yux
#Fallible(ParseErr)
fn parse_int(s String) i32 {
  if s.is_empty() { ret ParseErr::Empty }              ; 类型 ParseErr → 错误通道
  if !valid(s)    { ret ParseErr::Invalid(s) }
  ret atoi(s)                                          ; 类型 i32 → 成功通道
}
```

**类型分流前置约束**：函数成功值类型 `T` 与 `#Fallible(E)` 的 `E` **不得**相等（否则 `ret e` 歧义）。编译期检查 → E7008；违反报错。零新关键字。

**[#4.B] 传播 = 表达式后缀 `!`**。Phase 5 修订（撤销 [#2.C] 自动包装）+ Phase 6 修订（[#4.H] try-catch 块加入后形态）。

| 出现位置 | 行为 |
|---|---|
| **try block 内 + 裸调用 `#Fallible` 函数（无 `!`）** | ✅ callee 错误自动路由到本 try 的 catch 子句；try 块边界本身即"调用点显式"标记（[#1.2] 满足），替代逐调用 `!` |
| **try block 内 + 写了 `!`** | ⚠️ 行为与裸调用等价（同义），但 `!` 在 try 域内冗余 → 警告 E7016，fix = 移除 `!` |
| try block 外 + callee 错误类型 == 当前函数 `#Fallible(E)` 的 E | ✅ `!` 直接透传到错误通道，零包装 |
| try block 外 + callee 错误类型 ≠ 当前函数 `#Fallible(E)` 的 E | ❌ 编译错 E7004；用户必须用 try-catch 块显式转换 |
| try block 外 + 无 `#Fallible(E)` 的函数内 + 写了 `!` | ❌ 编译错 E7001 |
| try block 外 + 调用 `#Fallible` 函数但未加 `!` | ❌ 编译错 E7006（[#1.2] 调用点显式） |

**符号选择**：`!` 是当前空位的表达式后缀（lexer 层 `SymbolExcl` 仅作前缀一元 + `!=`，`yuxParser.g4:498`）。`?` **不**用于错误：

- `?` 三件套已属 nullable 家族（`T?` 类型 / `?.` 安全导航 / `??` 空合并）。
- 若 `?` 兼任"错误→null"折叠，函数返回 `T?` 且 `#Fallible(E)` 时 `call()? ?? default` 把"成功 None / 错误 V1 / 错误 V2"全部坍缩为 `null`，不可分辨——结构性沉默失败，违反 [#1.1]。详见 [#4.G]。

**词法边角**：后缀 `!` 与 `=` / `==` 之间**强制空白或换行**，避免被贪婪匹配吞成 `SymbolExclEq`（`x!=0` 永远是不等于；写"传播后比较"用 `x! == 0` 或绑变量）。这条进 §1 / §4。

**跨类型怎么写**：用 [#4.H] try-catch 块。Phase 6 决议把 v1 跨类型形态收口到 `try { ... } catch<E> { ... }`，详见 [#4.H]。

**[#4.C] ~~本地 catch v1 不做~~ — Phase 6 修订为允许 try-catch 块**

原决议为：v1 不做本地 catch，等 lambda 等多项前置完工后另立 v2。Phase 6 推翻——`try` / `catch` 升 v1 硬关键字，作为**专用块构造**（不依赖 lambda / Self 类型 / 方法泛型；详见 [#4.H]）。

修订后立场：

- v1 提供 try-catch 块作为跨类型错误转换的唯一形态；
- 不引入 lambda / 尾随 lambda / Self 类型 / 方法本地泛型 / generic enum（这些与 try-catch 无依赖）；
- [#1] "值返回 only / 无 unwind"基调不变——try-catch 是**编译期错误路径路由**，不引入运行期 SEH / DWARF。

**[#4.D] main 出口**：见 [#5.A] 完整定义（Phase 5 落锤）。

**[#4.E] 显式不做**（Phase 6 修订）：

- ❌ `throw` / `raise` / `err` 关键字。
- ✅ ~~`try` 关键字（保留为未来的 builtin 函数名）~~ — Phase 6 撤销，`try` 升 v1 硬关键字（[#4.H]）。
- ✅ ~~`catch` 关键字（保留为未来的方法名 / builder 段）~~ — Phase 6 撤销，`catch` 升 v1 硬关键字（[#4.H]）。
- ❌ 表达式级单点 catch（`expr catch e { ... }`）。**仍 ❌**——v1 catch 仅作为 try 块的子句出现，不允许"裸 expr catch"形态。
- ❌ `Result<T, E>` 内置根类型（仍按 [#2.A]）。
- ❌ `?` (error→null) 折叠（详见 [#4.G]）。
- ❌ `!::Variant` / `as Variant` / 任何跨类型显式包装糖（撤销 [#2.C] 后保持零糖；用户走 try-catch 块）。
- ❌ force-unwrap `!!`（不存在；nullable 取值仍走 `Nullable<T>.get()` → `_exit(1)`）。
- ❌ try-catch builder 链式形态（`try { }.catch<E>{}.catch<F>{}` 伪函数调用 / 真链式调用都不做）；v1 是专用块构造，不模拟函数调用形态（论证见 Phase 6 选型记录）。
- ❌ lambda / 闭包 / 函数类型 / 尾随 lambda 调用 / 方法本地泛型 / Self 类型 / generic enum —— 这些都**不**是 v1 错误模型的依赖；与 try-catch 完全解耦。

**[#4.F] ~~未来 v2 草图~~ — Phase 6 删除**

原决议草图（builder 链式 + lambda + generic enum）已被 Phase 6 [#4.H] 取代。v2 不再以 builder 路线立项；try-catch 块的扩展（labeled return / nested try / `errdefer`）按需追加，不需要新形态。

### [#4.G]（Phase 5 新增）显式不做"错误 → null"折叠

v1 **不**提供"出错则返回 `null`"的传播形态（无论用 `?` 还是别的符号）。

**结构性证明**（也是为什么 Swift `try?` 在 `T = U?` 时退化成 `U??`）：

```yux
#Fallible(IoErr)
fn read_optional_int() i32? { ... }    ; 三种结局：Some(n) / None / IoErr::*

; 假设我们提供了 call()? 把错误折成 null：
var v = read_optional_int()? ?? -1     ; v == -1 同时对应：
                                       ;   1) callee 成功 None（业务"未找到"，正常）
                                       ;   2) callee 错误 IoErr::NotFound（文件不存在）
                                       ;   3) callee 错误 IoErr::Denied（权限拒绝）
                                       ; 调试时不可分辨
```

这等价于 catch-all-ignore，与 [#1.1] "禁止沉默失败" / [#1.3] "不可 catch-all" 直接冲突。**v1 不开放、v2 不预承诺**；用户真要"我不关心错误"，写显式 `match`，5 行噪声是设计选择不是缺陷。

### [#4.H]（Phase 6 新增）try-catch 块作为跨类型形态

**形态**：

```yux
try BlockStart stmt* BlockEnd ( catch ID typeRef BlockStart catchBody BlockEnd )+
```

`catch <绑定名> <错误 enum 类型> { ... }` —— 错误类型**后置**而非泛型实参形态，与 yux `var x Type` / `fn f(x Type)` 声明风格一致。

不允许 `try { } finally { }`、不允许 `try { } catch e { }` 缺类型、不允许 `try { } catch IoErr { }` 缺绑定、不允许"裸 expr catch"（[#4.E] 仍 ❌）。

**示例**：

```yux
#Fallible(AppErr)
fn run() i32 {
  ; try block 内**裸调**可失败函数；错误按类型自动路由到对应 catch 子句
  var n = try {
    var s = read_file("a.txt")     ; 错误 IoErr → 自动路由到 catch e IoErr
    parse_int(s)                    ; 错误 ParseErr → 自动路由到 catch e ParseErr
    parse_int(s)                    ; 表达式值即整个 try block 的值（最后一句作为表达式）
  } catch e IoErr {
    ret AppErr::IoFailed             ; e 是绑定名；arm body 用 ret 走外层 fn 错误通道
  } catch e ParseErr {
    ; catch body 内可写 match e 分流（与 §3.10 match 同档）
    match e {
      ParseErr::Empty       => ret AppErr::ParseEmpty,
      ParseErr::Invalid(s)  => ret AppErr::ParseBad(s),
    }
  }
  ret n
}

; 反例（写法等价但触发警告）：
;   var s = read_file("a.txt")!    ; ⚠ E7016：try 域内 `!` 冗余；fix = 去掉 `!`
```

**`!` 与 try block 的关系（核心心智模型）**：

`!` 本身就是"如果错误就 catch + rethrow（同类型透传）"的糖。`try { } catch ... { }` 是显式的 catch 域。所以：

- **try 域内的可失败调用** —— 错误的"出口"已被 catch 子句承接，调用本身**不需要** `!` 重复声明"如果错误就抛"。`!` 在此处语义上等价于"嵌套一层 try-catch 自抛自捕"，是冗余的。给 E7016 警告。
- **try 域外的可失败调用** —— 错误的"出口"是外层 `#Fallible(E)` 通道；必须用 `!` 把调用点标显式（[#1.2] 调用点显式）。否则 E7006。
- **同一调用点不可能既"用 ! 标显式"又"在 try 域内被 catch 接住"** —— try 域内 catch 已经接，所以 `!` 重复。这是为什么 E7016 是**警告**而非错误：写出来不会改变语义（编译器视为同义），只是冗余表达。

**核心规则**：

| 规则 | 说明 |
|---|---|
| **关键字** | `try` / `catch` 升 v1 硬关键字；附录 A 增补两条；与 `else` / `match` 同级 |
| **try block 体** | 是普通 block；可含任意 stmt + 表达式；末尾表达式作为 try block 的值 |
| **try block 内可失败调用** | 裸调用 `#Fallible` 函数即可，错误自动路由到匹配 catch；不需要写 `!`。写 `!` 给 E7016 警告（fix = 去掉 `!`） |
| **catch 头部** | `catch <ID> <Type>`：绑定名 + 错误 enum 类型（后置）；类型必须是已声明的具体 enum；多个 catch 子句的类型必须两两不同；绑定名作用域仅 catch body 内部 |
| **catch body 形态** | catch body 是普通 block；body 内可对绑定变量 `e` 进行任意操作，包括 `match e { ... }` 按 variant 分流（与 §3.10 match 同档），或单分支处理 |
| **catch body 终结行为** | catch body 必须以以下之一结尾：(1) `ret X` 早返外层 fn；(2) `panic` 类不可恢复终止；(3) 表达式值（其类型必须等于 try block 的值类型，与 if-else 类型一致性同档） |
| **穷尽性** | try block 内**所有对 `#Fallible(E)` 函数的调用**（无论是否带 `!`）的 callee 错误类型集合，必须被 catch 子句声明的类型集合严格覆盖；不穷尽 → E7002 |
| **catch 子句多余** | catch 子句声明的类型不在 try block 错误集合内 → E7015 警告（不阻止编译；用户可能为防御性写） |
| **类型一致性** | try block 末尾表达式值类型 + 所有 catch arm 表达式值类型必须严格一致；不一致 → 编译错（与 if-else / match 同档） |
| **必须有 catch** | `var x = try { ... }` 不带 catch → 编译错；try 至少跟一个 catch |
| **嵌套合法** | try 内允许嵌套 try-catch；外层 try 不会"穿越"内层已捕获的错误（与 Java/Kotlin 同档） |
| **与外层 `#Fallible(E)` 的关系** | try-catch 块**完全消化**所捕获的错误类型；外层 fn 的 `#Fallible(E)` **仅**接收 catch arm 内 `ret` 出去的错误。换言之：try block 内的可失败调用错误"内部消化"，不直接接到外层 |
| **`!` 在 try 外** | 行为同 [#4.B]（同类型透传 / 跨类型 E7004）；try-catch 不影响 try block 外的 `!` 语义 |
| **try block 内 `!` 冗余警告** | try block 内对 `#Fallible` 函数的调用写了 `!` → E7016 "`!` is redundant inside `try` block"；fix = 移除 `!`。语义不变（编译器视同义），仅冗余表达 |
| **try block 全成功路径警告** | try block 内不含任何 `#Fallible` 调用 → 整个 try 退化为普通 block；E7017 "redundant try-catch (no errors possible in try block)" |
| **catch arm 直接 panic 警告** | catch arm body 唯一终结操作是 `panic(...)` → E7018 "`panic` in `catch` arm converts a recoverable error to abort — consider `ret` with an error variant or `exit(code)` if termination is intended"。可 suppress；确实"这条路径就是 bug"时正确忽略 |

**try block 内调用的路由模型**（informative，编译器实施可任选等价 IR）：

```yux
try { make_error().member } catch e E { HANDLE }
```

`make_error()` 类型 = `#Fallible(E)`；编译器在该调用点生成"若错误则跳到本 try 的 catch 路由"的控制流，等价于把每个对 `#Fallible` 函数的调用包成"local ! 风格的同类型透传，目标是本 try 域而非外层 fn"。实施层不强制嵌套 IR，可扁平化。

**为什么 `!` 在此处冗余**：`!` 的语义就是"如果错误就 catch 后 rethrow"。在 try 域内 catch 已显式存在，rethrow 的"目的地"就是该 catch（而非外层 fn）；写 `!` 等于让编译器先建一个内层 try-catch 再让外层 try-catch 接住，是两层等价路径，故同义。

**与 [#1] 防滥用三条硬约束的关系**：

| 约束 | try-catch 是否冲突 |
|---|---|
| [#1.1] 签名层显式 | ✅ 一致：`#Fallible(E)` 仍是函数签名层声明，try-catch 不绕过 |
| [#1.2] 调用点显式 | ✅ 一致：`!` 仍是调用点显式标记；try block 内 `!` 处仍可见 |
| [#1.3] 不可 catch-all | ✅ 一致：`catch e E` 必须指定具体 enum 类型；不允许 `catch e { }` 缺类型，不允许 `catch e Any` 之类元类型 |

**与 [#4.G] "错误→null 不做"的关系**：

try-catch arm 必须 `ret` 或产生表达式值；表达式值类型必须与 try block 一致。这意味着用户**不能**用 catch arm 把错误"转 null"——除非 try block 表达式值本身是 nullable 类型，但那是用户主动选择的类型，不是错误模型偷换。

**Phase 6 收口的 Open Issues**：

- O25 收口 ✅（D 子项）：跨类型形态 = try-catch 块；E7004 仅在 try block 外触发。
- O20 收口 ✅（v2 课题原本指 builder 路线，现 [#4.F] 已删除，无 v2 builder 课题）。
- O24 收口 ✅（消解）：v2 不再立 builder vs Result 路线之争；try-catch 块是 v1 + 未来增量的统一形态。

### Open Issues

- O21 收口 ✅（[#5.A]）：main 错误运行时呈现 = `_exit(1)` + stderr 模板。
- O22 收口 ✅（[#5.B] + [#4.H]）：诊断措辞模板 E7001-E7011 + E7015（含 try-catch 相关）。
- O23（新）：`ret ErrEnumValue` 当 `T` 是 nullable / generic 时的类型推断规则边界 —— Phase 6 子项 E/F，本决议未触；后续完成。
- O27（Phase 6 新增）：try-catch 块的"嵌套退出 / labeled return"形态（如 Kotlin `return@try` / Rust labeled break）—— v1 不做；用户先用 nested try-catch + 普通 ret 完成多层退出。后续草案可加。
- O28（Phase 6 新增）：`errdefer` / catch arm 内的清理路径 —— 与 Phase 9 defer 联动，本决议不动。

---

### 决议 [#5] — 传播糖与 main 出口（Phase 5 落锤）

**[#5.A] main 出口运行时呈现**：

- 退出码：固定 `_exit(1)`。理由：与 yux 既有"不可恢复终止"路径（`Nullable<T>.get()` 越界、`Array.at()` 越界）一致；不引入"错误 variant → exit code"映射（顺序耦合、ABI 漂移）。用户要区分错误码 → 在更外层 `#Fallible(E_outer)` 函数自行 `match` 后调 stdlib 进程退出函数，main 不再 `#Fallible`。
- stderr 输出：`error: <enum 限定名>::<variant>[(<payload.to_string()>)]\n`
    - 限定名形式：跟随 §10 模块路径（`.`） + §3.10.4.1 enum variant 限定（`::`）的**当前**书写约定，例：`mymath.ParseErr::Empty`、`app.io.IoErr::NotFound("path")`。本草案不硬编码字面量分隔符，跟随后续 enum / 模块约定演化。
    - payload 走 §12.7.1 `ToString`：实现了 `ToString` → 打 `(payload.to_string())`；**未实现 → 省略括号部分**（不打 `<unprintable>` 占位）。无 payload 的 variant 与"payload 无 ToString"统一无括号。
    - 多行 payload：原样输出（不转义）。用户嫌乱在外层包装。
    - 彩色：**纯文本，无 ANSI**。本输出是用户**程序运行期**的 stderr，不是编译期诊断；混入彩色会污染 `2>&1 | grep` 之类管道。

**[#5.B] 诊断段位与措辞**：错误模型独占段位 **E7xxx**（附录 D 既有 E1/E2/E3/E4/E5/E6/E11xx，E7-E10 当前空闲；本特性 + Phase 7 FFI / Phase 8 panic / Phase 9 defer 共享 E7xxx）。

| 错误码 | 措辞模板 |
|---|---|
| **E7001** | `` `!` used outside of `#Fallible(E)` function and outside of `try` block — wrap call in `try { ... } catch e E { ... }` or declare the enclosing function with `#Fallible(E)` `` |
| **E7002** | `` non-exhaustive `try` block: error type `{E}` thrown by callee `{callee}` is not handled by any `catch` clause — add `catch e {E} { ... }` `` |
| **E7003** | `` redundant `catch` clause: error type `{E}` cannot be thrown by any call in the `try` block (note: this is a warning, see E7015 for the suppressible form) — promoted to error if `--strict-catch` `` |
| **E7004** | `` cannot propagate error of type `{callee_err}` through `!`: caller declares `#Fallible({E})`, types differ — wrap the call in `try { ... } catch e {callee_err} { ret {E}::Variant... }`, or wrap in a function whose `#Fallible` matches `{callee_err}` `` |
| **E7005** | `` duplicate `catch` clause: error type `{E}` is handled by more than one `catch` in the same `try` — merge into a single `catch e {E} { match e { ... } }` `` |
| **E7006** | `` call to fallible function `{callee}` outside `try` block must propagate via `!` (same error type) — bare call is forbidden outside `try` (inside `try`, bare call is correct; `!` would be redundant) `` |
| **E7007** | `` `ret` of error type `{got}` does not match `#Fallible({expected})` — wrap the error in a `{expected}` variant or change the function's `#Fallible` `` |
| **E7008** | `` function return type `{T}` cannot equal its `#Fallible` type `{E}` (the compiler cannot disambiguate `ret` between success and error channels) — split into two enums (one for the success result type, one for the error type) and rethrow / wrap explicitly. v1 does not provide a `throw` keyword to disambiguate (reserved for future revision) `` |
| **E7009** | `` `try` block must be followed by at least one `catch` clause — bare `try { ... }` is forbidden `` |
| **E7010** | `` `catch` body must end with `ret`, `panic`-class terminator, or an expression of the same type as the `try` block (got `{T_catch}` vs `{T_try}`) `` |
| **E7011** | `` `catch e {E}` type `{E}` must be a declared enum; got `{actual}` `` |
| **E7015** | `` redundant `catch` clause: no call in `try` block can throw `{E}` declared by `catch e {E}` — remove this `catch` clause ``
| **E7016** | `` `!` is redundant inside `try` block: bare call to `#Fallible({E})` function `{callee}` already routes to the matching `catch e {E}` clause — remove `!` ``
| **E7017** | `` redundant `try-catch`: no call in `try` block can throw any error — remove the entire `try` and use a plain block `` |

诊断回归用例：沿用 §11.3.5 / §附录 D 的 `tests/cases/diag_*.yux` 模板，落到 `tests/cases/diag_throw_*.yux`，每条 E7xxx 至少一例。这是 Phase 10 落稿后的实施层动作，本草案仅记约束。

**[#5.C] O12 收口（歧义档显式 variant 选择糖）**：**不做**。撤销 [#2.C] 后已无"歧义档"概念（无自动查找就无歧义）；用户跨类型场景一律 `match` 显式，零糖。

**[#5.D] [#3.D] 修订 — draft 内 `fn` 签名允许 `#Fallible`**：

将 [#3.D] 表格 `draft 内 fn 签名` 行从 ❌ 改为 ✅，注：错误类型必须为已声明的具体 enum（沿用 [#3.B]，禁泛型参数 / 关联类型）。draft 实现签名必须与 draft 声明的 `#Fallible(E)` 完全一致（§12.3.1 签名一致性）。

| draft 形态 | v1 行为 |
|---|---|
| `draft D { #Fallible(E) fn m() T }` 且 `E` 是已声明 enum | ✅ 允许 |
| 实现方 `Type : D { #Fallible(E) fn m() T { ... } }` 与 draft 签名一致 | ✅ |
| 实现方省略 / 改写 `#Fallible` | ❌（§12.3.1 签名不一致，走现有诊断路径） |
| draft 方法 `#Fallible(关联类型 / 泛型参数)` | ❌（[#3.B] 已禁，沿用） |

**[#5.E] 词法 / 语法侧的同步修订**（落稿 Phase 10 一并做，本草案仅记清单；Phase 6 [#4.H] 加入 try-catch 后扩充）：

- §1 词法：补"后缀 `!` 与 `=` / `==` 之间需空白或换行"边角约束。
- §4 表达式：增加 `expr!` 后缀错误传播形态条目；与前缀 `!expr`（逻辑非）位置分流。新增 `tryExpr` 形态。
- 附录 A：保留字表新增 **`try`** / **`catch`** 两条（[#4.H] 升 v1 硬关键字）；`ret` 已存在仅校核；符号表 `!` 后缀语义补一行。
- 附录 B：`exprUnary` 后增加 `exprErrPropagate ::= postfix '!'` 形态；新增 `exprTry ::= 'try' '{' stmt* '}' (catchClause)+`、`catchClause ::= 'catch' ID typeRef '{' stmt* '}'`。
- 附录 D：新增段位 E7xxx 表头 + E7001-E7011 共 11 条 + E7015 警告码。
- §11.1 注解参数语法：按 [#3.E] 单参数糖解禁同步。
- `src/yux*.g4`：按 CLAUDE.md 须用户单独确认才动；本草案不直接修改。

### Open Issues（Phase 5 收尾）

- O21 收口 ✅。
- O22 收口 ✅。
- O12 收口 ✅。
- O25 收口 ✅（[#4.H] try-catch 块即 v1 跨类型形态）。
- O26 收口 ✅（[#6.E]）：`#Fallible` 与 nullable 返回类型 `T?` 组合允许；三种结局通过通道分流可分辨，不与 [#4.G] 冲突。
- O23 收口 ✅（[#6.F]）：泛型 `T` 上 `ret ErrEnumValue` 边界 = 单态化阶段触发 E7008（T 实例化为 E 时）。

---

### 决议 [#6] — Phase 6 类型推断边界（E / F 子项）

A / B / C / G 是编译器实施层课题，**对用户语义无影响**，按 §8.5 现有模式在 Phase 10 落稿时与 IR 一起决；本草案**不**预先画 IR。
D 子项已在 [#4.H] 写完（与 `!` 紧耦合，放 Phase 4 决议块下）。
E / F 是**用户可见**的类型推断边界，必须在草案里写清楚。

### [#6.E] `T?` × `#Fallible(E)` 组合允许

**形态**：

```yux
#Fallible(IoErr)
fn read_optional(path String) i32? { ... }    ; 三种结局：Some(n) / None / IoErr::*
```

**允许，无特殊规则**。返回类型 `T?` 是**成功通道**的类型；错误通道独立由 `#Fallible(E)` 承载。三种结局：

| 结局 | 通道 | 表现 |
|---|---|---|
| `Some(n)` | 成功通道 | callee 返回 `i32?`（含值），caller `read_optional()!` 得 `Some(n)` |
| `None` | 成功通道 | callee 返回 `i32?`（无值），caller `read_optional()!` 得 `None` |
| `IoErr::NotFound` 等 | 错误通道 | caller `!` 透传到外层 `#Fallible`（同类型）或被 `try-catch` 捕获 |

调用点写法：

```yux
#Fallible(IoErr)
fn use_it() i32 {
  var x i32? = read_optional("a.txt")!     ; 错误已透传；x 仍是 i32?
  ret x ?? -1                              ; ?? 处理"无值"，与错误无关
}
```

**与 [#4.G] 的关系**：[#4.G] 拒绝的是**操作符把错误折叠成 `null`**（`call()? ?? -1` 同时吞 `None` 和错误）。本组合**不折叠**——`!` 走错误通道、`??` 走成功通道的 nullable，分得清。

**用户视角的潜在误用 + 编译器抓点**：

| 误用尝试 | 现状 |
|---|---|
| `read_optional()!?.method()` | 合法且语义清晰：先 `!` 走错误通道（向上抛），再 `?.` 在 `i32?` 上做安全导航。两个操作符在不同通道，无歧义。 |
| `var x = read_optional()` 不加 `!`，调用点裸写 | E7006（[#4.B]）—— 既有规则覆盖，与 `T?` 无关。 |
| 用户期望 `read_optional()` 用 `??` 把错误也吞掉 | 不可能：`??` 作用于 `T?`，但调用点必须先有 `!` / `try-catch` 处理错误通道；`??` 接触不到 `IoErr`。"用 `??` 吞错"是结构上做不到的（这正是 [#4.G] 拒绝 `?` 的目的）。 |
| 嵌套 nullable：`#Fallible(E) fn() T??` | 允许但不鼓励；`T??` 本身的语义在 §3 nullable 决，错误模型不增规则。 |

**结论**：`T?` × `#Fallible(E)` 组合**不增任何规则**，复用既有 [#4.B] / [#4.H] / `T?` 既有语义。E7xxx 表无需新增条目。

### [#6.F] 泛型 `T` 上的 `ret ErrEnumValue` 边界

**前置约束**（既有，复述）：
- `#Fallible(E)` 的 `E` 必须是**已声明的具体 enum**（[#3.B]，禁泛型参数）。所以 `#Fallible` 通道的类型在所有实例化下不变。
- 函数成功值类型可以是泛型参数 `T`。

**形态**：

```yux
enum MyErr { Empty, Bad(String) }

#Fallible(MyErr)
fn first<T>(arr Array<T>) T {
  if arr.is_empty() { ret MyErr::Empty }    ; MyErr → 错误通道
  ret arr[0]                                 ; T → 成功通道
}
```

**声明阶段检查**：通过。`MyErr` 是具体 enum，`T` 是泛型参数；类型不同，[#4.A] 的"成功类型 ≠ 错误类型"约束在声明阶段无法判（因 `T` 未实例化）。声明本身合法。

**单态化阶段检查**：每次实例化时，把 `T` 替换为具体类型后**重跑** [#4.A] 约束：

| 实例化 | 行为 |
|---|---|
| `first<i32>` | T = i32 ≠ MyErr → ✅ |
| `first<String>` | T = String ≠ MyErr → ✅ |
| `first<MyErr>` | T = MyErr == #Fallible 的 E → ❌ E7008（在该实例化点报错；指出实例化位置 + 函数声明位置） |
| `first<Array<MyErr>>` | T = Array<MyErr> ≠ MyErr（顶层类型不等就 OK，不递归）→ ✅ |

**E7008 措辞调整**（[#5.B] 已有 E7008，本子项**不**新增码，仅扩措辞覆盖单态化场景；落稿 Phase 10 一并写）：

```
function return type `{T}` cannot equal its `#Fallible` type `{E}` (the compiler cannot disambiguate `ret` between success and error channels)
[generic instantiation]: in `{fn}<{T_arg}>` instantiated at {site}, type parameter `{T}` resolved to `{E}`, which equals the function's `#Fallible` type
— change the type argument or split the enum into two
v1 does not provide a `throw` keyword to disambiguate (reserved for future revision)
```

**用户视角的潜在误用 + 编译器抓点**：

| 误用尝试 | 现状 |
|---|---|
| `#Fallible(T) fn f<T>() i32` | E7008 前置——[#3.B] 禁 `#Fallible(GenericParam)` 在声明阶段直接拒。 |
| `first<MyErr>(some_array)` 实例化 | 单态化点 E7008；用户得知"具体的 T 与错误 enum 撞了"，要么换错误 enum、要么换业务 enum。**早期记录**：错误信息里附实例化栈 + 声明位置，避免用户在调用栈里盲找。 |
| `enum E { V(T) }` payload 含泛型 + `#Fallible(E)` | E 不是泛型 enum（v1 enum 无泛型，[#4.D] / Phase 6 之前已确认延期）。所以根本写不出来；不是错误模型课题。 |
| 写 `ret arr[0]` 在泛型函数里、运行时 `T` 恰好是 `MyErr` | 不是运行时事件——单态化阶段就报错；运行时不可能走到。 |

**与 [#4.A] 的关系**：[#4.A] 是声明阶段约束，本子项把它**延伸**到单态化阶段（同一条 E7008，不同触发点）。语义统一：成功通道类型与错误通道类型在**任何**实例化下都不可相等。

**结论**：F 子项**不增码**、**不增规则**；扩 E7008 措辞 + 单态化阶段重跑既有约束。落稿 Phase 10 一并写到附录 D。

### Open Issues（Phase 6 收尾）

- O23 收口 ✅（[#6.F]）。
- O26 收口 ✅（[#6.E]）。
- O29（Phase 6 新增）：单态化阶段诊断的"实例化栈"输出格式 —— 沿用 §11 / 现有泛型诊断模板（若已有）；落稿时校核现有诊断是否已具备此能力，无则推 §11 增补。本草案不动 §11。
- O30（Phase 6 新增 / 推 Phase 7）：`extern fn` 上的 `#Fallible(E)` 与 FFI 错误翻译 —— 已在 [#3.D] 标 Phase 7 决，本决议不动。

---

### 决议 [#7] — extern fn 边界：v1 整体推迟

**推迟理由**：要让 `extern fn` 承载 yux 错误通道，需要两项前置工程：

1. **extern 使用限制收紧**：当前 §7 `extern` 块对 C ABI 签名约束较松；要在 FFI 边界检测 / 翻译错误，需先把"什么 extern 签名合法"明确（如哪些类型可跨边界、是否允许 callback 等），否则错误翻译规则的适用面无法定义。
2. **函数类型 `fnType` 形态**：v1 尚无函数类型（callback / 函数指针 / lambda 全空）。将来 `fnType` 加入时，错误类型应是其参数之一（例如 `fn(i32) i32 #Fallible(E)` 或等价形态），与 `#Fallible` 注解保持一致。这部分必须等 `fnType` 草案落定。

两项任意一项未完成，FFI 错误翻译形态都会反复返工。Phase 7 在 v1 错误模型草案里**不开窗口**。

**v1 立场（用户视角）**：

| 场景 | 写法 |
|---|---|
| 调 C 库 `int read_file(char*, char**)` 返回 errno | `extern fn read_file(...) i32`；yux 侧用普通 wrapper 检 errno 后 `ret YuxIoErr::...`（手动翻译） |
| 给 C 库传 yux callback | v1 无函数类型，整个场景写不出来；不是错误模型独立课题 |
| 在 yux 库里把"可能失败的 C 调用"暴露给上层 | 写一层 yux wrapper：`#Fallible(E) fn yux_read(...) Result { var r = c_read(...); if r < 0 { ret E::IoFail }; ret ok(r) }` |
| 在 `extern fn` 头写 `#Fallible(E)` | ❌（[#3.D] 表 Phase 7 行保持 ❌；E70xx 不新增码，落到 §7 既有 extern 检查诊断里） |

**推迟检查**：写本草案的 [#3.D] 表时已把 `extern` 行标记为 Phase 7，不再单列 Open Issue。本决议**不**新增 E7xxx 码；不改任何条目，仅留推迟说明。

### Open Issues（Phase 7 推迟收尾）

- O15 仍开放：等 extern 限制 + `fnType` 草案落定后开 Phase 7 单独决议。本草案不再讨论。
- O30 收口（部分） ⏸️：v1 不解；推迟理由已写。

---

### 决议 [#8] — panic / 不可恢复：abort-only，与错误通道完全分离

**[#8.A] panic 在错误模型中的位置：abort-only，不入错误通道**

panic = "程序碰到 bug / 不变式破坏，无法继续"。它**不**走 `#Fallible(E)` 错误通道，**不**可被 `try-catch` 捕获，**不**可被 `match` 处理。这是 [#1] 防滥用约束 + O5 "Phase 8 必须 abort-only 否则等于以小子集重新引入 unwind" 的直接落实。

| 触发源 | 路径 |
|---|---|
| `Nullable<T>.get()` 在 `null` 上 | panic（既有，§3.10.2 已定）|
| `Array.at(idx)` 越界 | panic（既有）|
| 整数除零 / 算术溢出（已有运行时检查的） | panic（沿用既有路径）|
| 用户显式 `panic("...")` | panic（[#8.B] 新增）|
| OOM（`Box::new` / `Array` 扩容失败）| panic（[#8.E]）|
| 析构函数抛出 | panic（[#1] 已定，沿用）|
| `#Fallible(E)` 函数 `ret E::V(...)` | **不是 panic**，是错误通道 |

**与 try-catch 的隔离**（[#4.H] 已落，本节复述强调）：

```yux
try {
  some_fn()          ; 正常错误：可被 catch 接住
  arr.at(999)        ; panic：穿透 try-catch，直接 _exit(1)
  panic("bug")       ; panic：穿透 try-catch
} catch e SomeErr {
  ...                ; 永远接不到 panic
}
```

[#4.H] 的 catch 类型必须是已声明 enum（E7011），panic 不带 enum 类型；语言层从语法上就**不可能**写出 catch panic 的形态。零额外约束，零新诊断码。

**[#8.B] 触发形态：stdlib 函数 `panic(msg)`，不引入关键字**

```yux
#NoReturn
fn panic(msg String) { ... }    ; stdlib，路径 yux.core.panic（沿用 §10 模块路径）
```

形态选择：

| 候选 | 取舍 |
|---|---|
| 关键字 `panic "msg"` / `panic("msg")` | ❌ 多一个保留字；与"少符号 / 少关键字"基调冲突；且 `panic` 本质就是函数调用 + 永不返回，无须语法形态 |
| stdlib 函数 `panic(msg String)` ✅ | 与 `_exit` 同档；用户调用形态自然；编译器仅需识别 `#NoReturn` 注解做控制流分析 |
| 宏 `panic!("...")` | yux 无宏系统；不立项 |

**滥用方向 + 早期记录**：

| 误用 | 现状 |
|---|---|
| 用户用 `panic("...")` 当作快速错误处理（绕过 `#Fallible`） | 语言层**不**禁止；通过命名 + 文档引导：`panic` = "代码 bug / 不可能到达"；`#Fallible(E)` = "预期会失败的业务路径"。诊断层不强制 —— 强制会逼用户写出更糟的形态（如把所有错误塞 `Result<T, E>` 嵌套）。 |
| 在库代码里随便 `panic` 让上层调用方崩溃 | 同上，库设计风格问题，不是语言可强制的层面；落到 stdlib 编码规约 + 代码评审。 |
| 在 catch arm 内 `panic("...")` | 合法（[#4.H] catch body 终结行为已含 "panic-class terminator"）。这是用户主动声明"这条路径就是 bug"，正确用法。 |
| 在 `#Fallible(E)` 函数内既 `ret E::V` 又 `panic(...)` 混用 | 合法：错误通道与 panic 是两个独立通道；混用本身没问题（panic 总是无条件终止；error ret 是正常返回路径）。 |

**[#8.C] `#NoReturn` 注解（O1 收口）**

引入 `#NoReturn` 作为新注解（与 `#Fallible` 同档，[#3.E] 单参数糖以外的零参数注解）。

| 形态 | v1 行为 |
|---|---|
| `#NoReturn fn f(...) {}` | ✅ 函数声明永不返回；控制流分析将 `f()` 之后的代码视为不可达；返回类型必须为 unit（即省略） |
| `#NoReturn fn f(...) i32 {}` | ❌（既然不返回，写返回类型无意义；E7012）|
| `#NoReturn` + `#Fallible(E)` 同函数 | ❌ 互斥（E7013）：既然 panic 终止，错误通道无意义；语义冲突 |
| 用户在 `if panic(...) else x` 之类位置使用 panic | ✅ 合法；编译器视 `#NoReturn` 调用点为流终止（与 `ret` 同档），不参与表达式类型合并；该 arm 被排除出"两 arm 类型须一致"的统一计算。yux 不引入独立 `Never` 类型名，永不返回是注解层属性、不是类型层属性。 |
| `#NoReturn fn` 内未触发终止就到达函数末尾 | ❌ E7014：`#NoReturn` 函数控制流必须以终止操作（panic / 调另一 `#NoReturn` / 无限循环）结尾 |

**`#NoReturn` 选址**：放在错误模型草案里**因为 panic 需要它**；但 `#NoReturn` 本身是控制流 / 类型系统课题。落稿时该注解的描述放 §3 或 §11，错误草案仅引用——具体放哪 Phase 10 落稿决。

**[#8.D] assert 分流**

| 场景 | 路径 |
|---|---|
| 普通 yux 代码内 `assert(cond)` / `assert(cond, msg)` | stdlib 函数；条件不成立 → 调 `panic("assertion failed: <msg>")`；走 [#8.A] 路径 |
| `#Test` 函数内 assert | 沿用 §11.3.5.3 既有 SEH 路径（test runner 接住、记录失败、继续下一用例）。本草案**不动**该路径，仅复述：测试断言**不**走 panic 路径，这是 §11 的特殊机制 |
| `#Test` 内调到普通代码、普通代码触发 panic（含真 panic 与 assert→panic）| §11.3.5.3 现有 SEH 已能承接（test runner 视为该用例 panic 失败）；本草案不重申 |

**[#8.E] OOM 决策（O3 收口）**

`Box::new` / `Array` 扩容 / `String` 扩容等内存分配失败 → **panic**。

理由：
- 大多数业务代码碰到 OOM 无法有意义恢复；让每个 `Box::new` 返回 `Box<T>?` 或 `#Fallible(AllocErr)` 会污染**所有**类型签名与所有调用点。代价远超收益。
- 长寿命服务进程 / 嵌入式等真需要可恢复 alloc 的场景：stdlib 后续提供 `try_alloc<T>(...) Box<T>?` 类显式 API（v1 **不做**；O3 在此收口为"v1 abort，可恢复 API 留 stdlib 后续"）。
- 与 [#1] 防滥用一致：让 OOM 走错误通道 = 鼓励"假装能恢复 OOM"的 catch 模式，绝大多数场景是反模式。

**[#8.F] main 退出码 / stderr 与 [#5.A] 的关系**

| 终止源 | 退出码 | stderr 模板 |
|---|---|---|
| `#Fallible` main 错误未处理（[#5.A]） | `_exit(1)` | `error: <enum 限定名>::<variant>[(<payload.to_string()>)]\n` |
| panic（含 assert / 越界 / OOM / 用户 `panic("...")`）| `_exit(1)` | `panic: <msg>\n`（msg 由触发源决定：用户 panic 用其参数；越界 / 越界类用编译器内置模板）|
| 用户显式 `exit(code)`（[#8.I]） | `_exit(code)` | 无（用户自己决定要不要 print；exit 不打 stderr） |

**不区分退出码（panic / error）**：与 [#5.A] 一致用 `1`；用户要区分，调 [#8.I] `exit(code)` 自定义。

**[#8.F.1] panic stack trace —— v1 不追加**

panic stderr 模板**仅** `panic: <msg>\n`，**不**追加调用栈 / 文件 + 行号 / 符号信息。

理由：
- yux 当前无调试基础设施：无 DWARF / PDB 嵌入、无符号表保留约定、无 stack walker。强写 stack trace 只能输出 raw 返回地址，对用户无用、误导性强。
- panic 触发频率低、且大多数场景用户已能从 msg + 大致位置定位（用户 `panic("...")` 自带上下文；越界 / null get 等编译器内置 panic 的 msg 可由编译器在 panic 现场附上 file:line —— 这是**不**走 stack walker 的低成本写法，是否落地 Phase 10 决，本草案不强制）。
- 留待调试基础设施成熟后追加；不阻塞 v1 错误模型落稿。

记 Open Issue O33。

**[#8.G] 显式不做 / 终态**

- ❌ panic 可恢复（任何形态的 catch panic / on-panic hook）。
- ❌ panic propagation 跨线程（v1 无并发模型，本议题不开）。
- ❌ `#[no_panic]` 类编译期可达性证明（验证某函数静态不会 panic）—— 工具链课题，不是错误模型课题；可后续。
- ❌ panic message 结构化 payload（仅 String；v1 不引入 panic enum / panic info struct）。

**[#8.I] 开放公开 `exit(code u32)` stdlib 函数**

```yux
#NoReturn
fn exit(code u32) { ... }    ; stdlib，路径 yux.core.exit
```

参数类型 = **u32**（与当前 yux 约定一致）。理由：负退出码无意义（POSIX 截到 0-255 unsigned；Windows ExitProcess 取 UINT）；类型即文档。i32 唯一好处是匹配 C `exit(int)` 原型，但本函数是 yux stdlib API，不绑 C ABI。

| 形态 | 行为 |
|---|---|
| `exit(0u)` / `exit(2u)` / `exit(N)` | 进程立即终止，退出码为 `code` |
| 调用方需要 `#Fallible(E)` 标注？ | ❌ 不需要 —— `exit` 是 `#NoReturn`，不返回，没有错误通道 |
| 是否走 atexit / 全局析构 / 缓冲刷新 | v1 **不**承诺；行为等价于 POSIX `_exit`（直接终止）。后续若引入 atexit / RAII 全局变量析构，再考虑分裂为 `exit` (clean) / `_exit` (immediate) 两档（参 POSIX 命名约定）。v1 仅一档。 |
| `exit` vs `panic` 怎么选 | `panic("msg")` = 这是 bug / 不变式破坏，stderr 自动打模板；`exit(code)` = 业务正常终止（用户主动选择退出码），不打模板。用户视场景选。 |
| `exit` vs `#Fallible` 让 main 错误退出 | `#Fallible` 适合"这个函数业务上可能失败、上层可能想 catch / try"；`exit(N)` 适合"已经决定立即终止、上层无 catch 必要"。两者并存、按场景选。 |

**与 `_exit` 内部名的关系**：本草案多处引用的 `_exit(1)`（[#5.A] / [#8.F] / 既有 §3.10.2）当前是 yux 编译器**内部**约定（`#CompilerInner`），用户不可调；本节 `exit(code)` 是**公开** stdlib 函数。实施层 `_exit(N)` 与 `exit(N)` 可共用同一底层 syscall（POSIX 上是 `_exit(2)` / Windows 上是 `ExitProcess`）；命名分裂仅为区分"内部触发 vs 用户主动"。落稿 Phase 10 时若两者实施完全等价，可考虑合并；本草案保留两个名字。

**[#8.H] 诊断码新增**

| 错误码 | 措辞模板 |
|---|---|
| **E7012** | `` `#NoReturn` function `{fn}` cannot declare a return type — remove the return type or remove `#NoReturn` `` |
| **E7013** | `` `#NoReturn` and `#Fallible({E})` are mutually exclusive on the same function — a non-returning function cannot also propagate errors `` |
| **E7014** | `` `#NoReturn` function `{fn}` may reach end of body — control flow must terminate via `panic`-class call, another `#NoReturn` call, or unconditional infinite loop `` |

E7012-E7014 进 [#5.B] 的 E7xxx 表，落稿 Phase 10 一并写到附录 D。

### Open Issues（Phase 8 收尾）

- O1 收口 ✅（[#8.C]）：`#NoReturn` 形态决；具体落 §3 / §11 哪一档由 Phase 10 落稿决。
- O3 收口 ✅（[#8.E]）：OOM = panic；可恢复 alloc API 推 stdlib。
- O5 收口 ✅（[#8.A]）：panic abort-only 显式重申；与 [#1] / [#4.H] 一致。
- O31（Phase 8 新增）✅ 撤销：不引入独立 `Never` 类型名。`#NoReturn` 调用点视为"流终止"（与 `ret` 同档），不参与表达式类型合并，复用 §4.9 / §3.10 既有 arm 排除机制。`#NoReturn` 函数签名仍写空 retType（无返回值）；永不返回是注解层属性、不是类型层属性。
- O32（Phase 8 新增）：`assert(cond, msg)` 的 stdlib 形态（参数顺序 / 是否支持 lazy msg）—— stdlib 课题，本草案不定。
- O33（Phase 8 新增）：panic stack trace / file:line 附加输出 —— v1 不做（[#8.F.1]）；待调试基础设施（DWARF / 符号保留 / stack walker）成熟后追加。
- O34（Phase 8 新增）：`exit` vs `_exit` 是否合并、是否拆 clean exit / immediate exit 两档 —— 取决于未来是否引入 atexit / RAII 全局析构。v1 单档（[#8.I]）。

---

### 决议 [#9] — defer / finally：v1 不引入

**[#9.A] v1 不引入 `defer` / `errdefer` / `finally` 关键字或块构造**

**取舍依据**：

| 场景 | 现状 |
|---|---|
| **既存 RC 资源**类型释放（File / Mutex / Socket / Box / Array / String）| RC 析构覆盖：作用域结束自动 release，错误路径同样走 RC 释放（[#6.B] Phase 10 实施）。`var f = open(...)`，`ret` / `ret Err::X` / catch arm `ret` 三条路径都能正确关。 |
| panic 路径上的清理 | **不**运行 —— panic = abort-only（[#8.A]）；引入 defer 也救不了，且会与 [#1] / O5 "不引入栈展开"基调冲突。析构在 panic 路径同样不执行（C++ 终止处理也是这模式）。defer ≈ 析构在这一点上**等价**，不是 defer 独有缺陷。 |
| Zig 风格 `errdefer`（仅错误路径清理 / 提交-或-回滚事务）| 用 try-catch + 显式 `match` 写：在成功路径调 `commit()`，在 catch arm 调 `rollback()`。代码长一点但**显式**，与 [#1] 基调一致。 |
| Java `finally` 释放非 RC 资源 | yux stdlib 立场是所有资源都 RC 化（裸句柄 / FD 由用户 wrap）。但**用户层是否能轻松写出自己的 Guard 类型**是另一回事，见下条。 |
| **用户自写 RAII Guard**（C++ ScopeGuard 模式：捕获局部变量 / 闭包，作用域退出运行任意代码） | v1 **写起来不简单**：需要自定义 struct + 实现析构（§3 析构机制）+ 把要捕获的局部变量"装进" Guard。后者牵涉 yux 的栈/堆 + `T&` / `Box` 借用-移动语义（与 Rust 借用复杂度相当）：捕获引用要管生命周期；捕获值要 move；要执行任意代码需要函数指针 / lambda（lambda v1 已落地，但闭包捕获 + 生命周期组合仍是钝点）。结果：**理论上可写，实务上门槛高**，不是"defer 的天然替代"。这一点 [#9 v0] 略过了，本节修正承认。 |
| 业务上的 "scope-exit 任意代码"（例：函数退出前打日志 / 累计性能计数器 / 还原全局状态）| v1 无简洁形态；用户在每条退出路径**手动重复**调用。代码冗余的代价存在，不假装没有。 |

**修正立场**：v1 仍**不引入** defer，但理由不再是"RC 析构通用替代"——而是 **"v1 不引入新关键字 / 新机制"** + **"承认有 ergonomic gap，留作未来重启触发点"**（[#9.E] 已记此重启条件，本次修订加强）。

**[#9.B] 用户面对"清理"场景的 v1 写法**

清理类需求 → 三档处理：

1. **RC 析构**（默认）：

   ```yux
   #Fallible(IoErr)
   fn read_first_line(path String) String {
     var f = open(path)         ; f 是 RC 类型，作用域结束自动 close
     var line = f.read_line()
     ret line                    ; ret / ret Err 都释放 f
   }
   ```

2. **错误专属清理（事务 / 回滚）** —— 显式 try-catch：

   ```yux
   #Fallible(TxErr)
   fn do_tx() i32 {
     var tx = db.begin()
     var n = try {
       step1(tx)
       step2(tx)
     } catch e StepErr {
       tx.rollback()             ; 错误路径回滚
       ret TxErr::StepFailed
     }
     tx.commit()                 ; 成功路径提交
     ret n
   }
   ```

3. **必须在所有路径执行的非清理副作用**（罕见）：v1 没有专用语法；用户用 (1) + (2) 组合或显式封装函数。本草案不为此场景立项。

**[#9.C] 与 RC release 顺序的关系**

[#6.B] 已把"错误路径 RC 释放时序"推到 Phase 10 实施层。本节**不**重复决；仅记约束：

- 错误路径与成功路径的 RC 释放顺序应**一致**（按声明逆序，作用域 LIFO）。
- 在 try-catch 块中：try block 内已声明的 RC 值进入 catch arm 时**必须已释放**（因为 try block 已退出）；catch arm 内新声明的 RC 值在 arm 退出时释放。
- 这两条是 [#6.B] 实施层的输入约束，本节仅记录，不变动 [#6.B] 推迟状态。

**[#9.D] 显式不做（终态 / 防滥用）**

- ❌ `defer { ... }` 块（Go 风格）。
- ❌ `errdefer` / `defer if error`（Zig 风格）。
- ❌ `try { ... } finally { ... }`（Java 风格；[#4.H] 既有约束已禁，本节复述）。
- ❌ 析构函数内可恢复错误 / 析构内 `#Fallible` —— [#1] 已禁，沿用。
- ❌ "panic 时跑 defer" 的可恢复 panic 形态 —— [#8.A] 已禁。

**[#9.E] 未来加入 defer 的触发条件（不立项，仅记录）**

将来若用户实践产生**反复出现**的痛点，重启该议题。触发条件（满足任一）：

- 出现至少一类无法 RC 化的资源；或
- 用户自写 RAII Guard 类型在 lambda + 借用-移动语义稳定后**仍**门槛高（典型证据：≥3 个常用清理模式被反复手写）；或
- try-catch 编排回滚类逻辑出现 ≥3 次相同模式且用户反馈强烈；或
- panic 模型放松（v1 不预承诺）。

**与析构的关系**：defer 与析构在"不允许抛出"这一点上**对等**（[#1] 析构禁抛 + [#9.A] panic 路径不跑都对两者成立）；defer 的真正价值在**降低 ergonomic 门槛**——不需要为每个清理动作定义 Guard struct + 析构 + 处理捕获的借用/移动。重启该议题时这是核心评估维度，不是"能不能跑、跑得对不对"。

### Open Issues（Phase 9 收尾）

- O4 收口 ✅（[#9.A] / [#9.D]）：errdefer 不做；用户走 try-catch 显式编排。
- O35（Phase 9 新增）：try-catch arm 内 `tx.rollback()` 类多步清理的"清理失败本身"如何处理 —— v1 立场：清理函数不应 `#Fallible`（清理失败 → panic 或忽略，由 stdlib 各类型自定）。具体规约推 stdlib 编码规约，本草案不强制。
- O36（Phase 9 新增）：用户自写 RAII Guard 的 ergonomic 评估 —— 在 lambda + `T&` / `Box` 借用-移动语义稳定后，跟踪用户实际写法；若反复手写相同清理模式 ≥3 次，视为 [#9.E] 重启触发点之一。这是"defer 是否需要"的关键证据，不是预先决定。

---

### 决议 [#10.A] — Phase 6.A 错误通道 ABI（草案，待用户裁决）

**问题**：`#Fallible(E) fn f(...) T` 落到 LLVM 时返回值如何承载"成功 T / 失败 E::V(payload?)"两种结局？10g codegen 全量依赖此决议。

**约束清单**：

1. 现有 `compileRetStatement` 全程走单返回值 `_builder.CreateRet(retVal)`；callee-clean / move-return / Nullable 包装等都假设 LLVM 函数有单一返回 SSA 值。改最小：保持单返回值。
2. yux 已有 `Nullable<T> = { i1 _has, T _value }` 的 IR pattern（见 `compiler_call.cpp:1223` / `compiler_expr.cpp:1819`）—— 这是 yux 现成的"和类型"承载方式，复用可省一套设计。
3. ErrEnum 本身是 yux 普通 enum，已有自己的 `{ disc, payload }` 落 IR 路径（payload 含 RC handle 时按 enum 既有协议处理）。错误通道 ABI **不应重新发明 enum 表示**。
4. T 可为 void（最常见的 fallible：副作用 + 可能失败）。
5. T 可为堆句柄 / 结构体 / 整型 / `T?` / `T&` / fn-value fat-ptr。任何形态都要能塞进同一个 ABI。
6. v1 单 target，无跨语言 ABI 承诺；但同一 yux 模块跨 .obj 调用必须二进制一致 → ABI 由 callee retType + `#Fallible(E)` 注解唯一决定（声明侧已有 `FnSymbolInfo.fallibleErrType`）。
7. extern fn 禁 `#Fallible`（[#7]）—— FFI 边界不参与本决议。

**候选方案**：

**候选 A — anonymous struct 单返回值 `{ i1 isErr, T_ok, ErrEnum }`**

- T 非 void：`ret_type = { i1, T_ok, ErrEnum }`；T = void：`ret_type = { i1, ErrEnum }`。
- 成功路径：caller `extractvalue %r, 0` 取 i1 → 0 分支 `extractvalue %r, 1` 取 T_ok。
- 失败路径：caller `extractvalue %r, 0` 取 i1 → 1 分支 `extractvalue %r, 2` 取 ErrEnum，按 `!` 透传 / try-catch 路由 / match 解析。
- callee `ret expr`（成功）：构 `{ false, expr, undef/zero }`；`ret E::V(payload)`：构 `{ true, undef/zero, enumVal }`。
- LLVM 自动决定是寄存器返回还是 sret（小 struct 走多寄存器，大 struct LLVM 自动 sret 改 IR）—— 无需手写 sret 逻辑。

优点：
- 与 Nullable<T> 同形（i1 tag + payload），zero 套语义模型新增。
- 单 CreateRet 路径，`compileRetStatement` 改动局部。
- ErrEnum 复用现成 enum codegen，不污染用户 enum（不引入合成 OK variant）。
- void 成功通道自然退化。
- caller 端只是两次 `extractvalue` + 一次 `br i1`，IR 量可控。

代价：
- T_ok 与 ErrEnum 同时占空间（即使每次只用一个）；典型尺寸 16-32 字节，对栈 / 寄存器返回可接受；大 T_ok（含大 struct）由 LLVM 自动转 sret。
- ErrEnum payload 是 RC handle 时，错误分支 extract 后须按 enum 既有 retain 协议处理（与现状一致，无新规则）。

**候选 B — sret + i1 标量**

- callee 签名 `i1 @f(T_ok* sret %slot, ErrEnum* sret %errSlot, args)`（双 sret）或合并为 `{T_ok, ErrEnum}* sret`。
- 拒因：需要双 sret 槽位才能容纳两类 payload；caller 必须为两槽 alloca + load + branch；T = void 时退化复杂；与现有 `compileRetStatement` 的"单 SSA 返回值"假设强绑定决裂。代价 vs 收益不划算 → **不采纳**。

**候选 C — ErrEnum 内嵌 OK variant（双返回 `{T_ok, ErrEnum}` 无 i1）**

- ErrEnum 编译期合成 `_Ok` 0 号 variant，i1 由 ErrEnum 的 disc == 0 替代。
- 拒因：(1) 污染用户 enum 的 disc 编号空间，破坏 §A.5 / 附录 D 关于 enum disc 稳定性的承诺；(2) 用户在 `match` / 反射场景可见到合成 variant，违反"`#Fallible` 是签名层概念"原则；(3) 跨函数共用同一 E 时，合成 variant 注入时机与 enum 声明位置错配，IR 难以单态化。**不采纳**。

**推荐：候选 A**

**实施轮廓（informative，10g 落 IR 时按此走，不写进 spec）**：

1. `getLLVMType` 旁挂 `getFallibleReturnType(retType, errType)` → `{ i1, T_ok?, ErrEnum }`。`compileFn` 入口若 `fnInfo.fallibleErrType` 非空，函数 LLVM 返回类型替换为该 struct。
2. `compileRetStatement`：在 `_currentFnNode` 是 `#Fallible` 时分流——
   - `expr` 类型与 declRetType 匹配（成功）：构 struct `{false, expr_val, zero}`，CreateRet。
   - `expr` 类型 == ErrEnum 或 ErrEnum 的 variant 构造（失败）：构 struct `{true, zero, expr_val}`，CreateRet。
   - 既不匹配成功也不匹配错误 → 复用现有 E3020。
3. `compileCallExpr`（callee 是 `#Fallible`）：拿到 struct 后按调用上下文分流——
   - 顶层 `caller!` + 上游 `#Fallible` 同类型：i1=1 → 把 ErrEnum 字段塞进**当前 fn 的**错误返回 struct，CreateRet（透传）；i1=0 → extract T_ok 继续。
   - 顶层 `caller()` 在 try-catch 内（`_tryCatchStack` 非空，10f 已有上下文）：i1=1 → 跳到该 catch arm 的 entry block，把 ErrEnum 绑定符号设为 extract 出的值；i1=0 → extract T_ok 继续。
   - 其余形态被 10e/10f 静态层拦掉，不应走到 IR。
4. main 出口：若 `main` 标 `#Fallible(E)`（[#5] 允许），编译器在 `_yux_main_wrapper` 里 extract i1 → 1 分支调 stdlib 诊断打印 + `_exit(1)`；i1=0 分支按现有 i32 退出码路径。无需修改用户 main 体的 IR。

**RC / 析构（preview Phase 6.B / 6.C，本决议**只列与 ABI 形态强耦合的部分**，详细规则推 [#10.B] / [#10.C]）**：

- 成功分支：T_ok 字段复用现有 move-return RC 协议（堆句柄 +1）；ErrEnum 字段为 `zeroinitializer`，无 RC。
- 失败分支：ErrEnum payload 含 RC handle 时按 enum variant 构造路径已 +1（与 `MyEnum::V(box)` 现状一致）；T_ok 字段为 `zeroinitializer`，无 RC。
- 局部变量 / 形参 release：`compileRetStatement` 的 `callDestructorsForScope()` 在两条分支都不变，构 struct 与 release 顺序不耦合。
- ErrEnum payload 在 caller 端透传时 = "把同一个 ErrEnum 值从 callee 返回 struct 移到 caller 返回 struct" → 不 retain 不 release（move 语义，与函数返回堆句柄的 move-return 同档）。

**Open Issues（10.A 收尾）**：

- O38（10.A 新增）：`{ i1, T_ok, ErrEnum }` 当 T_ok 是大 struct 时 LLVM 自动 sret 是否引入额外 alloca / memcpy 开销？落 IR 后用 `--emit-ir` 看一眼，若热路径出现 `memcpy + alloca` 链，10g 后期再决定是否手工 sret。**v1 不预先优化**。
- O39（10.A 新增）：`#Fallible(E)` 用户函数被作为 fn-value 传递（赋给 `var f fn(T) T`）—— v1 fn-value 类型语法不承载 `#Fallible`（[#7] 推迟），所以根本写不出来；不入本决议，10g 也不实现。
- O40（10.A 新增）：跨模块调用 `#Fallible` 函数，签名变更需要重新 codegen —— 10g-1 校核结果：现有 `pkg_cache` 只 hash 编译器指纹 + 各源文件 mtime+size，**完全不**做函数签名 hash。任何 fn 签名变更（retType / params / 注解 / `#Fallible`）都会让 caller 模块 cache 误命中——这是**编译器级既存限制**，不是 `#Fallible` 引入的新问题。workaround 同既有：改签名后手工 `rm -rf build/<name>.exe build/src/`。**签名 hash 化推独立工程，不入 10g**。

---

### 决议 [#10.B] — Phase 6.B 错误返回路径的 RC 释放时序（草案）

**问题**：`#Fallible(E)` 函数体内执行 `ret E::V(payload)` 提前返回时，局部变量 / 形参 / 借用 / Box 等的 release 与"构造错误返回 struct"的相对顺序如何决？10.A 的 ABI 形态确定后，时序是 codegen 的下一个必决点。

**约束清单**：

1. spec §5.7.2 既定析构序：`ret` 在 CreateRet 前调 `callDestructorsForScope()` 释放当前作用域内所有局部变量（含形参，按声明逆序）。错误返回不应破坏这一统一序。
2. 10.A 候选 A 的成功通道沿用现有 move-return 协议：`ret expr`（成功）—— `expr` 求值时若是堆句柄会 +1（move-return retain），随后 destructor scope 释放本地原句柄，净 +1 移交 caller。错误通道应当同形：失败 payload 在变量值阶段 +1，destructor 阶段 -1，净 +1 移交。
3. ErrEnum 本身是普通 enum 值，构造时按现有 enum 协议：variant payload 若是堆句柄已自动 +1（与 `MyEnum::V(box)` 现状一致）。错误返回不应叠加额外 retain。
4. callDestructorsForScope 不区分成功 / 失败路径——同一作用域，同一释放表。

**决议**：

错误返回 `ret E::V(payload)` 在 IR 上严格按下面的时序展开（由 `compileRetStatement` 实施）：

| 步骤 | 动作 | 说明 |
|---|---|---|
| 1 | 求值 `E::V(payload)` | 走现有 enum variant 构造路径；payload 是堆句柄时 enum 构造代码已自动 retain（净 +1 落入 ErrEnum 值），与成功路径的 move-return retain **同档**。 |
| 2 | 求值结果存入 SSA 寄存器 `%errVal` | 不写回 alloca。 |
| 3 | 构造返回 struct `{ true, zero(T_ok), %errVal }` | T_ok 字段填 `zeroinitializer`（Nullable / Box 等堆句柄类型 zero = null pointer，无 RC）。 |
| 4 | `popAndReleaseTempFrame()` + `callDestructorsForScope()` | 释放本作用域内所有局部 / 形参 / 表达式临时；与现有 `ret expr`（成功路径）调用顺序**完全一致**。`%errVal` 已脱离作用域寄存器集（步骤 1-3 已落 SSA），不在析构表内，不会被误释放。 |
| 5 | `_builder.CreateRet(retStruct)` | 单返回值。 |

**与成功通道对照**：

| 路径 | 步骤 1 | 步骤 3 | 步骤 4 |
|---|---|---|---|
| `ret expr`（成功） | 求值 `expr`，move-return retain → `%okVal`（净 +1） | 构 `{false, %okVal, zero(ErrEnum)}` | 同 |
| `ret E::V(p)`（失败） | 求值 `E::V(p)`，enum 构造 retain → `%errVal`（净 +1） | 构 `{true, zero(T_ok), %errVal}` | 同 |

**两条路径共用同一段 destructor 调用**——`callDestructorsForScope()` 已经按声明逆序释放当前作用域，**不需要为错误路径额外区分**。

**关键不变量**：

- **U1 析构序统一**：成功 / 失败两条 `ret` 路径在 IR 上共享同一个 `callDestructorsForScope()` 调用点（在构 retStruct 之后、CreateRet 之前），不分流。
- **U2 净 +1 移交**：无论成功还是失败，被返回的"payload 值"（T_ok 或 ErrEnum）净流出 callee +1，由 caller 接管（caller-side 在 `compileCallExpr` 错误分流时遵循 [#10.C]）。
- **U3 未持值字段不参与 RC**：`zeroinitializer` 写入的字段（成功路径的 ErrEnum、失败路径的 T_ok）在 IR 层被 caller 视为"不存在的值"，caller 按 i1 分流后**不**对未走的字段做 retain / release。

**借用 / 引用 / receiver 形参**：

- `T&` 形参 / `$` receiver：**不参与 retain / release**（[#1] 既有规则）；析构表本就不含它们。错误路径 = 成功路径，无新规。
- `Box<T>` 形参：caller 已 +1（§6.5.1.1），callee 析构表会 -1，净 0；错误返回不影响该规则。
- 嵌套作用域中的 ret：与现状一致——`callDestructorsForScope()` 已按 spec §5.7.2 沿作用域链向上释放至 fn 顶层，错误 ret 不破坏该路径。

**多层嵌套的错误 ret 示例**（informative）：

```yux
#Fallible(MyErr)
fn f() i32 {
  var a = make_box()           ; +1, scope=fn
  if cond {
    var b = make_string()      ; +1, scope=if
    ret MyErr::Bad(b)          ; b 进 ErrEnum payload (+1)，然后 callDestructorsForScope 沿
                               ; if-scope → fn-scope 逆序释放：b -1（净进 enum 0），a -1（净 0）
  }
  ret a.value                  ; 成功路径，a move-return retain，析构 a
}
```

`b` 在错误路径的 RC 平衡：
1. `make_string()` → +1 落入 `b`
2. `MyErr::Bad(b)` enum 构造 retain `b` → ErrEnum value 含 payload +1
3. `callDestructorsForScope()` if-scope release `b` → -1
4. 净效果：`b` 的原 +1 被消掉，ErrEnum value 持有独立 +1 移交 caller。**无 use-after-free，无泄漏**。

`a` 同理：错误路径不动 `a`，但 fn-scope 析构表仍包含 `a`，在 step 4 中按声明逆序释放。

**实施备注**：

- `compileRetStatement` 现有错误检查 E3020（"成功通道类型不符"）需要在 `_currentFnNode` 标 `#Fallible` 时分流：先匹配 declRetType（成功），再匹配 fallibleErrType（失败），都不匹配才 E3020。否则现状会把合法的 `ret MyErr::Bad(b)` 误判 E3020（10e 备注已记录该已知行为）。
- `_currentFnNode` 没有 `#Fallible` 时（普通 fn），现有路径完全不变，新逻辑零侵入。
- `try-catch` 内部的 `ret E::V`（即在 try block 中提前抛错）—— try block 当前 IR 上还是普通 block（10f 占位），所以 `ret` 仍然走"当前 fn 的错误返回"，与 try-catch 路由**无关**；这与 spec §5.5 / [#5.5] 一致：catch arm 内的 `ret` 跳出整个 fn，不参与 try 表达式合并。

**Open Issues（10.B 收尾）**：

- O41（10.B 新增）：嵌套 `try-catch` 内的 catch arm body 末尾"不带 ret 的隐式产值"—— 若 catch arm 求值结果是堆句柄类型（参与 try-catch 表达式合并），caller 端如何接管 +1？落 10.C；本决议只覆盖 fn 边界的 ret 时序。
- O42（10.B 新增）：`#Fallible` fn 含 `defer`-like 提前清理—— v1 [#9] 已拒 defer，故无此场景；若未来重启 [#9.E]，需要重审本决议中"析构序统一"是否仍成立。**本决议不预设**。

---

### 决议 [#10.C] — Phase 6.C 错误 enum payload 的 RC 协议（草案）

**问题**：错误 enum `E::V(payload)` 的 payload 含堆句柄（String / Box<T> / Array<T> / 嵌套 enum 等）时，跨函数边界的"caller 接管 / move 透传 / catch 绑定"各自的 retain / release 协议如何决？

**约束清单**：

1. yux 既有 enum payload 规则：variant 构造时 payload retain（与 struct 字段构造同档）；enum 值整体 release 时按各 variant 的 active 判断 payload release。`#Fallible` 不应改写 enum 自身的 RC 协议。
2. ErrEnum 在 #Fallible 通道中"路过 caller"的形态有三种：
   - **透传**（`call()!` + caller `#Fallible` 同类型）：caller 不持有 ErrEnum 临时变量，直接把 callee 返回 struct 的 ErrEnum 字段塞进自己的错误返回 struct。
   - **catch 绑定**（`try { call() } catch e E { ... }`）：caller 在 catch arm 内有名为 `e` 的局部，类型 = E，作用域 = catch arm。
   - **wrap-rethrow**（catch arm 内 `ret OuterErr::Wrap(e)`）：`e` 作为 payload 进入外层 ErrEnum，与普通 enum 构造同形。
3. 成功 T_ok 透传：caller 调 `f()!` 在成功通道时 extract T_ok → 移交 caller 表达式上下文。规则与现有"调用返堆句柄"（move-return）同档，不需要新规则。
4. 现有 enum copy 路径在 `compiler_*.cpp` 已实现——本决议**只规定调用边界处的 RC 动作**，不重写 enum 内部 codegen。

**决议**：

按"路过形态"逐条规定 caller 端的 RC 动作：

#### 1. **透传**（`call()!`，外层 fn 标 `#Fallible(E)` 同类型）

```
%r = call ret_struct @callee()           ; callee 已构 {true, zero, %errVal}, %errVal 持 +1
%isErr = extractvalue %r, 0
br i1 %isErr, %errBB, %okBB

errBB:
  %errVal = extractvalue %r, 2           ; +1 from callee 现在挂在 %errVal SSA 上
  %outerRet = insertvalue {true, zero, %errVal}
  ; 不调 retain；不调 release
  callDestructorsForScope()              ; 当前 fn 作用域析构表（不含 %errVal）
  ret %outerRet                          ; %errVal 的 +1 现在挂在 %outerRet 字段上
```

**关键**：`%errVal` 是**纯 SSA 临时**，从未进入任何作用域析构表。其 +1 计数从 callee 返回点起就始终唯一，经 `extractvalue` → `insertvalue` 两次复制 SSA 句柄但**不动 RC**（heap block 仍是同一块，refcount 不变）。最终塞进外层返回 struct，**净 +1 移交外层 caller**。

#### 2. **catch 绑定**（`try { ... call() ... } catch e E { BODY }`）

```
%r = call ret_struct @callee()           ; %errVal 持 +1
%isErr = extractvalue %r, 0
br i1 %isErr, %catchBB, %okBB

catchBB:
  %errVal = extractvalue %r, 2
  store %errVal, %eAlloca                ; 绑定到 catch arm 的局部 `e`
  ; 不调 retain（直接把 +1 从 SSA 转给 alloca 持有）
  ; e 进入 catch arm 的析构表：scope-end 时 -1
  ... compile BODY ...
```

**关键**：`e` 是个普通局部变量，**它的 RC 生命周期与任何其他 enum 局部完全一致**。catch arm scope 的 `callDestructorsForScope()` 在 arm 结束时 -1。`%errVal` 的 +1 在 `store` 时所有权转移到 `%eAlloca`，无需额外 retain。

**与 BODY 内对 `e` 的使用**：和现状"普通局部 enum"完全同——`e.disc` 读 disc、`match e { ... }` 解 payload、`other_call(e)` 传给函数（caller-side 现有 retain 协议处理）等。

#### 3. **wrap-rethrow**（catch arm 内 `ret OuterErr::Wrap(e)`）

`OuterErr::Wrap(e)` 走 enum 既有构造路径：variant 构造时 retain `e` → `%outerErr` payload 持独立 +1；catch arm scope 析构释放 `e`（-1）；净效果：`e` 的引用从 `%eAlloca` 转移到 `%outerErr` payload。再由 `ret` 走 [#10.B] 错误返回路径塞进 fn 返回 struct。**无新规则**，是 [#10.B] + 现有 enum 构造的复合。

#### 4. **catch arm 不绑定 e**（仅 match 形态，arm 末表达式产值）

```yux
var n = try { f() } catch e E { 0 }      ; e 未被 BODY 使用
```

`e` 仍是 catch arm 的局部，仍在 arm 析构表内 → scope-end 时 -1。**完整 RC 平衡**，无需特例。BODY 末值 `0` 与 `n` 类型合并按 try-catch 表达式合并规则处理。

#### 5. **成功值 T_ok 透传**（`call()!` 成功分支）

```
okBB:
  %okVal = extractvalue %r, 1            ; +1 from callee move-return
  ; %okVal 现在与普通"调用返堆句柄"等价；落入表达式上下文
  ; 后续按现有"call 返回值"路径处理（绑定到 var / 传给函数 / RC 表达式临时帧）
```

**与现有 `var x = call()` 完全同**——move-return retain 协议已经处理 +1 的去向。**无新规则**。

**关键不变量**：

- **U4 SSA 临时不入析构表**：调用边界处用 `extractvalue` 取出的 ErrEnum / T_ok SSA 值，**不走** alloca / 析构表，其 +1 计数唯一存在于 SSA chain 上，直到塞进下一个持久存储（外层返回 struct / catch 局部 alloca / 外层 enum payload）。
- **U5 持久化 = 转移所有权**：`store SSA, alloca` 与 `insertvalue { ..., SSA, ... }` 都是**所有权转移**操作，**不调** retain（避免双 +1）。这与现有 yux RC 协议已有的 move 语义一致（[#1] / spec §6.3.2.1 move-return）。
- **U6 catch 局部 = 普通 enum 局部**：catch arm 的 `e` 在 RC / 析构 / match / 传参等所有场景**与用户写 `var e = make_err()` 完全等价**，无任何特殊待遇。

**panic / 异常路径**：

- `#Fallible` 通道与 panic 完全分离（[#1] / [#9.A]）—— `RaiseException` 路径**不跑** yux 析构（无栈展开），所以 ErrEnum payload 在 panic 时可能泄漏。这是 [#9] 的既定立场，**本决议不补**。10g codegen 不需要为 panic 路径加任何错误通道清理逻辑。

**与 §6.5.1（callee-clean）的一致性**：

- 错误通道形参（callee 的 ErrEnum 局部、catch arm 的 `e`）走 callee-clean —— 形参/局部在作用域结束时释放。
- 错误通道返回值（callee 的 ErrEnum 字段、caller 的 catch 绑定）走 move-return —— 净 +1 移交，由 caller 接管。
- 两条 ABI 协议在错误通道与成功通道**完全对称**，错误通道不引入新协议。

**实施备注**：

- `compileCallExpr` 错误分流处（10.A 实施轮廓步骤 3）需要按上述 1 / 2 / 5 三种形态各自生成 IR；catch arm 的 `eAlloca` 应当在 `visitCatchArm` push scope 时已分配（10f 已经在 `CatchArmNode` 上挂 binding 符号，IR 层只需在 catch entry block 第一指令处补 alloca）。
- enum variant 构造代码（用于 wrap-rethrow 情形）已存在于 `compiler_call.cpp` 的 enum constructor 路径，**复用**即可。
- 不需要为错误通道新增 runtime helper / SDK API。所有 RC 动作复用现有 retain / release 调用点。

**Open Issues（10.C 收尾）**：

- O43（10.C 新增）：try-catch 表达式合并（`var n = try { ... } catch e E { other }`）当 try block 末值 / catch arm 末值都是堆句柄时，IR 上需要在 join block 处对两条 BB 的 phi 结果做 RC 处理—— 当前 if-else / match 的表达式合并路径已有此处理（应该是 phi + 不重复 retain），10g 实施时复用同一套 helper；若发现 helper 不通用，再开 O43 子项。**v1 假设可复用**。
- O44（10.C 新增）：`#Fallible` 函数内部的 `_tryCatchStack` 嵌套（外层 try 包内层 try）—— RC 协议本身不变（每个 catch arm 局部 `e` 独立），但路由 IR 需要按"最近 try"分流；这是 IR 实施细节而非协议决议，记 codegen 阶段处理。

---

### 复审 [#R] — 整体过一遍（命名 / 易用 / 滥用）

走完 [#1]-[#9] 后整体扫一遍可能绊脚的地方，逐项裁决。

**[#R.1] `#Throw` → `#Fallible` 改名 ✅ 已生效**

理由：错误是值返回（`ret E::V`），`Throw` 暗示栈展开，与 [#1] 立场冲突。`Fallible(E)` 读作"该 fn 可能以 E 失败"，语义直白。文件全量替换 97 处完成（手工）。所有 E7xxx 措辞、[#3] / [#4.B] / [#4.H] / [#5] / [#6] / [#9] 等引用同步切换。

**[#R.2] `try` / `catch` 关键字保留**

虽然 try / catch 在 Java/C++ 语境里绑定异常机制，但 Zig 在值返回模型下沿用 try-catch 已被业界接受；替换（`attempt` / `handle` / `recover`）成本远大于收益。保留，并在用户文档明确"yux 的 try-catch 是错误路由块、不是异常处理"。

**[#R.3] 多类错误聚合 = 单错误 enum + 嵌套 / 未来 generic enum**

确认 [#2.B] 立场：v1 单 `#Fallible(E)`；多类错误用嵌套 enum（`enum ProcessErr { Io(IoErr), Parse(ParseErr) }`）+ try-catch wrap 写出。

```yux
#Fallible(ProcessErr)
fn process(path String) i32 {
  var s = try { read_file(path) } catch e IoErr { ret ProcessErr::Io(e) }
  ret try { parse_int(s) } catch e ParseErr { ret ProcessErr::Parse(e) }
}
```

verbosity 是有意代价（避 Java `throws E1, E2, E3` 爆炸）。**未来 generic enum 落地后** wrapper enum 写法变轻；嵌套通常已够用，本草案不预承诺糖。落稿章节加一段"为什么单 `#Fallible(E)`"说明权衡。

**[#R.4] `var n = try {} catch {ret X}` 跳赋值 / `var n = if {} else {ret X}` 同问题**

不是 try-catch 独有：if / match 表达式同样存在"某 arm 以 ret 跳出，绑定不赋值"。yux `if` / `match` / `try-catch` 三者表达式形态行为一致；catch arm 以 `ret`（或 `#NoReturn` 调用）结尾时该路径流终止，不参与 try 表达式类型合并。落稿示例旁注 "catch / else / arm 以 ret / panic 结尾时本路径不赋值；流终止"。

**[#R.5] 单行 `if-else` 是否加 elif** —— **不加**

§4.9 现有三档：
- §4.9.1 单行 `if c {a} else {b}` —— 二分（替代 C 三元 `?:`）
- §4.9.2 Python 风 `a if c else b` —— 二分
- §4.9.3 块形 `if {} elif {} else {}` —— 多分支已含 elif

二分场景常见，单行保持窄；多分支用块形（已支持 elif）。加单行 elif 会扩语法面、模糊"单行=二分"心智模型，且收益低（二分独占 80%+ 场景）。**保留现状**，错误模型草案不动 §4.9。

**[#R.6] 后缀 `!` 优先级**

后缀 `!` 加在 `exprCall` 同档（与 method call / `[]` 索引同级，**最高一档**）；优先级表：

```
exprCall / exprIndex / exprPostfixExcl     ; 最高
... method / field ...
... arithmetic / comparison ...
exprCoalesce ??                            ; 最低（条件类）
```

例：`call() ?? -1` —— `()` 后无 `!`，纯调用；`call()! ?? -1` —— `call()` 错误透传后 nullable 与 -1 折叠。两种意图都能写。

[#5.E] 同步表添加："后缀 `!` 优先级 = `exprCall` / index 同档；高于一切二元 / 三元 / coalesce"。

**[#R.7] catch arm 直接 panic 给 E7018 警告 ✅ 已加**

加在 [#4.H] 规则表 + [#5.B]（落稿时）。措辞：`panic in catch arm converts recoverable error to abort — consider ret with error variant or exit(code) if termination is intended`。可 suppress。

**[#R.8] 编译器内置注解的命名空间防撞 → 不需立项**

用户确认：注解未来 = 结构体（代码生成代码用），**仅构建时**生效。不进入运行时类型系统，与 fn / type 名空间隔离；用户不会因"定义自己的 #Throw"造成运行时 / 类型系统冲突。命名空间防撞回归 §11 注解机制设计课题（不是错误模型课题）。**O37 不立**。

**[#R.9] panic 实施层 = 复用 §11.3.5.3 SEH 路径**

§11.3.5.3 现有 `_yux_test_assert_failed()` 调 Win32 `RaiseException(0xE0FA17ED)`：
- `#Test` 模式有 SEH wrapper 接，FAIL 标记后继续下个测试
- 普通 `yux build` 无 SEH wrapper，未处理 RaiseException → 进程终止

panic 实施可走同一机制（不必新加路径）：
- `panic(msg)` → 调内部 `_yux_panic_failed(msg)` → `RaiseException(<panic_code>)`（与 assert 用的 SEH exception code 可同可不同；Phase 10 实施决）
- `#Test` 内调用普通函数触发 panic → SEH wrapper 接住，与 assert 失败一致归为 FAIL（[#8.D] 的"#Test 内 panic 由 SEH 接"自然落实）
- 普通 build → 进程终止 = `_exit(<code>)` 等价

**与 [#1] / O5 "不引入 unwind"的关系**：SEH RaiseException 在普通 build 模式下**没有匹配 handler**——OS 直接终止进程，不走任何 IR 层 cleanup landingpad / DWARF personality / 用户可见 catch。`#Test` 模式的 SEH wrapper 是 `yux test` runner 私有机制，不是用户可写的 catch；不暴露给用户语义，故不违反"无 unwind"立场。

落稿在 [#8] 末尾加实施 note 即可，本节不改决议。

**[#R.10] `exit(0u)` ergonomic ✅ 用户确认 OK**

yux 已支持无后缀整数字面量类型推断（"还有点 bug"，非本草案课题）；`exit(0)` / `exit(1)` 直接写即可。[#8.I] 不动；u32 形参保留。

### 复审 Open Issues 收尾

- O37 不立 ✅（[#R.8]）：命名空间防撞回归 §11 注解课题。
- 其他 O1-O36 已在各 Phase 收口；落稿时统一过一遍措辞，本节不重列。

整体复审结论：**草案 [#1]-[#9] 站得住，可推 Phase 10 落稿**。改名 `#Fallible` 已生效；E7018 已加；命名 / 优先级 / 多错误聚合写法 / panic 实施路径全部明确。剩余 ergonomic gap（[#R.3] verbosity / [#9] defer）记 Open Issue 留未来证据驱动重启。

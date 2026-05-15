# const-mut P1 实施日志

本文件归档 const-mut 体系 P1 的实施记录：核心决策、检查规则形态、关键代码点。是后续回答"为什么 `#Const fn` 不做推断"、"`#Frozen` 形参为什么不能传给非 `#Frozen` 受方"等问题的事实来源。

- 规范条款见 `docs/spec/05-语句与控制流.md` §5.1.5、`docs/spec/06-函数.md` §6.1.2a / §6.2.2a、`docs/spec/07-结构体.md` §7.1.4、`docs/spec/11-编译期注解.md` §11.6 / §11.7 / §11.8
- 错误码段见 `docs/spec/附录D-诊断.md` E3104–E3111
- 设计草案（已落地，可归档）：`docs/spec/draft/DRAFT-const-mut.md`
- 与本日志无关的待修 bug 见 `BUGS.md`（本地）

---

## Phase 0 — 起点摸底

接到任务时已落地的现状（非本轮新增，仅记录原态）：

- §3.2 `val` 重赋拒收（`compiler_stmt.cpp` 走 `E3093`）。
- §3.4 参数默认 `val`：`ast_builder.cpp` 注册 param 时不传 `writeable`，`SymbolInfo` 默认 `w=false`；param 重赋走同一条 `E3093`。
- AST 层 `DeclareType::CVal` 预埋（`statement_node.h` + `ast_builder.cpp` 三处死分支），等 G1 改 g4 把 `cval` 头打开。

缺口：G1 / G2 / G3 grammar 改动未做；`#Const fn` / `#Frozen` / `#Val` 任何注解的语义都未实现；const-mut checker 文件不存在。

## Phase 1 — 局部 `cval` 落地

G1 grammar 改动：`yuxParser.g4` `statement` 加 `Cval name=ID typeWithRef SymbolEq expr LineEnd #statementCvalDeclAssign` 分支（按用户决定走 `typeWithRef` 而非草案 §3.1 的 `type`，与 `val` / `var` 形态对齐）。`Cval` token 早已在 lexer 存在（`yuxLexer.g4`），无需新增。

`ast_builder.cpp` 加 `visitStatementCvalDeclAssign`：复用 `buildTypeWithRef` + `StatementDeclareAssignNode`（`DeclareType::CVal`），符号登记 `writeable=false`。codegen 不区分 declType，cval 局部走与 val 同一路径，重赋自动落 `E3093`。

§3.3 初值约束（基本类型 / 字面量算术 / 已声明 cval 引用）此阶段未做，留到 Phase 2 const-mut checker。

## Phase 2 — const-mut checker 骨架 + 局部 cval 初值约束

新文件 `src/analyzer/const_mut_checker.{h,cpp}` 承载 §3.3 局部 `cval` 初值约束（E3104）。

- `SymbolInfo` 加 `isConst` 位，在 `ast_builder.cpp` 全局 `globalConst` 与局部 `cval` 注册点置位。
- Checker 递归走 fn body（含 if / loop / match / lambda / try-catch 嵌套块），命中 `StatementDeclareAssignNode{declareType==CVal}` 则对 RHS 跑常量表达式校验。
- 允许：字面量（int / float / bool / null / string / codepoint）、对 `isConst` 符号的引用、一元 / 加减 / 乘除模 / 位 / 比较 / 逻辑 / 括号的递归组合。其它形态（call / dot / get / array / tuple / if-else 等）一律 `E3104`。
- Compiler 在 `compileFn` / `compileMethod` 入口、`checkBorrows` 与 `checkFlowTerminate` 之间调用 `checkConstMut`。

**未做**：`yux-check`（SemaPass）镜像未补；与 borrow checker 当前态一致。

## Phase 3 — `#Frozen` 形参注解（G3 方向 B）

G3 grammar 改动：`yuxParser.g4` 新增 `paramAnno: SymbolHash name=ID LineEnd?`，`fnParamStd` / `fnParamGroup` 前置 `(paramAnnos+=paramAnno)*`，inline 与换行两种形态都支持；group 形态注解共享给组内所有名字（与共享 type 对齐）。

- `FnParamNode` 加 `isFrozen` 位；`ast_builder.cpp` `visitFnParamStd / Group` 用 helper `readParamAnnos` 解析注解，未知注解抛 `E3105`。
- `SymbolInfo` 加 `isFrozen` 位，param 注册点同步置位；frozen 参数仍走默认 `writeable=false` → §5.2 重赋自动落 `E3093`。
- `const_mut_checker.cpp` 扩展：
  - §5.3 `s.f = ...`（`StatementAssignNode` `subs` 非空）/ `s[i] = ...`（`StatementSetNode`）的 `obj` 是 frozen → `E3106`；
  - §5.4 可写槽位（局部 var/val 声明 / 已声明可写局部重赋）承接 frozen 表达式 → `E3107`；
  - `copy_of:<T>(...)` 调用识别为脱 const 出口（不视为 carriesFrozen）。

Frozen 携带判定 P1-3 简化为"顶层 ID / paren 包裹 ID"；callsite "frozen 实参传给非 frozen 形参"留 follow-up（需 callee 形参表）。

## Phase 4 — `#Val` / `#Frozen` 字段注解（G2）

G2 grammar 改动：`yuxParser.g4` `filedDecl` 接 `(buildAnnos+=buildAnno)*`（`buildAnno` 自带 `LineEnd`，落在字段头上一行）。

- `StructFieldNode` 加 `isVal` / `isFrozen` 互斥位；`ast_builder.cpp` `visitFiledDecl` 用 helper `readFieldAnnos` 解析，未知 / 同时 `#Val` + `#Frozen` / 带实参糖均抛 `E3108`。
- `const_mut_checker.cpp` 加 §6.2 / §6.3 字段写白名单：
  - 走 `resolveFnContext` 拿 enclosing `FileNode` + `StructImplNode` 判定 constructor（`fn StructName(...)`）/ destructor（`fn ~()`）；
  - `obj.f = ...` 时按 `$ → Ref<Self>` 剥皮取类型，跨模块 `getStructOwner` 找 StructDecl；
  - `subs.size()==1` 时 `#Val` / `#Frozen` 均拒，`subs.size()>1` 时仅 `#Frozen` 拒（深传染），构造期内放行；析构期跳过整套检查；其它 fn / 自由函数全程拒。新错码 `E3109`。

**仍未覆盖**：(i) `StatementSetNode`（`obj[i] = ...`）的字段链拒收；(ii) 字段深链第二跳起的 `#Val` / `#Frozen` 解析（要逐级类型推断）；(iii) §6.4 含 `#Frozen` 字段类型的 callsite 传递约束，与 P1-3-followup 合并到后续 callsite 阶段。

## Phase 5 — `#Const fn`

`#Const` 加入 `ast_builder.cpp` 顶部 `knownAnnos` 白名单（零 g4 改动）。`FnSymbolInfo` 加 `isConst` 位，fn / 方法注册点从 header 注解读取。

`const_mut_checker.cpp` `ConstMutWalker` 加 `_isConstFn` / `_fnName` / `_localNames`：

- visitStmt 命中 `StatementDeclareNode` / `StatementDeclareAssignNode` 把名字塞入 `_localNames`；
- 写操作走 helper `checkConstFnWrite`（`StatementAssignNode`）/ `checkConstFnSet`（`StatementSetNode`），lhs 命中 `_localNames` 放行，否则按 `$` / 全局（带 moduleName）/ 参数（无 moduleName）分支抛 `E3110`；
- 调用点走 `checkConstFnCall`，识别两类 callee 形态：
  - (a) 自由函数 = `LiteralObjNode` → 走 file 自身 `lookupFnSymbol` + `wildcardImports` 顺序查表；
  - (b) 方法 = `ExprDotNode` 且 receiver 是简单变量字面量（含 `$`）→ 按 receiver 类型剥 `Ref<T>` 后查 `Type.method`；
  - 非 `#Const` 抛 `E3111`；查不到（编译器内置 / 复杂 callee）放行。

**未做**：

- SDK 内化（`Array.size` / `String.size` 等纯方法补 `#Const`）—— 需先评估能否给 `#CompilerInner` 注解叠 `#Const`，留单独 commit；
- §4.2.5 递归（"调任何对 `$` / 参数有写效果的方法"）—— 当前由 (1)(2)(4) 等价覆盖；
- `yux-check` sema 镜像（与 Phase 2 / 3 / 4 一致）。

## 跨 Phase 设计决议（详见草案决议日志 [#1.A] – [#1.M]）

- **`val` 浅 / `#Frozen` 深**：避免与 Kotlin `val` 偏离；深不可变交给 `#Frozen`。
- **`cval` 应支持局部**：否则与 `val` 无差异化；第一阶段初值限基本类型 + 字面量算术。
- **参数默认 `val`**：与 Kotlin 一致；强不可变需显式 `#Frozen`。
- **修饰一律走 `#` 注解，不引入新关键字**：保持 keyword 表稳定；与现有 `#Test` / `#Fallible` 一致。
- **const 传递取 C++ 风格——可加不可去**：`#Frozen` 实参不可传给非 `#Frozen` 受方；唯一脱 const 出口为现成 builtin `copy_of:<T>(x T&) T`，**不**引入新 builtin。
- **字段层修饰凌驾外层声明**：外层 `var o` 救不了 `#Frozen` 字段。
- **析构函数 `fn ~()` 不受 const-mut 约束**：析构本质需要修改 `$`（如 `conn.close()`），归入 §8（所有权与引用），不属本体系。
- **`#Pure` 本轮不实施，仅占名**：落脚场景未来 P4 `cval` 编译期求值出口、SIMD / 自动并行候选。`#Pure ⊂ #Const`，未来 `#Pure fn` 自动满足任何要求 `#Const` 的位置。
- **`#Cval` / 关联常量 / 静态成员归独立草案**：不复用 `#Cval` 名字，命名与语义留到 `DRAFT-data-struct.md`（工作名待定）。

## 跨 Phase TODO 汇总

- **callsite `#Frozen` 实参传非 `#Frozen` 形参的全形态检查**：v1 简化为顶层 ID / paren ID 形态。
- **`StatementSetNode` 的字段链拒收 + 字段深链 `#Val` / `#Frozen` 第二跳起解析**：需逐级类型推断，与 callsite 阶段一并。
- **SDK 内化 `#Const`**：评估 `#CompilerInner` 叠 `#Const`；至少 `Array.size` / `String.size` / 数值 `to_*` 等明显纯方法。
- **`yux-check` SemaPass 镜像**：含白名单 `kMigratedCodes` 同步（与 borrow / 错误模型一致）。
- **顶层 `globalConst` 是否放宽到任意 const-evaluable expr**：当前保守 `literal` RHS。

## 关键代码点

- `src/analyzer/const_mut_checker.{h,cpp}` —— P1 全部静态检查入口
- `src/ast/ast_builder.cpp` —— `knownAnnos` 顶部白名单、`readParamAnnos` / `readFieldAnnos` helper、`visitStatementCvalDeclAssign`
- `src/ast/node/statement_node.h` —— `DeclareType::CVal` 枚举值
- `src/yuxParser.g4` —— `statementCvalDeclAssign` 分支、`paramAnno` 产生式、`filedDecl` 上的 `(buildAnno)*`
- `include/error_code.h` —— `DEF_ERR(3104)` – `DEF_ERR(3111)`

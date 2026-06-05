# static-vars 静态变量实施日志

`DRAFT-static-vars.md` Phase 1–6 的落地记录。范围：全局 `let` 三档放开（val / `#Mut` / `#Cval`）、struct 静态字段声明 + 读写路径、`_yux_global_init` ABI、跨模块 init 顺序。

- 规范条款：`docs/spec/05-语句与控制流.md` §5.1.4（全局 `let` 三档）、`docs/spec/07-结构体.md` §7.11（struct 静态字段）、`docs/spec/11-编译期注解.md` §11.9 / §11.11（`#Mut` 全局位/静态字段位 + `#Static` 字段位）、附录 B（产生式）、附录 D §D.3.3（E3150–E3157）
- 草案归档：`DRAFT-static-vars.md` 头部"已落地（Phase 1–6，2026-06-05）"
- 实施分支：`static-vars`（从 const-eval Phase 5 提交后切出）

---

## 核心决策

- **全局 let 三档共用 `letGlobal` 产生式**（[#1.A]/[#1.C]/[#1.D]）：注解（无/`#Mut`/`#Cval`）决定档位。v1 `#Mut` 全局必须 init（禁延后赋值，E3154）；全局对象不调 dtor（生命周期保留到进程结束）。
- **初始化 ABI：`_yux_global_init_<Mod>()` + main shim**（[#1.C]）：不走 `@llvm.global_ctors`，改为编译器生成 `_yux_global_init_<Mod>()` 按 lexical 序 store、main shim 按模块拓扑序调用。理由：顺序可控、调试友好、跨平台一致。
- **const-eval 优先分流**（[#1.D]）：sema 期 ConstEvaluator 试求 RHS；成功 → emit LLVM ConstantInitializer 跳过 runtime init；失败 → 降级 runtime init 入 `_yux_global_init`。
- **静态字段独立产生式 `staticFieldDecl`**（[#1.B]）：与 `filedDecl` 并列，注解必须含 `#Static`，无 `let` 关键字（沿用字段段约定）。v1 泛型 struct 禁静态字段（E3157）。
- **读写路径复用 `ExprPathCallNode`**（[#1.B]）：`Type::FIELD` 读与 `#Static fn` 调用 `Type::name(args)` 共用 parser 节点，sema 按 LHS 类型 + RHS 名分流。写路径 `Type::FIELD = v` 走独立 `statementStaticFieldSet` 产生式。
- **LLVM GlobalVariable mangling**：`<Mod>::<Struct>::FIELD`（与 `#Static fn` 一致）。
- **跨模块 init 顺序**（[#1.E]）：按 `Yux::loadOrder()` 深度优先拓扑序（依赖在先，被依赖模块先 init）；循环 → E3153。init 函数 linkage 设为 `ExternalLinkage` 跨模块可见；始终 emit 桩（即使无 runtime item）。

---

## Phase 1 — 全局 val + runtime init ABI 骨架

- `src/ast/ast_builder_decl.cpp::visitLetGlobal`：放开无注解档（默认 val），接 expr RHS；非 const-evaluable 时 emit 占位（Phase 3 接 const-eval 分流前全部走 runtime init）
- `src/compiler/compiler_globals.cpp`（新增）：维护 per-module init item 列表 `(GlobalVariable*, ExprNode*)`；emit `_yux_global_init_<Mod>()` fn——按 lexical 序逐项 store；0 参 0 返回
- main shim 改造：调用本模块 `_yux_global_init_<Mod>()`（跨模块顺序 Phase 6）
- 错码 E3154（val 缺 init）注册
- 测试：`static_vars_global_val_basic` / `static_vars_global_val_calls_fn` / `diag_static_vars_E3154`

## Phase 2 — `#Mut` 全局放开

- `visitLetGlobal`：接受 `#Mut` 档（复用注解解析）
- `const_mut_checker`：扩"全局 #Mut var 可写"判定
- codegen：`#Mut` 全局 GlobalVariable 标 `internal` linkage，不进 LLVM constant attr
- 决议 [#1.A]：`#Mut let G T`（无 init）→ E3154（v1 禁）
- 错码 E3151（写入非 #Mut）注册
- `visitProgram` 预注册全局 let 符号时根据注解设置 `writeable`/`isConst` 标志（修复硬编码 bug）
- 测试：`static_vars_global_mut_basic` / `static_vars_global_mut_write` / `diag_static_vars_E3151` / `diag_static_vars_E3154_mut_no_init`

## Phase 3 — const-eval 优先分流

- `visitLetGlobal`：调 ConstEvaluator 试求 RHS；成功 → emit LLVM ConstantInitializer 直接挂 GlobalVariable，不入 `_yux_global_init` 列表；失败 → 降级 Phase 1 的 runtime init
- 验证：const-evaluable 全局 init 在 `-O0` 也是 LLVM Constant（不依赖 globalopt pass）
- 测试：`static_vars_const_eval_val_arith` / `static_vars_const_eval_mut_arith` / `static_vars_const_eval_val_struct`

## Phase 4 — struct 静态字段段（声明 + 读路径）

- g4 改动：`structDecl` body 增 `staticFieldDecl` 产生式（独立于 `filedDecl`）
- `src/ast/node/statement_node.h`：新增 `StaticFieldDeclNode`
- `src/ast/ast_builder.cpp`：visit `staticFieldDecl` → 注册到 struct symbol 表静态字段 slot
- mangler：`<Mod>::<Struct>::FIELD`（复用 #Static fn 路径 / 抽 helper）
- codegen：为每个静态字段 emit GlobalVariable；init 走 Phase 3 const-eval 优先分流
- 读路径：`ExprPathCallNode` sema 分流加"LHS 是用户 struct + RHS 是已知静态字段名"分支 → LoadInst on GlobalVariable
- 错码 E3150（v1 必须 init）/ E3157（泛型 struct 禁 #Static FIELD）注册
- E3152（obj.FIELD 实例位访问静态字段）v1 占位未落地（当前走既有字段查找报 E3040）
- 测试：`static_vars_struct_val_basic` / `static_vars_struct_read_path` / `diag_static_vars_E3150` / `diag_static_vars_E3157`

## Phase 5 — 静态字段写路径

- g4 改动：新增 `statementStaticFieldSet` 产生式（`Type::FIELD = expr`）
- `const_mut_checker`：扩静态字段写入档位检
- codegen：`Counter::FIELD = v` → 查 struct 静态字段 → 验 #Mut → StoreInst on GlobalVariable
- E3151 消息更新：从仅 "global variable" 扩为 "global/static"
- 测试：`static_vars_struct_mut_write` / `diag_static_vars_E3151_static_field`

## Phase 6 — 跨模块 init 顺序

- 收集 import 拓扑序：复用 `Yux::loadOrder()`（深度优先加载，依赖在先）
- main shim：按拓扑序依次 emit 外部声明 + call `_yux_global_init_<Mod>()`
- 循环依赖检测：模块加载层已有 E5011；E3153 占位注册作防御
- `_yux_global_init_<Mod>()` linkage 改为 `ExternalLinkage`（跨模块可见）
- 始终 emit init 函数桩（即使无 runtime item，确保 main shim 调用不落空）
- 错码 E3153 / E3155 / E3156 注册（E3155/E3156 v1 占位）
- 测试：`static_vars_cross_module`（两模块，counter 模块 runtime init + main 模块读取）

---

## 跨 Phase TODO / 未落事项

- **E3152**（实例访问静态字段 `obj.FIELD`）：v1 占位未启用；当前走既有字段查找路径自然报 E3040
- **E3158**（读未初始化的 `#Mut` 全局）：DAA 未落地，v1 禁 `#Mut` 全局无 init（E3154），E3158 无触发场景
- **Phase 7**（reflect 路径分流验证）：留待 reflect 合并时完成
- **全局 dtor**：v1 不调，`_yux_global_dtor_shim` hook 留空
- **泛型 struct 静态字段**：v1 禁（E3157），待泛型 mono 化整体收口
- **`#Frozen` 全局**：v1 用 `#Cval` 替代

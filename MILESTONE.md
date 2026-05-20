# 里程碑规划

> 内容多、短期难完，版本号从 **v0.x** 起步推进，**v1.0** 定义为"语言核心稳定 + 规范定稿 + 基础 SDK 可用"的状态，不设硬性日期。
> 当前条目仅为方向规划，不是承诺；具体实施进度看 `CURRENT.md`，已完成实现细节归档到对应 `*-实现.md`。
>
> 注：本文档历史条目使用旧名 `Box<T>`，等同当前的 `Rc<T>`（详见 `docs/dev/rc-rename-impl-log.md`）。

---

## 稳定版目标（参考）

### 语言参考

- `c` `c++` `rust` `kotlin` `java` `python` `go` 不分先后
- 主要参考 `rust` `c++` `kotlin`

### 特性

- 明确的使用
    - 知道每一步类型，在干什么
    - 没有隐式转换
    - 布局而不是继承
    - 移动 or 复制
    - 栈 or 堆
- 统一的用法
    - 尽可能少的特例规则
- 简约的代码
    - `kotlin` 思维
- 内存安全
    - 不直接接触指针
    - 内存由编译器管理，用户只关心业务代码，以及把数据放栈还是堆
    - 指针单向转换 `yux -> ptr`
    - 限制外部函数（后期内容），类 `rust`
- 代码生成代码（后期内容）
    - 类 `rust`
    - 类 `java` 注解预处理
- 更强大的接口 `draft`
    - 接口一致
    - 像接口
- 文档：md 格式
- 条件编译
- 代码修复建议
- 测试

---

## 版本路线（草案）

每个版本只列**主题** + **范围** + **退出标准**。范围之外的项不在该版本承诺内。

### v1.x 与之后（不在本规划展开）

- 异步 / 并发原语
- 多目标（除 Windows 外）
- 包管理与远程依赖
- ……

### v1.0 候选 — 规范定稿 + ABI 冻结

> **现状提示**：v1.0 仍远（v0.14 起步阶段）。spec 仍有较多遗留 Open Issues、潜在 bug 待挖；本节标准是终态门槛，不是近期目标。下方"后续主题"里的待编号工作要先落，才进 v1.0 候选。

**退出标准**：

- spec 全部章节 *Open Issues* 清空或显式转入 v1.x
- 调用 ABI / 内存模型在该版本内冻结
- `base.yux` SDK 公开方法签名稳定
- 所有 BUGS.md 中的 *blocker* 清零
- spec-unify v1 剥离的两个独立草案（`spec-default-body` / `spec-reflect`）已落地（v0.15）或显式推 v1.x
- 闭包捕获已落地（v0.16）—— v0.8 lambda 留口收口

### 后续主题（暂不编号，顺序未定）

下列主题**方向已定但版本号未排**。各自的优先级与依赖关系明朗后再编号。

- **错误模型 v2**：在 v0.9 之上叠加更高阶能力。候选方向：跨类型自动包装 / `Result<T,E>` 内置根类型 / 高阶错误适配（map / mapErr 等）。具体形态待 v0.9 落地一段时间后复盘再定。
  - **依赖**：v0.9 错误模型 v1（已完成）；可能还需泛型 enum / Self 类型 / 函数值的错误通道支持（[#22]）。
  - **不含**：unwind / SEH / personality（yux 不引入异常机制，永久拒绝）。

- **SDK 第一轮扩充**：
  - `math.*` / `Map<K, V>`
  - `String` 方法补全（`to_upper` / `split` / `contains` 等）
  - 基础 IO 改进（行读取、文件 API 雏形）
  - `format` 类格式化（候选 `?` / `{}`，配合 SDK 一并做）
  - `for in` 迭代（`Iter<T>` draft + Array / 定长数组特例 lowering）

- **工具链与编辑器支持**：LSP / IDE 插件（高亮、补全、跳转）；测试框架（约定 + runner）；文档生成（从源码注释 / spec 抽取）。

- **条件编译 + 注解可扩展**：条件编译指令；用户自定义注解雏形（仅元数据，尚不允许执行）。

- **代码生成代码（实验）**：注解预处理 / 编译期生成；类 Java 注解处理 / 类 Rust 派生宏的最小子集；仅允许产出代码、不允许任意副作用。退出标准示例：派生 `ToString` 可用。

- **性能与 layout 优化**：String 专属 FAM Block（`{strong, weak, len_cps, u32 data[]}`）；Array Block 内联小尺寸优化；内联策略与裁剪；基准测试无回归。

### v0.16.0 — 闭包捕获 + yux-check 闭环（草稿）

**主题**：v0.8 lambda 留下的"零捕获"限制收口；同步把 `yux-check` 残留 5 例（全部 lambda 体相关）关掉，让 `yux-check` 与 `yux build` 错误覆盖在常规路径上等价。

**范围（草稿）**：

- 闭包捕获模型：move / retain / 借用 三档显式声明形态；捕获包 layout 与 RC 协议
- lambda body sema 下钻：`SemaPass::visitExpr` 解锁 lambda 体（当前 `src/sema/sema_pass.cpp:1007-1015` 显式 skip）；自然关掉 BUGS #3 的 5 例漏报（E2030 ×2 / E4022 ×2 / E4024）
- 借用 / `$` 逃逸检查在 lambda 体内生效
- Heap by-value 捕获禁止（E4024 在 lambda 体内）
- 与 v0.10 错误模型 v1 的交互：`#Fallible` lambda 形态（若需要）

**不在范围**：高阶函数库化（map/filter/fold —— SDK 扩充时再做）、`dyn fn` 运行时多态、async lambda。

**退出标准**：草案 `DRAFT-closure-capture.md` 全节定型并迁入 spec；`yux-check` 漏报清零（5 例自动关闭）；BUGS.md #3 删条；`xmake test` + `yux test` 全绿；实施日志归档 `docs/dev/closure-capture-impl-log.md`。

### v0.15.0 — spec 收尾：默认方法体 + 编译期反射（草稿）

**主题**：把 `spec-unify v1` 落地时主动剥离的两个独立草案（`DRAFT-spec-default-body.md` + `DRAFT-spec-reflect.md`）按既定顺序实施完毕。**形态已锁**——本版只做落地，不再开形态决议。

> 范围限定：仅指 spec-unify v1 剥离出来的 2 个独立草案。不等同 v1.0 退出标准里的"spec 全部 Open Issues 清空"——其余遗留另排。

**范围（草稿）**：

Phase A（spec 默认方法体）：

- spec body 内方法可带 body：`#Spec struct ToString { fn to_string() String { ... } }`
- 实现者未覆盖 = fall-through 默认体；覆盖 = 自身 body 替代
- 仅签名 + 实现者没写 → E1136 维持；带默认体 + 实现者没写 → 通过
- 多 spec 默认体冲突 → E3132 必须显式覆盖消歧
- 内置类型 `#Impl(ToString)` 的 `#CompilerInner` 是否迁默认体（二选一定下来）
- **永久决议**：不引入 `#Derive` 独立注解

Phase B（编译期反射，依赖 Phase A）：

- 内置 spec `Reflect`：编译器隐式 `#Impl(Reflect)`，依赖 Phase A 默认体机制
- `#Static #Frozen` 字段子集：`type` / `fields` / `methods` / `variants`，仅类型形访问（`Counter::type` / `Self::fields`）
- 元数据 emit 到 `.rodata`；`#Reflect` 注解防 DCE 误删
- `Field` 类型为唯一编译器内置；`Field.value` 走 sema 期改名（**无** `#Inline for` / **无** IR-before pass）
- 反射数据走 runtime 数组（`Self::fields` 是 `[Field& * N]&`，可 `for` 遍历）
- 实例不能调静态成员
- 二进制尺寸基线：典型 yux 程序加入反射元数据后 .exe 增量 < TODO%；超阈值改 opt-in

**不在范围**：`is` / `as` 运行时反查、跨模块字段重命名兼容、反射驱动序列化（SDK 扩充再做）、`Arc<T>` 真正实现（留 v1.x）、闭包（v0.16）。

**退出标准**：两个 DRAFT 全节迁入 spec §12.x（或反射另起 §13）；CHANGELOG 收口；附录 D 新错码登记（E3132 等）；端到端示例（反射驱动 `to_string` / spec 默认体被实现者覆盖）；`xmake test` + `yux test` 全绿；实施日志 `docs/dev/spec-default-body-impl-log.md` + `docs/dev/spec-reflect-impl-log.md`。

### v0.14.0 — 数值语义收口 + 工程优化 + 并发 spec 留口（草稿）

**主题**：把"数字字面量推断 / 整数溢出语义"两件 v1.0 前必锁的事一次定型；同时把"多线程 / async"的 spec 留口写死，防止 v1.x 落地时被现有条款绑死。**并行**把编译器内部代码组织按 SRP 整理一遍（main.cpp 拆分、大文件按类别再切、流程小修），让 v0.15 / v0.16 的特性手术在更干净的代码上做。

> 工程优化按"单一职责 + 给后续版本铺路"为准，不动语义、不动 ABI，仅文件级 split + 模块边界清理；外部贡献开放推迟到 v1.0 稳定后再考虑。

**范围（草稿）**：

数值 / 文档面：

- 整数溢出语义条款（spec 层为主，必要 codegen 配合）
- 字面量类型推断规则：**所有逻辑上能推断的无后缀整数**走推断；浮点字面量保持"必须带 `.`、不接受 `1e2` 形态、不自动推断类型"的严格立场
- 多线程 / async spec 留口：在 §3 / §8 / §9 标注"将重新设计"的具体条款；`Arc<T>` 占名条款细化
- 数字 / 字面量章节文档完善（推断使用、转换、教程示例）
- 其他功能预留位（按需补）

工程优化（SRP 拆分，纯文件级、不动语义 / ABI）：

- `src/main.cpp` (2012 LOC) 按子命令拆：`src/cli/{main,build_cmd,test_cmd,format_cmd}.cpp` + `src/jit/{lljit_runner,seh_memory_manager}.cpp`（SEH MM 现埋 main.cpp 172-233）
- `src/compiler/compiler_expr.cpp` (3174) / `compiler_call.cpp` (2294) / `src/ast/ast_builder.cpp` (2948) 按节点类别二次拆分（literal / member / binary / unary / enum / generic / lambda 等各一文件），让"按节点找代码"成立
- `tests/xmake.lua` 的 `categorize` 在 CI 化之前先加构建期警告：新用例落入 `yux/misc` 时打 `warning`
- `README.md` 顶部加"项目规则索引"段，指向 `.claude/rules/README.md`
- 顺手：`AGENTS.md` → `.claude/rules/` 链接清理（已完成）

yux-check 收尾：

- 残留 5 例（全部 lambda 体相关，BUGS #3）明确推 v0.16 与闭包主题合并；本版只在 BUGS.md / CURRENT-check-sema-gap.md 标"待 v0.16"
- 当前覆盖率 ~96% 维持，不再单独推进

**不在范围**：

- `match` 升级（绑 error v2）
- RC 原子性（v1.0 ABI 冻结）
- 调试信息 / PDB（工具链 v1）
- 闭包捕获 → v0.16
- spec 默认方法体 / 反射 → v0.15（spec-unify v1 剥离的独立草案，本版不动）
- Sema/Codegen 拆分尾段（lambda body 由 v0.16 顺带做；其余 statement 层 / target-type 上下文 / alias 环留独立专项）
- 引入 HIR 中间层（不打算近期加新 backend / async 之前不上）
- CI / 外部贡献基础设施（开放推迟到 v1.0 后）

**退出标准**：

- 数值 / async 留口条款定稿，附录 A / B / D 同步
- 工程优化：拆完后 `main.cpp` < 300 LOC、`compiler_expr.cpp` 单文件 < 1000 LOC、`ast_builder.cpp` 同；`xmake test` + `cd sdk/yux && yux test` 全绿无回归
- BUGS.md #3 条注明"等 v0.16 闭包"
- CHANGELOG 收口
- 实施日志归档于 `docs/dev/v0.14-impl-log.md`（数值条款 + 工程优化两段）

### v0.13.0 — spec 核心收口 + Heap 完整落地 + 构造模型重构 + 工具链同步 ✅ 已完成（2026-05-18）

**主题**：把仍在 draft/ 的草案推到 spec 正文里归档完毕；同时把"栈 or 堆"语义矩阵补完——`Heap<T>` 非空 / 可空形态、`copy_of` 深拷贝唯一入口、NRVO / lambda 返回路径全套落地。

**范围（草稿）**：

- **Spec 核心收口**：
  - `docs/spec/draft/` 中已落地的草案（const-mut / dyn-draft / lambda / let-unify / 错误 / draft / 所有权 / 枚举）逐个核对正文章节同步完整，标"已落地"或迁移归档
  - 仍在草案的 spec-unify 等推到正文或明确推后
- **`Heap<T>` 完整落地**（`DRAFT-heap-types.md` 子特性 B-F）：
  - 子特性 B：`Heap<T>` 非空形态（栈作用域绑定、零 RC、不可传不可 move）
  - 子特性 C：`Heap<T>?` + B 档 move（接管者作用域尾释放、隐式 null 写回）
  - 子特性 D：A 档 NRVO（fn 返回 `Heap` 触发；不触发条件明确）
  - 子特性 E：Lambda 返回 `Heap` / 捕获 `Heap<T>?` move-out / 借用捕获 + return-move
  - 子特性 F：`copy_of:<T>(x T&) T` 深拷贝唯一入口；与 Clone draft 关系定型
  - 子特性 G：`Arc<T>` 占名条款细化（**仅占名**，真正实现留 v1.x）
- **LSP / 插件简单更新**：
  - LSP semantic_tokens / completion：`Heap` / `copy_of` 加入类型 / 内置函数集
  - yux-vscode tmLanguage：`Heap` 进类型关键字集（`Arc` 已在）
  - yux-idea：同上
- **草案归档**：`DRAFT-heap-types.md` 头部标"已落地（Phase 1-8）"+ 实施日志 `docs/dev/heap-types-impl-log.md`
- **构造模型重构**（范围扩展，原 `DRAFT-static-fn.md`）：砍同名 ctor，构造唯一通道收敛到 `#Static fn` + `Self { ... }` 字段字面量；引入 `Self` 类型字面量 + `Type::name(...)` 调用语法；19 个 SDK 文件 + 50 个 tests/examples/docs 全量迁；spec §7.10 / §11.11 / 附录 A.1 / 附录 D.3.3 同步；实施日志 `docs/dev/static-fn-impl-log.md`

**不在范围**：

- `Arc<T>` 真正实现（多线程版 RC，留 v1.x）
- 数值语义 / 字面量推断 / async 留口（推 v0.14）
- 闭包捕获（推 v0.15+）
- Phase 8c FFI 上下文门控（与 FFI 完善合并做）
- Heap C 档复合 move（全 struct move-in ABI 改造，专项）

**退出标准**：✅ `xmake test` 184/184、`cd sdk/yux && yux test` 534/534 全绿；`DRAFT-heap-types.md` / `DRAFT-static-fn.md` 头部均标"已落地"并归档实施日志；LSP completion 加 `Heap` / `Weak` / `Arc` / `Dyn` / `Self` 类型 + `copy_of` / `as_ref` / `ptr_of` / `same_ref` / `weak` / `upgrade` / `heap_some` / `heap_null` / `panic` 内置函数；VSCode tmLanguage `Heap` / `Arc` 已在（Phase 1 rc-rename 时占位）。

### v0.12.0 — 变量模型整理 + 类型命名 + Sema 拆分前奏 ✅ 已完成（2026-05-15，事后补写）

> 注：本条目为**事后补写**——v0.12 的多个工作项（const-mut / Rc 改名 / let-unify / Sema 拆分 Phase 3.x / yux-check 阶段 0）是逐步推进的，未在开工前先定版本目标。后续版本应回归"先定目标再开工"的流程。

**主题**：把变量可变性模型从隐式约定（旧 `var/val/cval`）整理成显式注解驱动的统一形态；同步把 `Box<T>` 改名为更直观的 `Rc<T>` 并预留 `Heap` / `Arc` 命名；并行推进 Sema/Codegen 两段分离的中段（Phase 3.3-3.5）与 `yux-check` 最小可用 exe。

**范围（达成情况）**：

- ✅ **const-mut P1**（`docs/dev/const-mut-impl-log.md`）：
  - 局部 `cval x T = expr` 编译期常量（E3104 初值表达式形态约束）
  - 参数 `#Frozen` 注解（E3105/E3106/E3107 深不可变 + 借用约束）
  - 字段 `#Val` / `#Frozen` 注解（E3108/E3109 字段层不可变）
  - `#Const fn` 注解（E3110/E3111 函数体内禁副作用 + 调用方约束）
  - spec §4.2 / §11.5-§11.8 + 用户教程同步
- ✅ **`Box<T>` → `Rc<T>` 改名**（`docs/dev/rc-rename-impl-log.md`）：纯改名（语义不变），同时占下 `Heap<T>` / `Arc<T>` 名字为未来堆模型 / 多线程预留；全仓 SDK + tests + docs 同步迁移。
- ✅ **let-unify P1-P3**（`docs/dev/let-unify-impl-log.md`）：
  - `var` / `val` / `cval` 三关键字合并为 `let` + `#Mut` / `#Cval` / `#Frozen` 注解（字段段不动）
  - 全局 `let` 强制 `#Cval`；局部 `#Mut let x T` 允许延后赋值（option A）
  - g4 `Cval` / `DeclKey` token 删除；ast 节点 `enum class DeclareType` 改 `bool isMut + bool isConst`
  - spec §5.1 / §11.9 / §11.10 / 附录 A + 用户教程 + LSP + VSCode tmLanguage + IDEA 插件全链路同步
  - 新错号 E3112-E3116（注解形态错）
- ✅ **Sema/Codegen 拆分 Phase 3.3-3.5**（持续推进，未完）：
  - SemaPass 接管 enum / match arm 静态校验（E2019/E2020/E2021/E2023-E2027/E2032）
  - SemaPass 接管 ArrayInit explicit vs value（E3009）、私有字段可见性（E3042）、ExprGetRef / ExprUnary（E3070/E3071）、int 字面量解析（E3103）、`#CompilerInner` / Array intrinsic、E7002 / E7011
  - `compileMemberAssignStatement` E3042 路径切走 sema 验证
- ✅ **`yux-check` 最小可用 exe（阶段 0）**：0 LLVM 依赖的诊断快速通道；CLAUDE.md 新增 Sema/Codegen 协议指南。
- ✅ **warning 通道首批**：E5013 / E5014 / E7016 走 warning 而非 error。
- ✅ **工具链 / 小修**：
  - tuple destructure 别名右值在 ast_builder 透明展开 alias
  - 类型别名在 fn 签名 / 返回位置透明（spec §3.9.1.2 / §3.9.3.1）
  - 0 参块 lambda tail-expr 返回 + `expectedFnType` 反推
  - `yux test` / `--jit-run` 错误路径 SIGSEGV 修复
  - `yux test` suite 末尾汇报失败名单
  - `yux-lsp` after_build 自动复制为 `yux-lsp-claude`

**不在范围**：

- 字段段 let-unify（保留 `var/val/cval f T` + `#Val`/`#Frozen` 字段注解，工作量大，留独立草案）
- `#Const fn` 调用方深度约束（仅声明位禁副作用，传染未做）
- Sema/Codegen 拆分尾段：泛型 fn / impl 体、lambda 体、statement 层、target-type 上下文驱动的类型检查、`compiler_types.cpp` alias 环检测；这些路径出错时仍由 codegen 兜底
- `yux-check` 完整覆盖（与 `yux build` 等价）——仍是子集

**退出标准**：✅ `xmake test` 167/167、`sdk/yux` `yux test` 492/492 全绿；DRAFT-const-mut.md / DRAFT-let-unify.md / DRAFT-rc-rename.md 三个草案均标"已落地"并归档实施日志；外围插件（yux-vscode / yux-idea / LSP）同步完毕。

### v0.11.0 — `Dyn<D>` / `Dyn<D&>` 运行时多态 ✅ 已完成（2026-05-11）

**主题**：把 v0.5 之后保留为"不在范围"的 `dyn Draft` 落地为 `Dyn<D>` / `Dyn<D&>` fat pointer 形态，与现有 `<T : D>` 单态化分发并存、互不替代。

**范围（达成情况）**：

- ✅ fat pointer `{ vtable_ptr, data_ptr }` 16 字节 sized 类型，owned `Dyn<D>` 与 `Box<U>` 同源（data 指 `[RC head | 实例]`，标准 RC + vtable[0] dtor）；借用 `Dyn<D&>` 不动 RC，按 §8.6 借用栈追踪
- ✅ 构造走 turbofish `Dyn:<D>(box_u)` / `Dyn:<D&>(u_ref)` 类型构造形态；不走隐式 coercion / 不引入 `as_dyn` builtin
- ✅ vtable 模型：per-`(Type, Draft)` 静态 `linkonce_odr` global；槽 0 = `fn(ptr) void` dtor、槽 1..N = D 方法按声明序；符号 `__yux_vtable_<U_mod>_<U_struct>__<D_qualified>`
- ✅ 对象安全 v1 第一轮：**禁止** `Self` / draft-name 在 receiver 之外的位置（E1134）；自反方法 `fn clone() Self` 走 `<T : D>` 单态化路径替代，thunk 解锁留 v0.X+1
- ✅ 静态检查：`E1131` 非 draft / `E1132` 嵌套（`Dyn<Dyn>` / `Box<Dyn>` / `Weak<Dyn>`）/ `E1133` 构造源不满足 D / `E1134` 非对象安全 / `E1135` `Dyn<D>?` / `E1136` extern 边界（占位）；方法调用复用 `E6012` / `E6015` / `E6016`
- ✅ codegen：vtable 生成（`compiler_dyn_vtable.cpp`）+ `compileDynCtorExpr` + `compileDynMethodCall` + `_dyn_release(data, vtable)`（emit 在 `emitBoxHelpers` 末尾）；内置 U（i32/i64/bool 等）走 `__yux_dyn_thunk__<U>__<draftQ>__<method>` 适配 thunk 调和 by-value ↔ ptr ABI
- ✅ 借用：`Dyn<D&>` 形参 / 局部按 `data_ptr` 视作借用根登记到 `refToRoot`；`rootFromDynBorrowInit` 解根
- ✅ spec §12.9 全章节落地（§12.9.1..§12.9.11）；§12.8 项 1 由"不在范围"改写为指针；附录 B §B.2a `Dyn` 类型形态；附录 D §D.3.8 追加 E1131..E1136
- ✅ 草案 `DRAFT-dyn-draft.md` 头部标"已落地，见 §12.9"；实施日志归档 `docs/dev/dyn-draft-impl-log.md`
- ✅ 测试：`sdk/yux/src/yux/core/dyn.test.yux`（11 用例，含 `Dyn<ToString>(Box<primitive>)`）；`tests/cases/dyn_*`（`array_iterate` / `pass_owned` / `field_owned` / `ctor_in_method`）；`tests/cases/diag_dyn_*`（六个错误码诊断回归）；BUGS.md 同步解决 `Box<primitive>.method()` ABI 不匹配条
- ✅ 同步修：`compileStructMethodCall` 末段加 `isBuiltinType(actualType.name)` 分支（直接调用路径 by-value 传 primitive receiver）；`ExprDotNode::getType` `baseType.isDyn()` 返回 `fn() <ret>`；`callFieldDestructor` 加 `isDynOwned` / `isDynBorrow` 分支

**不在范围**：

- `Dyn:<D&>(x)` 调用站构造形态（g4 `genericDef` 实参槽不允许 `Type&`；解锁后补 `Dyn<D&>` 端到端用例）
- 对象安全第二轮：`Self` / draft-name 在返回位置的 thunk 解锁（留 v0.X+1）
- `Dyn<D>?` nullable 形态、`Dyn<D>` ↔ `Box<U>` 向下转型（需 RTTI）、反射 / `is` / `as`
- 多线程 vtable 跨线程引用（v1 单线程承诺）
- 操作符 draft 的 dyn 化（沿用 §12.8 项 6）
- LSP semantic_tokens / tmLanguage / IntelliJ 的 `Dyn` 特殊高亮（工具链不在本轮）
- vtable 内联缓存 / devirtualization 性能优化

**退出标准**：✅ `xmake test` 109/109、`yux test` 289/289 全绿；spec §12.9 + 附录 B/D + CHANGELOG + 草案归档 + 实施日志同步；BUGS.md `Box<primitive>.method()` ABI 条已清。

### v0.10 — 工具链：IDE 同步 + Formatter AST 重写 ✅ 已完成（2026-05-10）

**主题**：把 v0.9 落地的语言新形态补齐到所有外围通道（高亮 / 语义 token / 格式化），同时把启发式 token-流 formatter 推倒、换成 AST 驱动的 Doc IR 引擎。这版纯工具链，不动语言面。

**范围（达成情况）**：

- ✅ **IDE / LSP / 高亮同步（v0.9 语法）**：
  - LSP 关键字与 semantic token：Try / Catch 入关键字集；enum / enumMember 两个 token type；EnumDecl / EnumVariant / ExprEnumCtor / PatternEnum / CatchArm 全覆盖；TypeNormalWithRef / TypeGenericWithRef 显式覆盖
  - VSCode tmLanguage：try / catch 关键字、`E::V` 模式、`::` accessor、`!` 错误传播
  - IntelliJ 插件：ENUM / ENUM_MEMBER 颜色键
- ✅ **Formatter AST 重写**（取代 `src/tools/formatter.cpp` 的 token 流启发式）：
  - 新引擎 `src/tools/format/{doc,trivia,render,printer}` —— Wadler/Prettier 风简化 best-layout（Group 试 flat / 不 fits 走 break），HIDDEN 通道 trivia 预扫描映射回输出
  - printer 节点级覆盖：program 顶层（imports / cval / aliasDecl / fn / externDelc）；类型全套（type / typeWithRef × Normal / Generic / Nullable / Array / Tuple / Fn）；fnHeader / fnBody；表达式内联形态（literal / paren / unary / binary 全档 / dot / tupleMember / get / getRef / call / array / tuple / arrayInit / enumCtor / lambdaSingle / lambdaParen / `$`）；语句 9 种全套（Declare / DeclareAssign / DeclareAssignTuple / Set / Loop / Assign / Expr / Ret / RetVoid / Break）；statementBlock 内的空行 / leading / trailing 注释
  - 多行 / 块形 expr（lambdaBlock / tryCatch / match / ifElse / 尾随 lambda 等）走"按原起始列 → 目标列整体平移"的 raw 回退，正确性优先
  - CLI `--format-engine` 选项整体下线；`yux::Formatter` 整文件删除；`formatter.h` 仅留 FormatConfig；LSP `handleFormatting` 切到 `yux::format::formatAst`
  - `tests/cases/format_*.yux` + `.expected_format` 7 个回归用例（basic / generic / fn_type / lambda / enum / tuple / call_chain）；`tests/xmake.lua` 加 `yux/format` 分组

**不在范围**：

- 块形 expr（try-catch / match / if-else / 块 lambda）的"内层结构化"渲染——当前由"语句多行 → raw + reindent"覆盖正确性，但还未按节点细化布局
- formatter 的 line-width 折行决策（Group 框架已就绪，尚未在节点实装；当前所有结构化输出按单行直出，过宽不主动断行）
- pretty-print 风的对齐 / 列对齐 / 注释列尾对齐

**退出标准**：✅ 全仓 159 个 .yux ast round-trip 仅剩有意的格式化规则差异（4-space → 2-space、行尾注释空格规整为 2、triple-blank → double-blank）；`xmake test` 98/98（含 7 个新 format 用例）+ `cd sdk/yux && yux test` 274/274 全绿。



**主题**：把"函数失败"作为一等模型纳入语言：值返回通道 + 注解声明 + 后缀传播 + try-catch 跨类型适配 + panic 终止；不引入异常 / unwind。

**范围（达成情况）**：

- ✅ `#Fallible(E)` 注解（§11 顺带解禁单参数注解糖）：声明函数可能返回错误 enum `E`；fn header 与 struct impl 方法同支持；extern 暂禁
- ✅ 抛出 = `ret ErrEnumValue`；传播 = 表达式后缀 `!`，仅同类型透传，跨类型必须 `match` / `try-catch`；不做自动包装、不做"错误→null"
- ✅ `try { ... } catch (e: E) { ... }` 表达式：跨类型错误形态聚合点；catch arm 穷尽性按 try 块内可见错误类型集合
- ✅ main 出口错误运行时呈现（E7xxx 段位 + 退出码非零 + stderr 错误信息）
- ✅ `panic(msg)` + `#NoReturn` 注解：abort-only 不可恢复路径；不入错误通道；assert / OOM 走 panic；流终止分析覆盖（E7014）
- ✅ ABI 形态：anonymous struct `{ i1 isErr, T_ok, ErrEnum }`；错误返回路径 RC 释放与成功路径共享析构序；错误 enum payload move 语义
- ✅ spec §4.12 / §6.7 / §11 修订；DRAFT-错误.md 起草至定稿；附录 A / B / D 同步；`docs/错误处理.md` 教程；CHANGELOG 收口
- ✅ SDK：`sdk/yux/src/yux/core/exit.yux`、`sdk/yux/src/yux/core/panic.yux`、`error_model.test.yux` 端到端测试

**不在范围**：

- 本地 catch-all（无 `catch _`）、defer / errdefer / finally
- extern fn 的 `#Fallible` 支持（依赖 §7 extern 收紧 + fnType 错误承载，推后续）
- 错误通道在函数值（fn 类型）上的形态（[#22]）
- panic stderr 通道的 STD_ERROR_HANDLE 接通已完成（2026-05-10 stdlib I/O 泛型化随项落地）

**退出标准**：✅ DRAFT-错误.md 全节迁入 spec；`xmake test` + `yux test` 全绿；实施日志归档于 `docs/dev/error-model-impl-log.md`（Phase 1-10 + 子项 10a-10i）。

### v0.8 — lambda + 函数类型

**主题**：引入一等函数值，为后续高阶 API（错误模型 v2 本地 catch、迭代器、Kotlin let 风格尾随块）打基础。

**范围**：

- 函数类型字面量 `fn()` / `fn(T) U`，进入 §3 类型系统
- 零捕获 lambda 字面量（最小子集；闭包捕获后置）
- 尾随 lambda 调用语法（Kotlin let 风格 `f(args) { block }`）
- 普通 struct 方法本地泛型解禁（`fn m<T>(...)`；§12.3.2 仅约束 draft 内方法）
- 函数值的 RC / 值语义协议（与 §8 callee-clean 对齐）

**不在范围**：

- 闭包捕获（move / retain / 借用模式 + 捕获包布局）—— 单列后续主题
- 高阶函数库化（map / filter / fold 等）—— SDK 扩充时再做
- 函数值运行时多态（`dyn fn`）

**依赖**：v0.7 enum 已落地（无强依赖，但实现期会触及 §3 类型系统 / §7 方法 / §8 RC 协议多处，需稳定基底）。

**退出标准**：

- 草案 `DRAFT-lambda.md` 全节定型并迁入 spec 相应章节
- 测试覆盖：函数类型声明、零捕获 lambda 字面量、尾随调用语法、方法本地泛型
- `xmake test` + `yux test` 全绿

### v0.7 — 枚举（enum + match）✅ 已完成（2026-05-07）

**主题**：引入代数数据类型与模式匹配；为后续 v0.8 异常的 `Error.code` 提供载体。

**范围**：

- enum 声明（顶层）：`enum E { V Variant Variant(T1, T2) }`，一行一 variant，无 `,`，详 [DRAFT-枚举.md](../docs/spec/draft/DRAFT-枚举.md)
- 构造表达式：`E::V` / `E::V()` / `E::V(args)`，永远全限定，类型别名透明传递
- match 表达式：`=>` 箭头、`else` 兜底、必须穷尽、payload 直接绑定（不引入 `_` 通配）
- 值类型 + 内联 union 布局；含 RC payload 按 tag dispatch retain/release；与 `Box<E>` / `Array<E>` 复用既有 RC 入口
- **不**包含：泛型 enum、独立 `impl` 块 / 方法、draft 实现、struct-style payload、discriminant 显式赋值、`as i32`、嵌套 / 字面量 / 守卫等高级模式

**退出标准**：

- DRAFT-枚举.md 全节定型并迁入 spec §3 / §4 / §5 / 附录 A B C D 与 CHANGELOG
- `sdk/yux/src/yux/core/enum.test.yux` 覆盖声明 / 构造 / match / 含 RC payload 析构 / 类型别名透传
- `tests/cases/diag_enum_*` 覆盖缺失 variant、重复 variant、`else` 非末尾、payload 元数 / 类型不符、裸 variant、`as i32`、`E::V.0` 等高频诊断
- `xmake test` + `yux test` 全绿

### v0.6 — 语法/语用可用性 ✅ 已完成（2026-05-05）

**主题**：让"日常代码"少一些不便。

**范围（达成情况）**：

- ✅ 字符串模板（Kotlin 风：`"$ident"` / `"${expr}"`，无前缀）；被插值表达式须实现 v0.5 的 `ToString` draft，由编译器在插值位置自动合成 `to_string()` 调用
- ✅ 字符串 `+` 与模板共享 `StringBuilder` lowering（任一操作数为 `String` 即整条 `+` 链合并为单条 SB 累加）
- ⏭ `for in` 迭代下放至 v0.7（语法骨架 `statementForIn` 已在 `yuxParser.g4` 落地，但 AST / codegen / `Iter<T>` draft / 借用集成尚未实现）

**退出标准**：✅ 字符串模板与 `+` 链路在 spec §4.3.1.7 / §4.4.1.4 / §9.3.4.1 落规范；`docs/内置类型.md` 教程同步；`xmake test` 40/40 与 `cd sdk/yux && yux test` 222/222 全绿。详见 `docs/spec/CHANGELOG.md` 2026-05-05 v0.6 Phase 1b/2a/2b/2c 条目。

### v0.5 — `draft`（接口/约束）+ `#DraftLike` ✅ 已完成（2026-05-05）

**主题**：参考 Rust trait + Java interface，一步到位落 **显式实现** + **结构化匹配** 两套形态的最小可用版本。

**范围**：

- `draft` 声明（含 fn 头）、`Type : Draft1 + Draft2 { ... }` 实现块（多 draft 用 `+` 合并到同一块）
- 用作参数类型 / 泛型边界，边界写法 `<T : Draft>`、多约束 `<T : D1 + D2>`
- **编译期单态化分发**（v0.5 阶段不上虚表 / `dyn Draft`）
- 与现有泛型实例化路径融合
- 内置 `ToString` draft 的声明与各内置类型实现，写在 `base.yux`，方法体用 `#Builtin` 注解由编译器替换
- **`#DraftLike` 注解**：标在 draft 声明上，开放该 draft 的"结构化匹配"——任何拥有匹配方法签名的类型自动视为实现；显式 `:`
  实现优先于结构化匹配；未标 `#DraftLike` 的 draft 必须显式实现
- `Box<T>` 自动 forward 已实现的 draft（基于 v0.4 借用规则）

**退出标准**：

- `Counter : ToString` 显式 + 某个未 `:` 但方法名匹配的类型走 `#DraftLike` 通过的端到端示例
- spec 增章节（拟 §10 或附录），`#DraftLike` 滥用边界（默认严格、需显式注解）写入条款
- `ToString` draft 可作为 v0.6 字符串模板的"可插值约束"，无需再回头改类型规则

### v0.4 — 借用与生命周期细化 ✅ 已完成（2026-05-03）

**主题**：把 `T&` 从"语法允许"推到"语义清晰"。提前到 draft 之前，是因为 v0.5 的 `<T : Draft>` 边界匹配需要先有清晰的"`T&`
能否满足 `T`"等规则。

**范围（达成情况）**：

- 借用作用域规则规范化：`borrow_checker.cpp` 引入块作用域栈，对齐 spec §8.6.5 / §8.6.5.8；O(1) 唯一规则保留
- 防 dangling 的静态检查：根对象重赋检测；`Array<T>&` 借用期内修改方法拒绝（§8.4.2.5）
- 与 `Box<T>` / `Array<T>` 互操作的边界：新增 baked `as_ref:<T>(box Box<T>) T&` 作为 v1 唯一获取 payload `T&` 的途径（§8.3.5.5）；明文"无隐式降级"（§8.3.5.4）
- 借用与泛型边界匹配的接口规则：新增 §8.6.7（类型形参限定 owned；signature 内可组合 `T&`；方法分发归一）

**退出标准**：✅ spec §8.6 / §8.9 完成（详见 `docs/spec/CHANGELOG.md` 2026-05-03 条目）；15 条 `tests/cases/borrow_*` 反例测试纳入 `xmake test`；T& 与泛型边界 / 方法分发交互见 spec §8.6.7，v0.5 draft 草案可直接引用。实施日志归档于 `docs/dev/borrow-impl-log.md`。

### v0.3 — 编译诊断改进 ✅ 已完成（2026-05-03）

**主题**：把"编译报错信息太少"这件事系统性补上。诊断质量影响所有后续开发体验，单列一档。

**范围**：

- 错误统一格式：`文件:行:列 [错误码] 主信息`
- 出错处源码片段 + 下划线/插入符指向
- 错误码体系（`E0001` 起，按类别分段：词法 / 语法 / 类型 / 所有权 / 模块 …），写入 `docs/spec/附录D-诊断.md`
- warning vs error 分级雏形（含静默 / 升级开关）
- 高频场景的"提示 / 修复建议"：
    - 类型不匹配（含 Nullable / 泛型实参）
    - 未声明标识符（含拼写近似建议，如可行）
    - 缺失 `;` / 空格规则违反
    - 泛型实参缺失或多余
    - `T&` 出现在禁止位置（字段 / 返回值 / `T& &`）
    - `Weak<T>?`、`Weak == / !=`
    - `extern` 签名不匹配
- 诊断回归测试集：`tests/cases/diag_*.{yux,expected_err}`

**退出标准**：

- 上述高频场景全部带行列 + 提示
- 诊断格式在附录 D 中规范化
- 诊断回归测试纳入 `xmake test`

### v0.2 — 文档与规范骨架 ✅ 已完成（2026-05-02）

**主题**：从教程升格到规范，对外可参考。

**范围（达成情况）**：

- `docs/spec/` §1–§11 + 附录 A / B / C 全部填实
- `DRAFT-所有权与引用.md` 已迁入 §8，原草案归档至 `docs/spec/draft/`
- 草案目录化 + `_模板.md` 骨架范本入库
- 用语统一（应当 / 不得 / 应该 / 可以）；附录 B 与 `src/yux.g4` 交叉核对
- `docs/spec/CHANGELOG.md` 上线，记录各 Phase 收口

**退出标准**：✅ spec 主体章节 TODO 清零（Open Issues 保留）；附录 B 与 `yux.g4` 对齐；详见 `docs/spec/CHANGELOG.md`。

### v0.1 — 核心模型稳定 ✅ 已完成

**主题**：所有权 / RC / 内置堆类型 layout 收口。

**范围**：

- Box / Array / Weak / String / Ptr 的 Block + RC 协议（strong/weak + 哨兵）
- callee-clean retain/release、move-return 注入
- 泛型函数与结构体方法实例化
- 字符串不可变 + StringBuilder
- 析构序与作用域规则

**退出标准**：

- 所有 Phase 勾完，归档于 `docs/dev/ownership-impl-log.md`
- RC 泄漏检测通过
- `xmake test` 全绿

---

## 术语

- **退出标准**：进入下一版本前必须满足的条件，非"完成度"而是"门槛"。
- **范围**：本版本承诺触达的领域；范围外的提议默认下放。

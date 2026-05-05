# 规范变更记录

> 上新下旧。每条记录规范层（`docs/spec/*.md`）的语义变化、章节增删、用语收口、与编译器实现的对齐点。
>
> 日常用户教程（`docs/*.md`）的改动不在此记录；语法（`src/yux.g4`）改动以 commit log 为准，本文件只记录其在 spec 层的反映。

记录格式：

```
## YYYY-MM-DD —— 摘要（commit-hash 可选）

- **新增 / 修改 / 删除**：影响章节与一句话说明
- **冲突 / 兼容**：是否破坏既有条款引用；如破坏，给出迁移指引
```

---

## 2026-05-05 —— v0.6 Phase 2b 字符串模板 `ToString` 自动分发

- **修改**：§4.3.1.7 类型规则放宽 —— 插值位置允许任何实现 `ToString` 的类型；非 String 由编译器在插值位置合成 `expr.to_string()` 调用，复用 v0.5 单态化方法分发。
- **修改**：[E3026] 措辞改为"未实现 `ToString`"，从"插值位置只接受 String"放宽为"插值类型未实现 `ToString`"；命中场景仍为类型不匹配段。
- **冲突 / 兼容**：纯放宽。Phase 2a 报 E3026 的非 String 内置类型插值（`i32` / `bool` / `f64` 等）现在合法；显式 `Type : ToString { ... }` 用户类型同样可插值。诊断用例 `diag_strtpl_non_string_interp` 改为不实现 `ToString` 的类型（`Array<i32>`）兜底。

## 2026-05-05 —— v0.6 Phase 2a 字符串模板 codegen（`StringBuilder` lower）

- **修改**：§4.3.1.7 增 lowering 伪代码与类型规则 —— 含插值的 `StringTemplateNode` lower 为 `sb := StringBuilder() / sb.append(...) / sb.build()`。
- **新增**：[E3026]`String template interpolation requires String type, got '{}'` —— Phase 2a 期间插值位置只接受 `String`；非 String 显式提示调 `.to_string()`，附录 D §D.3.3 类型不匹配段范围由 E3001..E3025 扩到 E3001..E3026。
- **冲突 / 兼容**：纯增量。Phase 2b 引入 `ToString` 自动分发后，约束放宽至"实现 `ToString`"；该错误码语义随之改写为"未实现 `ToString`"。

## 2026-05-05 —— v0.5 内置 `to_string` 迁入 `Type : ToString` 显式实现

- **修改**：§12.7.1.2 措辞调整 —— 内置类型 `to_string` 以 `Type : ToString { ... }` 形态在 `base.yux` 显式实现；方法体可 `#CompilerInner` 或 yux 实现，二者并存；当前 i64/u64/f64/bool/String 走 yux 实现，窄类型委派。
- **冲突 / 兼容**：`base.yux` 内对应方法从普通方法块迁入 draft 实现块，外部调用面（`x.to_string()`）不变，无破坏。

## 2026-05-04 —— 引入 §12 draft（接口与约束）+ `#DraftLike` + `<T : D>` 边界 + `copy_of`

- **新增**：§12 全章 —— draft 声明 / 显式 `Type : D { ... }` 实现 / `#DraftLike` 结构化匹配 / 跨包 orphan / Box forward 归一 / 内置 `ToString` 与 `Any` / builtin `copy_of:<T>(x T&) T`。决议依据见 `docs/spec/draft/DRAFT-draft.md`。
- **新增**：§7.8（draft 实现块语法扩展，详见 §12）；§6.4.4（draft 边界声明形态、单态化校验）；§11.4（`#DraftLike` 注解 + 互锁规则 + 风格指引）；§11.5 / §11.6 重编号（原 §11.4 → §11.5，原 §11.5 → §11.6）。
- **修改**：§6.4.1.2 把"v0.5 引入 draft 后启用 trait bounds"改为正文，明确 v1 支持类型参数 + 可选 draft 边界，无 `where` / 无 or 约束；§11.2.3 builtin 清单追加 `as_ref` / `copy_of`，内置类型 `to_string` 经 `Type : ToString { #CompilerInner ... }` 形式给出。
- **新增**：附录 A 关键字加 `draft`、注解表加 `#DraftLike`；附录 B 加 `draftDecl` / 扩展 `structImpl` / `genericDef` 引入 `typeParam` + `draftBound`（v0.5+ 形态，回写 `src/yux.g4` 前需用户确认）；附录 C 新增 §C.6a draft 术语 11 项；附录 D 段位表追加 E11xx 段，§D.3.7 列出 E1101–E1106 / E1110–E1112 / E1120 占位（编号在编译器实施期固化）。
- **冲突 / 兼容**：纯增量（v0.5 新引入）；既有 §6.4 / §7 / §11 / §8.6.7 条款均不破坏，仅追加交叉引用。`#DraftLike` 是 v1 第三种正式注解（继 `#CompilerInner` / `#Test`）。`src/yux.g4` 暂未改（按 CLAUDE.md 项目约束需先与用户确认）；附录 B 文本与 `.g4` 短期内不同步，以草案 `draft/DRAFT-draft.md` §10.3 为准。
- **后续**：编译器实现详见 CURRENT.md Phase 3；SDK / 测试详见 Phase 4；附录 D 占位错误码在 Phase 3 与 `include/error_code.h` 同步固化。

## 2026-05-04 —— 统一函数泛型声明调用

- **修改**：`fn <T> name()` => `fn name<T>()`
- **冲突 / 兼容**：仅影响sdk

## 2026-05-04 —— 修复 `yux test` JIT 模式下跨 yux 助手帧 SEH 静默崩溃

- **修改**：§11.3.5.8 known-issue 删除——根因是 LLVM `RTDyldMemoryManager::registerEHFramesInProcess` 在 Win64 COFF 上不调 `RtlAddFunctionTable`，导致 `RuntimeDyldCOFFX86_64` 收集的 `.pdata` 段从未注册到 OS，跨多个 JIT 帧 unwind 时 `RtlVirtualUnwind` 找不到 `RUNTIME_FUNCTION` → 进程静默退出。修法：自定义 `SectionMemoryManager` 子类覆盖 `registerEHFrames`/`deregisterEHFrames`，在 `RtlAddFunctionTable` / `RtlDeleteFunctionTable` 中注册 `.pdata`，ImageBase 取本对象内已分配 section 的最低非零地址。
- **修改**：§11 Open Issues 同步移除「`yux test` JIT 模式下，从 yux 实现的助手中触发的 SEH 异常未被 wrapper 捕获」条目；JIT 模式下 yux 助手 fail 路径与 `#CompilerInner` fail 路径行为一致，均产出 `FAIL <module>#<fn> (SEH ASSERT_FAILED 0xe0fa17ed)`。
- **冲突 / 兼容**：纯修复；既有 v1 用例（仅覆盖 pass 路径）继续通过；之前因 known-issue 暂时移除的 fail 用例可重新启用。

## 2026-05-04 —— `yux test --isolate=process` 子进程隔离（Phase 5）

- **新增**：§11.3.4.4 `yux test --isolate=process`：每个 `#Test` 在独立子进程内执行，崩溃 / 内存脏化只影响该测试。`--isolate=none`（默认）保持同进程 SEH wrapper 行为。
- **新增**：§11.3.4.5 显式记录 `-v / --verbose` 失败时回放 stdout / stderr 的语义（Phase 3 已实现，仅补 spec 层）。
- **修改**：§11.3.4.3 文本同步——v1 默认同进程 SEH wrapper 已能扛住单条 SEH 异常，不再"任一测试崩溃整体非零退出"。
- **副作用**：BUGS.md 记录的「yux 助手失败路径触发的 SEH 在 wrapper 内失活」在子进程模式下被自动绕开（子进程崩溃即子进程退出码，父进程翻译），可作为该 known-issue 的临时 workaround。
- **冲突 / 兼容**：纯增量；默认行为未变；新加的 `--isolate-child` / `--capture` 是 hidden CLI（父子进程协议），不暴露给用户。

## 2026-05-04 —— `assert_eq` 扩展到 String + 新增 `assert_contains` / `assert_starts_with`（Phase 4b）

- **修改**：§11.3.5.2 `assert_eq` 类型分派表加 `String`；分派改由编译器 dispatcher 在「参数严格匹配的非泛型重载存在时优先于泛型」实现（`compiler_call.cpp` 新 `getGenericFunction`），SDK 侧 `base.yux` 末尾追加 `fn assert_eq(actual String&, expected String&)` 等 yux 实现重载。`#CompilerInner` 泛型 `assert_eq:<T>` 仍是 i8..u64 / f32 / f64 / bool 路径，未变。
- **新增**：§11.3.5.7 `fn assert_contains(haystack String&, needle String&)` / `fn assert_starts_with(s String&, prefix String&)`，纯 yux 实现，分别调用新增的 `String.contains` / `String.starts_with` 方法。
- **新增**：`String` 加方法 `contains(needle String&) bool` 与 `starts_with(prefix String&) bool`（base.yux）。
- **新增**：§11.3.5.8 known-issue —— `yux test`（JIT 模式）下，由 yux 助手的失败路径触发的 `_yux_test_assert_failed()` SEH 异常**不**被 wrapper 捕获，runner 直接 abort；v1 用例只覆盖 pass 路径，fail 路径暂由 `#CompilerInner` 数值/`bool`/`fail` 断言覆盖。详见 `BUGS.md`。
- **冲突 / 兼容**：纯增量；既有 `.yux` 源码无破坏。运算符 dispatcher 副作用：当用户同时定义同名 generic 与非泛型重载时，参数严格匹配的非泛型现在优先（更接近常见语言语义；先前是先到先得）。`compileCustomTypeBinaryOp` 同期加固：操作数本身是 `T&` 时剥一层 ref 后再做方法表查找；`compileKnownFunctionCall` 加固：非局部变量（字面量 / 临时值）作为 `T&` 形参实参时 alloca-store 临时再传 ptr。

## 2026-05-04 —— 运算符重载形参收口为 `Self&`

- **修改**：§7.2.3.3 二元运算符方法形参从"应当与接收者类型一致"改为"应当为 `Self&`"；形参为 `Self`（按值）等其它类型时该方法只是普通方法，不再被运算符触发。
- **新增**：§7.2.3.6 运算符 `a OP b` 在编译期重写为 `a.method(&b)`，右操作数自动取址，是 §6.2 / §8.3 一般规则的运算符位置局部例外。
- **冲突 / 兼容**：v1 尚未发版，原"形参写 `Self`"是旧设计的不足（按值复制额外成本，且与 `$` 接收者借用形态不一致）；规范层一次性收口。本仓库内仅 `docs/结构体.md` Complex 示例使用旧形态，同步改为 `Self&`；SDK 与 tests/ 中无既有运算符重载实现，无代码迁移。

## 2026-05-03 —— 新增 `#Test` 测试断言 API（`assert_eq` / `assert_true` / `assert_false` / `fail`）

- **新增**：§11.3.5「测试断言 API」 —— SDK 在 `sdk/yux/src/yux/core/assert.yux`（与 `base.yux` 同属 `yux.core` 平铺）提供 4 个 `#CompilerInner` 断言：泛型 `assert_eq:<T>`（T ∈ i8…u64 / f32 / f64 / bool）、`assert_true(bool)` / `assert_false(bool)`、`fail(String)`。无需 import，全局可用。失败语义（v1）：调 SDK `_yux_test_assert_failed()` → `RaiseException(0xE0FA17ED)` → SEH 显示 `FAIL <module>#<fn> (SEH ASSERT_FAILED 0xe0fa17ed)`。v1 **不**打印断言种类与 `fail(msg)` 的 `msg` 内容（待 String stringify 扩展同期补齐）。
- **新增**：附录 D §D.3 错误码 E6030 —— `assert_eq:<T>` 类型实参越界（仅数值 + bool）。`E6027 {} expects {} argument(s)` 复用为四个断言的 arity 错误。
- **修改**：§11 Open Issues 收口"`#Test` assert API 形态"，新增"打印实参值需先引入 `Stringify` 约束"与"`assert_eq` 扩展到 String / 用户结构体"两项后续。
- **冲突 / 兼容**：纯增量。SDK API 集合扩展，新模块 `yux.test.assert`；既有 `.yux` 源码无破坏。`#Test` 函数体内调用断言与 §11.3.2「无参 / 无返回 / 必须有体」约束相容。

## 2026-05-03 —— 新增 `#Test` 注解与 `yux test` 子命令

- **新增**：§11.3 `#Test` 注解条款 —— 仅挂 `fn`；签名等价 `fn name(): void`（无参、无返回类型、必须有函数体）；与 `#CompilerInner` 互斥；仅可出现在 `*.test.yux` 文件中。普通 `yux build` 跳过 `#Test` 函数的 codegen，不进入 `.exe` / `.lib` 产物。
- **新增**：§11.3.3 测试文件发现规则 —— `*.test.yux` 仅由 `yux test` 在项目 `src/` 下递归发现；模块名取 src 相对路径转点分形式，保留 `.test` 段（例：`src/yux/core/arithmetic.test.yux` → `yux.core.arithmetic.test`）。
- **新增**：§11.3.4（informative）`yux test` 行为 —— 仅项目模式可用；选择器支持 `<prefix>` 前缀匹配、`<module>#<fnName>` 精确定位；v1 同进程顺序执行，无隔离（崩溃即整体非零退出）。
- **修改**：原 §11.3「其他注解」与 §11.4「用户自定义注解」整体下移为 §11.4 / §11.5；§11.4.1 注解对照表加入 `#Test` 行。
- **新增**：附录 C 加入 `#Test` 与 `yux test` 术语条目。
- **冲突 / 兼容**：纯增量。既有 `#CompilerInner` 条款全部保留；既有 `.yux` 源码无破坏（现有源码均不带 `#Test`，且不存在 `*.test.yux` 文件）。

## 2026-05-03 —— 诊断列号语义收口 + 多字节插入符对齐

- **修改**：附录 D §D.1.1 —— `col` 字段语义由"1-based 字节列号"更正为"1-based 字符列号"（按 Unicode codepoint 计数，与 ANTLR `getCharPositionInLine() + 1` 同源；纯 ASCII 输入下数值与原文档一致，回归测试 0 改动）。
- **新增**：§D.1.1 明确插入符 `^` 按**显示列宽**对齐：CJK / 全角 / 常见 emoji 计 2 列，组合标记 / 零宽字符计 0 列，Tab 原样保留。
- **删除**：§D.7 Open Issues 第 1 条（"列号当前按字节计算..."），改写为已收口注记。
- **冲突 / 兼容**：纯措辞 + 渲染对齐改进；`col` 数值不变；现有 `diag_*.expected_err` 子串断言全部兼容。新增 `tests/cases/diag_multibyte_caret.{yux,expected_err}` 锁定多字节场景下 ^ 落点。

## 2026-05-03 —— 语法换行规则放宽（Kotlin 风单行 / 多行）

- **新增**：
  - §6.1.1.5 `fnHeader` 形参列表支持单行 / 多行两种写法（`(` 后、参数间 `,` 后、`)` 前允许 `LineEnd*`，末参可尾随 `,`）。
  - §6.2.1 `fnParams` 产生式同步更新；§4.7.1.5 `exprGetRef` 链中 `.` 前允许换行；§4.7.2.5 `exprDot` 链式 `.` / `?.` 前允许换行；§4.7.3.3 `exprGet` 索引列表允许换行 + 尾逗号；§4.8.2.5 `exprCall` 实参列表允许换行 + 尾逗号；`exprArray` 数组字面量同此。
  - 附录 B §B.4 / §B.6 产生式与 `src/yux.g4` 对齐。
- **格式化器规则**（*informative*，规范层不强制；详见 `CURRENT.md` Phase 5 设计文档）：函数声明 1 参强制单行，多参默认单行、超 120 列折成多行；链式 `.` 默认仅超列宽时折，手写换行格式化器保留不合并。
- **冲突 / 兼容**：纯放宽，原有所有单行写法保持合法；AST 不变（`LineEnd` 在新位置仅作分隔，不入 AST），ASTBuilder / 编译器零改动。



- **新增**：
  - §8.3.5.4 `&box` 永远生成 `Box<T>&`，无隐式降级；§8.3.5.5 / §8.3.5.6 `as_ref:<T>(box Box<T>) T&` baked builtin 规范化为 v1 唯一获取 payload `T&` 形态的途径。
  - §8.4.2.5 / §8.4.2.6 `Array<T>&` 借用期内禁止调用修改方法（`push` / `pop` / `insert` / `remove` / `clear` / `set_len` / 容量 grow）。
  - §8.6.5.7 `as_ref` 站点根追溯。§8.6.5.8 借用作用域粒度按 §5 块作用域计（v0.4 由函数级扁平对齐至块级）。
  - §8.6.7（新章节）借用与泛型边界匹配：类型形参 `T` 限定 owned 类型（§8.6.7.1）；`Box<U>` 上 `obj.method` receiver 归一为 `U&`（§8.6.7.3）；`use_ref(as_ref(box))` 协作（§8.6.7.4）；类型形参可在签名内组合为 `T&`（§8.6.7.5）。
  - §8.9 / §3.7 联动追加三条禁忌：`as_ref(Box<T>?)` 类型禁、`Array<T>&` 借用期修改禁、用户泛型 `T = U&` 禁。
- **修改**：§8.3.5.4 编号顺延为 §8.3.5.7（FFI ptr_of）；§6.4.1.2 反向引用 §8.6.7.1 / §8.6.7.5。
- **冲突 / 兼容**：现有用户代码无破坏——`as_ref` 是新增能力；`Array<T>&` 借用期 push 限制对当前 SDK / 测试无触发（既有调用模式不构成借用 + 修改的并存）；类型形参 owned 限定与现状 v1 类型推断一致。
- **§8 Open Issues 清理**：`same_ref` / `ptr_of` 接 `T&` 由 §8.6.7.5 解决；其余 6 条转 v0.11 / v0.9 / v1.x 后续版本。

## 2026-05-03 —— Phase 5 B 阶段：未声明标识符的拼写近似建议

- **新增**：`src/symbol_suggest.{h,cpp}` 提供 `nearby` / `buildHint` / `throwSymbolNotFound`：从给定 `ScopeNode` 开始
  沿父链汇总可见变量与函数名，按 Levenshtein 距离 ≤ 2 排序后取最近 1–3 个候选，组装成
  `did you mean \`foo\`?` / `did you mean one of: \`foo\`, \`bar\`?` 形式的 help 行。
- **挂 hint 的站点**：E3030（`compiler_expr.cpp` 字面量加载、成员访问 / 取地址父链 lookup；`compiler_stmt.cpp`
  普通赋值与成员赋值；`node/expr_node.cpp` `&obj`）、E3031（`compiler_expr.cpp` / `compiler_stmt.cpp` 成员访问的
  `_localVarPtrs` miss）、E3032（`node/literal_node.cpp` 标识符字面量类型解析）。
- **测试**：新增 `tests/cases/diag_suggest_var.{yux,expected_err}`，断言 `conut` → `count` 的 help 行。
- **冲突 / 兼容**：无规范条款变更；候选为空时不附 hint，原有错误信息逐字保留，所有既有测试不受影响。

## 2026-05-03 —— Phase 5 A 阶段：诊断 help / note 基础设施 + 高频站点 hint

- **新增**：`Diagnostic` 已有的 `notes` / `hints` 字段接通 `YuxError` —— `YuxError` 携带 `_hints` / `_notes`，提供链式
  `withHint(string)` / `withNote(string)` 便利接口；`DiagnosticEngine::renderYuxError` 把它们作为 `= help: ...` /
  `= note: ...` 行附在源码片段之后输出，遵循 §D.1.1 既有格式。
- **挂 hint 的站点**：
  - E2001 `Weak<T>?`、E3078 `Weak == / !=`：提示 `upgrade(weak)` 路径
  - E3017 / E3018 / E3019：T& 局部初始化形态指引
  - E4001 `BorrowChecker` 借用初始化、E4004 不能绑非本地
  - E2006 / E2007 缺函数体：提示加 `#CompilerInner` 或补 body
  - E6010 / E6011 泛型实参个数：给出 `:<T...>` 模板
  - E1002 ANTLR 文法错误：按消息模式（`';'`、`mismatched/extraneous input`、`no viable alternative`）附简单空格 / `;` 提示
- **测试**：新增 `tests/cases/diag_ref_init_form.{yux,expected_err}` 与 `diag_generic_arity.{yux,expected_err}`；
  `diag_weak_nullable.expected_err` 追加 help 断言。
- **附录 D**：§D.5.4 路线图重写为 A / B 两阶段，标注 A 已落地；附 hint 的码段一并列出。
- **冲突 / 兼容**：诊断输出格式不变；新增的 `= help:` / `= note:` 行属于 §D.1.1 已经允许的"0..N 条 note / help"，
  既有 `expected_err` 子串匹配机制不会因之失败。

## 2026-05-03 —— Phase 4：诊断分级 / CLI 严重度开关 / 文件级聚合

- **新增**：`DiagSeverity { Note, Warning, Error }` 落地，每个错误码（`ErrorCode::EXXXX`）通过 `DEF_ERR` / `DEF_WARN` / `DEF_NOTE` 携带 `defaultSev`；当前所有码默认 `Error`，`Warning` / `Note` 留待 Phase 5 与未来错误恢复后启用。
- **新增**：CLI `--warn=<code>` / `--allow=<code>` / `--deny=<code>` / `--Werror`；主命令与 `build` 子命令均可使用（subcommand 通过 `fallthrough()` 继承）。
- **不可降级原则**：默认 `Error` 的码不允许通过 `--warn` / `--allow` 降级；尝试降级时打印 `cannot downgrade ... (default severity is error)` 并忽略，理由是当前 Compiler 在 `YuxError` 抛出后即停，没有错误恢复机制（详见 §D.5.3）。
- **聚合策略**：从"首错即出"改为**文件级聚合** —— 单文件 codegen 失败不再立即 `exit(1)`；驱动层继续编译其余模块，最后再以非零退出码结束。链接阶段在任一模块失败时跳过。
- **附录 D**：§D.1.1 严重度叙述更新；§D.1.2 改写为聚合语义；新增 §D.5 严重度策略与 CLI 开关；原"路线图"挪入 §D.5.4。
- **冲突 / 兼容**：诊断输出格式无变化；既有用例 / `expected_err` 全部沿用。新引入的 `--warn` 等选项不传时行为完全等价于此前。

## 2026-05-02 —— Phase 6：诊断回归测试

- **新增**：`tests/cases/diag_*.yux` + `*.expected_err` 用例形态，由 `tests/xmake.lua` 识别并走"编译期望失败 + 逐行子串包含 stderr"的判定路径。首批纳入 6 个用例，覆盖 E1001 / E1002 / E5010 / E2001 / E3020 / E3032 / E3093 / E3094。
- **附录 D**：§D.5 路线图收口（Phase 4 / 5 挪入 `TARGETS.md`，删除 Phase 6）；新增 §D.6 描述 `expected_err` 子串断言协议；原 §D.6 Open Issues 顺延为 §D.7。
- **冲突 / 兼容**：无规范条款变更；测试运行器对既有 `*.expected` 用例零影响。

## 2026-05-02 —— Phase 3：ANTLR 词法 / 文法错误接管

- **新增**：`src/syntax_error_listener.{h,cpp}` 自定义 `BaseErrorListener`，按 `recognizer` 是否为 `antlr4::Lexer` 派发为 `E1001 lexer error: {}` / `E1002 syntax error: {}`，即时通过 `DiagnosticEngine::render` 输出统一格式（file:line:col + 源码片段 + 插入符）。
- **接入**：`Yux::_parseFile` 与 `main.cpp` 的 `parseAST` / `compileIR` / `compileSdkDir` 全部 `removeErrorListeners()` + `addErrorListener(&errListener)`，原 `getNumberOfSyntaxErrors()` 走错合并为 `errListener.hasErrors() || ...`。
- **附录 D**：新增 §D.3.1（E1xxx 段），后续段编号顺延；§D.4 接入表"词法 / 文法"由"未接入"改为已接入；§D.5 路线图删除 Phase 3 项。
- **冲突 / 兼容**：原先 ANTLR 默认 `ConsoleErrorListener` 直接打印到 stderr 的 `line N:col` 形式不再出现；现在所有词法 / 文法错误都带 `[E1xxx]` 错误码与源码片段。

## 2026-05-02 —— 附录 D：诊断与错误码

- **新增**：`docs/spec/附录D-诊断.md`，规范化诊断输出形态（`file:line:col [Exxxx] severity: msg` + 源码片段 + 插入符 + `note` / `help`）、错误码段位（E1..E6xxx）、当前已分配错误码全表，与 `include/error_code.h` 一一对应。
- **新增**：`docs/spec/index.md` 附录索引补入附录 D。
- **冲突 / 兼容**：无规范条款变更；附录 D 与编译器 Phase 2 已落地的 `ErrorCode::E####` / `DiagnosticEngine` 同步。后续阶段（ANTLR 接管、warning 分级、修复建议、回归测试）仍记录在附录 D §D.5 路线图，落地后应回写本附录。

## 2026-05-02 —— 草案目录化：`docs/spec/draft/`

- **新增**：`docs/spec/draft/` 目录，收纳跨章节设计草案与骨架范本 `_模板.md`；草案改为入库（原先存放于仓库根目录、不入 git）。
- **定位明确**：草案**不等同规范**，是否实施以 `docs/spec/` 正文为准；草案与 spec 冲突时以 spec + `src/yux.g4` + 编译器源码为准。AGENTS.md 同步更新。
- **迁入**：原根目录 `DRAFT-所有权与引用.md` → `docs/spec/draft/DRAFT-所有权与引用.md`；§3 / 实施日志中相关相对链接同步修正。
- **冲突 / 兼容**：无规范条款变更；仅文档组织调整。

## 2026-05-02 —— Phase 6：一致性审校与用语统一

- **用语统一**：全文 *必须* → *应当*（§C.8 规范用语集合 = 应当 / 不得 / 应该 / 可以）。"须为" / "须可" 等描述性表述保留，不计入规范用语。
- **新增**：本文件 `docs/spec/CHANGELOG.md`；`docs/spec/index.md` 增入口；`docs/index.md` 增 spec 区块（教程 ↔ spec 互链）。
- **状态**：spec 与 v1 编译器（含 `src/yux.g4` 用户最新调整：`opShift` 优先级高于 `opBitAnd/Or/Xor`）已交叉核对，未发现需登 BUGS.md 的冲突。

## 2026-05-02 —— Phase 5：模块 / 注解 / 附录收口

- **§10 模块系统**（重写）：覆盖 `yux.toml`（含 `[lib]` 节）、`<projectRoot>/src/` 源根、文件模块 vs 包模块、`use a.b.c` / `use a.b.*`、**`pkg` 文件再导出清单**（`name` 别名 / `name.*` 扁平）、命名解析三段式、循环依赖禁、`_` 前缀私有、`yux.core` SDK / `base.yux`。
- **§11 编译期注解**：`buildAnno ::= '#' ID codeLineEnd`；`#CompilerInner` 互锁规则（带注解必省体；省体（除 extern）必带注解）；v1 不支持用户自定义注解。
- **附录 A 保留字**：与 `yux.g4` 全量对齐（关键字 / 上下文标识符 / 注解名 / 符号 token / 词法 token / 预留）；显式登记 v1 无 `continue` / `pub` / `priv` / `mut` / `const` / `trait` / `match` / `for` / `while` / `do` / `async`。
- **附录 C 术语表**：补 §C.6（模块 / 包 / 项目 / 入口 / 源根 / `pkg` 文件 / 文件模块 / 包模块 / 通配导入 / `_` 前缀私有 / `yux.core`）。

## 2026-05-02 —— Phase 4：所有权（§8）规范化

- **§8 所有权与引用**（新写）：来源 `DRAFT-所有权与引用.md` + `docs/dev/ownership-impl-log.md` 已稳定条目。
  - §8.1 概述（四档：值 / 堆句柄 / 借用 / FFI 指针）；
  - §8.2 RC 协议：Block 布局 `{ rc: { strong: u32, weak: u32 }, payload }`、强弱计数协议、哨兵 `0xFFFFFFFF`、单线程模型；
  - §8.3 `Box<T>`、§8.4 `Array<T>` / `String`、§8.5 `Weak<T>` 与 `upgrade`；
  - §8.6 借用 `T&`：形态 / 不参与 RC / 绑定与 rebind 禁 / 取址 `& expr` 寿命规则（声明作用域 ⊇）/ 接收者 `$`；
  - §8.7 调用 ABI：callee-clean retain、move-return retain 注入、`extern` 边界 `T&` ↔ `Ptr` 自动转换、地址比较；
  - §8.8 临时值清单（temp frame）/ fresh 句柄 / RC leak 计数 `_rc_block_count` / `rc_leak_count()`；
  - §8.9 禁忌一览（字段 `T&` / 返回值 `T&` / `T& &` / `Weak<T>?` / `Weak == /!=`）；
  - §8.10 v1 不在范围（cycle collector / 多线程 / `Weak<Array>` / `Weak<String>`）。

## 2026-05-02 —— Phase 3：结构体 / 内置类型

- **§7 结构体**：声明、方法块、构造函数 + DAA、析构函数、按值复制、平凡 / 非平凡分类、字段级派生、自引用必经堆。
- **§9 内置类型**：`Box<T>` / `Array<T>` / `String` / `StringBuilder` / `Weak<T>` / `Ptr` / `Ref<T>` / 数值标量；`null` 仅在 `T?` 与 `Ptr` 上下文出现。

## 2026-05-02 —— Phase 2：表达式 / 语句 / 函数

- **§4 表达式**：求值顺序（左到右、自底向上）、运算符语义、短路 `&&` / `||`、安全访问 `?.`、null 兜底 `??`、取址 `& expr`、`if` 表达式形态。
- **§5 语句与控制流**：`var` / `val` / `cval` 三类声明、赋值复合形态、`if` 语句、`loop` + `break`、`ret`、词法作用域析构序。
- **§6 函数**：声明、参数组（`a, b, c i32`）、表达式体 `= expr`、泛型 turbofish `name:<T>(args)` + 单态化、`extern fn` C ABI、ABI 概要指向 §8.7。

## 2026-05-02 —— Phase 1：词法 / 语法 / 类型基础

- **§1 词法**：行首列概念、注释三态（`LineComment` / `LineEndComment` / `EmptyLine`）、标识符（Unicode）、字面量（数值含进制 / 浮点 / 字符串 / 码点 `c'<ch>'` / `null`）。
- **§2 语法**：ANTLR4 / EBNF 叙述约定；附录 B 同步建立。
- **§3 类型系统**：标量 / 用户 struct / `[T*N]` 三种值类型、堆句柄、借用 `T&`、FFI `Ptr`；名义类型判等；可赋值性（无隐式标量转换）；§3.6 Nullable `T?` + 隐式包装；§3.7 禁忌（`Box<T>?` 等价 `Box<T>` / `Weak<T>?` 禁 / `Weak == /!=` 禁）。

## 2026-05-02 —— Phase 0：骨架

- **新建** `docs/spec/`：`index.md` + 11 章 + 附录 A / B / C 占位骨架，确立 §N / §N.M.K 编号与 *应当 / 不得 / 应该 / 可以* 用语约定，每章末预留 *Open Issues*。

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

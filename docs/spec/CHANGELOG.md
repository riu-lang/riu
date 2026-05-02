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

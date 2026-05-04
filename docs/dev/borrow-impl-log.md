# 借用与生命周期 v0.4 实施日志

本文件归档 v0.4 借用与生命周期细化的实施记录：核心决策、规则形态、关键代码点。是后续回答"as_ref 为什么是唯一获取 payload T& 的途径"、"Array 借用期 push 为什么禁"等问题的事实来源。

- 规范条款见 `docs/spec/08-所有权与引用.md` §8.3.5 / §8.4.2 / §8.6 / §8.9
- 前序 v0.1 模型见 `docs/dev/ownership-impl-log.md`
- 进行中的工作见 `CURRENT.md`（本地）
- 与本日志无关的待修 bug 见 `BUGS.md`（本地）

---

## Phase 1 — 缺口调研

对照 v0.4 退出标准盘点：spec §8.6 / §8.9 主体已写但缺 §8.6.7（借用×泛型）；`borrow_checker.cpp` 简化为函数级扁平作用域，与 §8.6.5「声明作用域 ⊇」不一致；T&×泛型 / 方法分发尚无明文；反例测试集仅 1 条。

## Phase 2 — spec 起草与回写

§8.3.5.4–§8.3.5.7：明文"无隐式降级"；新增 baked builtin `as_ref:<T>(box Box<T>) T&` 作为 v1 唯一获取 payload `T&` 的途径；FFI `ptr_of:<Box<T>>(box)` 编号顺延。

§8.4.2.5 / §8.4.2.6：`Array<T>&` 借用存活区间内禁止调用任何会修改 Block 内部 `len` / `cap` / `data` 的方法（push / pop / clear / set_len 等）。`&arr[i]` 既已在语法层禁，本节防的是"借 handle 本身 + 改 Block"。

§8.6.5.7：`as_ref(box)` 站点的根追溯——根 = box 的根，编译器视同对 box 整体 payload 的借用。
§8.6.5.8：借用作用域粒度按 §5 块作用域计；v0.4 由函数级扁平对齐至块级。

§8.6.7（新章节）借用与泛型边界匹配：
- §8.6.7.1 类型形参 T 限定 owned 类型，不接 `T&`；
- §8.6.7.3 `Box<U>` 上 `obj.method` 自动解引用后 receiver 归一为 `U&`；
- §8.6.7.4 `use_ref(as_ref(box))` 协作；
- §8.6.7.5 类型形参可在签名内组合为 `T&`（如 `fn same_ref:<T>(a T&, b T&)`），T 本身仍受 .1 限制。

§8.9 / §3.7 联动追加三条禁忌：`as_ref(Box<T>?)` 类型禁、`Array<T>&` 借用期修改禁、用户泛型 `T = U&` 禁。

§6.4.1.2 反向引用 §8.6.7.1 / §8.6.7.5。

§8 Open Issues 清理：`same_ref` / `ptr_of` 接 `T&` 由 §8.6.7.5 解决；其余 6 条转 v0.11 / v0.9 / v1.x。

## Phase 3 — 借用检查器对齐 spec

`src/borrow_checker.cpp` 引入 `Scope` 栈：
- 每层记录本块新增 declared / borrows
- `pushScope` 在进入 fn 体 / `StatementBlockNode` / `StatementLoopNode` 内 block / if-elif-else 各分支 block 时调用
- `popScope` 弹栈时摘除本块 declared、对借用反向减 `_activeBorrows`、清 `_refToRoot`

合法的"块内借用消亡后再重赋根对象"由此被允许，函数级扁平的误拒消除。

## Phase 4 — dangling 静态检查加深 + as_ref 实现

### 4a — 语法 / SDK 配套

- `src/yux.g4`：`retType=type` → `retType=typeWithRef`；重新生成 ANTLR parser
- `src/ast_builder.cpp`：3 处 retType 处理切到 `buildTypeWithRef`；新增 E2009 检查（非 `#CompilerInner` 不许 retType 含 `&`）
- `sdk/yux/src/yux/core/base.yux`：声明 `#CompilerInner fn as_ref<T>(box Box<T>) T&`

### 4b — Codegen

- `src/compiler_call.cpp` baked dispatch 新增 `as_ref` 分支：从 Box<T> handle GEP +8 跳过 RC 头，直接返回 payload 起点指针
- `src/compiler_stmt.cpp` 在 T& 局部声明的 RHS 形态白名单中加入 `ExprCallNode` 形 `as_ref(box)`，直接消费 baked codegen 返回的非空 ptr

### 4c — 借用检查扩展

- `borrow_checker.cpp` `rootFromRefInit` 识别 `as_ref(box)` 形态作为根追溯起点（§8.6.5.7）
- 新增 `_rootType` 表，跟踪 owned 根对象的类型
- 新增 `checkArrayMutation` 检查（§8.4.2.5）：受用 `obj.mutating(...)` 形态时若 obj 解析到 Array<T> 根且根对象有活跃借用，抛 E2010
- 修改方法名单：`{push, pop, clear, set_len, insert, remove}`

### 4d — 错误码

- `E2009`：用户函数 retType 含 `&` 但无 `#CompilerInner`
- `E2010`：Array<T>& 借用期内调用修改方法

## Phase 5 — 反例测试集

15 条 `tests/cases/borrow_*.{yux,expected/expected_err}` 覆盖：
- as_ref 合法形态（取借用、链式借用、泛型函数协作、拷绑透传）
- as_ref / 借用 与 root 重赋的冲突（标量 / Box）
- Array<T>& 借用期调用修改方法的拒绝（push / pop / clear / set_len 各一条）
- 块作用域借用消亡后允许重赋 / 修改（Phase 3 回归保护）
- 用户函数 retType 含 `&` 报 E2009（Phase 4 grammar 改动副作用验证）
- 类型形参组合为 `T&` 的合法签名（§8.6.7.5 验证）

## ABI / 关键代码点

- `as_ref:<T>(box) T&` baked：`compileGenericFnCall` `as_ref` 分支；`borrow_checker::rootFromRefInit` `as_ref` 识别；`compileStatementDeclareAssign` T& RHS 白名单
- 借用块作用域栈：`borrow_checker::Scope { declared, borrows }`，`pushScope` / `popScope` 在 `visitBlock` 包裹
- Array 借用期修改检测：`borrow_checker::checkArrayMutation`，挂在 `visitExpr` 对 `ExprCallNode` 的下钻
- retType 含 `&` 控制：`ast_builder::visitFunctionHeader` 检查 `retType->getType().isRef() && !hasAnno("CompilerInner")` 抛 E2009

## TODO（v0.4 未覆盖，记入后续版本）

- 通过 `Array<T>&` 借用变量直接调用修改方法的检测（v0.4 仅查直接对原句柄调用；ref 调用形态作为漏洞列出，v0.5+ 加深）
- 字段链中段 Box 句柄重绑（如 `s.box1 = s.box2` 当 `&s.box1.field` 活跃时）—— v0.4 仅查顶层赋值
- `as_ref` 接 `Box<T>?` 时的诊断 hint 加深
- §8.6.7.5 解决的 `same_ref` / `ptr_of` 接 `T&` turbofish 用例补 baked builtin 内部测试

# 闭包捕获实施日志

本文件归档 yux v0.16 闭包捕获模型的实施记录：独立草案起草、sema 解锁、借用检查集成、规范回写。是后续回答"三档捕获模式为何这样划分"、"lambda body 借用检查如何生效"、"yux-check lambda 体内漏报为何清零"等问题的事实来源。

- 规范条款见 `docs/spec/draft/DRAFT-closure-capture.md`（草案落稿）+ spec 正文 §4.11.6 / §8.7.6
- 用户教程见 `docs/Lambda与闭包.md`
- 原 lambda 草案见 `docs/spec/draft/DRAFT-lambda.md`（§5–§6 已迁至 DRAFT-closure-capture.md）
- 测试集 `tests/cases/lambda_closure_*`（14 个）+ `tests/cases/diag_heap_lambda_capture_non_null.yux`

---

## 背景

DRAFT-lambda.md Phase 4a–4e（v0.8，2026-05-09）已交付捕获的**完整 codegen 实现**（三档自动推断 + layout + fat-ptr ABI + E2030/E4022/E4024）。v0.16 的缺口在 **sema 未接管**（lambda body 显式 skip，yux-check 漏报 5 例）和**规范未收口**（三档模式在 spec 中未正式定型）。

---

## Phase 1：DRAFT-closure-capture.md 独立草案

**核心决策**：从 DRAFT-lambda.md §5–§6 提炼闭包捕获为独立规范草案，定型三档模式命名与语义。

### 三档捕获模式

| 档位 | 适用类型 | 捕获语义 |
|------|----------|----------|
| **move**（值复制） | 标量 / struct / enum / `[T*N]` | captures 字段存副本，字段级 retain |
| **retain**（共享句柄） | Rc / Array / Weak / String / StringBuilder | captures 字段 retain 后存句柄 |
| **borrow**（借用透传） | T& / `$` | captures 字段存指针，不 retain，栈嵌入，不可逃逸 |
| **move（Heap B 档）** | `Heap<T>?` | 捕获时 outer slot 写 null，env 独占所有权 |

### 决议项

- 不引入显式捕获列表语法（保持 DRAFT-lambda.md [#18] 决议）
- 嵌套闭包 FV 递推策略：逐层展开
- `#Fallible` lambda 形态 → Open Issue O2，推 v0.x+1
- DRAFT-lambda.md 头部加迁出注

**涉及**：`docs/spec/draft/DRAFT-closure-capture.md`（新建）、`docs/spec/draft/DRAFT-lambda.md`（加迁出注）

---

## Phase 2：Lambda body sema 解锁（策略 2b 宽松模式）

**核心决策**：移除 `SemaPass::visitExpr` 对 lambda body 的 `return;` skip，走宽松模式——能查的查，依赖形参类型的 deferred 给 codegen。

### 关键改动

- **`SemaPass::visitExpr`**（`src/sema/sema_pass.cpp`）：移除 `return;` 占位，改为下钻 lambda body
- **E2030 检查**：对 lambda 体内 `StatementAssignNode` / `StatementSetNode` 检查捕获变量写禁
- **E4022 检查**：lambda 字面量直接作 ret/var-init → 含 T& 捕获则报不可逃逸
- **E4024 检查**：lambda body 内引用外层非空 Heap 变量 → 报按值捕获禁止
- **修复**：`bodyScope->lookupSymbol` 沿父链误判 → 改用形参列表直判
- **修复**：`StatementSetNode` vs `StatementAssignNode` 两路径均覆盖
- **修复**：lambda 体内 ret 语句 E3020/E3022 用外层 `_currentFn` 误报 → 加 `_currentLambda` guard skip
- **Compiler 端**：原 throw 保留作幂等防御性双跑，每条加 `// sema shadow` 注释

### 验证（yux-check 5 例全部正确报错）

| 用例 | 错误码 |
|------|--------|
| `lambda_closure_assign_capture.yux` | E2030 |
| `lambda_closure_assign_capture_compound.yux` | E2030 |
| `lambda_closure_borrow_escape.yux` | E4022 |
| `lambda_closure_self_escape.yux` | E4022 |
| `diag_heap_lambda_capture_non_null.yux` | E4024 |

**涉及**：`src/sema/sema_pass.cpp`、`src/sema/sema_pass.h`、`src/compiler/compiler_lambda.cpp`、`src/compiler/compiler_stmt.cpp`、`src/compiler/expr/expr_literal.cpp`

---

## Phase 3：借用检查在 lambda 体内生效

**状态**：实现已在 v0.8 DRAFT-lambda.md Phase 4e 完成，v0.16 验证通过。

### 关键实现（`BorrowChecker::visitLambda`，`src/analyzer/borrow_checker.cpp`）

- `visitExpr` 遇到 `LambdaExprNode` → 派发 `visitLambda`
- lambda T& 形参注册为 lambda body 内的借用根（`_refToRoot[pname] = pname`）
- 捕获 T&：`_refToRoot` 为成员变量共享不按 scope 销毁，lambda 体内 `resolveRoot` 正确溯源外层根
- lambda ret T& 溯源检查：
  - 允许源集 = lambda 形参 T& 集合（捕获 T& **不进**允许源集）
  - body 单表达式 / tail-expr 的 ret T& 根判：∉ 允许源集 → E4020
  - retType 为 T& 但允许源集内 T& 形参数 ≠ 1 → E4021
- 外层 ret-ref 状态在进入/退出 lambda 时保存/恢复，支持嵌套 lambda

### 验证

- `lambda_closure_borrow_escape` / `lambda_closure_self_escape`：Compiler + yux-check 双路径通过
- `lambda_closure_ret_borrow_param` / `lambda_closure_ret_borrow_capture`：通过

---

## Phase 4：错误模型交互（评估 → 推后）

### 评估结论

- lambda body 内 `!` 传播 → E7001（lambda 隐式不可失败，正确行为）
- try-catch 在 lambda 内：codegen `emitLambdaFunction` 有独立编译流，与外层 `_tryCatchStack` 隔离
- `#Fallible` fn 内定义 lambda：lambda body 编译流独立，不受外层 fallible 状态影响
- fn 类型不承载 `#Fallible`（与 extern fn 同档推迟）
- 当前无已知 bug → **推 v0.x+1**

### Open Issue

DRAFT-closure-capture.md O2：`#Fallible` lambda 形态的规范定型。待 v0.x+1 专项。

---

## Phase 5：规范回写与收尾

### DRAFT-closure-capture.md 定型

- 头部标记从"草案 / 决议中" → "已落地（v0.16）"
- 涉及章节：§4.11.6、§6.5.5、§8.7.6、附录 D

### Spec 修订

- **§4.11.6**：顶部加注引用 DRAFT-closure-capture.md；新增 §4.11.6.5 `Heap<T>` 非空形态闭包捕获条款
- **§8.7.6**：顶部加注引用 DRAFT-closure-capture.md
- **附录 D**：错码均在 v0.13 已登记（E2030/E4020/E4021/E4022/E4024），无新增

### CHANGELOG

顶部追加 v0.16 闭包捕获条目（sema 解锁 + yux-check 漏报清零 + 借用检查集成）

### 用户教程同步

`docs/Lambda与闭包.md`：
- 头部加注引用 DRAFT-closure-capture.md
- 捕获模式表补充 `Heap<T>?` / `Heap<T>` 行
- 新增"Heap 捕获"小节（B 档 move + E4024）
- 交叉引用补 DRAFT 链接

### 测试与 lint

- `xmake test`：241/241 ✅
- `cd sdk/yux && yux test`：544/544 ✅
- `./lint.cmd`：0 警告 ✅

---

## 涉及文件总览

| 层面 | 文件 | Phase |
|------|------|-------|
| 草案 | `docs/spec/draft/DRAFT-closure-capture.md` | P1 新建，P5 定型 |
| 草案 | `docs/spec/draft/DRAFT-lambda.md` | P1 加迁出注 |
| Sema | `src/sema/sema_pass.cpp` | P2 解锁 + E2030/E4022/E4024 |
| Sema | `src/sema/sema_pass.h` | P2 `_currentLambda` guard |
| 借用 | `src/analyzer/borrow_checker.cpp` | P3 `visitLambda`（v0.8 已完成） |
| Codegen | `src/compiler/compiler_lambda.cpp` | P2 加 sema shadow 注释 |
| Codegen | `src/compiler/compiler_stmt.cpp` | P2 加 sema shadow 注释 |
| Codegen | `src/compiler/expr/expr_literal.cpp` | P2 加 sema shadow 注释 |
| 规范 | `docs/spec/04-表达式.md` | P5 §4.11.6 修订 |
| 规范 | `docs/spec/08-所有权与引用.md` | P5 §8.7.6 修订 |
| 规范 | `docs/spec/CHANGELOG.md` | P5 追加 |
| 教程 | `docs/Lambda与闭包.md` | P5 同步 |

---

## 跨 Phase TODO 汇总

- **O2**（DRAFT-closure-capture.md）：`#Fallible` lambda 形态的规范定型。当前无已知 bug，codegen 自然工作；留 v0.x+1 专项。
- **O1**（DRAFT-closure-capture.md）：嵌套闭包中 `T&` 捕获的多层验证。当前 codegen 已支持单层，多层 + T& 组合路径待验证。
- **O3**（DRAFT-closure-capture.md）：`Heap<T>?` 捕获在 body 内多次调用的所有权语义——当前 env 独占所有权，行为符合预期，无需额外条款。
- **O4**（DRAFT-closure-capture.md）：`[T*N]` 定长数组捕获——当前按值类型走 move 档，行为正确，不需单独列一行。
- Sema/Codegen 拆分尾段：泛型 fn / impl 体、statement 层、target-type 上下文驱动的类型检查、alias 环检测 —— 这些路径的 sema 镜像仍未完成，出错时由 codegen 兜底。

# 草案：yux 闭包捕获模型

状态：**已落地（v0.16，2026-06-10）**。对应 spec：§4.11.6、§6.5.5、§8.7.6、附录 D。
作用：从 DRAFT-lambda.md §5–§6 提炼闭包捕获的独立规范——定型 move / retain / borrow 三档模式、捕获包 layout 与 RC 协议、静态检查规则。本草案是 DRAFT-lambda.md 在捕获子域的**替代**（非补充）。

> 本草案的所有规则均已逐条决议（见末尾"决议日志"）。实施完成于 v0.16（Phase 1–5），归档实施日志见 `docs/dev/closure-capture-impl-log.md`。

---

## 1. 目标

- 把 DRAFT-lambda.md Phase 4 闭包捕获实现固化为独立草案，消除 §4/§6/§8 三处引用的碎片化。
- 定型三档捕获模式：**move**（值复制）、**retain**（共享句柄）、**borrow**（借用透传），每档的语义边界与触发条件精确。
- 描述捕获包内存布局与 RC 协议（已在 compiler_lambda.cpp 落地，本草案将其提升为规范描述）。
- 明确 `T&` 捕获的特殊路径（栈嵌入 + LSB 标记 + 不可逃逸）及其与通用路径的边界。
- 列出所有闭包相关静态检查（E2030/E4022/E4024/E4020/E4021/E2029）及其触发条件。
- **不改语法**：不引入显式捕获列表；捕获模式完全由外层变量类型自动推断（保持 [#18] 决议）。

---

## 2. 全景

| 捕获模式 | 触发条件（外层变量类型） | captures 字段操作 | 运行时路径 |
|----------|--------------------------|-------------------|------------|
| **move** | 标量 / 用户 struct / 元组 / enum（值类型） | 按值复制（struct/元组字段级 retain） | 堆 `_box_alloc` + dtor |
| **retain** | 堆句柄（`Rc<T>` / `Array<T>` / `String` / `StringBuilder` / `Weak<T>`） | 句柄复制 + `retain` | 堆 `_box_alloc` + dtor |
| **borrow** | `T&` | 存指针，不 retain | **栈 alloca**，LSB=1 标记 |
| **move（Heap B 档）** | `Heap<T>?`（可空） | 写 env 后 outer slot 置 `{_has=false, _value=null}` | 堆 `_box_alloc`（dtor 释放 Heap payload） |
| ❌ 禁止 | `Heap<T>`（非空） | —— | 编译错 E4024 |

要点：

- 零捕获 lambda：`captures = null`，retain/release 早返 NOP，零开销。
- 捕获判定 `O(1)`：`FV(λ)` 中是否存在 `T&` 类型变量 → 决定走栈嵌入还是堆路径。
- 两条路径（栈 vs 堆）在 ABI 层一致：`fn_ptr` 隐式首参签名都是 `Ptr`，调用点无分支。
- 捕获是**逐变量**而非"整个外层 frame"；每次 lambda 求值独立分配 captures。

---

## 3. 自由变量收集 FV(λ)

### 3.1 定义

编译期为每个 lambda 字面量计算自由变量集：

```
FV(λ) = { 标识符 x 在 λ 体内被引用 } \ ( λ 形参 ∪ 顶层全局 / cval / 顶层 fn 名 / 模块名 )
```

- 在词法作用域内解析，不跨越 fn 声明边界向上越级（除非外层 fn 本身是 lambda 从而构成嵌套闭包）。
- `$` 在方法体内 lambda 视作隐式 `Self&` 形参参与收集（§6）。
- 嵌套闭包：内层 lambda 的 `FV` 递归展开到外层 lambda 的 `FV`（逐层递推）。

### 3.2 CapturesT

`CapturesT` 是编译期为每个含捕获 lambda 单独合成的匿名 struct，字段为 `FV(λ)` 中每个变量在外层的类型 `Tx`，按 §4 规则映射为 captures 字段类型。字段按首次发现顺序排列，自然对齐。

---

## 4. 三档捕获模式

### 4.1 move（值复制）

**触发**：外层变量类型为值类型（标量 / 用户 struct / 元组 / enum）。

**语义**：
- captures 字段类型 = `Tx`（同类型）。
- lambda 求值时从外层栈槽 load 值，按 `Tx` 的字段级复制语义写入 captures buffer（struct/元组字段中的堆句柄走字段级 retain）。
- 闭包持有独立副本，后续外层变量变化对闭包不可见。
- 闭包多次调用间共享同一 captures 副本（`Rc<CapturesT>` retain 后多 fat-ptr 指向同一 captures block）。

**运行时**：堆路径，`_box_alloc` + dtor（全标量 captures → dtor=null → `_box_release_dtor` 仅 free，不调 dtor）。

### 4.2 retain（共享句柄）

**触发**：外层变量类型为堆句柄（`Rc<T>` / `Array<T>` / `String` / `StringBuilder` / `Weak<T>`）。

**语义**：
- captures 字段类型 = `Tx`（同类型）。
- lambda 求值时从外层栈槽 load 句柄值，调 `retainHandleAtCallSite` retain 后写入 captures buffer。
- 闭包与外层共享同一堆对象；闭包多次调用间通过 shared `Rc<CapturesT>` 共享句柄。
- 句柄在 `CapturesT` 析构时逐个 release（由 `emitCapturesDtorFunction` 合成的 dtor 驱动）。

**运行时**：堆路径，`_box_alloc` + dtor（必有非 null dtor）。

### 4.3 borrow（借用透传）

**触发**：外层变量类型为 `T&`。

**语义**：
- captures 字段类型 = `T&`（指针）。
- 不 retain，所有权不转移；闭包仅持有外层栈的观察指针。
- captures **不能**进 `Rc<CapturesT>`（§3.2.3.2 字段位置禁 `T&`），编译器改为**栈嵌入** `CapturesT`。
- fat-ptr 的 `captures` 字段存栈 alloca 地址，LSB=1 标记"跳过 RC 操作"。
- lambda 自身按"广义 `T&`"处理 → **不可逃逸**（§5.1）。

**判定**：`O(1)` — `FV(λ)` 中存在至少一个 `T&` 类型变量即触发。

**4.3.1 栈嵌入与堆路径不混合**

含 `T&` 捕获的 lambda **不得**同时含堆句柄捕获（`retain` 档）。违者编译错（当前由 E2029 在 compiler_lambda.cpp 捕获写入阶段拒）。理由：混合释放路径未实现（栈路径无 dtor 调度，堆句柄字段需析构）。

> 实际场景中极少出现"同时捕获 `T&` 和 `Rc`"的需求；若未来需要，可引入"部分栈 + 部分堆"的复合 captures layout，但不在 v1 范围。

**4.3.2 LSB 标记协议**

- 栈 alloca 地址低 1 位 = 0（对齐保证），编译器写 `alloca_ptr | 1` 到 fat-ptr.captures。
- retain/release 站点检测 `captures & 1`：为 1 → 跳过 RC 操作（早返）。
- body 入口 `emitLambdaFunction` 统一掩 LSB（`captures & ~1`）得到真实 alloca 指针 → `_currentLambdaCapturesArg`。
- heap 句柄（`_box_alloc` 返回）8 字节对齐，LSB=0，掩 LSB 不变，协议透明。

### 4.4 Heap B 档 move

**触发**：外层变量类型为 `Heap<T>?`（可空堆句柄）。

**语义**：
- captures 字段存 16 字节 `{i1 _has, ptr _value}`（LP64）。
- lambda 求值时 load 外层槽值写入 env，随后把 outer slot 写 `{_has=false, _value=null}`。
- 不 retain（`retainHandleAtCallSite` 无 Heap 分支），outer scope 尾 dtor 见 `_has=false` 跳 free，env 独占所有权。
- 与 §5.3 字段 move-out 同款语义。

**禁止**：`Heap<T>`（非空）按值捕获 → E4024（§7.3）。引导用户声明为可空形态 `Heap<T>?`。

---

## 5. 捕获包布局

### 5.1 通用堆路径（无 T& 捕获）

```
captures block (由 _box_alloc 分配)：
  [handle+0 .. +8]   RC 头（strong/weak 引用计数）
  [handle+8 .. +16]  dtor fn ptr（null = 无字段需析构；否则指向 __captures_dtor_<mangle>）
  [handle+16 .. +N]  capture 字段区，每槽按其类型 allocSize 自然对齐排列
```

- `RC 头` 由 `_box_alloc` 管理；dtor 槽 + 字段区 = payload。
- `_box_release_dtor`：strong 归零 → 若 dtor ≠ null 则调 `dtor(fields_base)` 释放各堆句柄字段 → `_box_free`。
- 多个 fat-ptr 副本共享同一 captures block（retain 增加 strong count）。

### 5.2 栈嵌入路径（含 T& 捕获）

```
captures buffer (栈 alloca)：
  [offset+0 .. +16]  保留前缀（与堆路径 layout 对齐，字节不参与语义）
  [offset+16 .. +N]  capture 字段区（T& 指针 + 标量值，无堆句柄）
```

- 保留 16 字节前缀是为了 body 内 GEP offset 计算与堆路径一致（从 +16 起），简化编译器实现。
- 无 RC 头、无 dtor；栈帧退出时自动回收。
- fat-ptr.captures 存 `alloca_ptr | 1`（LSB 标记）。

### 5.3 fat-ptr

```
fn(T)R 运行时值 = { fn_ptr: Ptr, captures: Ptr }   // 各 8 字节，LP64 总 16 字节
```

- `fn_ptr`：指向 lambda body 编译出的顶层 LLVM Function（InternalLinkage）。
- `captures`：零捕获 → null；堆路径 → `_box_alloc` 返回的 handle；栈路径 → `alloca_ptr | 1`。

---

## 6. `$` 在方法 lambda 中的捕获

方法体内的 lambda 引用 `$` / `$.field` / `$.method()` 合法：

- `$` 在 ast_builder 处登记为 `Ref<Self>`（`Self&`），走 §4.3 borrow 路径。
- `$` 视作隐式形参参与 `FV(λ)` 收集。
- 自动触发 §5.1 不可逃逸约束：闭包寿命 ≤ `$` 借用寿命 = 方法 frame。
- lambda 内 `$.field = ...` 受 §7.1 E2030 约束（视作捕获 binding 写入）。
- lambda 内不重新绑定 `$`（永远是外层方法的 receiver）。

---

## 7. 静态检查

### 7.1 E2030 — 捕获变量写禁

lambda 体内**不得**对 `FV(λ)` 中的标识符执行：

- `=` 赋值
- 复合赋值：`+=` `-=` `*=` `/=` `%=` `<<=` `>>=`
- 堆句柄重新绑定（`rc_var = ...`）
- 通过成员链写入（`obj.f = ...` / `obj[i] = ...`，其中 obj 为捕获变量）

理由：标量按值复制后赋值仅影响 captures 副本，对外层静默无效，与用户直觉冲突。

**合法替代路径**：
- 通过堆句柄的方法接口修改对象内部状态（`arr.push(x)` 不重新绑定句柄）
- 把外层借用作为 lambda 形参显式传入（`(r T&) => ...`）

### 7.2 E4022 — 含 T& 捕获的 lambda 逃逸

含 `T&` 捕获（`FV(λ)` 中至少一个 `T&` 类型变量）的 lambda **不可**：

- 作 `ret` 表达式（除非满足 §6.5 溯源约束）
- 入 struct 字段
- 入容器（`Array<fn(...)>` 等）
- 赋给寿命外延的局部变量（`var f = lambda_with_ref_capture`）
- 入 `Rc<fn(...)>`

即该 lambda 只能在当前 frame 内消费（作实参传给调用、直接调用等），不能逃逸出借用源 scope。

### 7.3 E4024 — Heap 非空按值捕获

`Heap<T>`（非空）出现在 `FV(λ)` 中 → 编译错。

- `Heap<T>?`（可空）→ ✅ 接受，走 §4.4 B 档 move。
- 理由：非空 `Heap<T>` 不允许复制 / move，只能以 `T&` 借用形式传入；lambda 捕获需要值语义。引导用户声明为 `Heap<T>?` 可空形态。

### 7.4 E4020 / E4021 — lambda 返回 T& 溯源

由 `borrow_checker.cpp` 检查，与 §8.6.10 同构：

- lambda 的"允许源集" = 形参中类型为 `T&` 者。
- 捕获来的 `T&` **不进**允许源集。
- ret expr 的 T& 根 ∉ 允许源集 → **E4020**。
- retType 为 T& 但允许源集内 T& 形参数 ≠ 1 → **E4021**。

### 7.5 E2029 — 不支持的捕获类型

`FV(λ)` 中出现既非标量/堆句柄/T&/Heap? 也非上述已知类型的变量 → E2029（compiler_lambda.cpp 捕获写入阶段）。（当前覆盖：Fn fat-ptr / 嵌套 lambda 等，这些类型捕获在 v1 未实现。）

---

## 8. 嵌套闭包

lambda 体内再写 lambda → 内层 lambda 的 `FV` 会包含外层 lambda 的形参 + 外层 lambda 的捕获变量（`CapturesT` 字段）。编译器**逐层展开**：

- 内层 lambda 的 `FV` 首先按 §3.1 规则计算。
- 内层 lambda 引用外层 lambda 的捕获变量 `x` → `x` 的类型在外层 `CapturesT` 中已确定（§4），内层按同样规则判定捕获模式。
- 实现上：外层 lambda 编译时 `_currentLambdaForCapture` 嵌套 → 内层 `compileLiteralExpr` 命中 `x`（在外层 `_currentLambdaBodyScope` 可解析但在内层 `_localVarPtrs` 不在）→ 向外层 `_currentLambdaForCapture` 递归 addCapture → 内层通过 GEP 读外层 captures buffer 拿到值。

> 当前实现（compiler_lambda.cpp）已支持单层嵌套的 `Rc`/标量捕获；多层嵌套 + `T&` 捕获的嵌套闭包需要在 sema 阶段额外验证（见 Open Issue O1）。

---

## 9. 错误模型交互

### 9.1 #Fallible lambda

lambda 字面量自身不携带 `#Fallible` 注解。lambda body 内的错误行为取决于：

- lambda body 内调用了 `#Fallible` fn 并使用 `!` 传播 → 该 lambda 的 fn 类型变为 fallible（编译器在 emitLambdaFunction 时自动在返回类型上套 `#Fallible` 对应 enum）。
- 当前实现（compiler_lambda.cpp）未显式处理 `#Fallible` lambda 的返回类型合成，codegen 沿用的 `emitLambdaFunction` 以 `retType` 直接生成 LLVM FunctionType，错误传播的 lowering 在 body 编译过程中由 `compileExpr` 对 `!` 后缀的处理自然完成。

**v0.16 评估**：当前无已知 `#Fallible` lambda 的 bug 报告。`try-catch` 在 lambda 内已有 codegen 路径。本草案将此项列为 Open Issue O2，待 v0.x+1 专项。

---

## 10. 不在范围

- 显式捕获列表语法（保持 [#18] 决议：类型即文档，不引入 `[move x]` 等）
- `#Inline` / `#CallOnce` 注解（推 v0.x+1）
- lambda 内对捕获变量赋值放开（需 DAA 透传 / inline，推 v0.x+1）
- 泛型 lambda 字面量（`<T>(x T) => ...`，推 v0.x+1）
- 函数值跨 FFI 边界（推 v0.x+1）
- 捕获包 layout 优化（字段重排去孔、小尺寸 SBO 等）

---

## 11. 迁移面

> 本节为实施分解的种子。当前捕获 codegen 已完整落地（compiler_lambda.cpp），主要工作集中在 sema 接管。

### 11.1 编译器（`src/`）

- `src/sema/sema_pass.cpp`：解锁 lambda body 下钻；新增 E2030/E4022/E4024 静态判定；更新 `kMigratedCodes`。
- `src/analyzer/borrow_checker.cpp`：递归进入 lambda body；注册 lambda 形参 T& 根；ret T& 溯源。
- `src/compiler/compiler_lambda.cpp`：E2030/E4022/E4024 原 throw 已移除（v0.16 收尾）。
- `src/compiler/compiler_stmt.cpp`：E2030/E4022 原 throw 已移除（v0.16 收尾）。
- `src/compiler/expr/expr_literal.cpp`：E4024 原 throw 已移除（v0.16 收尾）。

### 11.2 测试（`tests/`）

现有覆盖（14 用例 + 1 诊断）保持不变；sema 接管后 yux-check 路径新增覆盖验证。

### 11.3 规范文档（`docs/spec/`）

- `docs/spec/04-表达式.md` §4.11.6：引用本草案条款号
- `docs/spec/08-所有权与引用.md` §8.7.6：引用本草案条款号
- `docs/spec/draft/DRAFT-lambda.md`：头部加注"§5–§6 已迁至 DRAFT-closure-capture.md"
- `docs/spec/CHANGELOG.md`：追加 v0.16 条目

### 11.4 用户教程

- `docs/Lambda与闭包.md`：按本草案术语更新捕获模式章节

---

## 决议日志

- **2026-05-08 [#17]** 函数值 RC/ABI 选 C1：永远 fat-ptr `{fn_ptr, captures Rc<CapturesT>?}`，零捕获 captures = null。调用 ABI 统一，captures 永远作隐式首参。
- **2026-05-08 [#18]** 闭包捕获默认按外层变量类型自动选：标量/struct 复制，堆句柄 retain，T& 借用透传。**不引入**显式捕获列表语法。
- **2026-05-08 [#19]** 含 T& 捕获的 lambda：CapturesT 不进 Rc，改栈嵌入；lambda 按"广义 T&"不可逃逸。
- **2026-05-08 [#20]** 方法体内 lambda 引用 `$` 合法，按隐式 Self& 捕获，触发 [#19] 不可逃逸。
- **2026-05-08 [#21]** lambda 返回 T& 的允许源集 = 形参 T& 者；捕获 T& 不进允许源集。
- **2026-05-08 [#26]** lambda 体不得对捕获变量赋值（含复合赋值/句柄重绑）。
- **2026-05-08 [#27]** lambda 字面量在 AST 层是普通 fn 实参；不享有 inline/callonce。
- **2026-05-08 [#28]** v1 不引入 `#Inline` / `#CallOnce`。
- **2026-05-09 Phase 4a–4e** 全部落地：标量 + 堆句柄 + T& + `$` 捕获 codegen 完成。
- **2026-06-10 [本草案]** 从 DRAFT-lambda.md §5–§6 提炼独立草案；新增 Heap B 档 move（Phase 3e）；定型三档命名（move/retain/borrow）；保持 [#18] 不引入显式捕获列表。

---

## Open Issues

- **O1**：嵌套闭包中 `T&` 捕获的 sema 验证（多层嵌套下栈嵌入 captures 的寿命链路）。当前 codegen 已支持单层，多层 + T& 组合路径待验证。
- **O2**：`#Fallible` lambda 形态的规范定型。当前无已知 bug，codegen 自然工作；留 v0.x+1 专项。
- **O3**：~~`Heap<T>?` 捕获在 body 内多次调用的所有权语义~~ — **关**（v0.18）：env 持有，多次调用读同一 payload，无需额外条款。
- **O4**：~~`[T * N]` 定长数组的捕获~~ — **关**（v0.18）：按值类型走 move 档，归入 §4.1；不单列。

---

## 定型与归宿

定型后按 `_模板.md` 末尾流程拆分：

1. §4.11.6 / §6.5.5 / §8.7.6 条款逐项核对，引用本草案条目编号（如 [#18]）保留可追溯性。
2. `docs/spec/CHANGELOG.md` 顶部追加 v0.16 条目。
3. DRAFT-lambda.md 头部加注"§5–§6 已迁至 DRAFT-closure-capture.md"。
4. `CURRENT.md` Phase 列表从 §11 派生；全部完成后归档到 `docs/dev/closure-capture-impl-log.md`。
5. 本 DRAFT 头部标注「已落地，见 §4.11.6 / §8.7.6」并保留为历史档。

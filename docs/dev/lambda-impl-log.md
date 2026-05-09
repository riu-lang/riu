# Lambda 与函数类型 v1 实施日志

本文件归档 Lambda 与函数类型 v1（含闭包）的实施记录：核心决策、ABI 协议、关键代码点。是后续回答"fn 值为什么是 fat-ptr"、"`T&` 捕获为何走栈嵌入"、"为何 lambda 体不能赋值给捕获变量"等问题的事实来源。

- 规范条款见 `docs/spec/03-类型系统.md` §3.11 / `04-表达式.md` §4.8.4 / §4.11 / `06-函数.md` §6.5.5 / `08-所有权与引用.md` §8.7.6
- 决议草案见 `docs/spec/draft/DRAFT-lambda.md`
- 用户教程见 `docs/Lambda与闭包.md`

---

## 总策略

**先一次性改 g4，再分 Phase 实现**。SDK 扩充（`Array::map` / `try` / `each` 等）不在此任务，依赖错误模型与当前实施一并推后。

整体落地为 6 个 Phase：

| Phase | 范围 |
|---|---|
| 0 | g4 改动（Lexer / Parser / 附录 B） |
| 1 | 函数类型字面量（声明 / 别名 / 等同），不生成调用代码 |
| 2 | Lambda 字面量 + 直接调用（零捕获） |
| 3 | 函数值 RC / ABI 完整路径（字段 / `Array<fn>` / `Box<fn>`） |
| 4 | 闭包（标量 + 堆句柄 + `T&` + `$` 捕获 + 不可逃逸 + 返回 `T&` 溯源 + FFI 边界） |
| 收尾 | spec 正文回写 / 教程 / 草案归档 |

## Phase 0 — g4 改动

- `yuxLexer.g4 SymbolEqMt = '=>'`（已存在）
- `yuxParser.g4`：
  - `type → typeFn`：`Fn '?'? '(' fnTypeParams? ')' retType=typeWithRef?`
  - `typeWithRef → typeFnWithRef`：同形 + 末位 `&`
  - `fnTypeParams / fnTypeParam`：名可省 + 组糖 `a, b T`
  - `expr` 新增四 lambda 选择支：`exprLambdaSingle` / `exprLambdaParen` / `exprLambdaBlock` / `exprLambdaZeroBlock`
  - `lambdaParams / lambdaParam`：类型可省 + 组糖
  - `lambdaBody` 包装规则（修 `(a, b) => a + b` 被切成 `((a, b) => a) + b`）
  - `callExpr` 扩展 + `trailingLambda`：`f(args) { ... }` 与 `f { ... }`（spec §4.8.4）
  - `typeWithRef` 选择支重排：`typeNullableWithRef` 上提到 `typeNormalWithRef` 之前（修 `fn(s String)i32?` 被错切成 `(fn(s String)i32)?`）

**已发现/已修的歧义**：

- `T?` 在 `typeWithRef` 里被拆错 → 重排选择支顺序解决
- `(a, b) => a + b` 中 lambda 体只取 `a` → `body=expr` 改为走 `lambdaBody` 包装规则解决

## Phase 1 — 函数类型字面量

仅类型层：能写 `var f fn(i32)i32`、`Callback = fn(s String)bool`、`Array<fn()i32>`；不生成调用代码。

- AST：`TypeFnNode`（`type_node.h`）+ `TypeInfo::Fn` kind（`include/types.h`）；ast_builder `visitTypeFn` / `buildTypeWithRef`；`TypeInfo::substitute` / `==` / `getFullName` / `getGenericMangleName` 支持 Fn
- 类型系统：`getLLVMType` 加 Fn case → 16-byte fat-ptr `{ ptr, ptr }` 占位；`resolveAliasImpl` 加 Fn 递归（fn 形参 / 返回类型内的别名透明展开）
- 结构等同复用 `TypeInfo::operator==`：参数名不参与；arity + 各位置类型 + 返回类型递归判等；void 缺省作 nullptr 参与判等
- §3.7 容器禁忌：`Weak<fn(...)>` 在 ast_builder `visitTypeGeneric` / `buildTypeWithRef` 双通路抛 `E2001`

## Phase 2 — Lambda 字面量 + 直接调用（零捕获）

### 2a — AST + Parser desugar 骨架

- `LambdaExprNode` + `LambdaParamSlot` 落 `expr_node.h`；`getType()` 构造 Fn TypeInfo（缺类型槽位用 empty TypeInfo 占位，等 2b 反推回填）
- visitor：`visitExprLambdaSingle` / `visitExprLambdaParen` / `visitExprLambdaBlock` / `visitExprLambdaZeroBlock`；helper `collectLambdaParams` 处理 lambdaParamGroup / lambdaParamStd 两 alt
- Parser desugar：`visitExprCall` 末尾消费 `ctx->trailing`，把 trailingLambda 转 LambdaExprNode 追加到 args 末位；新增 `visitExprCallTrailingOnly` 处理唯一 trailing 实参糖；helper `buildTrailingLambda` 复用于两路
- `LambdaExprNode::getType()` 返回 `TypeInfo(FnTag, params, retType, false)`，nullable=false（lambda 字面量本身永非空）

### 2b — codegen 零捕获

ABI 决议（**[#17] C1**）：函数值永远 fat-ptr `{ fn_ptr, captures Box<CapturesT>? }`，零捕获时 captures = null。函数值是值类型 + 字段级 RC（不进堆句柄档位）；调用约定统一（captures 永远作隐式首参传 fn_ptr）。

- `Mangler::lambda(mod, line, col)` → `__lambda_<sanitized-mod>_<line>_<col>`
- `compiler_lambda.cpp`（新文件）：
  - `emitLambdaFunction`：ABI captures-leading `(Ptr captures, P1, ..., Pn) → R`；保存/恢复外层 `_currentFn / _currentFnNode / _localVarPtrs / _scopeVars / _tempStack / IRBuilder` 插点
  - `compileLambdaExpr`：InsertValue 构造 `{fn_ptr, null}` 16-byte 值
  - `compileFnValueCall`：从 fat-ptr extractvalue fn_ptr / captures，按 ABI `fn_ptr(captures, args...)` 调
- `LambdaExprNode` 加 `_bodyScope`（`LambdaScopeNode` 子类，ast_builder `visitExprLambda*` 中 push/pop `_scopeStack` 并登记形参符号）+ `_inferredFnType`（调用点反推后回填）
- `compileCallExpr` 早期对 ID-callee 的唯一 arity FnSymbol 做 lambda 反推：对 Fn 形参位置上 LambdaExprNode 实参调 `setInferredFnType` + 同步 bodyScope 形参符号 type + 预 emit Function（mangle 缓存命中后续 `compileLambdaExpr` 调用）
- `compileLiteralExpr` 兼容 `_currentFnNode == nullptr`：fall back 到 `node->findNearestScope()->lookupSymbol`，让 lambda body 内形参引用可达
- `ExprCallNode::getType` 加 `isFn()` callee 分支：直接返回 `fnReturnType()` 或空（void）

### 2c — sema：括号强制 / FV 校验存根 / if-match 体

- §4.3 括号强制：g4 已强制 —— `exprLambdaSingle` 仅接受单 ID 裸参；多参 lambda 必走 `exprLambdaParen` 已带括号；不合规裸多参列在分词阶段被切成多 args，无解析歧义遗留
- FV 校验存根：`emitLambdaFunction` 期间设 `_currentLambdaBodyScope`；`compileLiteralExpr` 的 LiteralObj 路径若 sym 是 Variable 且既非 lambda 形参（不在 `_localVarPtrs`）也非全局（无 globalVar）→ 抛 `E2028`（Phase 4a 后改 `E2029` 兜底，本错误码作"FV 通路未启"前置）
- §4.2 返回类型校验三段式：显式标注严格用之；Paren 无标注 → 强制 void（不接受上下文反推）；裸 / Block / ZeroBlock 无标注 → 取 expected `fnReturnType`，无上下文则兜底 void

## Phase 3 — 函数值 RC / ABI 完整路径

零捕获只占函数值的一半语义；Phase 3 把"函数值进入 §7.4 字段级 RC 系统"骨架打齐，让 §5.5 表格里 ✅ 的形态全部能跑。

### 3a — RC 接入零捕获骨架

- `typeNeedsDestructor`：`type.isFn()` → true（fn 值含 captures `Box<...>?` 字段，逻辑上需要 release）
- `releaseAtPtr` Fn 分支：GEP `{0,1}` captures slot，load，调 `_box_release`（runtime 已 null-safe）
- `retainHandleAtCallSite` Fn 分支：extractvalue `{1}` captures，IR 级 null guard（`_box_retain` 不做 null 检查）+ `_box_retain`
- `callFieldDestructor` Fn 字段 + `retainStructFieldsAtCallSite` Fn 字段：复用 captures retain/release（field 路径 retain 同样加 null guard）
- `compileDeclareStatement` Fn 类型未初始化变量零填充 fat-ptr，避免析构期读栈垃圾段错

### 3b — struct 字段位置

`resolveStructFieldTypes` / `getLLVMType` 路径已天然覆盖 fn 字段（Phase 1 已支持 `TypeInfo::Fn` → 16-byte fat-ptr struct）；构造路径 `$.h = h` 走通用 store + RC 钩子，方法路径 `$.h(x)` 走 fn-value-call。无新代码。

### 3c — `Array<fn>` / `Box<fn>` 容器

- `Array<fn(...)R>` 元素 size = 16 bytes：既有 elem-typed RC 钩子已天然覆盖，无新代码
- `Box<fn(...)R>` 自动解引 + 调用：新增 `compileBoxFnValueCall`（`compiler_lambda.cpp`），`compileCallExpr` 早期对 `calleeStaticType.isBox() && inner.isFn()` 分支接入；payload = handle + 8 字节 load fat-ptr 后走与 `compileFnValueCall` 同款 ABI；`ExprCallNode::getType` 同步加 Box<fn> 分支返回 inner fn 返回类型（修复 `b(5).to_string()` 链式调用类型推断）

### 3d — `(fn(A)R)&` 借用 — **弃**

g4 没有"括号包类型"规则，且 `typeFnWithRef` 末尾的 `SymbolAnd?` 在有非借用 R 时被 `retType=typeWithRef` 的贪心 `&` 吃掉（`fn(i32)i32&` → 返回 `i32&`，外层 `&` 永远轮不上），无语法表达手段。spec §3.2.3.1 / §3.6 / §5.5 已标"v1 不支持，g4 缺 paren-type 规则 + retType 贪心吞 `&`"。

## Phase 4 — 闭包

### 4a — FV 收集骨架（标量 only）

- AST：`CaptureSlot {name, type, byteOffset}` + `vector<CaptureSlot> _captures` + `_capturesTotalSize` 落 `expr_node.h`；`captures()` / `findCapture()` / `addCapture()` / `clearCaptures()`
- 编译器状态：`_currentLambdaForCapture: LambdaExprNode*` + `_currentLambdaCapturesArg: llvm::Value*`（`emitLambdaFunction` 期间有效）
- `emitLambdaFunction`：body emit 前 `clearCaptures()` 防累加 + 设 `_currentLambdaForCapture` + 缓存 captures 形参（block ptr）；emit 完恢复
- `compileLiteralExpr`：lambda body 内 sym 在外层 local 但不在 `_localVarPtrs` / 全局 → 检查 `_currentLambdaForCapture` 启用：
  - 非标量 → `E2029`
  - 标量首次出现 → `addCapture(name, type, offset=cur*8, total=cur+8)`；复用已有槽位 → 取 idx
  - GEP `payload+8+byteOffset` + Load typed value
- `compileLambdaExpr`：emit 后查 `node->captures()`：空 → captures = null（旧零捕获路径）；非空 → `_box_alloc(totalSize)` + 逐 capture 从外层 `_localVarPtrs` load + GEP store 到 captures box payload
- 8 字节固定槽位（spec 不锁布局）

### 4a-2 — 堆句柄捕获（Box / Weak / Array / String）

**Captures box 布局升级**：payload 头 8 字节加 dtor fn ptr 槽位；capture 字段从 payload+8 起。GEP 全部从 `handle+8+byteOffset` 改 `handle+16+byteOffset`；分配大小 `8 + capturesTotalSize`。

- 新增 runtime `_box_release_dtor(handle)`（`compiler_runtime.cpp`，`emitBoxHelpers` 一并 emit）：与 `_box_release` 同形 + 在 strong 归零时先调 `payload[0..8]` 处 dtor 函数 `dtor(handle+16)` 再走 weak/free —— 多 fat-ptr 副本共享 captures box 时只触发一次字段级析构
- `compiler_expr.cpp` 捕获识别放宽 gate：接收 `t.isBox() / isWeak() / isArrayGeneric() / name=="String"`；其余非标量仍 `E2029`
- `compiler_lambda.cpp`：新增 `emitCapturesDtorFunction(node, mangle)` 合成 `__captures_dtor_<lambda>(ptr fields_base)`：逐 capture 字段调 `releaseAtPtr`（标量字段被 `typeNeedsDestructor` 跳过）；全标量场景返 nullptr → dtor 槽存 null。`compileLambdaExpr` 写入 captures 时为每个 needs-dtor 字段调 `retainHandleAtCallSite`（callee-clean RC）
- `compiler_destructor.cpp`：`releaseAtPtr` Fn 分支 + `callFieldDestructor` 内 fn-字段分支改调 `_box_release_dtor`，把 dispatch 留给运行时

### 4b — 捕获变量赋值禁（spec §6.2.1 [#26]）

- 新错误码 `E2030` "lambda body cannot assign to captured variable"
- sema：`compileAssignStatement` 入口处先检测 `_currentLambdaForCapture && _currentLambdaBodyScope`；对 LHS objName 经 `node->findNearestScope()->lookupSymbol` 解析为 Variable + 不在 `_localVarPtrs` + 无对应 mangled 全局 → 抛 `E2030`。覆盖 `=` / `+= -= *= /= %= <<= >>=`，以及 `obj.f = ...` / `obj[i] = ...` 一并阻在前面（subs 非空时 obj 也是同一 LHS 主体）

### 4c — `T&` 捕获 + 栈嵌入 + 不可逃逸（spec §6.3）

- FV 中存在 `T&` 类型变量 → CapturesT 走"栈嵌入"：父帧 entry 处 alloca [16 前缀 + 字段]，**不构造 Box**；前缀 16 字节与 Box 形态对齐，body GEP base 不依赖路径选择
- captures 指针 LSB 标 1 标记栈嵌入（`alloca | 1`）；body 入口统一掩 LSB（heap 8 字节对齐 LSB=0 不变，stack 还原；null 保持）；`retainHandleAtCallSite` / `releaseAtPtr` 的 Fn 分支检测 LSB=1 跳过 RC 操作
- 不可逃逸（spec §6.3 v1 单源约束）：lambda 字面量直接作 `ret expr` 或 `var/val = ...` 初始化值时，预 emit body 触发捕获识别；若 `hasRefCapture` → 抛 `E4022`。其他形态（变量名 / 调用结果作 ret）属 §6.5 溯源范围
- 4c 限制：栈嵌入路径下不允许混入堆句柄字段（混合释放路径未实现）→ `E2029`
- body T& 捕获访问：load ptr from 槽 → 再 load inner T 实现 auto-deref，与外层 T& 局部读语义对齐
- 错误码：新增 `E4022` "lambda value with `T&` capture cannot escape current frame"

### 4d — `$` 捕获（spec §6.4）

- 方法体内 lambda 引用 `$` / `$.field` / `$.method()` 视作隐式 `Self&` 形参捕获，复用 4c 栈嵌入路径。`$` 已在 ast_builder 处登记为 `Ref<Self>`，FV 通路 `t.isRef()` 命中即走栈嵌入路径，**零代码改动**
- v1 不允许 lambda 内重新指代另一 receiver：lambda 自身无 `$` 形参，`$` 解析经 `findNearestScope` 走到外层方法的 `$`；自然成立
- `$.field = ...` / `$.field += ...` 受 §6.2.1 [#26] / Phase 4b 约束 → `E2030`；用户改内部状态走 mutator-method 绕道（`$.method()` 内的字段写在外层方法层级，不被 lambda 体捕获检查命中）

### 4e — 返回 `T&` 溯源（spec §6.5）

- `src/analyzer/borrow_checker.cpp` `visitExpr` 中递归 `LambdaExprNode`；新 `visitLambda` 保存 / 恢复外层 `_returnsRef / _returnAllowedSources / _returnAllowedDesc`，pushScope + 注册 lambda 形参（`T&` 形参登记 `_refToRoot[p]=p`，纳入本 lambda 单源候选），retType 是 `T&` 时单源约束（`refParams.size()!=1` → `E4021`）
- body 单表达式视作隐式 ret（`rootFromRetExpr` 推根 + 检查允许集），block stmts 走既有 `visitStmt` 通路
- 捕获 `T&` 在外层 `_refToRoot` 保留；resolveRoot 还原到外层根名（如外层 fn 形参 `c` 自身），不在 lambda 自身允许源集 → `E4020`

### 4f — FFI 边界拒绝（spec §7）

- `src/ast/ast_builder.cpp visitExternDelc`：在 paramTypes / retType 计算后扫描 Fn 类型 → 抛 `E2031`（fn(...) 不与 C 函数指针 ABI 兼容；零捕获静态判定推 v0.x+1）
- 新增错误码 `E2031` "extern fn `{}` cannot use fn(...) types in {}"

## ABI 协议（汇总）

### 函数值布局

```
fat-ptr 16 bytes:
  [0..8]   fn_ptr      ; 顶层匿名 fn 的代码地址
  [8..16]  captures    ; nullable Box<CapturesT> handle，零捕获时 null
```

### 调用约定

统一 captures-leading：

```
fn_ptr(captures, P1, ..., Pn) → R
```

零捕获也传 captures（值为 null），调用约定不分裂。

### Captures box 布局

堆形态（Phase 4a-2 起）：

```
payload[0..8]    dtor fn ptr    ; null 表全标量；非 null 时由 _box_release_dtor 调 dtor(handle+16)
payload[8..]     capture fields ; 每槽 8 字节固定，spec 不锁布局
```

栈形态（Phase 4c 起，hasRefCapture 触发）：

```
alloca [16 前缀 + fields]
ptr LSB 标 1            ; 调用站点和字段位置 retain/release 检测 LSB=1 跳过
body 入口掩 LSB        ; 还原 alloca 真实地址
```

布局前缀 16 字节统一：堆形态 = RC 头 8 + dtor 槽 8；栈形态 = 16 字节占位浪费。body GEP base offset 不依赖路径选择。

### RC 协议

- 函数值是值类型 + 字段级 RC（**不**进堆句柄档位）；`typeNeedsDestructor(Fn) = true`
- `releaseAtPtr` Fn 分支：GEP captures slot → load → `_box_release_dtor`（栈嵌入 LSB=1 跳过）
- `retainHandleAtCallSite` Fn 分支：extractvalue captures → null guard → `_box_retain`（栈嵌入 LSB=1 跳过）
- 字段位置 `struct S { h fn() }` / `Array<fn>` / `Box<fn>` 复用上述钩子

## 关键文件 / 函数索引

| 文件 | 责任 |
|---|---|
| `src/yux*.g4` | 函数类型 / lambda 四形态 / 尾随 lambda 糖 |
| `src/ast/node/expr_node.{h,cpp}` | `LambdaExprNode` / `LambdaParamSlot` / `CaptureSlot` / `_bodyScope` / `_inferredFnType` |
| `src/ast/node/type_node.h` | `TypeFnNode` |
| `src/ast/ast_builder.cpp` | `visitExprLambda*` / `visitTypeFn` / `buildTypeWithRef` / `visitExternDelc`（FFI 拒绝） |
| `src/ast/mangler.cpp` | `Mangler::lambda(mod, line, col)` |
| `src/compiler/compiler_lambda.cpp` | `emitLambdaFunction` / `compileLambdaExpr` / `compileFnValueCall` / `compileBoxFnValueCall` / `emitCapturesDtorFunction` |
| `src/compiler/compiler_call.cpp` | `compileCallExpr` 中 fn-callee / Box<fn>-callee 分支 + lambda 反推预 emit |
| `src/compiler/compiler_expr.cpp` | `compileLiteralExpr` 内 FV 通路 + 标量/堆句柄/`T&` 分类 + `_currentLambdaBodyScope` 兜底解析 |
| `src/compiler/compiler_destructor.cpp` | Fn 分支 RC 钩子（5 处：`typeNeedsDestructor` / `releaseAtPtr` / `retainHandleAtCallSite` / `callFieldDestructor` / `retainStructFieldsAtCallSite`） |
| `src/compiler/compiler_runtime.cpp` | `_box_release_dtor` runtime helper |
| `src/compiler/compiler_stmt.cpp` | `compileDeclareStatement` Fn 零填充 + `compileAssignStatement` 入口 `E2030` 检查 |
| `src/analyzer/borrow_checker.cpp` | `visitExpr` LambdaExprNode 递归 + `visitLambda` 单源约束 + 捕获 `T&` 溯源排除 |
| `include/types.h` | `TypeInfo::Fn` kind + 结构等同 |
| `include/error_code.h` | `E2028`（FV 兜底）/ `E2029`（不支持的捕获）/ `E2030`（捕获写禁）/ `E2031`（FFI 拒）/ `E4020`（捕获 T& 出现在 ret 允许源外）/ `E4021`（多形参 T&）/ `E4022`（含 T& 捕获 lambda 逃逸） |
| `tests/cases/lambda_*` | 用例族（type / call / closure / extern 子组） |

## 错误码

| 码 | 触发 | 来源 Phase |
|---|---|---|
| `E2001` | `Weak<fn(...)>` | 1 |
| `E2028` | FV 通路未启的兜底（lambda 体引用外层 local，非标量场景） | 2c（4a 后多由 E2029 兜底） |
| `E2029` | 不支持的捕获（struct / enum / fn 字段 / 栈嵌入混堆句柄） | 4a / 4a-2 / 4c |
| `E2030` | lambda 体对捕获变量赋值（含复合赋值 / 字段写 / 索引写） | 4b |
| `E2031` | extern fn 形参 / 返回值含 `fn(...)` 类型 | 4f |
| `E4020` | lambda 返回 `T&` 但根来自捕获 | 4e |
| `E4021` | lambda retType `T&` 但形参 `T&` 数 ≠ 1 | 4e |
| `E4022` | 含 `T&` 捕获的 lambda 作 ret / var-decl 初始化值 | 4c |

## 跨 Phase 的 TODO 汇总

- **泛型 lambda**（`<T>(x T) => x`）/ **泛型 fn 类型字面量**（`fn<T>(T)T`）：v1 弃，推 v0.x+1。多数需求由"外层泛型 fn + 内层单态 lambda" + 泛型类型别名 `Predicate<T> = fn(x T)bool` 已覆盖
- **`(fn(A)R)&` 借用**：v1 弃，g4 没有"括号包类型"规则；`typeFnWithRef` 末尾 `SymbolAnd?` 在有非借用 R 时被 retType 贪心吞
- **零捕获 fn 值跨 FFI 边界**：v1 显式不支持；推 v0.x+1 引入"零捕获"静态判定 + `ptr_of:<fn(...)>` / `to_fn:<fn(...)>` baked builtin + 调用约定差异化
- **`#Inline` / `#CallOnce` 注解**：v1 不引入；上线后将解锁 lambda 内对外层 `var` 的赋值直通 + 性能（无 fat-ptr 调用 + 无 captures 分配）+ 非局部返回 / break-continue 穿透
- **嵌套闭包 FV 二级捕获**：spec 留实施细节，未明文。当前实现按词法作用域逐层解析，嵌套 lambda 把内层 FV 再展开到外层 FV
- **栈嵌入混堆句柄字段**：当前 4c 直接 `E2029` 拒；将来若需，需补"栈嵌入 captures 的字段级 dtor"路径
- **`Array::map` / SDK 高阶函数**：依赖错误模型 v1，与本任务并行立项，未在此 Phase 做
- **TODO：函数值上的 method（`.compose(g)` / `.curry()`）**：走库函数，不进语言核心

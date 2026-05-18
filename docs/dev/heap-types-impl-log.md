# Heap<T> 类型族实施日志

`DRAFT-heap-types.md` Phase 2-8 的落地记录。Phase 1（Box→Rc 改名）独立归档在 `rc-rename-impl-log.md`，本日志不重复。

- 规范条款：`docs/spec/03-类型系统.md` §3.3（Heap 档位）、`07-结构体.md` §7.1.3.1 / §7.4.6（字段层）、`08-所有权与引用.md` §8.3a（Heap 全套）、`09-内置类型.md` §9.5a（运行时 / `__yux_heap_alloc/_free`）、`11-编译期注解.md` §11.2.3.2（`copy_of` / `as_ref` / `ptr_of` / `Heap:<T>(Ptr)` 重载）
- 诊断码：附录 D §D.3.D 新增 E4023..E4028
- 草案归档：`DRAFT-heap-types.md` 头部"已落地（Phase 2-8）"

---

## 核心决策

- **Heap<T> 物理形态 = 裸 `T*`**（不是 wrapper struct）。与 `Rc<T> = { ptr handle }` 形成对比：Heap 无 RC 头 / 无 retain 计数；句柄即指针。这决定了 `as_ref(Heap<T>) → T&` 是 GEP 偏移 0（vs Rc 偏移 8 跳 strong/weak）、`copy_of(Heap<T>)` 必须深拷（vs Rc 浅拷 handle）、`ptr_of(Heap<T>) → Ptr` 直接返回 args[0]。
- **Heap<T>? = `Nullable<Heap<T>>` = `{ i1 _has, ptr _value }` 16 字节**。是唯一支持 move 的形态（"B 档 nullable move"）；非空 `Heap<T>` 禁 by-value move（E4024），避免双 owner。
- **单 owner 不参与 retain**：`retainHandleAtCallSite` 故意不加 Heap 分支。所有"传递 Heap" 形态要么是 NRVO 同名 ret（A 档）、要么是 nullable B 档调用点写回源槽 null、要么是 ptr_of/take-over FFI 边界（Phase 8）。
- **C 档（复合 move）未落**：spec §5.3 跨函数 `fn build(b Builder) Built { ret Built { buf: b.buf } }` 要求全 struct move-in ABI 改造，工作量与收益不匹配，留专项。
- **scope-end 释放序**：先 inner T 的 dtor（递归 RC 字段）→ `__yux_heap_free(ptr)`。`__yux_heap_free` 对 null 安全；nullable 形态按 `_has` 跳过 inner dtor。
- **lambda 捕获 Heap<T>? 走 B 档**：env 写入后把 outer slot `{_has=false, _value=null}` —— lambda env 是 snapshot 语义，outer slot 不会被 lambda 体读，安全 null 化。非空 `Heap<T>` 捕获 → E4024（引导改可空）。
- **borrow_checker `_returnsHeap`**：只放行 `ret <local Heap ID>` 形态触发 NRVO；其他形态（fresh ctor / 调用 / 字段）一律 E4023 escape。

---

## Phase 2 — Heap<T> 非空形态最小集

- AST `TypeHeapNode` / `ExprHeapCtorNode`；ast_builder 在 `visitExprCall` 末段单点拦截 `Heap:<T>(x)` → `ExprHeapCtorNode`（与 `Dyn<D>` 同形）
- `include/types.h` `TypeInfo::isHeap()` / `heapElementType()`
- `getLLVMType(Heap<T>) = ptr`；`compileHeapCtorExpr`：`__yux_heap_alloc(sizeof(T))` + `store args[0]` + `consumeTemp(args[0])`
- runtime `src/runtime/heap_handle.{h,cpp}`：`emitHeapHandleHelpers` 注入 `__yux_heap_alloc(i64) → ptr` / `__yux_heap_free(ptr)` (Win32 HeapAlloc/HeapFree，null 安全)
- borrow_checker：decl-assign / 顶层重赋 RHS 必须是 `ExprHeapCtorNode`（禁 by-value move）；`Heap<T>` 形参 / 返回类型 / 字段位 允许
- `as_ref(Heap<T>) → T&`：sema 接受 Heap arg，codegen 直接返回 args[0]（无头）
- 字段持 `Heap<T>`：`releaseAtPtr` 加 Heap 分支（load ptr + inner dtor + heap_free）；struct 字段层走 `callFieldDestructor` 同款
- 测试 `sdk/yux/src/yux/core/heap.test.yux`：`scalar_ctor_and_as_ref` / `struct_ctor_field_read` / `mut_reassign_fresh_ctor` 等基础组

## Phase 3 子项

### 3a — FFI E4028
extern fn 形参 / 返回类型见 Heap → E4028。validateExternSignature 加 isHeap 检测。

### 3b — Arc<T> 占名 E4029
`Arc<T>` 名字占住，构造 / 使用一律 E4029（v1.x 多线程主题再实装）。

### 3c — A 档 NRVO（MVP）
- borrow_checker `_returnsHeap` 检查放行 `ret <local Heap ID>` 形态；其他形态 E4023
- decl-assign / 顶层重赋 RHS 接受 `ExprCallNode` 返回 Heap<T>（fresh handover from NRVO call）
- codegen 复用既有 ret 路径 + `_scopeVars` 摘除 peephole 跳作用域尾 dtor
- 未做：E4026 多源 / 外发借用冲突检测（依赖借用流分析延伸）；moved_out slot bit（3d 引入）

### 3d — Heap<T>? + B 档 nullable move

- **3d.0** baked intrinsic `heap_some<T>(v) Heap<T>?` / `heap_null<T>() Heap<T>?`（`compileKnownFunctionCall`）
- **3d.1** E4027 widen ban（`Heap<T>` → `Heap<T>?` 隐式 widen 禁）+ Heap<T>? lvalue 形态白名单（null literal / 同型 fresh call）
- **3d.2** B 档 nullable move 最小集：`compileKnownFunctionCall::recordBdangIfEligible` 识别 byval `Heap<T>?` 形参 + ID lvalue 实参，调用后 caller 槽 `{_has=false, _value=null}`；`releaseAtPtr` / `typeNeedsDestructor` 补 Nullable<Heap<T>> 分支
- **3d.3** B 档形态扩展：
  - `tryHeapNullableLvalueSlot` helper 抽出（支持局部 ID / 局部 struct 字段）
  - struct-lit `Self { .field = b.heapField }` 字段层 move-out + 调用点字段 lvalue 实参（`b.field`）
  - `callFieldDestructor` 补 `Nullable<Heap<T>>` 字段分支（修 pre-existing JIT lookup-failed）
  - 未落：§5.3 跨函数（C 档，待全 struct move-in ABI）；索引 lvalue `arr[i]`（依赖 Array<Heap> 语义）；if-else flow merge 字段窄化

### 3e — Lambda 捕获 Heap

- sema (`compiler_expr.cpp`) 接受 `Heap<T>?` 捕获 / 拒非空 `Heap<T>` → E4024
- 按 type allocSize 计算 slot 大小（Heap<T>? 16 字节，其余 8）—— 修 pre-existing capture-overlap bug
- codegen (`compiler_lambda.cpp`) 捕获 Heap<T>? 走 B 档：写 env 后把 outer slot `{_has=false, _value=null}`，跳 retain
- env dtor 通过 typeNeedsDestructor / releaseAtPtr 既有路径覆盖
- 借用形态 `Heap<T>?&` 走既有 ref 分支零代码改动；逃逸由 E4022 兜底

### 3f — copy_of 扩展 Heap

- `compiler_call.cpp::copy_of` 在通用分派前插入 `T.isHeap()` 与 `T.isNullable() && nullableInner.isHeap()` 两条分支
- Heap<T>：`__yux_heap_alloc(sizeof(T))` + 写入源 inner T + 递归 retain inner 的 RC 字段
- Heap<T>?：cond-br + alloc BB + cont BB + PHI（按 `_has` 选 null / new ptr）
- sema `hasRefDeep` 把 `t.isHeap()` 加入"不展开"列表（与 Rc/Weak/Array 同级）
- 修正既有 bug：之前 `copy_of(heap)` 走通用 retain → no-op + bitwise return → `a` / `b` 共享同一 ptr → 双 owner 双释放

## Phase 8 — FFI Heap ↔ Ptr 互转

- `ptr_of:<Heap<T>>(h Heap<T>) Ptr` baked：sema 接受 `T.isHeap()` + 强制 ID-literal 实参；codegen `extractRawPtr` 加 Heap 分支（args[0] 即裸 ptr）；调用点摘除 source `_scopeVars` + slot 写 null（防御性）
- `Heap:<T>(p Ptr) Heap<T>` baked take-over：sema/codegen `compileHeapCtorExpr` 识别 `argType.isPtr() && innerType.name != "Ptr"` 形态时跳 alloc+store，直接返回 Ptr 值；scope 尾走既有 Heap<T> dtor
- 未做：**8c 上下文门控**（仅 `extern` / `#FFI` 注解上下文允许互转）—— 留 TODO，与 FFI 完善合并做。当前任何上下文都允许，行为正确但缺误用兜底。

---

## 改动面清单

### 编译器 / runtime

- 新增：`src/runtime/heap_handle.{h,cpp}` (Win32 HeapAlloc/Free wrappers)
- 修改：`include/types.h` (isHeap/heapElementType)、`ast_builder.cpp` (Heap ctor 拦截)、`expr_node.{h,cpp}` (ExprHeapCtorNode)、`type_node.{h,cpp}` (TypeHeapNode)
- 修改：`compiler_call.cpp`（extractRawPtr / as_ref / copy_of / heap_some / heap_null / call-site B 档写回）、`compiler_expr.cpp`（compileHeapCtorExpr + lambda capture 接受 Heap?）、`compiler_lambda.cpp`（env 写入 + outer slot null 化）、`compiler_destructor.cpp`（Heap / Nullable<Heap> 释放序）、`compiler_types.cpp`（getLLVMType Heap）、`compiler_stmt.cpp`（decl-assign Heap RHS 形态接受）
- 修改：`sema/call_resolve.cpp`（hasRefDeep skip Heap、ptr_of 接受 Heap、validateCompilerInnerIntrinsicTypeShape ptr_of / copy_of / as_ref Heap 分支）、`sema/sema_pass.cpp`（visitExpr ExprHeapCtorNode、kMigratedCodes E4023-E4028 加白名单）
- 修改：`analyzer/borrow_checker.cpp`（_returnsHeap NRVO / decl-assign 白名单 / 重赋 / Heap<T>? 白名单 / E4027 widen ban）
- 修改：`include/error_code.h`（E4023..E4028 + E4029 Arc 占名）

### SDK / 测试

- `sdk/yux/src/yux/core/heap.test.yux`：~40 个 #Test 覆盖各 Phase
- `tests/cases/diag_heap_*.yux`：E4023..E4029 诊断回归

### Spec

- `03-类型系统.md` §3.3 Heap 档位 + widen 禁注脚
- `07-结构体.md` §7.1.3 / §7.1.3.1 / §7.4.2 / §7.4.6
- `08-所有权与引用.md` §8.3a 全节 + §8.3.5.5 as_ref Heap 重载 + §8.6.5.9 借用根
- `09-内置类型.md` §9.5a Heap + §9.2.3.4 Array<Heap> 例外 + §9.7.2.4 ptr_of Heap + §9.5.6 Arc 占名
- `11-编译期注解.md` §11.2.3.2 baked builtin Heap 重载
- 附录 C 术语 + 附录 D 诊断 E4023-E4029

---

## 跨 Phase TODO

- **8c FFI 上下文门控**：仅 `extern` 块 / `#FFI` 注解上下文允许 `ptr_of:<Heap>` / `Heap:<T>(Ptr)`，否则警告。与 FFI 完善合并做。
- **C 档（复合 move）**：跨函数 `fn build(b Builder) Built { ret Built { buf: b.buf } }`，要求全 struct move-in ABI 改造，留专项。
- **索引 lvalue `arr[i]`** B 档 move：依赖 Array<Heap> 语义先定（§9.2.3.4）。
- **if-else flow merge** 字段窄化：当前 flow 分析仅 ID 窄化。
- **spec Clone 优先级 for copy_of**：依赖 DRAFT-spec-unify 至少"Spec 形态可识别"完成。
- **Arc<T> 真实实现**：v1.x 多线程主题。
- **运行时 symbol 名 `__yux_heap_*`**：是否走 `_rc_*` 同款 internal linkage 收口，独立小 phase。

---

## 回归

每 Phase 提交时验证：
- `xmake test` 184/184 全绿
- `cd sdk/yux && yux test` 534/534 全绿（含 Phase 8 新增 3 例）

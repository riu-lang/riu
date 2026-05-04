# 所有权与引用 v0.1 实施日志

本文件归档 v0.1 所有权与 RC 模型的实施记录：每个 Phase 的核心决策、Block layout、ABI 协议、关键代码点。是后续回答"layout 为什么这样"、"sentinel 为何为 `0xFFFFFFFF`"等问题的事实来源。

- 设计草案见 `docs/spec/draft/DRAFT-所有权与引用.md`（已入库；不等同规范，以 §8 正文为准）
- 规范条款见 `docs/spec/08-所有权与引用.md`
- 进行中的工作见 `CURRENT.md`（本地）
- 与本日志无关的待修 bug 见 `BUGS.md`（本地）

> 本日志中的"TODO"即未在本期落地、登记到后续版本里程碑的事项；具体技术 bug 不出现在本文，统一记入 `BUGS.md`。

---

## Phase 0 — 语法改动

- `yux.g4` 新增 `typeWithRef`（仅参数 / 局部 var）、`exprGetRef: SymbolAnd obj=(ID|SymbolThis) (SymbolDot subs+=ID)*`
- `T&` 在字段 / 返回值 / 泛型实参 / `cval` / 嵌套 `T& &` 上结构性禁止
- `Weak<T>?` 走 `typeNullable` 后由语义层禁止
- `Ptr` 不入 g4，按普通 ID 在编译器特例化
- `ast_builder.cpp`：`fnParamStd` / `fnParamGroup` / `statementDeclareAssign` 类型访问改走 `typeWithRef()->type()`
- 后续在 Phase 7 把 `typeWithRef` 拆 4 个 alt（`typeNormalWithRef` / `typeNullableWithRef` / `typeGenericWithRef` / `typeArrayWithRef`），新增 `genericDefWithRef` 允许 `Box<i32&>` 这类嵌套

## Phase 1 — Block layout + RC runtime ABI

### 1a — Box 单分配 Block + 双计数 + 哨兵

- Box 实例 layout：`{ ptr handle }` 单字段 struct
- Block layout：`{ u32 strong, u32 weak, payload:T }`，payload 偏移 8
- 运行时：`_box_alloc` / `_box_retain` / `_box_release`：strong 计数 + 哨兵 `0xFFFFFFFF` 跳过；weak 字段 1d 才接入
- 简化：payload 析构由 IR 内联（保留现状），`_box_release` 只负责 strong-- 与整 Block free
- 相关改动：`compiler_destructor.cpp`（变量 / 字段析构两处 Box 分支）、`compiler_call.cpp` 三处（field / receiver / 参数 retain）、`compiler_expr.cpp` 两处（Box→Ptr cast / `box.field` 访问）、`compiler_stmt.cpp` Box 构造
- Box-to-Box 复制路径修正：从用 LHS 名字查 alloca 改为 `src exprVal` 落 tmp alloca 再 GEP，并接入 retain

### 1b — Array：handle + RC 共享语义

- `Array<T>` 实例 layout：`{ ptr handle }`
- Block layout：`{ u32 strong, u32 weak, i64 len, i64 cap, ptr data }`（offset 0 / 4 / 8 / 16 / 24）
- `base.yux` 中 Array 字段定义移除，方法标 `#CompilerInner`，编译器特例化
- realloc 走 `_array_grow(handle, elemSize, newCap)`：在 Block 内原地改 cap、data；句柄稳定
- 运行时：`_array_alloc(elemSize, initCap, initLen) -> Block*` / `_array_grow` / `_array_release` / `_array_retain`，含哨兵 + null 跳过
- 编译器各点全部改走 handle：`compiler_call.cpp` 的 `len/cap/at/first/last/pop/push/clear/set_len/is_empty`；`compiler_expr.cpp` 的 Array 字面量、索引、String 字面量；`compiler_stmt.cpp` 的 var 初始化 / 字面量赋值 / 字段赋值 / `arr[i]=v`

### 1c — String 字面量走 .rodata 哨兵

**决策**：String layout 维持 `{ data: Array<u32> }`（与 Phase 1b 后的 Array<T> 同形为 `{ ptr handle }`）。FAM 内联 Block 是性能优化，推到 v0.11。

**决策**：删 mutator + 重写 `to_string` 依赖 Phase 3 retain/release-on-copy（否则 `$.data = buf` 双重释放）。本阶段只做字面量 `.rodata` 哨兵——零回路、单点改动，先把启动堆分配消掉。其余移入 Phase 2。

- `compiler_expr.cpp` String 字面量分支：替换运行时 `_array_alloc + memcpy` 为 module 级 `.rodata` 全局 Block：`{ i32 0xFFFFFFFF, i32 0, i64 len, i64 len, ptr <const_data> }`（32 字节匹配 Array Block layout）
- 空字面量 `""` 共享 `.str.empty.block` 全局

### 1d — Weak<T> + upgrade

**1d.1 基础设施 + 单线 Weak**

- `types.h` 加 `isWeak()` / `weakElementType()`
- `base.yux` 加 `#CompilerInner struct Weak<T> {}`
- 运行时改 `_box_release`："strong--; if 0 then weak--; if 0 then free block"
- 新增 `_weak_release(handle)`：weak--、归零 free
- 编译器：`getLLVMType` 识别 Weak（`{ ptr handle }`）；Weak 局部变量 / 字段析构调 `_weak_release`；`typeNeedsDestructor` 接 Weak
- `var w Weak<T> = b`（b: Box<T>）写句柄 + weak++ 在声明路径

**1d.2 Weak 复制 retain + upgrade**

- Weak-to-Weak 赋值 / 初始化：retain weak（同 Box 1a，weak++ 走内联 CFG）
- `upgrade(w) Box<T>?` builtin：`base.yux` 声明 `#CompilerInner fn upgrade<T>(w Weak<T>) Box<T>?`；`compiler_call.cpp` CompilerInner 分发；运行时 `_box_upgrade(handle)` 实现 "null / strong==0 → null；哨兵 → handle；其他 strong++"
- `ExprCallNode::getType` 加隐式泛型推断（`unify` 递归 Generic↔Generic）
- TODO：失败路径 runtime 测试推迟到 Phase 5——当前缺 Nullable 内省手段（无 `.has`、无 Box payload 访问、无 `null ==`），无法差分输出

**1d.3 语义禁忌**

- 禁 `Weak<T>?`：`ast_builder.cpp::visitTypeNullable` 检 inner 为 Weak 时报错
- 禁 `Weak == / !=`：`compiler_expr.cpp::compileCompareExpr` 在类型一致检查后挡 `Eq/Ne`

## Phase 2 — String 不可变化

### 2a — base.yux 改造

- 新增 `fn String(buf Array<u32>)` 构造（`$.data = buf` 由 3d retain-on-assign 保证句柄共享平衡）
- 删除 `clear` / `append(cp u32)` / `append(s String)` / `append(b u8)` / `sub_string` 等 mutator
- 重写 `i64.to_string` / `u64.to_string` / `f64.to_string` / `bool.to_string`：改为 `Array<u32>` 缓冲 + 终态 `String(buf)`；`bool.to_string` 用 if 表达式直接返回 `"true"` / `"false"` 字面量
- `f64.to_string` 整数部分复用 `i64.to_string` 后用 `String.at(i)` 逐码点 push
- `print` / `println` 不变（仍 codepoint→UTF-8 编码循环；只读 `s.data` 句柄）

### 2.1 — StringBuilder

- `base.yux` 新增 `struct StringBuilder { data Array<u32> }` + 方法：`StringBuilder()` / `len()` / `append(cp u32)` / `append(b u8)` / `append(s String)` / `clear()` / `build() String`
- `build()` 语义：`String($.data) → $.data = []`（产出 String 后 SB 重置为空，继续 append 不污染已返回 String，因 `$.data = []` 触发 retain-on-assign，旧 handle release 后 SB 持新 empty Block，String 独占原 Block）

**后续单列**：String 改专属 FAM Block `{ strong, weak, len_cps, u32 data[] }`（性能优化，省一次 alloc + 一次间接）。

## Phase 3 — 用户结构体按值复制 + 字段级 retain/release

### 3a — callee-clean retain/release（仅堆句柄参数）

- 调用点：`compiler_call.cpp` 在传 Box / Array / Weak 实参前 retain（+1），覆盖 `compileKnownFunctionCall`（用户普通 fn）和 `compileGenericFunctionCall`（泛型 fn 实例）
- 新 helper `Compiler::retainHandleAtCallSite(val, type)`（`compiler_destructor.cpp`）：用 `ExtractValue` 直取 handle 字段，省去 alloca 临时栈
- 运行时新增 `_weak_retain(handle)`：null/哨兵跳过 → weak++
- callee prologue：`compileFn` / `compileMethod` 中"基本类型"参数路径里，对 `typeNeedsDestructor(paramType)` 追加 `_scopeVars.push_back(paramName)`，让 `callDestructorsForScope` 在作用域结束时统一 release

### 3b — move-return retain 注入

- `compiler_stmt.cpp::compileRetStatement`：在 `callDestructorsForScope()` 之前，对声明返回类型为 Box / Array / Weak 的 retVal 调 `retainHandleAtCallSite`（+1），随即 destructor 对局部 var release（-1），调用方接住净 +1 句柄
- 旧 `LiteralObjNode` peephole erase 仅对非堆句柄路径保留；堆句柄路径走"retain + 析构 release"
- TODO：String / Nullable<Handle> 返回路径在本期未处理，留 3c / Phase 8 临时值清单收尾

### 3c — 用户结构体按值传参 + 平凡 / 非平凡分类

**3c.1 平凡结构体按值**

- 引入 `Compiler::structParamUsesPointer(typeName)`：内置 / 未知 / 平凡用户 struct → by-value（false）；含 RC 字段的非平凡用户 struct / 泛型实例 / 仅在 `_structTypes` 注册的跨模块 struct → 仍指针（true）
- 13 处签名 / 调用点统一切到 helper：函数声明、方法签名（普通 + 泛型）、构造器（普通 + 泛型）、调用约定（实参与签名两侧）、callee 端 alloca + store
- receiver `$` 不动（始终指针）

**3c.2 非平凡结构体按值（含 String）+ 字段级 retain**

- `retainHandleAtCallSite` 扩展：识别非平凡 struct → 调新 helper `retainStructFieldsAtCallSite` 递归 ExtractValue 每个 RC 字段（嵌套 struct 一并递归）发 retain
- `structParamUsesPointer`：非泛型 user struct 一律 false；含 RC 字段也按值传
- callee 端 alloca + store + `_scopeVars.push_back`（路径已就绪 by 3a）
- String 自动走通用 by-value 路径：String 是非 builtin 用户 struct（SDK base.yux 注册），`structParamUsesPointer("String") → false`、`structNeedsDestructor("String") → true`（含 `Array<u32>` 字段）；调用点 `retainStructFieldsAtCallSite` 递归对 `data.handle` retain
- 新增 `Compiler::resolveStructFieldTypes(structName)`：普通 struct → `fields().getType()`；泛型实例 → `baseDecl` 字段套 `TypeInfo::substitute(typeParams→args)`
- `structNeedsDestructor` / `callFieldDestructor` / `retainStructFieldsAtCallSite` 三处全部改走 helper
- 泛型实例 `structParamUsesPointer` 翻为 false，与普通 struct 一致；保留 `_structTypes`-only 的跨模块保守 true

### 3d — 字段级 retain-then-release on assign + 含 RC 字段 struct 析构补完

- 新增 `Compiler::releaseAtPtr(slotPtr, type)`：Box/Array/Weak 走 GEP+load handle+release；含 RC 字段 struct 调其析构函数（默认析构按字段逆序 release）；refactor `callDestructor` 走它
- `callFieldDestructor` 改为按字段声明逆序遍历（"构造逆序对每个 RC 字段 release"）
- `compileAssignStatement` 简单变量 / 字段两条路径在 `assignOp == Eq && typeNeedsDestructor(type)` 下注入 retain-then-release：`retainHandleAtCallSite(new) → releaseAtPtr(slot) → store`；自赋值 `s = s` 安全（先 +1 再 -1，净 0）
- 数组字面量赋值：新 handle 来自 `_array_alloc`（strong=1）无需 retain，但旧 handle 必须 release → `releaseAtPtr` 后 `storeArrayHandle`
- `compileArraySetStatement`（`arr[i] = v`）：元素类型 `typeNeedsDestructor` 时同样 retain-then-release
- 数组字面量逐元素写入：从已有 var/field 读 RC 元素并写入新槽位 → 复制语义 retain
- 构造函数入口零初始化 `$`：`compileMethod` 中 `methodName == structName && !isDestructor` → `CreateMemSetInline` 全字段置 0；让 ctor 内首次写字段的 retain-then-release 路径在 release 阶段拿到 null handle（runtime 各 release 入口对 null 跳过）

## Phase 4 — `T&` 借用类型 + 寿命检查 + receiver `$`

### 4a — 语义层登记 `T&` + 自动 deref（read-only）

- `ast_builder` 新增 `buildTypeWithRef(twr, parent)` helper：识别 `typeWithRef->SymbolAnd()` 后把内层包成 `TypeGenericNode("Ref",[inner])`；4 处接入（fnHeader 收集签名 + visitFnParamStd / visitFnParamGroup + visitStatementDeclareAssign）
- `LiteralObj` 路径：`sym->type.isRef()` 时 `Load(getLLVMType(*inner), _localVarPtrs[name])`，自动解引用为 T
- `LiteralObjNode::getType`：sym 为 `Ref<T>` 时返回 T（值上下文按解引用语义）
- `compileDeclareAssignStatement` 增 Ref 分支：不分配 alloca，`_localVarPtrs[d] = (rhs is &expr ? compileGetRefExpr : (rhs is RefVar LiteralObj ? _localVarPtrs[srcName] : error))`
- `typeNeedsDestructor(Ref<T>)` 已返回 false

### 4b — rebind 禁 + `T&` 赋值落 store-through

- `var d T&` 必须初始化（g4 已强制：`statementDeclare` 用 `type` 不含 `&`）
- `d = expr`：简单变量分支增 Ref 分支 —— `_localVarPtrs[d]` 持底层 T 地址，`compileExpr(expr)` 走自动 deref 后 `CreateStore(valToStore, targetPtr)`；支持 `=` 与所有复合赋值
- 形参写权放行：`T&` 形参 / `val d T&` 局部 `writeable=false` 不挡 store-through（写被引对象的写权由源对象决定，4d 静态校验）

### 4c — `&` 扩展到 `&self.field` / `&box.field` / RC 字段链

- `compileGetRefExpr` 增 Box auto-deref：在每步字段 GEP 前若 `currentType.isBox()`，先 load handle 再 GEP +8 到 payload
- 起点剥 Ref：sym 为 `T&` 时 `currentType = inner`
- SDK struct 兜底：`_file->getStructDecl` miss → `_yux->sdkFile()->getStructDecl`
- `&box`（无 subs 句柄借用）：返 `_localVarPtrs[box]`（Box struct alloca 地址），`ExprGetRefNode::getType` 返 `Ref<Box<T>>`

### 4d — 寿命检查（O(1) 静态规则）+ 借用期根对象不可重赋

- 新增 `src/borrow_checker.{h,cpp}`：独立静态检查器，`Compiler::compileFn` / `compileMethod` 入口处调一次 `checkBorrows(node[, structName])`，每函数 O(N) 一遍 AST
- 扁平作用域模型（与现行 ast_builder 一致——`StatementBlockNode` 没有压入 `_scopeStack`，所有局部 var 都注册在 FnNode 上）：单函数维护 `declared` 集合 + `activeBorrows`（root → 计数）+ `refToRoot`（ref → 终极根，拷绑跟链直达）
- 拷绑透传：`var r T& = &x.f.f` 根 = `x`；`var r2 T& = d` → 根 = `refToRoot[d]`；根对象不在 `declared` → 报"作用域不足以覆盖借用方"
- 根对象重赋禁：`obj = expr`（`subs` 为空的顶层重赋）若 `obj ∈ activeBorrows` → 报"借用期内不可被赋值"
- 嵌套块遍历：`StatementBlockNode` / `StatementLoopNode` / `ExprIfElseNode` / `ExprIfElsePreValueNode` / `ExprOneLineIfElseNode` 全部递归
- TODO：错例测试基础设施待补——`tests/xmake.lua` 仅 glob `cases/*.yux`，错例需扩到 `cases/error/`

### 4e — receiver `$` 标 Self&

- 普通方法 + 析构函数两处 `fn->registerSymbol("$", …)`：sym.type 由 `TypeInfo(structName)` 改为 `Ref<structName>`；IR 层 `_localVarPtrs["$"]` 仍指向 Self 结构体
- 已就位的 Ref 自动剥皮路径让现有 `$.field` / `$.method(args)` / `&$.field` 全部不回归

## Phase 5 — Box 自动解引用 + Weak / upgrade

主体早在 1a / 1d / 4c 完成。本期补：

- `?.` 在 `Box<T>?` 上自动 deref：`ExprDotNode::getType` + `compileSafeDotExpr`，inner 为 Box 时剥到 payload 后查字段；codegen then 分支 load handle、+8、GEP 字段
- TODO：构造函数 `Weak(box Box<T>?)` / 显式 `weak:<T>(box)` builtin / "Weak 打破循环"测试——需先解决 `Nullable<Box<T>>` 析构 / 字段写 retain，归后续 v0.x

## Phase 6 — 构造函数 DAA（静态检查）

> 现状：3d 已经在构造函数入口对 `$` 做 `CreateMemSetInline` 全字段置 0，运行时层面已经安全。本阶段是纯静态检查，把"读未初始化字段 / 出口未全赋 / `$` 逃逸"在编译期挡掉，不改 codegen。

> 决策："首次写 store-only / 重赋 retain-then-release" 是性能优化，本期不做——保持 3d 的"统一 retain-then-release（旧值是 null，release 安全跳过）"，DAA 仅做静态检查；peephole 优化与字段级派生函数推到后续 v0.x。

- 新建 `src/ctor_daa.{h,cpp}`：风格仿 `borrow_checker`（namespace 匿名 class + 单遍 AST + 状态字典 `field → Uninit/Init`）
- 入口：`compileMethod` 中 `methodName == structName && !isDestructor` 分支（紧接现有 zero-init memset 之后）
- 顺序语句规则：`$.field = expr` 先 visit RHS 再标 Init；`$.a.b... = expr` 嵌套写要求 `$.a` 已 Init
- 表达式遍历：`$.field` / `&$.field` 要求 Init；`&$` / `$` 字面量 / `$.method(args)` 视为读所有字段
- 分支汇合：每分支独立从入口快照运行；汇合时每字段 Init iff 所有分支末态都是 Init（无 else 分支视作"该路径维持入口快照"）
- `StatementLoopNode`：保守——快照 `_state`，visit body，**还原**（循环可能 0 次执行）
- `ret expr` where expr 是 `$` 字面量 → 报错 "cannot return `$` from constructor"

## Phase 7 — `ptr_of` / `same_ref` / extern 边界

### 7a — `same_ref:<T>(a, b) bool` builtin

- `base.yux` 加 `#CompilerInner fn same_ref<T>(a T, b T) bool`
- `compileGenericFunctionCall` CompilerInner 分支统一处理 `same_ref` + `ptr_of`：lambda `extractRawPtr(i, forPtrOf)`：Box / Weak / Array → `ExtractValue 0` 取 handle；String → `ExtractValue {0,0}` 取 inner Array handle；Ref → 回溯 AST（`LiteralObj` 查 `_localVarPtrs[name]`，`ExprGetRef` 调 `compileGetRefExpr`）
- `same_ref` 主体：`extractRawPtr(0,false) ICmpEQ extractRawPtr(1,false)`
- TODO：`T&` 源类型未覆盖——`genericDef` 当前只用 `type` 而非 `typeWithRef`，`same_ref:<i32&>` 不可解析

### 7b — `ptr_of:<T>(obj) Ptr` builtin

- `base.yux` 加 `#CompilerInner fn ptr_of<T>(obj T) Ptr`
- 与 same_ref 共用 `extractRawPtr`；ptr_of 模式下：Box → handle GEP +8 跳过 RC 头到 payload；Array → handle GEP +24 后 load data 字段；String → inner Array handle GEP +24 后 load data；Ref → 同 same_ref Ref 路径

### 7c — extern fn 边界自动转 Ptr

- `ScopeNode::lookupFnSymbolWithParams` 增匹配规则：`fnInfo.isExternal && param.isPtr() && (arg.isBox() || arg.isWeak() || arg.isArrayGeneric() || arg.name=="String")` → 视为可调用
- `compileKnownFunctionCall` Ptr 形参分支扩展：`fnSymbol->isExternal` 时按规则抽 handle / data / payload；`T&` 走 AST 回溯 + `compileGetRefExpr`，与 same_ref / ptr_of 一致

### 7d — Ptr 算术 SDK-only

- 以"私有函数 + 编译器内部处理"实现替代 `@sdk_only` 注解机制
- 新增 `#CompilerInner fn _ptr_offset(p Ptr, off i64) Ptr`（`base.yux`），编译器 GEP i8
- 调用点按 `_` 前缀触发 `isPrivate` + 跨模块禁用检查（与 `_box_*` / `_array_*` 一致）
- TODO：`extern fn` 签名类型白名单收紧；`same_ref` / `ptr_of` 的 `T&` 源类型支持（需放宽 `genericDef` 让 turbofish 接受 `T&`，需决定是否合并 `genericDef` / `genericDefWithRef`）—— 推到后续 v0.x

## Phase 8 — 临时值清单

### 8a — leak 检测 runtime + `rc_leak_count` builtin

- 运行时全局 `_rc_block_count i64`（`compiler_runtime.{h,cpp}`）：SDK 端 `emitRcBlockCountDefinition` 升级 extern → `init=0` 定义；用户模块仅创建 extern 声明
- `_box_alloc` / `_array_alloc` 入口 `+1`；`_box_release` 的 freeBB / `_array_release` 的 freeBlockBB / `_weak_release` 的 freeBB 实际 `HeapFree` 后 `-1`
- `base.yux` 加 `#CompilerInner fn rc_leak_count() i64`（公开名，无 `_` 前缀，调试 / 测试用）

**baseline 量化**：每个含 RC 句柄的语句残留 ~5 个 Block，主要来源：
- (a) `rc_leak_count().to_string()` 自身的 String 链路
- (b) Box/Array 构造路径在 declare-assign 与 move-return 处双重 retain

8b 处理 (b)，8d 处理 (a)。

### 8b — declare-assign / assign 路径接入 fresh 判定

- 新增 `Compiler::isFreshHandleExpr(p<ExprNode>)`：`ExprCallNode`（callee 已 move-return retain）+ `ExprArrayNode`（数组字面量，`_array_alloc` 给 strong=1）→ true；变量引用 / 字段访问 / if-else / `??` / 一元等借用语义 → false
- declare-assign：Box copy / Array from-expression / Weak from-Weak（weak++ 包在 `needWeakInc` 条件下，仅 fromWeak 复用 fresh 跳过；fromBox 始终 weak++）
- assign：简单 var assign / 字段 assign retain 处加 fresh 跳过；旧值 release 仍无条件执行（自赋值 / 别名安全靠先 retain 再 release，fresh 路径直接 release，新值带入的 +1 抵消）
- 数组字面量元素写入 + `compileArraySetStatement`：每个元素表达式独立 fresh 判定

### 8c — 调用约定 callsite retain 接入 fresh 判定

- `compiler_call.cpp` 4 处 retain 站点全部接入 `isFreshHandleExpr(callNode->getArgs()[i])`：
    - 泛型 ctor / `compileGenericFunctionCall` / `compileConstructorCall` / `compileKnownFunctionCall`
- `compileConstructorCall` 函数无 callNode：签名加 `const vector<bool>& argFresh = {}` 默参；caller 按 `node->getArgs()` 预计算后传入
- ret 路径：`isFreshHandleExpr(node->expr())` 跳 retain；`didMoveRetainHandle = true` 仍设（fresh 也算 move-return）

### 8d — 块边界临时清单 + 链式调用 keep-alive

**8d.1 临时清单基础设施 + 线性 RC 句柄消费**

- `Compiler::_tempStack` + helpers：`pushTempFrame` / `popAndReleaseTempFrame` / `recordTemp` / `consumeTemp`
- 语句入口 push、结尾 pop+release，仅覆盖 Box / Array / Weak（单 handle by-value struct）
- BB 已被终结的路径直接丢弃帧避免 unreachable 插指令
- `compileExpr` 分发：`ExprCallNode` / `ExprArrayNode` 结果是 fresh +1，对 RC 句柄 `recordTemp(val, type)`
- 消费点（与 8b/8c `isFreshHandleExpr` 并列追加 `consumeTemp(val)` 分支）：declare-assign / assign / ret / callsite / 数组字面量元素

**8d.2 链式 `make().method()` receiver keep-alive**

8d.1 自然覆盖：`make_box().show()` 中 fresh Box 作为 receiver 进入 method，在 callee 端 `$` 是 `Self*` 指针；caller 处的 fresh Box 句柄记到 statement 帧，由 `popAndReleaseTempFrame` 在语句结束时 release。SSA use-def 自动让该 Value 在整条 method 调用期间 live（call 指令的 use 阻止 release 提前发出）。

`Box<T>&` 形参 + `make_box()` 实参由 g4 `exprGetRef: SymbolAnd obj=(ID|SymbolThis) (SymbolDot subs+=ID)*` 在语法层禁掉（`&` 不接调用结果，仅接命名 ID / `$`）。

**8d.3 分支汇合（if-else 表达式作为 RC 句柄结果）**

- 新增 helper `Compiler::compileBranchResultNormalized(expr, expectedType)`：仅对 RC 句柄进入"子帧 + 归一" 路径——`pushTempFrame` → `compileExpr(expr)` → `consumeTemp(val)`（拿到 wasFresh）→ `popAndReleaseTempFrame`（释放该分支内中间 fresh）→ 非 fresh 则 `emitRetainOnHandleValue` 归一为 +1
- `consumeTemp` 改为返回 bool（找到=移除=fresh）
- 4 个分支汇合点接入：`compileStatementBlockWithResult`、`compileIfElseExpr` 末尾汇合、`compileOneLineIfElseExpr` 两支显式归一、`compileIfElsePreValueExpr` 两支显式归一、`compileNullElseExpr`（then 支 retain，else 支走 `compileBranchResultNormalized`）

**8d.4 String / 含 RC 字段 struct 的 by-value 临时**

- `PendingTemp` 新增 `spillSlot`：含 RC 字段 struct value 临时落 entry-block alloca，pop 时调 `releaseAtPtr`
- `recordTemp` 扩展：非 Box/Array/Weak 时若 `structNeedsDestructor(type.name)` 真，则在 entry 块 alloca + store + 登记 (val, type, slot)
- `popAndReleaseTempFrame` 增 `spillSlot` 分支
- declare-assign 普通 struct 路径补 `consumeTemp(exprVal)`：fresh struct value 的 +1 转给 var slot

**8d.5 重新量化 baseline**

- 根因：baseline 的残余 leak 是 stale SDK——`sdk/yux/build/yux/yux.lib` 是 8d.4 修改前的产物。8d.4 改的是 `recordTemp` / `popAndReleaseTempFrame`，影响所有走临时帧的路径，包括 SDK 里 `print` / `println` / `to_string` 的 String/Array<u8> 临时
- 删除 `sdk/yux/build/`，用新编译器重 build SDK，所有 leak 计数全部归 0
- TODO：CI / 自动构建链路里要保证 SDK 与编译器一同重 build——任何 ABI / 临时帧改动都会被旧 SDK 屏蔽

### 8e — 测试与扫尾

- 新增 `tests/cases/temp_zero_leak.{yux,expected}`：覆盖 8d 涉及路径，断言每个 block 跑完后 `_rc_leak_count() - base == 0`
- 修复 `compileRetStatement` / `compileRetVoidStatement` 漏掉本语句临时帧释放：`CreateRet` 后 BB 终结，外层 `popAndReleaseTempFrame` 因 BB has terminator 早返、临时帧静默丢弃 → leak。修法：`CreateRet` 前先 `popAndReleaseTempFrame()` 再 `pushTempFrame()` 留空帧给外层平衡；同时把 `consumeTemp(retVal)` 路径扩到含 RC 字段 struct value（如 `i64.to_string()` 返回 String）

## Phase 9 — 文档与扫尾

- `xmake test` 全量回归全绿
- `examples/test` 冒烟（Nullable 烟测）通过

---

## v1 范围外 TODO 集合

来自上面各 Phase 的延后项汇总：

- **临时值清单**：含 RC 字段 struct 的非 declare-assign assign / `Nullable<Handle>` 包装 / `?.` SafeDot 链 `Nullable<inner>` 归一
- **错例测试基础设施**：扩 `tests/xmake.lua` glob `cases/error/`，覆盖寿命越界 / 嵌套 `T& &` / 绑临时 / 绑 `&arr[i]` / DAA 反例
- **`extern fn` 签名类型白名单收紧**
- **`same_ref` / `ptr_of` 的 `T&` 源类型**：需放宽 `genericDef` 接受 `T&` turbofish
- **性能优化**：peephole retain/release 抵消；构造函数首次写 store-only；派生独立 `__copy_S` / `__destroy_S`；String FAM Block
- **语言面**：`Weak<Array<T>>` / `Weak<String>`；显式 `weak:<T>(box)` builtin；`Box<T>?` / `Weak<T>?` 字段全链路 RC + cycle 测试
- **Ptr 算术 `@sdk_only` 注解机制**（当前以 `_ptr_offset` 私有函数替代）
- **工程**：CI 链路加 SDK 重 build 触发，避免 stale lib 屏蔽编译器修改
- **cycle collector**：v1 不做（`Weak<T>` 是用户侧打破循环的唯一手段）

# `Dyn<D>` / `Dyn<D&>` 运行时多态实施日志

本文件归档 v0.10.x `Dyn<D>` / `Dyn<D&>` fat pointer 多态形态的核心决策、ABI、关键代码点。是后续回答"为什么不引入 `dyn` 关键字"、"vtable 槽 0 为什么是 dtor"、"内置 U 怎么进 vtable"、"`Dyn<D&>` 借用根怎么挂"等问题的事实来源。

- 规范条款见 `docs/spec/12-draft.md` §12.9（§12.9.1..§12.9.11）+ 附录 B §B.2a + 附录 D §D.3.8（E1131..E1136）
- 草案沉淀见 `docs/spec/draft/DRAFT-dyn-draft.md`（已按模板末尾归宿处理，头部标"已落地，见 §12.9"）
- 前序 v0.5 单态化 draft 见 `docs/dev/draft-impl-log.md`

---

## Phase 0 — 草案审查

DRAFT-dyn-draft.md 起草，关键决议确认：

- **[#1.A]** fat pointer 第一槽 = per-(Type, Draft) vtable_ptr（Rust 风），不引入类型描述符 / Go interface 风的 type id；零运行时查表与 yux "零运行时开销" 基调一致
- **[#1.B]** owned `Dyn<D>` 内部模型 = "Box 同源"：data_ptr 指 `[RC head | 实例]`，标准 RC + vtable[0] dtor，二者职责分离
- **[#2.A]** 不引入 `dyn` 关键字；沿用 `Dyn<D>` / `Dyn<D&>` 类型名形态，与 `Box<T>` / `Weak<T>` 风格一致
- **[#3.A]** 构造形态 = `Dyn:<D>(x)` 类型构造，与 `Box<T>(x)` 一致；**不**走隐式 coercion，**不**引入 `as_dyn` builtin
- **[#4.A]** 第一轮允许 `Array<Dyn<D>>` / 字段，因为 `Dyn<D>` 是 16 字节 sized 类型，自然进入类型档位
- **[#5.A]** owned `Dyn<D>` ↔ `Box<U>` 关系：构造接管 RC，析构走标准 RC + vtable[0]；**不**为 dyn 引入独立 release 路径
- **[#Z]** 对象安全：v1 第一轮**禁止** `Self` / draft-name 出现在 draft 体的返回位置；解锁需要 per-(U, D, 含 Self 方法) thunk 生成，单独 Phase 引入
- **[#H]** v1 不提供 `Dyn<D>` 的 `==`、`same_ref` 等结构化相等；用户契约自管

## Phase 1 — 语法 / AST

### 1a — g4 评估

**不需要改 g4**：

- `genericDefWithRef` 实参槽 = `typeWithRef`，允许内嵌 `T&`
- `genericDef` 实参槽 = `typeParam` → `type`，不允许内嵌 `&`
- 所以 `Dyn<D&>` 只在 typeWithRef 位置（形参 / 返回 / declareAssign）能写，其它位置（不带初值 declare / 字段 / Array<...> 内层）天然拒绝
- 这三条限制都符合借用语义（无源 / 字段不持借 / Array 是 owned），不需要补丁

### 1b — 类型形态进 AST + LLVM 类型

- `TypeInfo::isDyn()` / `isDynOwned()` / `isDynBorrow()` / `dynDraftType()` 加入 `include/types.h`
- `Dyn<D>` 走现有 TypeGenericNode 路径（不需要新 AST 节点 / 不需要在 ast_builder 特化）
- `compiler_types.cpp::getLLVMType(Dyn<X>)` 返回 `{ ptr, ptr }` fat pointer

### 1c — 构造表达式 `Dyn:<D>(x)`

- 新增 `ExprDynCtorNode`（方案 B）
- ast_builder `visitExprCall` 命中 `callee=LiteralObj("Dyn") + 1 typeArg + 1 arg` → 重写为 `ExprDynCtorNode`
- `compileDynCtorExpr` emit 占位 fat ptr `{ vtable=null, data=src.handle }`；vtable 真值留 3a
- 调用站语法是 `Dyn:<D>(x)`（turbofish 需 `:` 前缀，g4 line 499），DRAFT 中 `Dyn<D>(x)` 是仅类型位写法

## Phase 2 — 语义检查

### 2a — 对象安全谓词

`DraftImplChecker::draftIsObjectSafe(DraftDeclNode*)`：检查 D 体内每个 fnSig 的返回 / 参数（非 receiver）有无 `Self` 或 draft 自身名；命中 → 该 draft 标 not object-safe；缓存结果。

### 2b — 构造检查

错误码 `E1131..E1136` 加入 `include/error_code.h`；`compileDynCtorExpr` 按顺序：

1. `E1131`（D 必须是 draft）
2. `E1132`（嵌套）
3. `E1134`（对象安全）
4. `E1133`（`Box<U>` / `U&` + `U:D`，复用 `boundSatisfied` 同时覆盖显式 impl + `#DraftLike`）

### 2c — 类型声明检查

`DraftImplChecker::validateDynTypeReferences()`：遍历 SDK+用户文件所有声明位 TypeNode（free fn / impl 方法 / draft sig / struct 字段 / enum payload / 顶层类型别名）。递归 helper `validateDynInTypeNode(tn, file, outerWrapper)` 处理 Box/Weak/Nullable 容器透传 + Dyn 内层 Ref 剥离 + draft 注册表解析 + 对象安全复用 2a。入口挂 `DraftImplChecker::validate()` 末尾。

局部 var 声明位不在 pass 范围：Dyn 形态必有初值（typeWithRef 不允许无初值 `Dyn<D&>`；`Dyn<D>` declareAssign 必带 `Dyn:<D>(x)`），由 `compileDynCtorExpr` (Phase 2b) 接管。

### 2d — 方法调用静态检查

`Compiler::compileDynMethodCall`：在 `compileMethodCall` 内 `baseType.isDyn()` 早分发。按 _file 可见性解析 D，按名+arity 找 sig：

- name 未命中 → `E6016`（沿用"未定义方法"段，type 写 `Dyn<...>`）
- arity 不匹配 → `E6012`
- 参数类型按 D 签名逐位 `getFullName` 比对，不匹配 → `E6015` 带 hint

### 2e — 借用检查扩 `Dyn<D&>`

`src/analyzer/borrow_checker.cpp`：

- 参数循环：`ty.isDynBorrow()` 时 `refToRoot[pname]=pname`（与 T& 同登记借用根，不入 refParams，故不可作 T& 返回源）
- declareAssign：`varType.isDynBorrow()` 分支调用 helper `rootFromDynBorrowInit(expr)` 解根并 registerBorrow
- `rootFromDynBorrowInit`：RHS 是 `ExprDynCtorNode` 时按 inner arg 形态推根（`&y.f` → y；变量名 → resolveRoot）；RHS 是 Dyn<D&> 变量拷绑时直接 resolveRoot；其它形态兜底 `E4001`+hint

`Dyn:<D&>(x)` 构造目前 ast_builder 仅识别 owned `Dyn:<D>` 形态（g4 `genericDef` 实参槽不允许 `&`），所以本阶段仅落地基础设施 + 参数登记，正/负回归测试俟构造解锁后补。

## Phase 3 — codegen / vtable

### 3a — vtable 生成模块

`src/compiler/compiler_dyn_vtable.cpp`（h 内嵌进 `compiler.h`）：

- 入口 `Compiler::getOrEmitDynVTable(const TypeInfo& U, const std::string& D_qualified, DraftDeclNode*) -> llvm::GlobalVariable*`
- 符号：`__yux_vtable_<U_mod>_<U_struct>__<D_qualified>`，`linkonce_odr`，`UnnamedAddr=Global`
- 槽 0：`structNeedsDestructor(U)` → `getDestructorFunction(U)`；否则 null
- 槽 1..N：D.signatures() 按声明序，跨 SDK+用户文件遍历 U 的 StructImplNode（先看 `Type:D{}`，再回落到 `Type{}` 普通方法块），按方法名取首条 FnNode；用其 header 的 param 类型走 `Mangler::method` 生成符号名，`_module->getFunction(...)` 找不到时 emit forward-declare 占位（v1 用 `fn()void` 占位签名，调用点再 BitCast）
- 找 U 所属模块：`yux->sdkFile()` 优先 → `yux->files()` 回落

### 3b — fat pointer LLVM 类型

`getLLVMType(Dyn<D>)` = `{ ptr, ptr }`；Phase 1b 已完成（`compiler_types.cpp:458`）。

### 3c — 构造 codegen

`compileDynCtorExpr` 把原 null vtable 占位替换为 `getOrEmitDynVTable(U, draftQualified, draftDecl)`；data 槽：Box<U> 取 handle 字段；U& 直接用。

验证：`examples/dyn_smoke` 编译后 IR 出现 `@__yux_vtable_main_G__main_Greet = linkonce_odr unnamed_addr constant [2 x ptr] [ptr null, ptr @"main#G_hi()"]`；exe exit=0。

### 3d — 方法调用 codegen

`compileDynMethodCall` 末段：把 baseExpr 落到 alloca、GEP `{0,0}`/`{0,1}` 取 vtable/data 槽；GEP `vtable[methodIdx+1]` 拿到 fn ptr。

- receiver：owned `Dyn<D>` → `data + 8`（跳 Box RC 头，与 `compileStructMethodCall` Box receiver 一致）；borrow `Dyn<D&>` → data 直接是实例指针
- FunctionType 按 D 签名还原：`(ptr receiver, P1, ..., Pn) → R`（对象安全保证 sig 不含 Self / 自身名，与 U.impl 形参逐位一致）
- void 返回：CreateCall 的 name 传空串避开 LLVM "Cannot assign a name to void values" assertion
- 类型推断侧：`ExprDotNode::getType` 给 `baseType.isDyn()` 加 draft 签名查找，返回 `fn() <ret>`（走 parent() 链，因 struct method FnNode 没挂 parentScope）

### 3e — release 路径

owned `Dyn<D>` 走 `_dyn_release(data, vtable)`：strong-- → if 0 then `dtor=vtable[0] dispatch(data+8)` → weak-- + free。

- 新增运行时辅助 `_dyn_release(ptr data, ptr vtable)`，签名 `void(ptr, ptr)`；声明在 `compiler_runtime.h::getDynReleaseFn`，IR 实现 emit 在 `emitBoxHelpers` 末尾（与 `_box_release_dtor` 同形 + vtable[0] dispatch）
- 在 `releaseAtPtr` 加入 `type.isDynOwned()` 分支：load vtable / data 字段后调 `_dyn_release`；`type.isDynBorrow()` 分支显式 no-op
- `typeNeedsDestructor` 加入 `type.isDyn()` 返回 true，确保 Dyn 局部变量进入帧管理

**构造 RC 交接**：`compileDynCtorExpr` Box 形态 — 源 box 是 fresh 临时（`consumeTemp` 命中）偷取 +1；命名变量则调 `_box_retain` 拷 +1（源 Box 自己照常 release）。Dyn 在自身 scope 退出走 `_dyn_release` 抵消。

## Phase 4 — 测试

### 4a — SDK 端到端

`sdk/yux/src/yux/core/dyn.test.yux` 8 用例：覆盖 `Dyn<ToString>` 多值、自定义 `Shape` draft 多方法、跨 fn 传 owned Dyn、含 String 字段 U 的 dtor 链；用 `Box<DynCounter>`/`Box<Square>`/`Box<Rect>`/`Box<Labeled>` 作 owned Dyn 源（避开 `Box<primitive>.method()` bug，详 §Bug 修复）。

`Dyn<D&>` 借用调用：构造端 `Dyn:<D&>(x)` 暂不可表达（g4 `genericDef` 实参不允许 `&`），延后。

同步修：

- vtable codegen 对内置 U（i32/i64/bool）的 impl 取 owner module 用实际 impl 所在文件（`compiler_dyn_vtable.cpp::findImplMethod` 返回 `ImplLookup{method, ownerModule}`）
- `ExprDotNode::getType()` `baseType.isDyn()` 返回 `fn() <ret>`（draft sig 查找走 parent() 链）
- `Compiler::callFieldDestructor` 加 `isDynOwned` / `isDynBorrow` 分支（之前会把 "Dyn" 当 struct 名找 dtor 符号崩）

### 4b — AOT / 多形态

`tests/cases/dyn_array_iterate.{yux,expected}`：`Array<Dyn<Shape>>` push 多种 U + indexed dispatch — AOT 通过。

`tests/cases/dyn_pass_owned.{yux,expected}`：跨函数 by-value 传 `Dyn<Shape>` — AOT 通过。

字段持 `Dyn<D>` + struct 方法体内调 Dyn 方法在 Phase 3 末态曾出现 JIT 正确、AOT 读到错误返回值 + `Dyn:<D>(x)` 在 struct 方法体内 E1131；见 §Bug 修复 A / B。

### 4c — 诊断

`tests/cases/diag_dyn_*.{yux,expected_err}` 覆盖：

| 错误码 | 用例 |
|---|---|
| E1131 | `diag_dyn_param_not_draft` |
| E1132 | `diag_dyn_nested_dyn` / `diag_dyn_field_box_nested`（Box<Dyn>） |
| E1133 | `diag_dyn_ctor_not_impl` |
| E1134 | `diag_dyn_not_object_safe` |
| E1135 | `diag_dyn_nullable` |
| E6016 | `diag_dyn_method_missing` |

E1136 extern 边界错误码已定义但暂无强制点，先不补用例。

## Phase 5 — 规范回写

- `docs/spec/12-draft.md`：§12.8 项 1 改写为指针；新增 §12.9 全章节（§12.9.1..§12.9.11）；Open Issues 删除 `dyn Draft` 验证条
- 附录 B：新增 §B.2a `Dyn` 类型形态小节
- 附录 D：§D.3.8 E11xx 表追加 E1131..E1136 六行（与 `include/error_code.h` 现有 `DEF_ERR` 同步）
- CHANGELOG：顶部新增 2026-05-11 条目
- DRAFT-dyn-draft.md 头部状态改"已落地，见 §12.9"

## Bug 修复（实施期同步落地）

### Bug A — struct 字段持 `Dyn<D>` AOT 读到错误返回值

当前 Phase 4 末态下已不能复现。多字段 Holder（`i64 + Dyn + i64`）/ String 字段 / `h.inner.m()` 链式 / `val d = h.inner` 拷绑，AOT (`yux build` + `yux <file>` 两路) 均返回正确值。Phase 4b 给 `releaseAtPtr` / `typeNeedsDestructor` / field destructor 加 Dyn 分支后，配合现有 aggregate `{ptr,ptr}` 字段 load/store 已经够用。`tests/cases/dyn_field_owned.{yux,expected}` 防回归。

### Bug B — `Dyn:<D>(x)` 在 struct 方法体内 E1131

`compileDynCtorExpr` 改走 `node->parent()` 链解析 FileNode（FnNode parentScope 未挂时断链）。`tests/cases/dyn_ctor_in_method.{yux,expected}` 通过。

### Bug C — `Box<primitive>.method()` 调用走错路径

BUGS.md 第 1 条。根因：SDK 内置类型方法 receiver 走 by-value ABI（`getMethodFunction` line 291 / `compileMethod` line 672-678），但调用站把 receiver 当 ptr 传。

- 直接调用路径（`b.to_string()` where `b:Box<i32>`）—— `compileStructMethodCall` (`compiler_call.cpp:2064`) 末段加 `isBuiltinType(actualType.name)` 分支：从 dataPtr load primitive 按值传，fn 第 0 槽用 `getLLVMType(actualType)`
- Dyn 派发路径（`Dyn:<ToString>(Box<i32>).to_string()`）—— Dyn 调用站统一 `(ptr,...)` 派发与 SDK `(<U> by-value,...)` 不可调和；在 vtable 槽插入 `linkonce_odr` 适配 thunk `__yux_dyn_thunk__<U>__<draftQ>__<method>`：load primitive 后转发到 SDK fn。新增 `Compiler::getOrEmitDynPrimitiveThunk`（`compiler_dyn_vtable.cpp`）；vtable 槽 1..N 在 `isBuiltinType(uStruct)` 时直接放 thunk，不走原 forward-decl 分支

回归用例：`sdk/yux/src/yux/core/box.test.yux` +4（`Box<i32>`/`Box<i64>`/`Box<bool>` + 字面量并存）；`sdk/yux/src/yux/core/dyn.test.yux` +3（`Dyn<ToString>(Box<i32>/Box<i64>/Box<bool>)`）。

## ABI / 关键代码点

- fat pointer 表示：`{ vtable: ptr, data: ptr }`，16 字节，sized；`getLLVMType` `compiler_types.cpp:458`
- vtable 符号：`__yux_vtable_<U_mod>_<U_struct>__<D_qualified>`，`linkonce_odr` `unnamed_addr`，槽 0 dtor / 槽 1..N D 方法按声明序
- 内置 U 适配 thunk：`__yux_dyn_thunk__<U>__<draftQ>__<method>`，`linkonce_odr`，load primitive 后转发到 SDK by-value fn
- 类型谓词：`TypeInfo::isDyn()` / `isDynOwned()` / `isDynBorrow()` / `dynDraftType()`，`include/types.h`
- 构造节点：`ExprDynCtorNode`，ast_builder 在 `visitExprCall` 命中 `Dyn` + 1 typeArg + 1 arg 重写
- vtable 生成：`Compiler::getOrEmitDynVTable`，`src/compiler/compiler_dyn_vtable.cpp`
- 方法分派：`Compiler::compileDynMethodCall`，`src/compiler/compiler_call.cpp`
- 释放：`_dyn_release(data, vtable)`，emit 在 `emitBoxHelpers` 末尾；`releaseAtPtr` `type.isDynOwned()` 分支
- 借用根：`refToRoot` 把 `Dyn<D&>` 形参 / 局部按 data_ptr 视作借用根；`rootFromDynBorrowInit` 推根
- 错误码：E1131..E1136 在 `include/error_code.h` line 101-106；E6012 / E6015 / E6016 复用既有方法调用诊断段位

## TODO（v0.10.x 未覆盖，记入后续版本）

- `Dyn:<D&>(x)` 调用站构造形态：g4 `genericDef` 实参槽不允许 `Type&`，需扩 g4 或换写法；解锁后补 `Dyn<D&>` 端到端测试
- 对象安全 v0.X+1：解锁 `Self` / draft-name 在返回位置 — 需要 per-`(U, D, 含 Self 方法)` 生成 thunk（sret 槽 16 字节、内部调具体 impl 得 `U`、`Box+vtable` 包成 `Dyn<D>`）
- `E1136` extern 边界强制点：当前错误码已分配但无触发位
- `Dyn<D>?` nullable 形态（占位 E1135）；`Dyn<D>` ↔ `Box<U>` 向下转型（需 RTTI / type id）
- 多线程下 vtable 跨线程引用（v1 单线程承诺）
- LSP semantic_tokens / tmLanguage / IntelliJ 给 `Dyn` 类型名特殊高亮（本轮不做工具链同步）
- vtable 内联缓存 / devirtualization 性能优化

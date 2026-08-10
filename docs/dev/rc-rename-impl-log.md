# Box → Rc 改名实施日志

heap-types 草案 Phase 1（`docs/spec/draft/DRAFT-heap-types.md` §3）的落地实施日志。零语义变化、零 ABI 变化、纯机械替换。

## 核心决策

- **runtime symbol**：实际不存在 `__yux_box_*` 这种外部符号。`src/` 下 `__yux_*` 只有 `__yux_vtable_` / `__yux_dyn_thunk__`。Box 相关 helper 都是模块内 LLVM Function（名字以 `_box_*` 之类生成或 internal linkage），无 ABI 暴露面 —— 此决策项 N/A。
- **C++ helper 同步 rename**：取 yes。`getBoxAllocFn` / `getBoxRetainFn` / `getBoxReleaseFn` / `getBoxReleaseDtorFn` / `getBoxUpgradeFn` / `emitBoxHelpers` / `compileBoxFnValueCall` / `isBox()` / `BoxType` / 各 `box*` 局部变量 → 对应 `Rc` 形态。
- **runtime symbol 字符串名 `_box_*`**：保留作模块内 symbol（C++ 端字符串字面量 + 注释里引用 symbol 名），不属本次范围。后续如统一可在独立 phase 做。
- **历史 impl-log**：`docs/dev/*-impl-log.md` 内文 Box 提及保留，文件顶部加脚注"本日志使用旧名 Box / box；当前等同 Rc<T>。"

## 改动面清单

### R2 — 编译器端（src/ + include/）
26 个 C++ 源文件 + `src/types.h` + `include/error_code.h` + `src/yuxParser.g4` 一行注释。涉及 ~480 处 identifier / 字符串 / 注释替换。

- C++ identifier rename：见核心决策第二条
- IR value name 字符串：`"box.handle"` / `"box.payload"` / `"box_src_tmp"` / `"box_block"` 等 → `"rc.handle"` / `"rc.payload"` / `"rc_src_tmp"` / `"rc_block"`
- 用户可见错误消息（E1132 / E1133 / E2001 / E2029 / E2032 / E3014 / E3016 / E3050 / E3056 / E4022 / E6029）：`Box<T>` / `Box` → `Rc<T>` / `Rc`
- 注释：`box payload` / `源 box` / `captures box` / `box 元素类型` / `box-deref` / `Box/Array/Weak` → `Rc/rc`
- `compiler.h` 注释中泛型实例化举例 `Box2` / `Box2$i32` → `Foo2` / `Foo2$i32`（避免与类型 Box 混淆）
- `src/types.h:321` `name == "Box"` 比对 → `name == "Rc"`（与 SDK base.yux 端类型名对齐）

### R3 — SDK 内 .yux
13 个文件，~174 处替换：`base.yux` 内置类型 / `#Builtin` 函数签名 + 12 个 `*.test.yux`。`Weak<Box<T>>` → `Weak<Rc<T>>`。

保留未改的 user-defined 标识符（"形似但非 Box 类型"）：`BoxPair` / `BoxCell` / `Container::Boxed` / `make_box_cell` / `rc_make_box` / `box_field_write` 等结构体名、enum 变体、测试函数名、`as_ref` 形参 `box`。

### R4 — tests / examples / docs / plugins

- tests/cases：24 .yux + 3 .expected_err 内容改 + 11 个文件 git mv 重命名
- tests/projects：`draft_box_forward/` 目录 git mv 为 `draft_rc_forward/`；内 `yux.toml` `name=` + `src/main.yux` 同步
- examples：无 Box 引用
- docs 用户面：28 .md（`docs/`、`docs/spec/`、`docs/spec/draft/`），`DRAFT-heap-types.md` 不动（描述本次改名）
- docs/dev impl-logs：6 个文件加脚注头，正文保留
- plugins/yux-vscode：tmLanguage `support.type.generic` keyword 列表加 `Heap` / `Arc` 占名；CHANGELOG / README 改 Box→Rc
- plugins/yux-idea：source 无 Box keyword（走 LSP semantic tokens），无改动

### 文件 / 目录 rename（16 项）

```
sdk/yux/src/yux/core/box.test.yux                        → rc.test.yux
sdk/yux/src/yux/core/draft_box_forward.test.yux          → draft_rc_forward.test.yux
tests/cases/diag_box_missing_type_arg.{yux,expected_err} → diag_rc_missing_type_arg.{...}
tests/cases/diag_box_type_mismatch.yux                   → diag_rc_type_mismatch.yux
tests/cases/borrow_as_ref_box_reassign.yux               → borrow_as_ref_rc_reassign.yux
tests/cases/diag_ctor_arg_box_mismatch.{yux,expected_err}→ diag_ctor_arg_rc_mismatch.{...}
tests/cases/diag_dyn_field_box_nested.yux                → diag_dyn_field_rc_nested.yux
tests/cases/lambda_call_box_fn.yux                       → lambda_call_rc_fn.yux
tests/cases/lambda_closure_capture_box.yux               → lambda_closure_capture_rc.yux
tests/cases/match_box_enum.yux                           → match_rc_enum.yux
tests/cases/match_box_enum_payload.yux                   → match_rc_enum_payload.yux
tests/projects/draft_box_forward/                        → tests/projects/draft_rc_forward/
```

### R5 — spec 回写
- §9.5：`Box<T>` 章节全量改为 `Rc<T>`（Agent 在 R4 同步完成）
- §9.5.6：新增"保留名"条款，`Heap` / `Arc` 占名
- 附录 D 诊断错误码字符串：Box→Rc（R2 已完成）
- `docs/spec/CHANGELOG.md`：顶部追加本次条目（日期 2026-05-15）

## 跨 Phase 的 TODO（不在本 Phase 范围）

- runtime symbol 字符串名 `_box_alloc` / `_box_retain` / `_box_release` / `_box_release_dtor` / `_box_upgrade` 是否同步 `_rc_*`：当前保留（属内部 symbol，rename 需 C++ 端 + sdk/yux 端的 `#Builtin` 字符串对应处一并改）。独立小 phase 可做。
- `Heap<T>` 非空形态实施（草案 Phase 2）
- `Heap<T>?` + B 档 nullable move（草案 Phase 3）
- A 档 NRVO
- `copy_of` 扩展 Heap 形态
- `Arc<T>` 实际实现（v1.x 多线程主题）

## 回归

- `xmake test`：159 / 159 passed
- `cd sdk/yux && yux test`：492 / 492 passed
- `xmake build yux yux-lsp yux-ast yux-check`：全部 build ok

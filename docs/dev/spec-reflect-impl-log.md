# spec-reflect 编译期反射实施日志

`DRAFT-spec-reflect.md` 的落地记录。范围：内置 spec `Reflect` + `#Static #Frozen` 字段段 + 反射数据类型 + `Field.value` sema 改名 + `.rodata` emit & DCE + `#Reflect` 注解。

- 规范条款：`docs/spec/13-反射.md`（新增）、`docs/spec/11-编译期注解.md` §11.12（`#Reflect`）、`docs/spec/12-spec.md` §12.1.1.1 / §12.8 项 14（`#Static #Frozen` 例外）、附录 D（E3133–E3136）
- 草案：`docs/spec/draft/DRAFT-spec-reflect.md`（已落地批注）
- 实施分支：`dev-reflect`（从 dev 切出）

---

## 核心决策

- **反射走 runtime 数组**：`Self::fields` 是 `[Field& * N]&` 定长数组引用，可索引/取 len/`for` 遍历。不引入 `#Inline for` / IR-before unroll pass（永不引入）。
- **Field.value sema 期改名**：识别 `<staticFieldsExpr>[<intLit>].value` AST pattern → resolve Field.name 静态值 → 改写为 `<receiver>.<name>`，复用既有字段访问通路。非编译期可定的 f → E3133。
- **Reflect 为内置 spec**：编译器隐式 `#Impl(Reflect)`，四个 `#Static #Frozen` 字段（`type`/`fields`/`methods`/`variants`）仅类型形访问（`Counter::type` / `Self::fields`）。实例形 → E1138。
- **反射数据类型 `#Builtin`**：`Type` / `Field` / `Method` / `Variant` 由编译器硬编码 LLVM 布局（`{ String name }`），用户不可构造/析构。
- **常量 emit to .rodata**：Type 全局 linkonce_odr + Field data/refs 数组 PrivateLinkage。String 走 immortal Block（strong=0xFFFFFFFF），零 RC 开销。`--gc-sections` 自动回收未引用数据。
- **`#Reflect` 注解防 DCE**：零参，标在 struct 上，强制加入 `llvm.compiler.used`。
- **E3136 消解**：反射元数据统一按值 copy（rodata → stack），`Field` 不再含 `type`/`offset`（避免 `T&`），无需引入按值/按引用语义选择错误。

---

## Phase 1 — `#Static` 字段段基础设施

- `StructFieldNode` 新增 `isStatic` 槽位 + `setStatic/isStatic` 访问器
- `SpecDeclNode` 新增 `_staticFields` 槽位
- `readFieldAnnos` 扩到 `(isVal, isFrozen, isStatic)`，`#Static` 与 `#Frozen` 可组合，与 `#Val` 互斥
- spec body 分支放行 `#Static` 字段，instance 字段仍 E2011
- 关键文件：`src/ast/node/struct_node.h`、`src/ast/ast_builder_decl.cpp`、`src/ast/ast_builder_struct.cpp`

## Phase 2 — 反射数据类型

- `base.yux` 新增 `Type` / `Method` / `Variant` / `Field` `#Builtin` stub
- `Field` 先于 `Type` 声明避前向引用
- sema：`Field` 标 `#Builtin` 暂不需要类型识别特例（无构造站点）

## Phase 3 — Reflect Type 节点 .rodata emit

- 拆 `emitStringConstBlock` helper（Block emit 与 alloca/load 解耦，供 reflect 节点复用）
- `base.yux` 加 `#Builtin fn __yux_reflect_type:<T>() Type` intrinsic
- `ensureReflectTypeGlobal(T)` lazy emit `__yux_reflect_<mod>_<typename>__type` linkonce_odr rodata 全局
- Layout 级联：Type → String → Array<u32> → ptr → immortal Block
- sema `validateBuiltinIntrinsicShape` 加 `__yux_reflect_type` arity 校验
- `call_fn.cpp` 加 intrinsic 分支：load Type 全局
- 关键文件：`src/compiler/compiler.cpp::ensureReflectTypeGlobal`、`src/compiler/expr/expr_literal.cpp::emitStringConstBlock`、`src/compiler/call/call_fn.cpp`

## Phase 3b — Type.fields 暴露 + Phase 5b Array→[Field& * N]& 重构

- 原设计用 `Array<Field>`（堆分配 Block+sentinel）。重构为 `[Field& * N]&`（rodata ref 数组引用，零拷贝）
- `ensureReflectTypeGlobal` 重写：Type 只存 `{String name}`，fields ref 数组 `[N x ptr]` 独立全局（constexpr GEP）
- `expr_ctor.cpp`：`Counter::fields` 返回 `fieldsRefGV`（`[N x ptr]*`），不再 load
- `borrow_checker.cpp`：`rootFromRefInit` 新增 `ExprGetNode`/`ExprPathCallNode` T& 追根（`$rodata` sentinel）；`registerBorrow` 跳过 `$rodata` 的 `_declared` 检查
- `compiler_stmt.cpp`：T& 初始化新增 reflect 分支；Field.value 写路径新增 Pattern B（`[N]` 索引）
- `expr_access.cpp`：`compileArrayGetExpr` 新增 auto-deref Ref + general fallback
- 关键文件：`src/compiler/compiler.cpp::ensureReflectTypeGlobal`、`src/compiler/expr/expr_ctor.cpp`、`src/compiler/expr/expr_access.cpp`、`src/compiler/compiler_stmt.cpp`、`src/analyzer/borrow_checker.cpp`

## Phase 4 — Type::field 类型形访问 + 实例形 E1138

- sema：`<StructName>::type` / `<StructName>::fields` path 访问解析（`sema_pass ExprPathCallNode` 分流加分支，优先于 impl-method/enum-ctor）
- sema：实例形 `c.type` / `c.fields` → E1138（`sema_pass ExprDotNode` 顶部加检测；用户声明同名实例字段则放行）
- `ExprPathCallNode::getType`：`Counter::type` → TypeInfo("Type")；`Counter::fields` → 返回 ref 数组类型
- codegen `expr_ctor.cpp`：加 reflect 分支
- 错误码：E1138（实例形访问静态成员）；E1137 已被 spec 缺方法占用
- 关键文件：`src/sema/sema_pass.cpp`、`src/ast/node/expr_node.cpp`、`src/compiler/expr/expr_ctor.cpp`

## Phase 5 — Field.value sema 期改名 + E3133/E3134

- AST 模式匹配优先策略：`ExprDotNode::getType` 中 `member == "value"` 先走 AST 模式匹配（`ExprCallNode→ExprDotNode(".at")→ExprPathCallNode("fields")`），不依赖可能失败的 `getType()` 类型推断
- 修复 `StructImplNode::setParentScope(file)` 缺失（`ast_builder_struct.cpp`），解决方法体内 scope 链断裂
- codegen：`compileDotExpr` 触发 `node->getType()` 填充缓存元数据
- sema：非编译期可定的 f（形参 Field&）→ E3133；无 `$` receiver → E3134
- 写赋值 `f.value = expr` → `$.<name> = expr`，compileSetStmt 加 Field.value 分支
- 关键文件：`src/ast/node/expr_node.cpp::ExprDotNode::getType`、`src/ast/ast_builder_struct.cpp`、`src/compiler/compiler_stmt.cpp`

## Phase 6 — #Reflect 注解防 DCE + enum variants + E3135 + const-eval 反哺

- sema：`#Reflect` 顶行注解识别（v1 零参），注册入 `knownAnnos()` / `nonFnAllowedAnnos()`
- codegen：`#Reflect` 标的类型 rodata 全局加 `llvm.compiler.used`
- codegen：非 enum 访问 `Counter::variants` → E3135
- const-eval Phase 6：`ConstantValue` 新增 `Kind::String` + `makeString`；`buildLLVMConstantFromValue` 加 String→Block→Array→String LLVM 链；`ensureReflectTypeGlobal` 从手搓 `llvm::ConstantStruct::get` 四层嵌套迁到声明式 `ConstantValue::makeStruct` + `buildLLVMConstantFromValue`
- 关键文件：`src/compiler/compiler.cpp`、`src/sema/const_eval.h`、`src/compiler/compiler_globals.cpp`

---

## 跨 Phase TODO

- `Self::type` / `Self::fields` 在 spec 默认体 / enum 方法体内的可用性
- `methods` / `variants` 数组填充（当前 N=0）
- `Field.type` / `Field.offset` 额外反射字段（使用时上下文已知遍历哪个 struct 的 fields，有需求时再加）
- 显式 receiver `other::fields[0].value` 改写
- `#Reflect` 命名参数扩展（`#Reflect(name=true, fields=false)`），待 `DRAFT-anno-struct.md` 落地
- LSP / hover / doc 硬编码 `Field.value`（当前 LSP 套件未接反射类型补全）
- intrinsic `__yux_reflect_type:<T>()` 退役（当前公开形态走 `T::type` path，intrinsic 暂留兼容）

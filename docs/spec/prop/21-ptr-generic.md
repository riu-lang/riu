# 21 `Ptr<T>`：类型化 FFI 指针

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-21 |
| 旧档 | §9.7.2.1 / CHANGELOG 2026-07-06：`Ptr` 即 `void*`，不引入 `Ptr<T>`。`genericDef` / `typeParam` 无默认实参。相关：[#19](19-ffi-layout.md)；Win32 生成把 C 指针收成透明 `type X = Ptr` |

不绑版本。**改 `riu.bnf`**：声明位 `typeParam` 可写 `= type`。翻 2026-07-06：`T` 只当编译期标签 / 所指类型；用户层仍无按 `sizeof(T)` 的指针算术（`_ptr_offset` 字节）。

先落地本条（`Ptr<T=()>` + 默认实参语法）。C 字符串转换不做。

## 提议

现行 `Ptr` 是类型擦除的 `void*`。`type HANDLE = Ptr` 透明（#17），`HANDLE` ≡ `LPCWSTR` ≡ `Ptr`。extern 再把 `T&` / `Rc` / `Array` / `String` / `Weak` 隐式收成 `Ptr`（§6.6.3）。编译期无所指校验。

改成：

- 声明默认类型实参（语法通用）：`struct Foo<T=i32>`、`#Builtin struct Ptr<T=()>`。
- 使用点少写 / 不写 `<>` 时，按声明从**尾部**补默认。裸 `Ptr` ≡ `Ptr<()>`。
- 不透明句柄：空结构体当幽灵 `T`，`type HANDLE = Ptr<_HANDLE>`。
- `Ptr<T>` 与 `Ptr<U>` 仅当 `T ≡ U` 可赋值。跨 `T` 只走 `ptr_cast`。
- 不做 C 字符串转换。不加 `Pointer` 别名。

### 现状

| | 现行 |
|---|---|
| `Ptr` | `#Builtin struct Ptr`，arity 0 |
| `typeParam` | `type (':' bound)*`，无 `= type` |
| 别名 | `type HANDLE = Ptr` 全塌成同一类型 |
| extern | 签名 `Ptr`；堆句柄 / `T&` 隐式转 |
| `ptr_of` | 返回无类型 `Ptr` |

### 语法：默认类型实参

改 `riu/ast/riu.bnf` 的声明槽（`genericDef` 已用于 struct / fn / enum / `#Spec` 头）。**不**改使用点 `genericDefWithRef`（那里仍只填实参类型，不写默认、不写边界）。

```
typeParam ::=
    type (SymbolColon type (SymbolAdd type)*)? (SymbolEq type)?
    ;
```

`=` 在边界之后。词法 `<=` 仍是 `<` + `=` 两 token（§1.7.2），`Foo<T=i32>` 无歧义。

```riu
#Builtin
struct Ptr<T=()> {
  #Builtin
  fn to_int() u64
}

struct Pair<T, U=i32> {
  a T
  b U
}
```

#### 声明约束

- 有默认的形参必须在**尾部连续**：`Foo<T=i32, U>` 非法（新诊断）。`Foo<T, U=i32>` / `Foo<T=i32, U=bool>` 合法。
- 默认右侧是 `type`（可 `()` / `Foo<A>` / 别名），不得根裸 `&`（E4039 / E4037 同档）。
- 默认类型可引用**更左**的形参（`struct Foo<T, U=T>`）；不得引用更右的、不得成环。
- 形参名规则不变：裸 ID，`requireBareTypeParamName`。
- `#Spec` / enum 头同一 `genericDef`；enum 头上 `<T : D>` 仍语义拒 E2037，但 `<T=i32>` 合法。

#### 使用点补齐

对已解析的目标泛型（struct / enum / 内置包装 / 本函数形参列表），`got < want` 时：从下标 `got` 起若**每一个**剩余形参都有默认，则按声明补上；否则仍 E6011（`Rc` 无默认，裸 `Rc` 照旧错）。

| 声明 | 写出 | 结果 |
|---|---|---|
| `Ptr<T=()>` | `Ptr` | `Ptr<()>` |
| `Ptr<T=()>` | `Ptr<()>` / `Ptr<u32>` | 写出的 |
| `Ptr<T=()>` | `Ptr<A,B>` | E6011 |
| `Pair<T, U=i32>` | `Pair<u8>` | `Pair<u8, i32>` |
| `Pair<T, U=i32>` | `Pair` | E6011（`T` 无默认） |
| `Pair<T=u8, U=i32>` | `Pair` | `Pair<u8, i32>` |

空 `Foo<>` 仍写不出（`genericDefWithRef` 至少一实参）。

补齐发生在类型出现位（字段、形参、返回、别名右侧、`let` 标注、`extern` 签名、`#Impl` 实参等）以及调用点 turbofish：已写出的实参从左对齐，右侧用默认补。调用点**完全不写** turbofish 时仍先走现有推断；推断失败的槽若有默认再填（`ptr_of(x)` 不因默认变成 `ptr_of:<()>(x)`）。

`typeGeneric` / 诊断 `getFullName`：intern 之后总是带齐实参（`Ptr<()>` / `Pair<u8, i32>`）。源上写的裸 `Ptr` 可以仍印 `Ptr`，前后一致即可。

`.ud` skeleton 须写入每形参的默认类型，跨文件 `use` 才能补齐。

### `Ptr<T=()>`

`#Builtin struct Ptr<T=()>`。ABI 永远一个指针字，与 `T` 的 size / align 无关。

- 数据指针：`Ptr<u32>` ≡ C `uint32_t*`
- 无类型：`Ptr` / `Ptr<()>`
- 句柄：`Ptr<幽灵空结构体>`

`T`：owned（值类型 / 堆句柄 / `Ptr<…>` / `()`），不得 `T&`（E4037）。允许 `Ptr<Ptr<u32>>`。extern 里任意 `Ptr<T>` 按 `void*` 传。

`null`：任意 `Ptr<T>` 仍是唯一非 `T?` 可赋 `null` 的上下文。`Ptr<T>?` 无意义。

判等：`Ptr<A>` ≡ `Ptr<B>` iff `A ≡ B`。`Ptr` ≡ `Ptr<()>`。`Ptr<i32>` ≠ `HANDLE`。

intern：`TypeKind::Ptr` + 恰好 1 个 `genericArgs`（缺省 unit）。

### 句柄：`Ptr` + 空结构体

幽灵空结构体只作 `Ptr` 的 `T`，约定不构造（语言不特判）。幽灵名 `_` 前缀私有：

```riu
struct _HANDLE {}
type HANDLE = Ptr<_HANDLE>
```

`HANDLE` ≢ `HWND` ≢ `Ptr`。生成器按 clang canonical / typedef 链共用或拆开幽灵。现网 `type HANDLE = Ptr` 在生成器改之前仍等于 `Ptr<()>`。

### 转换

无隐式 `Ptr<T>` → `Ptr<U>`（含 → 裸 `Ptr`）。

```riu
#Builtin
fn ptr_cast<T, U>(p Ptr<T>) Ptr<U>
```

擦类型：`ptr_cast:<T, ()>(p)`。`ptr_from_addr(addr u64) Ptr`。

`ptr_of<T>(obj T)` 返回带所指的指针：

| 源 `T` | 结果 |
|---|---|
| `U&` | `Ptr<U>` |
| `Rc<U>` / `Heap<U>` | `Ptr<U>`（Heap 仍交出所有权） |
| `Array<U>` | `Ptr<U>` |
| `String` | `Ptr<u32>`（码点；不是 C 字符串 / UTF-16） |
| 标量 | 仍禁 |

`let p Ptr = ptr_of(x)` 在结果不是 `Ptr<()>` 时不匹配：去掉标注或写成 `Ptr<U>`。

`_ptr_offset<T>(p Ptr<T>, off i64) Ptr<T>` 按字节，保持 `T`。`_ptr_as_ref` / `_ptr_write` 的 `T` 必须与指针一致；从裸 `Ptr` 读先 `ptr_cast`。

### extern 边界

允许任意 `Ptr<T>`（含裸 `Ptr`）。§6.6.3 隐式转换只进 `Ptr` / `Ptr<()>`。现网 `lpBuffer Ptr` 不变；改成 `Ptr<u16>` 后 `String` 不再隐式进入。

### 本条不做

- C 字符串 / `String` ↔ `LPCWSTR`
- `type Pointer = Ptr<()>`
- const 指针（`LPCWSTR` 与 `LPWSTR` 可同为 `Ptr<u16>`）
- 按 `T` 的指针算术、用户向 `_ptr_as_ref`
- 名义别名 / 注解限定 / `CPointer` / `Ref<T>` 进 extern / `unsafe`
- 表达式默认（`fn f(x i32 = 0)`）——只是类型形参默认

### 改为（落地时回写）

- 附录 B / §2：`typeParam` 增 `( '=' type )?`；§3 补默认实参补齐规则与诊断。
- §9.7.2.1：`Ptr<T=()>`；撤回「不引入泛型」。
- §3.1 / §6.6.2 / §8.7.4：FFI 写 `Ptr<T>`；extern 允许任意 `Ptr<T>`。
- §6.6.3：隐式转换只进 `Ptr` / `Ptr<()>`。
- §9.7.2.4：`ptr_of` 返回 `Ptr<U>`；加 `ptr_cast`。
- 附录 D：尾部不连续默认、默认成环 / 前向引用（新码，段内递增）。

### 冲突 / 兼容

现网裸 `Ptr` 继续合法（`= ()`）。会破的：`let p Ptr = ptr_of(…)`（RHS 变成 `Ptr<U>`）；生成器改句柄类型之后需要 `ptr_cast` 的调用点。

Win32 生成（语言之后）：`void*` 仍 `Ptr`；`DWORD*` → `Ptr<u32>`；`HANDLE` 族幽灵别名。

## 评估

- 赞成：
  - 语法通用，`Ptr<T=()>` 不是编译器私货。
  - 裸 `Ptr` 免改名。
  - 句柄 / `Ptr<u32>` 有编译期身份。
- 反对：
  - 改 bnf + `.ud` 多一个默认类型槽。
  - 裸 `Ptr` 仍是无校验口；`ptr_cast` 亦然。
- 未决：无（2026-09-21：加 `T=type` 语法；#21 先做 `Ptr<T=()>`；不做 C 字符串 / `Pointer` 别名）。

## 决定

- 日期：2026-09-21
- 结论：实施
- 理由：默认实参是语言功能，声明写在 bnf 里。第一刀产品是 `Ptr<T=()>` 与句柄幽灵类型。不另起 `Pointer`。C 字符串另条。

## 规范要点

- 语法：`typeParam` 尾部可选 `= type`。使用点 `genericDefWithRef` 不变。
- 语义：
  - 默认必须尾部连续；补齐从左对齐已写实参，右侧填默认；否则 E6011。
  - `Ptr<T=()>`；裸 `Ptr` ≡ `Ptr<()>`；ABI 一指针字；`null` / `to_int`。
  - 判等按 `T`；`ptr_cast<T,U>`；无隐式跨 `T`。
  - extern 任意 `Ptr<T>`；隐式堆句柄/`T&` 只进 `Ptr` / `Ptr<()>`。
  - `ptr_of` → `Ptr<payload>`；`_ptr_*` 保持 `T`。
- 句柄：`struct _HANDLE {}` + `type HANDLE = Ptr<_HANDLE>`。
- 不做 C 字符串、不做 `Pointer` 别名。

## 落地

- 2026-09-21（`notes/0.23`）：切片 1：`typeParam` `= type`；AST 默认槽；`.ud` v6；E2041 / E2042（声明期）。使用点补齐仍是切片 2。
- 2026-09-21（`notes/0.23`）：切片 2：`fillGenericNamedTypeArity` 尾部补齐；类型位 + turbofish；调用点先推断再填默认。
- 2026-09-21（`notes/0.23`）：切片 3：`#Builtin struct Ptr<T=()>`；intern 一律带 1 个 `genericArgs`；裸 `Ptr` ≡ `Ptr<()>`。
- 2026-09-21（`notes/0.23`）：切片 4：`ptr_of` → `Ptr<payload>`；`ptr_cast`；extern 隐式只进 `Ptr<()>`。
- 2026-09-22（`notes/0.23`）：切片 5：Win32 生成 `void*` 仍 `Ptr`；`DWORD*` → `Ptr<u32>`；`HANDLE` 族幽灵别名（SDK，不进 spec）。
- 2026-09-22（`notes/0.23`）：切片 6：回写 §3 / §6.6 / §8.7.4 / §9.7.2 / 附录 C；提案标已落地。

## 实施入口

全部完成。C 字符串 / `Pointer` 别名 / 按 `T` 算术另条。

### 切片 1：bnf / rd / AST / `.ud`

| 位置 | 做什么 |
|---|---|
| `riu/ast/riu.bnf` `typeParam` | 尾部加 `(SymbolEq type)?`。**不**改 `genericDefWithRef` |
| `riu/ast/rd/parser.cpp` `parseGenericDef` | 现逻辑：无 `:` 只推 name。在 name / 边界之后 `eat(SymbolEq)` 再 `parseType()`。`parseGenericArgs` 不动 |
| `riu/ast/rd_builder.cpp` `parseTypeParams` | 抽出默认 `type`；形参名仍 `requireBareTypeParamName` |
| `struct_node.h` / `fn_node.h` / `enum_node.h` / spec 头 | 与 `_typeParams` 等长的默认类型槽（`TypeNode*` / `TypeInfo`，空 = 无默认）。`_typeParamBounds` 旁 |
| `riu/ast/mod_decl.h` `kFormatVersion` | 现 **v5**；默认类型进二进制 → **v6**（否则旧 `.ud` 错位）。`mod_decl.cpp` 读写与 typeParams 平行 |
| 附录 D 新码 | 段内递增：E2040 之后 **E2041** 默认不在尾部连续；**E2042** 默认引用更右形参 / 成环。`error_code.h` |

rd dump：`tests/rd-cases/` 加 `generic_default.ut` + `.rd.txt`（`struct Foo<T=i32>` / `fn f<T=()>` / `T : Eq = i32`）。`./tests/rd-cases/run.ps1`。

声明期 Sema：扫一遍形参，一旦出现无默认的、其左侧已有默认 → E2041。默认 `type` 解析时只允许更左形参入 `subst`。

### 切片 2：使用点补齐

`fillGenericNamedTypeArity`（`riu/ast/name_lookup.cpp`）：`got < want` 时查目标 `typeParams` 的默认，从 `got` 起全部有默认则 append 再 intern。`Rc` 无默认 → 仍 E6011。`.ud` 按源码少写落盘，加载后再 recache + `syncFnSymbolsFromAst`，跨文件 mangling 与定义方一致。

同一套填：类型出现位 + turbofish。调用点完全不写 turbofish 仍先推断，失败槽再填默认。

用户 struct 回归：`tests/check-cases/generic_default_ok.ut`（`Pair<u8>` → `U=i32`）；`diag_generic_default_*.ut`（E2041 / E6011 / E2042）。跨文件 `.ud`：`tests/projects/generic_default`。

### 切片 3：`Ptr<T=()>`（语言）

坑（先改这些，否则 `Ptr<T>` 会变成 `TypeKind::Generic`）：

- `kindForBuiltinWrapper`（`riu/include/types.h`）**没有** `Ptr`；`TypeInfo(string)` 才把裸名标成 `TypeKind::Ptr`。`TypeInfo("Ptr", args)` 必须 `TypeKind::Ptr`
- `hasGenericArgs()` 不含 `TypeKind::Ptr` → `getFullName` 印不出 `Ptr<u32>`
- `compiler_types.cpp` `kBuiltinGenerics` 现 `{"Ptr", 0}` → arity 1（缺省由 Sema 填）
- intern 键含 `T`；凡 `isPtr()` 的值都带 1 个 `genericArgs`（unit 或写出的）
- SDK `sdk/riu/src/riu/core/base.ut`：`struct Ptr<T=()>`
- 裸 `Ptr` 继续合法，勿改现网标注

`tests/check-cases/ptr_generic_ok.ut`：`let a Ptr` 与 `let b Ptr<()>` 同型；`Ptr<i32>` 不能赋给 `Ptr`。`diag_ptr_generic_mismatch.ut`。

### 切片 4：builtin

`ptr_of` 返回 `Ptr<payload>`（`call_resolve.cpp`）；`ptr_cast<T,U>` 新 `#Builtin`。`_ptr_offset` / `_ptr_as_ref` / `_ptr_write` 保持 `T`。extern 隐式转换只认 `Ptr` / `Ptr<()>`（`call_resolve.cpp` `param.isPtr()` 处收紧：`T` 必须是 unit）。

会破：`sdk/riu/src/riu/core/ptr_ffi.test.ut` 里 `let p Ptr = ptr_of(...)` → 去掉标注或写成 `Ptr<U>`。

### 切片 5：Win32 生成（语言绿了再动）

`scripts/win32/gen-win32.py` `riu_type`：`void*` / 未分所指 → 仍 `Ptr`；`DWORD*` → `Ptr<u32>`；`HANDLE` 族先吐 `struct _HANDLE {}` + `type HANDLE = Ptr<_HANDLE>`。core 不要 `use kernel32.*`（见 `BUGS.md` E3091）。

### 切片 6：回写 spec

§2 / 附录 B `typeParam`；§3 默认补齐；§9.7.2.1 `Ptr<T=()>`；§6.6.2–3；附录 D；`notes/0.23.md`。三套：`riu-check test tests/check-cases/`、`sdk/riu` 下 `riu test`、`./build.ps1 test`；`./lint.ps1`。

### 不做

C 字符串、`Pointer` 别名、按 `T` 算术、`unsafe`、表达式默认参数。`CURRENT.md` 本地阶段表与上面对齐（git 忽略）。

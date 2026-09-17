# 规范变更记录

> 上新下旧。每条记录规范层（`docs/spec/*.md`）的语义变化、章节增删、用语收口、与编译器实现的对齐点。
>
> 日常用户教程（`docs/*.md`）的改动不在此记录；语法（`riu/ast/riu*.g4`）改动以 commit log 为准，本文件只记录其在 spec 层的反映。

记录格式：

```
## YYYY-MM-DD —— 摘要（commit-hash 可选）

- **新增 / 修改 / 删除**：影响章节与一句话说明
- **冲突 / 兼容**：是否破坏既有条款引用；如破坏，给出迁移指引
```

---

## 2026-09-17 —— 泛型 enum 简单切片（#3）

- **修改 §3.10.1.1 / §3.10.1.4 / §3.10.2.6 / §3.10.3.1 / §3.10.6 / §3.10.7 / §3.4.1.1 / §2.2.2 / §6.7.1.1 / §6.5.6.8 / §7.9.2**：`enum E<T>`；使用点带齐实参（E6011）；头上 `<T : D>` → **E2037**；每个单态一份；构造 `E:<T>::V`；match 绑定 subst payload；`T ! E` 按完整类型比较。方法 / `#Impl` / struct-style payload / 显式 discriminant 仍后切片。
- **附录 B**：`enumDecl` 接 `genericDef?`；泛型构造 turbofish。
- **附录 D**：新增 E2037；E6011 亦覆盖泛型 enum 实参个数。
- **冲突 / 兼容**：原先 §3.10.2.6 写「v1 不支持泛型 enum」，现允许。非泛型 enum / match 行为不变。`.ud` 升到 v4（enum 二进制接口携带 typeParams）。

## 2026-09-17 —— 泛型 spec（#15）

- **修改 §12.1.1.3 / §12.2.1.3–§12.2.1.4 / §12.2.2.3 / §12.3.2.3 / §12.4.2.3 / §6.4.4 / §7.8.1.4 / §11.4.2.4**：`#Spec struct D<T>`、`#Impl(D<A>)`、边界 `<X : D<A>>` 端到端。穷尽性 subst spec 形参；查找按 `(S, D+实参)`；宣告次数按 `(S, D)` 基名，不同实参并存本阶段不成立（见 #16）。缺参 / 错元 **E1142**。三种 `#Impl`：闭非泛型、闭标在泛型 struct、开。无 `where` / 特化 / `Dyn<D<T>>`。
- **附录 D**：新增 E1142；E1103 / E1106 触发说明对齐基名 vs 实参。
- **冲突 / 兼容**：原先条文写「`To<i32>` 与 `To<String>` 可并存」，现改为不成立。零参 spec 行为不变。`.ud` 升到 v3（fn 边界携带实参）。

## 2026-09-15 —— 语言改名为 riu

- **修改** 全篇用语：语言名 yux → **riu**（发音通 U，非英语）；源文件 `.yux` → `.ut`（U Text）；模块声明缓存 `.decl` → `.ud`；项目配置 `yux.toml` → `riu.toml`；SDK `yux.core` / `yux.io` / `yux.time` → `riu.core` / `riu.io` / `riu.time`；CLI `yux` → `riu`。
- **冲突 / 兼容**：破坏。迁移：改扩展名与 `riu.toml`；`use yux.*` 改为 `use riu.*`；工具链命令与运行时符号（`riurt` / `riu_rc_*` / `__riu_*`）一并改名。GitHub 仓库 URL 暂不改。

## 2026-09-13 —— SDK 常用 API：路径 / 字节 / 浮点解析 / 排序

- **修改 §9.1.4.5 / §9.2.3.6 / §9.3.3 / §9.3.4 / §9.3.4.6**：`u32` ASCII 分类；`swap` / `sort` / `sort_by` 为 Array 自由函数；`String::from_bytes`、`split_once` / `rsplit_once`、`parse_f32` / `parse_f64`、`String.is_ascii`。
- **修改 §9.3.3.3**：`#Static` 形参 `T&` 接受已是 `T&` 的标识符（值上下文自解为 `T` 仍按借用匹配）；本地值仍须 `&buf`。
- **修改 §10.4.1.4**：`FileInfo.mtime`；`Path` / 路径 facade 的 `extension` / `stem` / `with_extension`；`mkdir_all` / `copy_file` / `rename` / `chdir` / `set_env` / `unset_env` / `temp_dir` / `read_bytes` / `write_bytes`。
- **冲突 / 兼容**：纯增。v1 仍无 printf `format("{:04}")`、正则、`Command` / JSON。

## 2026-09-12 —— 整数字面量按类型范围校验

- **修改 §1.6.1.3 / §3.2.4.3**：字面量不得超出其类型（推断或显式）的值范围，**E3103**。一元负号计入（`i32`/`i64` 最小值合法）。`assert_eq` 等泛型调用按推断后的 `T` 回填灵活整数；两侧皆灵活且超出 `i32` 但落入 `i64` 时升到 `i64`。
- **冲突 / 兼容**：原先超出范围可能静默截断；现为编译期错误。无类型上下文的无后缀字面量仍默认 `i32`。

## 2026-09-12 —— 独立包 `yux.time`

- **新增 §10.4.1.7**：文件模块 `sdk/yux/src/yux/time.yux`。须 `use yux.time` / `use yux.time.*`。公开 `Duration` / `Instant` / `SystemTime` / `DateTime` 与 `sleep` / `sleep_ms`。
- **修改 §10.2.4.2.2 / §10.2.5.5 / §10.4.1.3.1 / §10.4.5.1**：独立包名单加上 `yux.time`；未 use 时全限定不引入。
- **冲突 / 兼容**：纯增。`tick_ms()` 仍在 `yux.core`。v1 不含 IANA 时区、日期字符串解析、高精度 sleep 承诺。

## 2026-09-12 —— Path / File 补齐查询与链接

- **修改 §10.4.1.4**：`Path` 增加 `#Impl(ToString)`、`is_empty()`、`same_as`（绝对化后大小写不敏感）。`File` 增加 `#Impl(ToString)`、`is_dir` / `is_file` / `is_link`、`same_as`、`symlink_to` / `hardlink_to` / `junction_to`（接收者是要创建的链接）。
- **冲突 / 兼容**：纯增。字符串 facade 不变。`examples/wln` 改走对象 API。

## 2026-09-12 —— `IoErr` 可读消息

- **修改 §10.4.1.4**：新增 `io_err_text(e IoErr) String`。命名 variant 固定英文短句；`Other(code)` 走 `FormatMessageW`，失败则 `Win32 ${code}`。enum 不能挂方法，故为自由函数。
- **冲突 / 兼容**：纯增。

## 2026-09-12 —— 位运算改为整数方法

- **删除**运算符 `&` `|` `^` `<<` `>>` `~`（一元）与复合赋值 `&=` `|=` `^=` `<<=` `>>=`。
- **修改 §4.4.2**：整数走 `#Builtin` 方法 `and` / `or` / `xor` / `shl` / `shr` / `inv`。`bool` 仅 `not` / `!` / `&&` / `||`。
- **修改 §1.7 / §2.4 / §5.2.1.2 / 附录 A·B / §7.2.3.2 / §9.1.4**：`&` 只作 `T&` / `&obj`；`~` 只作 `fn ~()`。lexer 注释 `|` `^`；删除 `&=` `|=` `^=`。
- **冲突 / 兼容**：破坏。`a & b` 写 `a.and(b)`；`~a` 写 `a.inv()`；`a &= b` 写 `a = a.and(b)`。负数接收者 `(-5).and(3)`。

## 2026-09-12 —— 单字段结构体字面量位置填充

- **新增**：`Type{ expr }` / `Self{ expr }` 单行简写，把 `expr` 赋给唯一实例字段（§7.3.2.4 / §7.10.3.4）。`{` 后以 `(` 开头不走简写，避免与尾随 lambda 冲突。
- **新增 E3129**：0 或 ≥2 个实例字段时不得用位置填充。
- **修改 g4 / 附录 B / 附录 D / §4.10.3.1 / §11.11.3.2**：`exprStructLit` 增加 `{ expr }` 分支；命名多行形态不变。
- **冲突 / 兼容**：纯增。既有 `Type { \n .f = expr \n }` 行为不变。

## 2026-09-10 —— 再收紧 T& 返回 / 声明头

- **新增 E4040**：不得返回 `Array<T&>` / `[T& * N]` 容器；裸 `T&` 仍走 §8.6.10。
- **修改声明头**：`fn f<T&>` / `struct Foo<T&>` / 别名 `A<T&>` → **E4037**（写 `fn f<T>(x T&)`）。
- **修改调用点**：自由函数返回 `T&` 的根恒为 `$rodata`，不再把 T& 实参当返回源。
- **冲突 / 兼容**：`fn leak(s S) T& = &s.field`、lambda `() T& => &$.n` 继续 E4020（定义侧白名单）。

## 2026-09-10 —— 合并 `type` / `typeWithRef`

- **修改 g4**：删 `typeWithRef`，`type` 尾部可选 `&`。持有位（字段 / 别名 / 全局 / enum payload）裸 `T&` / `Array<T&>` / `[T& * N]` → **E4039**。`Array<T&>` 可作形参 / 局部。
- **修改 §8.6.10**：自由函数 / lambda 返回 `T&` 只允许 `$rodata`；形参透传 → E4020。方法仍是 `$` + 静态。不再按 T& 形参个数报 E4021。
- **冲突 / 兼容**：`fn alias(p T&) T& = p` 从合法变为 E4020。字段 `i32&` 从 ANTLR 语法错变为 E4039。

## 2026-09-10 —— `<>` 内 `T&` 从语法迁到语义

- **修改 g4**：`typeGeneric`、调用站 turbofish（`exprCall` / `exprCallTrailingOnly` / `Type:<T>::`）接 `genericDefWithRef`。声明头 `fn f<T : D>` 仍走 `genericDef`。不合并 `type` / `typeWithRef`。
- **修改 §3.2.3.2 / §3.11.6.1 / §8.6.7.1 / §12.9.5 / 附录 B**：`Function<i32&, ()>` 可作字段与别名；`Dyn:<D&>(x)` 可写。`Array<T&>` / `Rc<T&>` / 用户 `Foo<T&>` / `f:<i32&>` → **E4037**；owned 位的 `Dyn<D&>` → **E4038**。
- **冲突 / 兼容**：纯增。原先这些形态是 ANTLR 语法错；字段 `i32&`、全局 `T&`、`T& &` 仍语法拒。

## 2026-09-09 —— 静态 fallible 调用支持 `!` 传播

- **修改 §4.2.1 / §4.12.1 / §7.10.2 / 附录 B**：`Type::static_fn(args)!` 与 `Self::static_fn(args)!` 支持在同错误类型的 `T ! E` 调用方中直接传播；诊断规则与普通调用一致。
- **边界**：`exprEnumCtor` 共用语法槽，但 enum 构造与静态字段不是 fallible 调用，后写 `!` 报 E7001。
- **冲突 / 兼容**：纯增；既有静态调用、enum 构造和静态字段行为不变。

## 2026-09-09 —— 实例字段默认可变措辞对齐

- **澄清 §7.1.4 / §13.5.3**：实例字段无注解时默认浅可变，可重新绑定字段自身；`#Val` / `#Frozen` 用于收紧不可变性。`#Mut` 当前不附着实例字段。
- **冲突 / 兼容**：仅修正文档漏写及反射章节的错误措辞，与现有编译器行为一致，无语义或 ABI 变化。

## 2026-09-09 —— `yux.io` 分层文件 API

- **修改 §10.4.1.4**：新增 `Path` / `File` / `FileInfo` / `FileInputStream` / `FileOutputStream` 与 `FileKind`；补充文件流所有权、EOF、严格 UTF-8、显式关闭和刷新语义。
- **错误模型**：`IoErr` 新增 `Closed`，用于关闭后读、写或刷新。
- **冲突 / 兼容**：既有字符串函数 facade 与签名保持不变，内部改由对象 API 复用实现；新增公开枚举分支要求穷尽 `match` 的调用方补充 `Closed` 分支。

## 2026-09-08 —— `StringBuilder.append` 标量文本重载

- **修改 §9.4.2**：`StringBuilder.append` 新增 `i8` / `i16` / `u16` / `i32` / `i64` / `u64` / `usize` / `f32` / `f64` / `bool` 重载，追加对应 `to_string()` 文本。
- **语义保留**：`u8` 仍按 ASCII 字节追加，`u32` 仍按 Unicode 码点追加；二者需要十进制文本时显式调用 `to_string()`。
- **冲突 / 兼容**：纯增量；既有 `u8` / `u32` / `String` 重载行为不变。

## 2026-09-08 —— SDK `Set<T>` 与 Map `get_or`

- **修改 §10.4.1.5**：新增纯 yux `Set<T>`，内嵌 `Map<T, bool>` 去重，公开 `make` / `insert` / `contains` / `remove` / `len` / `is_empty` / `keys` / `clear`。
- **Map 小补**：新增 `get_or(key K&, default V) V`；`Map` 与 `Set` 明确为 `#NoCopy`。
- **冲突 / 兼容**：`Set` 与 `get_or` 为纯增量。`Map` 的所有权约束收紧：既有隐式复制需改为 `move`；此约束与其内含 `Array` 的实际存储语义对齐。

## 2026-09-08 —— `Array` 急切高阶方法

- **修改 §9.2.3 / 新增 §9.2.3.5**：公开 `any` / `all` / `filter` / `map<U>`；回调元素统一为 `T&`。`any` / `all` 短路，`filter` / `map` 急切返回独立新数组，不引入惰性迭代器。
- **类型实参**：`map` 可写 `arr.map:<U>(transform)`；当实参已有明确 `Function<T&, U>` 类型时可反推 `U`。
- **冲突 / 兼容**：纯增量；既有 `Array` 方法和所有权规则不变。

## 2026-09-07 —— `Array` 查询 / 改位方法

- **修改 §9.2.3 / §9.2.3.2**：公开 `index_of` / `last_index_of`（`T&` → `usize?`）、`get_or_null` / `first_or_null` / `last_or_null`（`T?`，命中走 `copy_of`）、`insert` / `remove_at` / `reverse`（`#Mut` 接收者）、`take` / `drop`（`slice` 包装）。`insert` 在 `i == len` 时等价 `push`；`i > len` 与 `remove_at` 越界终止。
- **冲突 / 兼容**：纯增量。`get` / `first` / `last` 越界仍终止。

## 2026-09-07 —— `String` 切片 / 查找 / 解析 / `Ord`

- **修改 §9.3.3 / §9.3.4 / §9.3.5**：公开 `slice` / `slice_from`（夹取与 `Array.slice` 相同）、`index_of` / `last_index_of`、`strip_prefix` / `strip_suffix`、`from_codepoint` / `join`、`cmp`；`parse_i64` / `parse_u32` / `parse_u64` / `parse_usize` / `parse_bool`。`String` `#Impl(Eq)` / `#Impl(Ord)`，于是 `"a" < "b"` 合法。
- **修改 §9.3.4.1**：切段用 `slice`，仍不提供 `substring` 别名。
- **冲突 / 兼容**：纯增量。`get(i)` 越界仍终止。

## 2026-09-07 —— `yux.io` 路径类型与链接

- **修改 §10.4.1.4**：公开 `basename` / `abs` / `is_dir` / `is_file` / `is_link` / `remove_all` / `symlink` / `hardlink` / `junction`。
- **兼容**：既有 IO API 不变。`remove` 仍只删文件或空目录；树删除走 `remove_all`。软链失败时由调用方决定是否改 `junction`（`examples/wln`）。
- **冲突 / 兼容**：无破坏。

## 2026-09-07 —— SDK `Number` spec 与静态字段契约

- **新增 §12.7.5**：`Number` 统一 `MIN` / `MAX`、固定数值转换（不含 `to_bool`）、算术、比较与取负方法；12 个内置数字类型显式 `#Impl(Number)`。
- **修改 §12.1.1.1 / §12.2.2.1**：spec 可声明 `#Cval #Inline` 静态字段契约；`#Impl` 会校验字段名、替换 `Self` 后的类型及存储属性，不符报 E1141。
- **实现对齐**：`Number` 运算签名使用按值 `Self`，由已有 `#Builtin` intrinsic 直接满足；不使用会通过运算符回调同名方法的默认体。`num.yux` 删除重复的数值借用重载，并补齐 `to_i64`。
- **兼容**：内置数字方法调用行为不变；新增 `Number` 只是可选泛型边界。用户类型一旦显式 `#Impl(Number)`，必须完整满足其契约。
- 实施记录见 `docs/dev/number-spec-impl-log.md`。

## 2026-09-07 —— pkg 包边界与 `to` 访问检查落地

- **实现对齐**：§10.2.4.2–§10.2.4.7 的 `use`、包点链及限定类型路径按调用方检查清单；同目录兄弟可互访，包外仅使用公开／定向项的导出名。
- **诊断**：未导出或使用改名前的源名报 E5018；不存在的 `to` 目标报 E5019，并定位到 `pkg` 对应行；非法行继续报 E5020。
- **SDK 公开面**：core parent scope 仅扁平公开 `name.*`，公开 `name` 只登记模块别名；Win32 声明移入 `helper to yux.io`，不再向用户暴露裸 `GetStdHandle` / `yux.core.GetStdHandle`。
- **兼容**：有 `pkg` 的包不再允许通过全名访问未导出的子模块；裸数学函数改用 `math.fn`。无 `pkg` 的包行为不变。
- 实施记录见 `docs/dev/pkg-export-impl-log.md`。

## 2026-09-06 —— extern：yux 名按模块分区，LLVM 只认 C ABI 名

- **修改 §6.6.1.2–§6.6.1.4**：`extern fn` 在 yux 层与普通 `fn` 一样按模块分区；链接 / LLVM 只用 `#CName`（或声明名）。多模块可重复声明同一 C 签名；无限定名优先本模块；签名不同 → E2036。
- **修改 §10.4.1.3.1**：扁平进 `yux.core` 的成员可写 `yux.core.name`（如 `yux.core.GetStdHandle`）。
- **附录 D**：E2036。
- **冲突 / 兼容**：原先本模块再声明 SDK 已有的同名 extern 会 E6014；现可声明并调用，或用 `yux.core.fn` / `mod.fn` 选另一份声明。

## 2026-09-06 —— CLI：`args()` / `tick_ms()` / `String.parse_i32`

- **新增 §10.4.1.6**：`args() Array<String>`（含 argv[0]）、`tick_ms() u32`；`fn main() i32` 仍不做，非零退出走 `exit`。
- **新增 §9.3.4.6**：`String.parse_i32() i32?`；可选 `+`/`-`，不吃空白，溢出 / 非法为 `null`。
- **附录 C**：`args()`。
- **冲突 / 兼容**：纯增量。链接默认加 `shell32.lib`（`CommandLineToArgvW`）。

## 2026-09-06 —— FFI D2：C API 操作 yux 仍持有的 Rc / Nullable / String

- **新增 §8.7.4.7 / §9.7.2.9**：`yuxrt.lib` 手写 `yux_rc.h` / `yux_nullable.h` / `yux_string.h`。C 可读 payload、extra retain/release；**不**从 `Ptr` 重建 yux 类型；最后一次 strong 归零且 `T` 含嵌套堆字段时必须走 yux typed release。
- **修改 §6.5.6.6 / §8.8.4.1**：Rc 计数协议符号改为 `yux_rc_*`；`__yux_rc_block_count` 定义在 yuxrt。
- **冲突 / 兼容**：IR 不再定义 `__yux_box_alloc` / `__yux_box_retain` / `__yux_box_release`；链接 yuxrt 即可。用户 yux 代码无破坏。

## 2026-09-06 —— FFI D1：extern 白名单 + C-layout struct + `Ptr` 不透明

- **改写 §6.6.2**：允许表 = 标量 / `bool` / `Ptr` / C-layout struct / void；`Heap` E4028、`Function` E2031、`Dyn` E1136、其余堆句柄/`T&`/enum E2034；字段 E2035。白名单强制。
- **修改 §8.7.4 / §7.5.3 / §9.7.2 / §8.3a.5.3 / §9.5a.2.2 / §12.9.10**：Win64 C ABI（`bool` 边界 i8；小聚合当整数、大聚合 `byval`/`sret`）；C 返回 `Ptr` 不透明，禁止转成 `T&`/`Heap`/`Rc`；取消用户向 `Heap:<T>(Ptr)` 接管。
- **修改 §10.1.1.6**：`[link] lib_dirs`。
- **新增 §9.7.2.8**：`c_string` / `from_c_chars` 拷贝，不 wrap `Ptr`。
- **附录 C / D**：C-layout 术语；E2034 / E2035。
- **冲突 / 兼容**：原先能写进 `extern` 的 `Rc`/`String`/`T&`/非 C-layout struct 现报错；迁移：签名改 `Ptr` + `ptr_of`，或把 struct 收成标量/`Ptr`/`[T*N]`。

## 2026-09-06 —— `continue` / `for-in`（v0.21 Phase C）

- **新增 §5.5.3 / §5.5.4**：`continue` / `continue@label;` 与 `for item in expr`（item 为 `T&`；仅 `Array<T>` / `[T*N]`，可 peelRef；长度入口拍照）。
- **修改 §5.5.1–§5.5.2 / §5.5.5 / §5.7**：`break`/`continue` 作用于 `loop` 与 `for`；label 共用栈（E3022 / E3025 / E3094）。
- **修改 §1.5 / 附录 A / 附录 B §B.7**：关键字 `continue` / `for` / `in`；产生式 `statementContinue` / `statementForIn`。
- **修改 附录 C / 附录 D**：控制流术语；E3025 / E3094 文案泛化；新增 E3160。
- **冲突 / 兼容**：`for` / `in` / `continue` 升关键字，不得再作标识符。无 `while` / C 风格 `for(;;)` / `Iter<T>`。

## 2026-09-05 —— LLVM 符号与全限定 / yux 声明调用同形

- **修改 §6.5.6**：用户/SDK 符号用定义模块全限定；形参类型带 owner；静态方法 `::`；泛型 `<>`；`@Spec` 写作 `m@Spec`。不用 `$` / `_` 当路径分隔。泛型实例多 TU 靠 `linkonce_odr` 合并。
- **修改 §12.9.7.1 / §12.10.8.5**：vtable `__yux_vtable.<U>.<D>`；消歧符号 `S.m@Spec`。
- **冲突 / 兼容**：IR 符号名变更（informative，用户代码不得依赖）；链接仍由编译器生成。

## 2026-09-05 —— 限定类型路径、裸名分层、`yux.io` 独立包

- **新增 §3.2.5**：类型位为 `typePath`；身份 = 声明模块 + 短名；全限定只消歧、不引入。
- **修改 §10.2.1 / 新增 §10.2.2.3**：`use a.b.MyType` 具名类型导入；`use a.b.*` 同时登记末段别名。
- **改写 §10.2.5**：裸名 L1→L2→L3；同层多候选 [E5015]；废止先到优先。模块/包当值 [E5016]、类型当值 [E5017]。点链首段路径前缀 vs 局部值 [E5015]。
- **修改 §10.4.1.3.1 / §10.4.1.4**：IO 迁出为文件模块 `yux.io`（`sdk/yux/src/yux/io.yux`）；默认只 `yux.core.*`。未 `use yux.io` 时 `yux.io.xxx` 非法；用户可自定义 `io`。
- **修改 §10.4.4.2 / 附录 A.2.1**：允许与 core 同名的本文件声明；裸名用自己的，SDK 走路径。
- **修改 附录 B**：`type` / `typeWithRef` / `imports` 走 `typePath`。
- **修改 附录 D**：E5015 / E5016 / E5017。
- **冲突 / 兼容**：**破坏**无 use 的 `yux.core.io.fn` / 裸名 `read_file`（须 `use yux.io` 或 `use yux.io.*`）。迁移：`use yux.io` 后写 `io.read_file` / `yux.io.read_file` / `io.IoErr`。

## 2026-09-04 —— 跨文件诊断路径用出错节点所属文件

- **修改 D.1.1**：诊断 `file` 取 AST 节点 `enclosingFile` 的源路径；spec 默认体 fall-through 等跨文件 codegen 不再把 spec 行号绑到正在编译的入口文件。入口路径仅作回退。
- **冲突 / 兼容**：纯诊断呈现；错误码与行号不变。

## 2026-09-04 —— SDK 子模块别名与全限定路径

- **新增 §10.4.1.3.1**：每个 `yux.core.<stem>` 在 parent scope 登记末段模块别名（`io.read_file` / `math.abs`）与全限定包路径（`yux.core.io.read_file`，隐式包别名 `yux`）。`pkg` `name.*` 扁平与别名并存。用户项目 `use yux.core.io` 命中已加载 SDK 模块，不走用户源根。类型名仍为单 `ID`（不写 `io.IoErr`）。
- **修改 §10.4.1.4 / §10.4.1.5**：IO / Map 用法补别名与全限定；类型名仍靠扁平。
- **冲突 / 兼容**：纯增量。原先裸名 / `use yux.core.io.*` 不变。

## 2026-09-04 —— SDK String trim/lines 与 `Map<K, V>`

- **修改 §9.3.4 / 新增 §9.3.4.5**：`trim` / `trim_left` / `trim_right` / `lines()`。空白为 ASCII U+0009–U+000D 与空格；`lines` 按 `\n` 切、`\r\n` 归一，末尾换行不产生空段。§9.3.4.1「不提供」清单去掉 `trim`（仍不提供 `substring` / `repeat`）。
- **新增 §10.4.1.5**：`map.yux`（`pkg` `map.*`）纯 yux `Map<K, V>`：`make` / `insert` / `get` / `contains` / `remove`；线性查找；`K` 须可 `==`；不承诺 O(1) / 哈希。
- **冲突 / 兼容**：纯增量。

## 2026-09-04 —— SDK `yux.core.io` 文件 / stdin IO

- **新增 §10.4.1.4**：`io.yux`（逻辑名 `yux.core.io`，`pkg` `io.*`）提供 `IoErr` + `read_line` / `read_file` / `write_file` / `append_file`；错误通道 `T ! IoErr`；路径 UTF-16、内容 UTF-8。`eprint` / `eprintln` 仍在 `base.yux`。
- **冲突 / 兼容**：纯增量。隐式 `yux.core` 即可裸调；`use yux.core.io.*` 可选。

## 2026-09-04 —— Fallible 签名 `T ! E` 删除 `#Fallible` 注解（F6）

- **修改 §6.7**：标题改为「失败签名 `T ! E`」；失败声明从 `#Fallible(E)` 注解迁移为签名后缀 `fn f(...) T ! E` / `fn main() ! E`（void 成功）。
- **修改 §11.5.1**：删除 `#Fallible(E)` 行；§11.1.1.1 单参数糖示例不再列举 `#Fallible`。
- **修改 附录 D**：删除 E7020（过渡弃用 warning）；E7019 改为重复 `! E` 后缀；E7001–E7008 / E7013 / E7016–E7017 措辞统一为 `T ! E`。
- **冲突 / 兼容**：**破坏**旧写法 `#Fallible(E) fn f() T`；迁移为 `fn f() T ! E`。旧注解报 E2005 + hint。

## 2026-09-03 —— v0.20 编译器结构收口（实现归档）

- **SemaPass 收口**：调用 / 方法 / statement / 泛型体 / target-type 上下文语义检查迁入 `yux/frontend/sema/`；`kMigratedCodes` 反转；已覆盖码 Compiler 副本改 `throwSemaGap`（E3091）。残余 LLVM/内部兜底与未实例化泛型体策略见 `rules/sema-codegen.md`。
- **基础设施**：NameResolver 唯一查找；OwnershipOps 三入口；Array/String `#Builtin` 表驱动；`yux build --test --threads` 分片 spawn；模块 `.decl` + `yux-check test --threads`。
- **冲突 / 兼容**：无用户可见语法/语义破坏。`yux-check` 与用户路径语义对齐；已知慢迁项不挡 v0.20 退出。

## 2026-09-03 —— `<-` LHS 形态报 E4036

- **修改 §4.13.2.2 / 附录 D**：`<-` 左侧不是变量 / `$` / 字段 / 元组 `.N`（含索引）报 E4036。原先 Compiler 用占位 E0000，`yux-check` 漏报。
- **冲突 / 兼容**：无用户破坏；非法 `<-` 左侧从内部码改为正式诊断。

## 2026-08-28 —— String：`to_upper` / `to_lower` / `ends_with` / `split`

- **修改 §9.3.4**：方法表补 `starts_with` / `contains`（v0.18 已落地、表未列）以及 `ends_with` / `to_upper` / `to_lower` / `split`。`to_upper` / `to_lower` 仅 ASCII `a-z` ↔ `A-Z`（§9.3.4.3）；`split` 语义见 §9.3.4.4。
- **修改 §9.3.4.1**：从"不提供"清单去掉 `to_upper`；`trim` / `substring` / `repeat` 仍不提供。
- **冲突 / 兼容**：无用户破坏。

## 2026-08-28 —— Array:<T>::with_capacity 静态工厂

- **修改 §9.2.2.1 / 新增 §9.2.2.1a / 修改 §9.2.3**：空数组只走 `[]`；删除旧 `Array<T>()` 构造。新增 `#Builtin #Static` 工厂 `Array:<T>::with_capacity(n usize)`（调用须 turbofish）。
- **修改 §7.10.6**：SDK 工厂表补 `Array<T>`。关闭 §7 Open Issue「`#Builtin #Static fn` 路径尚未实施」。
- **冲突 / 兼容**：无用户破坏（旧 `Array<T>()` 构造早已删除；空数组继续写 `[]`）。

## 2026-08-28 —— §11.13.2.3：泛型 `#NoCopy` 字段识别 E4032

- **修改 §11.13.2.3**：去掉"修饰名漏检"实现限制。`#NoCopy` 查找按基名；泛型模板含 `#NoCopy` 字段同样报 E4032；字段类型为本模板形参时仍不报。
- **冲突 / 兼容**：破坏性，仅影响原先漏检的写法（如 `struct W<T> { inner MyNoCopy<T> }` 未标 `#NoCopy`）。补上 `#NoCopy` 即可。

## 2026-08-27 —— §6 收口：spec 边界已落地；extern / `#Mut` 形参 / T& panic 转后续

- **关闭 Open Issues**：类型约束即 `<T : D>` spec 边界（§12），不另引入 trait。析构体允许写字段（不属 const-mut，§7.1.4.3）。不引入 `var p T`（`var` 已删）。
- **转**：extern 白名单完整收紧（不引入 `unsafe`）→ 后续版本 / 错误模型 FFI。`#Mut` 形参 → 后续版本。`T&` 非空假设的 panic / 运行时检查 → 错误模型（v1 为 UB）。
- **修改 §6.1.2 / §6.2.1 / §6.3.1.1a / §6.4 / §6.5.4 / §6.6.2**：`structImpl` 改为 `structDecl` 体。E4021 与 §8.6.10.3 对齐（最多一个 `T&` 形参）。"draft 边界"改为 spec 边界；单态化校验只走 `#Impl(D)`。extern 表补 `Heap`（E4028 已强制）/ `Function`（E2031 已强制）/ `Dyn`（E1136 已分配）。
- **修改 §7.1.2.3 / §8.7.4.2 / §11.9.2.1 / 附录 C.6a**：泛型 struct 可带 `<T : D>`；去掉 §8 残留 Open Issue；写明 `#Mut` 不附着形参；术语表 "draft 边界" 改为 spec 边界。
- **冲突 / 兼容**：无行为变化。规范追上已落地的 spec 边界、形参默认浅不可变、extern 部分强制。

## 2026-08-27 —— §11 收口：未知注解报错；自定义 / 多参 / 表达式级转后续

- **关闭 Open Issues**：`#Test` 隔离已落地（DLL 子进程 + 进程内 SEH）。`@sdk_only` 不引入，Ptr 算术走 `_ptr_offset`（与 §8 重复）。用户自定义注解、多参数 / 命名参数、表达式级注解转后续版本（anno-struct）。`#Reflect` 命名参数转 §13。`assert_eq` 用户类型与断言打印实参转后续版本（v1 数值+bool+String；不引入独立 `Stringify`）。
- **修改 §11.1**：`buildAnno` 单参与 g4 `annoArg` 对齐（ID / 数字 / 字符串 / `type`）；可附着位置补 `letAnno` / `paramAnno`，去掉已不存在的 `globalConst`。未知名按位置报 E2005 / E3112 / E3108 / E3105。
- **修改 §11.2.2.3 / §11.4.3.2 / §11.5.2.4 / §11.15**：`#Builtin` 无路径白名单；`#DraftLike` 非 spec 位已报 E1110；用户自定义注解从重复的 §11.6 改编号为 §11.15。
- **修改 §6.1.3 / 附录 A.3 / §12 Open Issues**：未知注解不再是"解析但不绑定"；附录补 `#CName` / `#Reflect` / `#NoCopy` / `#Inline`。
- **冲突 / 兼容**：无行为变化。规范追上已落地的注解白名单与测试隔离。

## 2026-08-27 —— §12 收口：内置 spec 清单；操作符糖 / extend / AnyRef 转后续

- **关闭 Open Issues**：`Eq` / `Ord` / `ToJson` 方法契约已落地（`base.yux` / §12.10.6）。编译期 `Reflect` 已落地（§13）。跨编译单元边界 IR：v1 调用方需可见泛型函数体（§6.4.2.3）。操作符 spec 语法糖转后续版本（运算符维持 §7.2.3）。`Stringify` 与 `assert_eq` 用户类型转 §11。`is` / `as` / `AnyRef` 转后续版本。扩展实现块 / `extend` 转 v1.x 包管理。
- **新增 §12.7.4 / §12.5.3**：列出 `Eq` / `Ord` / `ToJson`；写明与 §7.2.3 方法名重合但不是操作符 spec。v1 不引入 `extend`。
- **修改 §12.7.1.2 / §12.7.2.2 / §12.8 项 6–7**：ToString 实现分文件；`Any` 删除后编译期反射已由 §13 承接；项 7 划掉已落地的编译期反射。
- **修改 §6.4.2.3**：单态化要求调用方可见函数体，不跨 CU 共享边界 IR。
- **冲突 / 兼容**：无行为变化。规范追上已落地的 SDK spec 与 v1 单态化边界。

## 2026-08-27 —— §10 收口：别名导入、无下沉、第三方库转包管理

- **关闭 Open Issues**：`pub` / `private` 永不引入（已是永久决议）；无 `pkg` 时保留别名导出（已决议）；`name as alias` 已落地。循环依赖 v1 禁止、不引入 forward 声明（§10.2.6.2）。嵌套模块无可见性下沉（§10.3.3）。第三方库转 v1.x 包管理（§10.4.5）。`extension` 转 §12。
- **修改 §10.2.2 / §10.2.3 / §10.2.5**：非通配 `use` 只登记末段别名，不注入裸名（对齐编译器）。`use` 须在其它顶层声明之前。同名冲突：E2004（导入时）/ E2008（通配别名使用点）；扁平成员先到优先。
- **修改 §10.4.1.3**：`yux.core` 为每个用户模块的 parent scope（隐式回退）；显式 `use yux.core.*` 可写。
- **修改 §9.10.2 / §12 Open Issues / 附录 A.6.2**：`extension` 窗口改指 §12；`extend` 列入未来保留字（当前仍可作 `ID`）。
- **冲突 / 兼容**：无行为变化。规范追上已落地的别名导入与 SDK 回退。

## 2026-08-27 —— §9 收口：工厂 / clone / Ptr 例外；to_string 合约

- **关闭 Open Issues**：`String(buf)` 已由 `String::from`（深拷贝）+ SDK 私有 `_take_buf`（move-in）替代；`rc:<T>` / `Ptr` 无泛型已落地；`Ptr` 不归入 `T?`（v1 不改）；Ptr 算术与 §8 重复（`_ptr_offset`）；`extension` 窗口转 §12；`to_string()` 返回堆 `String`（§9.1.4.2，关 §3 对应条）；`Array.clone()` 为深拷贝唯一入口（A4）。
- **修改 §9.2 / §9.3 / §9.4 / §9.7 / §9.10**：方法表补 `clone()`；构造改为静态工厂；`StringBuilder::make()`；`Ptr` 标题去掉 `<T>`。
- **修改 §3.6.1.3 / §7.6a.5 / §7.10.6 / §8.4 / §11.13.1.1 / §12.7.3.2**：`null` 的 `Ptr` 例外；`copy_of` 拒 `#NoCopy`（含 Array）。
- **转**：越界 / 空 `pop` / `Nullable.get()` 改为可恢复错误 → 错误模型（v1 保持 `_exit(1)`）。
- **冲突 / 兼容**：无行为变化。规范追上已落地的工厂与 `clone()`。

## 2026-08-27 —— §8 收口：草案已迁入；Array 与 RC 脱钩

- **关闭 Open Issues**：草案 §12 迁移；peephole retain/release（正文 §8.7.2.3 已记 v1 不做）；派生 `__copy_S` / `__destroy_S`（与 §7 重复）；`Ptr` `@sdk_only`（v1 走 `_ptr_offset`）；`copy_of` 非 Array 已落地；`_ptr_as_ref` / `_ptr_write` 已有 SDK 测试；Array 字面量哨兵（B-3 后不再适用）。
- **修改 §8.1.1 / §8.2.1.2 / §8.2.3.3 / §8.4 / §8.7.1.4 / §8.7.6.4**：Array 按 B-3 与 §9.2 对齐——三字段内联、`#NoCopy`、不参与 RC；哨兵仅 `String` 字面量；闭包捕获 Array 走 move。
- **修改** `DRAFT-所有权与引用.md` 头部：标注已落地，见 §8。
- **保留**：`Weak<Array<T>>` / `Weak<String>` 转 v1.x。Array `.clone()` vs `copy_of` 归 §9 Open Issues。
- **冲突 / 兼容**：无行为变化。规范追上已落地的 B-3 实现。

## 2026-08-27 —— §4 收口：`??` > `<-`；优先级表与 Open Issues

- **语法**：`yuxParser.g4` 把 `exprNullElse` 挪到 `exprMoveAssign` 之前，并为 `??` 加 `<assoc=right>`。`x <- y ?? z` 解析为 `x <- (y ?? z)`（原先 g4 顺序会解析成 `(x <- y) ?? z`）。
- **修改 §4.2.1**：优先级表按 g4 补 `exprLambda` / `exprStructLit` / `exprMatch` / `exprEnumCtor` / `exprTupleMember` / `exprTuple`；`??` 22、`<-` 23。
- **修改 §4.7.1 / §4.8.2.2 / §4.10.3.1 / §4.13.1.1**：`&` 不作用于调用结果；隐式取址仅运算符与泛型 `T&` 推断；普通位用 `Type { ... }`，`Self { ... }` 仍限 `#Static fn`；`<-` 低于 `??`。
- **修改 附录 B.6 / `docs/基础语法.md`**：分支序与教程优先级表对齐（移位 > 位运算，`??` > `<-`）。
- **关闭 Open Issues**：§4.2.1 优先级表；§4.7.1 `&` 作用于调用；§4.10.3.1 `Self { ... }` 出现位；§4.8.2.2 隐式取址。
- **冲突 / 兼容**：破坏性，仅影响无括号混写 `<-` 与 `??` 的表达式。请写成 `x <- (y ?? z)` 或依赖新优先级。

## 2026-08-27 —— §2 收口：拼合算符、显式换行槽、顶层产生式对齐

- **修改 §2.2**：`program` 与 `yuxParser.g4` 对齐（`LineEnd*` + `aliasDecl` / `enumDecl`；去掉独立 `structImpl`）。注释走 HIDDEN，不进产生式。
- **修改 §2.3.2**：正式否定隐式续行与反斜杠续行；列出 `LineEnd*` 槽位（方法链走 `exprDot` 的 `.` 前换行）。
- **修改 §2.4 / §1.7.2**：`??` / `?.` **不**升格为词法 token，与 `<<` / `<=` 同为语法拼合。`<-` 改为词法 token（原表误列为 `'<' '-'`）。优先级由 `expr` 分支序决定。
- **修改 §2.5 / 附录 B.1 / B.4 / B.6 / B.9 / 附录 C / §6.1.1**：总览与 `program` 摘录对齐 g4；`fnHeader` 改为 `fn name<T>`（原摘录误为 `fn <T> name`）；`exprMoveAssign` 改为 `'<-'`；写明附录 B 为手工摘录、无自动校验脚本。
- **关闭 Open Issue**：§2.4 token 升格；附录 B 自动校验脚本；隐式续行。
- **冲突 / 兼容**：无行为变化。

## 2026-08-27 —— 字符串转义对齐；LineEnd 去掉孤立 `\r`

- **修改 §1.1.2 / 附录 A / 附录 B**：`LineEnd` 与 `yuxLexer.g4` 对齐为 `\n` / `\r\n` / `EOF`；孤立 `\r` 不是合法换行。
- **修改 §1.6.4.1 / §4.3.1.6**：转义列表合并为 `\n \t \r \0 \v \b \\ \" \' \$ \xNN \uNNNN`；未知 `\X` 与不完整 / 非十六进制的 `\x` `\u` 为编译期错误 E2033（不再按字面追加）。
- **关闭 Open Issue**：§1.6.4 转义对照；是否将 `\r\n` 显式列入（正文已列，孤立 `\r` 删除）。
- **冲突 / 兼容**：破坏性。原先 `"\q"` 得到字面 `q`；现在报 E2033。请改成字面字符或合法转义。`r"..."` 不受影响。

## 2026-08-26 —— `?.method(args)` 安全方法调用

- **修改 §3.6.3 / §4.6.2 / §4.8.1.1 / §2.4 / 附录 C**：放开 `a?.method(args)`。receiver 为 `T?` 时从内层 `T` 查找方法；持值调用并包装为 `U?`，空则短路且实参不求值。省略返回时结果为 `()?`。语法是 `exprDot`（带 `?`）作为 `exprCall` 左侧，g4 无需改。
- **冲突 / 兼容**：纯增量。此前规范写 v1 不支持；编译器自 `c5f771f` 已实现字段以外的方法路径，现与规范对齐。

## 2026-08-26 —— match arm 支持块体 `=> { stmts }`

- **修改 §3.10.7 / §5.4.4 / §5.7.1 / 附录 B / DRAFT-枚举 §5.5**：`match` arm 体可为 `=> expr` 或 `=> { stmts }`。块值规则与 if 同（末表达式无 `;` 即块值；`ret` 流终止不参与类型合并）。块可单行：`E::V => { 1 }`。
- **语法**：`matchArm` 从 `=> expr` 改为 `=> (statementBlock | expr)`。
- **冲突 / 兼容**：纯增量。既有 `=> expr` 单行形态不变。

## 2026-08-26 —— 块可单行；lambda 形态收敛为括参前缀 + 调用尾随

- **修改 §2.3.2.3 / §5 / 附录 B**：`statementBlock` 不再强制 `{` 后 / `}` 前换行。`fn f() { ret 1 }`、`if c { a } else { b }` 与多行写法等价。语句末 `codeLineEnd` 在块末可省。
- **修改 §4.11 / §4.8.4**：lambda 只保留 `(args) Ret? => expr` 与 `(args) Ret? => { stmts }`。去掉裸 `x => expr`、表达式位 `{ args => stmts }` / `{ stmts }`。尾随**仅**挂在函数调用：`foo(xxx){ () => stmts }` / `foo { () => stmts }`（唯一实参可省 `()`）。
- **修改 §4.11.3**：括参无返回标注不再默认 void。形参类型与返回类型均可从期望函数类型或 body 推断。
- **语法**：`yuxParser.g4` 删除 `exprLambdaSingle` / `exprLambdaBlock` / `exprLambdaZeroBlock`；`trailingLambda` 统一为 `{ '(' params? ')' Ret? '=>' stmts '}'`。
- **冲突 / 兼容**：破坏性。旧写法 `x => expr` 改为 `(x) => expr`；`{ a, b => body }` 改为 `(a, b) => { body }` 或调用尾随 `f { (a, b) => body }`；`{ 42 }` 0 参块改为 `() => 42`。

---

## 2026-08-25 —— 函数类型改为特殊泛型 `Function<P..., Ret>`

- **修改 §3.11**：函数类型字面量从紧凑形 `fn(T)R` / `fn?(T)R` 改为内置特殊泛型 `Function<P1, P2, ..., Ret>`。末位永远是返回类型；`Function<()>` = 0 参 unit 返回；可空走标准 `Function<...>?`。
- **删除**：`typeFn` / `typeFnWithRef` / `fnTypeParams` 产生式（`yuxParser.g4`）。函数**声明** `fn foo(...)` 不动。
- **修改 附录 B**：去掉 `fnType`；新增 B.2b `Function<P..., Ret>`。
- **修改 E2031**：文案从 `fn(...)` 改为 `Function<...>`。
- **ABI**：可空仍是 16 字节 fat-ptr（`fn_ptr == null`），不套 `Nullable` 外壳。LLVM mangle 从 `fn(i32)i32` / `fn()void` 改为 `Function<i32,i32>` / `Function<()>`。
- **冲突 / 兼容**：破坏性。旧写法 `fn(i32)i32` / `fn?()` 改为 `Function<i32, i32>` / `Function<()>?`。类型位不再写形参名。含 `T&` 形参的函数类型暂不能作 struct 字段（`genericDef` 实参不允许 `Type&`，与 `Dyn<D&>` 同缺口）。

## 2026-08-25 —— 删除 Python 风三元 `a if c else b`

- **删除 §4.9.2**：`exprIfElsePreValue`（`trueExpr if cond else falseExpr`）下线；条件表达式统一为 §4.9.1 `if c { a } else { b }`
- **修改 §4.2.1 / §5.4.1 / §5.7.1.1 / §2.5 / §11.6.4.2 / 附录 B**：从产生式表与 const-eval 白名单去掉该形态
- **修改 附录 D E3141**：允许列表不再含 `exprIfElsePreValue`
- **语法**：`yuxParser.g4` 删除 `# exprIfElsePreValue` 分支
- **冲突 / 兼容**：破坏性。旧写法 `a if c else b` 改为 `if c { a } else { b }`

## 2026-07-15 —— 复合赋值补入 `|=`（按位或赋值）和 `&=`（按位与赋值）

- **修改 §1.7.1**：复合赋值行从 `+= -= *= /= %= ^=` 扩展为含 `|=` `&=`
- **修改 §5.2.1.2**：`opAssign` 集合追加 `|=` `&=`
- **修改 附录 B**：`opAssign` 产生式追加 `| '|=' | '&='`
- **语法**：`yuxParser.g4` `opAssign` 规则加 `| SymbolOrEq | SymbolAndEq`；lexer 层新增 `SymbolOrEq: '|='` 和 `SymbolAndEq: '&='` token
- **语义**：`a |= b` 等价 `a = a | b`，`a &= b` 等价 `a = a & b`，`a` 仅求值一次（§5.2.1.3 一致）
- **冲突 / 兼容**：纯新增——此前 `|=` `&=` 因 parser 层未包含而不可用，现在合法；不破坏既有代码

## 2026-07-06 —— `Ptr` 不引入泛型参数

- **修改 §9.7.2.1**：`Ptr` 即 `void*`，**不**引入 `Ptr<T>` 泛型形态（对齐等问题，统一当 `void*` 用）。节标题从 `Ptr<T> / Ptr` 收敛为 `Ptr`。
- **关闭 Open Issue**："`Ptr<T>` 与 `Ptr` 的形态收敛"——已决议。
- **冲突 / 兼容**：无破坏性变更。`Ptr` 行为不变；`ptr_of:<T>(obj)` 签名维持。

---

## 2026-07-06 —— 测试隔离机制落地（DLL 级子进程）

- **修改 §11.3.4.3–§11.3.4.4**：从设计的 `--isolate=process` 改为实际的 DLL 级子进程隔离——`yux test` 为每个 `*.test.dll` 启动独立 `yux-test-runner` 子进程，进程内 SEH 包裹单条测试。
- **关闭 Open Issue**：`#Test` 测试隔离机制——已落地。
- **冲突 / 兼容**：纯增量。测试语义不变；`--isolate` CLI flag 不再需要（隔离由架构保证）。

---

## 2026-07-06 —— `rc:<T>(v)` 显式 Rc 构造

- **修改 §9.5.2.2**：从"v1 不提供独立构造函数"更新为 `rc:<T>(v T) Rc<T>` baked builtin——分配 RC Block + store payload + 返回 `Rc<T>` 句柄，与隐式装箱等价。
- **关闭 Open Issue**："显式 `Rc(value)` 构造形态"——已落地。
- **冲突 / 兼容**：纯增量。已有上下文驱动构造语义不变；`rc:<T>(v)` 作为等价的显式路径补充。

---

## 2026-06-30 —— `#Inline` 对全局 `#Cval` 的内联支持

- **新增 §11.14**：`#Inline` 注解——标记全局 `#Cval` 为内联常量，不产生 LLVM GlobalVariable 符号，使用处直接替换常量值（类似 C `#define`）。零参，仅可附着于全局 `#Cval let`；与 `#Mut` / `#Frozen` 互斥；单独使用报 E3117。
- **修改 §11.5.1**：v1 注解表追加 `#Inline` 行；§11.5.1.1 落地清单纳入 `#Inline`。
- **修改 §11.5.2.4**：从候选未来注解中移除 `#Inline`（`#NoInline` 保留）。
- **修改 E3112 消息**：`let` 注解白名单追加 `#Inline`。
- **新增 E3117**：`#Inline` requires `#Cval` on `let` declaration。
- **冲突 / 兼容**：纯增量。已有 `#Cval` 行为不变；`#Inline` 是新注解，不影响已有代码。

## 2026-06-30 —— pkg 文件 `name as alias` 重命名导出

- **新增 §10.2.4.2**：pkg 文件语法行追加 `name as alias`——把子模块 `name` 以别名 `alias` 再导出，调用方以 `alias.member` 访问。
- **新增 §10.2.4.4.1**：重命名导出示例（`tests/projects/pkg_rename_export`）。
- **删除 Open Issue**："`pkg` 文件是否未来支持重命名导出（如 `name as alias`）"——已落地。
- **冲突 / 兼容**：纯增量。`name` / `name.*` 行为不变；`name as alias` 仅扩展 pkg 文件解析，不影响已有 `pkg` 文件。

---


## 2026-06-29 —— 关闭 §10 Open Issue ×2：自动扁平保留 + 不引入 `pub`/`private`

- **修改 §10.2.3.1**：新增 §10.2.3.1.1——明确无 `pkg` 文件时语义等价于 `pkg` 逐行列出所有子模块（`name` 形式，不做 `name.*` 扁平）。调用方通过 `use pkg.*` 后以 `child.fn()` 访问，不能直接用裸名。
- **修改 §10.3.1.2**：从"v1 不引入"升格为永久决议——yux **不**引入 `pub` / `private` 可见性修饰符；可见性靠 `_` 前缀约定决定。
- **删除 Open Issue**："包目录无 `pkg` 文件时的'自动扁平'是否值得保留"、"`pub` / `private` 关键字是否未来引入"——均已决议。
- **冲突 / 兼容**：编译器实现早已符合自动扁平语义；`pub`/`private` 从未存在，无行为变更。

## 2026-06-28 —— `()` 作为 unit 类型和值——零元素元组（5e5f698）

- **修改 §3.2.2**：复合类型表追加 `()` unit 类型行；元组行注解从"长度 ≥ 2"更新
- **修改 §3.8.1.1**：元组产生式从单一 `typeTuple`（≥ 2 元素）扩展为含 `typeUnit`（`()`，0 元素）；`()` 是零元素元组（unit 类型），等价于 void 但可作为类型名写出
- **新增 §3.8.1.0**：unit 类型 `()` 小节——形态 `typeUnit ::= '(' ')'`、语义（零元素元组、无值、可作泛型默认实参等）、与省略 retType 的关系
- **修改 §4.2.1**：优先级表追加 `exprUnit`
- **修改 附录 B**：`type` 产生式补 `typeUnit`；`typeWithRef` 产生式补 `typeUnitWithRef`；`expr` 产生式补 `exprUnit`
- **语法**：`yuxParser.g4` `type`/`typeWithRef`/`expr` 各加 `ParStart ParEnd` 分支（`#typeUnit`/`#typeUnitWithRef`/`#exprUnit`）
- **语义**：`()` 作为 fn 返回类型时规范化等价于省略 retType（皆为 unit）；`fn() i32` 返回 `i32`，`fn() ()` 显式返回 unit
- **冲突 / 兼容**：纯新增——此前 `()` 不存在于语法中，不影响既有代码

## 2026-06-26 —— 复合赋值补入 `^=`（按位异或赋值）

- **修改 §1.7.1**：复合赋值行从 `+= -= *= /= %=` 扩展为含 `^=`；移除独立的「异或赋值」行（归于复合赋值大类）
- **修改 §5.2.1.2**：`opAssign` 集合追加 `^=`
- **修改 §A.4.2**：移除「`^=` v1 不可用」的 Open Issue；`^=` 现由 `opAssign` 包含（`SymbolXorEq` 单 token，非拼接）
- **修改 附录 B**：`opAssign` 产生式追加 `| '^='`
- **语法**：`yuxParser.g4` `opAssign` 规则加 `| SymbolXorEq`（lexer 层 token 已预置）
- **语义**：`a ^= b` 等价 `a = a ^ b`，`a` 仅求值一次（§5.2.1.3 一致）
- **冲突 / 兼容**：纯新增——此前 `^=` 因 parser 层未包含而不可用，现在合法；不破坏既有代码

## 2026-06-26 —— labeled break（`break@label`）

- **新增 §5.5.1.6**：`ID ':' loop` labeled loop 语法，支持为循环命名
- **新增 §5.5.2.1–§5.5.2.3**：`break@ID` 形态，从嵌套 loop 中跳出指定标签层
- **修改 §5.5.1.1**：`loop` 产生式扩展为 `(ID ':')? Loop loopInit? statementBlock`
- **新增诊断码 E3022**：重复 loop label 检测（`Duplicate loop label '{}': already used by an enclosing loop`）
- **新增诊断码 E3025**：`break@label` 目标不存在（`break@label '{}' target not found: no enclosing loop with that label`）
- **冲突 / 兼容**：完全向后兼容——无 label 的 `loop { }` 与 `break;` 行为不变。label 检测仅对新增语法生效

## 2026-06-26 —— loop init 子句

- **新增 §5.5.1.5**：`loop name = expr { }` 和 `loop (a, b) = expr { }` 形态，在 loop 前声明默认可变的局部变量
- **修改 §5.5.1.1**：`loop` 产生式扩展为 `Loop loopInit? statementBlock`
- **冲突 / 兼容**：完全向后兼容——`loop { }` 无 init 形态不变。init 变量默认可变，和旧写法 `#Mut let x = ...; loop { ... }` 语义等价，仅更简洁

---

## 2026-06-25 —— Array<T> / String / StringBuilder 索引与长度迁移至 usize

- **修改 §9.2.1.1**：Array<T> layout `{ _data: Ptr, _len: usize, _cap: usize }`（原 `i64`）
- **修改 §9.2.3**：`len()` / `cap()` 返回 `usize`，`get(i usize)` / `set_len(len usize)` 形参改为 `usize`
- **修改 §9.3.4**：`String.len()` 返回 `usize`，`String.get(i usize)`
- **修改 §9.4.2**：`StringBuilder.len()` 返回 `usize`
- **冲突 / 兼容**：**破坏性变更**——现有 `i64` 类型作为 Array/String 索引或长度接收者的代码需更新为 `usize`（或去掉类型后缀借助 flexible integer 推断）。编译器内部 struct layout `{ ptr, i64, i64 }` 在 64-bit 上保持不变（`usize` ≡ `i64`），但 yux 层类型系统严格区分

## 2026-06-25 —— 添加 isize / usize 指针宽度整数类型

- **新增 §9.1.1**：整数类型表追加 `isize`（有符号指针宽度整数）、`usize`（无符号指针宽度整数），LLVM 落地为 `iN`（N = 目标指针位宽）
- **修改 §9.1.1.3**：原"v1 不提供 `isize` / `usize`"条款移除，标注已在 v0.18 落地
- **冲突 / 兼容**：无破坏性变更——新增类型，不影响现有 `i8`–`i64` / `u8`–`u64` 代码

---

## 2026-06-24 —— buildAnno 参数扩展 + `#CName` + yux.toml `[link]`

- **新增 §6.6.1.3**：`#CName("symbol")` 注解为 extern fn 指定链接时 C ABI 符号名
- **新增 §10.1.1.6**：`yux.toml` 的 `[link]` 段，项目级声明系统链接库（`libs` 数组）
- **修改 §11.1.3.2**：`buildAnno` 参数从 ID-only 扩展为接受数字、无插值字符串（`"..."` / `r"..."`）、type 引用
- **修改 §11.5.1**：注解表新增 `#CName`
- **删除 §11.5.2.1**：原 extern 块注解预留（`#Link`）撤销，链接库改由 yux.toml `[link]` 段统一管理
- **修改 附录B**：`buildAnno` 语法汇总对齐
- **冲突 / 兼容**：无破坏性变更——现有 `#Name(ID)` 形态全部兼容；新增字符串/数字/type 参数形态向后兼容

---

## 2026-06-11 —— `#CompilerInner` 重命名为 `#Builtin`

- **修改 §11.2**：注解名 `#CompilerInner` → `#Builtin`，所有子节、交叉引用同步更新。
- **修改 §12.7.1.3**：原"不引入新注解 `#Builtin`"条款随本次重命名自然解除。
- **修改**：`docs/`、`docs/spec/`、`docs/dev/`、`docs/spec/draft/`、`MILESTONE.md` 全仓文档同步重命名。
- **修改**：`sdk/yux/src/yux/core/base.yux`、`assert.yux` 注解名同步更新。
- **冲突 / 兼容**：破坏性变更——所有引用 `#CompilerInner` 的 yux 源码必须改为 `#Builtin`。用户代码原则上不应使用 `#CompilerInner`（§11.2.2.3），故无用户侧迁移成本。编译器内部标识符（`isBuiltinMethod`、`validateBuiltinIntrinsicShape` 等）同步重命名。

## 2026-06-10 —— 闭包捕获 sema 解锁 + yux-check 漏报清零（c90064d）

- **修改 §4.11.6**：新增 §4.11.6.5 `Heap<T>` 非空形态闭包捕获条款；顶部加注引用 `DRAFT-closure-capture.md`。
- **修改 §8.7.6**：顶部加注引用 `DRAFT-closure-capture.md`（三档捕获模式、捕获包 layout、RC 协议均已定型）。
- **新增草案**：`DRAFT-closure-capture.md` 从 `DRAFT-lambda.md` §5–§6 提炼为独立规范，定型 move / retain / borrow 三档模式、捕获包 layout、`Heap<T>` B 档 move 捕获、静态检查规则。状态：已落地（v0.16）。
- **sema 解锁**：`SemaPass::visitExpr` 移除 lambda body skip，yux-check 覆盖 lambda 体内 E2030（捕获写禁）×2、E4022（T& 捕获逃逸）×2、E4024（Heap 非空捕获）×1，共关闭 5 例漏报。
- **借用检查**：`BorrowChecker::visitLambda` 递归进入 lambda body，lambda T& 形参注册为借用根，ret T& 溯源（E4020/E4021）在 lambda 体内生效。
- **冲突 / 兼容**：无破坏性变更。sema 接管后 Compiler 端原 throw 已删除（v0.16 收尾）。yux-check 覆盖率从 ~96% 提升至与 yux build 等价（lambda 体路径）。

## 2026-06-10 —— `[]` ⇔ `get` 语法糖语义同步（arr[i] 返回 T&）

- **修改 §4.7.1.2**：`&arr[i]` 不合法理由从"`[]` 结果是右值"更新为"`[]` 返回 `T&`，`&T&` 形成 `T&&` 被 §2.6.1 拒绝"。
- **修改 §4.7.3.1–3.2**：`e[args]` 确认为 `e.get(args)` 语法糖，两者均返回 `T&`；删除"v1 不支持取索引位置引用"的过渡措辞。
- **修改 §9.2.3.1**：重写 `arr[i]` 语义——返回 `T&`（原为 `T` 值），语义等价于 `arr.get(i)`；删除"计划后续版本迁移"过渡说明。编译器直通 GEP 方案保留（`.get()` 声明保留给 LSP / 反射）。
- **修改 §9.9.3**：`[T * N]` 索引同步为返回 `T&`，与 `Array<T>` 一致。
- **冲突 / 兼容**：破坏性变更。`arr[i]` 从返回 `T` 变为返回 `T&`，直接赋值给 `T` 类型变量、传给 `T` 形参将产生类型错误。迁移：用 `copy_of:<T>(arr[i])` 显式取值；操作符（`+ - * / == != < >` 等）自动解包 `T&`，无需改动。

## 2026-06-10 —— 静态引用（`&global_var` / T& 返回全局引用）

- **修改 §8.6（借用 T&）**：新增静态借用子条款——根为全局变量 / `#Static FIELD` / `#Cval` 时寿命自动通过（全局作用域 ⊇ 任何局部）。
- **修改 §8.6.10（返回引用的溯源约束）**：允许源集修订——从「仅 T& 形参（或方法的 `$`）」扩为「T& 形参（或 `$`）∪ 全局/静态/cval」；0 T& 形参 + 仅静态借用合法（E4021 放宽为"最多 1 个 T& 形参"）。
- **修改 §8.9（禁忌一览）**：删除"函数返回值 T&"条目——E2009 已在 v0.15 移除，本版收口。
- **新增草案**：`DRAFT-static-ref.md` 入库并标注"已落地（Phase 1–2，2026-06-10）"；实施日志 `docs/dev/static-ref-impl-log.md`。
- **codegen**：`compileGetRefExpr` 全局变量 fallback（`_localVarPtrs` → `_module->getGlobalVariable`）。
- **借用检查**：`rootFromRefInit` 识全局根 → `$rodata`（immortal 哨兵）；`_returnAllowedSources` 始终含 `$rodata`；调用站零参 T& 函数溯源到 `$rodata`。
- **冲突 / 兼容**：无破坏性变更。`&global_var` / `&cval` 从编译错误变为合法；存量代码不受影响。

## 2026-06-09 —— 编译期反射（内置 spec Reflect）落地（8edfeee）

- **新增 §13**（反射）：内置 spec `Reflect` + 编译器隐式 `#Impl(Reflect)`、反射数据类型 `Type` / `Field` / `Method` / `Variant`（`#Builtin`）、`#Static #Frozen` 字段段（`type` / `fields` / `methods` / `variants`）仅类型形访问、`Field.value` sema 期改名、`.rodata` emit 与 `--gc-sections` DCE、`#Reflect` 注解防 DCE。
- **修改 §11**（编译期注解）：新增 §11.12 `#Reflect` 注解（零参，仅附着 structDecl）+ 更新 §11.5.1 注解表。
- **修改 §12.1.1.1 / §12.8 项 14**：spec body 内 `#Static #Frozen` 字段段**允许**（Reflect spec 的 type/fields/methods/variants 即以此形态声明）；关联常量仍留 `DRAFT-data-struct.md`。
- **修改 附录 D**：新增 E3133 / E3134 / E3135 / E3136（E3136 设计消解，未触发）。
- **冲突 / 兼容**：无破坏性变更。反射数据默认 emit + `--gc-sections` 自动回收；`#Reflect` 仅用于外部工具防 DCE 场景。`Field.value` 仅支持 `$` receiver（方法体内），显式 receiver 留后续扩展。

## 2026-06-08 —— `<-` 移入赋值表达式落地（983cf7a）

- **新增 §4.13**（移入赋值表达式 `a <- b`，`exprMoveAssign`）：形态、优先级（最低，右结合）、求值顺序（LHS 先于 RHS）、左值约束、类型规则、RC 所有权协议。与语句级赋值 `a = b` 分工（`=` 不产生值，`<-` 返回旧值）。
- **修改 §4.2.1**：优先级表补 `exprMoveAssign`（等级 21，右结合）。
- **修改 §2.4**：多字符运算符表补 `<-`。
- **修改 §2.5**：表达式产生式列表补 `exprMoveAssign`。
- **修改 §1.7.1**：运算符表补 `<-`（移入赋值）。
- **修改 §8.7**：新增 §8.7.7 `<-` 移入赋值的 RC 所有权协议（与 move-return / callee-clean 并列）。
- **修改附录 B**：B.6 表达式语法摘录补 `exprMoveAssign` 分支；`opAssign` 后补 `moveAssign` 说明。
- **教程层**：`docs/基础语法.md` 补"移入赋值 `<-`"小节；`rules/yux-syntax.md` 补速查条目。
- **冲突 / 兼容**：无破坏性；`<-` 是新语法形态，不影响既有代码。

---

## 2026-06-05 —— 运算符重载形参类型放宽 (Phase A1)

- **修改 §7.2.3.2**：表格"形参"列从 `1（Self&）` 改为 `1（推荐 Self&，不强制）`。
- **修改 §7.2.3.3**：二元运算符方法形参从"**应当**为 `Self&`（其它类型不可被运算符触发）"改为"**推荐** `Self&`，不强制。编译器按精确匹配 → 自动取址匹配 → 泛型匹配三级优先级选取最优候选；同优先级多候选歧义报 E6014；无匹配报 E3073"。
- **新增 §7.2.3.3a**：非内置类型按值传参时编译器**应当**发出警告推荐 `T&`；内置标量（iN/uN/f32/f64/bool）按值传参无警告。
- **修改 §7.2.3.6**：自动取址规则从"一律 `a.method(&b)`"改为"按形参类型决定：`T&` 形参自动取址，按值 `T` 形参按值传"。

---

## 2026-06-05 —— static-vars 落地：全局 let 三档放开 + struct 静态字段（DRAFT-static-vars Phase 1–6）

- **修改 §5.1.4**（全局 `let` 声明）：从"仅 `#Cval`"放宽为三档——无注解 val（运行期 init，浅不可变）/ `#Mut`（运行期 init，可变）/ `#Cval`（编译期求值，不可变）。三者互斥；非 `#Cval` 档 v1 必须 init（`E3154`）；写入非 `#Mut` 全局 → `E3151`。RHS 接受任意 `expr`；非 const-evaluable init 进入 `_yux_global_init` ABI（按模块拓扑序在 main 前执行）；const-evaluable 走 const-eval 优先分流 emit LLVM Constant。
- **新增 §7.11**（静态字段）：struct body 内 `#Static FIELD T = expr` 静态字段声明 + `Type::FIELD` 读 / `Type::FIELD = v` 写（仅 `#Mut`）语义。与 `#Static fn` 共用 `ExprPathCallNode` parser 路径、sema 分流。向 §7.1.1 / §7.2.1.2 追加 `staticFieldDecl` 产生式；翻转 §7.1.4.7（静态成员从"不在 v1 范围"移除）。
- **修改 §11.9.2.1**（`#Mut` 可附着位置）：从仅局部 `let` 扩展为局部 `let` + 全局 `let` + struct 静态字段（与 `#Static` 组合）。
- **修改 §11.11.2.1**（`#Static` 可附着位置）：从仅 struct body 内方法 `fn` 扩展为方法 `fn` + 字段 `staticFieldDecl`。
- **修改 §11.5.1 / 附录 A §A.3**：注解表 `#Mut` / `#Static` 行同步更新附着位置与章节引用。
- **修改 附录 B**：`globalConst` 注释更新为三档分流说明；`structDecl` body 加入 `staticFieldDecl`；新增 `staticFieldDecl` 产生式；`statement` 加入 `statementStaticFieldSet`。
- **修改 附录 D §D.3.3**：新增 static-vars 段 E3150 / E3151 / E3153 / E3154 / E3155 / E3156 / E3157（E3152 / E3158 v1 占位未启用）。
- **冲突 / 兼容**：向后兼容扩展。既有全局 `#Cval let X T = expr` 形态不变（原 §5.1.4.2 条款并入 §5.1.4.1 子节）；既有 `#Mut` 局部行为不变；既有 `#Static fn` 行为不变。新引入的 `_yux_global_init` ABI 对无全局变量的项目不产出额外代码。`E3116`（全局缺 `#Cval`）由 `E3154` / `E3114` 按档位替代。
- **g4 改动**（Phase 4/5）：`structDecl` body 增 `staticFieldDecl`；`statement` 增 `statementStaticFieldSet`。经用户确认后改。
- **决议依据**：[DRAFT-static-vars.md](draft/DRAFT-static-vars.md) 决议日志 [#1.A]–[#1.H]；实施记录见 [`docs/dev/static-vars-impl-log.md`](../dev/static-vars-impl-log.md)。
- **测试**：`xmake test yux_tests/static_vars_*` 10/10 + `diag_static_vars_*` 6/6 全绿；`yux test`（SDK）全量通过。

## 2026-05-30 —— const-eval 落地：全局 `#Cval` 初始化器升常量表达式 + struct 字面量 LHS/位置放宽 + `#Const fn` 编译期求值（DRAFT-const-eval Phase 1–5）

- **修改 §5.1.4.1 / §5.1.4.3**（全局 `#Cval let`）：初始值从 `literal` 升为**常量表达式**，接受字面量、`#Cval` 引用、算术/位/比较/逻辑运算、`#Const fn` 调用、struct 字面量 `<Type> { ... }`。非 const 子表达式 → E3140；算术溢出/除零/越界 → E3143。
- **修改 §5.1.5.3 / §5.1.5.4**（局部 `#Cval let`）：初值集合同步扩展到含 `#Const fn` 调用 + struct 字面量；删除"`#Const fn` 调用不纳入"的 informative 备注。
- **修改 §7.3.2.1**（struct 字面量）：LHS 从 `Self` 放宽到任意类型名 `<TypeName> { ... }`；出现位置从 `#Static fn` 体内放宽到任意 expr 位置。
- **修改 §7.10.3.4**：`exprStructLit` LHS 从 `'Self'` 改为 `typeName`。
- **修改 §7.10.3.5**：单一位置约束（仅 `#Static fn` 体内）替换为**出现位置矩阵**（5 种位置 × 字段初值要求）。
- **修改 §11.6**（`#Const`）：新增 §11.6.4「const-eval 通路」——自由/静态成员位的 `#Const fn` 在 sema 期可被递归求值（body 形态白名单 + 形参/返回类型白名单）；成员位语义不变。错误码 E3141（控制流越白名单）/ E3144（形参/返回类型不在白名单）。
- **修改 §11.10.1.1 / §11.10.2.2**（`#Cval`）：初值约束措辞同步扩展（含 `#Const fn` 调用 + struct 字面量）；全局注解示例从 `let NAME T = literal` 改为 `let NAME T = expr`。
- **修改附录 B §B.1**：`globalConst` 产生式从 `'cval' ID type '=' literal` 更新为 `'let' ID typeWithRef? '=' expr`；脚注同步。
- **修改附录 B §B.6**：表达式段追加 `exprStructLit ::= typeName '{' LineEnd ( fieldInit | LineEnd )* '}'`。
- **修改附录 D §D.3.3**：新增 const-eval 错误码段 E3140–E3144。
- **g4 变动**（Phase 2 / Phase 5，commit c74d243 / 0b3385c）：`letGlobal` RHS `literal` → `expr`；struct 字面量 LHS `Self` → `typeName`。经用户确认后改。
- **冲突 / 兼容**：向后兼容扩展。既有全局 `#Cval let X T = literal` 形态不变；全局/局部 `#Cval let` 初值集合扩大（纯放宽）。`E3104`（局部 `#Cval` 初值非 const）与 `E3140`（全局 `#Cval` 初值非 const）语义重叠但独立编号保留；新 struct 字面量 `<Type> { ... }` 形态不影响既有 `Self { ... }`。既有 `#Const` 注解成员位语义零变化。
- **决议依据**：[DRAFT-const-eval.md](draft/DRAFT-const-eval.md) 决议日志 [#1.A]–[#1.I]；实施记录见 [`docs/dev/const-eval-impl-log.md`](../dev/const-eval-impl-log.md)。
- **测试**：`xmake test yux_tests/const_eval_*` 6/6 + `diag_const_eval_*` 3/3 全绿。

## 2026-05-23 —— spec 默认体消歧调用 `@SpecA` 后缀（DRAFT-spec-disambig-at 落地）

- **新增章节**：[§12.10.8](12-spec.md#12108-消歧调用-speca-后缀)「消歧调用 `@SpecA` 后缀」。dot-call 方法名后可选附加 `@ID` 后缀，显式指向某 spec 的默认方法体；填补 §12.10.5 E3132 消歧覆盖体内 delegate 到 spec 默认体的形态空缺。
- **修改 §12.10.5.3**：删除"`a.SpecA::m()` 形态留 v0.X+1"措辞，改为指向 §12.10.8 的 `$.m@SpecA()` 形态。
- **修改 §12.10.7 不在范围**：原"显式消歧调用语法"条目标删除线 + "已落地，见 §12.10.8"（形态从草案预估的 `Type::m()` UFCS 改为 dot-call `@` 后缀）。
- **g4 变动**：`src/yuxParser.g4` `exprDot` 在 `member+=ID` 后加可选 `(SymbolAt specQual=ID)`；`src/yuxLexer.g4` 新增 `SymbolAt: '@';` token。附录 B `exprDot` 产生式同步。改 g4 经用户口头确认，按 `behavior.md` 例外照常走。
- **诊断变更**：
  - `E1101` 触发面追加："`$.m@SpecA()` 中 T 未 `#Impl(SpecA)`" / "`Dyn<D>` 上 `@OtherSpec`"，统一进 missing-impl 语义类。
  - `E1140` 触发面追加："`$.m@SpecA()` 中 SpecA 无该方法 / 默认体"；模板升级为 3 参数 `(specName, methodName, contextSuffix)`，消息按上下文区分"unknown method" vs "no default body"。
  - 不引入新错误码。
- **永久决议**（承 DRAFT-spec-disambig-at [#1.A]–[#1.H]）：
  - `@` 仅出现在 dot-call；free fn / `Type::factory` / 构造调用**不**接受 `@` 后缀。
  - `@` 后**单名**；跨包 / 全限定形态留"统一路径形态"专项。
  - `$.m@SpecA()` 永远指向 SpecA 默认方法体（escape hatch 语义）；fall-through 等价场景与 `$.m()` 行为一致。
  - `@label` 用于 `break` / `ret` 与本节同源 `@` 形态但分草案承担，本次不绑死 label 语义。
- **codegen 协议**：实现者类型 S 对每条 (spec, 带默认体签名) 在 fall-through 之外**额外合成** `S.m__at__<spec>` 符号；调用点 codegen 期把 member 重写为 `m__at__<spec>` 走常规 dispatch；mangler 不引入新规则。
- **Dyn 整合**：sema 端 `d.m@D()` 接受（走 vtable）/ `d.m@OtherSpec()` 拒（复用 E1101）。codegen 在 `baseType.isDyn()` 时跳过 member 重写。Dyn 上调用 spec 默认体 fall-through 方法本身的 LLVM assert（与 `@` 形态无关，见 `BUGS.md`）阻塞了端到端测试，sema 形态校验已完整。
- **测试**：`diag_spec_disambig_at_{not_impl,unknown_method,no_default_body}` / `spec_disambig_at_{basic,override_delegate,e3132_delegate}` 全过；Dyn 端到端推迟（实施日志 + BUG 记录）。
- **冲突 / 兼容**：仅向后兼容扩展。既有 dot-call（`a.b`）行为不变；既有 `m` 标识符不受影响（`@` token 仅在 ID 后位置作消歧后缀槽位生效）。

---

## 2026-05-22 —— spec 默认方法体 + fall-through（DRAFT-spec-default-body 落地）

- **新增章节**：[§12.10](12-spec.md#1210-默认方法体默认实现--fall-through)「默认方法体（默认实现 + fall-through）」。允许 `#Spec struct D { ... }` 体内方法签名附带函数体（`fnExprBody` 或 `fnBlockBody`）作"默认方法体"；`#Impl(D) struct S { ... }` 未覆盖该方法时默认体 fall-through 到 S。
- **修改 §12.1.1.1**：spec body 内允许"签名 + 可选默认体"形态；删除"带函数体的方法"禁项与 E1139 引用，保留 §12.10 交叉引用。
- **修改 §12.4.1.3**：`#Spec` 体内允许方法带体；条款语义从"拒收"翻转为"承载默认体"，删除 E1139 引用。
- **修改 §12.8 不在范围 item 2**：原"spec 默认方法体（占位）"删除线 + 标"已落地，见 §12.10"，与 §12.9 Dyn item 1 同款。
- **修改 Open Issues**：删除"spec 默认方法体 + fall-through 路径"条目（已闭环）。
- **诊断变更**：
  - `E1139` 退役（附录 D 标注，编号不复用）。spec body 方法带 body 不再是错误。
  - `E1140` 新增（§12.10.3.2）：spec 默认体引用本 spec 不存在的方法名（sema 占位校验阶段）。
  - `E3132` 新增（§12.10.5）：多 spec 默认体组合冲突未消歧。
  - `E1101` 复用为"实现者未实现 + 默认体不可用"的统一诊断（与 §12.2.2.1 既有 missing-impl 语义并轨；未引入草案曾估的 E1136 新码）。
- **永久决议**（承 DRAFT-spec-unify [#1.AA] / [#1.AE]）：
  - **不引入** `#Derive(Spec)` 独立注解 —— `#Impl(D) + 不写体` 即 fall-through。
  - **不引入** 按字段递归自动 derive 默认体（`ToJson.to_json` / `Eq.eq` 字段遍历形态永不引入）。
- **SDK 形态**（§12.10.6）：`base.yux` 5 件套补默认体可落地部分 —— `Ord.{lt,le,gt,ge}` 由 `cmp` 推、`Eq.ne` 由 `eq` 推；其余维持纯抽象签名。
- **g4**：未动。spec-unify v1 阶段已铺 `fnDecl` body 可选形态，本次仅放开 ast_builder 拒收。
- **测试**：`spec_default_body_parse_*` / `spec_default_body_sema_*` / `spec_default_fallthrough_{basic,block,ord_sdk,eq_sdk}` / `spec_combine_{conflict_E3132_basic,default_plus_abstract}` 端到端 + 占位校验全绿；全量回归 SDK / `xmake test` 通过（详 `docs/dev/spec-default-body-impl-log.md`）。
- **冲突 / 兼容**：仅向后兼容扩展。既有 spec（仅签名）行为不变；既有 `#Impl(D)` 实现若已写全部方法，新默认体被覆盖不触发回归。早期"spec body 方法带 body → E1139"形态升级为合法默认体，不破坏既有用户代码。
- **未尽事项**：显式消歧调用语法 `a.SpecA::m(args)` 留 v0.X+1（§12.10.5.3 / §12.10.7）。

---

## 2026-05-19 —— spec-unify v1：替代 `draft` 关键字

- **章节重命名**：`12-draft.md` → `12-spec.md`（同步 [`docs/spec/index.md`](index.md)）。
- **新增 / 修改**：
  - **§12 整章重写**：以 `#Spec struct D { fnSig* }` 替代旧 `draft D { ... }`；以 `#Impl(D)` 顶行注解替代旧 `Type : D { ... }` 外置实现块。struct 声明与方法 / 析构 / 构造合一到单一 `structDecl` body（字段段 → `fnClean?` → `fn` 段）。
  - **§7.1 / §7.2 / §7.8**：`structDecl` 形态合一；删除独立 `structImpl` 节；§7.8 改写为"`#Impl(D)` 顶行注解 + 共享方法 namespace"。`structImpl` 字样在 §7.10 等节迁为"struct body"。
  - **§11.4**：删除 `#DraftLike`（已废弃，v1 视为 noop）；新增 `#Spec` / `#Impl(D)` 章节及互锁规则。`#Impl(D)` 用单参数糖 `(ID genericDef?)`。
  - **§11.5.1 表格 / §A.3 表格**：`#DraftLike` → `#Spec` / `#Impl(D)`。
  - **附录 A**：删除 `draft` 关键字行（A.1）；A.6 预留区记 `draft` 已废弃。
  - **附录 B**：删除 `draftDecl` 产生式；`structDecl` 形态更新为合一形态；§B.5a 改为 spec 形态说明（共用 `structDecl`）。
- **诊断变更**：
  - `E1102`（实现块多余方法）**废弃**：声明合一后，未命中任何 `#Impl(D)` 的方法视为该 struct 普通方法，照常存在并参与方法分发。
  - `E1137` = 实现者漏 spec 方法（替代旧 E1101 在新形态下的语义点）。
  - `E1138` = 实例形访问 `#Static` / 关联成员（误用拦截）。
  - `E1139` = `#Spec` body 内方法带 body。
  - `E2011` = `#Spec` body 内出现字段 / 析构（spec body 非签名形态）。
  - `E1103` = 同一 struct 上重复宣告同一 spec（同名 `#Impl(D)`）。
  - `E1110` = `#Spec` / `#Impl` 标在非 struct 声明位 / `#Spec` 与 `#Impl` 共存。
- **删除诊断**（旧形态独有）：E1102（已废弃，见上）/ E1105（显隐冲突）/ E1110 旧含义 / E1112（`#DraftLike` + 方法本地泛型）已不适用；其它 diag 行号刷新。
- **删除 spec**：内置 `Any` 删除（[§12.7.2](12-spec.md#1272-any已删除)）。universal bound 留待 [`draft/DRAFT-spec-default-body.md`](draft/DRAFT-spec-default-body.md) / [`draft/DRAFT-spec-reflect.md`](draft/DRAFT-spec-reflect.md) §8a（类型擦除 + Any downcast）承接。
- **g4 改动反映**：`structDecl` body = `(filedDecl|LineEnd)* fnClean? (fn LineEnd | LineEnd)*`；删 `Draft` token / `draftDecl` / `draftType` / `structImpl` 产生式；`buildAnno` 单参 arg 升级 `literal | ID genericDef`。详见 commit 726931b / 512624e。
- **冲突 / 兼容**：
  - 旧 `draft D { ... }` 声明、`Type : D { ... }` 外置实现块、`#DraftLike` 注解形态**不再被解析**；用户代码需迁移到 `#Spec struct` + 顶行 `#Impl(D)` 形态。SDK / 测试已一次性迁移（commit 512624e）。
  - `Any` 形态的代码（如 `accept_any<T : Any>`）须替换为具体边界或留待 reflect / default-body 草案承接。
- **测试**：`yux test` SDK 532/532、`xmake test` 179/179 全绿（commit 512624e）。

### 配套：构造函数形态删除（与 DRAFT-static-fn 落地协同）

- **§7.3 整章改写**：删除"构造函数（与结构体同名的 `fn StructName(...)`）"形态。构造唯一通道：`#Static fn` 工厂 + `Self { ... }` 字段字面量（§7.3.2 / §7.10.3）。DAA 退化为"`Self { ... }` 全字段覆盖" 规则（§7.3.2.2 / §7.3.3）。
- **§7.2.1.2**：明确 struct body 顺序：字段段 → `fnClean?`（析构）→ 实例方法 / `#Static fn` 段。析构函数若声明，**位于其它方法之前**。
- **§7.4.1.1 / §7.4.1.3 / §7.4.6.1**：析构 / 字段级语义文本去构造函数 DAA 依赖，改引 `Self { ... }` 全字段覆盖。
- **§7.5.1**：变量绑定可变性用语从 `var` / `val` 迁到 `let` 默认 / `#Mut let`（与 let-unify 同期）。
- **§7.10.1.4 / §7.10.4.2 / §7.10.6**：原"`#Static fn` 与 ctor 并存"试点说明删除；记 ctor 通道已删除，SDK 形态全部走 `#Static fn`。
- **§4.3.2 / §4.1.3.1 / §4.10.3**：表达式章关于 `$` / 副作用点 / 用户结构体构造的描述同步——`$` 在 `#Static fn` 体内禁用；副作用点改 `Self { ... }` 字段写入；用户结构体构造唯一通道改为 `Type::factory(...)`。
- **§8.6.6.3**：构造期 DAA 文本改为 `#Static fn` + `Self { ... }` 一次性 init。
- **§8.6.7.3**：方法分发归一示例从 `U : DraftX { ... }` 迁到 `#Impl(D) struct U { ... }`。
- **§11.4.1 / §11.11.2.2**：`#Spec` body 禁项列表去构造函数项；`#Static fn` 与结构体同名禁忌降级为"不推荐"。
- **附录 B / 附录 C**：`structDecl` 形态说明 / 术语表"构造函数" → "静态工厂"。

---

## 2026-05-18 —— v0.13.0 收尾：草案归档 + 实施日志 + LSP completion 补全

- **草案归档**：
  - `docs/spec/draft/DRAFT-heap-types.md` 头部状态升至"已落地（Phase 1-8）"，链接到 spec 正文 + 实施日志；保留作历史档不再变更。
  - `docs/spec/draft/DRAFT-static-fn.md` 头部状态从"已落地（P1 + Phase 6A-6D）"升至"已落地（P1 + Phase 6A-6E）"，补 codegen 接通泛型 struct + `#Static fn` + `Self {}` 的 6E.4 B-E。
- **实施日志新增**：
  - `docs/dev/heap-types-impl-log.md`：Phase 2-8 实施记录（Phase 1 即 Box→Rc 改名独立归档在 `rc-rename-impl-log.md`）。核心决策（Heap 物理形态 = 裸 `T*`、单 owner 不参与 retain、B 档 nullable move、A 档 NRVO、scope-end 释放序）+ 各 Phase 改动面 + 跨 Phase TODO。
  - `docs/dev/static-fn-impl-log.md`：Phase 0-6E 实施记录。核心决策（`#Static` 注解识别、`Type::name(...)` 调用、`ExprPathCallNode` 复用、`Self` 升关键字、`Self { .field = value }` 仅 `#Static fn` 体内、砍同名 ctor 不留过渡期）+ 各 Phase 改动面 + 未落事项。
- **LSP completion 补全**（`src/lsp/completion.cpp`）：
  - 类型补全加入 `Heap` / `Weak` / `Arc`（占名）/ `Dyn` / `Self`；`Rc` 描述细化。
  - 内置函数补全加入 `copy_of` / `as_ref` / `ptr_of` / `same_ref` / `weak` / `upgrade` / `heap_some` / `heap_null` / `panic`。
- **清理**：删除已完工的 `CURRENT-remove-ctor.md` 与 `CURRENT.md`（Heap 主线条目）；遗留 `CURRENT-check.md` / `CURRENT-forin.md` 是独立长期任务，保留。
- **MILESTONE**：v0.13.0 标"已完成（2026-05-18）"，退出标准 ✅。

---

## 2026-05-18 —— Heap Phase 8：FFI Heap ↔ Ptr 互转

- **行为面新增**（DRAFT-heap-types Phase 8 / FFI handoff）：
  - `ptr_of:<Heap<T>>(h Heap<T>) Ptr`：把 Heap 单所有权交给裸 Ptr——返回 `h` 的裸 `T*`，同时把 source slot 写 null 并从 `_scopeVars` 摘除，避免作用域尾 `__yux_heap_free` 与 FFI 端 free 双释放。sema 只接受 ID-literal 实参 (E6028)；同时把 `T.isHeap()` 加入 `ptr_of` 合法 `T` 集合（`same_ref` 不接受 Heap，单 owner 比较无语义）。
  - `Heap:<T>(p Ptr) Heap<T>`：从裸 Ptr 接管所有权——跳 `__yux_heap_alloc` + store，直接把 `p` 作为 Heap 句柄；作用域尾走既有 `Heap<T>` dtor（`releaseAtPtr(inner T)` + `__yux_heap_free`）。sema/codegen 在 `ExprHeapCtorNode` 上识别 `argType.isPtr() && innerType.name != "Ptr"` 形态时放宽 E3028。责任方：调用者必须保证 `Ptr` 指向 `__yux_heap_alloc` 分配的 T-shape 内存。
  - `src/sema/call_resolve.cpp::validateBuiltinIntrinsicTypeShape`：`ptr_of` 接受 Heap；强制 Heap 实参为 ID-literal。
  - `src/compiler/compiler_call.cpp::extractRawPtr` + `ptr_of` 分派点：加 Heap 分支 + move-out。
  - `src/compiler/compiler_expr.cpp::compileHeapCtorExpr`：加 take-over 分支。
  - `src/sema/sema_pass.cpp::visitExpr(ExprHeapCtorNode)`：相同的 take-over 形态放宽 E3028。
- **不在范围**：
  - **8c FFI 上下文门控**：仅 `extern` / `#FFI` 上下文允许互转的诊断暂未落地，留 TODO，与后续 FFI 完善合并做。当前任何上下文都允许，行为正确但缺误用兜底。
- **测试**：`sdk/yux/src/yux/core/heap.test.yux` 新增 3 项 — `test_heap_8_ffi_ptr_roundtrip_scalar` / `_roundtrip_struct` / `_no_double_free`；SDK 531 → 534 通过。
- **回归**：`xmake test` 184/184、`yux test`（SDK）534/534 全绿。

---

## 2026-05-17 —— Heap Phase 3f / 6：copy_of 扩展 Heap

- **行为面新增**（DRAFT-heap-types Phase 6 / `copy_of` baked 扩展）：
  - `copy_of:<Heap<T>>(x Heap<T>&) Heap<T>`：深拷——`__yux_heap_alloc(sizeof(T))` 分配新句柄 + 把源 inner `T` 写入 + 递归 retain inner 的 Rc / Array / Weak / String 字段。两个 Heap 各持独立 owner，作用域尾各自 free，不会 double-free。
  - `copy_of:<Heap<T>?>(x Heap<T>?&) Heap<T>?`：源 `_has=false` → `{false, null}`；`_has=true` → 走 Heap 分支同款；以 PHI 选 ptr。
  - **修正既有 bug**：之前对 `Heap<T>` 走通用 `retainHandleAtCallSite` 路径——Heap 没有 retain 分支，等于 no-op + bitwise return，导致 `let b = copy_of(a)` 让 `a` / `b` 共享同一 ptr → 作用域尾 double-free。现在专门走深拷分支。
  - `src/compiler/compiler_call.cpp::copy_of`：在通用分派前插入 `T.isHeap()` 与 `T.isNullable() && nullableInner.isHeap()` 两条分支；后者用 cond-br + alloc BB + cont BB + PHI 合并。
  - `src/sema/call_resolve.cpp::copy_of` `hasRefDeep`：把 `t.isHeap()` 加入"不展开"列表（与 `Rc/Weak/Array/Nullable/Dyn/Ptr/Fn` 同级），把 Heap 当不透明堆句柄。
- **不在范围**：
  - **`copy_of` 深拷贝补全**（Phase 6 第三条）：Heap/Dyn 字段深拷已落地，Array 逐元素深拷待实施。
  - **显式 by-value 拒绝**：草案曾提"`copy_of(rc)` / `copy_of(heap)` 按值禁；引导 `Rc:<T>(copy_of(as_ref(rc)))`"——但 by-value 形态本就走 auto-borrow→T& 等价路径，新深拷分支下也得到正确独立 Heap，无再 broken；不再加显式禁止。
- **测试**：`sdk/yux/src/yux/core/heap.test.yux` 新增 5 项 — `test_heap_3f_copy_of_owned_scalar` / `_owned_struct` / `_nullable_some` / `_nullable_null` / `_no_double_free`；SDK 526 → 531 通过。
- **回归**：`xmake test` 184/184、`yux test`（SDK）531/531 全绿。

## 2026-05-17 —— Heap Phase 3e：Lambda 捕获 Heap

- **行为面新增**（DRAFT-heap-types §5.5 / Phase 5 计划提前）：
  - 接受 `Heap<T>?` 按所有权 move 捕获：lambda 创建点把外层 slot 写 `{_has=false, _value=null}`，env 独占所有权；lambda dtor 既有 `typeNeedsDestructor` / `releaseAtPtr` 路径自动 free。
  - 非空 `Heap<T>` 按值捕获 → 报 **E4024**（与 §5.2 / §5.3 一致：要求声明为 `Heap<T>?` 用 movable slot）。
  - `Heap<T>&` / `Heap<T>?&` 借用捕获走既有 `T&` ref 分支，零代码改动；逃逸由既有 **E4022** 兜底。
  - `src/compiler/compiler_expr.cpp`（capture 识别）：扩接受集为 `isScalar || isHandle || isRef || isHeapNullable`；非空 Heap 显式 throw E4024；槽位字节数从硬编码 8 改为 `DataLayout::getTypeAllocSize` 计算（向下兜底 8），防 `Heap<T>?` = `{i1, ptr}` 实际 16 字节与下一个 capture 槽重叠。
  - `src/compiler/compiler_lambda.cpp::compileLambdaExpr`：写入 env capture 后，若 `cap.type` 是 `Heap<T>?`，跳 retain（`retainHandleAtCallSite` 本就无 Heap 分支）并对 outer slot 写 `{_has=false, _value=null}`（与 3d.2 / 3d.3 同款 GEP 写回）。
- **不在范围**（保留供后续）：
  - 跨 lambda 字段 move-out（仍依 §5.3 跨函数全 struct move-in ABI）。
  - 索引 lvalue / lambda 内 if-else flow 字段窄化。
- **测试**：`sdk/yux/src/yux/core/heap.test.yux` 新增 3 项 `test_heap_3e_lambda_move_capture_{nullable,null_source,no_double_free}`；`tests/cases/diag_heap_lambda_capture_non_null.yux` 锁 E4024。SDK 523 → 526 通过，xmake 183 → 184 通过。
- **回归**：`xmake test` 184/184、`yux test`（SDK）526/526 全绿（pre-existing flaky `test_string_plus_left_non_string` 单独运行 PASS，与本改无关）。

## 2026-05-17 —— Heap Phase 3d.3：B 档 nullable move 字段形态扩展

- **行为面新增**（DRAFT-heap-types §5.2 / §5.3，同函数同 scope 内）：
  - struct-literal 字段 `Heap<T>?` + RHS 是 lvalue（局部 ID 或局部 struct 的字段 `b.field`）时走 move-out：跳 retain、写入后把源槽写回 `{_has=false, _value=null}`。
  - 调用点 B 档 elligible 实参形态从局部 ID 扩展到局部 struct 字段 `b.field`：调用后源字段被写回 null（与 3d.2 同款写回逻辑）。
  - `src/compiler/compiler.h` + `src/compiler/compiler_destructor.cpp::tryHeapNullableLvalueSlot`：抽出公共 helper，识别 `Heap<T>?` 的两种 lvalue 形态（局部 ID / 局部 struct 字段），返回 slot ptr + slot llvm 类型。
  - `src/compiler/compiler_call.cpp::compileKnownFunctionCall`：`recordBdangIfEligible` 改走 helper，自动覆盖字段实参形态。
  - `src/compiler/compiler_expr.cpp` ExprStructLitNode 分支：字段类型 `Heap<T>?` 时调 helper；命中 lvalue 源 → 跳过 `retainHandleAtCallSite`、写完字段后把源槽写 null。
  - `src/compiler/compiler_destructor.cpp::callFieldDestructor`：补 `Nullable<Heap<T>>` 字段分支（走 releaseAtPtr 内联），修了 pre-existing pop：之前含 `Heap<T>?` 字段的 user struct 自动 dtor 会 fallthrough 误查 bare `Nullable_~()` → JIT lookup-failed。
- **不在范围**（拆后续 / 单独 phase）：
  - §5.3 跨函数形态 `fn build(b Builder) Built { ret Built { buf: b.buf } }`：byval 形参在 callee 端写 null 不会回写到 caller 槽，需要全 struct move-in 语义（更大改 ABI），留待专项。
  - 索引 lvalue `arr[i]`、if-else flow merge 字段窄化、lambda 捕获 `Heap`（Phase 3e）仍保留。
- **测试**：`sdk/yux/src/yux/core/heap.test.yux` 新增 5 项 — `test_heap_3d3_struct_lit_field_moveout` / `test_heap_3d3_struct_lit_local_id_moveout` / `test_heap_3d3_struct_with_heap_nullable_field_no_leak` / `test_heap_3d3_call_site_field_lvalue_writeback` / `test_heap_3d3_call_site_field_no_double_free`；SDK 518 → 523 通过。
- **回归**：`xmake test` 183/183、`yux test`（SDK）523/523 全绿。

## 2026-05-17 —— Heap Phase 3d.2：B 档 nullable move 调用点写回（最小集）

- **行为面新增**（DRAFT-heap-types §5.2）：形参 `Heap<T>?` 按值 + 实参是 `Heap<T>?` 局部 lvalue ID 的调用点，调用后 caller 槽被自动写回 `{_has=false, _value=null}`。callee 接管所有权（包括 callee 作用域尾的 `__yux_heap_free`），caller 端 `a.has()` 立即变 false，作用域尾析构看到 null 跳过 free，互不冲突。
  - `src/compiler/compiler_call.cpp::compileKnownFunctionCall`：args 循环里识别 B 档 elligible（formal `Nullable<Heap<T>>` byval + arg 是 `_localVarPtrs` 命中的 ID）→ 记录 caller slot；`CreateCall` 后逐 slot 写 null。
  - `src/compiler/compiler_destructor.cpp::releaseAtPtr` / `typeNeedsDestructor`：补 `Nullable<Heap<T>>` 分支——之前 `Heap<T>?` 槽根本没析构，是 pre-existing 泄露；现在按 `_value != null` 走 `__yux_heap_free`（heap_free 自身 null-safe；内层 T 需析构时按 null-check 守门）。
- **不在范围**（拆 3d.3 / 后续）：字段 move-out `Built { buf: b.buf }`（§5.3）；if/else flow merge；lambda 捕获 Heap（Phase 3e）；call site 实参是字段 / 索引等非 ID lvalue。
- **测试**：`sdk/yux/src/yux/core/heap.test.yux` 新增 `test_heap_bdang_call_site_writeback` / `test_heap_bdang_no_double_free` / `test_heap_bdang_null_arg_passthrough`；SDK 515 → 518 通过。
- **回归**：`xmake test` 183/183、`yux test`（SDK）518/518 全绿。

## 2026-05-17 —— 构造模型重构 Phase 6E.4 B-E：泛型 struct + `#Static fn` + `Self {}` codegen

- **行为面新增**：用户可写 `GenericType:<X>::make(...)` 形式调用泛型 struct 的 `#Static fn`，搭配 `Self { ... }` 字段字面量；之前 codegen 完全不消费 turbofish lhsTypeArgs，调用形态实际不可用，本次接通。
  - `src/compiler/compiler_expr.cpp`：path-call struct 分支若 `lhsTypeArgs` 非空 → `ensureStructInstance(baseDecl, args, baseOwner, line)` 触发实例化 + 压一帧 `SubstFrame`；paramTypes / retType 走 `applySubst`；`getMethodFunction(effLhs, ...)` 切到 mangled instance 名。异常路径用 try/catch 兜底 pop 帧。
  - 同文件 `compileLiteralExpr` ExprStructLitNode 分支：`_currentStructName` 是 mangled 时（泛型实例方法体内），通过 `_structInstances` 查 baseDecl；字段类型走 `applySubst`，让 isArrayGeneric / typeNeedsDestructor 识别具体类型。
  - Self 返回类型由 applySubst 自动处理（TypeSelfNode 返回 bare struct 名 → SubstFrame 替换成 mangled 实例名）；无需写 setResolvedType。
- **测试恢复**：`sdk/yux/src/yux/core/struct.test.yux::test_struct_byval_generic_instance`（StGenericHolder<Rc<StPair>> + 按值传参）；SDK 由 514 → 515 通过。
- **回归**：`xmake test` 183/183、`yux test`（SDK）515/515 全绿。BUGS #3 关闭，构造模型重构主线全部收尾。

## 2026-05-17 —— 构造模型重构 Phase 6D 收尾：C++ 端 ctor 残余清理

- **行为面无变化**：sema E3130 已于 Phase 6A 落地，定义形态 `fn TypeName(...)` 已被拦截；SDK / tests / examples / docs 已于 6B/6C 全量迁完。本次仅清理 C++ 端遗留：
  - `src/compiler/compiler.cpp`：泛型 impl 实例化路径补 E3130 兜底（sema 跳过 generic impl，泛型同名 ctor 之前未被拦截），删除 `effMethodName` fallback。
  - `src/sema/sema_pass.cpp`：去掉多余的"方法 isGeneric 跳过"（line 162 已跳过整个 generic impl）。
  - `src/lsp/semantic_tokens.cpp`：取消"struct 名作 callee 着色为 Method"特例（Phase 6D 后非法），统一着色 Function；移除 `CollectState::structNames` 字段。
  - `src/analyzer/const_mut_checker.cpp`：移除 `FnContext::isConstructor` 字段及"构造函数内放行 `#Val`/`#Frozen` 字段写入"分支——构造期写入语义已迁至 `Self { .f = v }` 字段字面量（表达式分支，不走 `StatementAssignNode`）。
- **DRAFT 收尾**：`docs/spec/draft/DRAFT-static-fn.md` 头部状态升级为 "已落地（P1 + Phase 6A–6D）"。
- **遗留**：泛型 struct + `#Static fn make` + `Self {}` codegen 路径未通（见 BUGS #3 / Phase 6E.4 B-E），非阻塞主线。
- **回归**：`xmake test` 183/183、`yux test`（SDK）514/514 全绿。

## 2026-05-17 —— 构造模型重构 P1：`#Static fn` + `Self` + `Self { ... }` 字段字面量

- **新增 §7.10**：静态函数（关联函数）章节。声明形态 `#Static\nfn name(...) RT { ... }` 挂在 `structImpl` 内，无 `$` 接收者；调用语法 `Type::name(...)`，与 enum 构造共用 parser 节点（`ExprPathCallNode`），sema 按 LHS 类型分流。同结构体内 `#Static fn` 与同名实例 fn 共存允许，与同名 ctor 禁止。`Self` 升格为保留字（lexer token `SelfType`），在 `structImpl` 体内绑定为所属结构体类型；可出现在类型位 / 调用 LHS / 字段字面量 LHS。`Self { .field = expr ... }` 字段字面量仅在 `#Static fn` 体内合法，字段必须列全（DAA 退化），句柄字段 RHS 沿用 §7.4.4 的 retain-then-release / 转移 +1 语义。
- **新增 §11.11**：`#Static` 注解条款。附着于 `structImpl` 内方法，禁 `$` / `$.field` / `&$`；与 `#Const` 组合允许，与 `#Frozen` 不组合。
- **修订 §7.3.2**：原"v1 不引入结构体字面量语法"改为"v1 提供受限的 `Self { ... }` 字段字面量，仅 `#Static fn` 体内合法"；普通表达式位 `Foo { ... }` 仍由语法层拒收。
- **附录 A**：`Self` 从 A.2 上下文标识符提升到 A.1 关键字（lexer token `SelfType`）。
- **附录 D**：新增错误码段 E3120–E3128（构造模型重构相关）；补登记 E3112–E3116（let-unify 引入但此前未上附录 D）。
- **SDK 试点**：`sdk/yux/src/yux/core/base.yux` 给 `String` 加 `#Static` 工厂 `String::empty()` / `String::from(buf)`，与同名 ctor 并存。`Array::with_capacity(n)` 因 Array 是 `#Builtin` 空结构体、`Self { ... }` 字段字面量不适用，待 `#Builtin #Static fn` codegen 路径落地后再补。
- **冲突 / 兼容**：纯增。已有项目代码无 `#Static` 注解、不写 `Self` / `Self { ... }` / `Type::name(...)` 形态时行为不变；同名 ctor 通道未删。`yux-check`（SemaPass）镜像已接入 E3120 / E3121 / E3122 / E3123 / E3124 / E3128 等；E3125–E3127 仍在 codegen 端兜底。详见 `docs/dev/static-fn-impl-log.md`（实施期归档）。

## 2026-05-16 —— Heap<T> 类型族（DRAFT-heap-types.md Phase 0 回写）

- **新增 §3.1**：类型档位表追加 "堆作用域句柄" 一档（`Heap<T>`），与堆句柄并列；注脚说明 `Rc<Heap<T>>` / `Array<Heap<T>>` 互斥规则
- **新增 §3.3**：内置泛型表追加 `Heap<T>` 行；新增 Heap / Rc widen 禁条款
- **新增 §7.1.3 字段表 + §7.1.3.1 / §7.4.2 / §7.4.6 / §7.4.6.6**：含 `Heap<T>` / `Heap<T>?` 字段的非平凡结构体构造 / 析构 / 字段写规则；复合可空 `S?` 整体 move
- **新增 §8.3a（新节）**：`Heap<T>` 形态、A 档 NRVO / B 档 nullable move / C 档局部 move、容器规则 + FFI
- **修改 §8.3.5.5 / §8.6.5.9**：`as_ref` 增加 `Heap<T>` 重载；寿命检查把 `Heap` 句柄登记为根
- **新增 §9.5a（新节）**：Heap<T> 内置类型节；§9.5.6 改 `Arc` 占名；§9.2.3.4 `Array<Heap<T>>` 例外；§9.7.2.4 `ptr_of` 表加 Heap 行
- **修改 §11.2.3.2**：`#Builtin` 重载清单注明 `as_ref` / `ptr_of` / `copy_of` / `Heap:<T>(Ptr)` 含 Heap 形态
- **附录 D**：分配 E4023–E4028（Heap escape / move ban / 容器禁 / NRVO 不消解 / widen 禁 / FFI 禁）
- **附录 C**：新增术语 堆作用域句柄 / Heap<T> / NRVO（A 档）/ nullable move（B 档）/ 复合 move（C 档）/ 生命传染
- **冲突 / 兼容**：之前 §9.5.6 中 `Heap` 保留名报错 `E1132` 一并撤销（保留名仅留 `Arc`）；DRAFT-heap-types.md 与本次回写对齐后保留作 Phase 2+ 实施期参考

## 2026-05-15 —— let-unify P1：`var` / `val` / `cval` 统一到 `let` + 注解修饰

- **新增 §5.1.1（重写）**：局部变量声明产生式从 `statementDeclare` / `statementDeclareAssign` / `statementCvalDeclAssign` 三件套合并到单一 `statementLet`，元组解构改 `statementLetTuple`；全局常量 `globalConst` → `letGlobal`。默认 `let x` 浅不可变（重赋报 `E3093`）；`#Mut` 放宽可变；`#Cval` / `#Frozen` 提供编译期常量 / 深不可变档位。三注解互斥，互斥叠加报 `E3115`；未知 let 注解报 `E3112`；缺 type+init 报 `E3113`；有 type 缺 init 报 `E3114`（`#Mut let x T` 例外）；全局缺 `#Cval` 报 `E3116`。
- **新增 §5.1.2 / §5.1.3 / §5.1.4 / §5.1.5（重写）**：默认 / `#Mut` 差异、`T&` 局部、`#Cval` 全局 / 局部条款重新表达；语义零变化，仅形态改造。
- **新增 §11.9 / §11.10**：`#Mut`（局部 `let` 可变档位修饰，inline）/ `#Cval`（`let` 编译期常量档位，局部 inline / 全局顶行）两章。`#Frozen` 表自此可挂在局部 `statementLet` 上（§11.7 表更新）。
- **附录 A**：删 `cval` / `val` / `var` 关键字行，加 `let` 关键字行；注解表加 `#Const` / `#Frozen` / `#Val` / `#Mut` / `#Cval`（前三者补登记，后两者本次新增）；A.6 预留段加 `var/val/cval` 旧关键字归档说明。
- **附录 B**：（待 §11.10 落地后同步）产生式 `statementLet` / `statementLetTuple` / `letGlobal` / `letAnno` 替代旧四件套，`Cval` / `DeclKey` 词法 token 移除。
- **冲突 / 兼容**：**破坏性**。SDK + tests + 项目代码已用 `scripts/migrate_let.py` 一次性迁移：局部 `var x ...` → `#Mut let x ...`，`val x ...` → `let x ...`，`cval x ...` → `#Cval let x ...`；全局 `cval NAME T = literal` → `#Cval\nlet NAME T = literal`；元组解构同步。字段段保留现状（`var/val/cval` 字段关键字与 `#Val` / `#Frozen` 字段注解全部不动）。AST 内 `enum class DeclareType` 同步删除，`Statement(Declare|DeclareAssign|DeclareAssignTuple)Node` 改用 `bool isMut + bool isConst` 字段。 实施详见 `docs/dev/let-unify-impl-log.md`。

## 2026-05-15 —— `Box<T>` → `Rc<T>` 改名（语义不变）+ `Heap` / `Arc` 占名

- **重命名 §9.5**：`Box<T>` 章节全量改为 `Rc<T>`。语义零变化：layout、ABI、retain / release、自动解引用、构造形态、Weak 升级等条款一字不动，仅类型名 `Box` → `Rc`。运行时 Block layout 不变。
- **新增 §9.5.6**：保留名条款。`Heap` / `Arc` 在 v0.X 期间作为内置类型名保留，未来引入；当前作类型名使用报 `E1132`。
- **附录 D**：错误码诊断信息中 `Box<T>` → `Rc<T>` 同步（E1132 / E1133 / E2001 / E2029 / E2032 / E3014 / E3016 / E3050 / E3056 / E4022 / E6029）。
- **冲突 / 兼容**：用户代码中所有 `Box<T>` / `Box(...)` 一次性脚本替换为 `Rc<T>` / `Rc(...)`；无并存期。`docs/dev/*-impl-log.md` 历史日志在文件头加脚注保留旧名 `Box`，正文不动。改名动机见草案 `DRAFT-heap-types.md`：`Box` 名字与 Rust 的"唯一所有 owning box"语义冲突，yux 实际是 RC，统一改 `Rc` 以释放 `Box` 名（未来若做唯一所有堆句柄再考虑是否复用）。

## 2026-05-15 —— const-mut P1：`cval` 局部 / `#Const` / `#Frozen` / `#Val`

- **新增 §5.1.5**：局部 `cval` 声明（产生式 `statementCvalDeclAssign`）。初值约束限定为字面量 / 已声明 `cval` 引用 / 一元-二元-位-比较-逻辑组合；违反报 `E3104`。顶层 `globalConst` 仍保留 `literal` RHS（保守）。
- **新增 §6.1.2a / §11.6**：`#Const` 函数注解，禁写 `$.f` / 参数字段 / 非 `cval` 全局，禁调非 `#Const` 函数；违反报 `E3110` / `E3111`。
- **新增 §6.2.2a / §11.7**：`#Frozen` 形参注解（深不可变）。配合参数默认 `val` 形成"重赋禁 / 字段写禁 / 不可传可写受方"三段约束，违反报 `E3093` / `E3106` / `E3107`；未知参数注解报 `E3105`。脱 const 出口为 builtin `copy_of:<T>`。
- **新增 §7.1.4 / §11.8**：字段注解 `#Val`（浅不可变）/ `#Frozen`（深不可变，传染）。构造函数 / 析构函数内放行，其它成员函数写报 `E3109`；非法 / 重复注解报 `E3108`。字段层注解凌驾外层 `var` 声明。
- **附录 B**：`statement` 加 `statementCvalDeclAssign` 产生式；`filedDecl` 接 `(buildAnno)*`；`fnParamStd` / `fnParamGroup` 前置 `(paramAnno)*`，新增 `paramAnno ::= '#' ID LineEnd?`。
- **附录 D**：新增错误码段 E3104–E3111。
- **冲突 / 兼容**：纯增。已有项目代码无 `#Const` / `#Frozen` / `#Val` 注解、不写局部 `cval` 时行为不变。参数默认 `val` 早已落地（`E3093`），本条目仅文档化。Compiler 路径已接入；`yux-check`（SemaPass）镜像本轮**不**做（与 borrow / 错误模型 P 阶段一致）。

## 2026-05-14 —— 警告（warning）通道首批落地：E5013 / E5014 / E7016

- **新增**：`DiagnosticEngine::emit` 非抛出发射通道（附录 D §D.5.1）。默认 Warning 的码渲染到 stderr 后**不**中断编译；`-Werror` / `--deny=<code>` 升级为 Error 时正常抛出 `YuxError`、走顶层 `renderYuxError`（无双重渲染）。
- **新增 E5013**：`yux.toml` `entry` 为绝对路径 → error（附录 D §D.3.5、§10.1.3.2）。
- **新增 E5014**：`yux.toml` `entry` 解析后逃出 `<src>/` 子树 → warning（同上）。
- **激活 E7016**：DRAFT-错误.md §4.2 / spec §4.12.1.4 —— `try` 块内对 `#Fallible` 调用写 `!` 冗余。原 `DEF_WARN` 仅注册未发射，本次接入 `sema::checkErrPropagateForIdCall`，Compiler / SemaPass 双跑由 emit 内部 `(file, code, line, col, msg)` 5 元组去重。
- **冲突 / 兼容**：纯增。已有项目若 `entry` 是相对路径且解析在 `<src>/` 内（约定形态）行为不变。

## 2026-05-13 —— `#TestIsolate` per-test 隔离修饰注解（已移除）

- **移除**：`#TestIsolate` 注解已全面移除。`yux test` 现固定使用子进程派发模式，不再需要 per-test 隔离修饰。
- **冲突 / 兼容**：使用了 `#TestIsolate` 的测试文件需删除该注解。

## 2026-05-12 —— 嵌套数组字面量按外层靶向类型递归编译

- **新增**：§4.10.1.4 —— 靶向类型递归下传到嵌套字面量。`Array<Array<T>>` 用 `[[..],[..]]` 初始化时，内层数组字面量按 `Array<T>` 堆句柄编译，不再被自身 `getType()` 推断为 `[T * N]` 固定数组。补齐之前 §4.10.1.2 / §4.10.1.3 隐含但未明文化的递归规则。
- **冲突 / 兼容**：纯澄清，无破坏；之前嵌套字面量在实现层因写入越界触发 SEH 崩溃，规范层无人观测过该行为。

## 2026-05-12 —— assert_eq 两实参类型不一致诊断

- **新增**：附录 D §D.3 错误码 E6031 —— `assert_eq(actual, expected)` 两实参 LLVM 类型不一致时给出明确错误，取代旧版本直接触发 LLVM `CreateICmpEQ` same-type 断言导致编译器崩溃的行为。典型场景：`assert_eq(arr.len(), 3)` —— `Array.len()` 返 `usize`，整型字面量默认 `i32`。提示用户给字面量加后缀或写 turbofish `assert_eq:<T>(...)`。
- **冲突 / 兼容**：纯增诊断。原本会编译器崩溃的代码现在报 E6031；语义无变化，无 yux 程序行为差异。

## 2026-05-11 —— Dyn<D> / Dyn<D&> 运行时多态落地

- **新增**：`docs/spec/12-draft.md` §12.9 `Dyn<D>` / `Dyn<D&>` 全章节（类型档位 / 出现位置 / 静态检查 / 对象安全 v1 第一轮 / 构造 / 方法分派 / vtable 模型 / RC / ABI / FFI 边界 / 不在范围）。fat pointer = `{ vtable_ptr, data_ptr }`，per-(Type, Draft) vtable；owned `Dyn<D>` 与 `Box<U>` 同源，借用 `Dyn<D&>` 按 §8.6 借用栈追踪。构造走 `Dyn:<D>(x)` turbofish，对象安全 v1 禁止 `Self` / draft-name 在非 receiver 位置。
- **修改**：§12.8 项 1 由"不在范围"改写为"已实现，见 §12.9"，编号保留以维持追溯；Open Issues 删除 `dyn Draft` fat pointer ABI 验证条。
- **新增**：附录 B 加 §B.2a 小节（`Dyn` 类型形态与构造形态约束）；附录 D §D.3.8 E11xx 表追加 E1131..E1136 六行（与 `include/error_code.h` 现有 `DEF_ERR` 同步）。
- **冲突 / 兼容**：纯增。§12.8 项编号不变（项 1 保留为指针）；E11xx 段内号纯追加。草案 `docs/spec/draft/DRAFT-dyn-draft.md` 已落地，正文以 §12.9 为准；草案保留以维持决议日志（[#1.A][#1.B][#2.A][#3.A][#4.A][#5.A][#Z][#H]）追溯。

## 2026-05-10 —— stdlib I/O 泛型化 + panic stderr

- **修改**：SDK `sdk/yux/src/yux/core/base.yux` `print` / `println` 由非泛型 `(s String)` 推广为两态：非泛型借用 `(s String&)` 快路径 + 泛型 `<T : ToString>(x T)`。owned String / 字符串字面量经 §7.2.3.3 自动取址命中快路径，其它实现 `ToString` 的类型走单态化分发。
- **新增**：SDK 同文件追加 `eprint` / `eprintln`（同样两态 String& + 泛型），写 stderr。
- **修改**：`sdk/yux/src/yux/core/panic.yux` `_yux_panic_failed` 模板 `panic: <msg>\n` 由 stdout 改写 stderr，与 `docs/spec/draft/DRAFT-错误.md` §8.6（main 退出码 / stderr）对齐；删除 `panic.yux` 中 TODO(panic stderr)。
- **冲突 / 兼容**：纯放宽。旧 `print(some_string)` / `println(some_string)` 调用无变化；panic 文本现在写 stderr，依赖 `1>` 捕获 panic 信息的脚本需改 `2>`。spec 正文未引入新条款（`print` / `eprint` 为 stdlib，非语言面）；DRAFT-错误.md §8.6 stderr 模板措辞已就位。

## 2026-05-10 —— 错误处理 v1 草案落地（spec 正文 + 附录）

- **新增**：`docs/spec/04-表达式.md` §4.12 错误处理表达式（`exprErrPropagate` / `exprTry` / `catchClause`）；§4.2.1 优先级表追加 `exprErrPropagate`（与 `exprCall` 同档 lvl 10）；§4.9.1.4 / §4.9.3.5 加注 "`ret` / `#NoReturn` 调用所在 arm 视为流终止，不参与表达式类型合并"。
- **新增**：`docs/spec/06-函数.md` §6.7 失败声明 `#Fallible(E)`（声明形态 / 调用约束 / 与 `#NoReturn` 互斥 / extern 限制）。
- **修改**：`docs/spec/11-编译期注解.md` §11.1.1.1 `buildAnno` 单参数糖解禁（`'(' ID ')'` 可选段，仅 `#Fallible(E)` 用）；§11.1.3.2 同步；§11.5.1 表追加 `#Fallible(E)` / `#NoReturn` 两行；§11.5.1.1 由"三种正式注解"改为"五种"。
- **不动**：`docs/spec/03-类型系统.md`（**不**引入独立 `Never` / `Bottom` 类型名；`#NoReturn` 函数签名仍写空 retType；"永不返回"是注解层属性、不是类型层属性，控制流分析复用 `ret` arm 的流终止语义）。
- **修订**：根据 `src/yuxParser.g4` 实际改动同步附录 B 与 §4.12：`!` 是 `exprCall` / `exprCallTrailingOnly` 内嵌后缀槽（`errPropagate=SymbolExcl?`），**不**构成独立 `exprErrPropagate` 产生式；`exprTry` → `exprTryCatch`，`catchClause` → `catchArm`。DRAFT-错误.md §4.4 加入"trailing lambda 协同"小节：`f(...) { lam }!` / `f { lam }!` 合法；lambda body **不**承载 `#Fallible`（与 fn 值类型一致），try 域穷尽性**不下钻 lambda body**。
- **冲突 / 兼容**：§4.2.1 优先级表 lvl 4 追加 `exprTryCatch`（与 `exprOneLineIfElse` 同档）；引用方注意。其它章节增项纯增、不破坏既有条款引用。

## 2026-05-10 —— 错误处理 v1 草案落地（附录侧）

- **新增**：草案 `docs/spec/draft/DRAFT-错误.md` —— 错误处理 v1（值返回 only / `#Fallible(E)` 注解 / `ret E::V` 抛出 / 后缀 `!` 传播 / `try-catch` 块 / `panic` + `exit` + `#NoReturn`）；明确 yux **不引入异常机制**，无 unwind / SEH personality。
- **新增**：附录 A 关键字表追加 `try` / `catch`（v1 硬关键字，DRAFT-错误.md §5）；注解表追加 `#Fallible(E)` / `#NoReturn`；`ret` 备注扩"错误抛出"。
- **新增**：附录 B 语法汇总追加 `exprErrPropagate` / `exprTry` / `catchClause` 产生式；`buildAnno` 同步加单参数糖。
- **新增**：附录 D 段位表追加 E7xxx 行；新增 D.3.7 E7xxx 表（共 18 条；E7001-E7014 默认 error，E7015-E7018 默认 warning）；原 D.3.7 E11xx 顺移为 D.3.8；D.5.1 默认严重度段更新。
- **冲突 / 兼容**：附录 D 子节编号 D.3.7 → D.3.8（draft / 接口）需引用方注意；其它纯增。spec 正文 §3 / §4 / §6 / §11 章节同步、`src/yux*.g4` 改动、SDK `panic` / `exit` 实现、`tests/cases/diag_throw_*` 用例为后续阶段，本次仅落附录。

---

## 2026-05-09 —— `yux build` 的 `<name>` 改为可选

- **修改**：§10.1.2.1 —— `yux build [<name>]`，`<name>` 可省略，省略时驱动取 `yux.toml` 的 `name` 字段；显式给出时仍**应当**与 `name` 一致，不一致驱动以非零退出码报错。当前每个 `yux.toml` 仅声明一个目标，省略形式为推荐写法。
- **冲突 / 兼容**：纯放宽，旧用法 `yux build <name>` 行为不变。

---

## 2026-05-08 —— Lambda 与函数类型 v1 落地（spec 层）

- **新增**：§3.11 全节"函数类型 `fn(T)R`"——出现位置 / 紧凑空格形态 (`fnType ::= 'fn' '?'? '(' fnTypeParams? ')' retType?`，[#16][#23]) / nullable 紧凑形 `fn?(T)R`（唯一合法形，禁 `(fn(T)R)?`，[#24]）/ 结构等同 / 与类型别名 / 与借用 `T&` / 写法示例 / 比较禁。§3.2.2 复合类型表追加 `fn(T)R`；§3.4.1.4 加"结构等同两档"条款（元组 + 函数类型）；§3.7 禁忌表 +4 行。
- **新增**：§4.8.4 尾随 lambda 调用糖（仅块形可尾随，唯一实参省 `()`，与 turbofish 相容）；§4.11 全节"Lambda 字面量"——形态总览（表达式形 + 块形 + 0 参） / 形参类型推断（[#10]） / 返回类型规则表（裸参推断 / 括号无标注→void / 括号显式标注，[#12][#14]） / 实参位置必括（[#13]） / 表达式体可含 `if` / `match`（[#15]） / 闭包语义指路 §8.7.6 / 不在范围。
- **新增**：§6.1.1.6 lambda 字面量定位（落 §4.x，不属本章 fn 声明）；§6.5.5 函数值调用 ABI（fat-ptr `{ fn_ptr, captures }`，captures 永远作隐式首参，[#17] C1）；§6.6.2.1 不允许列追加 `fn(T)R`。
- **新增**：§8.7.6 全节"函数值与闭包传递"——档位 / 作实参字段返回值容器 / 字段容器合法性 / 自动捕获（[#18]，无显式列表） / 含 `T&` 捕获栈嵌入 + 不可逃逸（[#19]） / 方法体 lambda 引用 `$`（[#20]） / lambda 返回 `T&` 与 §8.6.10 衔接（[#21]，捕获 `T&` 不进允许源集） / lambda 体禁对捕获变量赋值（[#26]） / AST 层普通化（[#27]）。§8.1.1.1 档位表"值类型"行加 `fn(T)R`；§8.9 禁忌表 +6 行（与 §3.7 同步）。
- **修改**：§8.1.2.5 / §8.10.1 从 v1 不在范围清单中**移除**"闭包"——v2 错误模型 try/catch handler 必须能访问外层变量，闭包 v1 必含。
- **新增**：§8.10.1 v0.x+1 推迟项 +3：函数类型跨 FFI 边界（[#22]）/ 泛型 lambda + 泛型 fn 类型字面量（[#25]）/ `#Inline`-`#CallOnce` 注解（[#28]，解锁 lambda 内对外层 `var` 赋值与性能 / 非局部返回）。
- **范围**：v1 形态 = 函数类型字面量 + 表达式形 / 块形 / 0 参 lambda + 闭包（自动捕获，无显式列表） + 尾随调用糖 + fat-ptr ABI（永远 `{fn_ptr, captures}`，零捕获 captures=null）。**不**含：FFI 边界 fn 类型 / 泛型 lambda / `#Inline` 优化 / lambda 内对捕获赋值 / 块表达式（走 stdlib `fn run<R>(block fn()R) R` + 尾随糖）。决议日志见 [draft/DRAFT-lambda.md](draft/DRAFT-lambda.md)（[#1]–[#28]）。
- **冲突 / 兼容**：纯增量。`=>` 已是 enum match arm token（§4.7 / 附录 A.4 `SymbolEqMt`），lambda 复用同 token，不冲突。`fn?` 紧凑形是 `fn` + `?` 两 token 由 `fnType` 产生式吸收，不污染其它表达式位置；`(fn(T)R)?` 形态由 `fnType` 不进入 §3.6 `typeNullable` 后缀候选直接拒绝。
- **退出验证**：spec 层落地完毕；编译器实现 / `src/yux*.g4` 改动 / `tests/cases/lambda_*` 用例 / 用户教程 `docs/Lambda与闭包.md` 留后续 commit 分阶段落（按 [draft/DRAFT-lambda.md](draft/DRAFT-lambda.md) §10 迁移面）。

## 2026-05-07 —— 枚举（enum + match）v1 落地

- **新增**：§3 §3.10 全节"枚举"——形态与档位 / 声明位置与结构约束 / payload 类型限制 / variant 全限定访问 / discriminant 不可观测 / 与既有特性的相容性 / 构造与读取通道。§3.2.2 复合类型表追加"用户 `enum`"。
- **新增**：§7.9（informative）struct ↔ enum 对照（值类型族邻居）；选用建议。
- **新增**：§11.5.2.3 明确 enum / match v1 不引入新注解（`__enum_drop_<E>` / `__enum_copy_<E>` 是隐式派生函数，不暴露注解入口）。
- **新增**：附录 A 关键字 `enum` / `match`；A.4 符号表 `::` (`SymbolColonColon`) / `=>` (`SymbolEqMt`)；A.6.2 移除 `match`（已升格为关键字），保留 `case` 作为不入语言的对照。
- **新增**：附录 B 顶层加 `enumDecl`；新章节 B.5b 枚举形态；§B.6 表达式追加 `exprMatch` / `exprEnumCtor`，`opAssign` 之后追加 `matchArm` / `enumPattern`。
- **新增**：附录 C 术语表 §C.2 追加 enum / variant / payload / scrutinee / 穷尽性 / 绑定 / 兜底分支 / discriminant 共 8 项。
- **新增**：附录 D §D.3.2 追加 E2018–E2027（enum / match 语法 / 语义层）；§D.3.3 追加 E3027（match arm 类型失配）。
- **新增**：用户教程 `docs/枚举与匹配.md`，加入 `docs/index.md` 索引。
- **范围**：v1 形态 = 顶层声明 + 全部构造形态（`E::V` / `E::V()` / `E::V(args)`，含 RC payload）+ `match` 最小子集（穷尽 + binding + `else` 兜底）。**不**含：泛型 enum / enum 上方法 / draft 实现 / struct-style payload / 显式 discriminant / `as i32` / `_` 通配 / `|` 多模式 / 字面量模式 / 嵌套模式 / 守卫。形态决议日志见草案 [draft/DRAFT-枚举.md](draft/DRAFT-枚举.md) 决议日志（[#1.A]–[#6.F]）。
- **退出验证**：`xmake build yux` OK；`cd sdk/yux && yux test` 256/256（含新增 9 个 enum match 用例 `sdk/yux/src/yux/core/enum.test.yux`）；`xmake test` 45/45。
- **冲突 / 兼容**：纯增量。`enum` / `match` 升格为新关键字，原本可作 `ID` 使用的同名标识符**应当**重命名（仓库内无既存使用）。`::` / `=>` 是新符号 token，不与既有运算符冲突。enum 是新名义类型档，与现有 `struct` / `Box` / `Array` / `Nullable` 等无破坏性交互。
- **已知小坑**：`var z = NoSuch::A`（无类型注解 + 未知 enum）触发 `declareAssign` 段错；非 enum 特有，留待后续整改。`match` arm body 是借用绑定，把 binding 直接当 match 结果返回时仅 `Box` / `Array` / `Weak` 走归一，其它 RC 类型（含 String）作返回值时**应该**先用 binding 间接计算 POD 再返回。

## 2026-05-06 —— 元组（tuple）+ 透明类型别名

- **新增**：§3.2.2 复合类型表加入 `(T1, T2, ...)`；§3.8 全节"元组"（形态、构造 / 成员访问 / 解构 / 成员赋值 / 与别名泛型的协作）；§3.9 全节"类型别名"（透明 type alias、`A = T` / `Pair<T> = (T, T)`、解析与禁忌）。
- **新增**：附录 B 顶层 `aliasDecl`、`type` / `typeWithRef` 加 `typeTuple` / `typeTupleWithRef`；`expr` 加 `exprTuple` / `exprTupleMember`；`statement` 加 `statementDeclareAssignTuple`，`statementAssign` 路径段允许 `DOT_NUM`；词法 token 表追加 `DOT_NUM`。
- **新增**：附录 D §D.3.2 追加 E2015 / E2016 / E2017；§D.3.3 新增"元组（E3100..E3102）"段。
- **新增**：用户教程 `docs/类型系统.md` 加"元组类型 `(T1, T2, ...)`"与"类型别名"两节，含构造 / 访问 / 解构 / 成员赋值 / 别名循环 / 名称冲突的示例与错误码引用。
- **退出验证**：`xmake test` 45/45、`cd sdk/yux && yux test` 247/247。
- **冲突 / 兼容**：纯增量。元组 LLVM 落地为匿名 `struct`（结构等同），别名为透明 type alias（不引入新类型）；不破坏既有条款。曾经的 `struct A = T` 草稿写法在 g4 层移除（Phase 0 已落，commit 44619fd）；本次仅在 spec 层补全收口。

## 2026-05-05 —— v0.6 收口（语法/语用可用性）

- **范围回顾**：字符串模板（Phase 1a/1b/2a/2b/2c）+ String `+` 链合并 lowering 落地；`for in` 迭代下放 v0.7。
- **spec 触达章节**：§4.3.1.5–§4.3.1.7（字符串模板：词法、AST、lowering、类型规则）/ §4.4.1.4（String `+` 重写为正面条款）/ §9.3.4.1（`+` 移出"未提供"清单）/ §11.4 与附录 D §D.3.3（[E3026] 错误码）。
- **教程同步**：`docs/内置类型.md` 在 § String 内新增"字符串模板（v0.6）"与"字符串 `+` 连接（v0.6）"两节；移除"`+` 暂未提供"陈述。
- **退出验证**：`xmake test` 40/40、`cd sdk/yux && yux test` 222/222。
- **冲突 / 兼容**：纯增量。`for in` 仅在 `yuxParser.g4` 留有 `statementForIn` 语法骨架（无 AST / codegen），写入此结构的源码当前会在编译期落空（不报语法错），v0.7 实现时一次性补齐。MILESTONE.md 已把 `for in` 移至 v0.7 范围。

## 2026-05-05 —— v0.6 Phase 2c 字符串 `+` 链合并为 `StringBuilder`

- **新增**：§4.4.1.4 重写。`+` 任一操作数为 `String` 时整条左结合 `+` 链 lower 为单条 `StringBuilder` 累加；叶子允许 `String` 或实现 `ToString` 的类型，未实现报 [E3026]。规则与 §4.3.1.7 模板插值一致。
- **修改**：§9.3.4.1 不再把 `+` 列入"未提供"清单；新增引用 §4.4.1.4。
- **修改**：§4.3.1.7 阶段进度脚注中 Phase 2c 状态由"待落地"更新为"已落地"。
- **修改**：Open Issues 中"§4.4.1.4 字符串 `+` 是否在未来引入"标记为已收口。
- **冲突 / 兼容**：纯放宽。原本 `s1 + s2` 报 E3074 的代码现在合法；语义为新建 `String`（句柄独立），不修改原 `String`（仍不可变）。`String` 上未引入 `String.plus` 方法，纯由编译器 lower。`-` / `+=` 在 `String` 上仍未定义。

## 2026-05-05 —— v0.6 Phase 2b 字符串模板 `ToString` 自动分发

- **修改**：§4.3.1.7 类型规则放宽 —— 插值位置允许任何实现 `ToString` 的类型；非 String 由编译器在插值位置合成 `expr.to_string()` 调用，复用 v0.5 单态化方法分发。
- **修改**：[E3026] 措辞改为"未实现 `ToString`"，从"插值位置只接受 String"放宽为"插值类型未实现 `ToString`"；命中场景仍为类型不匹配段。
- **冲突 / 兼容**：纯放宽。Phase 2a 报 E3026 的非 String 内置类型插值（`i32` / `bool` / `f64` 等）现在合法；显式 `Type : ToString { ... }` 用户类型同样可插值。诊断用例 `diag_strtpl_non_string_interp` 改为不实现 `ToString` 的类型（`Array<i32>`）兜底。

## 2026-05-05 —— v0.6 Phase 2a 字符串模板 codegen（`StringBuilder` lower）

- **修改**：§4.3.1.7 增 lowering 伪代码与类型规则 —— 含插值的 `StringTemplateNode` lower 为 `sb := StringBuilder() / sb.append(...) / sb.build()`。
- **新增**：[E3026]`String template interpolation requires String type, got '{}'` —— Phase 2a 期间插值位置只接受 `String`；非 String 显式提示调 `.to_string()`，附录 D §D.3.3 类型不匹配段范围由 E3001..E3025 扩到 E3001..E3026。
- **冲突 / 兼容**：纯增量。Phase 2b 引入 `ToString` 自动分发后，约束放宽至"实现 `ToString`"；该错误码语义随之改写为"未实现 `ToString`"。

## 2026-05-05 —— v0.5 内置 `to_string` 迁入 `Type : ToString` 显式实现

- **修改**：§12.7.1.2 措辞调整 —— 内置类型 `to_string` 以 `Type : ToString { ... }` 形态在 `base.yux` 显式实现；方法体可 `#Builtin` 或 yux 实现，二者并存；当前 i64/u64/f64/bool/String 走 yux 实现，窄类型委派。
- **冲突 / 兼容**：`base.yux` 内对应方法从普通方法块迁入 draft 实现块，外部调用面（`x.to_string()`）不变，无破坏。

## 2026-05-04 —— 引入 §12 draft（接口与约束）+ `#DraftLike` + `<T : D>` 边界 + `copy_of`

- **新增**：§12 全章 —— draft 声明 / 显式 `Type : D { ... }` 实现 / `#DraftLike` 结构化匹配 / 跨包 orphan / Box forward 归一 / 内置 `ToString` 与 `Any` / builtin `copy_of:<T>(x T&) T`。决议依据见 `docs/spec/draft/DRAFT-draft.md`。
- **新增**：§7.8（draft 实现块语法扩展，详见 §12）；§6.4.4（draft 边界声明形态、单态化校验）；§11.4（`#DraftLike` 注解 + 互锁规则 + 风格指引）；§11.5 / §11.6 重编号（原 §11.4 → §11.5，原 §11.5 → §11.6）。
- **修改**：§6.4.1.2 把"v0.5 引入 draft 后启用 trait bounds"改为正文，明确 v1 支持类型参数 + 可选 draft 边界，无 `where` / 无 or 约束；§11.2.3 builtin 清单追加 `as_ref` / `copy_of`，内置类型 `to_string` 经 `Type : ToString { #Builtin ... }` 形式给出。
- **新增**：附录 A 关键字加 `draft`、注解表加 `#DraftLike`；附录 B 加 `draftDecl` / 扩展 `structImpl` / `genericDef` 引入 `typeParam` + `draftBound`（v0.5+ 形态，回写 `src/yux.g4` 前需用户确认）；附录 C 新增 §C.6a draft 术语 11 项；附录 D 段位表追加 E11xx 段，§D.3.7 列出 E1101–E1106 / E1110–E1112 / E1120 占位（编号在编译器实施期固化）。
- **冲突 / 兼容**：纯增量（v0.5 新引入）；既有 §6.4 / §7 / §11 / §8.6.7 条款均不破坏，仅追加交叉引用。`#DraftLike` 是 v1 第三种正式注解（继 `#Builtin` / `#Test`）。`src/yux.g4` 暂未改（按 RULES.md 项目约束需先与用户确认）；附录 B 文本与 `.g4` 短期内不同步，以草案 `draft/DRAFT-draft.md` §10.3 为准。
- **后续**：编译器实现详见 CURRENT.md Phase 3；SDK / 测试详见 Phase 4；附录 D 占位错误码在 Phase 3 与 `include/error_code.h` 同步固化。

## 2026-05-04 —— 统一函数泛型声明调用

- **修改**：`fn <T> name()` => `fn name<T>()`
- **冲突 / 兼容**：仅影响sdk

## 2026-05-04 —— 修复 `yux test` JIT 模式下跨 yux 助手帧 SEH 静默崩溃

- **修改**：§11.3.5.8 known-issue 删除——根因是 LLVM `RTDyldMemoryManager::registerEHFramesInProcess` 在 Win64 COFF 上不调 `RtlAddFunctionTable`，导致 `RuntimeDyldCOFFX86_64` 收集的 `.pdata` 段从未注册到 OS，跨多个 JIT 帧 unwind 时 `RtlVirtualUnwind` 找不到 `RUNTIME_FUNCTION` → 进程静默退出。修法：自定义 `SectionMemoryManager` 子类覆盖 `registerEHFrames`/`deregisterEHFrames`，在 `RtlAddFunctionTable` / `RtlDeleteFunctionTable` 中注册 `.pdata`，ImageBase 取本对象内已分配 section 的最低非零地址。
- **修改**：§11 Open Issues 同步移除「`yux test` JIT 模式下，从 yux 实现的助手中触发的 SEH 异常未被 wrapper 捕获」条目；JIT 模式下 yux 助手 fail 路径与 `#Builtin` fail 路径行为一致，均产出 `FAIL <module>#<fn> (SEH ASSERT_FAILED 0xe0fa17ed)`。
- **冲突 / 兼容**：纯修复；既有 v1 用例（仅覆盖 pass 路径）继续通过；之前因 known-issue 暂时移除的 fail 用例可重新启用。

## 2026-05-04 —— `yux test --isolate=process` 子进程隔离（Phase 5）

- **新增**：§11.3.4.4 `yux test --isolate=process`：每个 `#Test` 在独立子进程内执行，崩溃 / 内存脏化只影响该测试。`--isolate=none`（默认）保持同进程 SEH wrapper 行为。
- **新增**：§11.3.4.5 显式记录 `-v / --verbose` 失败时回放 stdout / stderr 的语义（Phase 3 已实现，仅补 spec 层）。
- **修改**：§11.3.4.3 文本同步——v1 默认同进程 SEH wrapper 已能扛住单条 SEH 异常，不再"任一测试崩溃整体非零退出"。
- **副作用**：BUGS.md 记录的「yux 助手失败路径触发的 SEH 在 wrapper 内失活」在子进程模式下被自动绕开（子进程崩溃即子进程退出码，父进程翻译），可作为该 known-issue 的临时 workaround。
- **冲突 / 兼容**：纯增量；默认行为未变；新加的 `--isolate-child` / `--capture` 是 hidden CLI（父子进程协议），不暴露给用户。

## 2026-05-04 —— `assert_eq` 扩展到 String + 新增 `assert_contains` / `assert_starts_with`（Phase 4b）

- **修改**：§11.3.5.2 `assert_eq` 类型分派表加 `String`；分派改由编译器 dispatcher 在「参数严格匹配的非泛型重载存在时优先于泛型」实现（`compiler_call.cpp` 新 `getGenericFunction`），SDK 侧 `base.yux` 末尾追加 `fn assert_eq(actual String&, expected String&)` 等 yux 实现重载。`#Builtin` 泛型 `assert_eq:<T>` 仍是 i8..u64 / f32 / f64 / bool 路径，未变。
- **新增**：§11.3.5.7 `fn assert_contains(haystack String&, needle String&)` / `fn assert_starts_with(s String&, prefix String&)`，纯 yux 实现，分别调用新增的 `String.contains` / `String.starts_with` 方法。
- **新增**：`String` 加方法 `contains(needle String&) bool` 与 `starts_with(prefix String&) bool`（base.yux）。
- **新增**：§11.3.5.8 known-issue —— `yux test`（JIT 模式）下，由 yux 助手的失败路径触发的 `_yux_test_assert_failed()` SEH 异常**不**被 wrapper 捕获，runner 直接 abort；v1 用例只覆盖 pass 路径，fail 路径暂由 `#Builtin` 数值/`bool`/`fail` 断言覆盖。详见 `BUGS.md`。
- **冲突 / 兼容**：纯增量；既有 `.yux` 源码无破坏。运算符 dispatcher 副作用：当用户同时定义同名 generic 与非泛型重载时，参数严格匹配的非泛型现在优先（更接近常见语言语义；先前是先到先得）。`compileCustomTypeBinaryOp` 同期加固：操作数本身是 `T&` 时剥一层 ref 后再做方法表查找；`compileKnownFunctionCall` 加固：非局部变量（字面量 / 临时值）作为 `T&` 形参实参时 alloca-store 临时再传 ptr。

## 2026-05-04 —— 运算符重载形参收口为 `Self&`

- **修改**：§7.2.3.3 二元运算符方法形参从"应当与接收者类型一致"改为"应当为 `Self&`"；形参为 `Self`（按值）等其它类型时该方法只是普通方法，不再被运算符触发。
- **新增**：§7.2.3.6 运算符 `a OP b` 在编译期重写为 `a.method(&b)`，右操作数自动取址，是 §6.2 / §8.3 一般规则的运算符位置局部例外。
- **冲突 / 兼容**：v1 尚未发版，原"形参写 `Self`"是旧设计的不足（按值复制额外成本，且与 `$` 接收者借用形态不一致）；规范层一次性收口。本仓库内仅 `docs/结构体.md` Complex 示例使用旧形态，同步改为 `Self&`；SDK 与 tests/ 中无既有运算符重载实现，无代码迁移。

## 2026-05-03 —— 新增 `#Test` 测试断言 API（`assert_eq` / `assert_true` / `assert_false` / `fail`）

- **新增**：§11.3.5「测试断言 API」 —— SDK 在 `sdk/yux/src/yux/core/assert.yux`（与 `base.yux` 同属 `yux.core` 平铺）提供 4 个 `#Builtin` 断言：泛型 `assert_eq:<T>`（T ∈ i8…u64 / f32 / f64 / bool）、`assert_true(bool)` / `assert_false(bool)`、`fail(String)`。无需 import，全局可用。失败语义（v1）：调 SDK `_yux_test_assert_failed()` → `RaiseException(0xE0FA17ED)` → SEH 显示 `FAIL <module>#<fn> (SEH ASSERT_FAILED 0xe0fa17ed)`。v1 **不**打印断言种类与 `fail(msg)` 的 `msg` 内容（待 String stringify 扩展同期补齐）。
- **新增**：附录 D §D.3 错误码 E6030 —— `assert_eq:<T>` 类型实参越界（仅数值 + bool）。`E6027 {} expects {} argument(s)` 复用为四个断言的 arity 错误。
- **修改**：§11 Open Issues 收口"`#Test` assert API 形态"，新增"打印实参值需先引入 `Stringify` 约束"与"`assert_eq` 扩展到 String / 用户结构体"两项后续。
- **冲突 / 兼容**：纯增量。SDK API 集合扩展，新模块 `yux.test.assert`；既有 `.yux` 源码无破坏。`#Test` 函数体内调用断言与 §11.3.2「无参 / 无返回 / 必须有体」约束相容。

## 2026-05-03 —— 新增 `#Test` 注解与 `yux test` 子命令

- **新增**：§11.3 `#Test` 注解条款 —— 仅挂 `fn`；签名等价 `fn name(): void`（无参、无返回类型、必须有函数体）；与 `#Builtin` 互斥；仅可出现在 `*.test.yux` 文件中。普通 `yux build` 跳过 `#Test` 函数的 codegen，不进入 `.exe` / `.lib` 产物。
- **新增**：§11.3.3 测试文件发现规则 —— `*.test.yux` 仅由 `yux test` 在项目 `src/` 下递归发现；模块名取 src 相对路径转点分形式，保留 `.test` 段（例：`src/yux/core/arithmetic.test.yux` → `yux.core.arithmetic.test`）。
- **新增**：§11.3.4（informative）`yux test` 行为 —— 仅项目模式可用；选择器支持 `<prefix>` 前缀匹配、`<module>#<fnName>` 精确定位；v1 同进程顺序执行，无隔离（崩溃即整体非零退出）。
- **修改**：原 §11.3「其他注解」与 §11.4「用户自定义注解」整体下移为 §11.4 / §11.5；§11.4.1 注解对照表加入 `#Test` 行。
- **新增**：附录 C 加入 `#Test` 与 `yux test` 术语条目。
- **冲突 / 兼容**：纯增量。既有 `#Builtin` 条款全部保留；既有 `.yux` 源码无破坏（现有源码均不带 `#Test`，且不存在 `*.test.yux` 文件）。

## 2026-05-03 —— 诊断列号语义收口 + 多字节插入符对齐

- **修改**：附录 D §D.1.1 —— `col` 字段语义由"1-based 字节列号"更正为"1-based 字符列号"（按 Unicode codepoint 计数，与 ANTLR `getCharPositionInLine() + 1` 同源；纯 ASCII 输入下数值与原文档一致，回归测试 0 改动）。
- **新增**：§D.1.1 明确插入符 `^` 按**显示列宽**对齐：CJK / 全角 / 常见 emoji 计 2 列，组合标记 / 零宽字符计 0 列，Tab 原样保留。
- **删除**：§D.7 Open Issues 第 1 条（"列号当前按字节计算..."），改写为已收口注记。
- **冲突 / 兼容**：纯措辞 + 渲染对齐改进；`col` 数值不变；现有 `diag_*.expected_err` 子串断言全部兼容。新增 `tests/cases/diag_multibyte_caret.{yux,expected_err}` 锁定多字节场景下 ^ 落点。

## 2026-05-03 —— 语法换行规则放宽（Kotlin 风单行 / 多行）

- **新增**：
  - §6.1.1.5 `fnHeader` 形参列表支持单行 / 多行两种写法（`(` 后、参数间 `,` 后、`)` 前允许 `LineEnd*`，末参可尾随 `,`）。
  - §6.2.1 `fnParams` 产生式同步更新；§4.7.1.5 `exprGetRef` 链中 `.` 前允许换行；§4.7.2.5 `exprDot` 链式 `.` / `?.` 前允许换行；§4.7.3.3 `exprGet` 索引列表允许换行 + 尾逗号；§4.8.2.5 `exprCall` 实参列表允许换行 + 尾逗号；`exprArray` 数组字面量同此。
  - 附录 B §B.4 / §B.6 产生式与 `src/yux.g4` 对齐。
- **格式化器规则**（*informative*，规范层不强制；详见 `CURRENT.md` Phase 5 设计文档）：函数声明 1 参强制单行，多参默认单行、超 120 列折成多行；链式 `.` 默认仅超列宽时折，手写换行格式化器保留不合并。
- **冲突 / 兼容**：纯放宽，原有所有单行写法保持合法；AST 不变（`LineEnd` 在新位置仅作分隔，不入 AST），ASTBuilder / 编译器零改动。



- **新增**：
  - §8.3.5.4 `&box` 永远生成 `Box<T>&`，无隐式降级；§8.3.5.5 / §8.3.5.6 `as_ref:<T>(box Box<T>) T&` baked builtin 规范化为 v1 唯一获取 payload `T&` 形态的途径。
  - §8.4.2.5 / §8.4.2.6 `Array<T>&` 借用期内禁止调用修改方法（`push` / `pop` / `insert` / `remove` / `clear` / `set_len` / 容量 grow）。
  - §8.6.5.7 `as_ref` 站点根追溯。§8.6.5.8 借用作用域粒度按 §5 块作用域计（v0.4 由函数级扁平对齐至块级）。
  - §8.6.7（新章节）借用与泛型边界匹配：类型形参 `T` 限定 owned 类型（§8.6.7.1）；`Box<U>` 上 `obj.method` receiver 归一为 `U&`（§8.6.7.3）；`use_ref(as_ref(box))` 协作（§8.6.7.4）；类型形参可在签名内组合为 `T&`（§8.6.7.5）。
  - §8.9 / §3.7 联动追加三条禁忌：`as_ref(Box<T>?)` 类型禁、`Array<T>&` 借用期修改禁、用户泛型 `T = U&` 禁。
- **修改**：§8.3.5.4 编号顺延为 §8.3.5.7（FFI ptr_of）；§6.4.1.2 反向引用 §8.6.7.1 / §8.6.7.5。
- **冲突 / 兼容**：现有用户代码无破坏——`as_ref` 是新增能力；`Array<T>&` 借用期 push 限制对当前 SDK / 测试无触发（既有调用模式不构成借用 + 修改的并存）；类型形参 owned 限定与现状 v1 类型推断一致。
- **§8 Open Issues 清理**：`same_ref` / `ptr_of` 接 `T&` 由 §8.6.7.5 解决；其余 6 条转 v0.11 / v0.9 / v1.x 后续版本。

## 2026-05-03 —— Phase 5 B 阶段：未声明标识符的拼写近似建议

- **新增**：`src/symbol_suggest.{h,cpp}` 提供 `nearby` / `buildHint` / `throwSymbolNotFound`：从给定 `ScopeNode` 开始
  沿父链汇总可见变量与函数名，按 Levenshtein 距离 ≤ 2 排序后取最近 1–3 个候选，组装成
  `did you mean \`foo\`?` / `did you mean one of: \`foo\`, \`bar\`?` 形式的 help 行。
- **挂 hint 的站点**：E3030（`compiler_expr.cpp` 字面量加载、成员访问 / 取地址父链 lookup；`compiler_stmt.cpp`
  普通赋值与成员赋值；`node/expr_node.cpp` `&obj`）、E3031（`compiler_expr.cpp` / `compiler_stmt.cpp` 成员访问的
  `_localVarPtrs` miss）、E3032（`node/literal_node.cpp` 标识符字面量类型解析）。
- **测试**：新增 `tests/cases/diag_suggest_var.{yux,expected_err}`，断言 `conut` → `count` 的 help 行。
- **冲突 / 兼容**：无规范条款变更；候选为空时不附 hint，原有错误信息逐字保留，所有既有测试不受影响。

## 2026-05-03 —— Phase 5 A 阶段：诊断 help / note 基础设施 + 高频站点 hint

- **新增**：`Diagnostic` 已有的 `notes` / `hints` 字段接通 `YuxError` —— `YuxError` 携带 `_hints` / `_notes`，提供链式
  `withHint(string)` / `withNote(string)` 便利接口；`DiagnosticEngine::renderYuxError` 把它们作为 `= help: ...` /
  `= note: ...` 行附在源码片段之后输出，遵循 §D.1.1 既有格式。
- **挂 hint 的站点**：
  - E2001 `Weak<T>?`、E3078 `Weak == / !=`：提示 `upgrade(weak)` 路径
  - E3017 / E3018 / E3019：T& 局部初始化形态指引
  - E4001 `BorrowChecker` 借用初始化、E4004 不能绑非本地
  - E2006 / E2007 缺函数体：提示加 `#Builtin` 或补 body
  - E6010 / E6011 泛型实参个数：给出 `:<T...>` 模板
  - E1002 ANTLR 文法错误：按消息模式（`';'`、`mismatched/extraneous input`、`no viable alternative`）附简单空格 / `;` 提示
- **测试**：新增 `tests/cases/diag_ref_init_form.{yux,expected_err}` 与 `diag_generic_arity.{yux,expected_err}`；
  `diag_weak_nullable.expected_err` 追加 help 断言。
- **附录 D**：§D.5.4 路线图重写为 A / B 两阶段，标注 A 已落地；附 hint 的码段一并列出。
- **冲突 / 兼容**：诊断输出格式不变；新增的 `= help:` / `= note:` 行属于 §D.1.1 已经允许的"0..N 条 note / help"，
  既有 `expected_err` 子串匹配机制不会因之失败。

## 2026-05-03 —— Phase 4：诊断分级 / CLI 严重度开关 / 文件级聚合

- **新增**：`DiagSeverity { Note, Warning, Error }` 落地，每个错误码（`ErrorCode::EXXXX`）通过 `DEF_ERR` / `DEF_WARN` / `DEF_NOTE` 携带 `defaultSev`；当前所有码默认 `Error`，`Warning` / `Note` 留待 Phase 5 与未来错误恢复后启用。
- **新增**：CLI `--warn=<code>` / `--allow=<code>` / `--deny=<code>` / `--Werror`；主命令与 `build` 子命令均可使用（subcommand 通过 `fallthrough()` 继承）。
- **不可降级原则**：默认 `Error` 的码不允许通过 `--warn` / `--allow` 降级；尝试降级时打印 `cannot downgrade ... (default severity is error)` 并忽略，理由是当前 Compiler 在 `YuxError` 抛出后即停，没有错误恢复机制（详见 §D.5.3）。
- **聚合策略**：从"首错即出"改为**文件级聚合** —— 单文件 codegen 失败不再立即 `exit(1)`；驱动层继续编译其余模块，最后再以非零退出码结束。链接阶段在任一模块失败时跳过。
- **附录 D**：§D.1.1 严重度叙述更新；§D.1.2 改写为聚合语义；新增 §D.5 严重度策略与 CLI 开关；原"路线图"挪入 §D.5.4。
- **冲突 / 兼容**：诊断输出格式无变化；既有用例 / `expected_err` 全部沿用。新引入的 `--warn` 等选项不传时行为完全等价于此前。

## 2026-05-02 —— Phase 6：诊断回归测试

- **新增**：`tests/cases/diag_*.yux` + `*.expected_err` 用例形态，由 `tests/xmake.lua` 识别并走"编译期望失败 + 逐行子串包含 stderr"的判定路径。首批纳入 6 个用例，覆盖 E1001 / E1002 / E5010 / E2001 / E3020 / E3032 / E3093 / E3094。
- **附录 D**：§D.5 路线图收口（Phase 4 / 5 挪入 `TARGETS.md`，删除 Phase 6）；新增 §D.6 描述 `expected_err` 子串断言协议；原 §D.6 Open Issues 顺延为 §D.7。
- **冲突 / 兼容**：无规范条款变更；测试运行器对既有 `*.expected` 用例零影响。

## 2026-05-02 —— Phase 3：ANTLR 词法 / 文法错误接管

- **新增**：`src/syntax_error_listener.{h,cpp}` 自定义 `BaseErrorListener`，按 `recognizer` 是否为 `antlr4::Lexer` 派发为 `E1001 lexer error: {}` / `E1002 syntax error: {}`，即时通过 `DiagnosticEngine::render` 输出统一格式（file:line:col + 源码片段 + 插入符）。
- **接入**：`Yux::_parseFile` 与 `main.cpp` 的 `parseAST` / `compileIR` / `compileSdkDir` 全部 `removeErrorListeners()` + `addErrorListener(&errListener)`，原 `getNumberOfSyntaxErrors()` 走错合并为 `errListener.hasErrors() || ...`。
- **附录 D**：新增 §D.3.1（E1xxx 段），后续段编号顺延；§D.4 接入表"词法 / 文法"由"未接入"改为已接入；§D.5 路线图删除 Phase 3 项。
- **冲突 / 兼容**：原先 ANTLR 默认 `ConsoleErrorListener` 直接打印到 stderr 的 `line N:col` 形式不再出现；现在所有词法 / 文法错误都带 `[E1xxx]` 错误码与源码片段。

## 2026-05-02 —— 附录 D：诊断与错误码

- **新增**：`docs/spec/附录D-诊断.md`，规范化诊断输出形态（`file:line:col [Exxxx] severity: msg` + 源码片段 + 插入符 + `note` / `help`）、错误码段位（E1..E6xxx）、当前已分配错误码全表，与 `include/error_code.h` 一一对应。
- **新增**：`docs/spec/index.md` 附录索引补入附录 D。
- **冲突 / 兼容**：无规范条款变更；附录 D 与编译器 Phase 2 已落地的 `ErrorCode::E####` / `DiagnosticEngine` 同步。后续阶段（ANTLR 接管、warning 分级、修复建议、回归测试）仍记录在附录 D §D.5 路线图，落地后应回写本附录。

## 2026-05-02 —— 草案目录化：`docs/spec/draft/`

- **新增**：`docs/spec/draft/` 目录，收纳跨章节设计草案与骨架范本 `_模板.md`；草案改为入库（原先存放于仓库根目录、不入 git）。
- **定位明确**：草案**不等同规范**，是否实施以 `docs/spec/` 正文为准；草案与 spec 冲突时以 spec + `src/yux.g4` + 编译器源码为准。AGENTS.md 同步更新。
- **迁入**：原根目录 `DRAFT-所有权与引用.md` → `docs/spec/draft/DRAFT-所有权与引用.md`；§3 / 实施日志中相关相对链接同步修正。
- **冲突 / 兼容**：无规范条款变更；仅文档组织调整。

## 2026-05-02 —— Phase 6：一致性审校与用语统一

- **用语统一**：全文 *必须* → *应当*（§C.8 规范用语集合 = 应当 / 不得 / 应该 / 可以）。"须为" / "须可" 等描述性表述保留，不计入规范用语。
- **新增**：本文件 `docs/spec/CHANGELOG.md`；`docs/spec/index.md` 增入口；`docs/index.md` 增 spec 区块（教程 ↔ spec 互链）。
- **状态**：spec 与 v1 编译器（含 `src/yux.g4` 用户最新调整：`opShift` 优先级高于 `opBitAnd/Or/Xor`）已交叉核对，未发现需登 BUGS.md 的冲突。

## 2026-05-02 —— Phase 5：模块 / 注解 / 附录收口

- **§10 模块系统**（重写）：覆盖 `yux.toml`（含 `[lib]` 节）、`<projectRoot>/src/` 源根、文件模块 vs 包模块、`use a.b.c` / `use a.b.*`、**`pkg` 文件再导出清单**（`name` 别名 / `name.*` 扁平）、命名解析三段式、循环依赖禁、`_` 前缀私有、`yux.core` SDK / `base.yux`。
- **§11 编译期注解**：`buildAnno ::= '#' ID codeLineEnd`；`#Builtin` 互锁规则（带注解必省体；省体（除 extern）必带注解）；v1 不支持用户自定义注解。
- **附录 A 保留字**：与 `yux.g4` 全量对齐（关键字 / 上下文标识符 / 注解名 / 符号 token / 词法 token / 预留）；显式登记 v1 无 `continue` / `pub` / `priv` / `mut` / `const` / `trait` / `match` / `for` / `while` / `do` / `async`。
- **附录 C 术语表**：补 §C.6（模块 / 包 / 项目 / 入口 / 源根 / `pkg` 文件 / 文件模块 / 包模块 / 通配导入 / `_` 前缀私有 / `yux.core`）。

## 2026-05-02 —— Phase 4：所有权（§8）规范化

- **§8 所有权与引用**（新写）：来源 `DRAFT-所有权与引用.md` + `docs/dev/ownership-impl-log.md` 已稳定条目。
  - §8.1 概述（四档：值 / 堆句柄 / 借用 / FFI 指针）；
  - §8.2 RC 协议：Block 布局 `{ rc: { strong: u32, weak: u32 }, payload }`、强弱计数协议、哨兵 `0xFFFFFFFF`、单线程模型；
  - §8.3 `Box<T>`、§8.4 `Array<T>` / `String`、§8.5 `Weak<T>` 与 `upgrade`；
  - §8.6 借用 `T&`：形态 / 不参与 RC / 绑定与 rebind 禁 / 取址 `& expr` 寿命规则（声明作用域 ⊇）/ 接收者 `$`；
  - §8.7 调用 ABI：callee-clean retain、move-return retain 注入、`extern` 边界 `T&` ↔ `Ptr` 自动转换、地址比较；
  - §8.8 临时值清单（temp frame）/ fresh 句柄 / RC leak 计数 `_rc_block_count` / `rc_leak_count()`；
  - §8.9 禁忌一览（字段 `T&` / 返回值 `T&` / `T& &` / `Weak<T>?` / `Weak == /!=`）；
  - §8.10 v1 不在范围（cycle collector / 多线程 / `Weak<Array>` / `Weak<String>`）。

## 2026-05-02 —— Phase 3：结构体 / 内置类型

- **§7 结构体**：声明、方法块、构造函数 + DAA、析构函数、按值复制、平凡 / 非平凡分类、字段级派生、自引用必经堆。
- **§9 内置类型**：`Box<T>` / `Array<T>` / `String` / `StringBuilder` / `Weak<T>` / `Ptr` / `Ref<T>` / 数值标量；`null` 仅在 `T?` 与 `Ptr` 上下文出现。

## 2026-05-02 —— Phase 2：表达式 / 语句 / 函数

- **§4 表达式**：求值顺序（左到右、自底向上）、运算符语义、短路 `&&` / `||`、安全访问 `?.`、null 兜底 `??`、取址 `& expr`、`if` 表达式形态。
- **§5 语句与控制流**：`var` / `val` / `cval` 三类声明、赋值复合形态、`if` 语句、`loop` + `break`、`ret`、词法作用域析构序。
- **§6 函数**：声明、参数组（`a, b, c i32`）、表达式体 `= expr`、泛型 turbofish `name:<T>(args)` + 单态化、`extern fn` C ABI、ABI 概要指向 §8.7。

## 2026-05-02 —— Phase 1：词法 / 语法 / 类型基础

- **§1 词法**：行首列概念、注释三态（`LineComment` / `LineEndComment` / `EmptyLine`）、标识符（Unicode）、字面量（数值含进制 / 浮点 / 字符串 / 码点 `c'<ch>'` / `null`）。
- **§2 语法**：ANTLR4 / EBNF 叙述约定；附录 B 同步建立。
- **§3 类型系统**：标量 / 用户 struct / `[T*N]` 三种值类型、堆句柄、借用 `T&`、FFI `Ptr`；名义类型判等；可赋值性（无隐式标量转换）；§3.6 Nullable `T?` + 隐式包装；§3.7 禁忌（`Box<T>?` 等价 `Box<T>` / `Weak<T>?` 禁 / `Weak == /!=` 禁）。

## 2026-05-02 —— Phase 0：骨架

- **新建** `docs/spec/`：`index.md` + 11 章 + 附录 A / B / C 占位骨架，确立 §N / §N.M.K 编号与 *应当 / 不得 / 应该 / 可以* 用语约定，每章末预留 *Open Issues*。

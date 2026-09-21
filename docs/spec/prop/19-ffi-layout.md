# 19 FFI 布局：`#Packed` / 对齐 / union 展开

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-20 |
| 旧档 | 无。现行：§7.5.3 声明序 + DataLayout pad、不 packed、不重排；C-layout §7.5.3.4 / §6.6.2；Win64 按值 ABI §8.7.4.2；`size_of<T>()`。相关：[#10 多目标](10-multi-target.md)（非 Win64 C ABI）；[#20 `_` 丢弃名](20-discard.md)（pad 槽，本条不依赖） |

不绑版本。不改 `riu.bnf`：`#Packed` / `#Align(N)` 走已有 `buildAnno`。**不**引入语言 `union`、**不**做 Zig 比特域 packed、**不**写 Win32 / 某个 C 库进 spec。任意 C 库用 `extern` + C-layout 都能绑。

## 提议

把「能按 C 的布局和调用约定过 `extern`」收成完整规则，而不是只够某份绑定脚本。今天缺的是：**关掉自动 pad**、**写出类型 / 字段对齐**、**C union 用多份同尺寸 struct 当不同视图**、**按值 ABI 跟目标 C 编译器走**（含 packed 后的 size / align）。默认非 packed 布局不变。

### 现状

| | 现行 | 缺口 |
|---|---|---|
| 字段序 | 声明序，永不重排（进过 `extern` 的类型） | 够 |
| 自动 pad | 目标 DataLayout，与默认 C 自然对齐同类 | 无法表达 `#pragma pack` / `__attribute__((packed))` |
| 类型 align | 由字段自然对齐推出 | 无法表达 `__declspec(align(N))` / `_Alignas`；packed 后 LLVM align=1，和 C 的 `alignof` 常不一致 |
| 字段 align | 无 | `_Alignas(N)` 成员要对齐偏移 |
| union | 无 | C 头里大量 union；不能加语法 |
| 按值 ABI | 只写了 Win64：store size 1/2/4/8 进整数，否则 byval/sret | packed / 抬对齐之后仍须按**目标 C ABI**分类；其它目标见 #10 |
| 核对 | `size_of<T>() u64` | 无 `align_of`，绑定时对不齐 `alignof` |

C-layout 字段白名单（标量无 `bool` / `Ptr` / 嵌套 C-layout / `[T*N]`）不动。`bool` 仍不准进 C-layout 字段（用 `u8`）。

### `#Packed`

标在 `structDecl` 上，零参，与 `#NoCopy` 同档（非 spec）。LLVM `StructType` packed：字段之间**不**再插编译器 pad，类型自然对齐变为 1。

不是 Zig 那种比特域 packed。bitfield / 灵活数组成员本条不做。

C 侧的 pad 由绑定作者（或生成器）写成显式字段。有 [#20](20-discard.md) 时用 `_`；没有就用 `_pad0` 这类普通名。本条不依赖 #20。

```riu
#Packed
struct FileHdr {
  magic u32
  flags u16
  _pad u16          ; 对应 C 的 2 字节洞；#20 落地后可写成 `_ u16`
  size u32
}
```

`#Packed` 不改变 C-layout 资格：字段类型仍走 §7.5.3.4。非 C-layout 字段的 struct 也可以 `#Packed`（内部布局），只是仍然不能进 `extern`。

### `#Align(N)`

`N` 为 2 的幂（`1` / `2` / `4` / `8` / `16` / …），单参整数，编译期常量。

- **struct 上**：该类型的对齐至少为 `N`。`size_of` 含尾 pad，使得 `size_of % align_of == 0`（与 C `sizeof` 一致）。`[T*N]` 的步长用这份 ABI size。
- **字段上**：该字段偏移向上对齐到 `N`（前面可出现编译器 pad，即使 struct 是 `#Packed`——这是显式对齐，不是「自动按自然对齐插 pad」）。字段自身对齐取 `max(自然对齐, N)`。

`#Packed` 与 `#Align` 独立、可叠：

| 组合 | 布局 |
|---|---|
| 无 | 现行：自然对齐 + DataLayout pad |
| 仅 `#Packed` | 无自动 pad，`align_of = 1`，无尾 pad |
| 仅 `#Align(N)` | 自然 pad，类型 align = `max(自然, N)` |
| 两者 | 无自然 pad；类型 align = `N`；尾 pad 补到 `N` 的倍数；字段 `#Align` 仍按其 `N` 抬偏移 |

绑定 C 类型时两件事都要对上：`size_of` = C `sizeof`，`align_of` = C `alignof`。只 packed、不写 `#Align`，按值传 / 数组步长会和 MSVC/Clang 对不齐。

`#Spec` / enum / 类型别名上不接受这两条。`N` 非法（非 2 的幂、≤0、溢出）诊断。

### union：展开成多份 struct，不引入 `union`

语言不增加 union 类型。一份 C union 写成**若干个同 ABI size / 同 align 的 C-layout struct**，每个是一种视图。绑定选自己要的那份去调 `extern`；需要同一块内存上换视图时用下面的 `overlay`。

C：

```c
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;
```

riu（示意；pad 名按有无 #20）：

```riu
#Packed
#Align(8)
struct LARGE_INTEGER {
  QuadPart i64
}

#Packed
#Align(8)
struct LARGE_INTEGER_Parts {
  LowPart u32
  HighPart i32
}
```

两份 `size_of` / `align_of` 都等于 C 的 `LARGE_INTEGER`。较小的成员视图把其余字节写成显式 pad，使 size 仍等于 union 的 size（取成员 max，align 取成员 max）。

**`overlay:<U>(x T&) U&`**：`T`、`U` 都是 C-layout；`size_of:<T>() == size_of:<U>()`；`align_of:<U>() <= align_of:<T>()`（地址对 `U` 合法）。结果借用与 `x` 同寿命，读写同一块内存。不是类型转换，不 copy。非 C-layout / size 不等 / 对齐不够 → 诊断。

用户绑定任意 C 库都需要这条：`_ptr_as_ref` 仍是 SDK 私有（§8.7.4.5），不能当公开 punning 口。生成在 SDK 里的绑定可以用 `overlay`，用户自己绑 sqlite / libpng 也可以。

不要求两份 struct「字段重叠关系」由编译器证明；重叠对不对是绑定的责任（和 C 里自己 cast 一样）。v1 无 `unsafe`。

### 调用：`extern` 走目标 C ABI

§8.7.4.2 从「仅 Win64」收成：**`extern fn` 的实参 / 返回按编译目标的 C ABI 传递**，内部 riu 函数 ABI 不动。

当前目标 `x86_64-pc-windows-msvc` 仍是现有规则：`bool` 边界 `i1`↔`i8`；C-layout 聚合 store size 为 1/2/4/8 则按整数进寄存器，否则 byval / sret，语义仍是 copy。**分类用 packed+align 之后的 ABI size**；byval / sret / alloca 的对齐用 `align_of`（可以 >1），不得因为 LLVM packed 类型自然 align=1 就把 C 的 8 对齐降成 1。

其它目标（SysV 等）的具体分类表跟 [#10](10-multi-target.md)，本条只钉「跟目标 C 编译器」和 packed/align 之后 size/align 参与分类。不引入 `stdcall` / `fastcall` 注解（x64 无意义；x86 归 #10）。可变参 `extern` 本条不做。回调仍是 `Ptr`，`Function<...>` 仍不准跨 FFI。

### `align_of<T>() u64`

与 `size_of` 同形态的 `#Builtin`。无 LLVM 布局 → 与 `size_of` 同路（E6019 一类）。`#Packed` / `#Align` 必须反映在这两个值里。

### 改为

1. `#Packed`：struct 零参；无编译器自然 pad；默认 align=1。
2. `#Align(N)`：struct 和字段；`N` 为 2 的幂。与 `#Packed` 可叠。
3. 无语言 union。C union → 多份同 size/align 的 C-layout struct；换视图走 `overlay:<U>(T&) U&`。
4. `extern` 按值走目标 C ABI；当前 Win64 规则保留，但 size/align 取本条布局之后的值。
5. `align_of<T>()`。C-layout 白名单、`bool` 禁字段、不重排、无 `unsafe`、不改 bnf。

## 评估

- 赞成：
  - 默认布局不动，只加 opt-in，现有 `extern` 结构体不用改。
  - packed + 显式 pad + `#Align` 能覆盖 pack(1)/pack(n)/`_Alignas`，不必再发明 `#Pack(N)`（pack(n) 的洞写成字段即可）。
  - union 展开成 struct，语法零新增；`overlay` 补上「同一实例两种视图」，公开、限 C-layout。
  - 绑定面是任意 C ABI，不是 Win32 专条。
- 反对：
  - `overlay` 是类型双关，无 `unsafe`。范围收在 C-layout + 等 size，和今天 SDK 里 `_ptr_as_ref` 同类，只是用户绑 C 必须有一个口。
  - 字段 `#Align` 让 `#Packed` 结构里仍可能出现「对齐造成的洞」；这是刻意的，和 C `_Alignas` 在 packed 结构里的行为同类。
- 未决：
  - `overlay` 是否第一切片就做。不做的话 SDK 生成物仍可用 `_ptr_as_ref`，用户自己绑 union 只能选一种视图或 memcpy。
  - `overlay` 是否允许 `size_of(U) < size_of(T)`（前缀视图）。本条正文按 **相等** 写，更小的视图用 pad 补到 union size。
  - 字段 `#Align` 是否可第二切片（先 struct `#Packed` + `#Align`）。完整 FFI 两边都要。
  - Win64 下 packed 聚合 byval 与 MSVC 不完全一致时，是修 ABI 还是生成侧改 `Ptr`。实施时用 C 对照测试钉，对不齐记 TODO，不在规范里写 LLVM 细节。

## 决定

- 日期：2026-09-21
- 结论：实施
- 理由：默认布局不动；packed + `#Align` + 多 struct + `overlay` 覆盖 C pack / `_Alignas` / union 视图，不改 bnf、不引入语言 union。

## 规范要点

待实施后回写 §7.5.3 / §6.6.2 / §8.7.4 / §11.5 / 附录 A.3 / 附录 C / 附录 D；`size_of` 旁补 `align_of`。Win32 / 生成脚本不进 spec。

- 语法：`#Packed` 零参、`#Align(N)` 单参整数，均 `buildAnno`。`overlay:<U>(x T&) U&`、`align_of<T>() u64` 为 `#Builtin`。不改 `riu.bnf`。
- `#Packed`：无自然 pad；未叠 `#Align` 时 `align_of = 1`。
- `#Align(N)`：struct = 类型最小对齐 + 尾 pad；字段 = 偏移对齐到 `N`。`N` 为 2 的幂。
- C-layout 资格仍只看字段类型（§7.5.3.4）。packed/align 的 C-layout struct 可按值进 `extern`。
- 无 union 类型。等 size/align 的多份 C-layout struct 表示 union 视图；`overlay` 限 C-layout、size 相等、`align_of(U) ≤ align_of(T)`。
- `extern`：目标 C ABI。现行 Win64 分类用布局后的 store size 与 `align_of`。
- 不做：语言 `union`、bitfield、FAM、`#Pack(N)`、Zig 比特 packed、`stdcall`、可变参 `extern`、`Function` 跨 FFI、Win32 专条。

## 落地

2026-09-21：完整切片。LLVM packed StructType + 显式 pad；`#Align` 落到 alloca / byval / 全局；`size_of` / `align_of` / `overlay`；Win64 分类用布局后 size/align。回归：`layout_ok` / `diag_*`、`layout.test.ut`、`tests/projects/ffi_layout`。

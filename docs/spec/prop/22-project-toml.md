# 22 项目配置（`riu.toml` 多产物）

| | |
|---|---|
| 状态 | 实施中 |
| 开 | 2026-09-22 |
| 旧档 | §10.1.1：每个 `riu.toml` 一个产物。顶层 `name` / `version` / `entry` 与 `[lib]` 互斥；`[link].libs` / `lib_dirs` 项目级系统库。驱动现在把 `kernel32.lib` / `shell32.lib` 写死进每次链接。相关：[#9 包管理](9-pkg.md)、[#10 多目标](10-multi-target.md) |

不绑版本。不改 `riu.bnf`（配置不是语言语法）。落地时改 §10.1.1 与附录 D（E5xxx），并迁仓库内全部 `riu.toml`。

## 提议

一个 `riu.toml` 描述**一个项目**（身份）和它的**若干产物**（可执行文件 / 库）。项目名不再兼任输出文件名。链接声明从项目级 `[link]` 收到每个产物的 `external_links`，用路径前缀区分系统库和本项目文件。

其它 riu 项目 / 三方库**先不写**。消费方看到的是源码、静态库、还是带 `.ud` 的编译中间件，要另开提案定库形态；形态没定，依赖语法会绑死错误的分发单元。

规范示例（落地后写入 `sdk/riu/riu.toml`；现文件仍是旧 `[lib]`）：

```toml
name="riu"
version="1.0.0"

[library]
type="static"
lib_mod="riu"
external_links=[
    { path="//kernel32" },
    { path="//shell32" },
]
```

### 现状

| | 现行 |
|---|---|
| 项目 | 恰好一个产物：有 `entry` 则 exe，有 `[lib]` 则静态库，二者互斥（E5008） |
| `name` | 项目名 = `riu build` 参数 = 输出文件名（`build/<name>.exe` / `riu.lib`） |
| `[lib].type` | `"static"`；`"dynamic"` 直接 E5007 |
| `[link]` | 项目级 `libs`（无后缀）+ `lib_dirs` |
| 系统库 | 另硬编码 `kernel32.lib` `shell32.lib` |
| `[fmt]` | 已有 `line_width`（`riu format` 向上找 toml） |
| `riu build [<name>]` | 省略则取 toml `name`；写出则必须与 `name` 一致 |

不够用：SDK 要声明自己链了哪些系统库；一个仓库里 exe + lib 可以并存（各自编）；动态库的伴随 DLL 要能抄到输出目录。

### 顶层：项目身份

```toml
name="riu"
version="1.0.0"
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | 字符串 | ✅ | 项目身份（包名，给 [#9](9-pkg.md) 预留）。**不是**默认输出文件名 |
| `version` | 字符串 | ✅ | 版本。v1 只记录，不强制 semver 语法 |

不再有顶层 `entry`、`[lib]`、`[link]`。至少要有一个 `[library]` 或一条 `[[executable]]`。二者可以同时存在（各自编；互链等 ulib 形态另案）。

其它项目级段（不是产物）：

| 段 | 现状 | 本提案 |
|---|---|---|
| `[fmt]` | `line_width` 整数 | 保留，语义不变 |
| 远程 / 三方 / 其它 riu 项目 | 无 | 本提案不定义。先定库形态（源码？`.lib` + `.ud`？），再谈 #9 |

### 产物

每项目最多一个 `[library]`（普通 TOML 表，**没有** `[library.foo]`）。可执行文件用数组表 `[[executable]]`，可写零到多条。

产出文件名（`name` 字段，缺省见下）在同一文件里必须唯一，避免 `build/` 下互撞。

### `[[executable]]`

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | 字符串 | 否 | 输出文件名（无扩展名），兼 `riu build <name>` 的 id。缺省 = 顶层项目 `name`。Windows：`build/<name>.exe`。多条 `[[executable]]` 时每条都要能区分，缺省撞车 → 配置错误 |
| `entry` | 字符串 | ✅ | 含 `fn main` 的源文件，相对源根（§10.1.3）。规则同现行 `entry`（相对路径、不得逃出 `src/`） |
| `external_links` | 内联表数组 | 否 | 见下。缺省 = 空 |

没有 `lib_mod`、没有 `type`。

```toml
name="echo"
version="1.0.0"

[[executable]]
entry="main.ut"
```

`riu build` / `riu build echo` → `build/echo.exe`。

需要自定义文件名或多 exe：

```toml
[[executable]]
name="mytool"
entry="cli.ut"

[[executable]]
name="helper"
entry="helper.ut"
```

→ `build/mytool.exe`、`build/helper.exe`；`riu build mytool`。

### `[library]`

最多一份。与 `[[executable]]` 相同的 `name`、`external_links`，再加上库专用字段。没有 `entry`。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | 字符串 | 否 | 输出文件名（无扩展名）。缺省 = 顶层项目 `name` |
| `type` | 字符串 | 否 | `"static"`（缺省）或 `"dynamic"` |
| `lib_mod` | 字符串 | ✅ | 本库拥有的模块根，点分路径 `a.b.c` |
| `external_links` | 内联表数组 | 否 | 同 executable |

`lib_mod` 决定编进这个库的源：

- 命中包目录（`<src>/a/b/c/`）：该目录树下全部非 `*.test.ut` 的 `.ut`（含子包）。
- 命中文件模块（`<src>/a/b/c.ut`）：该文件，以及它 `use` 闭包里、仍属于本项目源根的模块。
- 同名文件与目录冲突仍走 §10.1.3.4。

SDK：`lib_mod="riu"` → 编 `src/riu/` 整棵（`riu.core` / `riu.io` / `riu.time` / `riu.platform.windows` …），与现行 lib 项目扫 `src/` 但对齐到模块根。`*.test.ut` 仍只归 `riu test`（§11.3.3.2）。

输出：

- `type="static"` → `build/<name>.lib`
- `type="dynamic"` → `build/<name>.dll` + 导入库 `build/<name>.lib`（Windows）

### `external_links`

每个元素是内联表：

```toml
external_links=[
  { path="//kernel32" },
  { path="//shell32" },
  { path="./lib/ffi_demo", dll="./lib/ffi_demo" },
]
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `path` | 字符串 | ✅ | 链接输入。必须带下面两种前缀之一。**不写**库后缀 |
| `dll` | 字符串 | 否 | 运行时动态库，前缀规则与 `path` 相同，同样不写后缀。有则链接完成后**复制**到该产物输出目录（与 exe / dll 同级）。不做 delay-load。系统目录里的 `//` 可省略（加载器自己找） |

`path` 两种前缀（无前缀、其它前缀 → 配置错误）：

| 前缀 | 含义 | 配置写法 |
|---|---|---|
| `//` | 系统库 | `//kernel32`：名字本身，无后缀、无 `lib` 前缀。交给该目标链接器按系统规则搜 |
| `./` | 本项目文件 | 相对**项目根**的目录 + 逻辑名，如 `./lib/ffi_demo`。指向的是 stem，不是已带后缀的文件 |

`path` / `dll` **不得**写出库后缀（`.lib` `.dll` `.so` `.a` `.dylib` 以及 `lib*.a` 那种完整文件名）。写出 → 配置错误；不要靠剥后缀兼容。

驱动按**当前链接目标**补文件名，对齐该目标链接器的惯用名（v1 落地 Windows；ELF / Mach-O 是同一套配置写法，实现随 #10）：

| 目标 | `path`（链接输入）试探，先存在的赢 | `dll`（要复制的运行时库）试探 |
|---|---|---|
| Windows / COFF | `<stem>.lib` | `<stem>.dll` |
| ELF | `lib<stem>.a`、`<stem>.so`、`lib<stem>.so` | `<stem>.so`、`lib<stem>.so` |
| Mach-O | `lib<stem>.a`、`<stem>.dylib`、`lib<stem>.dylib` | `<stem>.dylib`、`lib<stem>.dylib` |

`//` 不在项目树里找文件：Windows 传 `<name>.lib` 走默认 libpath；ELF / Mach-O 等价 `-l<name>`（链接器自己找 `lib<name>.so` / `lib<name>.a`）。一条都找不到 → 链接错误。

`dll` 只复制，不改链接方式：`path` 仍是传给链接器的导入库 / 静态库；`dll` 是跑起来要能找到的那份文件。目标已存在且内容相同可跳过；复制失败 → 非零退出。

链接一个产物时并入：

1. 它自己的 `external_links`；
2. 它依赖的库的 `external_links`（静态库传递：用户 exe 链 SDK 时带上 SDK 声明的 `//kernel32` 等）；
3. 编译器自带的 `riurt`（运行时，**不**写进 toml）。

现行写死的 `kernel32.lib` / `shell32.lib` 删掉。SDK 要链它们就写在 `[library].external_links`。普通用户项目只 `use riu.*`、自己没有 `external_links` 时，从 SDK 传递过来。

`[link].lib_dirs` 不再单独存在：项目内的库用 `./` 写到逻辑名；系统库走 `//` 的默认搜索路径。

### CLI

```
riu build              # 本项目全部产物：先 `[library]`（若有），再按声明顺序所有 `[[executable]]`
riu build echo         # 只编 `name="echo"` 的那条 `[[executable]]`
riu build riu          # 库项目：命中 `[library]` 的产出名（缺省即顶层 `name`）
```

- 参数匹配产物产出 `name`（某条 `[[executable]]`，或 `[library]`）。缺省时产出名 = 顶层项目 `name`，所以单产物项目 `riu build` / `riu build <项目名>` 仍都能用。
- 未知名 → 非零退出。exe 与库的产出名撞车 → 配置错误（v1 不搞 `--lib` / `--bin` 消歧）。
- `riu build --test` / `riu test` 仍按 §11.3 扫 `*.test.ut`，不在 toml 里再开 `[[test]]`。
- SDK：`riu build` 或 `riu build riu`（库产出名缺省 `riu`）。

同一 toml 里有库又有 exe 时先各自编完。exe 要不要、怎么链本项目的 `[library]`，等 ulib 形态定了再写，不在本提案用 `./build/…` 或新前缀凑。

### 输出布局

仍落在 `<projectRoot>/build/`：

| 产物 | 文件 |
|---|---|
| executable `name="echo"` | `build/echo.exe` |
| library static `name="riu"` | `build/riu.lib` |
| library dynamic `name="foo"` | `build/foo.dll`、`build/foo.lib` |

中间 obj / IR / `.cache` 布局不变（镜像 `src/`）。

SDK 自构建探测仍可用顶层 `name="riu"`（不必改成认 `lib_mod`）。

### 迁移

破坏。仓库内所有 `riu.toml` 随实施切片改写。对照：

现行：

```toml
name="echo"
version="1.0.0"
entry="main.ut"
```

本提案：

```toml
name="echo"
version="1.0.0"

[[executable]]
entry="main.ut"
```

现行 FFI：

```toml
name="ffi"
version="1.0.0"
entry="main.ut"

[link]
libs=["ffi_demo"]
lib_dirs=["lib"]
```

本提案：

```toml
name="ffi"
version="1.0.0"

[[executable]]
entry="main.ut"
external_links=[
    { path="./lib/ffi_demo", dll="./lib/ffi_demo" },
]
```

不保留旧字段双读。

### 不做

- 不定义三方库、其它 riu 项目、同 toml 产物互链的配置（`proj:` 之类前缀本提案不出现）。
- 不多于一个 `[library]`；不用 `[library.foo]`。
- 不在 toml 里写远程依赖 / 版本约束（#9）。
- 不按目标三元组分 `external_links`（#10；v1 Windows-first，`//` = 主机系统库）。
- `dll` 不做 delay-load（`/delayload:`）。
- 不把 `riu test` 变成 toml 产物。
- 不改源根约定（`src/` 存在则源根 = `src/`）。
- 不引入 `authors` / `description` / `edition`。

### 切片（实施时，不绑版本）

1. 读新 schema；迁仓库 toml（单 `[library]` 或一条 `[[executable]]`）；`//` + `./` 链接；`dll` 复制到输出目录；去掉硬编码 `kernel32`/`shell32`；`[fmt]` 不动。
2. 多条 `[[executable]]`；`riu build` 无参编全部；有 `[library]` 则先编库（仍不互链）。
3. `type="dynamic"`（产出 `.dll` + 导入库）。

## 评估

对照 Cargo（[`manifest`](https://doc.rust-lang.org/stable/cargo/reference/manifest.html) / [`targets`](https://doc.rust-lang.org/stable/cargo/reference/cargo-targets.html) / [`build-scripts`](https://doc.rust-lang.org/stable/cargo/reference/build-scripts.html)）。前提：riu 现在只能链 SDK + 系统（及本树 vendored 原生库），没有其它 riu 包。

- 赞成：单 `[library]`（对标 Cargo `[lib]`）、`[[executable]]` 数组（对标 `[[bin]]`）、链接写在 toml 里而不是 build.rs、`dll` 复制、测试继续走 `*.test.ut`。
- 反对（建议改 schema / 收范围，尚未改正文）：
  - `type="static"` 不要对标 Cargo `staticlib`（那是给 C 链的、带齐上游的 `.a`）。现行 `riu.lib` 更接近「本语言产物归档」，消费规则未定。`dynamic` 也不要对标 `cdylib`（C ABI）或 `dylib`（Rust ABI）。
  - ELF 试探先 `lib<stem>.a` 再 `.so`：与 Unix 链接器默认（先共享）相反，也和 rustc `-l foo` 默认 dylib 不一致。v1 只做 Windows 的话，ELF/Mach-O 表不要写死错误顺序；有 `kind` 再写。
- 已收：每项目最多一个 `[library]`，表名不加 `.name`；exe 用 `[[executable]]`。
- 未决（对照后仍要拍）：
  - `./` 是否保留：这不是三方 riu 包，是本树原生库（`examples/ffi` 的 `lib_dirs`）。严格「只 sdk/系统」可以先只留 `//`，ffi 等动态。
  - 要不要 `kind`（`static` / `dylib` / 以后 `framework`）。没有的话 Windows 上 `.lib` 静态和导入库分不清，只能靠文件内容。
  - 传递：SDK 的 `external_links`（含 `dll`）是否跟到最终 exe。Cargo 靠 sys crate 的 `rustc-link-lib` 传递。应跟，否则用户 exe 只 `use riu.*` 会缺 `kernel32`。
  - 数组顺序是否即链接器参数顺序（Windows 下有意义）。应是。
  - 产出 `name` 是否禁 Windows 设备名（`nul` / `con` / `aux`…）。Cargo/crates.io 禁。
  - Unix 上只复制 `.so` 不够，还要 rpath / `$ORIGIN`（归 #10，正文应提一句）。
- 明确不抄：`[package]` 套一层、`edition` / `rust-version`、`autobins` 按目录猜产物、`[[test]]` / `[[example]]` / `[[bench]]`、`[dependencies]` / workspace、`package.links`、`[profile]` debug/release 目录、`build.rs`、`[library.foo]` 多库。


## 决定

- 日期：2026-09-22
- 结论：实施
- 理由：项目身份与产物分离；链接声明收到产物上，用 `//` / `./` 前缀区分系统库和本树文件；SDK 的 `external_links` 传到用户 exe，去掉驱动里写死的 `kernel32`/`shell32`。
- 未决按评估「应」收：保留 `./`；v1 无 `kind`、只落地 Windows 补后缀；数组顺序 = 链接器参数顺序；产出 `name` 禁 `nul`/`con`/`aux` 等；Unix rpath 归 #10。不双读旧字段。

## 规范要点

待实施后写入 §10.1.1。草案即上文：顶层身份、单 `[library]`、`[[executable]]`、`external_links` 的 `//` / `./`（配置不写库后缀，驱动按目标补）、`dll` 只复制、CLI、输出路径、与 `[fmt]` 的边界。

- 语法：TOML。不是 `riu.bnf`。
- 语义：一个项目最多一个库、零到多条 exe；链接声明在产物上；`lib_mod` 划库的源范围。不定义三方 / 跨项目依赖。

## 落地

- 2026-09-22（`notes/0.23`）：切片 1 — 新 schema 解析、迁仓库 toml、`//`/`./` 链接与 `dll` 复制；诊断 `toml_*`。
- 2026-09-22（`notes/0.23`）：切片 2 — 无参 `riu build` 先库后全部 exe；回归 `toml_multi_exe`。
- 2026-09-22（`notes/0.23`）：切片 3 — `type="dynamic"` 产出 `.dll` + 导入库；回归 `toml_dyn_lib`。

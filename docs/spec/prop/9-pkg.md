# 9 包管理与远程依赖

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-13 |
| 旧档 | 无。本地 `pkg` / `.ud` 已落地。[#22](22-project-toml.md) 把三方 / 跨项目依赖留给本提案；库形态未定 |

不绑版本。不改 `riu.bnf`（配置不是语言语法）。落地时改 §10.1.1 / §10.4.5 与附录 D（E5xxx），并拆 `sdk/riu/`、迁仓库内消费 `riu.io` / `riu.time` 的 toml。

## 提议

无统一依赖仓库。依赖只有**钉死版本的源码**（一个含 `riu.toml` + `[library]` 的 riu 项目）。`.ud` / `.lib` 是本机构建缓存，不是下载物。

三种来源，一条依赖恰好一种：

| 来源 | toml | 源码在哪 |
|---|---|---|
| 工具链 SDK（**不含 core**） | `{ sdk = "<id>" }` | 安装树 `sdk/<id>/` |
| 本地目录 | `{ path = "..." }` | 该路径（不拷贝） |
| git | `{ git = "...", rev = "<commit>" }` | 调用方 `build/dependences/<name>/` |

`use` 暂不限制直接依赖：进了解析图的源码视为一块。同一 `name` 不得以不同钉死源 / `rev` 进同一图（避免两套符号）。

SDK 拆成独立项目，产物各一份静态库。**core 隐式**（始终加载，toml 禁止声明）。**stdlib 显式**（与 net、path、git 一样走 `[dependencies]`；core 仍不写进 toml）。

### 现状

| | 现行 |
|---|---|
| 三方 / 跨项目 | 无。[§10.4.5.2](../10-模块系统.md) 明确不解析 |
| SDK | 单项目 `sdk/riu/`，`lib_mod="riu"`，整棵 `src/riu/` 编进 `riu.lib` |
| `riu.io` / `riu.time` / `riu.platform.windows` | `extraSdkPackages` 写死，所有项目无 toml 也能 `use` |
| 用户链 SDK | 读 `sdk/riu/build/riu.lib`（工具链侧编好） |
| 构建写入 | 只写**本项目** `<projectRoot>/build/` |

#22 预留了顶层 `name` 当包名，但没有 `[dependencies]`。

不够用：stdlib / net 无法可选；不能引用隔壁目录或 git 上的 riu 库；git 源码若在检出里直接 `riu build`，会把产物写进缓存、也写不进只读的工具链 SDK。

### SDK 拆分

兄弟项目，各一份 `riu.toml`、各一份 `[library]`（#22：每项目最多一个库）。

| 目录 | 顶层 `name` | `[library].name` | `lib_mod` | 产物文件 |
|---|---|---|---|---|
| `sdk/core/` | `core` | `riu.core` | `riu.core` | `riu.core.lib` |
| `sdk/stdlib/` | `stdlib` | `riu` | `riu` | `riu.lib` |
| `sdk/net/` | `net` | `riu.net` | `riu.net` | `riu.net.lib` |

`sdk/net/` 只定规则，本提案不建空包。#12（正则 / `Command` / JSON）落地时按层归入 stdlib 或 `sdk/<id>`，用同一套 `{ sdk = ... }`。

stdlib 源树 = 现行非 core：`io.ut` / `time.ut` / `platform/`。`external_links`（`//kernel32` / `//shell32`）从现行 `sdk/riu/riu.toml` 挪到 stdlib。

```toml
; sdk/core/riu.toml
name="core"
version="1.0.0"

[library]
name="riu.core"
type="static"
lib_mod="riu.core"
```

```toml
; sdk/stdlib/riu.toml
name="stdlib"
version="1.0.0"

[library]
name="riu"
type="static"
lib_mod="riu"
external_links=[
    { path="//kernel32" },
    { path="//shell32" },
]
```

stdlib **依赖 core**，写法与普通项目相同：**不写** `{ sdk = "core" }`。core 由编译器注入（parent scope、隐式 `use riu.core.*` 不变）。

net（举例）显式依赖 stdlib：

```toml
name="net"
version="1.0.0"

[dependencies]
stdlib = { sdk = "stdlib" }

[library]
name="riu.net"
lib_mod="riu.net"
```

查找 `{ sdk = "<id>" }`：与现行 `findSdkPath` 同级，相对 riu.exe → `.../sdk/<id>/`（现行 `.../sdk/riu/src/riu/core` 改为 `.../sdk/core`）。

禁止 `{ sdk = "core" }`（core 已隐式）。hello-world 只 `println` 可以不声明 stdlib；`use riu.io` 则图中必须有 `stdlib`（本包声明，或传递依赖带进来）。

### `[dependencies]`

项目级段，不是产物字段。[#22](22-project-toml.md) 的 `[library]` / `[[executable]]` / `external_links` / `[fmt]` 不动。

```toml
name="echo"
version="1.0.0"

[dependencies]
stdlib = { sdk = "stdlib" }
net = { sdk = "net" }
utils = { path = "../utils" }
http = { git = "https://example.com/http.git", rev = "a1b2c3d4e5f6789012345678901234567890abcd" }

[[executable]]
entry="main.ut"
```

| 约束 | 规则 |
|---|---|
| 表键 | = 对方顶层 `name`（必须一致；v1 不改名） |
| 来源 | `sdk` / `path` / `git` 恰好一个。混写或缺来源 → 配置错误 |
| `sdk` | 字符串，工具链包目录名 |
| `path` | 相对**本项目根**（可绝对）。目标必须含 `riu.toml` 且有 `[library]` |
| `git` | 仓库 URL。检出失败把 git 的 stderr / 退出码原样带上，非零退出；不另发明文案绕过 |
| `rev` | git **必填**，完整 commit。不写 branch / tag |
| 形态 | 依赖必须是库项目；exe-only → 配置错误。v1 **不**写 `dir`：`riu.toml` 必须在仓库根 |

递归读每包自己的 `[dependencies]` 构成有向图。**同一 `name` 只能有一个钉死源**（同一 `sdk` id，或同一规范化 `path`，或同一 `git` URL + `rev`）。冲突 → 配置错误。

`use` 可写图中任一包的 `lib_mod` 前缀（含传递）。未进图的模块路径 → 与现在一样找不到。调用方不把依赖源扫进自己的 `src/`。

链接：各包按自己的 `[library]` 编成静态库；最终产物链接图中全部库，并传递 `external_links`（§10.1.1.9）。`riurt` 仍由编译器自带，不写 toml。

### 构建产物位置

原则：**本次 `riu build` 所在项目的 `build/` 是唯一写入树。** 源码位置和编译产物位置分开。清掉 `build/` = 清掉本图全部缓存（含 git 检出）。

根项目自己的最终产物、中间产物路径 **不变**（§10.1.2.1）：`build/echo.exe`、`build/riu.lib`、`build/src/…obj`、`build/tests/`。

依赖另两层，都在调用方 `build/` 下：

| 层 | 路径 | 内容 |
|---|---|---|
| git 源码 | `build/dependences/<name>/` | 按 `rev` 检出的树（仓库根即项目根）。**不写** obj / `.lib` / `.ud` |
| 依赖产物 | `build/deps/<name>/` | 该包的 `.lib` / `.dll`、obj、`.ud`、`.cache`。布局相对**该包项目根**镜像，与根项目 `build/` 对 `src/` 的规则相同 |

`<name>` = 包顶层 `name`（与 toml 表键相同）。依赖库文件：

`build/deps/<name>/<library.name>.lib`

例：调用方声明了 stdlib 与一个 git 包 `http`：

```
build/
├── echo.exe
├── src/                  ; 本项目中间产物（现行）
├── tests/                ; 现行
├── dependences/
│   └── http/             ; git 源码 only
└── deps/
    ├── core/
    │   └── riu.core.lib  ; 隐式 core 也落在这里
    ├── stdlib/
    │   └── riu.lib
    └── http/
        └── http.lib
```

按来源，**源码**从哪读、**产物**写哪：

| 来源 | 读源码 | 写产物 |
|---|---|---|
| 本项目（根） | 自己的 `src/` | 自己的 `build/`（#22） |
| `sdk` | 工具链 `sdk/<id>/`（不拷贝） | 见下：新鲜则直接链工具链产物，否则编进调用方 `build/deps/<id>/` |
| `path` | 该目录（不拷贝） | 调用方 `build/deps/<name>/`。增量按**对方源文件 mtime**（规则同本项目 `.cache`） |
| `git` | `build/dependences/<name>/` | 调用方 `build/deps/<name>/` |
| core（隐式） | 工具链 `sdk/core/` | 同 sdk：新鲜则直接链，否则 `build/deps/core/` |

**禁止**把本次构建的 obj / `.lib` / `.ud` 写入：

- git 检出（`build/dependences/` 内）
- path 指向的那个项目自己的 `build/`
- 工具链 `sdk/<id>/`（用户构建当只读；短路只**读**其已有 `build/`）

**sdk 短路（允许）**：工具链该包自己的 `build/<library.name>.lib` 相对其源码新鲜（按源 mtime / 现行 freshness）→ 链接器直接用这份，不在调用方 `build/deps/<id>/` 再编。不新鲜或缺失 → 从工具链源码编进 `build/deps/<id>/`。不改安装树。

git：已存在且 HEAD 即为该 `rev` 则跳过网络。检出当不可变；目录在但被改过 → 报错。clone / fetch / checkout 失败 → **随 git 报错**（透传，非零退出）。不进版本库。

该包**自己就是根**（`cd sdk/stdlib && riu build`，或 `cd ../utils && riu build`）时走 #22：产物在**自己的** `build/`。别人消费它时不读、不写它的 `build/`，在消费者 `build/deps/<name>/` 再编一份（隔离；两份缓存可以并存）。

`riu test` 仍只跑**当前根**的 `*.test.ut`，产出 `build/tests/`。不自动跑依赖的测试。

### CLI

`riu build` / `riu test` / `riu format` 子命令不变。构建根项目前：解析图 → 缺的 git 检出（失败随 git）→ 按依赖序：sdk 新鲜则直接链，否则与 path/git 一样编进 `build/deps/<name>/`（增量看源 mtime）→ 再编根产物。

不另开 `riu fetch`（v1）。失败（git 非零、冲突 `name`、依赖不是库）→ 非零退出。

### 不做

- 中心仓库 / 索引 / `riu publish`
- semver 区间、lockfile（`rev` 已钉死）
- `{ sdk = "core" }`、把 core 当普通依赖声明
- 浮动 branch / 仅 tag 不写 `rev`
- git `dir`（仓库内子目录）。v1 要求 `riu.toml` 在仓库根
- `[patch]`、dev-dependencies、workspace
- 用预编译 `.lib` 当下载物（源码才是依赖）
- `use` 仅限直接依赖（以后若要收，另改）
- 全局 git / 编译缓存（保持每项目 `build/`）
- 改 #22 产物字段；改 `riu.bnf`
- 本提案建空的 `sdk/net/`

### 切片（实施时，不绑版本）

1. `[dependencies]` 解析；`path` + `sdk`；产物写入调用方 `build/deps/<name>/`；core 隐式进图；增量按源 mtime；sdk 允许工具链新鲜产物直接链。
2. SDK 物理拆分（`sdk/core` + `sdk/stdlib`）；stdlib 显式；迁 examples / tests 的 toml；去掉 `extraSdkPackages` 特判。
3. `git` + `rev` → `build/dependences/<name>/`；失败随 git；同源冲突检测。无 `dir`。
4. 回写 §10.1.1 / §10.4.5 / 附录 D；用户文档。

## 评估

对照 Cargo（`[dependencies]` / git+rev / path）、Go modules（无必须的中心仓，但有代理与版本选择）、Zig（URL + 内容哈希）、CMake FetchContent（源码进构建目录）。前提：riu 没有 registry，也不做区间解析。

- 赞成：三种来源够用；`rev` 即钉死；core 最小、stdlib 可从 hello-world 拿掉；git 与编译产物都在调用方 `build/`，`rm -rf build` 一次清干净；不往工具链 / path 项目 / git 检出里写。
- 反对（已收进正文，不再挡）：
  - `dependences` 不是常见拼写；按你写的保留。
  - `use` 不限直接依赖：传递包也能 `use`。图简单，包边界更糊。已选。
- 已收：无 lockfile；无 `{ sdk = "core" }`；stdlib 显式；物理拆成 `riu.core.lib` / `riu.lib` / `riu.net.lib`；构建只写调用方 `build/`；git 失败随 git 报错；v1 无 `dir`；sdk 允许工具链新鲜产物直接链；path / sdk 增量按源 mtime。
- 未决：Unix 上 `build/deps/<name>/<library.name>.lib` 的文件名随 [#10](10-multi-target.md)，本提案不写死。
- 明确不抄：Cargo `[package]` 套一层、`edition`、crates.io、`Cargo.lock` 的区间求解、`target/` 全局配置、git submodule 当依赖机制、把 `.lib` 当包。

## 决定

- 日期：2026-09-22
- 结论：实施
- 理由：无仓库、三种钉死源码来源；core 隐式、stdlib 显式、SDK 物理拆分；调用方 `build/` 唯一写入（git 源码 `dependences/`、产物 `deps/`）；git 失败随 git；v1 无 `dir`；sdk 新鲜则直接链；增量按 mtime。

## 规范要点

已回写 §10.1.1（`[dependencies]`、产物路径）与 §10.4（SDK 拆分、core 隐式、stdlib 显式）。草案即上文。

- 语法：TOML。不是 `riu.bnf`。
- 语义：依赖 = 钉死源码的库项目；三种来源；调用方 `build/dependences/` 只放 git 源码，`build/deps/` 放依赖编译产物（sdk 新鲜则可直接链工具链侧）；`use` 见图不限直接边。git 失败随 git。v1 无 `dir`。增量按源 mtime。

## 落地

- 2026-09-22（`notes/0.23`）：阶段 1 — `[dependencies]` 解析；path 图；产物写入调用方 `build/deps/<name>/`；core 隐式。回归 `toml_dep_*` / `dep_path*`。
- 2026-09-22（`notes/0.23`）：阶段 2 — SDK 拆成 `sdk/core` + `sdk/stdlib`；stdlib 显式；去掉 `extraSdkPackages`。
- 2026-09-22（`notes/0.23`）：阶段 3 — `{ git, rev }` 检出到 `build/dependences/<name>/`；失败随 git；同源冲突 / dirty checkout。回归 `dep_git*`。
- 2026-09-22（`notes/0.23`）：阶段 4 — 回写 §10.1.1 / §10.4.5 / 附录 C/D / 用户文档；提案标已落地。

; 草案：yux 静态变量（全局 + struct 命名空间内）

# 草案：yux 静态变量

状态：**草案 / 讨论中**。日期：2026-05-26。
作用：把"全局非 const 变量 / 全局可变变量 / struct 命名空间内的静态字段 / 静态字段读写语法 / 初始化时机与 ABI"这一组决策固化为单一规范，作为修改 `docs/spec/04-基础语法.md` `docs/spec/07-结构体.md` `docs/spec/11-编译期注解.md` 与 `CURRENT.md` 实施计划的依据。

> 与 `DRAFT-const-eval.md` 配套：const-eval 管"编译期算得出的常量"，本草案管"运行期初始化的变量 / 可变量"。两者在全局位共用同一个 `letGlobal` 语法骨架，按注解档位分流。

涉及章节（预估）：§04（变量声明 / 全局 let）、§07（结构体 — 新增静态字段段）、§11（`#Static` 字段位扩 / `#Mut` 全局位）、附录 A（产生式）、附录 D（错码）。

---

## 1. 目标

- **放开全局 `let`**：允许非 `#Cval` 全局（默认 val / `#Mut` var）；初始化器走运行期，可调任意 fn、可含 struct 字面量
- **新增 struct 命名空间内静态字段**：`#Static FIELD T = expr`（默认 val）/ `#Mut\n#Static FIELD T = expr`（可变），mangling `<Mod>::<Struct>::FIELD`
- **读写语法**：`Type::FIELD`（读）/ `Type::FIELD = v`（写，仅 `#Mut`）；与 reflect Phase 4 已落地的 `ExprPathCallNode` 框架共用 parser 路径
- **初始化时机 ABI**：所有全局 / 静态变量的初始化器收集到 `_yux_global_init()` fn，由生成的 `main` shim 在用户 `main` 前调用；顺序按"模块加载顺序 + 同模块内声明顺序"
- **与 const-eval 协同**：初始化器若 const-evaluable，sema 优先走 const-eval 通路 emit LLVM Constant（零运行期开销）；否则降级运行期 init
- **不做**：泛型 struct 静态字段、`#Frozen` 全局（v1 用 `#Cval` 替代）、初始化器错误处理（return-based 错误也不允许，必须无错完成；失败语义见 §6）、静态字段的延迟 / lazy 初始化、跨模块循环依赖检测（v1 简化为按拓扑序，循环 → 编译错）

## 2. 全景模型

| 概念 | 写法 | 语义 | 备注 |
|---|---|---|---|
| 全局 val | `let G T = expr` | 运行期初始化，浅不可变 | **新**（现状仅 `#Cval let` 接受） |
| 全局可变 | `#Mut<NL>let G T = expr` | 运行期初始化，可变 | **新** |
| 全局可变（延后赋值） | `#Mut<NL>let G T` | 声明无 init，首次赋值前不可读 | DAA 见 §6 |
| struct 静态 val | `#Static<NL>FIELD T = expr`（struct body 内） | 命名空间静态字段，浅不可变 | **新** |
| struct 静态可变 | `#Mut<NL>#Static<NL>FIELD T = expr` | 命名空间静态字段，可变 | **新** |
| 静态读 | `Type::FIELD` | sema 解析为 GlobalVariable 取值 | 复用 `ExprPathCallNode` parser 路径，sema 分流 |
| 静态写 | `Type::FIELD = v` | sema 检 `#Mut` + 类型兼容 → store | **新**；非 `#Mut` → E3151 |

要点：

- **三档全局 let 共用 letGlobal 产生式**：注解决定档位 —— 无注解 = val，`#Mut` = var，`#Cval` = const（const-eval 通路）
- **struct 静态字段段独立于 `filedDecl`**：新增产生式 `staticFieldDecl`，必须带 `#Static`，可叠加 `#Mut`；与 `filedDecl` 在 structDecl body 内混合书写
- **静态字段无 `let` 关键字**：沿用结构体字段段"无 let"约定（[yux-syntax.md §2](../../../.claude/rules/yux-syntax.md)）
- **读写路径走 `Type::NAME` 而非 `instance.field`**：静态字段不挂在实例上；`obj.FIELD` 不解析（E3152）
- **初始化顺序确定**：按"模块拓扑序 → 同模块内 lexical 序"，跨模块循环 = E3153

## 3. 全局 let 放开

### 3.1 语法形态

当前（受 const-eval 草案 §3 影响）：

```yux
#Cval
let MAX i32 = 100              ; 仅 #Cval 档接受，且 RHS 仅 literal
```

目标：

```yux
let DEFAULT_CONFIG Config = load_default_config()   ; 全局 val，运行期 init
#Mut
let counter i32 = 0                                 ; 全局可变
#Mut
let pending Array<Job>                              ; 声明无 init，首次写前不可读
```

### 3.2 g4 改动

> **高风险，需用户确认**。

`letGlobal` 当前要求 `(SymbolEq literal)?`（且语义层只接受 `#Cval` 档 + 必须有 init）。改为：

- RHS 升 `literal` → `expr`（与 const-eval §3.2 同步改动 —— 两草案落地需协调）
- 接受 `#Mut` 档（已在 statementLet 落地）
- 接受无注解（默认 val）档

### 3.3 静态检查与错码

| 档位 | init 缺失 | RHS 形态 | 写入 |
|---|---|---|---|
| 无注解（val） | E3154（必须 init） | 任意 expr；优先 const-eval | 禁写（E3110 借既有） |
| `#Mut` | 允许（首次写前不可读，DAA 见 §6） | 任意 expr | 允许 |
| `#Cval` | E3114（既有） | 必须 const-evaluable（const-eval §3） | 禁写 |

`#Cval` / `#Mut` / `#Frozen` 三档互斥（既有，E3111）。

### 3.4 初始化器约束

- 不允许在初始化器内 `try / catch`（yux 无 `throw`，错误走返回值；全局 init 必须无错 —— 若调用的 fn 返回 fallible 类型，必须在 init 之前用 `#Const fn` 或纯 fn 包装到无错形态，否则 E3155）
- 允许调用任意 fn（不限 `#Const`），调用栈在 `_yux_global_init` 内展开

## 4. struct 静态字段

### 4.1 语法形态

```yux
struct Counter {
  value i32                       ; 实例字段（filedDecl，无 init）

  #Static
  DEFAULT_STEP i32 = 1            ; 静态 val 字段

  #Mut
  #Static
  total_created i64 = 0           ; 静态可变字段

  fn inc() {
    $.value = $.value + Counter::DEFAULT_STEP
  }

  #Static
  fn create() Self {
    Counter::total_created = Counter::total_created + 1
    Self {
      .value = 0
    }
  }
}
```

### 4.2 g4 改动

> **高风险，需用户确认**。

`structDecl` body 当前：`(filedDecl|LineEnd)* fnClean? (fn LineEnd | LineEnd)*`

新增 `staticFieldDecl` 产生式，与 `filedDecl` 并列：

```
staticFieldDecl:
    (buildAnnos+=buildAnno)+      ; 必须含 #Static，可叠 #Mut
    name=ID type
    (SymbolEq init=expr)?
    LineEnd
    ;
```

structDecl body 改为允许 `(filedDecl | staticFieldDecl | LineEnd)*`。

注解集合至少含 `#Static`，sema 层检；缺 `#Static` → 走 `filedDecl` 失败回退（E1xxx 既有）。

### 4.3 mangling 与符号

- LLVM GlobalVariable 名：`<Mod>::<Struct>::FIELD`（与 `#Static fn` 一致，复用 `mangler`）
- 链接性：默认 `internal`；跨模块访问按既有模块导出规则（导出符号 → `external`，private → 编译错 E3156）
- 泛型 struct 上**禁止** `#Static FIELD`（v1 不做；与 `DRAFT-static-fn.md` 泛型遗留同档）→ E3157

### 4.4 读写路径

读：

```yux
let n = Counter::DEFAULT_STEP    ; ExprPathCallNode 复用（reflect Phase 4 已落地路径）
```

sema 分流：若 LHS 是 struct 类型且 RHS 是已知静态字段名 → 当作 GlobalVariable load；否则按既有 enum-ctor / static-fn 分支。

写（仅 `#Mut`）：

```yux
Counter::total_created = Counter::total_created + 1
```

写形态需 g4 支持 `Type::NAME` 作为赋值 LHS。当前 `statementSet` LHS 接受什么待复核（见 §10.3）。

非 `#Mut` 字段被赋值 → E3151。

### 4.5 与 `Type.field`（reflect）的关系

reflect 草案 Phase 4 落地的 `Type::FIELD` 读 reflect rodata 节点（如 `Type::fields[N]`）。本草案的 `Counter::DEFAULT_STEP` 走用户声明的静态字段。两者在 sema 分流：

- LHS 是 `Type`（reflect 内置）→ reflect 路径
- LHS 是用户 struct 类型名 + 字段是 `#Static` 字段 → 本草案路径
- 其他 → 既有 static-fn / enum-ctor 分支

## 5. 初始化时机 ABI

### 5.1 `_yux_global_init` fn

编译器为每个模块生成 `_yux_global_init_<Mod>()` fn，内含本模块所有：

- 全局 `let G T = expr`（非 const-evaluable）
- struct 静态字段 `#Static FIELD T = expr`（非 const-evaluable）

main shim（编译器生成的 `main`）按拓扑序依次调用各模块的 `_yux_global_init_<Mod>()`，然后调用用户 `main`。

伪代码：

```
_yux_main_shim():
  _yux_global_init_<DepA>()
  _yux_global_init_<DepB>()
  _yux_global_init_<Main>()
  user_main()
  _yux_global_dtor_shim()   ; v1 留 hook，dtor 顺序倒序，实际 dtor 实现见 §5.4
```

### 5.2 const-eval 优先

sema 期先尝试 const-eval：若初始化器 const-evaluable（含 `#Const fn` 调用 / struct 字面量等，见 const-eval §3 / §4 / §5），则 emit LLVM ConstantInitializer，**不进入 `_yux_global_init`**；零运行期开销。

### 5.3 跨模块顺序

- 同模块内：按 lexical 声明顺序
- 跨模块：按 import 拓扑序（被依赖模块先 init）
- 循环依赖 → E3153

### 5.4 dtor / cleanup

v1 简化：

- 全局 `let G T` 持有 Rc/Heap/Array/String 等带 dtor 的类型 —— 保留生命周期到进程结束（不调 dtor，避免 atexit 顺序坑）
- `_yux_global_dtor_shim` 留空 hook，未来按需启用

非 atomic / 单线程假设（与 v1 整体一致）。

### 5.5 与 LLVM `@llvm.global_ctors` 的关系

`@llvm.global_ctors` 是 LLVM 的标准 ctor 机制：模块级 `appending` 数组 `[{i32 prio, void()*, i8*}]`，linker 跨编译单元合并，C runtime（ELF `.init_array` / Mach-O `__mod_init_func` / Win CRT / WASM `__wasm_call_ctors`）在 `main` 之前自动跑。

v1 **不直接用**，理由对照：

| 维度 | `@llvm.global_ctors` | `_yux_global_init` shim |
|---|---|---|
| 跨模块顺序 | 同 priority 内未定义（静态初始化顺序灾难） | 编译期拓扑序，确定 |
| 调试 | 栈底是 C runtime，源映射差 | shim 内显式调用，stack trace 直接 |
| 错误归属 | crash 在 main 前发生，无标准退出码 | shim 内可统一捕获，与 yux runtime 入口对齐 |
| 平台一致性 | 各平台 crt / linker 实现差异（WASM / 嵌入式尤甚） | 单一路径，全平台一致 |
| 与 C++ ctor 互混 | 同一 ctor 列表混排，顺序不可控 | yux init 与 C ctor 互不干涉（C 走 libc，yux 走 shim） |
| dead-strip / LTO | ctor 数组项偶被误删（需 `used`） | 普通函数调用，DCE 不动 |
| 作动态库被外部调 | 自动 init（外部直接 dlopen 即可） | 外部必须显式调 `_yux_global_init`（v1 不是问题，未来作动态库时再补） |

**结论**：v1 yux 拥有自己的 `main` 入口、单 binary、单线程、无 C++ 全局对象互操作场景 —— 走显式 shim 净赚（顺序确定 + 调试友好 + 跨平台一致），代价（被外部作库调时要 export init 入口）等真做动态库时再补适配层（emit 一条 `@llvm.global_ctors` 项，内部仍调 `_yux_global_init`）。

## 6. DAA（定性赋值分析）—— `#Mut` 全局延后赋值

针对 `#Mut\nlet G T`（无 init）：

1. 入口状态：G 未初始化
2. 写规则：`G = v` 标为已初始化
3. 读规则：读未初始化的 G → E3158
4. 分支汇合：所有分支都写过 → 已初始化；否则未初始化
5. 循环：循环体内的写不算入口已初始化
6. 函数出口：N/A（G 是全局，跨 fn）
7. 跨 fn：v1 不做跨 fn DAA —— 任何 `#Mut\nlet G T`（无 init）在**首次读所在 fn 入口前**必须保证有写。**v1 简化**：要求 `#Mut\nlet G T` 必须在同模块内某 `#Init fn`（待定）或 `_yux_global_init` 显式写一次；否则首次读 → E3158

> Open Issue：v1 是否完全禁掉"全局 `#Mut` 无 init"形态？倾向**先禁**（E3154），等用例驱动再放开。决议 [#1.A]。

## 7. 错码

| 码 | 含义 |
|---|---|
| E3150 | `#Static` 字段段缺 init（v1 必须有 init） |
| E3151 | 写入非 `#Mut` 静态字段 / 全局 val |
| E3152 | 通过实例访问静态字段（`obj.FIELD`） |
| E3153 | 跨模块全局初始化循环依赖 |
| E3154 | 全局 val 缺 init（与 #Mut 无 init 共码或拆码待定） |
| E3155 | 全局 init 内含 fallible 调用（try/catch 形态） |
| E3156 | 跨模块访问 private 静态字段（v1 无 `#Private`，占位） |
| E3157 | 泛型 struct 上 `#Static FIELD`（v1 禁） |
| E3158 | 读未初始化的 `#Mut` 全局 |

## 8. 不在范围

- 泛型 struct 上的 `#Static FIELD`（v1 禁，单独草案）
- 静态字段的 lazy / once-init 语义（v1 全部预初始化）
- atexit / 全局 dtor 调度（v1 全局对象不调 dtor）
- 多线程下的全局可变安全（v1 单线程假设）
- `#Frozen` 全局（v1 用 `#Cval` 替代）
- 跨模块循环依赖的自动断环（v1 直接报错）
- 跨 fn 的全局 DAA（v1 简化为 init fn 内单次写）

---

## 8a. 实现替代方案（多路径）

主表已选定 v1 实现路线，下表保留"另一条路"备查；与 const-eval §4.8 同体例。

| 点 | v1 选 | 备选 | 切换代价 |
|---|---|---|---|
| 全局 init ABI | 显式 `_yux_global_init_<Mod>` + main shim 调用（详 §5.5） | `@llvm.global_ctors` / `.init_array` | 中：选备选则跨模块顺序失控、调试栈不友好；未来作动态库时反而是优势，可在 v2 加适配层 |
| 静态字段读路径 parser | 复用 `ExprPathCallNode`（reflect Phase 4 路径），sema 分流 | 新增产生式 `exprStaticAccess: ID SymbolColonColon ID` | 中：新增产生式与现有 `Type::name(args)` 重叠，需解决 ANTLR4 歧义；复用更稳 |
| 静态字段写 LHS | `statementSet` LHS 扩到接受 `Type::NAME`（待复核现状） | 新增 `statementStaticSet` 单独 stmt | 低：复用 `statementSet` 是渐进改动；新 stmt 需 g4 + sema 双改 |
| `staticFieldDecl` 与 `filedDecl` 关系 | 独立产生式并列 | 扩 `filedDecl` 增 `(SymbolEq init=expr)?` + 注解决定档位 | 中：扩 `filedDecl` 让"实例字段也能写 init"形态成为语法合法（语义层拒绝），错误推迟；独立产生式让错误前移 |
| `#Mut` 全局无 init | 禁（E3154）  | 允许 + DAA（首读前必写） | 中：允许需要全局级 DAA，跨 fn 分析复杂；v1 禁更省事，[#1.A] 已记 |
| 跨模块 init 顺序 | import 拓扑序 + 循环 E3153 | lexical 顺序（按 build 输入文件序）/ 用户显式 `#Init` 注解定序 | 高：lexical 不可移植；显式 `#Init` 增语言面 surface；拓扑序对用户透明 |
| 全局对象 dtor | v1 不调，保留 hook | atexit 注册 / yux shim 倒序调 | 高：atexit 顺序与 ctor 反序不保证；自管倒序调与 main 退出路径耦合；v1 单线程无并发清理需求，先不开 |
| const-eval 优先分流决策点 | sema 期决定（const-evaluable → emit Constant，否则降级 runtime init） | codegen 期统一走 runtime init，由 LLVM `-O1` 的 `globalopt` 把"显然常量"转成 Constant | 中：依赖 LLVM pass 不稳（debug build 不跑 globalopt 时全部 runtime init，启动开销骤增）；sema 期决定可控 |
| 静态字段 mangling | `<Mod>::<Struct>::FIELD`（与 `#Static fn` 一致） | 加类型签名后缀（防同名）| 低：v1 同 struct 内字段名唯一，无需后缀；未来重载场景再加 |
| 泛型 struct 静态字段 | v1 禁（E3157） | 每实例化一份独立 GlobalVariable（与 `#Static fn` 泛型未通同档） | 高：需要 mono 化时机把握 + 跨翻译单元去重，留到泛型整体收口 |

## 9. 与其他草案 / 已落地特性的关系

- **`DRAFT-const-eval.md`**：本草案的"const-evaluable 优先"分流直接消费 const-eval 输出；两草案 g4 改动（letGlobal RHS 升 expr）协调一次落地
- **`DRAFT-static-fn.md`**（已落地）：复用 `ExprPathCallNode` 解析路径、`mangler` 命名规则、`#Static` 注解
- **`DRAFT-spec-reflect.md`**：reflect Phase 4 的 `Type::FIELD` 路径与本草案的 `Counter::FIELD` 在 sema 分流（§4.5）
- **`DRAFT-const-mut.md`**（已落地）：复用 `#Mut` / `#Cval` / `#Frozen` 档位语义

## 10. 迁移面

### 10.1 编译器（`src/`）

- `src/sema/sema_pass.cpp`：新增 `staticFieldDecl` visit 分支；`ExprPathCallNode` sema 分流加"静态字段读"路径
- `src/ast/ast_builder_decl.cpp::visitLetGlobal`：放开非 `#Cval` 档，调 const-eval 优先，否则降级运行期 init
- `src/ast/ast_builder_decl.cpp`：新增 `visitStaticFieldDecl`
- `src/compiler/compiler_globals.cpp`（新增 / 抽出）：`_yux_global_init` 收集与 emit
- `src/compiler/compiler_*.cpp`：`Type::FIELD` 读写 codegen（LoadInst / StoreInst on GlobalVariable）
- `src/analyzer/const_mut_checker.cpp`：扩静态字段写入档位检（复用既有 #Mut 校验）
- `src/ast/mangler.{h,cpp}`：扩 `<Mod>::<Struct>::FIELD` 命名（应已有路径，复核）

### 10.2 SDK / runtime（`sdk/`）

- 暂无；`_yux_global_init` 由编译器 emit，runtime 不需要新增入口
- 未来 dtor shim 启用时再加 `sdk/yux/runtime/global_dtor.yux`

### 10.3 语法（`src/yux.g4`）

> 改语法属于**高风险动作**，需用户确认；本节只列"要改什么"，不动手。

- `letGlobal`：RHS `literal` → `expr`（与 const-eval 草案 §3.2 协调）
- `structDecl` body：增 `staticFieldDecl` 并列
- 新增 `staticFieldDecl` 产生式（见 §4.2）
- `statementSet` LHS 是否已支持 `Type::NAME`？待复核；若不支持需扩

### 10.4 测试（`tests/`）

- `static_vars_global_val` / `static_vars_global_mut` / `static_vars_struct_val` / `static_vars_struct_mut`
- `static_vars_read_path` / `static_vars_write_path`
- `static_vars_const_eval_priority`（const-evaluable init 不进 `_yux_global_init`）
- `static_vars_cross_module_order`（拓扑序）
- `diag_static_vars_E3150` 至 `E3158` 各一例

### 10.5 规范文档（`docs/spec/`）

- `docs/spec/04-基础语法.md`：全局 `let` 节扩展三档（无注解 / `#Mut` / `#Cval`），明确 RHS 形态
- `docs/spec/07-结构体.md`：新增 §7.X 静态字段段（语法 + 访问路径 + 与实例字段对照）
- `docs/spec/11-编译期注解.md`：`#Static` 节扩"字段位"用途；`#Mut` 节扩"全局位"用途
- `docs/spec/12-spec.md`：相应条款翻转拒收
- 附录 A：letGlobal / staticFieldDecl 产生式同步
- 附录 D：追加 E3150–E3158
- `docs/spec/CHANGELOG.md` 顶部追加

### 10.6 用户教程（`docs/`）

- `docs/基础语法.md`：全局变量节扩展示例
- `docs/结构体.md`：新增"静态字段"节（与既有"静态函数 `#Static`"并列）
- `docs/构建注解.md`：`#Static` 节增"字段位"说明

---

## 决议日志

按讨论顺序追加，标 `[#编号]`。每次反复或修订也追加新条目，不要覆盖。

- **[#1.A]** `#Mut` 全局是否允许无 init —— 倾向 v1 先禁（E3154），用例驱动再放开。理由：全局 DAA 跨 fn 复杂，v1 不做；强制 init 把"未初始化"风险压在编译期
- **[#1.B]** 静态字段读写路径复用 `ExprPathCallNode`（reflect Phase 4 已落地的 parser 路径），sema 分流。理由：避免 g4 新增 `exprStaticAccess` 产生式与现有 `Type::name` 形态歧义；sema 层用已知信息（LHS 类型 / RHS 名是否为静态字段）即可分流
- **[#1.C]** 初始化时机走显式 `_yux_global_init` shim，不用 `@llvm.global_ctors`。理由：顺序可控、错误排查直接、与 yux 自带 runtime 入口一致
- **[#1.D]** const-eval 优先分流：若 init const-evaluable，直接 emit LLVM ConstantInitializer，跳过 `_yux_global_init`。理由：零运行期开销 + 与 `#Cval` 路径语义一致
- **[#1.E]** 全局对象 v1 不调 dtor。理由：atexit 顺序坑大、v1 单线程无并发清理需求；留 `_yux_global_dtor_shim` hook 备用
- **[#1.F]** 泛型 struct 上禁 `#Static FIELD`（E3157）。理由：与 `DRAFT-static-fn.md` 泛型遗留同档（BUGS #3）；v1 不开
- **[#1.G]** 全局 init 内禁 `try / catch`（E3155）。理由：yux 无 `throw`，错误走返回值；全局 init 失败无处归（main 未启），强制设计上无错完成
- **[#1.H]** struct 静态字段无 `let` 关键字，沿用字段段约定。理由：与既有 `filedDecl` 形态一致（`name Type`），减少视觉切换

---

## 定型与归宿

草案定型后按以下步骤拆分迁入正式文档：

1. **§10.5 列出的每个 spec 章节**逐条改写，引用本草案条目编号（如 [#1.B]）保留可追溯性
2. **`docs/spec/CHANGELOG.md`** 顶部追加一条，摘要 + 影响章节，日期为合并日
3. **附录 A / D** 按 §10 同步
4. **`CURRENT.md`** 的 Phase 列表从 §10 派生
5. 处置 `docs/spec/draft/DRAFT-static-vars.md`：在头部加一句「已落地，见 §N.M」并保留为历史档

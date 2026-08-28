; 草案：yux 编译期求值扩展（const-eval）

# 草案：yux 编译期求值扩展

状态：**已落地（Phase 1–5，2026-05-30）**。详见 §5.1.4 / §5.1.5 / §7.3.2 / §7.10.3 / §11.6.4 / §11.10 / 附录 D / CHANGELOG。
实施记录：[`docs/dev/const-eval-impl-log.md`](../dev/const-eval-impl-log.md)。

> 本草案承接 `DRAFT-const-mut.md` 的可变性档位体系，扩展其中 `#Cval` 档的**初始化器表达力**与**编译期求值能力**；不动可变性语义本身。

涉及章节（预估）：§04（变量声明）、§07（结构体字面量）、§11（编译期注解 `#Cval fn`）、附录 A / B（产生式 + 错码）。

---

## 1. 目标

- **放宽**全局 `#Cval` 初始化器：从纯 `literal` 升到**常量表达式**（含算术 / 位 / 比较 / 逻辑 / `#Cval` 名字引用 / **`#Const fn` 调用** / **struct 字面量**）
- **沿用** `#Const`（已在 `DRAFT-const-mut.md` §4 落地）作为编译期可求值标记，**不引新注解**；全局 / 静态成员位的 `#Const fn` 等价 C++ `constexpr`，成员位的 `#Const fn` 沿用既有"不改自己 / 不调非 #Const"语义
- **扩展 struct 字面量** `Self{...}`（构造函数已取消，`Self{...}` 是当前唯一构造路径）：LHS 从 `Self` 放宽到任意类型名 `Point{...}`；出现位置从"`#Static fn` 体内"放宽到"任意 const 上下文 + 任意 runtime 上下文"；**要求全字段公开**
- 反哺 reflect：`Type` / `Field` rodata 节点的初始化器走 const-eval 通路，不再走专门的 `compiler_*.cpp` ad-hoc emit
- **不做**：泛型 `#Const fn`、`#Const fn` 递归调用、`String` / `Array<T>` / `Rc<T>` / `Heap<T>` / `T&` 作为 const 值或 `#Const fn` 形参 / 返回类型（`String` 长期解：见 §4.6 + `DRAFT-str-view.md`）、`#Mut` 静态变量初始化器（独立 `DRAFT-static-vars.md`）、错误处理形态 `try / catch`（yux 无 `throw`，错误走返回值；§4.5 编译期"必定成功 or 编译失败"二态）

## 2. 全景模型

| 概念 | 写法 | 语义 | 备注 |
|---|---|---|---|
| 字面量 | `42` / `"hi"` / `true` / `null` | 直接 ConstantInt / ConstantFP / 字符串 rodata / ConstantPointerNull | 现状已支持 |
| const 名字引用 | `OTHER_CVAL` | sema 期解析为另一个 `#Cval` 的常量值 | 现状已支持 |
| const 算术表达式 | `1 + 2 * SIZE` | sema 期递归求值；溢出按 `#Cval` 整数语义检 | **新** |
| const 位 / 比较 / 逻辑 | `(FLAGS \| 1) >> 2`、`A == B`、`X && Y` | 同上 | **新** |
| `#Const fn` 调用 | `make_offset(1, 2)` | 调用 `#Const` 标记的纯函数，sema 期递归求值整个 body | **新**（注解沿用，求值通路新） |
| struct 字面量（扩展 LHS / 位置） | `Point { .x = 1<NL>.y = 2 }` 多行块 | LLVM ConstantStruct（const 上下文）/ runtime store（普通上下文） | **扩展**（基于已落地 `Self{...}`） |

要点：

- **求值阶段**：所有 const-expr 在 **sema 期**算出 IR 无关 ConstantValue；codegen 翻 LLVM Constant emit 全局，不走 IRBuilder runtime 路径
- **递归约束**：`#Const fn` body 内每条 stmt 必须 const；const struct 字面量每个字段初值必须 const
- **错码集中**：非 const 子表达式 / 非 `#Const` fn 调用 → 单一错码 `E3140`，错点定位到该子表达式
- **不引入"任意 fn 自动 const"**：必须显式 `#Const` 标，与 Rust `const fn` / C++ `constexpr` 标记式路线一致
- **`#Const` 是统一注解**：成员位（既有语义）"不改自己 / 不调非 #Const"；全局位 / 静态成员位（本草案新加 const-eval 通路）等价 C++ `constexpr` —— **同一注解、两个使用面**，无 `#Cval fn` 等新档

## 3. 全局 const 初始化器

### 3.1 语法形态

当前：

```yux
#Cval
let MAX i32 = 100              ; 仅 literal RHS
```

目标：

```yux
#Cval
let MAX i32 = 100              ; 现状仍合法
#Cval
let DOUBLE i32 = MAX * 2       ; 算术表达式
#Cval
let ORIGIN Point = Point {     ; struct 字面量（多行块，无逗号）
  .x = 0
  .y = 0
}
#Cval
let SIZE i32 = compute_size(MAX, DOUBLE)      ; #Const fn 调用
```

### 3.2 g4 改动

`letGlobal` RHS 从 `literal` 升到 `expr`：

```antlr
; 旧
letGlobal:
    (letAnnos+=letAnno)* Let name=ID type? (SymbolEq literal)? LineEnd ;

; 新
letGlobal:
    (letAnnos+=letAnno)* Let name=ID type? (SymbolEq expr)? LineEnd ;
```

> **改 g4 属高风险动作**，按 [RULES.md](../../../RULES.md) 须先与用户对齐再改。本草案要求该改动，落地前 review。

sema 在 `ast_builder_decl.cpp::visitLetGlobal` 内对 `expr` 走 const-eval 校验；非 const 子树报 `E3140`。

### 3.3 错误码

| 码 | 触发 |
|---|---|
| E3140 | const 上下文（全局 `#Cval let` RHS / `#Const fn` body / const struct 初值）含非 const 子表达式 |
| E3141 | `#Const fn` body 控制流形态超出白名单（含 `for` / `while` / 普通 stmt 序列） |
| E3142 | const struct 字面量字段非全公开（含 `#Private` 等访问修饰；v1 无该修饰故事实不触发） |
| E3143 | const 表达式溢出 / 除零 / 索引越界（sema 期可定的算术错误） |
| E3144 | `#Const fn` 被 const-eval 调用，但形参 / 返回类型不在白名单（`T&` / `Rc<T>` / `Heap<T>` / `Array<T>` / `String`） |

既有错码沿用：`E3110` / `E3111`（const_mut_checker 既有"#Const fn 不改写 / 不调非 #Const"）、`E3113` / `E3114`（全局 let type / init 缺失）。

## 4. `#Const fn` 作为 const-eval 入口

### 4.1 形态（沿用既有 `#Const`，无新注解）

`#Const` 注解形态见 `DRAFT-const-mut.md` §4；本草案不改其语法表面，**仅扩展可使用面 + 求值通路**。

```yux
#Const
fn add(a i32, b i32) i32 = a + b                    ; 自由函数：等价 C++ constexpr，可在全局 #Cval RHS 调用

#Const
fn make_point(x i32, y i32) Point {                  ; 同上，配 struct 字面量
  ret Point {
    .x = x
    .y = y
  }
}

extend Counter {
  #Const
  fn doubled() i32 = $.n * 2                        ; 成员：沿用既有"不改 $/参数"语义
}
```

两个使用面：

| 使用面 | 既有 #Const 语义 | 本草案新增 |
|---|---|---|
| 自由函数 / `#Static fn` | 不调非 #Const | sema 期可被 const-eval；可出现在全局 `#Cval let` / `#Const fn` body / const struct 初值的 RHS |
| 成员方法 | 不改 $ / 不改参数 / 不调非 #Const | 暂不主动求值（成员调用 receiver 多为 runtime 实例）；若 receiver 也是 const，理论上可 fold，**v1 不做** |

### 4.2 body 约束（自由函数位 const-eval 视角）

成员位约束沿用 `DRAFT-const-mut.md` §4.2，不重复。下表是**自由 / 静态成员位**额外要求（叠加在既有 #Const 校验之上）：

| 形式 | 允许 | 备注 |
|---|---|---|
| `= expr` 单表达式体 | ✅ | 推荐形态 |
| 单 ret 块体（多行） | ✅ | 见示例 A |
| 顺序 const let 块体（多行） | ✅ | 局部 `let` 初值必须 const；见示例 B |
| `exprOneLineIfElse`（两分支均单 expr，多行块） | ✅ | 求 cond 后挑分支递归，零成本；嵌套不限；见示例 C |
| `a if c else b`（Python 式三元） | ✅ | 同上 |
| `if c { ... } else { ... }` statement 形态（两分支均以 `ret <const expr>` 结尾，多行块） | ✅ | 见示例 D |
| `for` / `while` / `match`（除草案另行开放） / 普通 stmt 序列 | ❌ | E3141 |
| `try` / `catch` 错误处理（yux 无 `throw`，错误走返回值） | ❌ | E3141；理由见 §4.5 |
| 调用非 `#Const` fn | ❌ | **既有** E3111（const_mut_checker 已拦） |
| 触及 `#Mut` / `#Static` 变量 / 全局可变 | ❌ | **既有** E3110 / E3111 |

合法形态示例（**块 `{...}` 多行强制**，见 `yux-syntax.md` §7）：

```yux
; A. 单 ret 块体
#Const
fn double(n i32) i32 {
  ret n * 2
}

; B. 顺序 const let
#Const
fn area(w i32, h i32) i32 {
  let half_w i32 = w / 2
  let half_h i32 = h / 2
  ret half_w * half_h * 4
}

; C. exprOneLineIfElse —— 两分支单 expr 但块仍多行
#Const
fn abs(n i32) i32 = if n < 0 {
  -n
} else {
  n
}

; D. if statement + ret
#Const
fn sign(n i32) i32 {
  if n > 0 {
    ret 1
  } else {
    ret -1
  }
}
```

→ 既有 #Const 校验已覆盖大部分负面用例；本草案补的主要是"控制流形态"白名单（仅 `= expr` / 顺序 const let / if-expr / if-ret）。

### 4.3 形参 / 返回类型约束（v1 简化）

`#Const fn` 用于 const-eval 时，形参 / 返回类型限制为**值类型**：

- ✅ 标量（`i32` / `u64` / `bool` / `f64` / 字符 codePoint 等）
- ✅ const struct（全字段公开 + 字段类型递归满足约束）
- ✅ `#Builtin` rodata struct（如 `Type` / `Field`）
- ❌ `T&` / `Rc<T>` / `Heap<T>` / `Array<T>` / `String`（涉及 Block / 分配 / refcount，超出 rodata；ref 还涉及逃逸分析）

违反 → **E3144**（`#Const fn` 形参 / 返回类型不支持 const-eval）。

> `#Const` 注解本身不强制这些限制（成员位的 `#Const fn` 接 `T&` 参数是合法的）；只在**被 const-eval 调用站点**触发时检查。即：用 `#Const fn` 在 runtime 上下文调用仍按常规检查走。

### 4.4 控制流白名单（汇总）

见 §4.2 表 + 示例 A-D。要点：

- ✅ `exprOneLineIfElse`（两分支单 expr） / `exprIfElsePreValue`（Python 式三元）
- ✅ if statement 形态，两分支均以 `ret <const expr>` 终结
- ✅ 顺序 const `let` 序列 + 末尾 `ret`
- ❌ `for` / `while` / `match`（match 当前仅 enum；const enum match 可扩展，留草案外）
- ❌ 任何 stmt 副作用形态（赋值非局部 / 调用非 #Const）

注：上述合法形态的块 `{...}` 一律多行（yux-syntax §7），不存在"单行块"形态；`= expr` 单表达式体是块体的语法外特例。

实现复杂度：if 求 cond 后挑分支递归求值，与算术 expr 同级。无需 unroll / loop fixpoint 等机制。

### 4.5 错误处理（禁）—— 全局 const 不能含运行期失败路径

yux **无 `throw` 关键字**，错误通过**返回值传递**（fallible 函数返回 Result-like 类型，调用方用 `try { ... } catch e T { ... }` 接住）。`#Const fn` 用于 const-eval 时，body 禁出现错误处理形态：

| 形态 | 禁用理由 |
|---|---|
| 调用 fallible fn（返回类型为错误传播载体） | const-eval 不进入失败路径；调用方拿不到"错误返回值"分支可走 |
| `try { ... } catch e T { ... }` | 引入 catch 等于把"运行期错"概念塞进编译期；const-eval 无 catch 分支求值语义 |
| `?` 错误传播操作符（如已落地） | 同上 |
| 触发运行时 panic 的算术（溢出 / 除零 / 越界） | sema 期能算出来的，直接 **E3143**；算不出来的（含 runtime 子树）→ **E3140** |

→ **结论**：全局 `#Cval` 初始化器 / `#Const fn` body 是"必定成功 or 编译失败"的两态系统；不存在"运行期初始化失败"形态。这与 `#Mut` 全局变量（独立草案）形成对比 —— 后者初始化器走运行时通路，可含错误处理。

> 既有 `DRAFT-const-mut.md` §4.2 未明确禁 `try / catch`；本草案要求扩展 const_mut_checker：`#Const fn` body 内检出错误处理形态 → E3141。具体哪些 fn 算"fallible"待 SDK 错误约定收口（独立讨论），v1 简化为"#Const fn 内禁 `try / catch`"即可保证 const-eval 通路不含失败分支。

### 4.6 字符串形参 / 返回类型 Open Issue

当前 `String` = `Array<u32>` handle + Block（含 refcount / cap / data），不能进 rodata。但用户写 const fn 处理字符串（拼接、hash、格式化）需求合理。

候选方案：

| 方案 | 描述 | 评估 |
|---|---|---|
| (a) v1 直接禁 | `#Const fn` 不接字符串，用户绕开 | 简单；用户体验差 |
| (b) 引入 `Str` 类型 | 类比 Rust `&str`：`{ptr data, usize len}` fat pointer，不带 Block；字符串字面量天然 `Str`，可从 `String` 取 view | 优雅；引入新 builtin 类型 + 字面量推断规则 + `String↔Str` 转换 |
| (c) 编译器特例 | 字符串字面量在 const-eval 时按 rodata 处理，但类型仍是 `String` | hack；后续遇到从字面量传给非字面量函数会撞墙 |

**v1 选 (a)**；方案 (b) 单独起 `DRAFT-str-view.md`（涉及 SDK / String 类型 / 字面量推断，跨章节）。本草案在 §7 不在范围里登记。

### 4.7 残余 Open Issues

- 跨 `#Const fn` 调用栈深度限制（C++ 用 `__cpp_constexpr_in_depth`）：先不限，发现编译性能问题再加
- 整数溢出语义：sema 期检的话与 codegen 运行期 wrap 不一致 → 决议先按 trap（E3143），与运行期形成"一致失败"而不是"一致 wrap"
- `match` 在 const-eval 中是否可行（仅 enum 形态）：留下一阶段，与 `DRAFT-枚举.md` 收口后讨论

### 4.8 实现替代方案（多路径）

下列点 v1 选定的实现路线已写入主表，这里保留"另一条路"供后续如撞坑需切换时回看。

| 点 | v1 选 | 备选 | 切换代价 |
|---|---|---|---|
| `ConstantValue` 表示 | sema 自定义 variant（int / float / bool / null / struct），0 LLVM | 直接复用 `llvm::Constant*` 但禁 IRBuilder 调用 | 中等：选备选则 sema 重新拉 LLVM 依赖，违反 sema/codegen 分离协议（见 [sema-codegen.md](../../../rules/sema-codegen.md)），不推荐反向 |
| 非 const 子表达式错码 | 单一 E3140 + 错点定位到子表达式 | 细分（E3140a 非 const fn 调 / E3140b 含 `$` / E3140c 含全局非 #Cval / ...） | 低：错码细分可后续追加，不破坏 v1 测试 |
| `#Const fn` body 校验时机 | 复用 `src/analyzer/const_mut_checker.cpp` 扩白名单 | SemaPass 内单开 `ConstFnBodyChecker` pass | 中：单开 pass 与 `kMigratedCodes` 白名单冲突，需同步迁 |
| `#Const fn` 同参多次调用求值 | 每次求（无缓存） | sema 期 memoize `(fn, args) → ConstantValue` | 低：缓存可后置加，不影响正确性；v1 不做避免缓存键设计 |
| 整数溢出 | sema 期 trap（E3143） | sema 期 wrap，与运行期一致 | 中：选 wrap 则全局 const 可静默吃溢出，与 C++ `constexpr` 严格性不一致；保留 trap |
| 浮点求值 | 按 IEEE-754 binary32 / binary64 严格按目标三元组对齐 | 用 host `double` 求值 | 中：host 求值在交叉编译时与目标行为分歧；v1 严格对齐目标 |
| reflect 反哺迁移时机 | Phase 6 一次性迁完（见 §6） | 渐进迁（一个 `Type` 字段一个迁） | 高：渐进迁导致同一 `Type` rodata 半新半旧 emit 路径，调试痛；一次迁完代价集中但收敛快 |

## 5. struct 字面量扩展

### 5.1 当前形态（已落地）

构造函数已取消（详见 `docs/结构体.md` 末段 §"静态函数 #Static"）。当前 struct 构造唯一路径：在 `#Static fn` 体内写 `Self{...}` 字面量：

```yux
struct Point {
  x i32
  y i32
}

Point {
  #Static
  fn make(x i32, y i32) Point {
    ret Self {
      .x = x
      .y = y
    }
  }
}
```

形态硬约束：
- LHS 只能是 `Self`
- 必须在 `#Static fn` 体内
- 多行块强制（yux-syntax §7）
- 每字段 `.field = expr` 单行，**无逗号**
- 字段必须全填（不设默认值）

### 5.2 本草案扩展

放宽 LHS + 出现位置：

| 维度 | 现状 | 本草案 |
|---|---|---|
| LHS | 仅 `Self` | `Self` 或任意可见类型名 `Point` / `Counter` 等 |
| 位置 | 仅 `#Static fn` 体内 | 任意 expr 位置（全局 `#Cval` RHS / `#Const fn` body / 普通 fn body / 嵌套字面量字段值 ...） |
| 多行块格式 | 多行 + 无逗号 + 全字段 | **不变** |

放宽后示例：

```yux
struct Point {
  x i32
  y i32
}

#Cval
let ORIGIN Point = Point {              ; LHS = 类型名，全局 const 位
  .x = 0
  .y = 0
}

#Const
fn translate(p Point, dx i32, dy i32) Point {     ; 普通参数
  ret Point {
    .x = p.x + dx
    .y = p.y + dy
  }
}

fn run() {
  let p Point = Point {                 ; runtime 位
    .x = 1
    .y = 2
  }
}
```

### 5.3 出现位置矩阵

| 位置 | 允许 | 字段初值要求 | 备注 |
|---|---|---|---|
| 全局 `#Cval let` RHS | ✅ | 全 const | 走 const-eval 通路 emit ConstantStruct |
| `#Const fn` body / 返回 | ✅ | 全 const | 同上 |
| `#Static fn` 内 `Self{...}` | ✅ | 可 runtime | **现状**，未变 |
| 普通 fn body | ✅ | 可 runtime | 走 IRBuilder runtime 通路 |
| 嵌套（其它 struct 字面量的字段值） | ✅ | 上下文决定 | const 位字段值仍要 const |

### 5.4 字段约束

- **全公开**：struct 所有字段必须**无访问修饰**（v1 yux 没有 private 修饰，故"全公开"事实上对当前所有 struct 成立）；后续若引入 `#Private` 字段，含 `#Private` 字段的 struct 在**非内部上下文**不允许字面量构造 → E3142
- **`#Frozen` 字段**：允许在字面量里初始化（这就是它的初始化时机），后续写 `obj.field = expr` 仍报 E3109
- **`#Mut` 字段**：允许初始化
- **顺序无关 + 必须全填**：与现有 `Self{...}` 规则保持一致

### 5.5 g4 改动

`expr` 文法的 struct 字面量产生式当前 LHS 是否仅接 `Self` 需 review；若是，要扩到通用 `<TypeName>` 形态。多行块格式不动。

> **改 g4 属高风险动作**，先与用户对齐。

## 6. reflect 反哺

当前 `src/compiler/compiler.cpp::ensureReflectTypeGlobal` 手搓 LLVM ConstantStruct（嵌套 String / Array / Block sentinel）。const-eval 落地后：

- `Field { .name = "n" }` / `Type { .name = "Counter" <NL> .fields = [...] }` 直接用 struct 字面量形态在编译器内部**生成对应 AST**（或绕过 AST，直接调用 sema-期 const-eval API 输出 ConstantStruct）
- 简化 codegen，统一走"const expr → LLVM Constant"通路

收益：Phase 5 / 6 / 后续反射扩展不再每加一个反射字段就改 `compiler.cpp`，只改 base.yux 的 `Type` / `Field` 定义。

## 7. 不在范围

- `#Const fn` 递归 / 互递归调用（先非递归，深度无限制但无环）
- 泛型 `#Const fn`（如 `#Const fn make_pair<T>(a T, b T) Pair<T>`）
- const-eval 内闭包 / lambda
- `#Mut` 全局变量（独立 `DRAFT-static-vars.md`）
- struct 静态字段 `Counter::FIELD` 的声明形态（同上）
- `&str` 类只读字符串视图类型（独立 `DRAFT-str-view.md`，见 §4.6）
- const-eval 内 `match` / 错误传播（`throw` / `try` / `!`）
- `String` / `Array<T>` / `Rc<T>` 作为 const 值（涉及 Block 分配 / RC，超出 rodata）
- 任意 fn 自动 const 推断（坚持显式 `#Cval fn` 标）

## 8. 迁移面（粗估）

### 8.1 编译器（`src/`）

- `src/ast/ast_builder_decl.cpp`：`visitLetGlobal` 接受 expr，调 sema-期 const-eval 校验
- `src/analyzer/const_mut_checker.cpp`：扩 `#Const fn` body 校验 —— 自由 / 静态成员位检控制流白名单（仅 if-expr / 顺序 let / ret） + 禁 `throw` / `try` / `catch` / `?` 错误传播（E3141）
- `src/sema/const_eval.{h,cpp}`（新增）：递归求值器，输入 ExprNode，输出 `std::optional<ConstantValue>`（先用 IR 无关的 ConstantValue variant，可移交 codegen 翻成 llvm::Constant）
- `src/sema/sema_pass.cpp`：拦截 const 上下文，调 const_eval；非 const 报 E3140 / E3141
- `src/compiler/compiler_globals.cpp`（或现有全局 emit 处）：从 const_eval 拿 ConstantValue 翻 llvm::Constant emit 全局
- `src/compiler/expr/expr_ctor.cpp`：struct 字面量在 const 上下文走 const_eval；runtime 上下文仍走现有 IRBuilder 路径
- `src/compiler/compiler.cpp::ensureReflectTypeGlobal`：迁到 const-eval 通路

### 8.2 SDK / runtime（`sdk/`）

- `sdk/yux/base.yux`：评估把 `Type` / `Field` 的字段填充改成在 SDK 里写 `#Const fn make_type_<T>() Type` 的形态？（要求泛型 `#Const fn` → 暂不做，留下一阶段）
- 现阶段 SDK 无须改动；const-eval 是编译器内能力

### 8.3 语法（`src/yux.g4`）

> 改 g4 属高风险动作，须先与用户确认。

- `letGlobal` RHS：`literal` → `expr`
- struct 字面量产生式：通用化（若 `Self{...}` 之外形态尚未在 expr 文法里）
- `#Cval fn`：注解词汇已有，无需新增 token

### 8.4 测试（`tests/`）

新增前缀建议：`const_eval_*`（合法）/ `diag_const_eval_*`（错码）

- `const_eval_global_arith`：`#Cval let X = 1 + 2 * 3`
- `const_eval_global_struct_literal`：`#Cval let O Point = Point { .x = 0 <NL> .y = 0 }`（多行块）
- `const_eval_const_fn_basic`：`#Const fn add(a, b) = a + b` 被全局 let 调用
- `diag_const_eval_E3140`：全局 let 含非 #Const 函数调用
- `diag_const_eval_E3141`：`#Const fn` body 含 `for` 循环（自由 fn 位）
- `diag_const_eval_E3142`：含 `#Private` 字段 struct 在 const lit（v1 暂无修饰故占位用例）
- `diag_const_eval_E3143`：`#Cval let X i32 = 2147483647 + 1` 溢出
- `diag_const_eval_E3144`：`#Const fn` 形参为 `String` 被 const-eval 调用

### 8.5 规范文档（`docs/spec/`）

- `docs/spec/04-基础语法.md`：全局 `#Cval let` 节追加"初始化器为常量表达式"
- `docs/spec/07-结构体.md`：`Self{...}` 节扩展为通用 `<TypeName>{...}` 字面量
- `docs/spec/11-编译期注解.md`：`#Const` 节追加"全局 / 静态成员位的 const-eval 通路"（不动既有"成员位不改自己"语义）；`#Cval` 节追加"初始化器可调 `#Const fn` / 含 const struct 字面量"
- 附录 D：追加 E3140 / E3141 / E3142 / E3143 / E3144
- `docs/spec/12-spec.md`：相应条款（§12.x.y）翻转拒收
- 附录 A：产生式 letGlobal / structLiteral 同步
- `docs/spec/CHANGELOG.md`：顶部追加条目

### 8.6 用户教程（`docs/`）

- `docs/基础语法.md` 全局常量节扩展示例
- `docs/构建注解.md` `#Cval` 节增 `#Cval fn` 示例

---

## 决议日志

按讨论顺序追加，标 `[#编号]`。每次反复或修订也追加新条目，不要覆盖。

- **[#1.A]** 全局 `#Cval` 初始化器从 `literal` 升到 `expr`（含算术 / `#Const fn` 调用 / struct 字面量）：动机是反哺 reflect rodata emit + 用户写常量表显式表达需求；代价是 g4 改动 + sema-期 const-eval 框架
- **[#1.B]** **复用 `#Const` 注解**（既在 `DRAFT-const-mut.md` §4 落地），不新引 `#Cval fn`：成员位沿用既有"不改自己 / 不调非 #Const"语义；全局 / `#Static fn` 位本草案叠加 const-eval 通路，等价 C++ `constexpr`。**统一注解、两个使用面**，避免分裂
- **[#1.B.1]** 关于返回值类型是否要标 const：**不需要**。const-eval 是调用点驱动（call site 检查 callee 是否 #Const ∧ args 是否 const），返回值类型不带 const-ness，避免污染类型系统
- **[#1.C]** const struct 字面量要求"全字段公开"，v1 yux 无 private 修饰故事实对所有 struct 成立；引入 `#Private` 后再用 E3142 拦
- **[#1.D]** const-eval 走 sema 期，输出独立 ConstantValue（IR 无关），codegen 端翻 llvm::Constant：保持 sema/codegen 分离协议不破
- **[#1.E]** Open Issue：`#Const fn` body 是否允许 `if` 表达式 / 三元 / 顺序 let 序列；最简实现仅 `= expr` 单表达式体
- **[#1.F]** 不做：泛型 `#Const fn` / `#Const fn` 递归调用 / `String` `Array` `Rc` `Heap` `T&` 作为 const 值或 `#Const fn` 形参 / 返回类型 / `#Mut` 全局（最后一项推到 `DRAFT-static-vars.md`）
- **[#1.G]** 控制流白名单含 `if` 表达式形态（`exprOneLineIfElse` / `exprIfElsePreValue`）+ 顺序 const let + 单 ret；实现复杂度近乎为零（cond 算出后挑分支递归）。`for` / `while` / `match` 排除：`for` / `while` 无 sema-期 fixpoint；`match` 待 `DRAFT-枚举.md` 收口后再议
- **[#1.H]** **禁错误处理形态**（`try / catch`，yux 无 `throw`，错误走返回值）：编译期"必定成功 or 编译失败"二态；sema 期可算出的算术错 → **E3143** 直接编译失败；含 runtime 子树 → **E3140**。具体哪些 fn 算 fallible 待 SDK 错误约定收口；v1 简化为"#Const fn 内禁 `try / catch`"。这条与 `#Mut` 全局变量（独立草案，运行期初始化可含错误处理）形成对照
- **[#1.I]** `String` 形参 / 返回 v1 直接禁（方案 a）；不做编译器特例（方案 c）；长期解走独立草案 `DRAFT-str-view.md`（方案 b，引入 `Str` 类型类比 Rust `&str`）。理由：字符串字面量 rodata 形态本应能进 const，但 `String` 类型含 Block 头是阻碍，引入 `Str` 是跨章节决策（SDK / 类型系统 / 字面量推断），不宜挤在本草案内

---

## 定型与归宿

草案定型后按以下步骤拆分迁入正式文档：

1. **§8.5 列出的每个 spec 章节**逐条改写，引用本草案条目编号（如 [#1.A]）保留可追溯性
2. **`docs/spec/CHANGELOG.md`** 顶部追加一条，摘要 + 影响章节，日期为合并日
3. **附录 A / D** 按 §8 同步
4. **`CURRENT.md`** 的 Phase 列表从 §8 派生
5. 完成后归档 `docs/dev/const-eval-impl-log.md`
6. 本 DRAFT 头部加「已落地，见 §N」批注或删除

# 附录 C：术语表

本附录给出规范中使用的核心术语。出处指向首次定义或权威条款。

## C.1 词法 / 语法

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 行首列 | column 0 | §1.2 / §1.3 | 物理行的第 0 列；`LineComment` / `EmptyLine` 触发条件 |
| 行内注释 | line-end comment | §1.3 / `LineEndComment` | 代码后由空格 + `;` 起的注释 |
| 行注释 | line comment | §1.3 / `LineComment` | 行首列起的 `;...` 注释 |
| 标识符 | identifier | §1.4 / `ID` | Unicode 标识符 token |
| 字面量 | literal | §1.6 | 数值 / 布尔 / 字符串 / 码点 / `null` |
| 码点 | code point | §1.6.5 / `CODE_POINT` | `c'<ch>'`，类型 `u32` |
| 产生式 | production | §2.1 | EBNF / ANTLR 文法规则 |
| 关键字 | keyword | §A.1 | `yux.g4` 独立 lexer token |
| 上下文标识符 | contextual identifier | §A.2 | 内置类型名等非关键字保留名 |
| 注解 | build annotation | §11 | `#Name` 形态的编译期标签 |

## C.2 类型系统

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 类型档位 | type tier | §3.1 / §8.1.1 | 值 / 堆句柄 / 借用 / FFI 指针四档 |
| 值类型 | value type | §3.1 | 标量 / 用户 struct / `[T*N]`，按值复制 |
| 堆句柄 | heap handle | §3.1 / §8 | `Rc<T>` / `Weak<T>` / `Array<T>` / `String` / `StringBuilder` |
| 堆作用域句柄 | heap-scope handle | §3.1 / §8.3a / §9.5a | `Heap<T>`，单 owner、零 RC、作用域绑定 free |
| Heap<T> | heap pointer | §8.3a / §9.5a | 裸 `T*` payload，按值禁 copy / move，`Heap<T>?` 启用 nullable move |
| NRVO（A 档） | named return value optimization | §8.3a.4.1 | `ret heap_local` 折叠为分配 + 接管 + slot moved_out |
| nullable move（B 档） | nullable move | §8.3a.4.2 | 调用点把 `Heap<T>?` 实参 slot 改写为 null，避免 owner 复制 |
| 复合 move / 局部 move（C 档） | local / composite move | §8.3a.4.4 / §7.4.6.6 | `Heap<T>?` 左值赋值搬移；含 Heap 复合可空 slot 按值搬移 |
| 生命传染 | Heap-life propagation | §9.5a.1.4 | 含 Heap 子项的复合按 Heap 生命计；`Array<Heap<T>>` 不走 retain |
| 借用 | borrow | §3.2.3 / §8.6 | `T&`，非空指针，不参与 RC |
| FFI 指针 | FFI pointer | §3.3 / §9.7 | `Ptr`，等价 C `void*` |
| 名义类型 | nominal type | §3.4.1 | 用户 struct 按声明源判等 |
| 可赋值性 | assignability | §3.4.2 | 何种赋值合法（无隐式标量转换） |
| 隐式包装 | implicit wrapping | §3.6.4 | `T` → `T?` 自动构造 `Nullable<T>` |
| 内置泛型 | builtin generic | §3.3 / §9 | `Rc` / `Weak` / `Array` / `Nullable` / `Ref` / `Ptr` |
| 平凡结构体 | trivial struct | §7.4.2 | 不含 RC 字段，按位 memcpy |
| 非平凡结构体 | non-trivial struct | §7.4.2 | 至少一个 RC 字段，需字段级派生 |
| 枚举 | enum | §3.10 | 命名变体集合（和类型），值类型，名义判等 |
| 变体 | variant | §3.10.1 | enum 的命名分支；零参或带 tuple-style payload |
| payload | payload | §3.10.1 | variant 携带的数据载荷，按位置 tuple 形态 |
| 标量 | scrutinee | §3.10.7 / 草案 §5 | `match` 求值一次的被匹配对象 |
| 穷尽性 | exhaustiveness | §3.10.5 / 草案 §5.4 | match 覆盖全部 variant 或带 `else` 兜底 |
| 绑定 | binding | 草案 §5.2 | match 模式中按位置取出的不可变名 |
| 兜底分支 | else arm | 草案 §5 | `match` 中 `else` 分支，**应当**为最后一条 |
| discriminant | discriminant | §3.10.5 | enum 的内部 tag；v1 不暴露给用户代码 |

## C.3 表达式 / 语句

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 求值顺序 | evaluation order | §4.1 | 从左到右、自底向上 |
| 短路 | short-circuit | §4.5 | `&&` / `\|\|` 的短路语义 |
| 取址 | address-of | §4.7 / §8.6.4 | `& expr`，结果 `T&` |
| Nullable | nullable type | §3.6 / §9.8 | `T?`，解糖为 `Nullable<T>` |
| 安全访问 | safe-dot | §4.6 | `a?.field`，链式展开 |
| Null 兜底 | null-coalescing | §4.6 / `exprNullElse` | `a ?? b` |
| 表达式语句 | expression statement | §5.3 | `expr ';'? codeLineEnd` |
| 声明语句 | declaration statement | §5.1 | `var` / `val` / `cval` 三种 |
| 控制流 | control flow | §5.4 / §5.5 | `if` / `loop` / `break` / `ret` |
| 作用域 | scope | §5.7 | 词法块；析构按声明逆序 |

## C.4 函数

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 函数体 | function body | §6.1 | `fnExprkBody` / `fnBlockBody` |
| 表达式体 | expression body | §6.1 / `fnExprkBody` | `= expr` 形态 |
| 析构函数 | destructor | §6.1 / §7.4 / `fnClean` | `fn ~()`，位于字段段之后、其它 `fn` 之前 |
| 静态工厂 | static factory | §7.3 / §7.10 | `#Static fn` 返回 `Self`，构造唯一通道（v1 已删除构造函数形态） |
| 接收者 | receiver | §7.2.2 / §8.6.6 | `$`，隐式 `Self&` |
| 形参组 | param group | §6.2 / `fnParamGroup` | `a, b, c i32` 共享类型 |
| Turbofish | turbofish | §6.4 / §4.8 | `name:<T>(args)` 显式泛型 |
| 单态化 | monomorphization | §6.4 / §7.1.2.2 | 泛型按实参组合实例化 |
| 调用约定 | calling convention | §8.7 | callee-clean retain / move-return |
| extern | extern fn | §6.6 / §8.7.4 | 外部 C ABI 函数 |

## C.5 所有权 / RC

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 句柄 | handle | §8.2 | 堆句柄类型的指针字段 |
| Block | RC block | §8.2.1 | 堆上 `{ rc, payload }` 结构 |
| RC 头 | RC header | §8.2.1 | `{ strong: u32, weak: u32 }` |
| 强计数 | strong count | §8.2.2 | 外部强引用数 |
| 弱计数 | weak count | §8.2.2 | 弱引用数 + (1 if strong>0) |
| retain | retain | §8.2.2 / §8.7.1 | 计数 +1 |
| release | release | §8.2.2 / §8.7.1 | 计数 -1，归零析构 / free |
| upgrade | upgrade | §8.2.2.7 / §8.5 | `Weak<T> → Rc<T>?` |
| 哨兵 | sentinel | §8.2.3 | `0xFFFFFFFF`，retain / release 跳过 |
| callee-clean | callee-clean | §8.7.1 | 调用方传前 retain，被调方析构 release |
| 移动返回 | move-return | §8.7.2 | `ret box_expr` 注入 retain |
| fresh 句柄 | fresh handle | §8.3.3.2 / §8.8.2 | 已携带 +1 的临时，跳过 retain |
| 临时值 | temporary | §8.8 | 表达式 / 语句边界的未消费句柄 |
| 临时清单 | temp frame | §8.8.1 | 边界处释放未消费临时 |
| RC leak 检测 | RC leak counter | §8.8.4 | `_rc_block_count` / `rc_leak_count()` |
| retain-then-release | retain-then-release | §7.4.4 / §8.3.3.1 | 赋值的 RC 序，自赋值安全 |
| 字段级派生 | field-level derive | §7.4 | 按字段 retain / release |
| DAA | definite assignment analysis | §7.3.3 | 字段定性赋值分析（v1 退化为 `Self { ... }` 全字段覆盖规则） |
| same_ref | same_ref | §8.7.5.2 / §10 | 地址相等 builtin |
| ptr_of | ptr_of | §8.7.4.4 / §9.7.2.4 | 显式转 `Ptr` builtin |

## C.6 模块 / 注解

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| 模块 | module | §10 | 文件级编译单元 |
| 包 | package | §10.1.3 | 目录形式的模块容器 |
| 项目 | project | §10.1 | `yux.toml` 标记的根 |
| 入口 | entry | §10.1.1 | `yux.toml` 的 `entry` 字段 |
| 源根 | source root | §10.1.3 | `<projectRoot>/src/`，模块解析起点 |
| `pkg` 文件 | pkg file | §10.2.4 | 包目录的再导出清单 |
| 文件模块 | file module | §10.1.3 / §10.2.2 | 由单个 `.yux` 文件构成的模块 |
| 包模块 | package module | §10.1.3 / §10.2.4 | 由含 `pkg` / 多个 `.yux` 的目录构成的模块 |
| 通配导入 | wildcard import | §10.2.3 | `use a.b.*` 扁平化导入 |
| `_` 前缀私有 | underscore-private | §10.3.2 | 仅当前模块可见 |
| `yux.core` | yux core SDK | §10.4 | 内置 SDK 模块名 |
| `base.yux` | SDK base | §10.4 | 内置类型 / 内置函数声明源 |
| 构建注解 | build annotation | §11 | `#Name` 形态 |
| `#CompilerInner` | compiler-internal annotation | §11.2 | 编译器合成实现 |
| `#Test` | test annotation | §11.3 | 标记单元测试函数；仅 `*.test.yux` 中允许 |
| `#DraftLike` | draft-like annotation | §11.4 / §12.4 | 开放结构化匹配的 draft |
| 测试文件 | test file | §11.3.3 | 以 `.test.yux` 结尾；`yux test` 专属 |
| `yux test` | yux test command | §11.3.4 | 收集并执行项目下 `#Test` 函数的子命令 |

## C.6a draft（接口与约束）

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| draft | draft (interface contract) | §12 | 一组方法签名集合；显式 `:` 实现 + 可选 `#DraftLike` 结构化匹配 |
| 显式实现 | explicit impl | §12.2 | `Type : D1 + D2 { ... }` 块；穷尽且不多余 |
| 结构化匹配 | structural match | §12.4 | `#DraftLike` draft 按 §12.3 签名等价命中 |
| draft 边界 | draft bound | §6.4.4 / §12.4 | `<T : D1 + D2>` 内联约束 |
| 单态化分发 | monomorphized dispatch | §12.4.4.5 / §6.5 | v1 边界泛型的实例化 + 静态分发，无 vtable |
| orphan 规则 | orphan rule | §12.5 | `Type : D` 实现块只能在 `Type` 包或 `D` 包 |
| Rc forward | box forward | §12.6 | `Rc<U>` 上调 D 方法走 §8.6.7.3 自动解引用 + 归一 |
| `as_ref` | as_ref | §8.3.5.5 | `Rc<T> → T&` builtin |
| `copy_of` | copy_of | §12.7.3 | `T& → T` 显式拷贝 builtin |
| `Any` | any | §12.7.2 | 空签名集 draft；所有 owned 类型自动满足 |
| `ToString` | to_string draft | §12.7.1 | 内置 `fn to_string() String` 契约；不标 `#DraftLike` |

## C.7 编译期 / 实现

| 术语 | 英文 | 出处 | 简述 |
|---|---|---|---|
| ANTLR4 | ANTLR4 | §2.1 / `yux.g4` | 文法工具与左递归优先级 |
| 优先级 | precedence | §4.2 | 由 `expr` 分支顺序决定 |
| 单态化 | monomorphization | §6.4 | 泛型按实参实例化 |
| informative | informative | §index | 非规范性说明 |
| Open Issue | open issue | §index | 各章末待决条目 |
| `#CompilerInner` | — | §11.2 | 见 §C.6 |

## C.8 规范用语（normative）

| 词 | 英文 | 含义 |
|---|---|---|
| 应当 | shall / must | 规范要求；违反即不合规 |
| 不得 | shall not / must not | 规范禁止；违反即不合规 |
| 应该 | should | 强烈推荐；偏离需说明理由 |
| 可以 | may | 允许，无强制 |

§C.8.1 全文 *应当 / 不得 / 应该 / 可以* 的用语统一在 Phase 6 验收（CURRENT.md）。

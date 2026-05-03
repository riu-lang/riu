# 附录 D：诊断与错误码

> 权威来源：[`include/error_code.h`](../../include/error_code.h) 的 `ErrorCode::E####` 与
> [`src/diagnostic.{h,cpp}`](../../src/diagnostic.cpp) 的 `Diagnostic` / `DiagnosticEngine`。
> 本附录是规范化摘录，与上述源码冲突时**应当**修订本附录。

本附录约定编译器面向用户输出的诊断信息形态、错误码段位与全量码表。运行时诊断（panic、栈回溯等）不在本附录范围。

## D.1 诊断输出格式

### D.1.1 单条诊断

```
<file>:<line>:<col> [<code>] <severity>: <message>
  |
N | <源码行原文>
  |     ^
  = note: <说明>
  = help: <修复建议>
```

约束：

- `file` 为相对或绝对路径，由命令行入口决定；缺失时回退为 `line N` 或省略。
- `line` 为 1-based 行号；`col` 为 1-based **字节**列号（多字节字符按字节对齐，未来可能改为列宽对齐，*informative*）。
- `code` 形如 `EXXXX`，与 `ErrorCode::EXXXX.code` 同字面值；占位错误码 `E0000` 表示尚未挂码的位置，迁移完成后**不应**再出现。
- `severity` 取 `note` / `warning` / `error` 之一；每个错误码挂默认严重度，CLI `--warn` / `--allow` / `--deny` / `--Werror` 可在策略允许范围内调整。详见 D.5。
- `message` 为消息模板用具体参数渲染后的结果，模板见 D.3。
- 当 `file` + `line` 同时有效时**应当**输出源码片段；`col` 有效时**应当**追加插入符行。
- 一条诊断**可以**附带 0..N 条 `note` / `help`。

### D.1.2 多条诊断

实现采用**文件级聚合**：单个 `.yux` 文件内首次抛出 `YuxError` 仍会终止该文件的后续阶段，但**不会**让整个构建立即退出；驱动层会继续尝试编译其余模块，最后再以非零退出码结束。这样多个文件中的错误可以在一次编译中一起呈现，便于一次性看清问题面。

约束：

- 同一 `.yux` 文件内的多条诊断不强求；首条错误后该文件不再继续。
- 跨文件的诊断顺序**应当**与 `loadOrder()` 一致（主模块在前，导入模块按拓扑顺序）。
- 链接阶段在任一模块 codegen 失败时**应当**被跳过。

## D.2 错误码段位

| 段位  | 主题                | 语义层 | 备注                                                |
|-------|---------------------|--------|-----------------------------------------------------|
| E0000 | 占位                | —      | 兼容用，迁移完成后不再使用                          |
| E1xxx | 词法 / 文法（ANTLR）| §1 §2  | 由 `SyntaxErrorListener` 接管 lexer / parser 报错   |
| E2xxx | 语法 / AST 结构     | §2     | 文法接受但语义级 AST 构造拒绝；纯文法错误归 E1xxx   |
| E3xxx | 类型                | §3 §4  | 类型不匹配 / 符号查找 / 字段访问 / 泛型实参等       |
| E4xxx | 所有权 / 借用       | §8     | `T&` 借用合法性、`$` 字段 DA/DAA、构造器返回限制    |
| E5xxx | 模块 / 包           | §10    | `yux.toml` 解析、模块发现、循环依赖                 |
| E6xxx | 内置 / 调用         | §6 §9  | 函数 / 方法调用、`#CompilerInner`、内置类型方法     |

段内号**应当**按主题聚类、号段递增；段间**不得**复用号；新增码必须同步更新 D.3。

## D.3 错误码表

下表列出当前所有已分配的错误码及其消息模板。模板使用 `std::vformat` 占位符 `{}`，按 `throw YuxError(loc, ec, args...)` 站点（或 `SyntaxErrorListener::syntaxError`）提供的实参顺序填入。

> 表中的"模板"列与 [`include/error_code.h`](../../include/error_code.h) 的 `DEF_ERR(code, msg)` 字面同步；任何修改**应当**两处一并完成。

### D.3.1 E1xxx — 词法 / 文法（ANTLR）

由 `SyntaxErrorListener`（`src/syntax_error_listener.{h,cpp}`）接管 ANTLR 默认 ConsoleErrorListener，按 `recognizer` 是否为 `antlr4::Lexer` 派发：

| 码     | 模板                  | 触发                                  |
|--------|-----------------------|---------------------------------------|
| E1001 | `lexer error: {}`     | 词法 token 识别失败（如非法字符）     |
| E1002 | `syntax error: {}`    | 文法层 mismatched / no viable / 等     |

`{}` 为 ANTLR 给出的原始消息文本。每条 syntaxError **应当**即时渲染（不延后），随后的 E5010 充当"aborting due to N previous errors"汇总；**不得**因为存在 E5010 而抑制单条 E1xxx 输出。

### D.3.2 E2xxx — 语法 / AST 结构

| 码     | 模板 |
|--------|------|
| E2001 | `Weak<T>? is forbidden: Weak is natively nullable (upgrade returns Box<T>?)` |
| E2002 | `buildTypeWithRef: unknown typeWithRef alternative` |
| E2003 | `module \`{}\` is ambiguous: both \`{}.yux\` and \`{}/\` exist` |
| E2004 | `module alias \`{}\` conflicts with existing symbol` |
| E2005 | `Unknown build annotation \`#{}\`` |
| E2006 | `Function \`{}\` has no body; only \`#CompilerInner\` functions may omit the body` |
| E2007 | `Method \`{}.{}\` has no body; only \`#CompilerInner\` methods may omit the body` |
| E2008 | `wildcard alias \`{}\` is ambiguous, matched {}` |

### D.3.3 E3xxx — 类型

类型不匹配（E3001..E3025）：

| 码     | 模板 |
|--------|------|
| E3001 | `Type mismatch in +-/ operation: left is {}, right is {}` |
| E3002 | `Type mismatch in */% operation: left is {}, right is {}` |
| E3003 | `Type mismatch in &|^ operation: left is {}, right is {}` |
| E3004 | `Type mismatch in comparison: left is {}, right is {}` |
| E3005 | `Type mismatch in if-elif branches: {} vs {}` |
| E3006 | `Type mismatch in if-else branches: {} vs {}` |
| E3007 | `Type mismatch in one-line if-else: true branch is {}, false branch is {}` |
| E3008 | `Type mismatch in if-else expression: true branch is {}, false branch is {}` |
| E3009 | `Array fill literal type mismatch: literal is {}, but explicit type is {}` |
| E3010 | `Array fill element type mismatch: expected {}, got {}` |
| E3011 | `Array elements must have the same type: {} vs {}` |
| E3012 | `Array size mismatch: expected {}, got {}` |
| E3013 | `Array element type mismatch: expected {}, got {}` |
| E3014 | `Box type mismatch: expected Box<{}>, got {}` |
| E3015 | `Cannot assign {} to Nullable<{}>` |
| E3016 | `Weak<{}> 仅支持从 Box<{}> 或 Weak<{}> 构造` |
| E3017 | `T& local initializer type mismatch: expected {}&, got {}&` |
| E3018 | `T& copy-bind source type mismatch: '{}' is not {}&` |
| E3019 | `T& local initializer must be &expr or copy-bind from a T& variable` |
| E3020 | `Return type mismatch: function declares '{}', but expression has type '{}'` |
| E3021 | `Function declares return type '{}', but returns void` |
| E3022 | `Void function cannot return a value of type '{}'` |
| E3023 | `` `??` right side type {} doesn't match Nullable inner type {} `` |
| E3024 | `` Left side of `??` must be Nullable<T>, got {} `` |
| E3025 | `` `?.` requires Nullable<T> on the left, got {} `` |

符号查找（E3030..E3033）：

| 码     | 模板 |
|--------|------|
| E3030 | `Undefined variable: {}` |
| E3031 | `Variable not found: {}` |
| E3032 | `Symbol {} not found` |
| E3033 | `Array variable not found: {}` |

字段 / 结构体访问（E3040..E3046）：

| 码     | 模板 |
|--------|------|
| E3040 | `Struct {} has no field: {}` |
| E3041 | `Cannot access field on non-struct type: {}` |
| E3042 | `Cannot access private field '{}' of struct '{}'` |
| E3043 | `Cannot find struct declaration for field access` |
| E3044 | `` `?.` inner type {} has no struct decl `` |
| E3045 | `Cannot access member on non-struct type: {}` |
| E3046 | `Nested member access not yet supported` |

泛型参数缺失（E3050..E3057）：

| 码     | 模板 |
|--------|------|
| E3050 | `Box<T> missing inner type T` |
| E3051 | `Nullable type requires inner type` |
| E3052 | `Weak type requires element type` |
| E3053 | `Ref type requires element type` |
| E3054 | `Ref type missing inner type` |
| E3055 | `Array type requires element type` |
| E3056 | `Box type requires element type` |
| E3057 | `Invalid array type: missing element type` |

数组操作（E3060..E3068）：

| 码     | 模板 |
|--------|------|
| E3060 | `Array access requires at least one index` |
| E3061 | `Array access requires a variable` |
| E3062 | `Cannot index non-array type: {}` |
| E3063 | `Empty array literal not supported` |
| E3064 | `Array<T> initialization requires Array<T> expression or array literal` |
| E3065 | `Array assignment requires at least one index` |
| E3066 | `Array assignment requires a variable` |
| E3067 | `Array fill expression requires array type annotation with size` |
| E3068 | `Array fill expression requires array type annotation` |

运算符（E3070..E3078）：

| 码     | 模板 |
|--------|------|
| E3070 | `Cannot apply bitwise NOT to float type: {}` |
| E3071 | `Cannot apply logical NOT to non-bool type: {}` |
| E3072 | `Unknown unary operator` |
| E3073 | `Type '{}' does not support operator '{}' (method '{}' not found)` |
| E3074 | `Type '{}' does not support unary operator '{}' (method '{}' not found)` |
| E3075 | `Unsupported binary operation` |
| E3076 | `Unsupported comparison operation` |
| E3077 | `Unsupported mul/div/mod operation` |
| E3078 | `Weak<T> does not support == / != (v1 does not expose handle comparison)` |

字面量（E3080..E3082）：

| 码     | 模板 |
|--------|------|
| E3080 | `Unsupported literal type` |
| E3081 | `Unsupported literal type for array fill` |
| E3082 | `Unsupported literal type for global constant: {}` |

其他类型相关（E3090..E3099）：

| 码     | 模板 |
|--------|------|
| E3090 | `Unsupported dot expression` |
| E3091 | `Unknown expression type` |
| E3092 | `Unknown statement type` |
| E3093 | `Cannot assign to immutable variable: {}` |
| E3094 | `break statement not within a loop` |
| E3095 | `Type {} is not a Function` |
| E3096 | `Cannot get LLVM type for '{}'` |
| E3097 | `Cannot determine type for reference expression: no scope` |
| E3098 | `Unknown type '{}' for field '{}' of generic struct '{}'` |
| E3099 | `{}\n  {}`（用于 `compiler_types` 包装上下文：原消息 + 上下文行） |

### D.3.4 E4xxx — 所有权 / 借用

| 码     | 模板 |
|--------|------|
| E4001 | `T& borrow initializer must be &expr or an existing T& variable` |
| E4002 | `T& '{}' borrows root '{}' whose scope does not cover the borrow` |
| E4003 | `root '{}' cannot be reassigned while borrowed (§3.5)` |
| E4004 | `Cannot bind T& to non-local: {}` |
| E4010 | `field '$.{}' {} before initialization (§8.2)` |
| E4011 | `field '$.{}' must be initialized before {} (§8.2)` |
| E4012 | `field '$.{}' is not initialized at constructor exit (§8.2)` |
| E4013 | `cannot return $ from constructor (§8.3)` |

### D.3.5 E5xxx — 模块 / 包

| 码     | 模板 |
|--------|------|
| E5001 | `yux.toml not found in {}` |
| E5002 | `yux.toml is missing required field \`name\`` |
| E5003 | `yux.toml field \`name\` must be a string` |
| E5004 | `yux.toml field \`name\` must not be empty` |
| E5005 | `yux.toml \`lib\` must be a table` |
| E5006 | `yux.toml \`lib.type\` must be "static" or "dynamic"` |
| E5007 | `yux.toml \`lib.type="dynamic"\` not yet supported` |
| E5008 | `yux.toml \`[lib]\` and \`entry\` are mutually exclusive` |
| E5009 | `failed to parse yux.toml: {}` |
| E5010 | `syntax errors in {}` |
| E5011 | `circular module import: {}` |
| E5012 | `module not found: {} (expected file {})` |

### D.3.6 E6xxx — 内置 / 调用

调用解析与可见性（E6001..E6019）：

| 码     | 模板 |
|--------|------|
| E6001 | `module \`{}\` not found in package \`{}\`` |
| E6002 | `function \`{}\` not found in module \`{}\`` |
| E6003 | `Cannot call private function \`{}\` via package alias` |
| E6004 | `Cannot call private function \`{}\` via module alias` |
| E6005 | `module \`{}\` (alias \`{}\`) not loaded` |
| E6006 | `Cannot call private function '{}'` |
| E6007 | `Cannot call private method '{}' of struct '{}'` |
| E6008 | `Cannot use private struct '{}' in constructor` |
| E6009 | `Generic struct '{}' constructor requires explicit type arguments` |
| E6010 | `Generic function '{}' expects {} type args, got {}` |
| E6011 | `Generic struct '{}' expects {} type args, got {}` |
| E6012 | `Generic function '{}' expects {} params, got {} args` |
| E6013 | `Cannot infer type parameter '{}' for generic function '{}'` |
| E6014 | `Ambiguous call to '{}({})': {} overloads match; add type suffix to disambiguate:{}` |
| E6015 | `Unsupported call expression` |
| E6016 | `Unknown method '{}' for builtin type '{}'` |
| E6017 | `Unknown #CompilerInner function '{}'` |
| E6018 | `Cannot determine type argument for size_of` |
| E6019 | `Cannot determine LLVM type for '{}'` |

builtin 调用 / 类型实参数量（E6020..E6029）：

| 码     | 模板 |
|--------|------|
| E6020 | `ptr_from_addr expects 1 argument` |
| E6021 | `rc_leak_count expects 0 arguments` |
| E6022 | `_ptr_offset expects 2 arguments` |
| E6023 | `Cannot call private function '_ptr_offset' (SDK-only Ptr arithmetic)` |
| E6024 | `upgrade expects 1 type argument` |
| E6025 | `upgrade expects 1 argument` |
| E6026 | `{} expects 1 type argument` |
| E6027 | `{} expects {} argument(s)` |
| E6028 | `{}:<T&> requires a local var or &expr argument` |
| E6029 | `{}:<T> requires T to be Box/Weak/Array/String or U& (got '{}')` |

Array 内置方法（E6040..E6044）：

| 码     | 模板 |
|--------|------|
| E6040 | `at requires 1 argument` |
| E6041 | `Array.pop() requires an lvalue array` |
| E6042 | `Array mutation method '{}' requires an lvalue array` |
| E6043 | `set_len requires 1 argument` |
| E6044 | `push requires 1 argument` |

内置算子方法 arity（E6045）：

| 码     | 模板 |
|--------|------|
| E6045 | `{} requires 1 argument`（plus / minus / 等共用此模板，`{}` 为方法名） |

## D.4 与编译流程的关系

诊断由 `DiagnosticEngine::renderYuxError` 把 `YuxError`（携带 `SourceLocation` + `ErrorCodeDef`）渲染为 D.1 形态。当前各阶段的接入情况：

| 阶段                | 入口                                | 是否上诊断 |
|---------------------|-------------------------------------|------------|
| 词法 / 文法         | `SyntaxErrorListener`（替换 ANTLR 默认 ConsoleErrorListener） | 已接入（E1001 / E1002） |
| AST 构造            | `ASTBuilder`                        | 已接入（E2xxx 主体） |
| 借用检查            | `BorrowChecker` / 构造器 DAA        | 已接入（E4xxx）       |
| 类型检查 / 代码生成 | `Compiler*`                         | 已接入（E3xxx / E6xxx 主体） |
| 模块加载            | `Yux::loadModule` 等                | 已接入（E5xxx）       |
| 入口驱动            | `main.cpp::reportRuntimeError`      | 统一 catch，上诊断    |

## D.5 严重度策略与 CLI 开关

### D.5.1 默认严重度

每个错误码（`ErrorCode::EXXXX`）在 `include/error_code.h` 的 `DEF_ERR` / `DEF_WARN` / `DEF_NOTE` 宏中携带 `defaultSev`。当前所有码段（E1xxx..E6xxx）默认 `Error`；`Warning` / `Note` 段为后续 D.5 规划保留（如未使用变量、可疑类型转换等）。

### D.5.2 CLI 开关

驱动层 `yux` / `yux build` 接受以下选项（每项可重复多次）：

| 选项                 | 语义                                               |
|----------------------|----------------------------------------------------|
| `--warn=<code>`      | 把 `<code>` 的有效严重度改为 `warning`             |
| `--allow=<code>`     | 把 `<code>` 的有效严重度改为 `note`                |
| `--deny=<code>`      | 把 `<code>` 的有效严重度改为 `error`               |
| `--Werror`           | 全局把 `warning` 升级为 `error`                    |

应用顺序：默认 → 单码覆盖 → `--Werror` 升级。

### D.5.3 不可降级原则

默认严重度为 `error` 的码**不允许**通过 `--warn` / `--allow` 降级；尝试降级时驱动层**应当**忽略该覆盖并打印 `warning: cannot downgrade error code 'EXXXX' (default severity is error); --allow ignored` 的提示。原因：当前实现在 `Compiler::compile` 中遇到首个 `YuxError` 即抛出退出该文件，没有错误恢复机制；强行把 error 当 warning 会让后续 IR 在不一致状态下继续生成。

未来如果某些码引入错误恢复路径，**可以**把它们从默认 `Error` 重新挂为 `Warning`（在 `DEF_WARN` 中重新声明），从而获得"可降级 / 默认仍报错"的双重特性。

### D.5.4 路线图（*informative*）

- **高频场景 提示/修复建议**（原 v0.3 Phase 5）：类型不匹配修复建议、未声明标识符的拼写近似（Levenshtein ≤ 2）、`T&` 禁止位置、`Weak<T>?` / `Weak ==`、`extern` 签名不匹配等的 `note` / `help`。

## D.6 诊断回归测试

诊断测试用例位于 `tests/cases/diag_*.yux`，配对 `*.expected_err`。测试运行器规则：

- 编译必须以非零退出码结束；若 yux 意外编译成功（生成 `.exe`），用例失败。
- `expected_err` 中以 `;` 开头或全空白的行是注释/空行；其余每一非空行视为**子串断言**：该字符串必须出现在 yux 进程的 stderr/stdout（合并视图）中。
- 不强求行的出现顺序、不约束未列出的额外诊断；这一形态便于断言"必须命中"的关键内容（错误码、文件:行:列、关键消息片段），而对源码片段、对齐空白、绝对路径前缀保持鲁棒。

每个用例**应当**至少断言 `<basename>:line:col [Exxxx] error:` 这条诊断头，使得错误码、定位与消息文本三者中任一回归都能被捕获。

## D.7 Open Issues

- 列号当前按字节计算，多字节字符（中文、emoji）下的插入符位置可能与视觉列偏离；是否改为列宽 / Unicode 段分割尚未决议。
- E3099 采用包装上下文的双行模板，长期看应当替换为结构化 `note` 而非内嵌换行。
- E6045（内置算子 arity）目前用占位字段承载方法名，未来若按算子细分，可能拆为 E60xx 段独立码。

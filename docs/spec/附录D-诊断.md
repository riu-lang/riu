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
- `line` 为 1-based 行号；`col` 为 1-based **字符**列号（按 Unicode codepoint 计数，与 ANTLR `getCharPositionInLine() + 1` 同源）。一个 CJK 字 / 一个 emoji 计 1 列；BMP 外的码点（如组合表情 + 变体选择子）按各自的 codepoint 数计列。
- 源码片段下的插入符（`^`）按**显示列宽**对齐：插入符前的 padding 不再是单纯的列号空格，而是按源码行中各 codepoint 的视觉宽度展开（CJK / 全角 / 常见 emoji 计 2 列宽，组合标记 / 零宽字符计 0 列宽，Tab 原样保留以让终端按相同 tab stop 扩展）。这样 `^` 在等宽终端中始终落在出错字符的正下方，不受多字节字符前缀影响。
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
| E6xxx | 内置 / 调用         | §6 §9  | 函数 / 方法调用、`#Builtin`、内置类型方法     |
| E7xxx | 错误处理 / panic    | §6 §8  | `#Fallible` / `!` / try-catch / `#NoReturn` / `panic` 边界（详见 DRAFT-错误.md / 待 spec 落地后补 §引用） |
| E11xx | draft / 接口        | §12    | draft 实现穷尽性 / `#DraftLike` 误用 / orphan / 边界 |

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
| E2001 | `Weak<T>? is forbidden: Weak is natively nullable (upgrade returns Rc<T>?)` |
| E2002 | `buildTypeWithRef: unknown typeWithRef alternative` |
| E2003 | `module \`{}\` is ambiguous: both \`{}.yux\` and \`{}/\` exist` |
| E2004 | `module alias \`{}\` conflicts with existing symbol` |
| E2005 | `Unknown build annotation \`#{}\`` |
| E2006 | `Function \`{}\` has no body; only \`#Builtin\` functions may omit the body` |
| E2007 | `Method \`{}.{}\` has no body; only \`#Builtin\` methods may omit the body` |
| E2008 | `wildcard alias \`{}\` is ambiguous, matched {}` |
| E2009 | `Function \`{}\` cannot return \`T&\`; only \`#Builtin\` baked builtins may have a reference return type (spec §8.9)` |
| E2010 | `Cannot call mutating method \`Array.{}\` on \`{}\`: it has an active borrow (spec §8.4.2.5)` |
| E2011 | `Build annotation \`#{}\` is not allowed on this declaration (only \`fn\` accepts it)` |
| E2012 | `\`#Test\` function \`{}\` must have signature \`fn {}(): void\` (no params, no return type, must have body)` |
| E2013 | `\`#Test\` and \`#Builtin\` cannot both be applied to function \`{}\`` |
| E2014 | `\`#Test\` is only allowed in \`*.test.yux\` files; \`{}\` is not a test file` |
| E2015 | `draft bounds (\`: D\`) are only allowed at declaration sites (fn/struct/draft generic params); not at type references or call-point turbofish` |
| E2016 | `Type alias \`{}\` forms a cycle (recursive without indirection)` |
| E2017 | `Type alias name \`{}\` conflicts with existing {} \`{}\`` |
| E2018 | `Duplicate variant \`{}\` in enum \`{}\`` |
| E2019 | `Unknown enum \`{}\` in constructor \`{}::{}\`` |
| E2020 | `Enum \`{}\` has no variant \`{}\`` |
| E2021 | `Enum variant \`{}::{}\` expects {} payload arg(s), got {}` |
| E2022 | `match scrutinee must be an enum type, got \`{}\`` |
| E2023 | `non-exhaustive match on enum \`{}\`: missing variant(s) {}` |
| E2024 | `duplicate variant \`{}::{}\` in match arms` |
| E2025 | `\`else\` arm must be the last arm in match` |
| E2026 | `match pattern for \`{}::{}\` expects {} binding(s), got {}` |
| E2027 | `duplicate binding \`{}\` in match pattern \`{}::{}\`` |
| E2033 | `\`#TestIsolate\` on \`{}\` requires a sibling \`#Test\` annotation (isolation modifies how a \`#Test\` runs; it is not a standalone marker)` |

### D.3.3 E3xxx — 类型

类型不匹配（E3001..E3027）：

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
| E3014 | `Rc type mismatch: expected Rc<{}>, got {}` |
| E3015 | `Cannot assign {} to Nullable<{}>` |
| E3016 | `Weak<{}> 仅支持从 Rc<{}> 或 Weak<{}> 构造` |
| E3017 | `T& local initializer type mismatch: expected {}&, got {}&` |
| E3018 | `T& copy-bind source type mismatch: '{}' is not {}&` |
| E3019 | `T& local initializer must be &expr or copy-bind from a T& variable` |
| E3020 | `Return type mismatch: function declares '{}', but expression has type '{}'` |
| E3021 | `Function declares return type '{}', but returns void` |
| E3022 | `Void function cannot return a value of type '{}'` |
| E3023 | `` `??` right side type {} doesn't match Nullable inner type {} `` |
| E3024 | `` Left side of `??` must be Nullable<T>, got {} `` |
| E3025 | `` `?.` requires Nullable<T> on the left, got {} `` |
| E3026 | `` String template interpolation requires type implementing ToString, got '{}' (impl `Type : ToString { fn to_string() String { ... } }`) `` |
| E3027 | `Type mismatch in match arms: expected {}, arm produces {}` |

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
| E3050 | `Rc<T> missing inner type T` |
| E3051 | `Nullable type requires inner type` |
| E3052 | `Weak type requires element type` |
| E3053 | `Ref type requires element type` |
| E3054 | `Ref type missing inner type` |
| E3055 | `Array type requires element type` |
| E3056 | `Rc type requires element type` |
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

元组（E3100..E3102）：

| 码     | 模板 |
|--------|------|
| E3100 | `Tuple index {} out of range for type '{}' (size {})` |
| E3101 | `Tuple destructure expects type tuple, got '{}'` |
| E3102 | `Tuple destructure arity mismatch: {} names vs tuple size {}` |

const-mut（E3104..E3111；引入自 [draft/DRAFT-const-mut.md](draft/DRAFT-const-mut.md)，落地章节 §5.1.5 / §6.1.2a / §6.2.2a / §7.1.4 / §11.6 / §11.7 / §11.8）：

| 码     | 模板 |
|--------|------|
| E3104 | `Local \`cval\` initializer must be a constant expression: {} (DRAFT-const-mut §3.3; allowed: numeric/bool/null/string literals, references to declared \`cval\`, and arithmetic / bitwise / comparison / logical combinations thereof)` |
| E3105 | `Unknown parameter annotation '#{}' (only '#Frozen' is supported on parameters; DRAFT-const-mut §5.1)` |
| E3106 | `Cannot write field '{}' of \`#Frozen\` parameter '{}' (DRAFT-const-mut §5.3)` |
| E3107 | `Cannot pass \`#Frozen\` value '{}' to mutable parameter '{}'; use copy_of to obtain an owned copy (DRAFT-const-mut §5.4)` |
| E3108 | `Unknown or duplicate field annotation '#{}' (only '#Val' and '#Frozen' are supported on fields, mutually exclusive; DRAFT-const-mut §6.1)` |
| E3109 | `Cannot write field '{}' marked '#{}' outside the constructor of struct '{}' (DRAFT-const-mut §6.2)` |
| E3110 | `\`#Const fn\` '{}' cannot {}: {} (DRAFT-const-mut §4.2)` |
| E3111 | `\`#Const fn\` '{}' cannot call non-\`#Const\` function '{}' (DRAFT-const-mut §4.2.4)` |

let-unify（E3112..E3116；引入自 [draft/DRAFT-let-unify.md](draft/DRAFT-let-unify.md)，落地章节 §5.1.1 / §11.9 / §11.10）：

| 码     | 模板 |
|--------|------|
| E3112 | `Unknown \`let\` annotation '#{}' (only '#Mut', '#Frozen', '#Cval' are supported on \`let\`; DRAFT-let-unify §3.4)` |
| E3113 | `\`let {}\` requires a type or initializer (DRAFT-let-unify §3.4)` |
| E3114 | `\`let {} <type>\` requires an initializer (use \`#Mut let\` for deferred assignment; DRAFT-let-unify §3.4)` |
| E3115 | `Annotations '#{}' and '#{}' are mutually exclusive on \`let\` (DRAFT-let-unify §3.4)` |
| E3116 | `Global \`let {}\` requires \`#Cval\`: only compile-time constants are allowed at global scope (DRAFT-let-unify §3)` |

const-eval（E3140..E3144；引入自 [draft/DRAFT-const-eval.md](draft/DRAFT-const-eval.md)，落地章节 §5.1.4 / §5.1.5 / §7.3.2 / §7.10.3 / §11.6.4）：

| 码 | 模板 |
|---|---|
| E3140 | `Global \`let {}\` initializer is not a constant expression (DRAFT-const-eval §2; allowed: literals, references to declared \`#Cval\` globals, and arithmetic / bitwise / comparison / logical combinations thereof)` |
| E3141 | `` \`#Const fn\` '{}' body contains disallowed control-flow form: {} (DRAFT-const-eval §3; allowed: \`= expr\`, \`{{ ret expr }}\`, sequential \`#Cval let\`, \`exprOneLineIfElse\` / \`exprIfElsePreValue\`, if-statement with \`ret\` in both branches) `` |
| E3142 | `struct literal for '{}' requires all-public fields (DRAFT-const-eval §5.4; v1 placeholder — no \`#Private\` modifier exists yet)` |
| E3143 | `Constant expression evaluation error in global \`let {}\` (overflow, division by zero, or unsupported operation; DRAFT-const-eval §4.7)` |
| E3144 | `` \`#Const fn\` '{}' cannot be invoked in a constant expression: {} type '{}' is not in the const-eval whitelist (DRAFT-const-eval §4; allowed: scalar integer / float / bool) `` |

static-vars（E3150..E3157；引入自 [draft/DRAFT-static-vars.md](draft/DRAFT-static-vars.md)，落地章节 §5.1.4 / §7.11 / §11.9 / §11.11）：

| 码 | 模板 |
|---|---|
| E3150 | `#Static field '{}' requires an initializer (DRAFT-static-vars §4.3; v1 static fields must be initialized at declaration)` |
| E3151 | `Cannot write to non-#Mut global/static '{}' (DRAFT-static-vars §4/§7; globals and static fields default to \`val\`; use \`#Mut let\` for globals or \`#Mut\\n#Static\` for static fields)` |
| E3153 | `Circular module dependency detected involving '{}'; cannot determine global init order (DRAFT-static-vars §5.5; break the cycle or use lazy init)` |
| E3154 | `Global \`let {}\` requires an initializer (DRAFT-static-vars §3.3; non-#Cval globals must be initialized at declaration)` |
| E3155 | `Global/static initializer for '{}' must not contain try-catch; init must be infallible (DRAFT-static-vars §6)` |
| E3156 | `Cross-module access to private static '{}' is not allowed (DRAFT-static-vars §4.4; v1 placeholder)` |
| E3157 | `#Static field '{}' on generic struct '{}' is not allowed (DRAFT-static-vars §4.3; v1 prohibits static fields on generic structs)` |

> E3152（实例访问静态字段 `obj.FIELD`）与 E3158（读未初始化的 `#Mut` 全局）在 v1 占位未启用；当前实例走静态字段时由既有字段查找路径报 `E3040`。

构造模型重构（E3120..E3128；引入自 [draft/DRAFT-static-fn.md](draft/DRAFT-static-fn.md)，落地章节 §7.10）：

| 码     | 模板 |
|--------|------|
| E3120 | `` `{}::{}` resolves to an instance method, not a `#Static fn`: use `<receiver>.{}(...)` instead (DRAFT-static-fn) `` |
| E3121 | `` struct `{}` has no static fn `{}` (`Type::name(...)` requires a method annotated `#Static`; DRAFT-static-fn) `` |
| E3122 | `` `{}::{}` LHS is neither an enum nor a struct in scope (DRAFT-static-fn) `` |
| E3123 | `` `Self` type only allowed inside a `structImpl` body (DRAFT-static-fn) `` |
| E3124 | `` `Self {{ ... }}` field literal only allowed inside a `#Static fn` body (DRAFT-static-fn) `` |
| E3125 | `` `Self {{ ... }}` for struct `{}` is missing field `.{}` (all fields must be listed; DRAFT-static-fn) `` |
| E3126 | `` struct `{}` has no field `.{}` (DRAFT-static-fn) `` |
| E3127 | `` duplicate field `.{}` in `Self {{ ... }}` literal (DRAFT-static-fn) `` |
| E3128 | `` `$` (current instance) cannot be used inside a `#Static fn` body (DRAFT-static-fn) `` |

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
| E4020 | `return T& root must be {}, got '{}' (§8.6)` |
| E4021 | `function returning T& requires exactly one source: \`$\` (method) or a single T& parameter (free fn)` |
| E4023 | `Heap<T> '{}' escapes its scope: ret position requires NRVO (§8.3a.4.1)` |
| E4024 | `Heap<T> '{}' cannot be moved by value; declare as Heap<T>? for movable slots (§8.3a.3.2)` |
| E4025 | `Rc<Heap<T>> / Weak<Heap<T>> / Heap<T> as Rc<U> inner field is forbidden (§8.3a.5.1)` |
| E4026 | `NRVO not applicable: multiple ret sources or coexists with outliving borrow (§8.3a.4.1)` |
| E4027 | `implicit widen Heap<T> -> Heap<T>? is forbidden; restructure source signature (§8.3a.4.3)` |
| E4028 | `extern fn parameter / return must not be Heap<T>; use Ptr at FFI boundary (§8.3a.5.3)` |
| E4030 | `` result of `<-` move-assign is discarded; the old value will be immediately released — use `a = b` if you don't need the old value `` |

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
| E5013 | `yux.toml \`entry\` must be a relative path under \`src/\`, got absolute path: {}` |
| E5014 | `yux.toml \`entry\` resolves outside \`src/\` (\`{}\`): convention is that all sources live under \`src/\`; obj path layout may also be inconsistent`（默认 warning） |

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
| E6017 | `Unknown #Builtin function '{}'` |
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
| E6029 | `{}:<T> requires T to be Rc/Weak/Array/String or U& (got '{}')` |
| E6030 | `assert_eq:<T> requires T to be a numeric or bool type (got '{}')` |
| E6031 | `assert_eq operand type mismatch: actual is '{}', expected is '{}' ...` |

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

### D.3.7 E7xxx — 错误处理 / panic（草案，待 spec 落地）

> 由 `DRAFT-错误.md` 引入；落地章节为 §6 函数（`#Fallible`）、§4 表达式（`!` 后缀 / `tryExpr`）、§8 panic（待新建或并入相关章节）。E7001-E7014 默认严重度 = `error`；E7015-E7018 默认严重度 = `warning`。所有码挂诊断回归用例 `tests/cases/diag_throw_*.yux`（实施期落地）。

| 码     | 模板（占位） | 触发 |
|--------|-------------|------|
| E7001  | `` `!` used outside of `#Fallible(E)` function and outside of `try` block — wrap call in `try { ... } catch e E { ... }` or declare the enclosing function with `#Fallible(E)` `` | 表达式后缀 `!` 写在无 `#Fallible` 注解的函数内、且不在 `try` 块内 |
| E7002  | `` non-exhaustive `try` block: error type `{}` thrown by callee `{}` is not handled by any `catch` clause — add `catch e {} {{ ... }}` `` | `try` 块内调用的可失败函数错误类型未被任一 `catch` 子句覆盖 |
| E7003  | `` redundant `catch` clause: error type `{}` cannot be thrown by any call in the `try` block — promoted to error if `--strict-catch`，否则同 E7015 `` | 与 E7015 同触发；启用 `--strict-catch` 时升级为 error |
| E7004  | `` cannot propagate error of type `{}` through `!`: caller declares `#Fallible({})`, types differ — wrap the call in `try { ... } catch e {} {{ ret {}::Variant... }}` `` | `!` 后缀作用于 callee 错误类型与外层 `#Fallible(E)` 不一致的调用，且不在 `try` 域内 |
| E7005  | `` duplicate `catch` clause: error type `{}` is handled by more than one `catch` in the same `try` — merge into a single `catch e {} {{ match e {{ ... }} }}` `` | 同一 `try` 内多个 `catch` 子句指向同一错误 enum 类型 |
| E7006  | `` call to fallible function `{}` outside `try` block must propagate via `!` (same error type) — bare call is forbidden outside `try` (inside `try`, bare call is correct; `!` would be redundant) `` | 调用 `#Fallible` 函数但未加 `!` 且不在 `try` 块内 |
| E7007  | `` `ret` of error type `{}` does not match `#Fallible({})` — wrap the error in a `{}` variant or change the function's `#Fallible` `` | 函数体内 `ret` 表达式类型属于错误通道但与声明 `#Fallible(E)` 的 `E` 不匹配 |
| E7008  | `` function return type `{}` cannot equal its `#Fallible` type `{}` (the compiler cannot disambiguate `ret` between success and error channels) — split into two enums and rethrow / wrap explicitly `` | 函数成功值类型 `T` 与 `#Fallible(E)` 的 `E` 相等（声明阶段或单态化阶段） |
| E7009  | `` `try` block must be followed by at least one `catch` clause — bare `try {{ ... }}` is forbidden `` | `try` 块未跟 `catch` 子句 |
| E7010  | `` `catch` body must end with `ret`, `panic`-class terminator, or an expression of the same type as the `try` block (got `{}` vs `{}`) `` | `catch` body 终结值类型与 `try` block 值类型不一致 |
| E7011  | `` `catch e {}` type `{}` must be a declared enum; got `{}` `` | `catch` 后置类型不是已声明 enum |
| E7012  | `` `#NoReturn` function `{}` cannot declare a return type — remove the return type or remove `#NoReturn` `` | `#NoReturn` 函数声明带返回类型 |
| E7013  | `` `#NoReturn` and `#Fallible({})` are mutually exclusive on the same function — a non-returning function cannot also propagate errors `` | 同一函数同时标 `#NoReturn` 与 `#Fallible(E)` |
| E7014  | `` `#NoReturn` function `{}` may reach end of body — control flow must terminate via `panic`-class call, another `#NoReturn` call, or unconditional infinite loop `` | `#NoReturn` 函数体可达末尾（控制流分析） |
| E7015  | `` redundant `catch` clause: no call in `try` block can throw `{}` declared by `catch e {}` — remove this `catch` clause `` | `catch` 子句声明的错误类型不在 `try` 块的调用错误集合内（默认 warning） |
| E7016  | `` `!` is redundant inside `try` block: bare call to `#Fallible({})` function `{}` already routes to the matching `catch e {}` clause — remove `!` `` | `try` 块内对 `#Fallible` 函数的调用写了 `!`（默认 warning） |
| E7017  | `` redundant `try-catch`: no call in `try` block can throw any error — remove the entire `try` and use a plain block `` | `try` 块内不含任何 `#Fallible` 调用（默认 warning） |
| E7018  | `` `panic` in `catch` arm converts a recoverable error to abort — consider `ret` with an error variant or `exit(code)` if termination is intended `` | `catch` arm body 唯一终结操作是 `panic(...)`（默认 warning） |

### D.3.8 E11xx — draft / 接口（v0.5+ 占位）

> v0.5 §12 引入；编号在 Phase 3 编译器实现期固化进 `include/error_code.h`。下表为规范层占位，模板文本可在实施期微调。

| 码     | 模板（占位） | 触发 |
|--------|-------------|------|
| E1101  | `Type '{}' does not implement draft method '{}: {}' (impl block missing)` | `Type : D` 实现块缺方法（§12.2.2.1）；spec-default-body 落地后复用为"未实现 + 无默认体"统一诊断；`$.m@SpecA()` 中 T 未 `#Impl(SpecA)` 或 `Dyn<D>` 上 `@OtherSpec` 复用本码（§12.10.8.2 / §12.10.8.4） |
| E1102  | `Method '{}' in 'Type {} : D' impl block is not part of D's signature set` | 实现块多余非 draft 方法（§12.2.2.1） |
| E1103  | `Duplicate impl block 'Type {} : {}'` | 同 `Type : D` 实现块重复出现（§12.2.2.2） |
| E1104  | `draft method '{}.{}' must not introduce its own generic parameters` | draft 体内 `fn` 引入本地泛型（§12.3.2） |
| E1105  | `Method '{}' on type '{}' is defined in both an ordinary impl block and a 'Type : {}' impl block` | 同包显隐共存情形 A（§12.4.2.1） |
| E1106  | `Type '{}' does not satisfy draft bound '{}' for type parameter '{}'` | `<T : D>` 边界单态化未命中（§6.4.4.4） |
| E1110  | `'#DraftLike' annotation is only allowed on 'draft' declarations` | `#DraftLike` 标在非 draft（§11.4.2.1） |
| E1111  | `'#DraftLike' draft '{}' must not declare default method bodies` | `#DraftLike` 与默认体共用（§11.4.2.2） |
| E1112  | `'#DraftLike' draft '{}' must not contain method-local generic parameters` | `#DraftLike` 与方法本地泛型共用（§11.4.2.3） |
| E1120  | `Cannot implement draft '{}' for type '{}': both belong to external packages (orphan rule, spec §12.5)` | 跨外部包 orphan（§12.5） |
| E1131  | `` Type argument of `Dyn<...>` must be a draft name; `{}` is not a draft `` | `Dyn<X>` 中 `X` 非 draft（§12.9.3.1） |
| E1132  | `` Nested `Dyn<...>` is not allowed: `{}` cannot wrap another `Dyn` / `Rc` of `Dyn` `` | `Dyn` 嵌套 / `Rc<Dyn>` / `Weak<Dyn>`（§12.9.3.2） |
| E1133  | `` Cannot construct `Dyn<{}>` from `{}`: argument must be `Rc<U>` (owned) or `U&` (borrow) where `U` implements `{}` `` | `Dyn:<D>(x)` 构造源不满足 D（§12.9.5.3） |
| E1134  | `` Draft `{}` is not object-safe: signatures contain `Self` or the draft's own name in non-receiver position; `Dyn<{}>` / `Dyn<{}&>` is not allowed `` | draft 非对象安全（§12.9.4） |
| E1135  | `` `Dyn<D>?` (nullable dyn) is not supported in v1 `` | `Dyn<D>?` 形态（§12.9.3.3） |
| E1136  | `` `Dyn<{}>` cannot cross `extern` boundary: vtable layout is internal ABI `` | `Dyn` 跨 `extern` 边界（§12.9.10） |
| E1137  | `` Type `{}` does not implement spec method `{}` (declared in `#Impl({})`) `` | `#Impl(D)` 缺方法且 D 中无默认体（spec-unify v1，与 E1101 同语义类） |
| E1138  | `` Cannot access static member `{}` on instance of `{}`; use `{}::{}` instead `` | 实例形访问 `#Static fn`（§12.2.4.1） |
| E1139  | *（已退役）* | DRAFT-spec-default-body 落地（§12.10）解锁 spec body 方法带 body，编号保留不复用 |
| E1140  | `` spec `{}` default body references unknown method `$.{}`; must appear in this spec's signatures `` | spec 默认体 sema 占位校验失败（§12.10.3.2）；`$.m@SpecA()` 中 SpecA 无该方法 / 默认体（§12.10.8.2，消息按上下文区分"unknown method" vs "no default body"） |
| E3132  | `` Type `{}` inherits conflicting default bodies for method `{}` from specs {}; implementer must provide an explicit override `` | 多 spec 默认体组合冲突未消歧（§12.10.5） |
| E3133  | `` `Field.value` requires `f` to be compile-time determinable; `{}` is a runtime variable — cannot rewrite to field access `` | `Field.value` sema 改名时 f 非编译期可定（§13.5.2） |
| E3134  | `` `Field.value` rewrite has no receiver binding in this context (must be inside a struct method with `$`) `` | `Field.value` 改名无 `$` receiver 绑定（§13.5.2） |
| E3135  | `` `{}::variants` is only valid on enum types; `{}` is not an enum `` | 非 enum 访问 `variants` 静态字段（§13.2.1） |
| E3136  | （已消解，未触发）按值取 rodata 单例 `Counter::type` 等——设计决议：反射元数据统一按值 copy（rodata → stack），不引入 E3136 | §13.1.3 |

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

每个错误码（`ErrorCode::EXXXX`）在 `include/error_code.h` 的 `DEF_ERR` / `DEF_WARN` / `DEF_NOTE` 宏中携带 `defaultSev`。当前所有码段（E1xxx..E6xxx / E11xx）默认 `Error`；E5xxx 中 E5014 默认 `Warning`；E7xxx 中 E7001-E7014 默认 `Error`、E7015-E7018 默认 `Warning`（DRAFT-错误.md 引入）；其余 `Warning` / `Note` 段为后续 D.5 规划保留（如未使用变量、可疑类型转换等）。

默认 `Warning` 的码经 `DiagnosticEngine::emit` 非抛出发射：渲染到 stderr 后继续编译；`-Werror` / `--deny=<code>` 把其升级为 `Error` 时，emit 不重复渲染，直接抛 `YuxError` 走顶层 `renderYuxError` 路径，保持"首条 error 终止当前文件"协议。

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

- **高频场景 提示/修复建议**（原 v0.3 Phase 5）：
  - **A 阶段（已落地）**：诊断渲染支持 `= help: ...` 与 `= note: ...`；`YuxError` 通过链式 `withHint` / `withNote` 携带。
    已在 E2001（`Weak<T>?`）、E3078（`Weak == / !=`）、E3017 / E3018 / E3019 / E4001 / E4004（`T&`
    初始化与借用形态）、E2006 / E2007（缺函数体 vs `#Builtin`）、E6010 / E6011（泛型实参个数）以及
    E1002（`SyntaxErrorListener` 对常见 `';'` / `mismatched input` / `extraneous input` 等模式）站点附了简单 hint。
    回归位于 `tests/cases/diag_*.yux`。
  - **B 阶段（已落地）**：未声明标识符的拼写近似建议（Levenshtein ≤ 2）。`src/symbol_suggest.{h,cpp}` 沿
    `ScopeNode` 父链汇总可见变量与函数名，对 E3030 / E3031 / E3032 抛出处给出最近 1–3 个候选，组装为
    `did you mean ...` 风格的 `= help:` 行；候选为空时不附 hint。回归位于 `tests/cases/diag_suggest_var.{yux,expected_err}`。
  - **后续候选场景（待启）**：类型不匹配时的 `.to_<type>()` 候选；`extern` 签名不匹配；字段拼写近似（E304x）等。

## D.6 诊断回归测试

诊断测试用例位于 `tests/cases/diag_*.yux`，配对 `*.expected_err`。测试运行器规则：

- 编译必须以非零退出码结束；若 yux 意外编译成功（生成 `.exe`），用例失败。
- `expected_err` 中以 `;` 开头或全空白的行是注释/空行；其余每一非空行视为**子串断言**：该字符串必须出现在 yux 进程的 stderr/stdout（合并视图）中。
- 不强求行的出现顺序、不约束未列出的额外诊断；这一形态便于断言"必须命中"的关键内容（错误码、文件:行:列、关键消息片段），而对源码片段、对齐空白、绝对路径前缀保持鲁棒。

每个用例**应当**至少断言 `<basename>:line:col [Exxxx] error:` 这条诊断头，使得错误码、定位与消息文本三者中任一回归都能被捕获。

## D.7 Open Issues

- E3099 采用包装上下文的双行模板，长期看应当替换为结构化 `note` 而非内嵌换行。
- E6045（内置算子 arity）目前用占位字段承载方法名，未来若按算子细分，可能拆为 E60xx 段独立码。

> 已收口：原"列号按字节计算导致多字节字符插入符偏移"的 Open Issue（v0.4.x）已实现 —— `col` 改为 codepoint 列号、插入符按显示列宽对齐，详见 D.1.1。

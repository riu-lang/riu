// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 错误码与消息模板集中表
//
// 设计：每个 ErrorCodeDef 携带 (code, message_template)。throw 站点写
//   throw YuxError(line, col, ErrorCode::E3001, leftType.name, rightType.name);
// 其中 message 是 std::vformat 兼容的模板（{} 占位）。
//
// 分段：
//   E1xxx  词法（Phase 3 ANTLR Listener 接管，预留）
//   E2xxx  语法 / AST 结构
//   E3xxx  类型
//   E4xxx  所有权 / 借用
//   E5xxx  模块 / 包
//   E6xxx  内置 / 调用
//
// E0000 为兼容占位，迁移完成后应不再出现。新增码请保持段内号递增并同步附录 D。

#ifndef YUX_LANG_ERROR_CODE_H
#define YUX_LANG_ERROR_CODE_H

#include <cstdint>
#include <map>
#include <string>

// 诊断严重等级
// Note < Warning < Error；后续 CLI --warn / --allow / --deny 可在不超过该上限范围内调整
// 默认 Error 的码为"硬错误"，CLI 不允许降级为 Warning/Note（仅允许 -Werror 升级 Warning→Error）
enum class DiagSeverity : uint8_t {
    Note,
    Warning,
    Error,
};

// 错误码定义：除 code/message 外携带默认严重等级
// defaultSev 决定该码在没有 CLI 覆盖时的呈现级别，也决定该码是否可被 CLI 降级
struct ErrorCodeDef {
    const char* code;
    DiagSeverity defaultSev;
    const char* message;
};

namespace ErrorCode {

namespace detail {
    // 进程级注册表：code 字符串 → 默认严重等级
    // 每个 DEF_ERR 通过一个 inline const 变量在静态初始化阶段注册。
    inline std::map<std::string, DiagSeverity>& registry() {
        static std::map<std::string, DiagSeverity> m;
        return m;
    }
    struct CodeRegistration {
        CodeRegistration(const char* code, DiagSeverity sev) {
            registry()[code] = sev;
        }
    };
} // namespace detail

// 按字符串查 code 的默认严重等级；未知 code 返回 nullptr
inline const DiagSeverity* lookupDefaultSeverity(const std::string& code) {
    auto& m = detail::registry();
    auto it = m.find(code);
    if (it == m.end()) return nullptr;
    return &it->second;
}

// 默认 Error 的常用宏
#define DEF_ERR(code, msg) \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Error, msg}; \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Error};
// 默认 Warning（当前还没有 Warning 类码，留作 Phase 5 引入未使用变量等场景）
#define DEF_WARN(code, msg) \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Warning, msg}; \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Warning};
// 默认 Note（信息性提示）
#define DEF_NOTE(code, msg) \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Note, msg}; \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Note};

// ── 占位 ──────────────────────────────────────────────────────────────
DEF_ERR(0000, "")

// ── E1xxx 词法 / 文法（ANTLR 报告） ───────────────────────────────────
DEF_ERR(1001, "lexer error: {}")
DEF_ERR(1002, "syntax error: {}")

// ── E11xx draft / 接口（v0.5+） ───────────────────────────────────────
// 附录 D §D.3.7
DEF_ERR(1101, "Type '{}' does not implement draft method '{}: {}' (impl block missing)")
DEF_ERR(1102, "Method '{}' in 'Type {} : D' impl block is not part of D's signature set")
DEF_ERR(1103, "Duplicate impl block 'Type {} : {}'")
DEF_ERR(1104, "draft method '{}.{}' must not introduce its own generic parameters")
DEF_ERR(1105, "Method '{}' on type '{}' is defined in both an ordinary impl block and a 'Type : {}' impl block")
DEF_ERR(1106, "Type '{}' does not satisfy draft bound '{}' for type parameter '{}'")
DEF_ERR(1110, "'#DraftLike' annotation is only allowed on 'draft' declarations")
DEF_ERR(1111, "'#DraftLike' draft '{}' must not declare default method bodies")
DEF_ERR(1112, "'#DraftLike' draft '{}' must not contain method-local generic parameters")
DEF_ERR(1120, "Cannot implement draft '{}' for type '{}': both belong to external packages (orphan rule, spec §12.5)")
// ── Dyn<D> / Dyn<D&> 运行时多态 (DRAFT-dyn-draft / 拟 §12.9 — Phase 2) ────
DEF_ERR(1131, "Type argument of `Dyn<...>` must be a draft name; `{}` is not a draft")
DEF_ERR(1132, "Nested `Dyn<...>` is not allowed: `{}` cannot wrap another `Dyn` / `Box` of `Dyn`")
DEF_ERR(1133, "Cannot construct `Dyn<{}>` from `{}`: argument must be `Box<U>` (owned) or `U&` (borrow) where `U` implements `{}`")
DEF_ERR(1134, "Draft `{}` is not object-safe: signatures contain `Self` or the draft's own name in non-receiver position; `Dyn<{}>` / `Dyn<{}&>` is not allowed")
DEF_ERR(1135, "`Dyn<D>?` (nullable dyn) is not supported in v1")
DEF_ERR(1136, "`Dyn<{}>` cannot cross `extern` boundary: vtable layout is internal ABI")

// ── E2xxx 语法 / AST 结构 ─────────────────────────────────────────────
DEF_ERR(2001, "Weak<T>? is forbidden: Weak is natively nullable (upgrade returns Box<T>?)")
DEF_ERR(2002, "buildTypeWithRef: unknown typeWithRef alternative")
DEF_ERR(2003, "module `{}` is ambiguous: both `{}.yux` and `{}/` exist")
DEF_ERR(2004, "module alias `{}` conflicts with existing symbol")
DEF_ERR(2005, "Unknown build annotation `#{}`")
DEF_ERR(2006, "Function `{}` has no body; only `#CompilerInner` functions may omit the body")
DEF_ERR(2007, "Method `{}.{}` has no body; only `#CompilerInner` methods may omit the body")
DEF_ERR(2008, "wildcard alias `{}` is ambiguous, matched {}")
DEF_ERR(2009, "Function `{}` cannot return `T&`; only `#CompilerInner` baked builtins may have a reference return type (spec §8.9)")
DEF_ERR(2010, "Cannot call mutating method `Array.{}` on `{}`: it has an active borrow (spec §8.4.2.5)")
DEF_ERR(2011, "Build annotation `#{}` is not allowed on this declaration (only `fn` accepts it)")
DEF_ERR(2012, "`#Test` function `{}` must have signature `fn {}()` (no params, no return type, must have body)")
DEF_ERR(2013, "`#Test` and `#CompilerInner` cannot both be applied to function `{}`")
DEF_ERR(2014, "`#Test` is only allowed in `*.test.yux` files; `{}` is not a test file")
DEF_ERR(2015, "draft bounds (`: D`) are only allowed at declaration sites (fn/struct/draft generic params); not at type references or call-point turbofish")
DEF_ERR(2016, "Type alias `{}` forms a cycle (recursive without indirection)")
DEF_ERR(2017, "Type alias name `{}` conflicts with existing {} `{}`")
DEF_ERR(2018, "Duplicate variant `{}` in enum `{}`")
DEF_ERR(2019, "Unknown enum `{}` in constructor `{}::{}`")
DEF_ERR(2020, "Enum `{}` has no variant `{}`")
DEF_ERR(2021, "Enum variant `{}::{}` expects {} payload arg(s), got {}")
DEF_ERR(2022, "match scrutinee must be an enum type, got `{}`")
DEF_ERR(2023, "non-exhaustive match on enum `{}`: missing variant(s) {}")
DEF_ERR(2024, "duplicate variant `{}::{}` in match arms")
DEF_ERR(2025, "`else` arm must be the last arm in match")
DEF_ERR(2026, "match pattern for `{}::{}` expects {} binding(s), got {}")
DEF_ERR(2027, "duplicate binding `{}` in match pattern `{}::{}`")
DEF_ERR(2028, "lambda body references outer local `{}`: closures not yet supported (Phase 4)")
DEF_ERR(2029, "lambda capture of `{}` (type `{}`) not yet supported: Phase 4a / 4a-2 / 4c cover scalars / 8-byte heap handles (Box / Weak / Array / String) / `T&`; structs / enums / fn / mixing `T&` with heap handles arrive in later phases")
DEF_ERR(2030, "lambda body cannot assign to captured variable `{}` (spec §6.2.1: captures are immutable in v1)")
DEF_ERR(2031, "extern fn `{}` cannot use fn(...) types in {} (function values are not ABI-compatible with C; spec §7)")

// ── E3xxx 类型 — 类型不匹配 ───────────────────────────────────────────
DEF_ERR(3001, "Type mismatch in +-/ operation: left is {}, right is {}")
DEF_ERR(3002, "Type mismatch in */% operation: left is {}, right is {}")
DEF_ERR(3003, "Type mismatch in &|^ operation: left is {}, right is {}")
DEF_ERR(3004, "Type mismatch in comparison: left is {}, right is {}")
DEF_ERR(3005, "Type mismatch in if-elif branches: {} vs {}")
DEF_ERR(3006, "Type mismatch in if-else branches: {} vs {}")
DEF_ERR(3007, "Type mismatch in one-line if-else: true branch is {}, false branch is {}")
DEF_ERR(3008, "Type mismatch in if-else expression: true branch is {}, false branch is {}")
DEF_ERR(3009, "Array fill literal type mismatch: literal is {}, but explicit type is {}")
DEF_ERR(3010, "Array fill element type mismatch: expected {}, got {}")
DEF_ERR(3011, "Array elements must have the same type: {} vs {}")
DEF_ERR(3012, "Array size mismatch: expected {}, got {}")
DEF_ERR(3013, "Array element type mismatch: expected {}, got {}")
DEF_ERR(3014, "Box type mismatch: expected Box<{}>, got {}")
DEF_ERR(3015, "Cannot assign {} to Nullable<{}>")
DEF_ERR(3016, "Weak<{}> 仅支持从 Box<{}> 或 Weak<{}> 构造")
DEF_ERR(3017, "T& local initializer type mismatch: expected {}&, got {}&")
DEF_ERR(3018, "T& copy-bind source type mismatch: '{}' is not {}&")
DEF_ERR(3019, "T& local initializer must be &expr or copy-bind from a T& variable")
DEF_ERR(3020, "Return type mismatch: function declares '{}', but expression has type '{}'")
DEF_ERR(3021, "Function declares return type '{}', but returns void")
DEF_ERR(3022, "Void function cannot return a value of type '{}'")
DEF_ERR(3023, "`??` right side type {} doesn't match Nullable inner type {}")
DEF_ERR(3024, "Left side of `??` must be Nullable<T>, got {}")
DEF_ERR(3025, "`?.` requires Nullable<T> on the left, got {}")
DEF_ERR(3026, "String template interpolation requires type implementing ToString, got '{}' (impl `Type : ToString {{ fn to_string() String {{ ... }} }}`)")
DEF_ERR(3027, "Type mismatch in match arms: expected {}, arm produces {}")

// ── E3xxx 类型 — 符号查找 ─────────────────────────────────────────────
DEF_ERR(3030, "Undefined variable: {}")
DEF_ERR(3031, "Variable not found: {}")
DEF_ERR(3032, "Symbol {} not found")
DEF_ERR(3033, "Array variable not found: {}")

// ── E3xxx 类型 — 字段 / 结构体访问 ────────────────────────────────────
DEF_ERR(3040, "Struct {} has no field: {}")
DEF_ERR(3041, "Cannot access field on non-struct type: {}")
DEF_ERR(3042, "Cannot access private field '{}' of struct '{}'")
DEF_ERR(3043, "Cannot find struct declaration for field access")
DEF_ERR(3044, "`?.` inner type {} has no struct decl")
DEF_ERR(3045, "Cannot access member on non-struct type: {}")
DEF_ERR(3046, "Nested member access not yet supported")

// ── E3xxx 类型 — 泛型参数缺失 ─────────────────────────────────────────
DEF_ERR(3050, "Box<T> missing inner type T")
DEF_ERR(3051, "Nullable type requires inner type")
DEF_ERR(3052, "Weak type requires element type")
DEF_ERR(3053, "Ref type requires element type")
DEF_ERR(3054, "Ref type missing inner type")
DEF_ERR(3055, "Array type requires element type")
DEF_ERR(3056, "Box type requires element type")
DEF_ERR(3057, "Invalid array type: missing element type")

// ── E3xxx 类型 — 数组操作 ─────────────────────────────────────────────
DEF_ERR(3060, "Array access requires at least one index")
DEF_ERR(3061, "Array access requires a variable")
DEF_ERR(3062, "Cannot index non-array type: {}")
DEF_ERR(3063, "Empty array literal not supported")
DEF_ERR(3064, "Array<T> initialization requires Array<T> expression or array literal")
DEF_ERR(3065, "Array assignment requires at least one index")
DEF_ERR(3066, "Array assignment requires a variable")
DEF_ERR(3067, "Array fill expression requires array type annotation with size")
DEF_ERR(3068, "Array fill expression requires array type annotation")

// ── E3xxx 类型 — 运算符 ───────────────────────────────────────────────
DEF_ERR(3070, "Cannot apply bitwise NOT to float type: {}")
DEF_ERR(3071, "Cannot apply logical NOT to non-bool type: {}")
DEF_ERR(3072, "Unknown unary operator")
DEF_ERR(3073, "Type '{}' does not support operator '{}' (method '{}' not found)")
DEF_ERR(3074, "Type '{}' does not support unary operator '{}' (method '{}' not found)")
DEF_ERR(3075, "Unsupported binary operation")
DEF_ERR(3076, "Unsupported comparison operation")
DEF_ERR(3077, "Unsupported mul/div/mod operation")
DEF_ERR(3078, "Weak<T> does not support == / != (v1 does not expose handle comparison)")

// ── E3xxx 类型 — 字面量 ───────────────────────────────────────────────
DEF_ERR(3080, "Unsupported literal type")
DEF_ERR(3081, "Unsupported literal type for array fill")
DEF_ERR(3082, "Unsupported literal type for global constant: {}")

// ── E3xxx 类型 — 其他 ─────────────────────────────────────────────────
DEF_ERR(3090, "Unsupported dot expression")
DEF_ERR(3091, "Unknown expression type")
DEF_ERR(3092, "Unknown statement type")
DEF_ERR(3093, "Cannot assign to immutable variable: {}")
DEF_ERR(3094, "break statement not within a loop")
DEF_ERR(3095, "Type {} is not a Function")
DEF_ERR(3096, "Cannot get LLVM type for '{}'")
DEF_ERR(3097, "Cannot determine type for reference expression: no scope")
DEF_ERR(3098, "Unknown type '{}' for field '{}' of generic struct '{}'")
DEF_ERR(3099, "{}\n  {}") // compiler_types 包装上下文（msg + ctx）
DEF_ERR(3100, "Tuple index {} out of range for type '{}' (size {})")
DEF_ERR(3101, "Tuple destructure expects type tuple, got '{}'")
DEF_ERR(3102, "Tuple destructure arity mismatch: {} names vs tuple size {}")

// ── E4xxx 所有权 / 借用 ───────────────────────────────────────────────
DEF_ERR(4001, "T& borrow initializer must be &expr or an existing T& variable")
DEF_ERR(4002, "T& '{}' borrows root '{}' whose scope does not cover the borrow")
DEF_ERR(4003, "root '{}' cannot be reassigned while borrowed (§3.5)")
DEF_ERR(4004, "Cannot bind T& to non-local: {}")
DEF_ERR(4010, "field '$.{}' {} before initialization (§8.2)")
DEF_ERR(4011, "field '$.{}' must be initialized before {} (§8.2)")
DEF_ERR(4012, "field '$.{}' is not initialized at constructor exit (§8.2)")
DEF_ERR(4013, "cannot return `$` from constructor (§8.3)")
DEF_ERR(4020, "return T& root must be {}, got '{}' (§8.6)")
DEF_ERR(4021, "function returning T& requires exactly one source: `$` (method) or a single T& parameter (free fn)")
DEF_ERR(4022, "lambda value with `T&` capture cannot escape current frame (cannot be returned, stored to var/field/container/Box; only consumable inline as call argument; spec §6.3)")

// ── E5xxx 模块 / 包 ───────────────────────────────────────────────────
DEF_ERR(5001, "yux.toml not found in {}")
DEF_ERR(5002, "yux.toml is missing required field `name`")
DEF_ERR(5003, "yux.toml field `name` must be a string")
DEF_ERR(5004, "yux.toml field `name` must not be empty")
DEF_ERR(5005, "yux.toml `lib` must be a table")
DEF_ERR(5006, "yux.toml `lib.type` must be \"static\" or \"dynamic\"")
DEF_ERR(5007, "yux.toml `lib.type=\"dynamic\"` not yet supported")
DEF_ERR(5008, "yux.toml `[lib]` and `entry` are mutually exclusive")
DEF_ERR(5009, "failed to parse yux.toml: {}")
DEF_ERR(5010, "syntax errors in {}")
DEF_ERR(5011, "circular module import: {}")
DEF_ERR(5012, "module not found: {} (expected file {})")

// ── E6xxx 内置 / 调用 ─────────────────────────────────────────────────
DEF_ERR(6001, "module `{}` not found in package `{}`")
DEF_ERR(6002, "function `{}` not found in module `{}`")
DEF_ERR(6003, "Cannot call private function `{}` via package alias")
DEF_ERR(6004, "Cannot call private function `{}` via module alias")
DEF_ERR(6005, "module `{}` (alias `{}`) not loaded")
DEF_ERR(6006, "Cannot call private function '{}'")
DEF_ERR(6007, "Cannot call private method '{}' of struct '{}'")
DEF_ERR(6008, "Cannot use private struct '{}' in constructor")
DEF_ERR(6009, "Generic struct '{}' constructor requires explicit type arguments")
DEF_ERR(6010, "Generic function '{}' expects {} type args, got {}")
DEF_ERR(6011, "Generic struct '{}' expects {} type args, got {}")
DEF_ERR(6012, "Generic function '{}' expects {} params, got {} args")
DEF_ERR(6013, "Cannot infer type parameter '{}' for generic function '{}'")
DEF_ERR(6014, "Ambiguous call to '{}({})': {} overloads match; add type suffix to disambiguate:{}")
DEF_ERR(6015, "Unsupported call expression")
DEF_ERR(6016, "Unknown method '{}' for builtin type '{}'")
DEF_ERR(6017, "Unknown #CompilerInner function '{}'")
DEF_ERR(6018, "Cannot determine type argument for size_of")
DEF_ERR(6019, "Cannot determine LLVM type for '{}'")

// builtin 调用：参数 / 类型实参数量
DEF_ERR(6020, "ptr_from_addr expects 1 argument")
DEF_ERR(6021, "rc_leak_count expects 0 arguments")
DEF_ERR(6022, "_ptr_offset expects 2 arguments")
DEF_ERR(6023, "Cannot call private function '_ptr_offset' (SDK-only Ptr arithmetic)")
DEF_ERR(6024, "upgrade expects 1 type argument")
DEF_ERR(6025, "upgrade expects 1 argument")
DEF_ERR(6026, "{} expects 1 type argument")
DEF_ERR(6027, "{} expects {} argument(s)")
DEF_ERR(6028, "{}:<T&> requires a local var or &expr argument")
DEF_ERR(6029, "{}:<T> requires T to be Box/Weak/Array/String or U& (got '{}')")
DEF_ERR(6030, "assert_eq:<T> requires T to be a numeric or bool type (got '{}')")
DEF_ERR(6031, "assert_eq operand type mismatch: actual is '{}', expected is '{}' (yux 不做隐式整型/浮点转换；整型字面量默认 i32，需要时加后缀如 `3i64`/`3u8` 或写 `assert_eq:<T>(...)` 锁定类型)")
DEF_ERR(6032, "copy_of:<T> cannot copy types containing Ref fields (offending: '{}') — Ref 借的是别人的可写状态，独立 owned 副本与借用语义冲突 [DRAFT-const-mut §5.3]")
DEF_ERR(6033, "No matching constructor for '{}({})'; declared overloads:{}")

// Array 内置方法
DEF_ERR(6040, "at requires 1 argument")
DEF_ERR(6041, "Array.pop() requires an lvalue array")
DEF_ERR(6042, "Array mutation method '{}' requires an lvalue array")
DEF_ERR(6043, "set_len requires 1 argument")
DEF_ERR(6044, "push requires 1 argument")

// 内置算子方法 arity（plus / minus / ... 共用同一模板）
DEF_ERR(6045, "{} requires 1 argument")

// ── E7xxx 错误模型 / panic（DRAFT-错误.md） ─────────────────────────────
// 附录 D §D.3.7 之后段位；E7001-E7014 默认 Error，E7015-E7018 默认 Warning（Phase 10d+ 启用）
// E7012 / E7013 / E7014 由 Phase 10d 启用；E7001 / E7004 / E7006 / E7008 由 Phase 10e 启用；其余诊断码留 10f
DEF_ERR(7001, "`!` used outside of `#Fallible(E)` function and outside of `try` block — wrap call in `try {{ ... }} catch e E {{ ... }}` or declare the enclosing function with `#Fallible(E)`")
DEF_ERR(7004, "cannot propagate error of type `{}` through `!`: caller declares `#Fallible({})`, types differ — wrap the call in `try {{ ... }} catch e {} {{ ret {}::Variant... }}`, or wrap in a function whose `#Fallible` matches `{}`")
DEF_ERR(7006, "call to fallible function `{}` outside `try` block must propagate via `!` (same error type) — bare call is forbidden outside `try` (inside `try`, bare call is correct; `!` would be redundant)")
DEF_ERR(7008, "function return type `{}` cannot equal its `#Fallible` type `{}` (the compiler cannot disambiguate `ret` between success and error channels) — split into two enums (one for the success result type, one for the error type) and rethrow / wrap explicitly")
DEF_ERR(7012, "`#NoReturn` function `{}` cannot declare a return type — remove the return type or remove `#NoReturn`")
DEF_ERR(7013, "`#NoReturn` and `#Fallible({})` are mutually exclusive on the same function — a non-returning function cannot also propagate errors")
DEF_ERR(7014, "`#NoReturn` function `{}` may reach end of body — control flow must terminate via `panic`-class call, another `#NoReturn` call, or unconditional infinite loop")
// E7002 / E7009 / E7010 / E7011 / E7015-E7018 由 Phase 10f 启用（try-catch 块）
DEF_ERR(7002, "non-exhaustive `try` block: error type `{}` thrown by callee `{}` is not handled by any `catch` clause — add `catch e {} {{ ... }}`")
DEF_ERR(7009, "`try` block must be followed by at least one `catch` clause — bare `try {{ ... }}` is forbidden")
DEF_ERR(7010, "`catch` body must end with `ret`, `panic`-class terminator, or an expression of the same type as the `try` block (got `{}` vs `{}`)")
DEF_ERR(7011, "`catch e {}` type `{}` must be a declared enum; got `{}`")
DEF_WARN(7015, "redundant `catch` clause: no call in `try` block can throw `{}` declared by `catch e {}` — remove this `catch` clause")
DEF_WARN(7016, "`!` is redundant inside `try` block: bare call to `#Fallible({})` function `{}` already routes to the matching `catch e {}` clause — remove `!`")
DEF_WARN(7017, "redundant `try-catch`: no call in `try` block can throw any error — remove the entire `try` and use a plain block")
DEF_WARN(7018, "`panic` in `catch` arm converts a recoverable error to abort — consider `ret` with an error variant or `exit(code)` if termination is intended")

#undef DEF_ERR
#undef DEF_WARN
#undef DEF_NOTE
} // namespace ErrorCode

#endif // YUX_LANG_ERROR_CODE_H

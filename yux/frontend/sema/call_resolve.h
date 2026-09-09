// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_CALL_RESOLVE_H
#define YUX_LANG_SEMA_CALL_RESOLVE_H

#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/spec_node.h"
#include "sema/builtin_methods.h"
#include "sema/name_resolver.h"

class Yux;
class SpecRegistry;
class SpecImplChecker;

// 调用重载解析（Sema/Codegen 拆分 Phase 3.3 前置）
//
// 从 `yux/yux/compiler/compiler_call.cpp` 抠出, 不依赖 LLVM, 用于:
// - SemaPass 在 visitCall 阶段调用, 提前完成歧义诊断 + 灵活整数推断
// - Compiler 在 compile 调用 / 构造点仍调用（泛型体 SemaPass 跳过，作兜底）
//
// 副作用: 唯一匹配时会通过 tryInferIntType 把灵活整数实参类型回填到 AST 节点;
//        因此整条流水线必须保证只调用一次, 否则推断会被重复执行 (幂等但浪费).
namespace sema {

// 解析普通函数 / 模块函数的重载.
// 命中多个候选时抛 E6014 (ambiguous overload).
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName, const vector<p<ExprNode>>& args,
                       int line);

// 解析构造器重载 (在符号表中以 `S.S` 注册, params[0] 是接收者).
// 命中多个候选时抛 E6014.
void resolveCtorOverload(FileNode* file, const string& structName, const vector<p<ExprNode>>& args, int line);

// 解析结构体方法调用的重载 (在符号表中以 `TypeName.methodName` 注册, params[0] 是接收者).
// 与 resolveCtorOverload 同思路，但方法名由调用方拼接 `baseTypeName + "." + member`.
// 命中多个候选时抛 E6014；无候选时 no-op.
// sdkFile 为可选的 SDK 回退查找 (允许 nullptr).
void resolveMethodOverload(FileNode* file, FileNode* sdkFile, const string& baseTypeName, const string& member,
                           const vector<p<ExprNode>>& args, int line);

// 泛型函数 / 泛型构造器调用点的类型实参 arity 校验 (Phase 3.3 前置.3c).
//
// 当调用点写了显式 `:<T1, T2, ...>` 时, 检查 arity 与声明的 typeParams 是否匹配.
// 不匹配抛 E6010, 带 `.withHint(...)` 提示用户如何补全.
//
// 注意:
//   - 仅在 explicitTypeArgsCount > 0 时调用 (无 typeArgs 走类型推断, 不是 arity 错).
//   - 调用方需自行确认 callee 已识别为泛型 fn / 泛型 struct.
void validateGenericTypeArgsArity(const string& fnName, size_t expectedCount, size_t actualCount, int line, int col);

// 泛型 fn 的 typeArgs 边界校验 (Phase 3.3.3.c, E3032 / E1106).
//
// `<T : D1 + D2>` 形态: 对每个 typeParam[i], 在 fnOwner 的可见性下解析每个 bound
// 名 D (SpecRegistry), 然后用 SpecImplChecker::boundSatisfied 校验 typeArgs[i]
// 是否满足该 draft. 不满足:
//   - bound 名 D 无法解析 → E3032 (draft 名未声明)
//   - 解析成功但 boundSatisfied 返回 false → E1106 (typeArg 不实现 D)
//
// `registry` / `checker` 任一为 nullptr 时 helper 直接 no-op (Yux 未就绪时不强制).
//
// v0.5: 函数声明位 specBound 暂未携带类型实参 (ast_builder 仅取基名), 这里始终用
// 空 vector 传给 boundSatisfied; 草案 §6.4.4.1 文法允许 `D<T>` 形态留待后续扩展.
//
// 调用方:
//   - Compiler::compileGenericFunctionCall 在 typeArgs 解析就绪后调用
//
// 纯 AST + Registry 查表 + checker 调用, 无 LLVM 依赖.
void validateGenericTypeArgsSpecBound(const SpecRegistry* registry, const SpecImplChecker* checker, FileNode* fnOwner,
                                      FnHeaderNode* header, const vector<TypeInfo>& typeArgs, int line, int col);

// Dyn<D> callee 的 draft 名字解析 (Phase 3.3.3.b, E1131).
//
// 入口: 调用 `someDynVal.foo(...)` 时, 取 baseType (`Dyn<D>` / `Dyn<D&>`) 的内层
// D 名, 在 visibleFrom 文件的可见性范围内通过 SpecRegistry 解析 → draft decl + 限定名.
//
// 解析失败 (Phase 2b/2c 理论已拦截, 这里是 codegen 兜底) 抛 E1131, payload 为裸名
// (若 baseType 不是 Dyn 形态导致裸名为空, 用 "?" 占位).
//
// `registry` 允许为 nullptr (SemaPass 早期未持有 Yux*); 此时直接抛 E1131, 由
// Compiler 路径覆盖.
//
// 返回值: 命中 → 填充 decl + qualified; 未命中 → throw, 不返回.
//
// 纯 AST / 字符串查表, 无 LLVM 依赖.
struct DynCalleeResolved {
    SpecDeclNode* decl = nullptr;
    string qualified;
};
DynCalleeResolved resolveDynCalleeSpec(const SpecRegistry* registry, FileNode* visibleFrom, const TypeInfo& baseType,
                                       int line, int col);

// Dyn<D> 方法调用的静态形态校验 (Phase 3.3 前置.3f).
//
// 调用方 (Compiler::compileDynMethodCall) 在解析到 specDecl 后调用.
// 校验:
//   - E6016: specDecl.signatures 内找不到名为 member 的签名
//   - E6012: 找到签名但 params arity 与 argTypes 不一致
//   - E6015: 形参类型 (按 draft 签名 TypeInfo== 比对) 与实参不一致, 带 hint
//
// 命中 (合法) 时返回找到的 `FnHeaderNode*`, 让 codegen 走 vtable indirect call.
// 纯 AST / TypeInfo, 无 LLVM 依赖.
//
// `specQualified` 仅用于 E6015 hint 的人类可读名 (e.g. "yux.core.io.Writer").
FnHeaderNode* resolveDynMethodSig(SpecDeclNode* specDecl, const string& specQualified, const TypeInfo& baseType,
                                  const string& member, const vector<TypeInfo>& argTypes, int line, int col);

// 非-ID callee 路径的 `!` 错误传播 fallback 校验 (Phase 3.3 前置.3d).
//
// 适用场景:
//   - 非 ID-literal callee 写了 `!` (如 nullable 字面量调用 `obj.f!()`)
//   - fn-value callee 写了 `!` (lambda IIFE / fn-typed 变量, 非 ExprDotNode)
//
// 调用方负责在 callee 路径 + `errPropagate() == true` + 不在 try block 内时调用.
// 当 caller 也不是 #Fallible 时抛 E7001.
//
// 与 checkErrPropagateForIdCall 区分: 那一路负责 ID-callee 完整三态 (E7001/E7004/E7006);
// 这里只覆盖 fallback "caller 必须 fallible" 这一个最弱条件.
//
// 当前 Compiler 端是唯一调用方 (compileCallExpr 入口的两处 else 分支);
// SemaPass 暂未跟踪 try block, 不能直接调用.
void checkBangWithoutFallibleCaller(FnNode* currentFnNode, p<ExprCallNode> callNode,
                                    LambdaExprNode* currentLambda = nullptr);

// ID-callee 错误传播语义校验 (Phase 10e/10f).
//
// 抛错码:
//   - E7001: caller 非 #Fallible 且不在 try block 内, 但调用点写了 `!`
//   - E7004: caller/callee 错误类型不一致, 同 `!` 不可跨类型透传
//   - E7006: callee 是 #Fallible 但调用点未加 `!`, 且不在 try block 内
//   - E7016: try block 内 `!` 冗余 (warning; 经 DiagnosticEngine::emit 渲染,
//            不抛出, 除非 -Werror / --deny 把它升级为 error)
//
// 副作用: 当 tryBlockSeenErrs != nullptr 且 callee 的 fallibleErrType 非空时,
// 把 calleeErr 追加进 vector —— 用于 try block 的 E7002/E7015 穷尽性判定.
//
// sourcePath: 仅用于 E7016 warning 渲染时的 file:line:col 前缀; 空串 = 无路径,
// emit 会按 line:col 形态渲染. emit 内部按 (file, code, line, col, message) 5 元组去重.
void checkErrPropagateForIdCall(FnNode* currentFnNode, p<ExprCallNode> callNode, const string& fnName,
                                const FnSymbolInfo* calleeSym, vector<string>* tryBlockSeenErrs,
                                const string& sourcePath = "", LambdaExprNode* currentLambda = nullptr);

// fn-value callee 错误传播语义校验 (Phase F7).
// calleeFnType 须为 isFn()；fallible 元数据取自 fnReturnType().fallibleErr。
// 抛错码与 checkErrPropagateForIdCall 同族 (E7001/E7004/E7006/E7016)。
void checkErrPropagateForFnValueCall(FnNode* currentFnNode, p<ExprCallNode> callNode, const TypeInfo& calleeFnType,
                                     vector<string>* tryBlockSeenErrs, const string& sourcePath = "",
                                     LambdaExprNode* currentLambda = nullptr);

// `Type::name(...)!` 静态方法路径调用的错误传播校验。
// 语义与 ID-callee 相同；callee 错误类型直接来自已解析的静态方法签名。
void checkErrPropagateForPathCall(FnNode* currentFnNode, p<ExprPathCallNode> callNode, const string& fnName,
                                  const string& calleeErr, vector<string>* tryBlockSeenErrs,
                                  const string& sourcePath = "", LambdaExprNode* currentLambda = nullptr);

// 包/模块别名调用解析 (Phase 3.3.1.a).
// 类型路径见 sema::resolveTypePath（name_resolver.h）：对称的已 use 前缀查找，不 loadModule.
//
// 识别两种调用形态:
//   1. 包别名: `pkg.mod.fn(args)` —— ExprDotNode::parseChain 拿到 (aliasName, segs),
//      segs.size() >= 2, aliasName 在 file 符号表登记为 Package
//   2. 模块别名: `mod.fn(args)` —— baseExpr 是 ID literal, aliasName 登记为 Module
//
// 命中其中一种时返回 `matched = true`, 填充 fnName + fnSym, 调用方据此走
// compileKnownFunctionCall 路径; 未命中时返回 `matched = false`, 调用方继续后续
// 内置类型 / 结构体方法分派.
//
// 抛错:
//   - E6001 (包下找不到子模块) / E6002 (模块下找不到 fn) / E6003 (包下 fn 私有)
//   - E6005 (模块文件 Yux::module 加载失败) / E6004 (模块下 fn 私有)
//   - 多重别名命中 (Package + Module 同名) 直接走 throwAmbiguousAlias
//
// `yux` 参数允许为 nullptr (SemaPass 早期可能拿不到; Compiler 路径恒非空), 此时
// 模块别名分支整段跳过 (返回 matched=false), 由 Compiler 路径兜底; 不会因 yux 缺
// 失而误报 E6005.
//
// 纯 AST / 纯符号查 + 字符串拼接, 无 LLVM 依赖.
struct ModuleFnCallResult {
    bool matched = false;
    string fnName;
    FnSymbolInfo* fnSym = nullptr;
    // 泛型函数回退：当 lookupFnSymbolWithParams 未命中具体重载，但目标模块存在
    // 同名泛型函数时，填入 genericFn + genericOwner。调用方据此走 compileGenericFunctionCall。
    FnNode* genericFn = nullptr;
    FileNode* genericOwner = nullptr;
};

ModuleFnCallResult resolveModuleFnCall(FileNode* file, Yux* yux, p<ExprCallNode> callNode, p<ExprDotNode> dotNode,
                                       const vector<TypeInfo>& argTypes);

// 泛型函数调用的 typeArgs 推断 (Phase 3.3.1.b).
//
// 调用前提: callNode->getTypeArgs() 为空 (无显式 typeArgs 路径), genericFn->header()->isGeneric()
// 已被调用方确认.
//
// 抛错:
//   - E6012: genericFn 的 params arity 与 argTypes.size() 不一致
//   - E6013: 某 type param 无法从形参 / 实参的 unify 反推 (未出现在裸类型位置或嵌套泛型对位)
//
// 输出: outTypeArgs 按 typeParams 顺序追加推断结果 (调用方应保证传入为空 vector).
// 纯 TypeInfo unify + map 查表, 无 LLVM 依赖.
void inferGenericFnTypeArgs(p<ExprCallNode> callNode, p<FnNode> genericFn, const string& fnName,
                            const vector<TypeInfo>& argTypes, vector<TypeInfo>& outTypeArgs);

// 泛型重载消歧：从多个同名泛型函数中选最佳匹配 (Phase 3.3.1.a+).
//
// 逐个调用 inferGenericFnTypeArgs 尝试推断 typeArgs，成功则按形参/实参 Ref 一致性
// 打分（pRef==aRef 加分），返回最高分者。平局时保留输入列表顺序（靠前者优先）。
// 所有候选都推断失败时返回 {nullptr, nullptr}。
//
// callNode 仅用于错误行号/列号上报（inferGenericFnTypeArgs 抛 E6012/E6013 时内部吞掉）。
// 纯 TypeInfo unify + 计数, 无 LLVM 依赖。
//
// 调用方:
//   - SemaPass::visitExpr (Bucket 4 泛型 spec-bound 校验前)
//   - Compiler::compileFunctionCall (泛型消歧块)
std::pair<FnNode*, FileNode*> resolveBestGenericOverload(const std::vector<std::pair<FnNode*, FileNode*>>& genericFns,
                                                         p<ExprCallNode> callNode, const std::string& fnName,
                                                         const std::vector<TypeInfo>& argTypes);

// Builtin 泛型 intrinsic 的 typeArgs / args arity 校验 (Phase 3.3.2.c).
//
// 覆盖 compileGenericFunctionCall 的 #Builtin 分支的纯计数校验:
//   - E6017: 未知 intrinsic (兜底, 不在清单内的 fnName)
//   - E6018: size_of typeArgs 为空
//   - E6026: upgrade typeArgs != 1
//   - E6027: upgrade args != 1
//   - E6026: assert_eq / same_ref / ptr_of / as_ref / copy_of / weak typeArgs != 1
//   - E6027: assert_eq args != 2; same_ref args != 2; ptr_of / as_ref / copy_of / weak args != 1
//
// 内建清单 (按当前 SDK assert.yux / mem.yux / ref.yux / weak.yux):
//   assert_eq, size_of, upgrade, same_ref, ptr_of, as_ref, copy_of, weak
//
// E6019 (size_of typeArg getLLVMType 失败) 依赖 LLVM, 留在 codegen.
// E6028 / E6029 / E6032 由 validateBuiltinIntrinsicTypeShape 抛（SemaPass 显式 typeArgs 与推断后都查）.
//
// 纯字符串 + size 比较, 无 LLVM 依赖.
void validateBuiltinIntrinsicShape(const string& fnName, size_t typeArgsCount, size_t argsCount, int line, int col);

// Builtin 泛型 intrinsic 的类型形态校验 (Phase 3.3.2.d).
//
// 在 validateBuiltinIntrinsicShape 通过 arity 校验之后调用.
// 覆盖与类型相关 (typeArg / argType / AST 形态) 的检查:
//   - same_ref / ptr_of:
//       * E6029: T 必须是堆句柄 (Rc / Weak / Array 泛型 / String Normal) 或 T&
//       * E6028: T = Ref 时, 每个 arg 的 AST 必须是 ID-literal 或 ExprGetRefNode
//                (取裸源指针只支持这两种 AST 形态; 其他形态无静态出口)
//   - as_ref:
//       * E6029: argType[0] 必须是 Rc 且不能是 Nullable
//   - weak:
//       * E6029: argType[0] 必须是 Rc 或 Rc?, 若 Nullable 其 inner 必须是 Rc
//   - copy_of:
//       * E6032: T 深度含有 Ref<U> 字段, 拒绝 (DRAFT-const-mut §5.3 决议 #2)
//   - assert_eq:
//       * E6030: T 剥 Ref/Heap 后须是数值 / bool（与 compileTestAssertEq 同款）
//       * E6031: 两实参剥 Ref/Heap 后 LLVM 位宽组不一致（别名 resolveAlias；
//         灵活整数字面量按 T 收束）。isize/usize 与 i64/u64 同组（x64）
//
// 不覆盖:
//   - E6019 (size_of llvm getLLVMType 失败) — 依赖 LLVM, 留 codegen
//   - extractRawPtr 内 _localVarPtrs 查不到：sema 已校验 AST 形态，Compiler 改 throwSemaGap
//
// `file` / `sdkFile` 用于 copy_of 的 struct 字段深度递归; 为 nullptr 时按"找不到声明 → 保守放过"处理.
void validateBuiltinIntrinsicTypeShape(const string& fnName, const vector<TypeInfo>& typeArgs,
                                       const vector<TypeInfo>& argTypes, const vector<p<ExprNode>>& argNodes,
                                       FileNode* file, FileNode* sdkFile, int line, int col);

// Builtin 操作符方法的 arity / 类型域校验 (Phase 3.3.2.e).
//
// 覆盖 `compileBuiltinTypeMethodCall` 内 `isBuiltinMethod` 分支的纯校验:
//   - E6027: 二元 op arity != 1 (plus/minus/mul/div/mod/eq/ne/lt/le/gt/ge/and/or/xor/shl/shr)
//   - E3070: 一元 `inv` 不接受 float 类型
//
// 不命中 op 清单时 no-op (调用方继续 fall-through 到 IR emit).
// 纯字符串 + 计数 + baseType.isFloat(), 无 LLVM 依赖.
void validateOperatorMethodCall(const string& member, const TypeInfo& baseType, size_t argsCount, int line, int col);

// 自由内建 intrinsic 的 arity 校验 (Phase 3.3.2.b).
//
// 覆盖 compileExternalOrSdkFunctionCall 顶部的纯 arity 分派:
//   - E6027: assert_eq/assert_true/assert_false/fail/ptr_from_addr/rc_leak_count/_ptr_offset arity 不匹配
//
// 命中的 fnName 才会校验, 其他 fnName 是 no-op (调用方继续 fall-through 到
// fnSymbol / generic / external 路径). E6023 (_ptr_offset 跨模块私有) 与 E6006
// 语义重叠但错误码不同, 暂留 inline; 等 SemaPass 接管后再统一.
//
// 纯字符串 + size 比较, 无 LLVM 依赖.
void validateFreeIntrinsicArity(const string& fnName, size_t argsCount, int line, int col);

// Array<T> 方法调用的静态形态校验 (Phase 3.3.2.a).
// 查 kBuiltinMethods：E3055 / E6042 / E6027。未知方法名不抛错.
void validateArrayMethodCall(const TypeInfo& baseType, const string& member, size_t argsCount, bool baseIsLvalue,
                             int line, int col);

// Array 高阶方法的类型实参与 Function 形参检查；返回调用结果类型。
// map<U> 无显式 U 时从 Function<T&, U> 的返回类型反推。
TypeInfo validateArrayMethodTypes(const TypeInfo& baseType, const string& member,
                                  const vector<TypeInfo>& methodTypeArgs, const vector<TypeInfo>& argTypes, int line,
                                  int col);

// `_ptr_offset` 跨模块私有访问检查 (Phase 3.3.3.a, E6023).
//
// 私有 `_`-prefixed 内建 fn 仅允许在声明所在模块内调用. 语义上与 E6006
// (validateFnSymbolVisibility) 重叠, 但 yux 历史给 `_ptr_offset` 这条单独分配了
// 错误码 E6023 以便诊断更精确; 这里保留分支以保持错误码不变.
//
// 调用方:
//   - Compiler::compileExternalOrSdkFunctionCall 在 `_ptr_offset` 分支顶部调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validatePtrOffsetVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, int line, int col);

// struct method 跨可见性调用检查 (Phase 3.3.3.a, E6007).
//
// 私有方法 (符号表 `isPrivate=true`, 即 `_` 前缀) 只允许 `self` struct 自身调用.
// `currentStructName` 是调用方所在 impl 的 struct 名 (可能带 `$<泛型实例后缀>`,
// 由 helper 内部剥掉); 不在任何 struct 内时传空串, 此时任何私有方法调用都被拒.
//
// 调用方:
//   - Compiler::compileStructMethodCall 在 methodSymbol 命中且 isPrivate 时调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validateStructMethodVisibility(const FnSymbolInfo* methodSymbol, const string& currentStructName,
                                    const string& actualTypeName, const string& member, int line, int col);

// 函数符号可见性检查 (Phase 3.3.1.c, E6006).
//
// 当 `fnSymbol` 已解析到一个跨模块的私有函数 (`_`-prefixed) 时, 拒绝调用.
// 私有函数: `fnSymbol->isPrivate == true` 且 `moduleName` 非空且与当前 file 模块不同.
// `currentModuleName` 是调用点所在 file 的 moduleName (`file->moduleName()`).
//
// 调用方:
//   - Compiler::compileFunctionCall 在 fnSymbol 命中重载后立即调用
//   - SemaPass.visitExpr 在 ID-callee 路径解析到 fnSymbol 时调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validateFnSymbolVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, const string& fnName,
                                int line, int col);

// 枚举构造表达式形态校验 (Phase 3.4.a, E2019/E2020/E2021/E2032).
//
// 入口: `E::V` / `E::V(args)` 构造点. 调用前 node->setResolvedType(getType()) 已写好
// (Compiler::compileEnumCtorExpr 顶部 / SemaPass.visitExpr 顶部),
// 因此 helper 直接读 node->getType().name 得到经别名解析后的真实 enum 名.
//
// 抛错:
//   - E2019: enum 名找不到 (用 node->enumName().getText() 即用户写法填 payload)
//   - E2020: variant 名不在 enum 内
//   - E2021: arity 不匹配 (零参 / tuple-payload variant 严格相等)
//   - E2032: tuple-payload variant 第 i 个实参类型 != 声明 payload 类型,
//            payload 用 Rc<T> / Array<T> / [N]T 等用户友好形式渲染
//
// 实参类型经 argExpr->getType() 计算; 任一参数 getType 抛错时跳过该参数的 E2032 校验
// (典型: lambda 形参未推断 → E3001), 留给 codegen 路径继续报.
//
// 纯 AST / TypeInfo, 无 LLVM 依赖. 调用方:
//   - Compiler::compileEnumCtorExpr 在 setResolvedType 后立即调用
//   - SemaPass.visitExpr ExprPathCallNode 分支调用
void validateEnumCtorShape(FileNode* file, FileNode* sdkFile, p<ExprPathCallNode> node);

// match 表达式 arm 静态校验 (Phase 3.4.b).
//
// 在调用方已解析 scrutinee 的 enum 名 (含 rc-deref / alias) 并 lookup 到 enumDecl
// 之后调用. helper 一次性覆盖以下错误码:
//   - E2023: arms 空 (语法上 +, 防御性) / 不带 else 时穷尽性失败 (列缺失 variant)
//   - E2025: else arm 不在末位
//   - E2019: pattern 的 enum 名既不等于 enumName, 也不能经 file 上一步别名解析到 enumName
//   - E2020: variant 名不在 enumDecl 内
//   - E2024: 同一 variant 在多个 arm 中重复
//   - E2026: arm 绑定 arity 与 variant payload 声明 arity 不一致 (零参允许 0 binds)
//   - E2027: 同一 arm 内绑定名重复
//
// 不覆盖:
//   - E2022 (scrutinee 不是 enum / Rc<E> 仅借用语义) —— 调用方 (Compiler) 自身在
//     lookupEnum 失败时抛, 涉及 rc-deref / alias / isFreshHandleExpr; SemaPass 暂跳过
//   - E3027 (arm body 结果类型不一致) —— 跨 arm body getType 计算, 可能因 lambda
//     形参未推断而误判, 留 Compiler
//   - E3091/E3096 —— codegen 兜底
//
// 调用方:
//   - Compiler::compileMatchExpr 在 enumDecl 取到后立即调用
//   - SemaPass.visitExpr ExprMatchNode 分支主动调用 (scrutType 直接是 enum 名,
//     非 Rc/非 alias 时才接入; 否则跳过, 由 Compiler 兜底)
//
// 纯 AST / 字符串, 无 LLVM 依赖.
void validateMatchArms(EnumDeclNode* enumDecl, const TypeInfo& enumType, p<ExprMatchNode> node, FileNode* file);

// 私有字段可见性校验 (Phase 3.4.d.2).
//
// 单字段层级的低阶检查: 在调用方已经 resolve 到 structDecl + fieldName 之后调用.
// 字段非 private 时直接返回; 字段 private 且 accessor 名等于 owner (base) 名时
// 返回 (允许同 struct 内自访问); 否则抛 E3042.
//
// `accessorStructName`: 当前所在 struct impl 名 (Compiler 端可能含 `$<...>` 后缀
// — 来自 generic 实例化; helper 内部剥 `$` 后比对). SemaPass 端不下钻泛型 impl,
// 恒为非 `$<...>` 形态. 空串表示自由 fn (任何私有访问都报错).
//
// 调用方:
//   - Compiler::compileGetRefExpr 链式字段访问每段一次 (E3042 单点 throw 处)
//   - Compiler::compileDotExpr 字段访问命中私有时调用
//   - SemaPass.visitExpr ExprGetRefNode / ExprDotNode 分支
//
// 纯 AST / 字符串, 无 LLVM 依赖.
void validatePrivateFieldAccess(StructDeclNode* structDecl, const string& fieldName, const string& baseTypeName,
                                const string& accessorStructName, int line, int col);

// ExprGetRefNode 链式字段访问的可见性整桶校验 (Phase 3.4.d.2).
//
// 内部走 `compileGetRefExpr` 同款链路: scope.lookupSymbol(obj) → 剥 ref → 逐
// sub 解析 struct decl (含 rc-deref) → fieldIndex → validatePrivateFieldAccess.
// E3030/E3040/E3041/E3043/E3097 由 ExprGetRefNode::getType() 抛 (SemaPass 默认重抛),
// helper 仅补 E3042.
//
// scope 取自 node->findNearestScope(); 若拿不到则静默返回 (留 Compiler 兜底).
// SemaPass 调用时可传当前 `_currentStructName`, Compiler 调用时传当前
// `_currentStructName` (含 `$<...>` 后缀也行, helper 内部剥).
//
// 纯 AST / 字符串, 无 LLVM 依赖.
void validateGetRefPrivacy(FileNode* file, FileNode* sdkFile, p<ExprGetRefNode> node, const string& accessorStructName);

// ExprDotNode 单层字段访问可见性校验 (Phase 3.4.d.2).
//
// 仅处理非 safe (`.` 不是 `?.`) 路径的字段访问: 取 baseType (经 ref / Rc 剥),
// lookup struct decl, 若 fieldIndex >= 0 且字段 private 时校验. 路径与
// `compileDotExpr` 内的私有判定块对齐. baseType 取自 node->baseExpr()->getType().
//
// SemaPass 调用方传 `_currentStructName`; Compiler 类似.
//
// 纯 AST / 字符串, 无 LLVM 依赖.
void validateDotFieldPrivacy(FileNode* file, FileNode* sdkFile, p<ExprDotNode> node, const string& accessorStructName);

// 整数字面量解析 + 越界校验 (Phase 3.4.f.2).
//
// 原位于 compiler_expr.cpp 顶部 anonymous-ns, 现整体抠到 sema 共享.
// 识别 suffix (i8/i16/i32/i64/u8/u16/u32/u64) → 走 stoll/stoull 路径;
// 识别 base prefix (0b/0o/0x); 移下划线分隔符; out_of_range / invalid_argument
// 抛 E3103. `line=0` 时降级到 1, col 透传.
//
// 调用方:
//   - Compiler::compileLiteralExpr / compileArrayInitExpr int 分支
//   - SemaPass.visitExpr ExprLiteralNode 分支 (LiteralIntNode 命中时)
//
// 纯字符串解析, 无 LLVM / AST 依赖.
i64 parseIntLiteral(const string& text, int line = 0, int col = 0);

// Bucket 6 单点 (CURRENT-check.md): 比较表达式 leftType 形态校验.
//
// 覆盖:
//   - E3078: Weak<T> 不支持 == / != (DRAFT §5 v1 不暴露 handle 比较)
//   - E3073: Ptr 不支持 < / <= / > / >= (仅开放 == / !=)
//   - E3073: Function 值不支持 == / !=（§3.11.8）
//
// rightType 与 leftType 类型不匹配 (E3004) 已由 ExprCompareNode::getType 抢先抛
// (SemaPass 默认重抛), 这里仅做 leftType 单边形态校验.
//
// 调用方:
//   - SemaPass::tryValidateCompareForm（subst + peelAutoDeref 后；模板形参跳过）
//
// 纯 TypeInfo / 枚举判定, 无 LLVM 依赖.
void validateCompareOpForm(const TypeInfo& leftType, ExprCompareNode::Op op, int line, int col);

// Bucket 6 单点 (CURRENT-check.md): 二元运算符方法解析 (E3073 + byval hint).
//
// 镜像 compiler_expr.cpp::compileCustomTypeBinaryOp 内的 method lookup + 二次探测.
// 在调用方已剥 Ref / applySubst 得到 `leftType` / `rightType` 之后调用 (即 eff* 形态);
// 形参名 `methodName` 是底层名 (plus/minus/mul/div/mod/eq/ne/lt/le/gt/ge/and/or/xor/shl/shr).
//
// 行为:
//   - 优先按 `[leftType, Ref<rightType>]` 查 `<leftType>.<methodName>`; 命中 → 直接返回, 不抛.
//   - 命中失败时再试 `[leftType, rightType]` 二次探测:
//       * 命中 → 抛 E3073 + byval hint (告知用户应把形参改为 Self&).
//       * 仍不命中 → 抛 E3073 普通版.
//
// 调用方:
//   - SemaPass.visitExpr ExprAddSubNode / ExprMulDivModNode / ExprBinOpNode /
//     ExprCompareNode 分支, 仅在 leftType 为非 builtin / 非 Ref / 非容器 / 非泛型实例时调用.
//
// `sdkFile` 允许为 nullptr (无 SDK 上下文). 纯 AST 查表, 无 LLVM 依赖.
void validateBinOpMethodResolution(FileNode* file, FileNode* sdkFile, const TypeInfo& leftType,
                                   const TypeInfo& rightType, const string& methodName, int line, int col);

// 一元运算符自定义类型方法解析 (E3074).
//
// 与 Compiler::compileCustomTypeUnaryOp 同款查找: `Type.neg` / `Type.inv` / `Type.not`，
// 形参仅为接收者。找不到 → E3074。调用方负责剥 Ref/Heap/Rc、跳过 builtin / 泛型 struct。
//
// `sdkFile` 允许为 nullptr。纯 AST 查表，无 LLVM 依赖。
void validateUnaryOpMethodResolution(FileNode* file, FileNode* sdkFile, const TypeInfo& operandType,
                                     const string& methodName, int line, int col);

// String / ToString：`t` 是 String，或 file / sdkFile 有 `<TypeName>.to_string`。
// 与 compileStringPlusChain / validateStringTemplateInterps 同款查表。
bool typeImplementsToString(FileNode* file, FileNode* sdkFile, const TypeInfo& t);

// Bucket 6 单点 (CURRENT-check.md): 字符串模板插值类型校验 (E3026).
//
// 对每个 interp 计算 getType(), 若不是 String 且既不在 file 也不在 sdkFile
// 注册到 `<TypeName>.to_string` 自由函数 → 抛 E3026 (要求实现 ToString).
//
// interp getType 抛错时跳过该 interp (lambda 形参等), 留 Compiler 兜底.
// 泛型体 subst 后的复查由 SemaPass::tryValidateToString 做（本函数不 subst）。
//
// 调用方:
//   - SemaPass.visitExpr ExprLiteralNode/StringTemplate 分支（非 subst 路径仍可走）
//
// 纯 AST / 字符串查表, 无 LLVM 依赖.
void validateStringTemplateInterps(FileNode* file, FileNode* sdkFile, StringTemplateNode* tpl);

// Phase B：`Array:<T>::with_capacity(n)` 静态工厂形态校验。
//   - E6011: turbofish 必须恰好 1 个类型实参
//   - E3131: 恰好 1 个 usize 值实参（灵活整数按 usize 回填）
// Compiler::compileArrayWithCapacity 同款检查；SemaPass 接管后正常路径不可达。
void validateArrayWithCapacity(p<ExprPathCallNode> node);

} // namespace sema

#endif // YUX_LANG_SEMA_CALL_RESOLVE_H

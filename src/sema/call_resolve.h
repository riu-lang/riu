// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_CALL_RESOLVE_H
#define YUX_LANG_SEMA_CALL_RESOLVE_H

#include "ast/node/draft_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"

class Yux;

// 调用重载解析（Sema/Codegen 拆分 Phase 3.3 前置）
//
// 从 `src/compiler/compiler_call.cpp` 抠出, 不依赖 LLVM, 用于:
// - 当前: Compiler 在 compile 调用 / 构造点调用, 行为不变
// - 后续: SemaPass 在 visitCall 阶段调用, 提前完成歧义诊断 + 灵活整数推断
//
// 副作用: 唯一匹配时会通过 tryInferIntType 把灵活整数实参类型回填到 AST 节点;
//        因此整条流水线必须保证只调用一次, 否则推断会被重复执行 (幂等但浪费).
namespace sema {

// 解析普通函数 / 模块函数的重载.
// 命中多个候选时抛 E6014 (ambiguous overload).
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName,
                       const vector<p<ExprNode>>& args, int line);

// 解析构造器重载 (在符号表中以 `S.S` 注册, params[0] 是接收者).
// 命中多个候选时抛 E6014.
void resolveCtorOverload(FileNode* file, const string& structName,
                         const vector<p<ExprNode>>& args, int line);

// ctor 调用形态校验 (Phase 3.3 前置.3b).
//
// 在 callee 已经解析到一个 structDecl 后, 校验调用形态:
//   - E6008: 结构体名以 `_` 开头 (私有), 不允许跨可见性构造
//   - E6009: 泛型结构体调用时未给类型实参
//
// `hasTypeArgs` 为调用点是否携带显式类型实参 (`Foo:<i32>(...)` 形态).
// 非泛型结构体不查 hasTypeArgs (yux 当前不允许给非泛型类型带 typeArgs).
//
// 调用方:
//   - Compiler::compileFunctionCall 在解析 structDecl 后立即调用
//   - SemaPass::visitExpr 在 ExprCallNode 分支识别 ctor 时调用
void validateCtorCallShape(StructDeclNode* structDecl, const string& fnName,
                           bool hasTypeArgs, int line, int col);

// 泛型函数 / 泛型构造器调用点的类型实参 arity 校验 (Phase 3.3 前置.3c).
//
// 当调用点写了显式 `:<T1, T2, ...>` 时, 检查 arity 与声明的 typeParams 是否匹配.
// 不匹配抛 E6010, 带 `.withHint(...)` 提示用户如何补全.
//
// 注意:
//   - 仅在 explicitTypeArgsCount > 0 时调用 (无 typeArgs 走类型推断, 不是 arity 错).
//   - 调用方需自行确认 callee 已识别为泛型 fn / 泛型 struct.
void validateGenericTypeArgsArity(const string& fnName,
                                  size_t expectedCount,
                                  size_t actualCount,
                                  int line, int col);

// Dyn<D> 方法调用的静态形态校验 (Phase 3.3 前置.3f).
//
// 调用方 (Compiler::compileDynMethodCall) 在解析到 draftDecl 后调用.
// 校验:
//   - E6016: draftDecl.signatures 内找不到名为 member 的签名
//   - E6012: 找到签名但 params arity 与 argTypes 不一致
//   - E6015: 形参类型 (按 draft 签名 getFullName 比对) 与实参不一致, 带 hint
//
// 命中 (合法) 时返回找到的 `FnHeaderNode*`, 让 codegen 走 vtable indirect call.
// 纯 AST / TypeInfo, 无 LLVM 依赖.
//
// `draftQualified` 仅用于 E6015 hint 的人类可读名 (e.g. "yux.core.io.Writer").
FnHeaderNode* resolveDynMethodSig(DraftDeclNode* draftDecl,
                                  const string& draftQualified,
                                  const TypeInfo& baseType,
                                  const string& member,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col);

// ctor 重载未匹配的诊断 (Phase 3.3 前置.3e).
//
// 调用方在 ctor 重载解析路径 (compileConstructorCall) 返回 null 后调用:
// 即 callee 已识别为 structDecl, 但既有 ctor 重载均不接受当前 argTypes.
// 函数总是抛 E6033, 附带:
//   - 当前 file + sdkFile 上 `S.S` 的所有重载签名 (跳过 params[0] 接收者), 按
//     Box<T> / Array<T> / [N]T 等用户友好形式渲染
//   - 实参的同款友好渲染列表
//   - `.withHint(...)` 提示如何修正
//
// 纯类型 / 纯 AST, 无 LLVM 依赖.
void diagnoseCtorOverloadMismatch(FileNode* file, FileNode* sdkFile,
                                  const string& fnName,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col);

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
void checkBangWithoutFallibleCaller(FnNode* currentFnNode, p<ExprCallNode> callNode);

// ID-callee 错误传播语义校验 (Phase 10e/10f).
//
// 抛错码:
//   - E7001: caller 非 #Fallible 且不在 try block 内, 但调用点写了 `!`
//   - E7004: caller/callee 错误类型不一致, 同 `!` 不可跨类型透传
//   - E7006: callee 是 #Fallible 但调用点未加 `!`, 且不在 try block 内
//   - E7016: try block 内 `!` 冗余 (TODO: 接 warning 通道; 当前 silent)
//
// 副作用: 当 tryBlockSeenErrs != nullptr 且 callee 的 fallibleErrType 非空时,
// 把 calleeErr 追加进 vector —— 用于 try block 的 E7002/E7015 穷尽性判定.
//
// 当前 Compiler 端是唯一调用方 (在 compileCallExpr 入口); SemaPass 暂未跟踪
// try block 栈, 不能直接调用 (传 nullptr 会对 try 内 `foo()!` 误报 E7001).
// 等 SemaPass 落 try block visit 后再接入.
void checkErrPropagateForIdCall(FnNode* currentFnNode,
                                p<ExprCallNode> callNode,
                                const string& fnName,
                                const FnSymbolInfo* calleeSym,
                                vector<string>* tryBlockSeenErrs);

// 包/模块别名调用解析 (Phase 3.3.1.a).
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
};

ModuleFnCallResult resolveModuleFnCall(FileNode* file, Yux* yux,
                                       p<ExprCallNode> callNode,
                                       p<ExprDotNode> dotNode,
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
void inferGenericFnTypeArgs(p<ExprCallNode> callNode, p<FnNode> genericFn,
                            const string& fnName,
                            const vector<TypeInfo>& argTypes,
                            vector<TypeInfo>& outTypeArgs);

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
void validateFnSymbolVisibility(const FnSymbolInfo* fnSymbol,
                                const string& currentModuleName,
                                const string& fnName,
                                int line, int col);

} // namespace sema

#endif //YUX_LANG_SEMA_CALL_RESOLVE_H

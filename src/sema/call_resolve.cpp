// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 调用重载解析实现 (Sema/Codegen 拆分 Phase 3.3 前置)
//
// 本文件内容原位于 `src/compiler/compiler_call.cpp` 顶部, 现整体迁移到 sema 层:
// - paramAccepts / overloadMatchesDefault / overloadMatchesFlexible: 纯 TypeInfo 匹配
// - resolveFnOverload / resolveCtorOverload: 重载解析 + 灵活整数推断 + E6014 歧义诊断
//
// 不依赖任何 LLVM 头; 由 yux_frontend 静态库提供, Compiler 与未来的 SemaPass 共享.

#include "sema/call_resolve.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/node/enum_node.h"
#include "ast/yux.h"
#include "tools/diagnostic.h"
#include "types.h"
#include <algorithm>
#include <format>
#include <functional>
#include <regex>

namespace sema {

// ==================== 辅助函数: 参数类型匹配 ====================

// 检查参数类型是否可以接受
// 支持精确匹配和引用类型匹配
static bool paramAccepts(const TypeInfo& param, const TypeInfo& argType) {
    if (param == argType) return true; // 精确匹配
    // 不再隐式取 ref：T 与 T& 是不同的类型，各有各的重载
    // 需要引用时在调用处用显式 &arg（而非 &expr）
    if (param.isPtr() && argType.isRef()) return true; // 指针参数接受引用
    return false;
}

// 灵活的函数重载匹配
// 对灵活整数字面量 (如 42) 允许匹配任何整数类型
static bool overloadMatchesFlexible(const vector<p<ExprNode>>& args, const vector<TypeInfo>& params) {
    if (params.size() != args.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (isFlexibleIntExpr(args[i])) {
            // 灵活整数可以匹配任何整数类型
            if (isIntTypeName(params[i].name)) continue;
            try {
                if (paramAccepts(params[i], args[i]->getType())) continue;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            return false;
        }
        try {
            if (!paramAccepts(params[i], args[i]->getType())) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

// 默认的函数重载匹配
// 灵活整数字面量默认匹配 i32
static bool overloadMatchesDefault(const vector<p<ExprNode>>& args, const vector<TypeInfo>& params) {
    if (params.size() != args.size()) return false;
    TypeInfo i32Type("i32");
    for (size_t i = 0; i < args.size(); ++i) {
        TypeInfo argType;
        if (isFlexibleIntExpr(args[i])) {
            argType = i32Type; // 灵活整数默认为 i32
        } else {
            try {
                argType = args[i]->getType();
            } catch (...) {
                return false;
            }
        }
        if (!paramAccepts(params[i], argType)) return false;
    }
    return true;
}

// ==================== 构造函数重载解析 ====================
// 与 resolveFnOverload 同思路，但 ctor 在符号表中以 `S.S` 注册，且 params[0] 是
// 接收者（结构体类型本身）。匹配时跳过 params[0]，按用户写的实参列表推断未带后缀
// 的整数字面量类型，避免后续在 LLVM 后端因 i32→i64 形参不匹配而走到外部函数路径
// 触发 `isSized` 断言（见 BUGS.md「构造函数 i64 形参传 untyped int 字面量」）。
void resolveCtorOverload(FileNode* file, const string& structName, const vector<p<ExprNode>>& args, int line) {
    string ctorFullName = structName + "." + structName;
    vector<FnSymbolInfo*> candidates;
    file->collectFnOverloads(ctorFullName, candidates);
    if (candidates.empty()) return;

    auto matchesDefault = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        TypeInfo i32Type("i32");
        for (size_t i = 0; i < args.size(); ++i) {
            TypeInfo argType;
            if (isFlexibleIntExpr(args[i])) {
                argType = i32Type;
            } else {
                try {
                    argType = args[i]->getType();
                } catch (...) {
                    return false;
                }
            }
            if (!paramAccepts(c->params[i + 1], argType)) return false;
        }
        return true;
    };
    auto matchesFlexible = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i])) {
                if (isIntTypeName(c->params[i + 1].name)) continue;
                try {
                    if (paramAccepts(c->params[i + 1], args[i]->getType())) continue;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                return false;
            }
            try {
                if (!paramAccepts(c->params[i + 1], args[i]->getType())) return false;
            } catch (...) {
                return false;
            }
        }
        return true;
    };

    vector<FnSymbolInfo*> defaultMatches;
    for (auto c : candidates)
        if (matchesDefault(c)) defaultMatches.push_back(c);

    vector<FnSymbolInfo*> matches;
    if (defaultMatches.empty()) {
        for (auto c : candidates)
            if (matchesFlexible(c)) matches.push_back(c);
    } else {
        matches = defaultMatches;
    }

    if (matches.size() == 1) {
        // 唯一匹配：把每个灵活整数实参推断到对应 ctor 形参类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->params[i + 1].name)) {
                tryInferIntType(args[i], fn->params[i + 1]);
            }
        }
    } else if (matches.size() > 1) {
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + structName + "(";
            for (size_t i = 1; i < m->params.size(); ++i) {
                if (i > 1) sigs += ", ";
                sigs += m->params[i].name;
            }
            sigs += ")";
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try {
                argSigs += args[i]->getType().name;
            } catch (...) {
                argSigs += "?";
            }
        }
        throw YuxError(line, ErrorCode::E6014, structName, argSigs, matches.size(), sigs);
    }

    // candidates 非空但无任何匹配 (同 resolveFnOverload 的 BUGS.md #3)
    if (!candidates.empty()) {
        // ctor 符号表注册格式 `S.S`, params[0] 是接收者; 用户可见形参从 index 1 起
        bool anyArityMatches = false;
        for (auto c : candidates) {
            if (c->params.size() - 1 == args.size()) {
                anyArityMatches = true;
                break;
            }
        }
        if (!anyArityMatches) {
            set<size_t> arities;
            for (auto c : candidates) {
                arities.insert(c->params.size() - 1);
            }
            if (arities.size() == 1) {
                size_t expected = *arities.begin();
                throw YuxError(line, ErrorCode::E6027, structName, expected)
                    .withHint(std::format("期望 {} 个实参，实际 {} 个", expected, args.size()));
            }
            string sigs;
            for (auto c : candidates) {
                sigs += "\n  " + structName + "(";
                for (size_t i = 1; i < c->params.size(); ++i) {
                    if (i > 1) sigs += ", ";
                    sigs += c->params[i].name;
                }
                sigs += ")";
            }
            throw YuxError(line, ErrorCode::E6027, structName, *arities.begin())
                .withHint(std::format("实参 {} 个，候选重载有 {} 种参数个数；声明的重载:{}", args.size(),
                                      arities.size(), sigs));
        }
    }
}

// ==================== 函数重载解析 ====================
// 解析函数重载，确定应该调用哪个版本
// 如果有歧义，抛出错误要求用户添加类型后缀
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName, const vector<p<ExprNode>>& args,
                       int line) {
    (void)sdkFile;
    vector<FnSymbolInfo*> candidates;
    file->collectFnOverloads(fnName, candidates);
    if (candidates.empty()) return;

    // 首先尝试默认匹配 (灵活整数 -> i32)
    vector<FnSymbolInfo*> defaultMatches;
    for (auto c : candidates) {
        if (overloadMatchesDefault(args, c->params)) defaultMatches.push_back(c);
    }

    vector<FnSymbolInfo*> matches;
    if (defaultMatches.empty()) {
        // 如果默认匹配失败，尝试灵活匹配
        for (auto c : candidates) {
            if (overloadMatchesFlexible(args, c->params)) matches.push_back(c);
        }
    } else {
        matches = defaultMatches;
    }

    if (matches.size() == 1) {
        // 唯一匹配: 推断灵活整数的类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->params[i].name)) {
                tryInferIntType(args[i], fn->params[i]);
            }
        }
    } else if (matches.size() > 1) {
        // 多个匹配: 报告歧义错误
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + fnName + "(";
            for (size_t i = 0; i < m->params.size(); ++i) {
                if (i) sigs += ", ";
                sigs += m->params[i].name;
            }
            sigs += ")";
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try {
                argSigs += args[i]->getType().name;
            } catch (...) {
                argSigs += "?";
            }
        }
        throw YuxError(line, ErrorCode::E6014, fnName, argSigs, matches.size(), sigs);
    }

    // candidates 非空但无任何匹配: 仅当实参个数与所有候选形参个数都不一致
    // 时才报 E6027——这是最常见的原因（漏写/多写实参，BUGS.md #3）。
    // 若个数匹配但类型不匹配，留给 codegen 按原路径报类型错误。
    {
        bool anyArityMatches = false;
        for (auto c : candidates) {
            if (c->params.size() == args.size()) {
                anyArityMatches = true;
                break;
            }
        }
        if (!anyArityMatches) {
            // 所有候选的形参个数都与实参个数不一致
            set<size_t> arities;
            for (auto c : candidates) {
                arities.insert(c->params.size());
            }
            if (arities.size() == 1) {
                size_t expected = *arities.begin();
                throw YuxError(line, ErrorCode::E6027, fnName, expected)
                    .withHint(std::format("期望 {} 个实参，实际 {} 个", expected, args.size()));
            }
            string sigs;
            for (auto c : candidates) {
                sigs += "\n  " + fnName + "(";
                for (size_t i = 0; i < c->params.size(); ++i) {
                    if (i) sigs += ", ";
                    sigs += c->params[i].name;
                }
                sigs += ")";
            }
            throw YuxError(line, ErrorCode::E6027, fnName, *arities.begin())
                .withHint(std::format("实参 {} 个，候选重载有 {} 种参数个数；声明的重载:{}", args.size(),
                                      arities.size(), sigs));
        }
        // 个数匹配但类型不匹配 → 留 codegen 兜底
    }
}

// ==================== 泛型类型实参 arity 校验 (Phase 3.3 前置.3c) ====================
// 原 compileCallExpr (line 434) 与 compileGenericFunctionCall (line 938) 两处重复的
// E6010 throw + .withHint 抠成 sema 共享; SemaPass 同步接入.
void validateGenericTypeArgsArity(const string& fnName, size_t expectedCount, size_t actualCount, int line, int col) {
    if (expectedCount == actualCount) return;
    throw YuxError(line, col, ErrorCode::E6010, fnName, expectedCount, actualCount)
        .withHint(std::format("调用处的类型实参个数需与声明匹配；改写为 `{}:<{}>(...)` 形式补齐 {} 个类型", fnName,
                              std::string(expectedCount == 1 ? "T" : "T1, T2, ..."), expectedCount));
}

// ==================== Dyn 方法静态形态校验 (Phase 3.3 前置.3f) ====================
// 原位于 `compiler/compiler_call.cpp::compileDynMethodCall` 第 2 / 3 / 4 步:
// 按名查 sig (E6016) → arity (E6012) → 形参类型 (E6015). 纯 AST + TypeInfo,
// 抠到 sema 层后 Compiler 继续走 codegen, SemaPass 后续可在 Dyn 调用点提前调.
FnHeaderNode* resolveDynMethodSig(SpecDeclNode* specDecl, const string& specQualified, const TypeInfo& baseType,
                                  const string& member, const vector<TypeInfo>& argTypes, int line, int col) {
    // 2. 找方法签名 (按名匹配; yux 暂无方法名重载, 第一处即终)
    FnHeaderNode* sig = nullptr;
    for (auto& s : specDecl->signatures()) {
        if (!s) continue;
        if (s->name().getText() != member) continue;
        sig = s;
        break;
    }
    if (!sig) {
        throw YuxError(line, col, ErrorCode::E6016, member, baseType.getFullName());
    }

    // 3. arity 校验
    if (sig->params().size() != argTypes.size()) {
        throw YuxError(line, col, ErrorCode::E6012, member, static_cast<int>(sig->params().size()),
                       static_cast<int>(argTypes.size()));
    }

    // 4. 参数类型按 D 签名比对 (类型名 + 全名相等; 与 yux 名义类型一致)
    for (size_t i = 0; i < sig->params().size(); ++i) {
        auto sp = sig->params()[i];
        if (!sp || !sp->type()) continue;
        TypeInfo expected = sp->type()->getType();
        if (expected.getFullName() != argTypes[i].getFullName()) {
            std::string hint = "Dyn<";
            hint += specQualified;
            hint += ">.";
            hint += member;
            hint += " arg#";
            hint += std::to_string(i);
            hint += ": 期望 ";
            hint += expected.getFullName();
            hint += ", 实际 ";
            hint += argTypes[i].getFullName();
            hint += " (Dyn 方法调用参数类型按 draft 签名静态匹配)";
            throw YuxError(line, col, ErrorCode::E6015).withHint(hint);
        }
    }
    return sig;
}

// ==================== 非-ID callee `!` fallback 校验 (Phase 3.3 前置.3d) ====================
// 原 compileCallExpr 入口两处 else 分支的内联 E7001 throw 抠成共享 helper.
// 调用方 (Compiler) 已确认: callee 非 ID-literal + `errPropagate()` + 不在 try block.
void checkBangWithoutFallibleCaller(FnNode* currentFnNode, p<ExprCallNode> callNode) {
    string callerErr;
    if (currentFnNode) {
        if (auto eOpt = currentFnNode->header()->getAnnoArg("Fallible")) {
            callerErr = *eOpt;
        }
    }
    if (callerErr.empty()) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
    }
}

// ==================== ID-callee 错误传播校验 (Phase 10e/10f) ====================
// 原位于 `src/compiler/compiler_call.cpp` 的 static 自由函数, 形参从
// `Compiler::TryCatchCtx*` 改成 `vector<string>* tryBlockSeenErrs` 以解开 LLVM 耦合
// (原 TryCatchCtx 内含 llvm::BasicBlock*, 函数体只读 seenErrTypes).
void checkErrPropagateForIdCall(FnNode* currentFnNode, p<ExprCallNode> callNode, const string& fnName,
                                const FnSymbolInfo* calleeSym, vector<string>* tryBlockSeenErrs,
                                const string& sourcePath) {
    bool hasBang = callNode->errPropagate();
    string callerErr;
    if (currentFnNode) {
        if (auto eOpt = currentFnNode->header()->getAnnoArg("Fallible")) {
            callerErr = *eOpt;
        }
    }
    string calleeErr = calleeSym ? calleeSym->fallibleErrType : "";

    // Phase 10f: try block 内的 #Fallible 调用 → 路由到 catch 子句
    if (tryBlockSeenErrs && !calleeErr.empty()) {
        tryBlockSeenErrs->push_back(calleeErr);
        if (hasBang) {
            // E7016: try block 内 ! 冗余 (语义不变, 警告; 默认 Warning,
            // -Werror / --deny=E7016 升级为 Error 时 emit 内部会 rethrow).
            // emit 按 (file, code, line, col, msg) 去重.
            DiagnosticEngine::emit(sourcePath, YuxError(callNode->getLineNumber(), callNode->getColumn(),
                                                        ErrorCode::E7016, calleeErr, fnName, calleeErr));
        }
        return;
    }

    if (hasBang) {
        // E7001: caller 不在 #Fallible(E) 函数内 + 不在 try block 内
        if (callerErr.empty()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
        }
        // E7004: caller / callee 错误类型不一致 (同 ! 不可跨类型透传)
        if (!calleeErr.empty() && calleeErr != callerErr) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7004, calleeErr, callerErr,
                           calleeErr, callerErr, calleeErr);
        }
        // callee 不是 #Fallible 但写了 ! : 暂不在 10e/10f 报; 保留给后续考虑
    } else {
        // E7006: 调用 #Fallible 函数但未加 ! (不在 try block 内)
        if (!calleeErr.empty()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7006, fnName);
        }
    }
}

// ==================== 包/模块别名调用解析 (Phase 3.3.1.a) ====================
// 原 `compileMethodCall` line 195-254 的两个 inline 块 (包别名 + 模块别名)
// 抠到 sema 层. 命中其中一种时返回 {matched=true, fnName, fnSym}, 调用方
// 走 compileKnownFunctionCall; 未命中返回 {matched=false} 由调用方继续.
// 纯 AST 符号查 + 字符串拼接, 无 LLVM 依赖.
ModuleFnCallResult resolveModuleFnCall(FileNode* file, Yux* yux, p<ExprCallNode> callNode, p<ExprDotNode> dotNode,
                                       const vector<TypeInfo>& argTypes) {
    ModuleFnCallResult result;
    auto member = dotNode->member();

    // 包别名调用: package.module.fn(args)
    {
        string aliasName;
        vector<string> segs;
        if (ExprDotNode::parseChain(dotNode, aliasName, segs) && segs.size() >= 2) {
            auto aliasSym = file->lookupSymbol(aliasName);
            if (aliasSym && (aliasSym->kind == SymbolKind::Package || aliasSym->kind == SymbolKind::Module) &&
                file->isAmbiguousAlias(aliasName)) {
                file->throwAmbiguousAlias(aliasName, callNode->getLineNumber());
            }
            if (aliasSym && aliasSym->kind == SymbolKind::Package) {
                string childKey;
                for (size_t i = 0; i + 1 < segs.size(); ++i) {
                    if (i) childKey += ".";
                    childKey += segs[i];
                }
                auto* target = file->packageChild(aliasName, childKey);
                if (!target) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6001, childKey,
                                   aliasSym->moduleName);
                }
                const string& fnName = segs.back();
                auto* fnSym = target->lookupFnSymbolWithParams(fnName, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6002, fnName,
                                   aliasSym->moduleName + "." + childKey);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6003, fnName);
                }
                result.matched = true;
                result.fnName = fnName;
                result.fnSym = fnSym;
                return result;
            }
        }
    }

    // 模块别名调用: module.fn(args)
    // yux 为 nullptr 时整段路径跳过 (SemaPass 早期可能拿不到 Yux*, 留给 Compiler 兜底).
    if (auto baseLit = dynamic_cast<ExprLiteralNode*>(dotNode->baseExpr()); yux && baseLit) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
            auto aliasName = objLit->getValue().getText();
            auto aliasSym = file->lookupSymbol(aliasName);
            if (aliasSym && aliasSym->kind == SymbolKind::Module) {
                auto targetMod = yux->module(aliasSym->moduleName);
                if (!targetMod) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6005,
                                   aliasSym->moduleName, aliasName);
                }
                auto* fnSym = targetMod->lookupFnSymbolWithParams(member, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6002, member,
                                   aliasSym->moduleName);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6004, member);
                }
                result.matched = true;
                result.fnName = member;
                result.fnSym = fnSym;
                return result;
            }
        }
    }

    return result;
}

// ==================== 泛型函数 typeArgs 推断 (Phase 3.3.1.b) ====================
// 原 `compileGenericFunctionCall` 的 else 分支 (无显式 typeArgs 路径) 整体抠出.
// arity / 推断 / unify 全部纯 TypeInfo, 无 LLVM 依赖.
void inferGenericFnTypeArgs(p<ExprCallNode> callNode, p<FnNode> genericFn, const string& fnName,
                            const vector<TypeInfo>& argTypes, vector<TypeInfo>& outTypeArgs) {
    const auto& typeParams = genericFn->header()->typeParams();
    auto params = genericFn->header()->params();
    if (params.size() != argTypes.size()) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6012, fnName, params.size(),
                       argTypes.size());
    }

    map<string, TypeInfo> inferred;
    // 递归 unify: 形参 pType 与实参 aType 匹配; 遇到形如 T 的裸类型形参则记录推断
    std::function<void(const TypeInfo&, const TypeInfo&)> unify = [&](const TypeInfo& pType, const TypeInfo& aType) {
        if (pType.isNormal() && !isBuiltinType(pType.name)) {
            for (auto& tp : typeParams) {
                if (pType.name == tp) {
                    inferred[tp] = aType;
                    return;
                }
            }
        }
        // Generic vs Generic: 同名同元数则递归各 typeArg
        if (pType.kind == TypeKind::Generic && aType.kind == TypeKind::Generic && pType.name == aType.name &&
            pType.genericArgs.size() == aType.genericArgs.size()) {
            for (size_t i = 0; i < pType.genericArgs.size(); ++i) {
                if (pType.genericArgs[i] && aType.genericArgs[i]) {
                    unify(*pType.genericArgs[i], *aType.genericArgs[i]);
                }
            }
        }
    };
    for (size_t i = 0; i < params.size(); ++i) {
        auto paramType = params[i]->type();
        if (!paramType) continue;
        unify(paramType->getType(), argTypes[i]);
    }

    for (auto& tp : typeParams) {
        auto it = inferred.find(tp);
        if (it == inferred.end()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6013, tp, fnName);
        }
        outTypeArgs.push_back(it->second);
    }
}

// 泛型重载消歧 (Phase 3.3.1.a+)：从多个同名泛型函数中选最佳匹配。
// 逐个 inferGenericFnTypeArgs 推断 typeArgs，成功则按形参/实参 Ref 一致性打分。
std::pair<FnNode*, FileNode*> resolveBestGenericOverload(const std::vector<std::pair<FnNode*, FileNode*>>& genericFns,
                                                         p<ExprCallNode> callNode, const std::string& fnName,
                                                         const std::vector<TypeInfo>& argTypes) {
    if (genericFns.empty()) return {nullptr, nullptr};
    if (genericFns.size() == 1) return genericFns[0];

    FnNode* best = genericFns[0].first;
    FileNode* bestOwner = genericFns[0].second;
    int bestScore = -1;
    for (auto& [gFn, gOwner] : genericFns) {
        vector<TypeInfo> trial;
        bool ok = true;
        try {
            inferGenericFnTypeArgs(callNode, gFn, fnName, argTypes, trial);
        } catch (...) {
            ok = false;
        }
        if (!ok) continue;
        int score = 0;
        auto gp = gFn->header()->params();
        for (size_t i = 0; i < gp.size() && i < argTypes.size(); ++i) {
            if (!gp[i]->type()) continue;
            bool pRef = gp[i]->type()->getType().isRef();
            bool aRef = argTypes[i].isRef();
            if (pRef == aRef) score++;
        }
        if (score > bestScore) {
            best = gFn;
            bestOwner = gOwner;
            bestScore = score;
        }
    }
    return {best, bestOwner};
}

// ==================== 函数符号可见性 (Phase 3.3.1.c) ====================
// 原 `compileFunctionCall` line 770 的 inline 块, 仅一行条件 + E6006 throw.
// 抽出供 SemaPass 与 Compiler 共用; 纯字符串比较.
// ==================== 泛型 typeArgs draft 边界 (Phase 3.3.3.c, E3032 / E1106) ====================
void validateGenericTypeArgsSpecBound(const SpecRegistry* registry, const SpecImplChecker* checker, FileNode* fnOwner,
                                      FnHeaderNode* header, const vector<TypeInfo>& typeArgs, int line, int col) {
    if (!registry || !checker || !header || !fnOwner) return;
    const auto& bounds = header->typeParamBounds();
    if (bounds.empty()) return;
    const auto& typeParams = header->typeParams();
    for (size_t i = 0; i < typeParams.size() && i < bounds.size() && i < typeArgs.size(); ++i) {
        for (auto& boundName : bounds[i]) {
            auto resolved = registry->resolve(boundName, fnOwner);
            if (!resolved) {
                throw YuxError(line, col, ErrorCode::E3030, boundName);
            }
            // v0.5: 函数声明位 specBound 暂未携带类型实参 (ast_builder 仅取基名),
            // specTypeArgs 传空; 草案 §6.4.4.1 文法允许 `D<T>` 形态留待扩展.
            vector<TypeInfo> specTypeArgs;
            if (!checker->boundSatisfied(typeArgs[i], resolved->decl, resolved->qualifiedName, specTypeArgs)) {
                throw YuxError(line, col, ErrorCode::E1106, typeArgs[i].getFullName(), resolved->qualifiedName,
                               typeParams[i]);
            }
        }
    }
}

// ==================== Dyn callee draft 解析 (Phase 3.3.3.b, E1131) ====================
DynCalleeResolved resolveDynCalleeSpec(const SpecRegistry* registry, FileNode* visibleFrom, const TypeInfo& baseType,
                                       int line, int col) {
    auto specInner = baseType.dynSpecType();
    string specBare = specInner ? specInner->name : string();
    DynCalleeResolved out;
    if (registry && visibleFrom && !specBare.empty()) {
        if (auto resolved = registry->resolve(specBare, visibleFrom)) {
            out.decl = resolved->decl;
            out.qualified = resolved->qualifiedName;
            return out;
        }
    }
    throw YuxError(line, col, ErrorCode::E1131, specBare.empty() ? string("?") : specBare);
}

// ==================== _ptr_offset 跨模块私有 (Phase 3.3.3.a, E6023) ====================
void validatePtrOffsetVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, int line, int col) {
    if (!fnSymbol) return;
    if (fnSymbol->isPrivate && !fnSymbol->moduleName.empty() && fnSymbol->moduleName != currentModuleName) {
        throw YuxError(line, col, ErrorCode::E6023);
    }
}

// ==================== struct method 私有可见性 (Phase 3.3.3.a, E6007) ====================
void validateStructMethodVisibility(const FnSymbolInfo* methodSymbol, const string& currentStructName,
                                    const string& actualTypeName, const string& member, int line, int col) {
    if (!methodSymbol || !methodSymbol->isPrivate) return;
    string currentBase = currentStructName;
    auto dollarPos = currentBase.find('$');
    if (dollarPos != string::npos) currentBase = currentBase.substr(0, dollarPos);
    if (currentBase != actualTypeName) {
        throw YuxError(line, col, ErrorCode::E6007, member, actualTypeName);
    }
}

void validateFnSymbolVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, const string& fnName,
                                int line, int col) {
    if (!fnSymbol) return;
    if (!fnSymbol->isPrivate || fnSymbol->moduleName.empty() || fnSymbol->moduleName == currentModuleName) return;
    // SDK 平铺文件（yux.core.*）之间允许私有调用（拆分前它们同属 yux.core 模块）
    auto isSdkFlat = [](const string& mod) -> bool { return mod.starts_with("yux.core.") || mod == "yux.core"; };
    if (isSdkFlat(fnSymbol->moduleName) && isSdkFlat(currentModuleName)) return;
    throw YuxError(line, col, ErrorCode::E6006, fnName);
}

// ==================== Builtin intrinsic arity (Phase 3.3.2.c) ====================
// 原 compileGenericFunctionCall 的 #Builtin 分支顶部散落的 typeArgs/args
// 计数检查 (~12 处 throw 跨 7 个 fnName) 收口到单一 helper.
void validateBuiltinIntrinsicShape(const string& fnName, size_t typeArgsCount, size_t argsCount, int line, int col) {
    if (fnName == "assert_eq") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "size_of") {
        if (typeArgsCount == 0) throw YuxError(line, col, ErrorCode::E6018);
        return;
    }
    if (fnName == "upgrade") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "same_ref" || fnName == "ptr_of") {
        size_t expectedArgs = (fnName == "same_ref" ? 2u : 1u);
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != expectedArgs) {
            throw YuxError(line, col, ErrorCode::E6027, fnName, expectedArgs);
        }
        return;
    }
    if (fnName == "as_ref" || fnName == "copy_of" || fnName == "weak") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // DRAFT-heap-types §8.3a.4.2 (Phase 3d): Heap<T>? 构造助手
    if (fnName == "heap_some") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "heap_null") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 0) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    // DRAFT-spec-reflect Phase 3a (捷径 A): __yux_reflect_type:<T>() 拿反射 Type 节点
    if (fnName == "__yux_reflect_type") {
        if (typeArgsCount != 1) throw YuxError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 0) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    // 未知 Builtin intrinsic
    throw YuxError(line, col, ErrorCode::E6017, fnName);
}

// ==================== 自由内建 intrinsic arity (Phase 3.3.2.b) ====================
// 原 `compileExternalOrSdkFunctionCall` 顶部三处 inline 分派 (line 724-752) 收口.
// 仅命中清单内的 fnName 才校验, 其他 fnName 是 no-op.
void validateFreeIntrinsicArity(const string& fnName, size_t argsCount, int line, int col) {
    if (fnName == "ptr_from_addr") {
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "rc_leak_count") {
        if (argsCount != 0) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    if (fnName == "_ptr_offset") {
        if (argsCount != 2) throw YuxError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(2));
        return;
    }
}

// ==================== Array<T> 方法形态校验 (Phase 3.3.2.a) ====================
// 原 `compileArrayMethodCall` 散落的 6 处 throw 收口为一个 helper.
// 调用方仅需在函数顶部传 (baseType, member, argsCount, baseIsLvalue) 即可一次性校验.
void validateArrayMethodCall(const TypeInfo& baseType, const string& member, size_t argsCount, bool baseIsLvalue,
                             int line, int col) {
    // len / cap 在 RC 头, 不需要 elemType, 也不需要 lvalue
    if (member == "len" || member == "cap") return;

    auto elemType = baseType.arrayGenericElementType();
    if (!elemType) {
        throw YuxError(line, col, ErrorCode::E3050);
    }

    if (member == "is_empty" || member == "first" || member == "last") return;
    if (member == "get") {
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, member, static_cast<size_t>(1));
        return;
    }
    if (member == "pop") {
        if (!baseIsLvalue) throw YuxError(line, col, ErrorCode::E6042, member);
        return;
    }
    if (member == "push" || member == "set_len" || member == "clear") {
        if (!baseIsLvalue) {
            throw YuxError(line, col, ErrorCode::E6042, member);
        }
        if (member == "clear") return;
        if (member == "set_len") {
            if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, member, static_cast<size_t>(1));
            return;
        }
        // push
        if (argsCount != 1) throw YuxError(line, col, ErrorCode::E6027, member, static_cast<size_t>(1));
        return;
    }
    // 未知 member: 由 Compiler 端返回 nullptr fall-through 到 builtin/sdk 方法路径
}

// ==================== Builtin intrinsic 类型形态校验 (Phase 3.3.2.d) ====================
// 原 compileGenericFunctionCall 的 #Builtin 分支内散落的 E6028 / E6029 / E6032
// 校验 (跨 same_ref / ptr_of / as_ref / weak / copy_of 五个 fnName) 收口到单一 helper.
void validateBuiltinIntrinsicTypeShape(const string& fnName, const vector<TypeInfo>& typeArgs,
                                       const vector<TypeInfo>& argTypes, const vector<p<ExprNode>>& argNodes,
                                       FileNode* file, FileNode* sdkFile, int line, int col) {
    if (fnName == "same_ref" || fnName == "ptr_of") {
        // arity / typeArgs 计数已由 validateBuiltinIntrinsicShape 保证
        const auto& T = typeArgs[0];
        bool isHeapHandle =
            T.isRc() || T.isWeak() || T.isArrayGeneric() || (T.name == "String" && T.kind == TypeKind::Normal);
        // Phase 8a: ptr_of:<Heap<T>>(h) move-out FFI handoff (DRAFT-heap-types §8.3a)
        // same_ref 不接受 Heap (Heap 单所有权, 两个 Heap 不可能指同一块, 比较无意义)
        bool isHeapForPtrOf = (fnName == "ptr_of" && T.isHeap());
        if (!isHeapHandle && !T.isRef() && !isHeapForPtrOf) {
            throw YuxError(line, col, ErrorCode::E6029, fnName, T.getFullName());
        }
        if (T.isRef() || isHeapForPtrOf) {
            // 取源裸指针仅支持: ID-literal (栈/堆变量) 或 ExprGetRefNode (`&x` 字面)
            // _localVarPtrs 查不到的 fallback 仍由 Compiler 抛 E6028.
            // Phase 8a: Heap move-out 也只接受 ID-literal (槽要 null 化 / 摘除 _scopeVars).
            size_t expectedArgs = (fnName == "same_ref" ? 2u : 1u);
            for (size_t i = 0; i < expectedArgs && i < argNodes.size(); ++i) {
                auto node = argNodes[i];
                bool ok = false;
                if (auto lit = dynamic_cast<ExprLiteralNode*>(node)) {
                    if (dynamic_cast<LiteralObjNode*>(lit->literal())) ok = true;
                }
                if (!ok && !isHeapForPtrOf && dynamic_cast<ExprGetRefNode*>(node)) ok = true;
                if (!ok) {
                    throw YuxError(line, col, ErrorCode::E6028, fnName);
                }
            }
        }
        return;
    }
    if (fnName == "as_ref") {
        // 实参必须是 Rc<T> (不接受 Rc<T>?) 或 Heap<T> (DRAFT-heap-types §8.3a.6, Phase 2.7)
        if (argTypes.empty()) return;
        const auto& argType = argTypes[0];
        if (argType.isHeap()) return;
        if (!argType.isRc() || argType.isNullable()) {
            throw YuxError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
        }
        return;
    }
    if (fnName == "weak") {
        // 实参必须是 Rc<T> 或 Rc<T>? (Nullable<Rc<T>>)
        if (argTypes.empty()) return;
        const auto& argType = argTypes[0];
        if (argType.isRc() && !argType.isNullable()) return;
        if (argType.isNullable()) {
            auto inner = argType.nullableInnerType();
            if (inner && inner->isRc()) return;
            throw YuxError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
        }
        throw YuxError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
    }
    if (fnName == "copy_of") {
        // 递归扫 T 是否 (深度) 含 Ref 字段; 命中即报 E6032.
        // 不展开 Rc / Array / Weak / Nullable / Dyn / Ptr / Fn 的类型参数 —— 它们是堆句柄包装.
        std::function<bool(const TypeInfo&, string&)> hasRefDeep;
        hasRefDeep = [&](const TypeInfo& t, string& path) -> bool {
            if (t.isRef()) {
                path = t.getFullName();
                return true;
            }
            if (t.isRc() || t.isArrayGeneric() || t.isWeak() || t.isNullable() || t.isDyn() || t.isPtr() || t.isFn() ||
                t.isHeap()) {
                // Heap<T> 与其他堆句柄一致, 当作不透明把柄不展开 (Phase 3f / 6)
                return false;
            }
            if (isBuiltinType(t.name)) return false;
            auto sd = file ? file->getStructDecl(t.name) : nullptr;
            if (!sd && sdkFile) {
                sd = sdkFile->getStructDecl(t.name);
            }
            if (!sd) return false;
            for (auto& field : sd->fields()) {
                auto ft = field->getType();
                string inner;
                if (hasRefDeep(ft, inner)) {
                    path = t.name + "." + field->name().getText() + " : " + inner;
                    return true;
                }
            }
            return false;
        };
        const auto& T = typeArgs[0];
        string refPath;
        if (hasRefDeep(T, refPath)) {
            throw YuxError(line, col, ErrorCode::E6032, refPath);
        }
        return;
    }
    // 其他 intrinsic (assert_eq / size_of / upgrade) 无类型形态校验, no-op
}

// ==================== Builtin 操作符方法 arity / 类型域 (Phase 3.3.2.e) ====================
// 覆盖 compileBuiltinTypeMethodCall 内 isBuiltinMethod 分支:
//   - 17 处 E6027 arity != 1 (二元 op)
//   - 1 处 E3070 inv on float
void validateOperatorMethodCall(const string& member, const TypeInfo& baseType, size_t argsCount, int line, int col) {
    // 二元 op: args.size() 必须为 1
    static const vector<string> binaryOps = {
        "plus", "minus", "mul", "div", "mod", "eq", "ne", "lt", "le", "gt", "ge", "and", "or", "xor", "shl", "shr",
    };
    for (auto& op : binaryOps) {
        if (member == op) {
            if (argsCount != 1) {
                throw YuxError(line, col, ErrorCode::E6027, member, static_cast<size_t>(1));
            }
            return;
        }
    }
    // 一元 inv: 不接受 float
    if (member == "inv") {
        if (baseType.startsWith('f')) {
            throw YuxError(line, col, ErrorCode::E3070, baseType.name);
        }
        return;
    }
    // neg / not 等其他一元 op 无校验
}

// ========== Phase 3.4.a: 枚举构造表达式形态校验 ==========

namespace {
// 等价于 Compiler::lookupEnumDecl: 本文件 → SDK → wildcard imports.
EnumDeclNode* lookupEnumInFiles(FileNode* file, FileNode* sdkFile, const string& name) {
    if (!file) return nullptr;
    if (auto* d = file->getEnumDecl(name)) return d;
    if (sdkFile && sdkFile != file) {
        if (auto* d = sdkFile->getEnumDecl(name)) return d;
    }
    for (auto* imp : file->wildcardImports()) {
        if (auto* d = imp->getEnumDecl(name)) return d;
    }
    return nullptr;
}

// 用户友好类型渲染: Rc<T> / Array<T> / [N]T / Generic<A,B>
// 与 compiler_expr.cpp compileEnumCtorExpr 内 fmtType lambda 等价.
string fmtTypeFriendly(const TypeInfo& t) {
    if (t.kind == TypeKind::Generic && !t.genericArgs.empty()) {
        string r = t.name + "<";
        for (size_t j = 0; j < t.genericArgs.size(); ++j) {
            if (j) r += ", ";
            r += t.genericArgs[j] ? fmtTypeFriendly(*t.genericArgs[j]) : string("?");
        }
        r += ">";
        return r;
    }
    if (t.kind == TypeKind::Array && t.elementType) {
        return "[" + std::to_string(t.arraySize) + "]" + fmtTypeFriendly(*t.elementType);
    }
    return t.name;
}
} // namespace

void validateEnumCtorShape(FileNode* file, FileNode* sdkFile, p<ExprPathCallNode> node) {
    if (!node) return;
    string enumName = node->getType().name;          // 经别名解析后的真实 enum 名
    string enumNameRaw = node->enumName().getText(); // 用户写法
    string variantName = node->variantName().getText();
    int line = node->getLineNumber();
    int col = node->getColumn();

    auto* enumDecl = lookupEnumInFiles(file, sdkFile, enumName);
    if (!enumDecl) {
        throw YuxError(line, col, ErrorCode::E2019, enumNameRaw, enumNameRaw, variantName);
    }

    auto* variant = enumDecl->variant(variantName);
    if (!variant) {
        throw YuxError(line, col, ErrorCode::E2020, enumName, variantName);
    }

    size_t givenArity = node->args().size();
    size_t declArity = variant->payloadArity();
    if (givenArity != declArity) {
        throw YuxError(line, col, ErrorCode::E2021, enumName, variantName, declArity, givenArity);
    }

    // E2032: 实参类型与 variant payload 类型严格匹配.
    // payload 元素类型从 variant->payloadTypes()[i]->getType() 取;
    // 实参类型 argExpr->getType() 任一抛错 (lambda 形参未推断 等) 时跳过该参数,
    // 留 codegen 原路径继续报.
    for (size_t i = 0; i < declArity; ++i) {
        auto argExpr = node->args()[i];
        TypeInfo expectedType;
        TypeInfo actualType;
        try {
            expectedType = variant->payloadTypes()[i]->getType();
            actualType = argExpr->getType();
        } catch (...) {
            continue;
        }
        if (!(expectedType == actualType)) {
            throw YuxError(argExpr->getLineNumber(), argExpr->getColumn(), ErrorCode::E2032, enumName, variantName, i,
                           fmtTypeFriendly(expectedType), fmtTypeFriendly(actualType));
        }
    }
}

// ========== Phase 3.4.b: match arm 静态校验 ==========

void validateMatchArms(EnumDeclNode* enumDecl, const string& enumName, p<ExprMatchNode> node, FileNode* file) {
    if (!enumDecl || !node) return;
    auto& arms = node->arms();
    int line = node->getLineNumber();
    int col = node->getColumn();

    if (arms.empty()) {
        throw YuxError(line, col, ErrorCode::E2023, enumName, string("(none)"));
    }

    set<string> seenVariants;
    bool hasElse = false;
    for (size_t i = 0; i < arms.size(); ++i) {
        auto arm = arms[i];
        auto pat = arm->pattern();
        if (pat->isElse()) {
            if (i + 1 != arms.size()) {
                throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2025);
            }
            hasElse = true;
            continue;
        }
        string patEnumName = pat->enumName().getText();
        // 与 Compiler 端等价: 直接相等 OK; 否则尝试 file 上一步 alias 解析.
        // 多步 alias 链留 Compiler 兜底 (resolveAlias 递归), 此处仅做一步避免假阳性.
        if (patEnumName != enumName) {
            bool aliasOk = false;
            if (file) {
                if (auto* alias = file->getAliasDecl(patEnumName)) {
                    if (!alias->isGeneric() && alias->target()) {
                        try {
                            if (alias->target()->getType().name == enumName) aliasOk = true;
                        } catch (...) { // NOLINT(bugprone-empty-catch)
                        }
                    }
                }
            }
            if (!aliasOk) {
                throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2019, patEnumName, patEnumName,
                               pat->variantName().getText());
            }
        }

        string vName = pat->variantName().getText();
        auto* variant = enumDecl->variant(vName);
        if (!variant) {
            throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2020, enumName, vName);
        }
        if (seenVariants.count(vName)) {
            throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2024, enumName, vName);
        }
        seenVariants.insert(vName);

        size_t bindArity = pat->binds().size();
        size_t declArity = variant->payloadArity();
        if (bindArity != declArity && !(bindArity == 0 && declArity == 0)) {
            throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2026, enumName, vName, declArity,
                           bindArity);
        }

        set<string> seenBinds;
        for (auto& tk : pat->binds()) {
            const string& bn = tk.getText();
            if (seenBinds.count(bn)) {
                throw YuxError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2027, bn, enumName, vName);
            }
            seenBinds.insert(bn);
        }
    }

    if (!hasElse) {
        vector<string> missing;
        for (auto v : enumDecl->variants()) {
            if (!seenVariants.count(v->name().getText())) {
                missing.push_back(v->name().getText());
            }
        }
        if (!missing.empty()) {
            string s;
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i) s += ", ";
                s += enumName + "::" + missing[i];
            }
            throw YuxError(line, col, ErrorCode::E2023, enumName, s);
        }
    }
}

// ==================== 私有字段可见性 (Phase 3.4.d.2) ====================

namespace {
// 剥 `$<...>` 后缀, 取 generic 实例化前的 base struct 名.
string stripGenericSuffix(const string& name) {
    auto pos = name.find('$');
    return pos == string::npos ? name : name.substr(0, pos);
}

// 查 struct decl: file → sdkFile.
StructDeclNode* lookupStructIn(FileNode* file, FileNode* sdkFile, const string& name) {
    if (!file) return nullptr;
    if (auto* d = file->getStructDecl(name)) return d;
    if (sdkFile && sdkFile != file) {
        if (auto* d = sdkFile->getStructDecl(name)) return d;
    }
    return nullptr;
}
} // namespace

void validatePrivateFieldAccess(StructDeclNode* structDecl, const string& fieldName, const string& baseTypeName,
                                const string& accessorStructName, int line, int col) {
    if (!structDecl) return;
    int idx = structDecl->fieldIndex(fieldName);
    if (idx < 0) return;
    auto field = structDecl->fields()[idx];
    if (!field->isPrivate()) return;
    string currentBase = stripGenericSuffix(accessorStructName);
    if (currentBase == baseTypeName) return;
    throw YuxError(line, col, ErrorCode::E3042, fieldName, baseTypeName);
}

void validateGetRefPrivacy(FileNode* file, FileNode* sdkFile, p<ExprGetRefNode> node,
                           const string& accessorStructName) {
    if (!node) return;
    auto scope = node->findNearestScope();
    if (!scope) return;
    auto sym = scope->lookupSymbol(node->obj().getText());
    if (!sym) return; // E3030 由 getType 抢; 这里静默

    TypeInfo currentType = sym->type;
    if (currentType.isRef()) {
        if (auto inner = currentType.refElementType()) currentType = *inner;
    }

    int line = node->resolveLineNumber();
    int col = node->resolveColumn();

    for (auto& sub : node->subs()) {
        // 与 compileGetRefExpr / ExprGetRefNode::getType 同款: Rc<T> 自动 deref.
        TypeInfo lookupType = currentType;
        if (lookupType.isRc()) {
            if (auto inner = lookupType.rcElementType()) lookupType = *inner;
        }

        auto structDecl = lookupStructIn(file, sdkFile, lookupType.name);
        if (!structDecl) return; // E3041 由 getType 抢

        int idx = structDecl->fieldIndex(sub.getText());
        if (idx < 0) return; // E3040 由 getType 抢

        validatePrivateFieldAccess(structDecl, sub.getText(), lookupType.name, accessorStructName, line, col);

        // 推进 currentType: 取 field type, 含泛型实参替换 (与 getType 同款).
        TypeInfo fieldType = structDecl->fields()[idx]->getType();
        if (lookupType.isGeneric() && structDecl->isGeneric() &&
            lookupType.genericArgs.size() == structDecl->typeParams().size()) {
            std::map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    lookupType.genericArgs[i] ? *lookupType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        currentType = fieldType;
    }
}

void validateDotFieldPrivacy(FileNode* file, FileNode* sdkFile, p<ExprDotNode> node, const string& accessorStructName) {
    if (!node) return;
    if (node->isSafe()) return; // safe `?.` 走 getType 路径, 不在此处校验

    // baseType 取自 baseExpr; getType 异常时静默跳过 (lambda 形参等).
    TypeInfo baseType;
    try {
        baseType = node->baseExpr()->getType();
    } catch (...) {
        return;
    }

    TypeInfo actualType = baseType;
    if (actualType.isRef()) {
        if (auto inner = actualType.refElementType()) actualType = *inner;
    }
    if (actualType.isRc()) {
        if (auto inner = actualType.rcElementType()) actualType = *inner;
    }

    auto structDecl = lookupStructIn(file, sdkFile, actualType.name);
    if (!structDecl) return; // 不是 struct 字段访问 (可能 method / 别的形态), 跳过

    validatePrivateFieldAccess(structDecl, node->member(), actualType.name, accessorStructName,
                               node->resolveLineNumber(), node->resolveColumn());
}

// ==================== 整数字面量解析 (Phase 3.4.f.2) ====================

i64 parseIntLiteral(const string& text, int line, int col) {
    string numStr = text;

    // 识别类型后缀 (决定 signed/unsigned 解析路径)
    static const std::regex suffix_regex(R"([iu](?:8|16|32|64)?$)");
    std::smatch m;
    string suffix;
    if (std::regex_search(numStr, m, suffix_regex)) {
        suffix = m.str();
    }
    bool isUnsigned = !suffix.empty() && suffix[0] == 'u';
    numStr = std::regex_replace(numStr, suffix_regex, "");

    int base = 10;
    string parseStr = numStr;

    // 进制前缀
    if (numStr.size() >= 2) {
        if (numStr[0] == '0' && (numStr[1] == 'b' || numStr[1] == 'B')) {
            base = 2;
            parseStr = numStr.substr(2);
        } else if (numStr[0] == '0' && (numStr[1] == 'o' || numStr[1] == 'O')) {
            base = 8;
            parseStr = numStr.substr(2);
        } else if (numStr[0] == '0' && (numStr[1] == 'x' || numStr[1] == 'X')) {
            base = 16;
            parseStr = numStr.substr(2);
        }
    }

    // 下划线分隔符
    std::erase(parseStr, '_');

    try {
        if (isUnsigned) {
            u64 v = std::stoull(parseStr, nullptr, base);
            return static_cast<i64>(v);
        }
        return std::stoll(parseStr, nullptr, base);
    } catch (const std::out_of_range&) {
        int errLine = line > 0 ? line : 1;
        throw YuxError(errLine, col, ErrorCode::E3103, text, suffix.empty() ? string("i64") : suffix);
    } catch (const std::invalid_argument&) {
        int errLine = line > 0 ? line : 1;
        throw YuxError(errLine, col, ErrorCode::E3103, text, suffix.empty() ? string("i64") : suffix);
    }
}

// ==================== Bucket 3: 类型别名一次性校验 ====================
// 镜像 Compiler::validateAliases (compiler_types.cpp). 0 LLVM, 由 SemaPass 起调.

namespace {

TypeInfo resolveAliasImpl(const TypeInfo& t, FileNode* file, std::set<std::string>& visited) {
    if (!file) return t;
    if (t.kind == TypeKind::Normal) {
        auto* alias = file->getAliasDecl(t.name);
        if (!alias) return t;
        if (alias->isGeneric()) return t;
        if (visited.count(t.name)) {
            throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
        }
        visited.insert(t.name);
        if (!alias->target()) return t;
        TypeInfo target = alias->target()->getType();
        return resolveAliasImpl(target, file, visited);
    }
    if (t.kind == TypeKind::Generic) {
        auto* alias = file->getAliasDecl(t.name);
        if (alias && alias->isGeneric() && alias->typeParams().size() == t.genericArgs.size() && alias->target()) {
            if (visited.count(t.name)) {
                throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
            }
            visited.insert(t.name);
            std::map<std::string, TypeInfo> subst;
            for (size_t i = 0; i < alias->typeParams().size(); ++i) {
                subst[alias->typeParams()[i]] = t.genericArgs[i] ? *t.genericArgs[i] : TypeInfo();
            }
            TypeInfo inst = alias->target()->getType().substitute(subst);
            return resolveAliasImpl(inst, file, visited);
        }
        // 普通泛型不递归到 args (sema 仅做环检测起点, 简化处理)
        return t;
    }
    return t;
}

} // namespace

void validateAliases(p<FileNode> file) {
    if (!file) return;
    auto& aliases = file->getAliasDecls();

    for (auto& a : aliases) {
        string name = a->name().getText();
        if (auto* s = file->getStructDecl(name)) {
            (void)s;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("struct"), name);
        }
        if (auto* d = file->getSpecDecl(name)) {
            (void)d;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("draft"), name);
        }
        size_t cnt = 0;
        for (auto& b : aliases) {
            if (b->name().getText() == name) ++cnt;
        }
        if (cnt > 1) {
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("type alias"), name);
        }
    }

    for (auto& a : aliases) {
        if (!a->target()) continue;
        std::set<std::string> visited;
        visited.insert(a->name().getText());
        (void)resolveAliasImpl(a->target()->getType(), file, visited);
    }
}

// Bucket 6 单点: 比较表达式 leftType 形态校验.
void validateCompareOpForm(const TypeInfo& leftType, ExprCompareNode::Op op, int line, int col) {
    if (leftType.isWeak()) {
        if (op == ExprCompareNode::Op::Eq || op == ExprCompareNode::Op::Ne) {
            throw YuxError(line, col, ErrorCode::E3078)
                .withHint("先 `upgrade(weak)` 取得 Rc<T>?，再用 `?.` / `??` / 相等比较判定目标对象");
        }
    }
    if (leftType.isPtr()) {
        if (op == ExprCompareNode::Op::Eq || op == ExprCompareNode::Op::Ne) return;
        if (op == ExprCompareNode::Op::AndAnd || op == ExprCompareNode::Op::OrOr) return;
        const char* opSym = op == ExprCompareNode::Op::Lt   ? "<"
                            : op == ExprCompareNode::Op::Le ? "<="
                            : op == ExprCompareNode::Op::Gt ? ">"
                                                            : ">=";
        const char* mname = op == ExprCompareNode::Op::Lt   ? "lt"
                            : op == ExprCompareNode::Op::Le ? "le"
                            : op == ExprCompareNode::Op::Gt ? "gt"
                                                            : "ge";
        throw YuxError(line, col, ErrorCode::E3073, "Ptr", opSym, mname)
            .withHint("Ptr 只支持 == / != 比较（与 null 或另一 Ptr）");
    }
}

// Bucket 6 单点: 二元运算符方法解析 (E3073 / E6014).
//
// spec §7.2.3.3: 遍历 leftType 的全部同名方法候选，
// 精确匹配（形参非 Ref，类型一致）优先，其次自动取址（形参 Ref<T>，T 一致）。
// 同优先级多候选歧义 → E6014；无匹配 → E3073。
// 调用方负责事先剥 Ref / applySubst (Compiler) 或保证 leftType 为非泛型 struct (SemaPass).
void validateBinOpMethodResolution(FileNode* file, FileNode* sdkFile, const TypeInfo& leftType,
                                   const TypeInfo& rightType, const string& methodName, int line, int col) {
    string methodFullName = leftType.name + "." + methodName;

    // 收集全部候选
    vector<FnSymbolInfo*> candidates;
    if (file) file->collectFnOverloads(methodFullName, candidates);
    if (sdkFile && sdkFile != file) {
        sdkFile->collectFnOverloads(methodFullName, candidates);
    }
    // 去重：_parentScope 递归 + SDK 直查可能返回同一 FnSymbolInfo*
    // NOLINTBEGIN(modernize-use-ranges)
    sort(candidates.begin(), candidates.end());
    candidates.erase(unique(candidates.begin(), candidates.end()), candidates.end());
    // NOLINTEND(modernize-use-ranges)

    // 按优先级分类
    int exactCount = 0;
    int refCount = 0;
    for (auto* cand : candidates) {
        if (cand->params.size() != 2) continue; // 二元运算符：接收者 + 1 形参
        const TypeInfo& candParam = cand->params[1];

        if (!candParam.isRef() && candParam == rightType) {
            exactCount++;
        } else if (candParam.isRef()) {
            auto refElem = candParam.refElementType();
            if (refElem && *refElem == rightType) {
                refCount++;
            }
        }
    }

    // E6014: 同优先级多候选歧义
    if (exactCount > 1 || refCount > 1) {
        string sigs;
        for (auto* cand : candidates) {
            if (cand->params.size() != 2) continue;
            const TypeInfo& cp = cand->params[1];
            bool matches = (!cp.isRef() && cp == rightType) ||
                           (cp.isRef() && cp.refElementType() && *cp.refElementType() == rightType);
            if (!matches) continue;
            if (!sigs.empty()) sigs += " | ";
            sigs += leftType.name + "." + methodName + "(" + cp.name + ")";
        }
        int matchCount = exactCount + refCount;
        throw YuxError(line, col, ErrorCode::E6014, methodFullName, rightType.name, matchCount, sigs);
    }

    if (exactCount == 1 || refCount == 1) return; // 命中 → codegen 继续

    // 无匹配 → E3073
    const char* opSym = methodName == "plus"    ? "+"
                        : methodName == "minus" ? "-"
                        : methodName == "mul"   ? "*"
                        : methodName == "div"   ? "/"
                        : methodName == "mod"   ? "%"
                        : methodName == "and"   ? "&"
                        : methodName == "or"    ? "|"
                        : methodName == "xor"   ? "^"
                        : methodName == "shl"   ? "<<"
                        : methodName == "shr"   ? ">>"
                        : methodName == "eq"    ? "=="
                        : methodName == "ne"    ? "!="
                        : methodName == "lt"    ? "<"
                        : methodName == "le"    ? "<="
                        : methodName == "gt"    ? ">"
                        : methodName == "ge"    ? ">="
                                                : methodName.c_str();
    throw YuxError(line, col, ErrorCode::E3073, leftType.name, opSym, methodName);
}

// Bucket 6 单点: 字符串模板插值 ToString 校验 (E3026).
void validateStringTemplateInterps(FileNode* file, FileNode* sdkFile, StringTemplateNode* tpl) {
    if (!tpl) return;
    auto canToString = [&](const TypeInfo& t) -> bool {
        if (t.name == "String") return true;
        string fullName = t.name + ".to_string";
        if (sdkFile && sdkFile->lookupFnSymbol(fullName)) return true;
        if (file && file->lookupFnSymbol(fullName)) return true;
        return false;
    };
    for (auto& e : tpl->interps()) {
        TypeInfo t;
        try {
            t = e->getType();
        } catch (...) {
            continue; // lambda 形参等未推断, 留 Compiler 兜底
        }
        if (!canToString(t)) {
            throw YuxError(e->getLineNumber(), e->getColumn(), ErrorCode::E3026, t.name);
        }
    }
}

} // namespace sema

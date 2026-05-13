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
#include "types.h"
#include <format>

namespace sema {

// ==================== 辅助函数: 参数类型匹配 ====================

// 检查参数类型是否可以接受
// 支持精确匹配和引用类型匹配
static bool paramAccepts(const TypeInfo& param, const TypeInfo& argType) {
    if (param == argType) return true;  // 精确匹配
    if (param.isRef()) {
        auto ref = param.refElementType();
        if (ref && *ref == argType) return true;  // 引用参数接受值类型
    }
    if (param.isPtr() && argType.isRef()) return true;  // 指针参数接受引用
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
            } catch (...) {}
            return false;
        }
        try {
            if (!paramAccepts(params[i], args[i]->getType())) return false;
        } catch (...) { return false; }
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
            argType = i32Type;  // 灵活整数默认为 i32
        } else {
            try { argType = args[i]->getType(); } catch (...) { return false; }
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
void resolveCtorOverload(FileNode* file, const string& structName,
                         const vector<p<ExprNode>>& args, int line) {
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
                try { argType = args[i]->getType(); } catch (...) { return false; }
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
                } catch (...) {}
                return false;
            }
            try {
                if (!paramAccepts(c->params[i + 1], args[i]->getType())) return false;
            } catch (...) { return false; }
        }
        return true;
    };

    vector<FnSymbolInfo*> defaultMatches;
    for (auto c : candidates) if (matchesDefault(c)) defaultMatches.push_back(c);

    vector<FnSymbolInfo*> matches;
    if (defaultMatches.size() == 1) {
        matches = defaultMatches;
    } else if (defaultMatches.empty()) {
        for (auto c : candidates) if (matchesFlexible(c)) matches.push_back(c);
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
            try { argSigs += args[i]->getType().name; } catch(...) { argSigs += "?"; }
        }
        throw YuxError(line, ErrorCode::E6014, structName, argSigs, matches.size(), sigs);
    }
}

// ==================== 函数重载解析 ====================
// 解析函数重载，确定应该调用哪个版本
// 如果有歧义，抛出错误要求用户添加类型后缀
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName,
                       const vector<p<ExprNode>>& args, int line) {
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
    if (defaultMatches.size() == 1) {
        matches = defaultMatches;
    } else if (defaultMatches.empty()) {
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
            try { argSigs += args[i]->getType().name; } catch(...) { argSigs += "?"; }
        }
        throw YuxError(line, ErrorCode::E6014, fnName, argSigs, matches.size(), sigs);
    }
}

// ==================== 泛型类型实参 arity 校验 (Phase 3.3 前置.3c) ====================
// 原 compileCallExpr (line 434) 与 compileGenericFunctionCall (line 938) 两处重复的
// E6010 throw + .withHint 抠成 sema 共享; SemaPass 同步接入.
void validateGenericTypeArgsArity(const string& fnName,
                                  size_t expectedCount,
                                  size_t actualCount,
                                  int line, int col) {
    if (expectedCount == actualCount) return;
    throw YuxError(line, col, ErrorCode::E6010, fnName, expectedCount, actualCount)
        .withHint(std::format("调用处的类型实参个数需与声明匹配；改写为 `{}:<{}>(...)` 形式补齐 {} 个类型",
            fnName,
            std::string(expectedCount == 1 ? "T" : "T1, T2, ..."),
            expectedCount));
}

// ==================== ctor 调用形态校验 (Phase 3.3 前置.3b) ====================
// 原为 `compileFunctionCall` 在解析到 structDecl 后的两处内联 throw,
// 纯类型 / 纯 AST 检查, 无 LLVM 依赖, 抠到 sema 后 SemaPass 也能调.
void validateCtorCallShape(StructDeclNode* structDecl, const string& fnName,
                           bool hasTypeArgs, int line, int col) {
    if (!structDecl) return;
    // E6008: 私有结构体不允许跨可见性构造
    if (structDecl->isPrivate()) {
        throw YuxError(line, col, ErrorCode::E6008, fnName);
    }
    // E6009: 泛型结构体调用时未给类型实参
    if (structDecl->isGeneric() && !hasTypeArgs) {
        throw YuxError(line, col, ErrorCode::E6009, fnName);
    }
}

// ==================== Dyn 方法静态形态校验 (Phase 3.3 前置.3f) ====================
// 原位于 `compiler/compiler_call.cpp::compileDynMethodCall` 第 2 / 3 / 4 步:
// 按名查 sig (E6016) → arity (E6012) → 形参类型 (E6015). 纯 AST + TypeInfo,
// 抠到 sema 层后 Compiler 继续走 codegen, SemaPass 后续可在 Dyn 调用点提前调.
FnHeaderNode* resolveDynMethodSig(DraftDeclNode* draftDecl,
                                  const string& draftQualified,
                                  const TypeInfo& baseType,
                                  const string& member,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col) {
    // 2. 找方法签名 (按名匹配; yux 暂无方法名重载, 第一处即终)
    FnHeaderNode* sig = nullptr;
    for (auto& s : draftDecl->signatures()) {
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
        throw YuxError(line, col, ErrorCode::E6012, member,
            (int)sig->params().size(), (int)argTypes.size());
    }

    // 4. 参数类型按 D 签名比对 (类型名 + 全名相等; 与 yux 名义类型一致)
    for (size_t i = 0; i < sig->params().size(); ++i) {
        auto sp = sig->params()[i];
        if (!sp || !sp->type()) continue;
        TypeInfo expected = sp->type()->getType();
        if (expected.getFullName() != argTypes[i].getFullName()) {
            throw YuxError(line, col, ErrorCode::E6015)
                .withHint("Dyn<" + draftQualified + ">." + member + " arg#"
                    + std::to_string(i) + ": 期望 " + expected.getFullName()
                    + ", 实际 " + argTypes[i].getFullName()
                    + " (Dyn 方法调用参数类型按 draft 签名静态匹配)");
        }
    }
    return sig;
}

// ==================== ctor 重载未匹配诊断 (Phase 3.3 前置.3e) ====================
// 原 `compileFunctionCall` 在 compileConstructorCall 返回 null 后的 inline 块:
// 收集 `S.S` 重载 + 用 fmtType 渲染候选签名 / 实参类型 + 抛 E6033 + .withHint.
// 整体抠到 sema 层; 纯 TypeInfo, 无 LLVM 依赖.
void diagnoseCtorOverloadMismatch(FileNode* file, FileNode* sdkFile,
                                  const string& fnName,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col) {
    string ctorFullName = fnName + "." + fnName;
    vector<FnSymbolInfo*> ctorCands;
    if (file) file->collectFnOverloads(ctorFullName, ctorCands);
    if (sdkFile && sdkFile != file) {
        sdkFile->collectFnOverloads(ctorFullName, ctorCands);
    }
    // 把 TypeInfo 渲染成用户友好形式: Box<T>、Array<T>、Fn(P)->R 等
    std::function<string(const TypeInfo&)> fmtType = [&](const TypeInfo& t) -> string {
        if (t.kind == TypeKind::Generic && !t.genericArgs.empty()) {
            string r = t.name + "<";
            for (size_t i = 0; i < t.genericArgs.size(); ++i) {
                if (i) r += ", ";
                r += t.genericArgs[i] ? fmtType(*t.genericArgs[i]) : string("?");
            }
            r += ">";
            return r;
        }
        if (t.kind == TypeKind::Array && t.elementType) {
            return "[" + std::to_string(t.arraySize) + "]" + fmtType(*t.elementType);
        }
        return t.name;
    };
    string ctorSigs;
    for (auto* c : ctorCands) {
        ctorSigs += "\n  " + fnName + "(";
        // params[0] 是接收者本身, 跳过
        for (size_t i = 1; i < c->params.size(); ++i) {
            if (i > 1) ctorSigs += ", ";
            ctorSigs += fmtType(c->params[i]);
        }
        ctorSigs += ")";
    }
    if (ctorCands.empty()) {
        ctorSigs = " (none declared)";
    }
    string argSigs;
    for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i) argSigs += ", ";
        argSigs += fmtType(argTypes[i]);
    }
    throw YuxError(line, col, ErrorCode::E6033, fnName, argSigs, ctorSigs)
        .withHint("若实参与形参类型仅差 Box<T>，先 `var p Box<T> = T(...)` 落地再传；否则按上方候选签名补齐实参");
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
void checkErrPropagateForIdCall(FnNode* currentFnNode,
                                p<ExprCallNode> callNode,
                                const string& fnName,
                                const FnSymbolInfo* calleeSym,
                                vector<string>* tryBlockSeenErrs) {
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
            // E7016: try block 内 ! 冗余 (语义不变, 警告)
            // TODO(10f-4): 接入诊断警告通道; 当前仅注释保留
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
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                ErrorCode::E7004, calleeErr, callerErr, calleeErr, callerErr, calleeErr);
        }
        // callee 不是 #Fallible 但写了 ! : 暂不在 10e/10f 报; 保留给后续考虑
    } else {
        // E7006: 调用 #Fallible 函数但未加 ! (不在 try block 内)
        if (!calleeErr.empty()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                ErrorCode::E7006, fnName);
        }
    }
}

} // namespace sema

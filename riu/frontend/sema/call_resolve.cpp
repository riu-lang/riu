// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 调用重载解析实现 (Sema/Codegen 拆分 Phase 3.3 前置)
//
// 本文件内容原位于 `riu/riu/compiler/compiler_call.cpp` 顶部, 现整体迁移到 sema 层:
// - paramAccepts / overloadMatchesDefault / overloadMatchesFlexible: 纯 TypeInfo 匹配
// - resolveFnOverload / resolveCtorOverload: 重载解析 + 灵活整数推断 + E6014 歧义诊断
//
// 不依赖任何 LLVM 头; 由 riu_frontend 静态库提供, Compiler 与未来的 SemaPass 共享.

#include "sema/call_resolve.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/struct_node.h"
#include "ast/riu.h"
#include "tools/diagnostic.h"
#include "types.h"
#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace sema {

// ==================== 辅助函数: 参数类型匹配 ====================

// 检查参数类型是否可以接受
// 支持精确匹配和引用类型匹配
static bool paramAccepts(const TypeInfo& param, const TypeInfo& argType) {
    if (param == argType) return true; // 精确匹配
    // 不再隐式取 ref：T 与 T& 是不同的类型，各有各的重载
    // 需要引用时在调用处用显式 &arg（而非 &expr）
    if (param.isPtr() && argType.isRef()) return true; // 指针参数接受引用
    // null 字面量（类型 Ptr）可以匹配任何 Nullable<T> 形参
    if (param.isNullable() && argType.isPtr()) return true;
    // T 值可以匹配 Nullable<T> 形参（自动包装 T → {_has=true, _value=T}）
    if (param.isNullable()) {
        auto inner = param.nullableInnerType();
        if (inner && *inner == argType) return true;
    }
    return false;
}

// 灵活的函数重载匹配
// 对灵活整数字面量 (如 42) 允许匹配任何整数类型
static bool overloadMatchesFlexible(const vector<ExprNode*>& args, const vector<const TypeInfo*>& params) {
    if (params.size() != args.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        const TypeInfo& pi = *params[i];
        if (isFlexibleIntExpr(args[i])) {
            // 灵活整数可以匹配任何整数类型
            if (isIntTypeName(pi.name)) continue;
            // 形参为 Nullable<T> 且 T 为整数类型 → 允许灵活整数匹配
            if (pi.isNullable()) {
                auto inner = pi.nullableInnerType();
                if (inner && isIntTypeName(inner->name)) continue;
            }
            try {
                if (paramAccepts(pi, args[i]->getType())) continue;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            return false;
        }
        // 灵活 null 可以匹配任何 Nullable<T> 形参
        if (isFlexibleNullExpr(args[i])) {
            if (pi.isNullable()) continue;
            try {
                if (paramAccepts(pi, args[i]->getType())) continue;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            return false;
        }
        try {
            if (!paramAccepts(pi, args[i]->getType())) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

// 默认的函数重载匹配
// 灵活整数字面量默认匹配 i32
static bool overloadMatchesDefault(const vector<ExprNode*>& args, const vector<const TypeInfo*>& params) {
    if (params.size() != args.size()) return false;
    TypeInfo i32Type("i32");
    TypeInfo ptrType("Ptr");
    for (size_t i = 0; i < args.size(); ++i) {
        TypeInfo argType;
        if (isFlexibleIntExpr(args[i])) {
            argType = i32Type; // 灵活整数默认为 i32
        } else if (isFlexibleNullExpr(args[i])) {
            argType = ptrType; // 灵活 null 默认保持 Ptr，通过 paramAccepts 匹配 Nullable<T>
        } else {
            try {
                argType = args[i]->getType();
            } catch (...) {
                return false;
            }
        }
        if (!paramAccepts(*params[i], argType)) return false;
    }
    return true;
}

// ==================== 构造函数重载解析 ====================
// 与 resolveFnOverload 同思路，但 ctor 在符号表中以 `S.S` 注册，且 params[0] 是
// 接收者（结构体类型本身）。匹配时跳过 params[0]，按用户写的实参列表推断未带后缀
// 的整数字面量类型，避免后续在 LLVM 后端因 i32→i64 形参不匹配而走到外部函数路径
// 触发 `isSized` 断言（见 BUGS.md「构造函数 i64 形参传 untyped int 字面量」）。
void resolveCtorOverload(FileNode* file, const string& structName, const vector<ExprNode*>& args, int line) {
    string ctorFullName = structName + "." + structName;
    vector<FnSymbolInfo*> candidates;
    file->collectFnOverloads(ctorFullName, candidates);
    if (candidates.empty()) return;

    auto matchesDefault = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        TypeInfo i32Type("i32");
        TypeInfo ptrType("Ptr");
        for (size_t i = 0; i < args.size(); ++i) {
            TypeInfo argType;
            if (isFlexibleIntExpr(args[i])) {
                argType = i32Type;
            } else if (isFlexibleNullExpr(args[i])) {
                argType = ptrType;
            } else {
                try {
                    argType = args[i]->getType();
                } catch (...) {
                    return false;
                }
            }
            if (!paramAccepts(c->paramType(i + 1), argType)) return false;
        }
        return true;
    };
    auto matchesFlexible = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i])) {
                if (isIntTypeName(c->paramType(i + 1).name)) continue;
                // Nullable<整数> 形参：剥 Nullable 后检查内层是否为整数类型
                if (c->paramType(i + 1).isNullable()) {
                    auto inner = c->paramType(i + 1).nullableInnerType();
                    if (inner && isIntTypeName(inner->name)) continue;
                }
                try {
                    if (paramAccepts(c->paramType(i + 1), args[i]->getType())) continue;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                return false;
            }
            if (isFlexibleNullExpr(args[i])) {
                if (c->paramType(i + 1).isNullable()) continue;
                try {
                    if (paramAccepts(c->paramType(i + 1), args[i]->getType())) continue;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                return false;
            }
            try {
                if (!paramAccepts(c->paramType(i + 1), args[i]->getType())) return false;
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
        // 唯一匹配：把每个灵活整数 / 灵活 null 实参推断到对应 ctor 形参类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->paramType(i + 1).name)) {
                tryInferIntType(args[i], fn->paramType(i + 1));
            } else if (isFlexibleIntExpr(args[i]) && fn->paramType(i + 1).isNullable()) {
                // Nullable<整数> 形参：按内层 T 推断灵活整数
                auto inner = fn->paramType(i + 1).nullableInnerType();
                if (inner && isIntTypeName(inner->name)) {
                    tryInferIntType(args[i], *inner);
                }
            }
            if (isFlexibleNullExpr(args[i]) && fn->paramType(i + 1).isNullable()) {
                tryInferNullType(args[i], fn->paramType(i + 1));
            }
        }
    } else if (matches.size() > 1) {
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + structName + "(";
            for (size_t i = 1; i < m->params.size(); ++i) {
                if (i > 1) sigs += ", ";
                sigs += m->paramType(i).name;
            }
            sigs += ')';
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try {
                argSigs += args[i]->getType().name;
            } catch (...) {
                argSigs += '?';
            }
        }
        throw RiuError(line, ErrorCode::E6014, structName, argSigs, matches.size(), sigs);
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
                throw RiuError(line, ErrorCode::E6027, structName, expected)
                    .withHint(std::format("期望 {} 个实参，实际 {} 个", expected, args.size()));
            }
            string sigs;
            for (auto c : candidates) {
                sigs += "\n  " + structName + "(";
                for (size_t i = 1; i < c->params.size(); ++i) {
                    if (i > 1) sigs += ", ";
                    sigs += c->paramType(i).name;
                }
                sigs += ')';
            }
            throw RiuError(line, ErrorCode::E6027, structName, *arities.begin())
                .withHint(std::format("实参 {} 个，候选重载有 {} 种参数个数；声明的重载:{}", args.size(),
                                      arities.size(), sigs));
        }
    }
}

// ==================== 方法重载解析 ====================
// 解析结构体方法调用的重载，推断灵活整数 / 灵活 null 类型
// 与 resolveCtorOverload 同思路，但方法在符号表中以 `TypeName.methodName` 注册，
// params[0] 是接收者；匹配时跳过 params[0]，按用户写的实参列表推断未带后缀的整数字面量类型。
void resolveMethodOverload(FileNode* file, FileNode* sdkFile, const string& baseTypeName, const string& member,
                           const vector<ExprNode*>& args, int line) {
    string methodFullName = baseTypeName + "." + member;
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

    if (candidates.empty()) return;

    auto matchesDefault = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        TypeInfo i32Type("i32");
        TypeInfo ptrType("Ptr");
        for (size_t i = 0; i < args.size(); ++i) {
            TypeInfo argType;
            if (isFlexibleIntExpr(args[i])) {
                argType = i32Type;
            } else if (isFlexibleNullExpr(args[i])) {
                argType = ptrType;
            } else {
                try {
                    argType = args[i]->getType();
                } catch (...) {
                    return false;
                }
            }
            if (!paramAccepts(c->paramType(i + 1), argType)) return false;
        }
        return true;
    };
    auto matchesFlexible = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i])) {
                if (isIntTypeName(c->paramType(i + 1).name)) continue;
                // Nullable<整数> 形参：剥 Nullable 后检查内层是否为整数类型
                if (c->paramType(i + 1).isNullable()) {
                    auto inner = c->paramType(i + 1).nullableInnerType();
                    if (inner && isIntTypeName(inner->name)) continue;
                }
                try {
                    if (paramAccepts(c->paramType(i + 1), args[i]->getType())) continue;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                return false;
            }
            if (isFlexibleNullExpr(args[i])) {
                if (c->paramType(i + 1).isNullable()) continue;
                try {
                    if (paramAccepts(c->paramType(i + 1), args[i]->getType())) continue;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                return false;
            }
            try {
                if (!paramAccepts(c->paramType(i + 1), args[i]->getType())) return false;
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
        // 唯一匹配：把每个灵活整数 / 灵活 null 实参推断到对应形参类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->paramType(i + 1).name)) {
                tryInferIntType(args[i], fn->paramType(i + 1));
            } else if (isFlexibleIntExpr(args[i]) && fn->paramType(i + 1).isNullable()) {
                // Nullable<整数> 形参：按内层 T 推断灵活整数
                auto inner = fn->paramType(i + 1).nullableInnerType();
                if (inner && isIntTypeName(inner->name)) {
                    tryInferIntType(args[i], *inner);
                }
            }
            if (isFlexibleNullExpr(args[i]) && fn->paramType(i + 1).isNullable()) {
                tryInferNullType(args[i], fn->paramType(i + 1));
            }
        }
    } else if (matches.size() > 1) {
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + methodFullName + "(";
            for (size_t i = 1; i < m->params.size(); ++i) {
                if (i > 1) sigs += ", ";
                sigs += m->paramType(i).name;
            }
            sigs += ')';
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try {
                argSigs += args[i]->getType().name;
            } catch (...) {
                argSigs += '?';
            }
        }
        throw RiuError(line, ErrorCode::E6014, methodFullName, argSigs, matches.size(), sigs);
    }

    // candidates 非空但无任何匹配：仅当实参个数与所有候选形参个数都不一致
    // 时才报 E6027；若个数匹配但类型不匹配，留给 codegen 按原路径报类型错误。
    if (!candidates.empty()) {
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
                throw RiuError(line, ErrorCode::E6027, methodFullName, expected)
                    .withHint(std::format("期望 {} 个实参，实际 {} 个", expected, args.size()));
            }
        }
    }
}

// ==================== 函数重载解析 ====================
// 解析函数重载，确定应该调用哪个版本
// 如果有歧义，抛出错误要求用户添加类型后缀
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName, const vector<ExprNode*>& args,
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
        // 唯一匹配: 推断灵活整数 / 灵活 null 的类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->paramType(i).name)) {
                tryInferIntType(args[i], fn->paramType(i));
            } else if (isFlexibleIntExpr(args[i]) && fn->paramType(i).isNullable()) {
                // Nullable<T> 形参：按内层 T 推断灵活整数
                auto inner = fn->paramType(i).nullableInnerType();
                if (inner && isIntTypeName(inner->name)) {
                    tryInferIntType(args[i], *inner);
                }
            }
            if (isFlexibleNullExpr(args[i]) && fn->paramType(i).isNullable()) {
                tryInferNullType(args[i], fn->paramType(i));
            }
        }
    } else if (matches.size() > 1) {
        // 多个匹配: 报告歧义错误
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + fnName + "(";
            for (size_t i = 0; i < m->params.size(); ++i) {
                if (i) sigs += ", ";
                sigs += m->paramType(i).name;
            }
            sigs += ')';
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try {
                argSigs += args[i]->getType().name;
            } catch (...) {
                argSigs += '?';
            }
        }
        throw RiuError(line, ErrorCode::E6014, fnName, argSigs, matches.size(), sigs);
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
                throw RiuError(line, ErrorCode::E6027, fnName, expected)
                    .withHint(std::format("期望 {} 个实参，实际 {} 个", expected, args.size()));
            }
            string sigs;
            for (auto c : candidates) {
                sigs += "\n  " + fnName + "(";
                for (size_t i = 0; i < c->params.size(); ++i) {
                    if (i) sigs += ", ";
                    sigs += c->paramType(i).name;
                }
                sigs += ')';
            }
            throw RiuError(line, ErrorCode::E6027, fnName, *arities.begin())
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
    throw RiuError(line, col, ErrorCode::E6010, fnName, expectedCount, actualCount)
        .withHint(std::format("调用处的类型实参个数需与声明匹配；改写为 `{}:<{}>(...)` 形式补齐 {} 个类型", fnName,
                              std::string(expectedCount == 1 ? "T" : "T1, T2, ..."), expectedCount));
}

static void throwGenericNamedArity(const string& name, size_t want, size_t got, int line, int col) {
    throw RiuError(line, col, ErrorCode::E6011, name, want, got)
        .withHint(std::format("实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", name,
                              std::string(want == 1 ? "T" : "T1, T2, ..."), want));
}

void validateGenericNamedTypeArity(const TypeInfo& raw, const NameResolver& nr, int line, int col,
                                   const string& currentStructName) {
    auto rec = [&](auto&& self, const TypeInfo& t) -> void {
        TypeInfo t0 = t;
        try {
            t0 = resolveAlias(t, nr.file, nr.sdkFile);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return;
        }
        if (t0.empty()) return;

        // Ref / Fn / 元组的 genericArgs 不是用户类型实参；先查具名 struct·enum 再下钻。
        // 泛型 impl 里 `Self` / 裸名即当前单态，不要求写出实参。
        const bool currentInst =
            t0.isSelf() || (!currentStructName.empty() && t0.name == currentStructName && t0.genericArgs.empty());
        if (!currentInst && !t0.isRef() && !t0.isFn() && !t0.isTuple() && !t0.name.empty()) {
            // 基本类型无 typeParams；lookupStruct 以前会线性扫全文件 struct。
            if (!isBuiltinType(t0.name)) {
                if (auto* sd = nr.lookupStruct(t0, true)) {
                    size_t want = sd->typeParams().size();
                    size_t got = t0.genericArgs.size();
                    if (want > 0 && want != got) throwGenericNamedArity(t0.name, want, got, line, col);
                } else if (auto* ed = nr.lookupEnum(t0)) {
                    size_t want = ed->typeParams().size();
                    size_t got = t0.genericArgs.size();
                    if (want > 0 && want != got) throwGenericNamedArity(t0.name, want, got, line, col);
                }
            }
        }

        if (t0.isFn()) {
            for (auto& p : t0.fnParamTypes()) {
                if (p) self(self, *p);
            }
            if (auto r = t0.fnReturnType()) self(self, *r);
            return;
        }
        if (t0.isArray() && t0.elementType) self(self, *t0.elementType);
        for (auto& a : t0.genericArgs) {
            if (a) self(self, *a);
        }
    };
    rec(rec, raw);
}

// ==================== Dyn 方法静态形态校验 (Phase 3.3 前置.3f) ====================
// 原位于 `compiler/compiler_call.cpp::compileDynMethodCall` 第 2 / 3 / 4 步:
// 按名查 sig (E6016) → arity (E6012) → 形参类型 (E6015). 纯 AST + TypeInfo,
// 抠到 sema 层后 Compiler 继续走 codegen, SemaPass 后续可在 Dyn 调用点提前调.
FnHeaderNode* resolveDynMethodSig(SpecDeclNode* specDecl, const string& specQualified, const TypeInfo& baseType,
                                  const string& member, const vector<TypeInfo>& argTypes, int line, int col) {
    // 2. 找方法签名 (按名匹配; riu 暂无方法名重载, 第一处即终)
    FnHeaderNode* sig = nullptr;
    for (auto& s : specDecl->signatures()) {
        if (!s) continue;
        if (s->name().getText() != member) continue;
        sig = s;
        break;
    }
    if (!sig) {
        throw RiuError(line, col, ErrorCode::E6016, member, baseType.getFullName());
    }

    // 3. arity 校验
    if (sig->params().size() != argTypes.size()) {
        throw RiuError(line, col, ErrorCode::E6012, member, static_cast<int>(sig->params().size()),
                       static_cast<int>(argTypes.size()));
    }

    // 4. 参数类型按 D 签名比对 (TypeInfo::operator==; 与 riu 名义类型一致)
    for (size_t i = 0; i < sig->params().size(); ++i) {
        auto sp = sig->params()[i];
        if (!sp || !sp->type()) continue;
        TypeInfo expected = sp->type()->getType();
        if (expected != argTypes[i]) {
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
            throw RiuError(line, col, ErrorCode::E6015).withHint(hint);
        }
    }
    return sig;
}

namespace {

string callerFallibleErr(FnNode* fn, LambdaExprNode* lam) {
    if (fn && fn->header()) {
        string e = fn->header()->resolvedFallibleErr();
        if (!e.empty()) return e;
    }
    if (lam && lam->fallibleErrTypeNode()) {
        return fallibleErrKey(lam->fallibleErrTypeNode()->getType());
    }
    if (lam) {
        const TypeInfo& ft = lam->getType();
        if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->fallibleErr.empty()) {
            return ft.fnReturnType()->fallibleErr;
        }
    }
    return {};
}

string fnTypeFallibleErr(const TypeInfo& fnTy) {
    if (!fnTy.isFn()) return {};
    if (auto rt = fnTy.fnReturnType()) return rt->fallibleErr;
    return {};
}

} // namespace

// ==================== 非-ID callee `!` fallback 校验 (Phase 3.3 前置.3d) ====================
// 原 compileCallExpr 入口两处 else 分支的内联 E7001 throw 抠成共享 helper.
// 调用方 (Compiler) 已确认: callee 非 ID-literal + `errPropagate()` + 不在 try block.
void checkBangWithoutFallibleCaller(FnNode* currentFnNode, ExprCallNode* callNode, LambdaExprNode* currentLambda) {
    if (!callerFallibleErr(currentFnNode, currentLambda).empty()) return;
    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
}

// ==================== ID-callee 错误传播校验 (Phase 10e/10f) ====================
// 原位于 `riu/riu/compiler/compiler_call.cpp` 的 static 自由函数, 形参从
// `Compiler::TryCatchCtx*` 改成 `vector<string>* tryBlockSeenErrs` 以解开 LLVM 耦合
// (原 TryCatchCtx 内含 llvm::BasicBlock*, 函数体只读 seenErrTypes).
void checkErrPropagateForIdCall(FnNode* currentFnNode, ExprCallNode* callNode, const string& fnName,
                                const FnSymbolInfo* calleeSym, vector<string>* tryBlockSeenErrs,
                                const string& sourcePath, LambdaExprNode* currentLambda) {
    bool hasBang = callNode->errPropagate();
    string callerErr = callerFallibleErr(currentFnNode, currentLambda);
    string calleeErr = calleeSym ? calleeSym->fallibleErrType : "";

    // Phase 10f: try block 内的 #Fallible 调用 → 路由到 catch 子句
    if (tryBlockSeenErrs && !calleeErr.empty()) {
        tryBlockSeenErrs->push_back(calleeErr);
        if (hasBang) {
            // E7016: try block 内 ! 冗余 (语义不变, 警告; 默认 Warning,
            // -Werror / --deny=E7016 升级为 Error 时 emit 内部会 rethrow).
            // emit 按 (file, code, line, col, msg) 去重.
            DiagnosticEngine::emit(sourcePath, RiuError(callNode->getLineNumber(), callNode->getColumn(),
                                                        ErrorCode::E7016, fnName, calleeErr));
        }
        return;
    }

    if (hasBang) {
        // E7001: caller 不在 #Fallible(E) 函数内 + 不在 try block 内
        if (callerErr.empty()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
        }
        // E7004: caller / callee 错误类型不一致 (同 ! 不可跨类型透传)
        if (!calleeErr.empty() && calleeErr != callerErr) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7004, calleeErr, callerErr,
                           calleeErr, callerErr, calleeErr);
        }
        // callee 不是 #Fallible 但写了 ! : 暂不在 10e/10f 报; 保留给后续考虑
    } else {
        // E7006: 调用 #Fallible 函数但未加 ! (不在 try block 内)
        if (!calleeErr.empty()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7006, fnName);
        }
    }
}

void checkErrPropagateForPathCall(FnNode* currentFnNode, ExprPathCallNode* callNode, const string& fnName,
                                  const string& calleeErr, vector<string>* tryBlockSeenErrs, const string& sourcePath,
                                  LambdaExprNode* currentLambda) {
    bool hasBang = callNode->errPropagate();
    string callerErr = callerFallibleErr(currentFnNode, currentLambda);

    if (tryBlockSeenErrs && !calleeErr.empty()) {
        tryBlockSeenErrs->push_back(calleeErr);
        if (hasBang) {
            DiagnosticEngine::emit(sourcePath, RiuError(callNode->getLineNumber(), callNode->getColumn(),
                                                        ErrorCode::E7016, fnName, calleeErr));
        }
        return;
    }

    if (hasBang) {
        if (callerErr.empty()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
        }
        if (!calleeErr.empty() && calleeErr != callerErr) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7004, calleeErr, callerErr,
                           calleeErr, callerErr, calleeErr);
        }
    } else if (!calleeErr.empty()) {
        throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7006, fnName);
    }
}

// ==================== fn-value callee 错误传播校验 (Phase F7) ====================
// 镜像 checkErrPropagateForIdCall：callee 的 fallible 元数据来自静态 Fn TypeInfo（Function<..., T ! E> /
// fallible lambda），而非 FnSymbolInfo。
void checkErrPropagateForFnValueCall(FnNode* currentFnNode, ExprCallNode* callNode, const TypeInfo& calleeFnType,
                                     vector<string>* tryBlockSeenErrs, const string& sourcePath,
                                     LambdaExprNode* currentLambda) {
    if (!calleeFnType.isFn()) return;
    string calleeErr = fnTypeFallibleErr(calleeFnType);
    if (calleeErr.empty() && !callNode->errPropagate()) return;

    bool hasBang = callNode->errPropagate();
    string callerErr = callerFallibleErr(currentFnNode, currentLambda);

    if (tryBlockSeenErrs && !calleeErr.empty()) {
        tryBlockSeenErrs->push_back(calleeErr);
        if (hasBang) {
            DiagnosticEngine::emit(sourcePath, RiuError(callNode->getLineNumber(), callNode->getColumn(),
                                                        ErrorCode::E7016, "<fn-value>", calleeErr));
        }
        return;
    }

    if (hasBang) {
        if (callerErr.empty()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7001);
        }
        if (!calleeErr.empty() && calleeErr != callerErr) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7004, calleeErr, callerErr,
                           calleeErr, callerErr, calleeErr);
        }
    } else {
        if (!calleeErr.empty()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E7006, "<fn-value>");
        }
    }
}

// ==================== 包/模块别名调用解析 (Phase 3.3.1.a) ====================
// 原 `compileMethodCall` line 195-254 的两个 inline 块 (包别名 + 模块别名)
// 抠到 sema 层. 命中其中一种时返回 {matched=true, fnName, fnSym}, 调用方
// 走 compileKnownFunctionCall; 未命中返回 {matched=false} 由调用方继续.
// 纯 AST 符号查 + 字符串拼接, 无 LLVM 依赖.
ModuleFnCallResult resolveModuleFnCall(FileNode* file, Riu* riu, ExprCallNode* callNode, ExprDotNode* dotNode,
                                       const vector<TypeInfo>& argTypes) {
    ModuleFnCallResult result;
    auto member = dotNode->member();
    // 泛型实例体仍挂在定义文件 AST 上；`file` 在 emitFnInstances 里是调用点 TU。
    FileNode* lookupFile = dotNode->enclosingFile();
    if (!lookupFile) lookupFile = file;
    if (!lookupFile) return result;

    // 包别名调用: package.module.fn(args)
    {
        string aliasName;
        vector<string> segs;
        if (ExprDotNode::parseChain(dotNode, aliasName, segs) && segs.size() >= 2) {
            auto aliasSym = lookupFile->lookupSymbol(aliasName);
            if (aliasSym && (aliasSym->kind == SymbolKind::Package || aliasSym->kind == SymbolKind::Module) &&
                lookupFile->isAmbiguousAlias(aliasName)) {
                lookupFile->throwAmbiguousAlias(aliasName, callNode->getLineNumber());
            }
            if (aliasSym && aliasSym->kind == SymbolKind::Package) {
                string childKey;
                for (size_t i = 0; i + 1 < segs.size(); ++i) {
                    if (i) childKey += '.';
                    childKey += segs[i];
                }
                if (riu)
                    (void)riu->resolvePkgPath(lookupFile, childKey, callNode->getLineNumber(), aliasSym->moduleName);
                auto* target = lookupFile->packageChild(aliasName, childKey);
                if (!target) {
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6001, childKey,
                                   aliasSym->moduleName);
                }
                const string& fnName = segs.back();
                auto* fnSym = target->lookupFnSymbolWithParams(fnName, argTypes);
                if (!fnSym) {
                    // 泛型回退：检查目标模块是否有同名泛型函数
                    auto [gFn, gOwner] = target->getGenericFunction(fnName);
                    if (gFn) {
                        result.matched = true;
                        result.fnName = fnName;
                        result.genericFn = gFn;
                        result.genericOwner = gOwner ? gOwner : target;
                        return result;
                    }
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6002, fnName,
                                   aliasSym->moduleName + "." + childKey);
                }
                if (fnSym->isPrivate) {
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6003, fnName);
                }
                result.matched = true;
                result.fnName = fnName;
                result.fnSym = fnSym;
                return result;
            }
        }
    }

    // 模块别名调用: module.fn(args)
    if (auto baseLit = dynamic_cast<ExprLiteralNode*>(dotNode->baseExpr()); riu && baseLit) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
            auto aliasName = objLit->getValue().getText();
            auto aliasSym = lookupFile->lookupSymbol(aliasName);
            if (aliasSym && aliasSym->kind == SymbolKind::Module) {
                auto targetMod = riu->module(aliasSym->moduleName);
                if (!targetMod) {
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6005,
                                   aliasSym->moduleName, aliasName);
                }
                auto* fnSym = targetMod->lookupFnSymbolWithParams(member, argTypes);
                if (!fnSym) {
                    // 泛型回退：检查目标模块是否有同名泛型函数
                    auto [gFn, gOwner] = targetMod->getGenericFunction(member);
                    if (gFn) {
                        result.matched = true;
                        result.fnName = member;
                        result.genericFn = gFn;
                        result.genericOwner = gOwner ? gOwner : targetMod;
                        return result;
                    }
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6002, member,
                                   aliasSym->moduleName);
                }
                if (fnSym->isPrivate) {
                    throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6004, member);
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
void inferGenericFnTypeArgs(ExprCallNode* callNode, FnNode* genericFn, const string& fnName,
                            const vector<TypeInfo>& argTypes, vector<TypeInfo>& outTypeArgs) {
    const auto& typeParams = genericFn->header()->typeParams();
    auto params = genericFn->header()->params();
    if (params.size() != argTypes.size()) {
        throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6012, fnName, params.size(),
                       argTypes.size());
    }

    map<string, TypeInfo> inferred;
    set<string> lockedFromConcrete;
    // 递归 unify: 形参 pType 与实参 aType 匹配; 遇到形如 T 的裸类型形参则记录推断
    std::function<void(const TypeInfo&, const TypeInfo&)> unify = [&](const TypeInfo& pType, const TypeInfo& aType) {
        // 实参为引用但形参非引用：剥引用（如 arr[0] 返回 T& 传给形参 T）
        if (aType.isRef() && !pType.isRef()) {
            auto aElem = aType.refElementType();
            if (aElem) {
                unify(pType, *aElem);
                return;
            }
        }
        if (pType.isNormal() && !isBuiltinType(pType.name)) {
            for (auto& tp : typeParams) {
                if (pType.name == tp) {
                    inferred[tp] = aType;
                    return;
                }
            }
        }
        // 泛型实例化 vs 泛型实例化（含内置包装）：同名同元数则递归各 typeArg
        if (pType.hasGenericArgs() && aType.hasGenericArgs() && pType.kind == aType.kind && pType.name == aType.name &&
            pType.genericArgs.size() == aType.genericArgs.size()) {
            for (size_t i = 0; i < pType.genericArgs.size(); ++i) {
                if (pType.genericArgs[i] && aType.genericArgs[i]) {
                    unify(*pType.genericArgs[i], *aType.genericArgs[i]);
                }
            }
        }
        // 形参为 Ref<X>、实参为非引用 (值类型): 编译器自动取址传参, unify 时剥 Ref
        // 例: copy_of:<T>(x T&), 调用 copy_of(v) 其中 v: i32
        //     形参 Ref(T) vs 实参 i32 → 剥 Ref 后 unify(T, i32) → T=i32
        if (pType.isRef()) {
            auto elem = pType.refElementType();
            if (elem) {
                if (aType.isRef()) {
                    auto aElem = aType.refElementType();
                    if (aElem) unify(*elem, *aElem);
                } else {
                    unify(*elem, aType);
                }
            }
        }
        // 形参为 Nullable<X>、实参非 Nullable: weak 等接受 T 与 T? 两种输入,
        // unify 时剥 Nullable 继续匹配内层
        // 但如果实参是 Ptr（null 字面量），不参与 T 推断（null 不携带内层类型信息）
        if (pType.isNullable()) {
            auto inner = pType.nullableInnerType();
            if (inner) {
                if (aType.isNullable()) {
                    auto aInner = aType.nullableInnerType();
                    if (aInner) unify(*inner, *aInner);
                } else if (aType.isPtr()) {
                    // null 字面量：不贡献类型推断，允许通过
                } else {
                    unify(*inner, aType);
                }
            }
        }
    };
    for (size_t i = 0; i < params.size(); ++i) {
        auto paramType = params[i]->type();
        if (!paramType) continue;

        TypeInfo pType = paramType->getType();

        // 若实参是灵活整数，且对应形参的类型参数已被推断为整型，跳过 unify
        // （不覆盖已推断的泛型类型参数，避免 `assert_eq(a_i8, 42)` 中 T 被 42 的默认 i32 覆盖）
        bool skipUnify = false;
        if (isFlexibleIntExpr(callNode->getArgs()[i]) && pType.isNormal() && !isBuiltinType(pType.name)) {
            for (auto& tp : typeParams) {
                if (pType.name == tp) {
                    auto it = inferred.find(tp);
                    if (it != inferred.end()) {
                        // 检查推断结果是否为整型（含引用剥壳：arr[0] 返 T&，T 记为 i64&）
                        string inferredName = it->second.name;
                        if (it->second.isRef()) {
                            if (auto inner = it->second.refElementType()) inferredName = inner->name;
                        }
                        if (isIntTypeName(inferredName)) {
                            skipUnify = true;
                            break;
                        }
                    }
                }
            }
            // Nullable<T> 形参（pType.name="Nullable"）：若内层类型参数已被推断为整型，同样跳过
            if (!skipUnify && pType.isNullable()) {
                auto nullableInner = pType.nullableInnerType();
                if (nullableInner) {
                    for (auto& tp : typeParams) {
                        if (nullableInner->name == tp) {
                            auto it = inferred.find(tp);
                            if (it != inferred.end()) {
                                string inferredName = it->second.name;
                                if (it->second.isRef()) {
                                    if (auto ri = it->second.refElementType()) inferredName = ri->name;
                                }
                                if (isIntTypeName(inferredName)) {
                                    skipUnify = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        // 若实参是灵活 null，跳过 unify（null 不携带内层类型信息，不参与 T 推断）
        if (isFlexibleNullExpr(callNode->getArgs()[i])) {
            skipUnify = true;
        }

        if (!skipUnify) {
            map<string, TypeInfo> before = inferred;
            unify(pType, argTypes[i]);
            if (!isFlexibleIntExpr(callNode->getArgs()[i])) {
                for (auto& [k, v] : inferred) {
                    auto prev = before.find(k);
                    if (prev == before.end() || !(prev->second == v)) lockedFromConcrete.insert(k);
                }
            }
        }
    }

    auto litFits = [](LiteralIntNode* lit, const string& ty) -> bool {
        try {
            (void)parseIntLiteral(lit->getValue().getText(), 0, 0, ty, lit->isUnaryNegated());
            return true;
        } catch (const RiuError&) {
            return false;
        }
    };
    auto flexibleArgsFit = [&](const string& tpName, const string& ty) -> bool {
        for (size_t i = 0; i < params.size(); ++i) {
            if (!isFlexibleIntExpr(callNode->getArgs()[i])) continue;
            auto pt = params[i]->type();
            if (!pt) continue;
            TypeInfo pType = pt->getType();
            bool involves = (pType.isNormal() && pType.name == tpName);
            if (pType.isNullable()) {
                if (auto inner = pType.nullableInnerType(); inner && inner->name == tpName) involves = true;
            }
            if (pType.isRef()) {
                if (auto inner = pType.refElementType(); inner && inner->name == tpName) involves = true;
            }
            if (!involves) continue;
            for (auto* lit : collectFlexibleIntLits(callNode->getArgs()[i])) {
                if (!litFits(lit, ty)) return false;
            }
        }
        return true;
    };

    for (auto& tp : typeParams) {
        auto it = inferred.find(tp);
        if (it == inferred.end()) {
            throw RiuError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6013, tp, fnName);
        }
        TypeInfo t = it->second;
        if (t.isRef()) {
            if (auto inner = t.refElementType()) t = *inner;
        }
        // 仅灵活整数贡献的 T 默认为 i32 时，若字面量装不下则升到 i64。
        if (t.name == "i32" && !lockedFromConcrete.contains(tp) && !flexibleArgsFit(tp, "i32") &&
            flexibleArgsFit(tp, "i64")) {
            it->second = TypeInfo("i64");
        }
        outTypeArgs.push_back(it->second);
    }
}

// 泛型重载消歧 (Phase 3.3.1.a+)：从多个同名泛型函数中选最佳匹配。
// 逐个 inferGenericFnTypeArgs 推断 typeArgs，成功则按形参/实参 Ref 一致性打分。
std::pair<FnNode*, FileNode*> resolveBestGenericOverload(const std::vector<std::pair<FnNode*, FileNode*>>& genericFns,
                                                         ExprCallNode* callNode, const std::string& fnName,
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
        for (auto& bound : bounds[i]) {
            auto resolved = registry->resolve(bound.name, fnOwner);
            if (!resolved) {
                throw RiuError(line, col, ErrorCode::E3030, bound.name);
            }
            std::map<std::string, TypeInfo> subst;
            for (size_t j = 0; j < typeParams.size() && j < typeArgs.size(); ++j) {
                subst[typeParams[j]] = typeArgs[j];
            }
            auto specTypeArgs = substSpecTypeArgs(bound.typeArgs, subst);
            if (!checker->boundSatisfied(typeArgs[i], resolved->decl, resolved->qualifiedName, specTypeArgs)) {
                throw RiuError(line, col, ErrorCode::E1106, typeArgs[i].getFullName(),
                               formatSpecBound(resolved->qualifiedName, specTypeArgs), typeParams[i]);
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
    throw RiuError(line, col, ErrorCode::E1131, specBare.empty() ? string("?") : specBare);
}

// ==================== _ptr_offset 跨模块私有 (Phase 3.3.3.a, E6023) ====================
void validatePtrOffsetVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, int line, int col) {
    if (!fnSymbol) return;
    if (fnSymbol->isPrivate && !fnSymbol->moduleName.empty() && fnSymbol->moduleName != currentModuleName) {
        throw RiuError(line, col, ErrorCode::E6023);
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
        throw RiuError(line, col, ErrorCode::E6007, member, actualTypeName);
    }
}

void validateFnSymbolVisibility(const FnSymbolInfo* fnSymbol, const string& currentModuleName, const string& fnName,
                                int line, int col) {
    if (!fnSymbol) return;
    if (!fnSymbol->isPrivate || fnSymbol->moduleName.empty() || fnSymbol->moduleName == currentModuleName) return;
    // SDK 平铺文件（riu.core.*）之间允许私有调用（拆分前它们同属 riu.core 模块）
    auto isSdkFlat = [](const string& mod) -> bool { return mod.starts_with("riu.core.") || mod == "riu.core"; };
    if (isSdkFlat(fnSymbol->moduleName) && isSdkFlat(currentModuleName)) return;
    throw RiuError(line, col, ErrorCode::E6006, fnName);
}

// ==================== Builtin intrinsic arity (Phase 3.3.2.c) ====================
// 原 compileGenericFunctionCall 的 #Builtin 分支顶部散落的 typeArgs/args
// 计数检查 (~12 处 throw 跨 7 个 fnName) 收口到单一 helper.
void validateBuiltinIntrinsicShape(const string& fnName, size_t typeArgsCount, size_t argsCount, int line, int col) {
    if (fnName == "assert_eq") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 2) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(2));
        return;
    }
    if (fnName == "size_of") {
        if (typeArgsCount == 0) throw RiuError(line, col, ErrorCode::E6018);
        return;
    }
    if (fnName == "upgrade") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "same_ref" || fnName == "ptr_of") {
        size_t expectedArgs = (fnName == "same_ref" ? 2u : 1u);
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != expectedArgs) {
            throw RiuError(line, col, ErrorCode::E6027, fnName, expectedArgs);
        }
        return;
    }
    if (fnName == "as_ref" || fnName == "copy_of" || fnName == "weak") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // DRAFT-heap-types §8.3a.4.2 (Phase 3d): Heap<T>? 构造助手
    if (fnName == "heap_some") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "heap_null") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 0) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    // heap:<T>(v T) Heap<T> — 从值构造 owned Heap
    if (fnName == "heap") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // _heap_take:<T>(p Ptr) Heap<T> — 从裸 Ptr 接管（私有）
    if (fnName == "_heap_take") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // rc:<T>(v T) Rc<T> — 从值构造 owned Rc
    if (fnName == "rc") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // DRAFT-spec-reflect Phase 3a (捷径 A): __riu_reflect_type:<T>() 拿反射 Type 节点
    if (fnName == "__riu_reflect_type") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 0) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    // Phase B-1: move:<T>(x T&) T — 所有权转移
    if (fnName == "move") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // B-3: _ptr_as_ref:<T>(p Ptr) T& — 裸指针→引用（1 typeArg + 1 arg）
    if (fnName == "_ptr_as_ref") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    // B-3: _ptr_write:<T>(p Ptr, v T) — 裸指针写入（1 typeArg + 2 args）
    if (fnName == "_ptr_write") {
        if (typeArgsCount != 1) throw RiuError(line, col, ErrorCode::E6026, fnName, static_cast<size_t>(1));
        if (argsCount != 2) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(2));
        return;
    }
    // 未知 Builtin intrinsic
    throw RiuError(line, col, ErrorCode::E6017, fnName);
}

// ==================== 自由内建 intrinsic arity (Phase 3.3.2.b) ====================
// 原 `compileExternalOrSdkFunctionCall` 顶部三处 inline 分派 (line 724-752) 收口.
// 仅命中清单内的 fnName 才校验, 其他 fnName 是 no-op.
void validateFreeIntrinsicArity(const string& fnName, size_t argsCount, int line, int col) {
    if (fnName == "assert_eq") {
        if (argsCount != 2) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(2));
        return;
    }
    if (fnName == "assert_true" || fnName == "assert_false" || fnName == "fail") {
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "ptr_from_addr") {
        if (argsCount != 1) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(1));
        return;
    }
    if (fnName == "rc_leak_count") {
        if (argsCount != 0) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(0));
        return;
    }
    if (fnName == "_ptr_offset") {
        if (argsCount != 2) throw RiuError(line, col, ErrorCode::E6027, fnName, static_cast<size_t>(2));
        return;
    }
}

// ==================== Array<T> 方法形态校验 (Phase 3.3.2.a) ====================
void validateArrayMethodCall(const TypeInfo& baseType, const string& member, size_t argsCount, bool baseIsLvalue,
                             int line, int col) {
    auto* spec = lookupInstanceBuiltin(baseType, member);
    if (!spec) return;
    validateBuiltinMethodCall(*spec, baseType, argsCount, baseIsLvalue, line, col);
}

TypeInfo validateArrayMethodTypes(const TypeInfo& baseType, const string& member,
                                  const vector<TypeInfo>& methodTypeArgs, const vector<TypeInfo>& argTypes, int line,
                                  int col) {
    auto* spec = lookupInstanceBuiltin(baseType, member);
    if (!spec) return {};

    if (!methodTypeArgs.empty() || spec->typeArity == 0) {
        validateGenericTypeArgsArity(member, spec->typeArity, methodTypeArgs.size(), line, col);
    }

    TypeInfo methodTypeArg;
    const TypeInfo* methodTypeArgPtr = nullptr;
    if (spec->typeArity == 1) {
        if (!methodTypeArgs.empty()) {
            methodTypeArg = methodTypeArgs[0];
        } else if (!argTypes.empty() && argTypes[0].isFn()) {
            if (auto ret = argTypes[0].fnReturnType(); ret && !ret->empty()) {
                methodTypeArg = ret->withoutFallible();
            }
        }
        if (methodTypeArg.empty()) {
            throw RiuError(line, col, ErrorCode::E6013, "U", member);
        }
        methodTypeArgPtr = &methodTypeArg;
    }

    auto expected = builtinHigherOrderArgType(*spec, baseType, methodTypeArgPtr);
    if (!expected.empty() && !argTypes.empty() && argTypes[0] != expected) {
        throw RiuError(line, col, ErrorCode::E3014, expected.getFullName(), argTypes[0].getFullName());
    }

    return builtinMethodCallReturnType(*spec, baseType, methodTypeArgs, argTypes);
}

// ==================== LLVM 布局（0 LLVM，镜像 getLLVMType 成败）====================

namespace {
bool optimisticSdkName(const string& name) {
    return name == "String" || name == "StringBuilder" || name == "Type" || name == "Field" || name == "Method" ||
           name == "Variant";
}
} // namespace

bool typeHasLlvmLayout(const TypeInfo& raw, FileNode* file, FileNode* sdkFile,
                       const std::set<std::string>& typeParams) {
    NameResolver nr(file, sdkFile);
    std::set<string> visiting;
    std::function<bool(TypeInfo)> rec = [&](TypeInfo t) -> bool {
        try {
            t = resolveAlias(t, file, sdkFile);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return false;
        }
        t = t.withoutFallible();
        if (t.empty()) return true;
        if (t.isNormal() && typeParams.contains(t.name)) return true;
        // 句柄 / fat-ptr：LLVM 布局不依赖内层（与 getLLVMType 一致）
        if (t.isPtr() || t.isRef() || t.isRc() || t.isWeak() || t.isHeap() || t.isDyn() || t.isFn() ||
            t.isArrayGeneric()) {
            return true;
        }
        if (t.isNullable()) {
            auto inner = t.nullableInnerType();
            return inner && rec(*inner);
        }
        if (t.isArray()) {
            return t.elementType && rec(*t.elementType);
        }
        if (t.isTuple()) {
            for (auto& e : t.tupleElements()) {
                if (!e || !rec(*e)) return false;
            }
            return true;
        }
        if (isBuiltinType(t.name)) return true;
        if (t.name == "Ptr" || t.name == "Self" || t.name == "Function") return true;
        if (t.name == "Type" || t.name == "Field" || t.name == "Method" || t.name == "Variant") return true;

        const string key = t.identityKey();
        if (!visiting.insert(key).second) return true;

        auto finish = [&](bool ok) {
            visiting.erase(key);
            return ok;
        };

        auto* sd = nr.lookupStruct(t);
        if (sd) {
            if (sd->isGeneric()) {
                if (t.genericArgs.size() != sd->typeParams().size()) return finish(true); // E6011
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < sd->typeParams().size(); ++i) {
                    if (!t.genericArgs[i] || t.genericArgs[i]->empty()) return finish(true);
                    subst[sd->typeParams()[i]] = *t.genericArgs[i];
                }
                for (auto* f : sd->fields()) {
                    if (!f || f->isStatic()) continue;
                    TypeInfo ft = f->getType().substitute(subst);
                    if (!rec(std::move(ft))) return finish(false);
                }
                return finish(true);
            }
            for (auto* f : sd->fields()) {
                if (!f || f->isStatic()) continue;
                if (!rec(f->getType())) return finish(false);
            }
            return finish(true);
        }
        auto* ed = nr.lookupEnum(t);
        if (ed) {
            if (ed->isGeneric()) {
                // 缺参 / 错元留给 E6011；与泛型 struct 同档，不在这里报 E3096。
                if (t.genericArgs.size() != ed->typeParams().size()) return finish(true);
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < ed->typeParams().size(); ++i) {
                    if (!t.genericArgs[i] || t.genericArgs[i]->empty()) return finish(true);
                    subst[ed->typeParams()[i]] = *t.genericArgs[i];
                }
                for (auto* v : ed->variants()) {
                    if (!v || !v->hasPayload()) continue;
                    for (auto* pt : v->payloadTypes()) {
                        if (!pt) continue;
                        if (!rec(pt->getType().substitute(subst))) return finish(false);
                    }
                }
                return finish(true);
            }
            for (auto* v : ed->variants()) {
                if (!v || !v->hasPayload()) continue;
                for (auto* pt : v->payloadTypes()) {
                    if (!pt || !rec(pt->getType())) return finish(false);
                }
            }
            return finish(true);
        }
        visiting.erase(key);
        return !sdkFile && optimisticSdkName(t.name);
    };
    return rec(raw);
}

void validateGenericStructFieldLayouts(StructDeclNode* sd, const std::map<std::string, TypeInfo>& subst, FileNode* file,
                                       FileNode* sdkFile, const std::set<std::string>& typeParams) {
    if (!sd || subst.empty()) return;
    const string baseName = sd->name().getText();
    for (auto* f : sd->fields()) {
        if (!f || f->isStatic()) continue;
        TypeInfo ft = f->getType().substitute(subst);
        if (ft.isNormal() && typeParams.contains(ft.name)) continue;
        if (typeHasLlvmLayout(ft, file, sdkFile, typeParams)) continue;
        throw RiuError(static_cast<int>(f->name().getLine()), ErrorCode::E3098, ft.getFullName(), f->name().getText(),
                       baseName);
    }
}

// ==================== Builtin intrinsic 类型形态校验 (Phase 3.3.2.d) ====================
// 原 compileGenericFunctionCall 的 #Builtin 分支内散落的 E6028 / E6029 / E6032 / E6030 / E6031
// 校验 (跨 same_ref / ptr_of / as_ref / weak / copy_of / assert_eq) 收口到单一 helper.
void validateBuiltinIntrinsicTypeShape(const string& fnName, const vector<TypeInfo>& typeArgs,
                                       const vector<TypeInfo>& argTypes, const vector<ExprNode*>& argNodes,
                                       FileNode* file, FileNode* sdkFile, int line, int col) {
    if (fnName == "same_ref" || fnName == "ptr_of") {
        // arity / typeArgs 计数已由 validateBuiltinIntrinsicShape 保证
        const auto& T = typeArgs[0];
        bool isHeapHandle = T.isRcHandle();
        // Phase 8a: ptr_of:<Heap<T>>(h) move-out FFI handoff (DRAFT-heap-types §8.3a)
        // same_ref 不接受 Heap (Heap 单所有权, 两个 Heap 不可能指同一块, 比较无意义)
        bool isHeapForPtrOf = (fnName == "ptr_of" && T.isHeap());
        // 实参为 T& 时：
        //   - ExprGetRefNode (&x) → getType() 返回 i32& → argTypes 保留引用
        //   - LiteralObjNode (ref 变量) → getType() 自动剥引用 → argTypes 丢失引用
        // 因此同时检查 argTypes 和 AST 符号类型。
        bool argIsRef = false;
        for (auto& at : argTypes) {
            if (at.isRef()) {
                argIsRef = true;
                break;
            }
        }
        if (!argIsRef) {
            for (auto& node : argNodes) {
                if (auto lit = dynamic_cast<ExprLiteralNode*>(node)) {
                    if (auto objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                        auto scope = objLit->findNearestScope();
                        if (scope) {
                            auto sym = scope->lookupSymbol(objLit->getValue().getText());
                            if (sym && sym->type->isRef()) {
                                argIsRef = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!isHeapHandle && !T.isRef() && !isHeapForPtrOf && !argIsRef) {
            throw RiuError(line, col, ErrorCode::E6029, fnName, T.getFullName());
        }
        if (T.isRef() || isHeapForPtrOf || argIsRef) {
            // 取源裸指针仅支持: ID-literal (栈/堆变量) 或 ExprGetRefNode (`&x` 字面)
            // _localVarPtrs 查不到：sema 已校验 AST 形态，Compiler 改 throwSemaGap.
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
                    throw RiuError(line, col, ErrorCode::E6028, fnName);
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
            throw RiuError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
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
            throw RiuError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
        }
        throw RiuError(line, col, ErrorCode::E6029, fnName, argType.getFullName());
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
                const TypeInfo& ft = field->getType();
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
            throw RiuError(line, col, ErrorCode::E6032, refPath);
        }
        return;
    }
    // Phase B-1: move:<T>(x T&) T
    // E4035: T 本身不能是引用（如 move:<i32&>(x)）
    // E4034: 参数必须是 lvalue（变量引用或 &x），不能是纯右值（字面量/调用结果）
    if (fnName == "move") {
        if (typeArgs.size() >= 1 && typeArgs[0].isRef()) {
            throw RiuError(line, col, ErrorCode::E4035);
        }
        if (argNodes.size() >= 1) {
            // 参数必须是 ID-literal（栈/堆变量）或 ExprGetRefNode（&x）
            // 与 same_ref/ptr_of 同模式；不满足时抛 E4034 而非 E6028
            bool ok = false;
            if (auto* lit = dynamic_cast<ExprLiteralNode*>(argNodes[0])) {
                if (dynamic_cast<LiteralObjNode*>(lit->literal())) ok = true;
            }
            if (!ok && dynamic_cast<ExprGetRefNode*>(argNodes[0])) ok = true;
            if (!ok) {
                throw RiuError(line, col, ErrorCode::E4034, argTypes.size() >= 1 ? argTypes[0].getFullName() : "?");
            }
        }
        return;
    }
    // heap:<T>(v T) Heap<T> — arg 类型必须 == T
    if (fnName == "heap") {
        if (typeArgs.size() >= 1 && argTypes.size() >= 1) {
            const auto& T = typeArgs[0];
            const auto& argType = argTypes[0];
            if (argType.isPtr()) {
                // Ptr → Heap<T> 接管路径留给 _heap_take，heap 不接受
                throw RiuError(line, col, ErrorCode::E3014, T.name, "Ptr");
            }
            if (!(argType == T)) {
                throw RiuError(line, col, ErrorCode::E3014, T.name, argType.name);
            }
        }
        return;
    }
    // _heap_take:<T>(p Ptr) Heap<T> — arg 必须是 Ptr（私有）
    if (fnName == "_heap_take") {
        if (argTypes.size() >= 1) {
            const auto& argType = argTypes[0];
            if (!argType.isPtr()) {
                throw RiuError(line, col, ErrorCode::E3014, "Ptr", argType.name);
            }
        }
        return;
    }
    // rc:<T>(v T) Rc<T> — arg 类型必须 == T
    if (fnName == "rc") {
        // 类型校验由 codegen 端在 CreateStore 前做（对齐 Rc 初始化路径）
        return;
    }
    // assert_eq:<T>(actual, expected)：T 须是数值 / bool；两实参 LLVM 位宽组须一致。
    // 与 compileTestAssertEq 对齐。模板形参由 caller 跳过。
    if (fnName == "assert_eq") {
        if (typeArgs.empty()) return;
        auto peelT = [](TypeInfo t) {
            if (t.isRef() && t.refElementType()) t = *t.refElementType();
            if (t.isHeap()) {
                if (auto inner = t.heapElementType()) t = *inner;
            }
            return t;
        };
        auto peelArg = [](TypeInfo t) {
            if (t.isHeap()) {
                if (auto inner = t.heapElementType()) return *inner;
            } else if (t.isRef() && t.refElementType()) {
                t = *t.refElementType();
            }
            return t;
        };
        auto llvmGroup = [](const TypeInfo& t) -> const char* {
            const string& n = t.name;
            if (n == "i8" || n == "u8") return "i8";
            if (n == "i16" || n == "u16") return "i16";
            if (n == "i32" || n == "u32") return "i32";
            if (n == "i64" || n == "u64" || n == "isize" || n == "usize") return "i64";
            if (n == "f32") return "f32";
            if (n == "f64") return "f64";
            if (n == "bool") return "i1";
            return nullptr;
        };
        TypeInfo t = peelT(typeArgs[0]);
        try {
            t = resolveAlias(t, file, sdkFile);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        t = peelT(t);
        const string& tname = t.name;
        const bool okScalar = isIntTypeName(tname) || tname == "bool" || t.isFloat();
        if (!okScalar) {
            throw RiuError(line, col, ErrorCode::E6030, t.getFullName());
        }
        if (argTypes.size() < 2 || argNodes.size() < 2) return;
        TypeInfo a0 = peelArg(argTypes[0]);
        TypeInfo a1 = peelArg(argTypes[1]);
        try {
            a0 = peelArg(resolveAlias(a0, file, sdkFile));
            a1 = peelArg(resolveAlias(a1, file, sdkFile));
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        if (isFlexibleIntExpr(argNodes[0]) && isIntTypeName(t.name)) a0 = t;
        if (isFlexibleIntExpr(argNodes[1]) && isIntTypeName(t.name)) a1 = std::move(t);
        const char* g0 = llvmGroup(a0);
        const char* g1 = llvmGroup(a1);
        if (!g0 || !g1) return;
        if (g0 != g1) {
            throw RiuError(line, col, ErrorCode::E6031, a0.getFullName(), a1.getFullName());
        }
        return;
    }
    if (fnName == "size_of") {
        // 镜像 compileGenericFunctionCall：getLLVMType 失败 → E6019
        static const std::set<string> kNoTypeParams;
        if (!typeHasLlvmLayout(typeArgs[0], file, sdkFile, kNoTypeParams)) {
            throw RiuError(line, col, ErrorCode::E6019, typeArgs[0].getFullName());
        }
        return;
    }
    if (fnName == "__riu_reflect_type") {
        // 镜像 ensureReflectTypeGlobal：仅 Normal 用户 / SDK struct
        const auto& T = typeArgs[0];
        if (T.kind != TypeKind::Normal || T.name.empty() || !NameResolver(file, sdkFile).lookupStruct(T)) {
            throw RiuError(line, col, ErrorCode::E6019, T.getFullName());
        }
        return;
    }
    // 其他 intrinsic (upgrade 等) 无类型形态校验, no-op
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
                throw RiuError(line, col, ErrorCode::E6027, member, static_cast<size_t>(1));
            }
            return;
        }
    }
    // 一元 inv: 不接受 float
    if (member == "inv") {
        if (baseType.isFloat()) {
            throw RiuError(line, col, ErrorCode::E3070, baseType.name);
        }
        return;
    }
    // neg / not 等其他一元 op 无校验
}

// ========== Phase 3.4.a: 枚举构造表达式形态校验 ==========

namespace {
// 用户友好类型渲染: Rc<T> / Array<T> / [N]T / Generic<A,B>
// 与 compiler_expr.cpp compileEnumCtorExpr 内 fmtType lambda 等价.
string fmtTypeFriendly(const TypeInfo& t) {
    if (t.hasGenericArgs() && !t.genericArgs.empty()) {
        string r = t.name + "<";
        for (size_t j = 0; j < t.genericArgs.size(); ++j) {
            if (j) r += ", ";
            r += t.genericArgs[j] ? fmtTypeFriendly(*t.genericArgs[j]) : string("?");
        }
        r += '>';
        return r;
    }
    if (t.kind == TypeKind::Array && t.elementType) {
        return "[" + std::to_string(t.arraySize) + "]" + fmtTypeFriendly(*t.elementType);
    }
    return t.name;
}

void throwEnumCtorTypeArity(const string& name, size_t want, size_t got, int line, int col) {
    auto err = RiuError(line, col, ErrorCode::E6011, name, want, got);
    if (want == 0) {
        throw err.withHint(std::format("非泛型 enum `{}` 不能写 `:<...>` turbofish，去掉类型实参", name));
    }
    throw err.withHint(std::format("泛型 enum 构造须写 turbofish；改写为 `{}:<{}>::V` 形式补齐 {} 个类型", name,
                                   std::string(want == 1 ? "T" : "T1, T2, ..."), want));
}
} // namespace

void validateEnumCtorShape(FileNode* file, FileNode* sdkFile, ExprPathCallNode* node) {
    if (!node) return;
    const TypeInfo& enumTy = node->getType();
    string enumName = enumTy.name; // 经别名 / 路径解析后的真实 enum 名
    string enumNameRaw = node->lhsPath().empty() ? node->enumName().getText() : node->lhsPath().dotted();
    string variantName = node->variantName().getText();
    int line = node->getLineNumber();
    int col = node->getColumn();

    auto* enumDecl = NameResolver(file, sdkFile).lookupEnum(enumTy);
    if (!enumDecl) {
        throw RiuError(line, col, ErrorCode::E2019, enumNameRaw, enumNameRaw, variantName);
    }

    const auto& tps = enumDecl->typeParams();
    const auto& lhsTArgs = node->lhsTypeArgs();
    if (tps.empty()) {
        if (!lhsTArgs.empty()) throwEnumCtorTypeArity(enumName, 0, lhsTArgs.size(), line, col);
    } else if (lhsTArgs.size() != tps.size()) {
        throwEnumCtorTypeArity(enumName, tps.size(), lhsTArgs.size(), line, col);
    }

    map<string, TypeInfo> subst;
    if (!tps.empty()) {
        vector<TypeInfo> owned;
        owned.reserve(tps.size());
        bool argsOk = true;
        for (size_t i = 0; i < tps.size(); ++i) {
            try {
                TypeInfo a = lhsTArgs[i]->getType();
                subst[tps[i]] = a;
                owned.push_back(std::move(a));
            } catch (...) { // NOLINT(bugprone-empty-catch)
                argsOk = false;
                subst.clear();
                break;
            }
        }
        if (argsOk) validateOwnedTypeArgs(enumName, owned, line, col);
    }

    auto* variant = enumDecl->variant(variantName);
    if (!variant) {
        throw RiuError(line, col, ErrorCode::E2020, enumName, variantName);
    }

    size_t givenArity = node->args().size();
    size_t declArity = variant->payloadArity();
    if (givenArity != declArity) {
        throw RiuError(line, col, ErrorCode::E2021, enumName, variantName, declArity, givenArity);
    }

    // E2032: 实参类型与 variant payload 类型严格匹配.
    // 泛型 enum 先按该次 turbofish 实参 subst 再比.
    // 实参类型 argExpr->getType() 任一抛错 (lambda 形参未推断 等) 时跳过该参数,
    // 留 codegen 原路径继续报.
    for (size_t i = 0; i < declArity; ++i) {
        auto argExpr = node->args()[i];
        TypeInfo expectedType;
        TypeInfo actualType;
        try {
            expectedType = variant->payloadTypes()[i]->getType();
            if (!subst.empty()) expectedType = expectedType.substitute(subst);
            actualType = argExpr->getType();
        } catch (...) {
            continue;
        }
        if (!(expectedType == actualType)) {
            throw RiuError(argExpr->getLineNumber(), argExpr->getColumn(), ErrorCode::E2032, enumName, variantName, i,
                           fmtTypeFriendly(expectedType), fmtTypeFriendly(actualType));
        }
    }
}

// ========== Phase 3.4.b / 泛型 enum 3.4: match arm 静态校验 ==========

void validateMatchArms(EnumDeclNode* enumDecl, const TypeInfo& enumType, ExprMatchNode* node, FileNode* file) {
    if (!enumDecl || !node) return;
    auto& arms = node->arms();
    int line = node->getLineNumber();
    int col = node->getColumn();
    const string& enumName = enumType.name;
    // 诊断用带实参的完整类型（`Box<i32>`）；穷尽仍按声明上的 variant 名。
    const string shown = fmtTypeFriendly(enumType);

    if (arms.empty()) {
        throw RiuError(line, col, ErrorCode::E2023, shown, string("(none)"));
    }

    set<string> seenVariants;
    bool hasElse = false;
    for (size_t i = 0; i < arms.size(); ++i) {
        auto arm = arms[i];
        auto pat = arm->pattern();
        if (pat->isElse()) {
            if (i + 1 != arms.size()) {
                throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2025);
            }
            hasElse = true;
            continue;
        }
        TypeInfo patTy;
        if (!pat->enumPath().empty()) {
            auto r = resolveExprTypeLhs(pat, file, nullptr, pat->enumPath(), pat->getLineNumber(), pat->getColumn());
            patTy = r.type;
            if (!r.resolved || patTy.name != enumName || !patTy.sameOwner(enumType)) {
                string patShown = pat->enumPath().dotted();
                throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2019, patShown, patShown,
                               pat->variantName().getText());
            }
        } else if (pat->enumName().getText() != enumName) {
            throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2019, pat->enumName().getText(),
                           pat->enumName().getText(), pat->variantName().getText());
        }

        string vName = pat->variantName().getText();
        auto* variant = enumDecl->variant(vName);
        if (!variant) {
            throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2020, shown, vName);
        }
        if (seenVariants.count(vName)) {
            throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2024, shown, vName);
        }
        seenVariants.insert(vName);

        size_t bindArity = pat->binds().size();
        size_t declArity = variant->payloadArity();
        if (bindArity != declArity && !(bindArity == 0 && declArity == 0)) {
            throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2026, shown, vName, declArity,
                           bindArity);
        }

        set<string> seenBinds;
        for (auto& tk : pat->binds()) {
            const string& bn = tk.getText();
            if (isDiscardName(bn)) continue;
            if (seenBinds.count(bn)) {
                throw RiuError(pat->getLineNumber(), pat->getColumn(), ErrorCode::E2027, bn, shown, vName);
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
                s += shown + "::" + missing[i];
            }
            throw RiuError(line, col, ErrorCode::E2023, shown, s);
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
    throw RiuError(line, col, ErrorCode::E3042, fieldName, baseTypeName);
}

void validateGetRefPrivacy(FileNode* file, FileNode* sdkFile, ExprGetRefNode* node, const string& accessorStructName) {
    if (!node) return;
    auto scope = node->findNearestScope();
    if (!scope) return;
    auto sym = scope->lookupSymbol(node->obj().getText());
    if (!sym) return; // E3030 由 getType 抢; 这里静默

    TypeInfo currentType = *sym->type;
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

        auto structDecl = NameResolver(file, sdkFile).lookupStruct(lookupType.name);
        if (!structDecl) return; // E3041 由 getType 抢

        int idx = structDecl->fieldIndex(sub.getText());
        if (idx < 0) return; // E3040 由 getType 抢

        validatePrivateFieldAccess(structDecl, sub.getText(), lookupType.name, accessorStructName, line, col);

        // 推进 currentType: 取 field type, 含泛型实参替换 (与 getType 同款).
        TypeInfo fieldType = structDecl->fields()[idx]->getType();
        if (lookupType.hasGenericArgs() && structDecl->isGeneric() &&
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

void validateDotFieldPrivacy(FileNode* file, FileNode* sdkFile, ExprDotNode* node, const string& accessorStructName) {
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

    auto structDecl = NameResolver(file, sdkFile).lookupStruct(actualType.name);
    if (!structDecl) return; // 不是 struct 字段访问 (可能 method / 别的形态), 跳过

    validatePrivateFieldAccess(structDecl, node->member(), actualType.name, accessorStructName,
                               node->resolveLineNumber(), node->resolveColumn());
}

// ==================== 整数字面量解析 (Phase 3.4.f.2) ====================

namespace {

bool intTypeIsUnsigned(const string& name) {
    return name == "u8" || name == "u16" || name == "u32" || name == "u64" || name == "usize";
}

// 返回位宽；未知名返回 0.
int intTypeBitWidth(const string& name) {
    if (name == "i8" || name == "u8") return 8;
    if (name == "i16" || name == "u16") return 16;
    if (name == "i32" || name == "u32") return 32;
    if (name == "i64" || name == "u64") return 64;
    if (name == "isize" || name == "usize") return static_cast<int>(sizeof(void*) * 8);
    return 0;
}

bool intMagFits(u64 mag, bool negative, const string& typeName) {
    int w = intTypeBitWidth(typeName);
    if (w <= 0) return false;
    if (intTypeIsUnsigned(typeName)) {
        if (negative && mag != 0) return false;
        if (w >= 64) return true;
        return mag < (static_cast<u64>(1) << static_cast<unsigned>(w));
    }
    if (w >= 64) {
        const u64 i64MinMag = u64{1} << 63u;
        return negative ? mag <= i64MinMag : mag < i64MinMag;
    }
    u64 maxPos = (static_cast<u64>(1) << static_cast<unsigned>(w - 1)) - 1;
    u64 maxNeg = static_cast<u64>(1) << static_cast<unsigned>(w - 1);
    return negative ? mag <= maxNeg : mag <= maxPos;
}

} // namespace

i64 parseIntLiteral(const string& text, int line, int col, const string& typeName, bool negatedOperand) {
    int errLine = line > 0 ? line : 1;
    auto throwRange = [&](const string& ty) { throw RiuError(errLine, col, ErrorCode::E3103, text, ty); };

    string numStr = text;

    // 识别类型后缀（与 lexer INT_SUFFIX 对齐）
    static const std::regex suffix_regex(R"([iu](?:8|16|32|64|size)$)");
    std::smatch m;
    string suffix;
    if (std::regex_search(numStr, m, suffix_regex)) {
        suffix = m.str();
    }
    numStr = std::regex_replace(numStr, suffix_regex, "");

    string checkType = !typeName.empty() ? typeName : (!suffix.empty() ? suffix : string("i32"));
    if (intTypeBitWidth(checkType) <= 0) {
        throwRange(checkType);
    }

    bool negative = false;
    if (!numStr.empty() && (numStr[0] == '+' || numStr[0] == '-')) {
        negative = numStr[0] == '-';
        numStr = numStr.substr(1);
    }
    if (negatedOperand) negative = !negative;

    int base = 10;
    string parseStr = numStr;

    // 进制前缀
    if (numStr.size() >= 2 && numStr[0] == '0') {
        if (numStr[1] == 'b' || numStr[1] == 'B') {
            base = 2;
            parseStr = numStr.substr(2);
        } else if (numStr[1] == 'o' || numStr[1] == 'O') {
            base = 8;
            parseStr = numStr.substr(2);
        } else if (numStr[1] == 'x' || numStr[1] == 'X') {
            base = 16;
            parseStr = numStr.substr(2);
        }
    }

    std::erase(parseStr, '_');
    if (parseStr.empty()) throwRange(checkType);

    u64 mag = 0;
    try {
        mag = std::stoull(parseStr, nullptr, base);
    } catch (const std::out_of_range&) {
        throwRange(checkType);
    } catch (const std::invalid_argument&) {
        throwRange(checkType);
    }

    if (!intMagFits(mag, negative, checkType)) throwRange(checkType);

    // 外层还会 CreateNeg / evalUnary：返回幅度位，避免双重取负。
    if (negatedOperand) return static_cast<i64>(mag);
    if (negative) return static_cast<i64>(static_cast<u64>(0) - mag);
    return static_cast<i64>(mag);
}

// Bucket 6 单点: 比较表达式 leftType 形态校验.
void validateCompareOpForm(const TypeInfo& leftType, ExprCompareNode::Op op, int line, int col) {
    if (leftType.isWeak()) {
        if (op == ExprCompareNode::Op::Eq || op == ExprCompareNode::Op::Ne) {
            throw RiuError(line, col, ErrorCode::E3078)
                .withHint("先 `upgrade(weak)` 取得 Rc<T>?，再用 `?.` / `??` / 相等比较判定目标对象");
        }
    }
    if (leftType.isFn()) {
        if (op == ExprCompareNode::Op::Eq || op == ExprCompareNode::Op::Ne) {
            const char* opSym = op == ExprCompareNode::Op::Eq ? "==" : "!=";
            const char* mname = op == ExprCompareNode::Op::Eq ? "eq" : "ne";
            throw RiuError(line, col, ErrorCode::E3073, leftType.getFullName(), opSym, mname)
                .withHint("function values have no equality; address comparison is not provided");
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
        throw RiuError(line, col, ErrorCode::E3073, "Ptr", opSym, mname)
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
        const TypeInfo& candParam = cand->paramType(1);

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
            const TypeInfo& cp = cand->paramType(1);
            bool matches = (!cp.isRef() && cp == rightType) ||
                           (cp.isRef() && cp.refElementType() && *cp.refElementType() == rightType);
            if (!matches) continue;
            if (!sigs.empty()) sigs += " | ";
            sigs += leftType.name + "." + methodName + "(" + cp.name + ")";
        }
        int matchCount = exactCount + refCount;
        throw RiuError(line, col, ErrorCode::E6014, methodFullName, rightType.name, matchCount, sigs);
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
    throw RiuError(line, col, ErrorCode::E3073, leftType.name, opSym, methodName);
}

// 一元运算符：`Type.neg` / `Type.inv` / `Type.not`，形参仅为接收者（与 compileCustomTypeUnaryOp 对齐）。
void validateUnaryOpMethodResolution(FileNode* file, FileNode* sdkFile, const TypeInfo& operandType,
                                     const string& methodName, int line, int col) {
    string methodFullName = operandType.name + "." + methodName;
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(operandType);

    FnSymbolInfo* methodSymbol = nullptr;
    if (file) methodSymbol = file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && sdkFile && sdkFile != file) {
        methodSymbol = sdkFile->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    if (methodSymbol) return;

    const char* opSym = methodName == "neg"   ? "-"
                        : methodName == "inv" ? "~"
                        : methodName == "not" ? "!"
                                              : methodName.c_str();
    throw RiuError(line, col, ErrorCode::E3074, operandType.name, opSym, methodName);
}

bool typeImplementsToString(FileNode* file, FileNode* sdkFile, const TypeInfo& t) {
    if (t.isString() || t.name == "String") return true;
    string fullName = t.name + ".to_string";
    if (sdkFile && sdkFile->lookupFnSymbol(fullName)) return true;
    if (file && file->lookupFnSymbol(fullName)) return true;
    return false;
}

// Bucket 6 单点: 字符串模板插值 ToString 校验 (E3026).
void validateStringTemplateInterps(FileNode* file, FileNode* sdkFile, StringTemplateNode* tpl) {
    if (!tpl) return;
    for (auto& e : tpl->interps()) {
        TypeInfo t;
        try {
            t = e->getType();
        } catch (...) {
            continue; // lambda 形参等未推断, 留 Compiler 兜底
        }
        if (!typeImplementsToString(file, sdkFile, t)) {
            throw RiuError(e->getLineNumber(), e->getColumn(), ErrorCode::E3026, t.name);
        }
    }
}

void validateArrayWithCapacity(ExprPathCallNode* node) {
    if (!node) return;
    auto* spec = lookupStaticBuiltin("Array", "with_capacity");
    int line = node->getLineNumber();
    int col = node->getColumn();
    const auto& lhsTArgs = node->lhsTypeArgs();
    if (lhsTArgs.size() != 1) {
        throw RiuError(line, col, ErrorCode::E6011, "Array", static_cast<size_t>(1), lhsTArgs.size());
    }
    const size_t expectArity = spec ? static_cast<size_t>(spec->arity) : 1;
    const char* expectArg0 = spec && spec->arg0Type ? spec->arg0Type : "usize";
    if (node->args().size() != expectArity) {
        string got;
        for (size_t i = 0; i < node->args().size(); ++i) {
            if (i) got += ", ";
            try {
                got += node->args()[i]->getType().getFullName();
            } catch (...) {
                got += '?';
            }
        }
        throw RiuError(line, col, ErrorCode::E3131, "Array", "with_capacity", expectArity, expectArg0,
                       node->args().size(), got);
    }
    TypeInfo expectTy(expectArg0);
    tryInferIntType(node->args()[0], expectTy);
    TypeInfo actualTy;
    try {
        actualTy = node->args()[0]->getType();
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return;
    }
    if (!actualTy.empty() && !(actualTy == expectTy)) {
        throw RiuError(line, col, ErrorCode::E3131, "Array", "with_capacity", expectArity, expectArg0,
                       static_cast<size_t>(1), actualTy.getFullName());
    }
}

} // namespace sema

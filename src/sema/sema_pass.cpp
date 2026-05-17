// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 实现 —— 详见 sema_pass.h
//
// Phase 3.2a：visitExpr 在每个表达式节点上写入 `setResolvedType(getType())`,
// 覆盖范围扩到 file 顶层 fn body + struct impl 的方法/析构 body。
// 泛型模板 / #CompilerInner 仍跳过 —— 它们的 codegen 路径会自行写 resolvedType,
// 留作本步 known-issue (lambda 反推 / 泛型 applySubst 的"运行时再写")。
//
// Phase 3.2b 前置 (本步)：将 `getType()` 抛出的"已迁移诊断"从静默吞掉改为
// 向外抛, 让 SemaPass 实际接管该错误码。当前已迁移清单 (kMigratedCodes)：
//   C1 算术 / 比较 / 分支结果类型不匹配:
//     - E3001 / E3002 / E3003: 算术 / 乘除模 / 二元位运算左右类型不匹配
//     - E3004:                 比较运算左右类型不匹配
//     - E3005 / E3006:         if-elif / if-else 分支结果类型不匹配
//     - E3007:                 one-line if-else 真假分支类型不匹配
//     - E3008:                 if-else 预值表达式真假分支类型不匹配
//   C1 字段 / 元组访问 / 引用 / 索引 形态:
//     - E3025: `.?` safe-dot base 不是 Nullable
//     - E3040: 字段不存在 (struct / nullable struct payload)
//     - E3041: `&` getRef 时 struct 未找到
//     - E3043: `&` getRef 时找不到 file 作用域
//     - E3044: `.?` safe-dot 在 inner struct 上找不到 struct decl
//     - E3050: Rc<T>? 取 inner 时 Rc 元素类型缺失
//     - E3051: `.?` safe-dot 的 nullable inner 类型缺失
//     - E3057: 数组索引时元素类型缺失 (含 Array 泛型 / 普通数组)
//     - E3062: 索引目标非数组
//     - E3097: `&` getRef 时找不到 nearestScope
//     - E3100: 元组下标越界
// 其余错误码 (lambda 形参未推断、泛型 arity E3095 等) 仍走原 codegen 路径
// 报错; 等后续 batch 一并迁过来再扩 kMigratedCodes。

#include "sema/sema_pass.h"

#include <array>
#include <string_view>

#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "sema/call_resolve.h"
#include "types.h"

namespace {
// Phase 3.2b 已由 SemaPass 接管的错误码白名单。SemaPass 在 visitExpr 中
// 捕获 YuxError 时, 命中此清单的直接 rethrow, 让 SemaPass 成为该诊断的
// 实际抛出点。新增迁移码追加到此处即可。
constexpr std::array<std::string_view, 24> kMigratedCodes = {
    // 算术 / 比较 / 分支结果
    "E3001", "E3002", "E3003", "E3004",
    "E3005", "E3006", "E3007", "E3008",
    // 数组 / 字段 / 元组 / 引用
    // E3011 (数组元素类型不一致) 暂不迁移: 嵌套数组字面量 / 目标类型上下文
    // (`var rows Array<Array<i32>> = [[1,2],[3,4,5]]`) 在 codegen 走 target-type
    // 驱动路径, 不调用 `ExprArrayNode::getType()`; 但 SemaPass 下钻 visitExpr
    // 时会触发 E3011, 是假阳性。需把"目标类型上下文"协议建到 SemaPass 里才能
    // 安全迁; 留作下一批。
    // Phase 3.4.f.1: E3009 (ArrayInit explicitType vs value 字面量不匹配) 已迁入
    // ExprArrayInitNode::getType. 不依赖 targetType, AST 层即可判定. E3010
    // 依赖 targetType, 留 codegen 兜底.
    "E3009",
    "E3025",
    "E3040", "E3041", "E3043", "E3044",
    "E3050", "E3051", "E3057", "E3062",
    "E3097",
    "E3100",
    // Phase 3.4.f.2: 字面量越界
    "E3103",
    // Phase 3.4.h: ExprUnaryNode 内置 op 形态校验 (Rev on float / Not on non-bool)
    "E3070", "E3071",
    // Phase 2.6 (heap-types): Heap:<T>(arg) 形参类型不匹配
    "E3028",
};

// 与 Compiler::lookupEnumDecl 等价的本地版本: 本文件 → SDK → wildcard imports.
// SemaPass 不依赖 LLVM, 无法直接调用 Compiler 成员, 这里复制查找规则。
EnumDeclNode* lookupEnumIn(p<FileNode> file, p<FileNode> sdkFile, const string& name) {
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

// Phase 3.3.2.f: 与 Compiler::isCompilerInnerMethod 等价的本地版本.
// 仅查 sdkFile 的 struct impl (内建运算符方法都注册在 SDK 上), 不存在
// 时返回 false. Sema 不依赖 Compiler 成员, 这里复制规则.
bool isCompilerInnerMethodIn(FileNode* sdkFile,
                             const string& structName,
                             const string& methodName) {
    if (!sdkFile) return false;
    auto structImpl = sdkFile->getStructImpl(structName);
    if (!structImpl) return false;
    for (auto& m : structImpl->methods()) {
        if (m->header()->name().getText() == methodName) {
            return m->header()->hasAnno("CompilerInner");
        }
    }
    return false;
}

// Phase 3.3.2.f: 近似 Compiler 端 compileArrayMethodCall 的 arrayPtr 计算 ——
// arrayPtr 非空当且仅当 baseExpr AST 形态为:
//   * ID-literal (栈/堆局部变量)
//   * ID-literal.<field> ... 的 Dot 链 (struct 字段直接命名)
// 其余形态 (函数调用、字面量、表达式) 在 Compiler 端 arrayPtr 仍为 nullptr,
// 视为 rvalue. SemaPass 没有 _localVarPtrs, 改走 AST 形态判定; 与 Compiler
// 实际语义等价 (Compiler 也只支持这两种 AST 形态查到栈/堆指针).
bool isLvalueArrayBase(ExprNode* baseExpr) {
    while (baseExpr) {
        if (auto lit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            return dynamic_cast<LiteralObjNode*>(lit->literal()) != nullptr;
        }
        if (auto dot = dynamic_cast<ExprDotNode*>(baseExpr)) {
            baseExpr = dot->baseExpr();
            continue;
        }
        return false;
    }
    return false;
}

bool isMigratedCode(const char* code) {
    if (!code) return false;
    std::string_view sv(code);
    for (auto c : kMigratedCodes) {
        if (sv == c) return true;
    }
    return false;
}
}

SemaPass::SemaPass(p<FileNode> file, p<FileNode> sdkFile, string sourcePath)
    : _file(file), _sdkFile(sdkFile), _sourcePath(std::move(sourcePath)) {
}

void SemaPass::run() {
    if (!_file) return;
    for (auto& fn : _file->getFunctions()) {
        // 泛型模板 / #CompilerInner 不走常规 codegen, 在 Compiler::compile 里也
        // 是被跳过的; SemaPass 这里同步跳过, 保持与 codegen 覆盖一致。
        if (fn->header()->isGeneric()) continue;
        if (fn->header()->hasAnno("CompilerInner")) continue;
        visitFn(fn);
    }
    // struct impl 内的方法 / 析构 body 同样要走 SemaPass —— 它们的 codegen
    // 入口也是 compile<Foo>Expr, 不覆盖会导致后续 3.2b 把 set 改 assert 时
    // 方法体内表达式全部 assert 失败。
    for (auto& impl : _file->getStructImpls()) {
        if (impl->isGeneric()) continue;
        // Phase 3.4.d.2: 进入 impl 时记录 currentStructName, 供 visitExpr 走
        // ExprGetRefNode / ExprDotNode 字段访问时校验 E3042 私有可见性.
        _currentStructName = impl->structName();
        for (auto& m : impl->methods()) {
            if (m->header()->isGeneric()) continue;
            if (m->header()->hasAnno("CompilerInner")) continue;
            visitFn(m);
        }
        if (impl->hasDestructor()) {
            visitFn(impl->destructor());
        }
        _currentStructName.clear();
    }
}

void SemaPass::visitFn(p<FnNode> fn) {
    if (!fn) return;
    // Phase 3.3 前置.4: 进入 fn 时记 _currentFn, 让 visitExpr 里的
    // checkErrPropagateForIdCall / checkBangWithoutFallibleCaller 能拿到
    // caller 的 #Fallible(E) 注解.
    auto savedFn = _currentFn;
    _currentFn = fn;
    for (auto& stmt : fn->body()) {
        visitStmt(stmt);
    }
    _currentFn = savedFn;
}

void SemaPass::visitBlock(p<StatementBlockNode> block) {
    if (!block) return;
    for (auto& s : block->statements()) {
        visitStmt(s);
    }
    if (block->hasResult()) {
        visitExpr(block->resultExpr());
    }
}

void SemaPass::visitStmt(p<StatementNode> stmt) {
    if (!stmt) return;
    if (auto loop = dynamic_cast<p<StatementLoopNode>>(stmt)) {
        visitBlock(loop->block());
        return;
    }
    if (auto set = dynamic_cast<p<StatementSetNode>>(stmt)) {
        visitExpr(set->arrayExpr());
        for (auto& idx : set->indices()) visitExpr(idx);
        visitExpr(set->valueExpr());
        return;
    }
    if (dynamic_cast<p<StatementBreakNode>>(stmt)) return;
    if (dynamic_cast<p<StatementRetVoidNode>>(stmt)) return;
    if (dynamic_cast<p<StatementDeclareNode>>(stmt)) return; // 无表达式
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
        if (se->expr()) visitExpr(se->expr());
        return;
    }
    // 兜底：未识别的 stmt 直接跳过, 不抛错 —— SemaPass 当前是 no-op, 漏处理
    // 不应阻塞 codegen; 3.2 起开始有实际写入后再改成 assert(false)。
}

void SemaPass::visitExpr(p<ExprNode> expr) {
    if (!expr) return;
    // Phase 3.2a：在每个 expr 节点上写 resolvedType, 与 compile<Foo>Expr 入口
    // 的同款 set 并存 (值相同, 后者随后变成 no-op)。getType() 是各子类的纯查询,
    // 不读 resolvedType, 此处先写后递归都安全; 选先写, 让下游若有早读路径也能命中。
    //
    // 已知问题: 部分 getType() 在错误形态下会抛 YuxError —— 例如算术节点遇到
    // lambda 形参未推断 (E3001), 或调用节点遇到泛型 arity 错配 (E3095)。
    //
    // Phase 3.2b 起开始按错误码白名单 (kMigratedCodes) 接管诊断: 命中清单的
    // 重新抛出, 由 SemaPass 实际报错; 其余仍吞掉, 留给 compile<Foo>Expr 的
    // 原有路径继续报。这样可以一码一码迁, 不必一次性把整个 getType 路径搬空。
    try {
        expr->setResolvedType(expr->getType());
    } catch (const YuxError& e) {
        if (isMigratedCode(e.getCode())) {
            throw;
        }
        // 未迁移码: 暂留给原 codegen 路径
    } catch (...) {
        // 非 YuxError (内部异常) 不该出现; 防御性吞掉以免影响 codegen
    }

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps()) visitExpr(e);
        }
        // Phase 3.4.f.2: int 字面量越界 (E3103) — getType 仅返回类型不解析值,
        // 这里主动调 sema::parseIntLiteral 触发越界 / 非法格式校验.
        if (auto intLit = dynamic_cast<p<LiteralIntNode>>(n->literal())) {
            (void)sema::parseIntLiteral(intLit->getValue().getText(),
                                        n->getLineNumber(), n->getColumn());
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr()); return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr());
        for (auto& a : n->getArgs()) visitExpr(a);

        // Phase 3.3 前置.4: ID-callee / 非-ID-callee 的错误传播校验
        // (E7001/E7004/E7006/E7016). 协议与 Compiler::compileCallExpr 顶部
        // 完全一致 —— ID-literal 走 checkErrPropagateForIdCall (按 fnName
        // 取第一候选, 10e 视多重载为同质); 非 ID-literal + ! + 不在 try 内
        // 走 checkBangWithoutFallibleCaller. _tryStack 顶端的 vector* 用作
        // tryBlockSeenErrs (callee 是 #Fallible 时把 errType append 进去,
        // 供 E7002 穷尽性使用; SemaPass 暂不读它, 由 Compiler 走 E7002).
        auto calleeExpr = n->getCalleeExpr();
        if (auto litCallee = dynamic_cast<p<ExprLiteralNode>>(calleeExpr)) {
            if (auto objLit = dynamic_cast<p<LiteralObjNode>>(litCallee->literal())) {
                string fnNameProp = objLit->getValue().getText();
                vector<FnSymbolInfo*> cands;
                _file->collectFnOverloads(fnNameProp, cands);
                if (_sdkFile && _sdkFile != _file) {
                    _sdkFile->collectFnOverloads(fnNameProp, cands);
                }
                const FnSymbolInfo* sym = cands.empty() ? nullptr : cands.front();
                vector<string>* seen = _tryStack.empty() ? nullptr : &_tryStack.back();
                sema::checkErrPropagateForIdCall(_currentFn, n, fnNameProp, sym, seen, _sourcePath);
            } else if (n->errPropagate()) {
                if (_tryStack.empty()) {
                    sema::checkBangWithoutFallibleCaller(_currentFn, n);
                }
            }
        } else if (n->errPropagate() && !dynamic_cast<p<ExprDotNode>>(calleeExpr)) {
            if (_tryStack.empty()) {
                sema::checkBangWithoutFallibleCaller(_currentFn, n);
            }
        }

        // Phase 3.3 前置.2：SemaPass 主动驱动重载解析 + 灵活整数推断.
        // 只接管"纯 ID callee + 无显式类型实参 + 非泛型"的情形, 与
        // compiler_call.cpp 中 `else if (structDecl)` / `else` 分支的进入条件保持一致;
        // 其余 (Fn 类型 callee / 泛型 fn 或 ctor / 方法调用) 仍由 codegen 自行处理.
        //
        // 副作用幂等性: tryInferIntType 仅在 isFlexibleIntExpr 为真时改写; SemaPass
        // 跑完后字面量已带类型, Compiler 端再次调用 resolve* 时 isFlexibleIntExpr 返回 false,
        // 不会重复推断 (见 call_resolve.h 的契约说明).
        if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                int line = n->getLineNumber();
                int col = n->getColumn();
                bool hasTypeArgs = !n->getTypeArgs().empty();

                // Phase 3.3.2.f: 自由 intrinsic arity 校验 (E6020/E6021/E6022).
                // helper 仅对清单内 fnName 实际校验, 其他 fnName 是 no-op,
                // 故无条件调用安全; 与 Compiler 端 compileExternalOrSdkFunctionCall
                // 顶部的 sema::validateFreeIntrinsicArity 互为防御性双跑.
                sema::validateFreeIntrinsicArity(fnName, n->getArgs().size(), line, col);

                auto* structDecl = _file->getStructDecl(fnName);
                if (!structDecl && _sdkFile) structDecl = _sdkFile->getStructDecl(fnName);

                // Phase 3.3.2.f: CompilerInner 泛型 intrinsic 的 shape + type-shape 校验.
                // 接管 E6017/E6018/E6024-E6029/E6032 实际抛出点 (与 Compiler::compileGenericFunctionCall
                // 的 #CompilerInner 分支镜像).
                // 限制:
                //   * 仅在 callee 是 ID-literal 且解析到泛型 fn 且 fn 头部 hasAnno(CompilerInner) 时接管;
                //   * typeArgs 仅在显式 (`f:<T>(...)`) 时由 SemaPass 取; 无显式 typeArgs (推断路径)
                //     需要 sema::inferGenericFnTypeArgs, 它会抛 E6012/E6013, 而这两码当前仍归 Compiler
                //     兜底 (3.3.1.b 未让 SemaPass 接管). 推断路径整体跳过, 留 Compiler 抛.
                //   * argTypes 经 getType() 计算, 任一 arg 未推断 (lambda 形参) 时跳过.
                if (!structDecl) {
                    auto* genFn = _file->getGenericFunction(fnName);
                    if (!genFn && _sdkFile) genFn = _sdkFile->getGenericFunction(fnName);
                    if (genFn && genFn->header()->hasAnno("CompilerInner") && hasTypeArgs) {
                        vector<TypeInfo> typeArgs;
                        bool typeArgsOk = true;
                        try {
                            for (auto& tn : n->getTypeArgs()) typeArgs.push_back(tn->getType());
                        } catch (...) { typeArgsOk = false; }

                        vector<TypeInfo> argTypes;
                        bool argTypesOk = true;
                        for (auto& a : n->getArgs()) {
                            try { argTypes.push_back(a->getType()); }
                            catch (...) { argTypesOk = false; break; }
                        }

                        if (typeArgsOk) {
                            sema::validateCompilerInnerIntrinsicShape(fnName, typeArgs.size(),
                                n->getArgs().size(), line, col);
                            if (argTypesOk) {
                                sema::validateCompilerInnerIntrinsicTypeShape(
                                    fnName, typeArgs, argTypes, n->getArgs(),
                                    _file, _sdkFile, line, col);
                            }
                        }
                    }
                }

                if (structDecl) {
                    // 形态校验对泛型 / 非泛型 ctor 都适用 (E6008 私有 / E6009 缺 typeArgs)
                    sema::validateCtorCallShape(structDecl, fnName, hasTypeArgs, line, col);
                    // 泛型 ctor + 显式 typeArgs 的 arity 校验 (E6010)
                    if (structDecl->isGeneric() && hasTypeArgs) {
                        sema::validateGenericTypeArgsArity(fnName,
                            structDecl->typeParams().size(),
                            n->getTypeArgs().size(), line, col);
                    }
                }

                // 泛型 fn + 显式 typeArgs 的 arity 校验 (E6010, 与 compileCallExpr 入口一致)
                if (!structDecl && hasTypeArgs) {
                    auto* genFn2 = _file->getFunction(fnName);
                    if (!genFn2 && _sdkFile) genFn2 = _sdkFile->getFunction(fnName);
                    if (genFn2 && genFn2->header()->isGeneric()) {
                        sema::validateGenericTypeArgsArity(fnName,
                            genFn2->header()->typeParams().size(),
                            n->getTypeArgs().size(), line, col);
                    }
                }

                // 仅在无显式 typeArgs + 非泛型路径上才驱动重载解析:
                // 泛型 fn/ctor 走 Compiler 的 substitute 推断, 灵活整数推断由
                // 那条路径自行完成; SemaPass 暂不接入泛型实例化.
                if (!hasTypeArgs) {
                    auto* genFn = _file->getFunction(fnName);
                    if (!genFn && _sdkFile) genFn = _sdkFile->getFunction(fnName);
                    if (!genFn || !genFn->header()->isGeneric()) {
                        if (structDecl && !structDecl->isGeneric()) {
                            sema::resolveCtorOverload(_file, fnName, n->getArgs(), line);
                            if (_sdkFile && _sdkFile != _file) {
                                sema::resolveCtorOverload(_sdkFile, fnName, n->getArgs(), line);
                            }
                        } else if (!structDecl) {
                            sema::resolveFnOverload(_file, _sdkFile, fnName, n->getArgs(), line);
                            // Phase 3.3.1.c: 非泛型 ID-callee 解析到 fnSymbol 后做可见性校验 (E6006).
                            // argTypes 经 getType() 计算; 若任一实参未推断 (lambda 形参等),
                            // 跳过并交给 Compiler 兜底.
                            vector<TypeInfo> argTypes;
                            bool ok = true;
                            for (auto& a : n->getArgs()) {
                                try { argTypes.push_back(a->getType()); }
                                catch (...) { ok = false; break; }
                            }
                            if (ok) {
                                auto* fnSym = _file->lookupFnSymbolWithParams(fnName, argTypes);
                                if (!fnSym && _sdkFile && _sdkFile != _file) {
                                    fnSym = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                                }
                                sema::validateFnSymbolVisibility(fnSym, _file->moduleName(),
                                                                  fnName, line, col);
                            }
                        }
                    }
                }
            }
        }

        // Phase 3.3.1.a: Dot-callee 包/模块别名调用 (E6001-E6005).
        // 与 compileMethodCall line 195-254 同款条件; argTypes 经 getType()
        // 计算, 任一参数未推断时跳过, 交给 Compiler 兜底.
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            vector<TypeInfo> argTypes;
            bool ok = true;
            for (auto& a : n->getArgs()) {
                try { argTypes.push_back(a->getType()); }
                catch (...) { ok = false; break; }
            }
            if (ok) {
                auto modCall = sema::resolveModuleFnCall(_file, nullptr, n, dotCallee, argTypes);
                if (!modCall.matched) {
                    // Phase 3.3.2.f: 镜像 Compiler::compileMethodCall 的 baseType 派发,
                    // 主动调用 3.3.2.a / 3.3.2.e 抠出的 helper.
                    //   * baseType.isArrayGeneric() → validateArrayMethodCall (E3055/E6040-E6044)
                    //   * isBuiltinType + isCompilerInnerMethodIn → validateOperatorMethodCall (E6045/E3070)
                    // baseType 经 getType() 计算; 任一异常 (lambda 形参等) → 跳过, 交 Compiler 兜底.
                    // SemaPass 走非泛型 fn / 非泛型 impl 路径, 不需要 applySubst (替换栈为空).
                    TypeInfo baseType;
                    bool baseOk = true;
                    try { baseType = dotCallee->baseExpr()->getType(); }
                    catch (...) { baseOk = false; }
                    if (baseOk) {
                        const string& member = dotCallee->member();
                        size_t argsCount = n->getArgs().size();
                        int dline = n->getLineNumber();
                        int dcol = n->getColumn();
                        if (baseType.isArrayGeneric()) {
                            bool baseIsLvalue = isLvalueArrayBase(dotCallee->baseExpr());
                            sema::validateArrayMethodCall(baseType, member, argsCount,
                                                          baseIsLvalue, dline, dcol);
                        } else if (isBuiltinType(baseType.name) &&
                                   isCompilerInnerMethodIn(_sdkFile, baseType.name, member)) {
                            sema::validateOperatorMethodCall(member, baseType, argsCount,
                                                             dline, dcol);
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        visitExpr(n->baseExpr());
        // Phase 3.4.d.2: 字段私有可见性 (E3042). safe `?.` 路径在 helper 内
        // 自跳过 (走 getType, kMigratedCodes 已覆盖). 异常静默吞掉, 留 Compiler.
        try {
            sema::validateDotFieldPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) {
            // 防御性
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitBlock(n->thenBlock());
        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            visitBlock(el->block());
        }
        if (n->elseBlock()) visitBlock(n->elseBlock());
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElsePreValueNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices()) visitExpr(i);
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // lambda 体内表达式的类型依赖调用点对形参的反推 / 上下文回填
        // (典型: `x => x + 1`, 在 `apply(it, 20)` 处才知道 `x : i32`)。
        // 3.2a 时这条路径靠 catch(...) 吞掉所有错误才没炸;
        // 3.2b 起 SemaPass 接管已迁移码 (E3001 等), 必须不再下钻 lambda 体,
        // 留给 codegen 在 compileCallExpr 回填形参类型后再走 compile<Foo>Expr
        // 入口的 setResolvedType 兜底写入。
        (void)n;
        return;
    }
    if (auto n = dynamic_cast<p<ExprStructLitNode>>(expr)) {
        // Phase 1b：AST 已构造，sema/codegen 尚未接管
        // 出现位限制（仅 #Static fn 体内）、字段全列、类型绑定均待 Phase 2
        throw YuxError(n->resolveLineNumber(), n->resolveColumn(),
                       ErrorCode::E0000,
                       "Self { ... } 结构体字面量未实现 (Phase 2)");
    }
    if (auto n = dynamic_cast<p<ExprEnumCtorNode>>(expr)) {
        for (auto& a : n->args()) visitExpr(a);
        // Phase 3.4.a: SemaPass 接管 E2019/E2020/E2021/E2032.
        // node->setResolvedType 已在 visitExpr 顶部写好 (getType 抛错时已在白名单
        // 重抛, 否则吞掉; 这里能跑到说明 getType 至少没抛已迁移码).
        // 任一异常被 helper 内部 try/catch (E2032 路径) 吞掉; E2019/E2020/E2021
        // 由 helper 主动抛出, SemaPass 实际接管.
        try {
            sema::validateEnumCtorShape(_file, _sdkFile, n);
        } catch (const YuxError&) {
            throw;
        } catch (...) {
            // 防御: helper 内部异常 (理论不应出现) 跳过, 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms()) visitExpr(arm->body());

        // Phase 3.4.b: SemaPass 接管 E2019/E2020/E2023/E2024/E2025/E2026/E2027.
        // 仅在 scrut 直接是 enum 名 (非 Rc/E / 非 alias 链) 时接入: 那两条路径
        // Compiler 端走 isFreshHandleExpr / resolveAlias (递归), SemaPass 暂未镜像,
        // 跳过留 Compiler 兜底. scrutType getType 抛错 (lambda 形参等) 时也跳过.
        try {
            TypeInfo scrutType = n->scrutinee()->getType();
            // Rc<E> 自动 deref 走 Compiler 兜底, 不在此处接入
            if (!scrutType.isRc()) {
                auto* enumDecl = lookupEnumIn(_file, _sdkFile, scrutType.name);
                if (enumDecl) {
                    sema::validateMatchArms(enumDecl, scrutType.name, n, _file);
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) {
            // getType 等内部异常: 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprTryCatchNode>>(expr)) {
        // Phase 3.3 前置.5: SemaPass 接管 E7011 (catch 类型必须是已声明 enum)
        // 与 E7002 (try block 内 callee 错误类型未被任一 catch 覆盖).
        //
        // 顺序:
        //   1) 逐 arm 校验 errType 为已声明 enum (E7011), 同时收集 catchTypes;
        //   2) push 新的 seenErrTypes 层, visitBlock(tryBlock) —— 内部 ID-callee
        //      检查把 #Fallible callee 的错误类型 append 进栈顶;
        //   3) pop 取出 seenErrTypes, 与 catchTypes 比对穷尽性 (E7002);
        //   4) 再访问每个 catch arm body (catches 在外层 try 视野之外).
        //
        // Compiler 端 compileTryCatchExpr 中相同形态的 E7011 / E7002 throw 保留
        // 作幂等防御性双跑: SemaPass 已先抛出, Compiler 不会再到达。
        vector<string> catchTypes;
        catchTypes.reserve(n->catches().size());
        int line = n->getLineNumber();
        int col = n->getColumn();
        for (auto& arm : n->catches()) {
            const string& errType = arm->errType();
            auto* enumDecl = lookupEnumIn(_file, _sdkFile, errType);
            if (!enumDecl) {
                int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                throw YuxError(aline, acol, ErrorCode::E7011,
                    arm->errName().getText(), errType, errType);
            }
            catchTypes.push_back(errType);
        }

        _tryStack.emplace_back();
        visitBlock(n->tryBlock());
        vector<string> seenErrTypes = std::move(_tryStack.back());
        _tryStack.pop_back();

        for (auto& seen : seenErrTypes) {
            bool covered = false;
            for (auto& ct : catchTypes) {
                if (ct == seen) { covered = true; break; }
            }
            if (!covered) {
                throw YuxError(line, col, ErrorCode::E7002,
                    seen, string("<unknown>"), seen);
            }
        }

        for (auto& c : n->catches()) visitBlock(c->body());
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg()); return;
    }
    if (auto n = dynamic_cast<p<ExprHeapCtorNode>>(expr)) {
        // Phase 2.6: Heap:<T>(x) 形态检查 (DRAFT-heap-types §8.3a)
        // - 递归 arg
        // - E3028: arg 类型必须与 turbofish 内层 T 等价；Compiler 端同 throw 留作幂等防御性双跑
        // - E4025: Rc/Weak/Array<Heap<...>> 在 getLLVMType 容器分支拦截，不在此处
        visitExpr(n->arg());
        auto resultType = n->getType();
        auto innerSp = resultType.heapElementType();
        if (innerSp) {
            const auto& innerT = *innerSp;
            auto argType = n->arg()->getType();
            if (!(argType == innerT)) {
                throw YuxError(n->getLineNumber(), n->getColumn(),
                    ErrorCode::E3028, innerT.name, innerT.name, argType.name);
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    // Phase 3.4.d.1: ExprGetRefNode —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprGetRefNode::getType 抛
    // E3040/E3041 (kMigratedCodes 命中, 自动重抛), 由此 Compiler 端
    // compileGetRefExpr 的 1656/1661 内联 throw 在正常 codepath 下不可达。
    // Phase 3.4.d.2: 补 E3042 链式私有字段可见性校验.
    if (auto n = dynamic_cast<p<ExprGetRefNode>>(expr)) {
        try {
            sema::validateGetRefPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) {
            // 防御性
        }
        return;
    }
    // Phase 3.4.f.1: ExprArrayInitNode 显式化 —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprArrayInitNode::getType 抛 E3009
    // (explicitType vs value 字面量类型不匹配, kMigratedCodes 命中, 自动重抛).
    // Compiler 端 compileArrayInitExpr 1131-133 内联 throw 在 sema 跑过的正常
    // codepath 下不可达, 保留作幂等防御性双跑.
    if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
        (void)n;
        return;
    }
    // 其余未识别节点 3.2 起补 assert。
}

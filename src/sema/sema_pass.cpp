// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 实现 —— 详见 sema_pass.h
//
// Phase 3.2a：visitExpr 在每个表达式节点上写入 `setResolvedType(getType())`,
// 覆盖范围扩到 file 顶层 fn body + struct impl 的方法/析构 body。
// 泛型模板 / #Builtin 仍跳过 —— 它们的 codegen 路径会自行写 resolvedType,
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

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

#include "../types.h"
#include "analyzer/borrow_checker.h"
#include "analyzer/const_mut_checker.h"
#include "analyzer/flow_terminate_checker.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include "tools/diagnostic.h"

namespace {
// Phase 3.2b 已由 SemaPass 接管的错误码白名单。SemaPass 在 visitExpr 中
// 捕获 YuxError 时, 命中此清单的直接 rethrow, 让 SemaPass 成为该诊断的
// 实际抛出点。新增迁移码追加到此处即可。
constexpr std::array<std::string_view, 21> kMigratedCodes = {
    // 算术 / 比较 / 分支结果（E3001-E3004 → E3001, E3005-E3008 → E3005）
    "E3001",
    "E3005",
    // 数组 / 字段 / 元组 / 引用
    // E3009 已移除 kMigratedCodes: 合并 E3011 后 SemaPass 对嵌套数组字面量
    // 产生假阳性（缺少 target-type 上下文），交回 Compiler 端兜底。
    "E3024", // 原 E3025 (Nullable 操作符左侧类型要求)
    "E3040",
    "E3041",
    "E3043",
    "E3044",
    "E3050", // 原 E3050-E3057 合并
    "E3062",
    "E3097",
    "E3100",
    // Phase 3.4.f.2: 字面量越界
    "E3103",
    // Phase 3.4.h: ExprUnaryNode 内置 op 形态校验 (Rev on float / Not on non-bool)
    "E3070",
    "E3071",
    // Phase 6A: 砍同名 ctor 定义形态
    "E3130",
    // Bucket 2: LiteralObjNode::getType 抛 undefined symbol (原 E3032 → E3030)
    "E3030",
    // Phase B-1: move intrinsic 类型形态校验（sema validateBuiltinIntrinsicTypeShape）
    "E4034",
    "E4035",
    // Phase B-1: #NoCopy 隐式复制 / 传播 / use-after-move（已由 SemaPass 接管）
    "E4031",
    "E4032",
    "E4033",
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

// Bucket 4 (CURRENT-check.md): 与 lookupEnumIn 同款的 struct decl 三段查找.
// FileNode::getStructDecl 默认过滤 #Builtin (Rc/Ref/Ptr/Array...) ——
// SemaPass 走 E6011 arity 校验等需要看到这些占位, 这里统一传 true。
// wildcardImports 已在 getStructDecl 内部覆盖, 只需再补 sdkFile 一档。
StructDeclNode* lookupStructIn(p<FileNode> file, p<FileNode> sdkFile, const string& name) {
    if (!file) return nullptr;
    if (auto* d = file->getStructDecl(name, /*includeBuiltin=*/true)) return d;
    if (sdkFile && sdkFile != file) {
        if (auto* d = sdkFile->getStructDecl(name, /*includeBuiltin=*/true)) return d;
    }
    return nullptr;
}

// Phase 3.3.2.f: 与 Compiler::isBuiltinMethod 等价的本地版本.
// 仅查 sdkFile 的 struct impl (内建运算符方法都注册在 SDK 上), 不存在
// 时返回 false. Sema 不依赖 Compiler 成员, 这里复制规则.
bool isBuiltinMethodIn(FileNode* sdkFile, const string& structName, const string& methodName) {
    if (!sdkFile) return false;
    auto structImpl = sdkFile->getStructImpl(structName);
    if (!structImpl) return false;
    for (auto& m : structImpl->methods()) {
        if (m->header()->name().getText() == methodName) {
            return m->header()->hasAnno("Builtin");
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

// E4025 (DRAFT-heap-types §8.3a.5.1): Rc/Weak/Array 容器禁止内嵌 Heap.
// 递归扫描 TypeInfo: 若任一 Rc/Weak/Array 直接 elem 是 Heap, 抛 E4025;
// 否则继续下钻 (覆盖 `Rc<Rc<Heap<T>>>` / `Array<Rc<Heap<T>>>` 等).
void validateNoNestedHeap(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Rc"), inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Weak"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Array"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) validateNoNestedHeap(*e, line, col);
        return;
    }
    // 其余形态 (struct / tuple / nullable / ref / ptr / dyn ...) 递归 genericArgs.
    for (const auto& g : t.genericArgs) {
        if (g) validateNoNestedHeap(*g, line, col);
    }
}

// Phase B-1: 与 Compiler::isNoCopyType 等价的本地版本（0 LLVM 依赖）。
// 判定类型是否为 #NoCopy：Array<T> 隐含，或 struct decl 显式标注 #NoCopy。
bool isNoCopyTypeIn(const TypeInfo& type, p<FileNode> file, p<FileNode> sdkFile) {
    if (isBuiltinType(type.name)) return false;
    if (type.isRc() || type.isWeak() || type.isHeap()) return false;
    if (type.isRef() || type.isPtr()) return false;
    if (type.isArrayGeneric()) return true; // Array<T> 隐含 #NoCopy

    auto* decl = lookupStructIn(file, sdkFile, type.name);
    if (decl && decl->hasAnno("NoCopy")) return true;
    return false;
}

// Phase B-1: 与 Compiler::isFreshHandleExpr（compiler_destructor.cpp）等价的本地版本（0 LLVM 依赖）。
// fresh 表达式自带 +1 所有权，隐式复制路径可安全跳过 retain。
// !! 两处须保持同步 — 新增 case 需两边同时添加 !!
bool isFreshHandleExpr(p<ExprNode> expr) {
    if (!expr) return false;
    if (dynamic_cast<p<ExprCallNode>>(expr)) return true;      // 函数调用结果 / builtin intrinsic
    if (dynamic_cast<p<ExprArrayNode>>(expr)) return true;     // 数组字面量
    if (dynamic_cast<p<ExprPathCallNode>>(expr)) return true;  // 枚举构造器 / #Static fn 调用
    if (dynamic_cast<p<ExprMoveAssignNode>>(expr)) return true;// move-assign 结果
    if (dynamic_cast<p<LambdaExprNode>>(expr)) return true;    // lambda 字面量
    if (dynamic_cast<p<ExprStructLitNode>>(expr)) return true; // struct 字面量 (Self { ... })
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
} // namespace

SemaPass::SemaPass(p<FileNode> file, Yux* yux)
    : _file(file), _yux(yux), _sdkFile(yux ? yux->sdkFile() : nullptr),
      _sourcePath((yux && file) ? yux->modulePath(file->moduleName()) : "") {}

void SemaPass::run() {
    if (!_file) return;
    // Bucket 3: 顶层类型别名一次性校验 (E2017 名字冲突 + E2016 环).
    // 必须在遍历 fn 之前: 一旦命中, 直接抛错.
    sema::validateAliases(_file);
    for (auto& fn : _file->getFunctions()) {
        // 泛型模板 / #Builtin 不走常规 codegen, 在 Compiler::compile 里也
        // 是被跳过的; SemaPass 这里同步跳过, 保持与 codegen 覆盖一致。
        if (fn->header()->isGeneric()) continue;
        if (fn->header()->hasAnno("Builtin")) continue;
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
            // 注：m->header()->isGeneric() 不再单独跳过——line 162 已跳过整个 generic impl,
            // 单方法泛型形态目前不支持 (Phase 6D-tail 清理)。
            if (m->header()->hasAnno("Builtin")) continue;
            // Phase 6A: 砍同名 ctor —— `fn TypeName(...)` 定义形态废除,
            // 构造唯一通道收敛到 `#Static fn`. `#Static fn TypeName(...)` 形态
            // 仍合法 (虽不推荐, 与 `#Static fn make()` 等并行).
            const auto& mname = m->header()->name();
            if (mname.getText() == _currentStructName && !m->header()->isStatic()) {
                throw YuxError(mname.getLine(), static_cast<int>(mname.getCharPositionInLine()), ErrorCode::E3130,
                               _currentStructName, _currentStructName, _currentStructName);
            }
            visitFn(m);
        }
        if (impl->hasDestructor()) {
            visitFn(impl->destructor());
        }
        _currentStructName.clear();
    }

    // DRAFT-spec-default-body Phase 2: spec 默认体占位符号校验
    // (sema 期不下钻完整 typecheck; 仅识别 `$.method(...)` 形态)
    visitSpecDefaults();

    // Phase B-1: #NoCopy 字段传播 (E4032) — 含显式 #NoCopy 字段的 struct
    // 自身也必须标注 #NoCopy（与 Compiler::inferNoCopyAnnotations 镜像）。
    {
        // 收集所有可见 struct decl（本地 + SDK + wildcard imports）
        vector<StructDeclNode*> allDecls = _file->getStructDecls();
        if (_sdkFile && _sdkFile != _file) {
            for (auto* d : _sdkFile->getStructDecls()) {
                if (std::ranges::find(allDecls, d) == allDecls.end()) {
                    allDecls.push_back(d);
                }
            }
            for (auto* imp : _sdkFile->wildcardImports()) {
                for (auto* d : imp->getStructDecls()) {
                    if (std::ranges::find(allDecls, d) == allDecls.end()) {
                        allDecls.push_back(d);
                    }
                }
            }
        }

        for (auto* decl : allDecls) {
            if (decl->isGeneric()) continue;       // 泛型 struct 实例化后才知字段类型
            if (decl->hasAnno("NoCopy")) continue; // 已标注，跳过
            for (auto* field : decl->fields()) {
                auto ft = field->getType();
                if (ft.isRc() || ft.isArrayGeneric() || ft.isWeak() || ft.isHeap()) continue;
                if (ft.isRef() || ft.isPtr()) continue;
                if (isBuiltinType(ft.name)) continue;

                auto* fieldDecl = lookupStructIn(_file, _sdkFile, ft.name);
                // TODO: 泛型 NoCopy 类型（如 MyNoCopyStruct<i32>）的 ft.name 是修饰名，
                // lookupStructIn 按基名匹配不到，E4032 静默跳过。
                // 修法：剥泛型参数后查找，或用 decl 指针替代 name 查找。
                if (fieldDecl && fieldDecl->hasAnno("NoCopy")) {
                    throw YuxError(decl->getLineNumber(), decl->getColumn(), ErrorCode::E4032,
                                   decl->name().getText(), field->name().getText());
                }
            }
        }
    }
}

namespace {
// 判定 e 是不是字面量 `$`（spec 默认体里 self 句柄, ast_builder 构成
// ExprLiteralNode(LiteralObjNode("$")))。
bool isBareSelf(const p<ExprNode>& e) {
    auto lit = dynamic_cast<p<ExprLiteralNode>>(e);
    if (!lit) return false;
    auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal());
    return obj && obj->getValue().getText() == "$";
}

// DRAFT-spec-default-body Phase 2: 递归扫描 expr 树寻找 `$.method(args)`
// 形态调用; 命中则验证 method 是否在 spec 自身签名集内, 不在则抛 E1140。
// 仅覆盖常见表达式形态; lambda / try-catch / match 等复杂形态在 Phase 2
// 主动 skip (留待 Phase 3 单态化时机的完整 typecheck)。
void walkExprForSpecDefault(const p<ExprNode>& e, SpecDeclNode* spec) {
    if (!e) return;
    if (auto n = dynamic_cast<p<ExprCallNode>>(e)) {
        if (auto dot = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            if (isBareSelf(dot->baseExpr())) {
                const std::string m = dot->member();
                bool found = false;
                for (auto& sig : spec->signatures()) {
                    if (sig->name().getText() == m) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw YuxError(dot->resolveLineNumber(), dot->resolveColumn(), ErrorCode::E1140,
                                   spec->name().getText(), m,
                                   " (referenced from default body — must appear in this spec's signatures)");
                }
            }
        }
        walkExprForSpecDefault(n->getCalleeExpr(), spec);
        for (auto& a : n->getArgs())
            walkExprForSpecDefault(a, spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(e)) {
        walkExprForSpecDefault(n->baseExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(e)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(e)) {
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    // 其它形态 (lambda / try-catch / match / struct lit / array / 索引 / 元组 ...)
    // Phase 2 不下钻; Phase 3 克隆 + 真实 typecheck 会兜底。
}

// 递归扫描 stmt 中的所有表达式入口。
void walkStmtForSpecDefault(const p<StatementNode>& s, SpecDeclNode* spec) {
    if (!s) return;
    if (auto n = dynamic_cast<p<StatementRetNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementAssignNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementSetNode>>(s)) {
        walkExprForSpecDefault(n->arrayExpr(), spec);
        for (auto& idx : n->indices())
            walkExprForSpecDefault(idx, spec);
        walkExprForSpecDefault(n->valueExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementDeclareAssignNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementExprNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementLoopNode>>(s)) {
        if (n->hasInit()) {
            walkExprForSpecDefault(n->initExpr(), spec);
        }
        return;
    }
    // Block / Declare(无 init) / RetVoid / Break 等 Phase 2 不处理
}
} // namespace

void SemaPass::visitSpecDefaults() {
    if (!_file) return;
    for (auto& spec : _file->getSpecDecls()) {
        if (!spec) continue;
        const auto& bodies = spec->defaultBodies();
        for (auto& body : bodies) {
            if (!body) continue;
            for (auto& stmt : body->body()) {
                walkStmtForSpecDefault(stmt, spec);
            }
        }
    }
}

void SemaPass::visitFn(p<FnNode> fn) {
    if (!fn) return;
    // Phase 3.3 前置.4: 进入 fn 时记 _currentFn, 让 visitExpr 里的
    // checkErrPropagateForIdCall / checkBangWithoutFallibleCaller 能拿到
    // caller 的 #Fallible(E) 注解.
    auto savedFn = _currentFn;
    _currentFn = fn;
    _movedVars.clear(); // Phase B-1: 进入 fn 时清空 move 追踪

    // Bucket 1 (CURRENT-check.md): 把 0-LLVM analyzer 接入 sema, 让 yux-check
    // 也能覆盖 borrow / const-mut / NoReturn 流终止 检查.
    checkBorrows(fn, _currentStructName);
    checkConstMut(fn);
    checkFlowTerminate(fn);

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
        const auto& label = loop->label();
        // 检测重复 label：同名 label 不可在外层 loop 栈中出现
        if (!label.getText().empty()) {
            for (const auto& existing : _loopLabelStack) {
                if (existing.getText() == label.getText()) {
                    throw YuxError(loop->getLineNumber(), loop->getColumn(), ErrorCode::E3022, label.getText());
                }
            }
        }
        _loopLabelStack.push_back(label); // 空 Token = 无 label
        if (loop->hasInit()) {
            visitExpr(loop->initExpr());
        }
        visitBlock(loop->block());
        _loopLabelStack.pop_back();
        return;
    }
    if (auto set = dynamic_cast<p<StatementSetNode>>(stmt)) {
        visitExpr(set->arrayExpr());
        for (auto& idx : set->indices())
            visitExpr(idx);
        visitExpr(set->valueExpr());
        // v0.16 闭包捕获: lambda body 内对捕获变量赋值 → E2030。
        // StatementSetNode 覆盖简单变量 `a = 20` / 复合赋值 `a += 1` / 索引赋值 `a[i] = x`。
        // LHS arrayExpr 抽取变量名后按 StatementAssignNode 同款规则判定。
        if (_currentLambda && _currentFn && set->indices().empty()) {
            auto lhsLit = dynamic_cast<p<ExprLiteralNode>>(set->arrayExpr());
            if (lhsLit) {
                auto lhsObj = dynamic_cast<p<LiteralObjNode>>(lhsLit->literal());
                if (lhsObj) {
                    string objName = lhsObj->getValue().getText();
                    if (objName != "$") {
                        bool isParam = false;
                        for (auto& p : _currentLambda->params()) {
                            if (p.name.getText() == objName) {
                                isParam = true;
                                break;
                            }
                        }
                        if (!isParam && _currentFn->lookupSymbol(objName)) {
                            throw YuxError(set->getLineNumber(), set->getColumn(), ErrorCode::E2030, objName);
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto br = dynamic_cast<p<StatementBreakNode>>(stmt)) {
        const auto& brLabel = br->label();
        if (brLabel.getText().empty()) {
            // 无 label 的 break：检查是否有外层 loop
            if (_loopLabelStack.empty()) {
                throw YuxError(br->getLineNumber(), br->getColumn(), ErrorCode::E3094);
            }
        } else {
            // break@label：从内向外搜索匹配 label
            bool found = false;
            for (auto it = _loopLabelStack.rbegin(); it != _loopLabelStack.rend(); ++it) {
                if (it->getText() == brLabel.getText()) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw YuxError(br->getLineNumber(), br->getColumn(), ErrorCode::E3025, brLabel.getText());
            }
        }
        return;
    }
    if (dynamic_cast<p<StatementRetVoidNode>>(stmt)) return;
    if (auto d = dynamic_cast<p<StatementDeclareNode>>(stmt)) {
        // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct arity).
        // 无 init 形态 (`let p Pair<i32>`), 仅 varType, 同款检查.
        if (d->varType()) {
            try {
                auto vt = d->varType()->getType();
                if (!vt.name.empty() && !isBuiltinType(vt.name) && !vt.isRef() && !vt.isFn() && !vt.isTuple()) {
                    if (auto* sd = lookupStructIn(_file, _sdkFile, vt.name)) {
                        size_t want = sd->typeParams().size();
                        size_t got = vt.genericArgs.size();
                        if (want > 0 && want != got) {
                            throw YuxError(d->getLineNumber(), d->getColumn(), ErrorCode::E6011, vt.name, want, got)
                                .withHint(std::format(
                                    "实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", vt.name,
                                    std::string(want == 1 ? "T" : "T1, T2, ..."), want));
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // 留 Compiler 兜底
            }
        }
        return;
    }
    if (auto as = dynamic_cast<p<StatementAssignNode>>(stmt)) {
        // Phase 2e: `$.field = ...` 在 `#Static fn` 体内禁用 (E3128).
        // StatementAssign 的 `obj` (LHS 根) 不会被 visitExpr 递归, 这里单独拦截.
        if (as->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
            throw YuxError(as->obj().getLine(), static_cast<int>(as->obj().getCharPositionInLine()), ErrorCode::E3128);
        }
        // Bucket 2 收口 (CURRENT-check.md): 简单变量赋值 (subs 为空) 的写可见性校验
        // (E3093). 与 compiler_stmt.cpp:952 同款条件: !writeable && !type.isRef().
        // T& 形参 / val 局部 T& 的 writeable=false 不影响"写被引", 由 borrow 检查
        // 在 4d 校验.
        if (as->subs().empty() && _currentFn) {
            string objName = as->obj().getText();
            if (objName != "$") {
                if (auto sym = _currentFn->lookupSymbol(objName)) {
                    if (!sym->writeable && !sym->type.isRef()) {
                        throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3093, objName);
                    }
                }
            }
        }
        // v0.16 闭包捕获: lambda body 内对捕获变量赋值 / 成员链写 → E2030.
        // 覆盖 `=` / `+= -= *= /= %= ^=` / `<<= >>=` 及 `obj.f = ...` / `obj[i] = ...`
        // (obj 为捕获变量)。
        // 判定: objName 不在 lambda 自身的形参列表 → 外层变量 → 捕获 → 禁写.
        // 不能用 bodyScope->lookupSymbol(), 因其沿父链查找到外层 fn 作用域.
        if (_currentLambda && _currentFn) {
            string objName = as->obj().getText();
            if (objName != "$") {
                bool isParam = false;
                for (auto& p : _currentLambda->params()) {
                    if (p.name.getText() == objName) {
                        isParam = true;
                        break;
                    }
                }
                if (!isParam && _currentFn->lookupSymbol(objName)) {
                    throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E2030, objName);
                }
            }
        }
        // Bucket 5 起步: 成员链赋值的两条简单形态诊断 (与 compiler_stmt.cpp 1188-1207
        // tuple 越界 / 1361 中段拒收 镜像).
        //   * E3100 元组下标越界: actualType.isTuple() + memberText 为纯数字 + idx 越界
        //   * E3046 中段非纯 struct:  walk 到非末段, interType 命中
        //                          Rc/Array/Ref/Nullable/Weak/Ptr/builtin
        // 跳过策略 (留 Compiler 兜底):
        //   * objName == "$" (sema 不跟踪 $)
        //   * lookupSymbol 失败 (E3031 Compiler 抢先)
        //   * 起点 / 中段是泛型 struct (Compiler applySubst, sema 不替换泛型实参)
        //   * 中段 typeNeedsDestructor (递归 RC 字段扫描, 复杂, 留 Compiler)
        //   * 非纯数字下标命中 tuple 形态.
        if (!as->subs().empty() && _currentFn) {
            string objName = as->obj().getText();
            if (objName != "$") {
                if (auto sym = _currentFn->lookupSymbol(objName)) {
                    TypeInfo curType = sym->type;
                    if (curType.isRef()) {
                        if (auto inner = curType.refElementType()) curType = *inner;
                    }
                    if (curType.isRc()) {
                        if (auto inner = curType.rcElementType()) curType = *inner;
                    }
                    const auto& subs = as->subs();
                    auto isPureDigits = [](const string& s) {
                        return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
                    };
                    if (curType.isTuple()) {
                        // tuple 链: 仅 OOB (E3100), 中段非 tuple / 非纯数字 留 Compiler
                        bool stop = false;
                        for (size_t i = 0; i < subs.size() && !stop; ++i) {
                            string memberText = subs[i].getText();
                            if (!isPureDigits(memberText)) {
                                stop = true;
                                break;
                            }
                            if (!curType.isTuple()) {
                                stop = true;
                                break;
                            }
                            const auto& elems = curType.tupleElements();
                            auto idx = static_cast<size_t>(std::stoul(memberText));
                            if (idx >= elems.size()) {
                                throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3100, memberText,
                                               curType.getFullName(), std::to_string(elems.size()));
                            }
                            if (i + 1 < subs.size()) curType = *elems[idx];
                        }
                    } else if (!curType.name.empty() && !isBuiltinType(curType.name)) {
                        // struct 链: 中段 E3046 (Rc/Array/Ref/Nullable/Weak/Ptr/builtin)
                        StructDeclNode* decl = _file ? _file->getStructDecl(curType.name) : nullptr;
                        if (!decl && _sdkFile) decl = _sdkFile->getStructDecl(curType.name);
                        // 泛型 struct 留 Compiler (applySubst)
                        if (decl && !decl->isGeneric()) {
                            for (size_t i = 0; i + 1 < subs.size(); ++i) {
                                string memberText = subs[i].getText();
                                int fi = decl->fieldIndex(memberText);
                                if (fi < 0) break; // E3040 Compiler 抢先
                                auto interType = decl->fields()[fi]->getType();
                                if (interType.isRc() || interType.isArrayGeneric() || interType.isRef() ||
                                    interType.isNullable() || interType.isWeak() || interType.isPtr() ||
                                    isBuiltinType(interType.name)) {
                                    throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3046)
                                        .withHint("嵌套成员赋值中间字段需为纯 struct（不含 Rc/Array/Ref/RC "
                                                  "等）；可拆方法或在中段先 `var t = $.field` 落地后再写");
                                }
                                StructDeclNode* nextDecl = _file ? _file->getStructDecl(interType.name) : nullptr;
                                if (!nextDecl && _sdkFile) nextDecl = _sdkFile->getStructDecl(interType.name);
                                if (!nextDecl || nextDecl->isGeneric()) break;
                                decl = nextDecl;
                            }
                        }
                    }
                }
            }
        }
        if (as->expr()) visitExpr(as->expr());
        return;
    }
    if (auto tup = dynamic_cast<p<StatementDeclareAssignTupleNode>>(stmt)) {
        // Bucket 2: 元组解构 LHS 数量 vs RHS 元组实际元素数 (E3102).
        // 简化策略 —— 仅在 RHS 直接是 ExprTupleNode 字面量时校验, 因为此时元素
        // 数从 AST 直接可得, 无需走 applySubst. 类型标注路径 (varType) 留 Compiler.
        if (tup->expr()) {
            if (auto tn = dynamic_cast<p<ExprTupleNode>>(tup->expr())) {
                if (tn->elements().size() != tup->names().size()) {
                    throw YuxError(tup->getLineNumber(), tup->getColumn(), ErrorCode::E3102,
                                   std::to_string(tup->names().size()), std::to_string(tn->elements().size()));
                }
            }
            visitExpr(tup->expr());
        }
        return;
    }
    if (auto ret = dynamic_cast<p<StatementRetNode>>(stmt)) {
        // Bucket 2 收口 (CURRENT-check.md): E3020 / E3022 return 类型校验.
        // 只覆盖"简单形态" —— 跳过以下复杂路径, 交 Compiler 兜底:
        //   * declRetType.isRef()       —— T& 返回, 走 borrow 溯源 + getRef compile
        //   * Fallible(E) 注解          —— 成功 / 错误双通道, 复用 E3020 但多分支
        //   * declRetType.isNullable()  —— null 字面量 / T 值自动 wrap
        //   * isFlexibleIntExpr(expr)   —— 灵活整数推断后再比, sema 不改写 expr 类型
        //   * declRetType.name == "Self" —— 方法上下文 Self 解析需 currentStructName 替换
        // 普通 case: `fn add() i32 { ret true }` (E3020) /
        //           `fn foo() { ret 42 }` (E3022).
        // v0.16: lambda body 内 ret 的返回类型校验依赖 lambda 自身的 retType,
        // 但 lambda 形参 / retType 可能在调用点才反推; sema 阶段 _currentFn 仍是
        // 外层 fn, E3020/E3022 以 _currentFn 的 retType 为准会误报。整个 check
        // skip, 留 codegen 在 emitLambdaFunction 内兜底。
        if (_currentLambda) {
            if (ret->expr()) visitExpr(ret->expr());
            return;
        }
        if (_currentFn && ret->expr()) {
            auto header = _currentFn->header();
            bool hasFallible = header && header->getAnnoArg("Fallible").has_value();
            bool hasDeclRet = header && header->retType();
            TypeInfo declRetType;
            if (hasDeclRet) declRetType = header->retType()->getType();
            // 灵活整数推断仅在有 declRetType 时影响匹配 (Compiler 会先 tryInferIntType
            // 改写 expr 类型再比); 无 decl 时 (E3022 路径) 不构成 skip 理由.
            // alias 形态 (`IPair = (i32, i32)` 等) 名称直比会假阳性 (`IPair` vs `(i32,i32)`),
            // sema 暂未做 resolveAlias 递归比对, 任一侧名称命中 alias 即 skip 留 Compiler 兜底.
            auto isAliased = [&](const string& n) -> bool {
                if (!_file) return false;
                return _file->getAliasDecl(n) != nullptr;
            };
            bool skip = hasFallible || (hasDeclRet && (declRetType.isRef() || declRetType.isNullable())) ||
                        (hasDeclRet && declRetType.name == "Self") || (hasDeclRet && isFlexibleIntExpr(ret->expr())) ||
                        (hasDeclRet && isAliased(declRetType.name));
            if (!skip) {
                TypeInfo retType;
                bool gotType = true;
                try {
                    retType = ret->expr()->getType();
                } catch (...) {
                    gotType = false;
                }
                if (gotType && !(hasDeclRet && isAliased(retType.name))) {
                    int line = ret->getLineNumber();
                    if (line < 0) line = ret->expr()->resolveLineNumber();
                    if (hasDeclRet) {
                        if (retType.empty()) {
                            throw YuxError(line, ErrorCode::E3014, declRetType.getFullName(), "void");
                        }
                        // 名称直比 —— 不做 resolveAlias (sema 暂无该 helper);
                        // alias 形态 / Self 已在 skip 排除, 这里假阴性可接受 (Compiler 兜底).
                        if (retType.getFullName() != declRetType.getFullName()) {
                            throw YuxError(line, ErrorCode::E3014, declRetType.getFullName(), retType.getFullName());
                        }
                    } else {
                        if (!retType.empty()) {
                            throw YuxError(line, ErrorCode::E3014, "void", retType.getFullName());
                        }
                    }
                }
            }
        }
        if (ret->expr()) visitExpr(ret->expr());
        // v0.16 闭包捕获: lambda 字面量直接作 ret expr 且含 T& 捕获 → E4022
        // (spec §8.7.6.5 不可逃逸)。仅拦截直接形 (lambda 字面量), 穿透检测
        // (ret 变量名 / 调用结果含 lambda) 留 codegen 兜底。
        if (ret->expr()) {
            if (auto litLambda = dynamic_cast<p<LambdaExprNode>>(ret->expr())) {
                if (litLambda->hasRefCapture()) {
                    throw YuxError(litLambda->getLineNumber(), litLambda->getColumn(), ErrorCode::E4022);
                }
            }
        }
        return;
    }
    if (auto da = dynamic_cast<p<StatementDeclareAssignNode>>(stmt)) {
        // Bucket 6 (CURRENT-check.md): T& 局部声明初始化形态校验 (E3018).
        // 只接管"低风险"分支: expr 是 ID-literal (LiteralObjNode) 且 varType 为 ref —
        //   srcName 必须查到符号, 且符号本身是 T&, refElementType 与声明 inner 一致.
        //   不满足 → 抛 E3018 (与 compiler_stmt.cpp:484-499 同款 hint).
        // 其它形态 (ExprGetRef E3017 / ExprCall as_ref E3019 / 复杂 expr) 留 Compiler 兜底.
        // lambda 体 sema 不下钻 — 这里检查 _currentFn 非空再做.
        // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct arity 不匹配).
        // 不依赖 expr / _currentFn, 仅 varType 形态. varType.name 命中已知 struct decl,
        // decl.isGeneric() 且 typeParams.size() != genericArgs.size() → 抛 E6011.
        // 镜像 compiler_types.cpp:241 与 :597 两条路径. Builtin / Ref / Fn / Tuple 跳过.
        if (da->varType()) {
            try {
                auto vt = da->varType()->getType();
                if (!vt.name.empty() && !isBuiltinType(vt.name) && !vt.isRef() && !vt.isFn() && !vt.isTuple()) {
                    if (auto* sd = lookupStructIn(_file, _sdkFile, vt.name)) {
                        size_t want = sd->typeParams().size();
                        size_t got = vt.genericArgs.size();
                        if (want > 0 && want != got) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E6011, vt.name, want, got)
                                .withHint(std::format(
                                    "实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", vt.name,
                                    std::string(want == 1 ? "T" : "T1, T2, ..."), want));
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // 留 Compiler 兜底
            }
        }
        if (da->varType() && _currentFn && da->expr()) {
            auto varType = da->varType()->getType();
            // Bucket 6 收口+ (CURRENT-check.md): 目标类型驱动的形态校验.
            // E3012 (fixed-array 大小不匹配) / E3015 (Nullable 内部类型不匹配).
            // 镜像 compiler_stmt.cpp:773 / 740. 复杂路径 (alias / 嵌套数组目标类型)
            // 留 Compiler 兜底. lambda 体 sema 不下钻.
            try {
                if (varType.isArray()) {
                    auto exprType = da->expr()->getType();
                    // exprType.arraySize == 0 → ExprArrayInit fill 形态 (`[v ...]`),
                    // 实际大小靠 target-type 推断, 跳过比较留 Compiler 兜底.
                    if (exprType.isArray() && exprType.arraySize > 0 && varType.arraySize != exprType.arraySize) {
                        throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3012, varType.arraySize,
                                       exprType.arraySize);
                    }
                } else if (varType.isNullable()) {
                    auto innerType = varType.nullableInnerType();
                    if (innerType) {
                        // null 字面量直通
                        if (!isFlexibleNullExpr(da->expr())) {
                            if (isIntTypeName(innerType->name) && isFlexibleIntExpr(da->expr())) {
                                tryInferIntType(da->expr(), *innerType);
                            }
                            auto exprType = da->expr()->getType();
                            bool wholeCopy = exprType.isNullable() && exprType == varType;
                            bool wrap = exprType == *innerType;
                            if (!wholeCopy && !wrap) {
                                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, exprType.name,
                                               innerType->name);
                            }
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败: 留 Compiler 兜底
            }
            if (varType.isRef()) {
                auto innerType = varType.refElementType();
                if (innerType) {
                    if (auto litExpr = dynamic_cast<p<ExprLiteralNode>>(da->expr())) {
                        if (auto litObj = dynamic_cast<p<LiteralObjNode>>(litExpr->literal())) {
                            string srcName = litObj->getValue().getText();
                            auto sym = _currentFn->lookupSymbol(srcName);
                            if (!sym || !sym->type.isRef() || !sym->type.refElementType() ||
                                *sym->type.refElementType() != *innerType) {
                                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3018, srcName,
                                               innerType->name)
                                    .withHint(std::format("`{}` 不是 {}& 类型，无法 copy-bind 到此声明；改写为 "
                                                          "`&<expr-of-{}>` 或先声明同类型 T&",
                                                          srcName, innerType->name, innerType->name));
                            }
                        }
                    }
                }
            }

            // 通用类型匹配检查：声明类型与表达式类型必须严格一致
            // 跳过已独立处理的容器类型 (Ref / Array / Nullable / Rc / Weak / ArrayGeneric / Heap)
            // 跳过灵活整数字面量 (类型会在编译器端按目标类型推断)
            // 跳过类型别名声明 (如 `type A = i32` / `type IPair = (i32,i32)`) —
            // 别名解析可能跨 kind（Normal→Tuple/Array），完整解析留 Compiler 端 applySubst 兜底
            auto isAliasName = [&](const string& n) -> bool {
                if (_file && _file->getAliasDecl(n)) return true;
                if (_sdkFile && _sdkFile->getAliasDecl(n)) return true;
                return false;
            };
            // 跳过 ExprPathCallNode（如 `Label::COUNT` / `P::get_x()`）—
            // 这些表达式有独立的类型/语义校验（E3120/E3121 等），不应被通用类型检查遮蔽
            if (!varType.isRef() && !varType.isArray() && !varType.isNullable() && !varType.isRc() &&
                !varType.isWeak() && !varType.isArrayGeneric() && !varType.isHeap() && !varType.isFn() &&
                varType.genericArgs.empty() && !isFlexibleIntExpr(da->expr()) && !isAliasName(varType.name) &&
                !dynamic_cast<p<ExprPathCallNode>>(da->expr())) {
                try {
                    auto exprType = da->expr()->getType();
                    // 跳过泛型形参 / 未解析类型（如 T, U 等）：此时尚未实例化，比较无意义
                    auto isKnownType = [&](const TypeInfo& t) -> bool {
                        if (isBuiltinType(t.name)) return true;
                        if (_file && _file->getStructDecl(t.name)) return true;
                        if (_sdkFile && _sdkFile->getStructDecl(t.name)) return true;
                        return false;
                    };
                    if (!varType.name.empty() && !exprType.name.empty() && varType.name != "Self" &&
                        exprType.name != "Self" && !exprType.isRef() && !exprType.isFn() &&
                        !isAliasName(exprType.name) && isKnownType(varType) && isKnownType(exprType)) {
                        if (varType != exprType) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014,
                                           varType.getFullName(), exprType.getFullName())
                                .withHint(std::format("声明类型为 `{}`，但表达式类型为 `{}`；yux 无隐式类型转换",
                                                      varType.getFullName(), exprType.getFullName()));
                        }
                    }
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // getType 失败: 留 Compiler 兜底
                }
            }
        }
        if (da->expr()) visitExpr(da->expr());
        // v0.16 闭包捕获: lambda 字面量直接作 var/val 初始化值且含 T& 捕获 → E4022
        // (spec §8.7.6.5 不可逃逸：fn 值不可被存储到寿命外延的变量)。
        // 仅拦截直接形 (lambda 字面量), 穿透检测 (右值 wrapper 调用结果等) 留 codegen 兜底。
        if (da->expr()) {
            if (auto litLambda = dynamic_cast<p<LambdaExprNode>>(da->expr())) {
                if (litLambda->hasRefCapture()) {
                    throw YuxError(litLambda->getLineNumber(), litLambda->getColumn(), ErrorCode::E4022);
                }
            }
        }
        // Phase B-1: #NoCopy 类型不可从现有变量隐式复制（let 绑定）
        if (da->varType() && da->expr()) {
            auto varType = da->varType()->getType();
            if (isNoCopyTypeIn(varType, _file, _sdkFile) && !varType.isRef()) {
                if (!isFreshHandleExpr(da->expr())) {
                    throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E4031, varType.name,
                                   "let 绑定", varType.name);
                }
            }
        }
        return;
    }
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
        // E4030: `a <- b` 作为表达式语句时结果被丢弃，建议改用 `a = b`
        if (auto ma = dynamic_cast<p<ExprMoveAssignNode>>(se->expr())) {
            DiagnosticEngine::emit(_sourcePath,
                                   YuxError(ma->resolveLineNumber(), ma->resolveColumn(), ErrorCode::E4030));
        }
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
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // 非 YuxError (内部异常) 不该出现; 防御性吞掉以免影响 codegen
    }

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // Phase 2e 构造模型重构: `#Static fn` 体内禁用 `$` (E3128).
        // `$` 在 ast_builder 里生成 ExprLiteralNode(LiteralObjNode("$")),
        // `$.field` / `$.method()` 读路径会递归到此, 一处拦截即覆盖.
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(n->literal())) {
            if (obj->getValue().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
            }
            // Phase B-1: use-after-move 检查 (E4033)
            if (_currentFn && obj->getValue().getText() != "$") {
                string varName = obj->getValue().getText();
                if (_movedVars.count(varName)) {
                    // lambda 体内仅当 varName 不是 lambda 形参时才报错
                    if (!_currentLambda) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                    } else {
                        bool isParam = false;
                        for (auto& p : _currentLambda->params()) {
                            if (p.name.getText() == varName) {
                                isParam = true;
                                break;
                            }
                        }
                        if (!isParam) {
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                        }
                    }
                }
            }
        }
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps())
                visitExpr(e);
            // Bucket 6 (CURRENT-check.md): E3026 插值类型必须实现 ToString.
            sema::validateStringTemplateInterps(_file, _sdkFile, tpl);
        }
        // v0.16 闭包捕获: lambda body 内标识符引用检查。
        // - 引用外层 Heap<T> (非空) 变量 → E4024 (Heap 按值捕获禁止, §7.3 / [#18])
        // - 引用外层 T& 变量 → 标记 hasRefCapture (E4022 数据收集)
        // 非 ID-obj / 全局 / template 插值等其它字面量形态不触发捕获, 跳过。
        // $ 在方法体内 lambda 是 Self&, 同样标记 hasRefCapture。
        if (_currentLambda && _currentFn) {
            auto obj2 = dynamic_cast<p<LiteralObjNode>>(n->literal());
            if (obj2) {
                string varName = obj2->getValue().getText();
                // 检查标识符是否为 lambda 形参 (不在形参列表 → 外层变量 → 捕获).
                // 不能用 bodyScope->lookupSymbol, 因其沿父链查找.
                bool isParam = false;
                for (auto& p : _currentLambda->params()) {
                    if (p.name.getText() == varName) {
                        isParam = true;
                        break;
                    }
                }
                if (!isParam) {
                    if (auto sym = _currentFn->lookupSymbol(varName)) {
                        const auto& t = sym->type;
                        if (t.isHeap()) {
                            auto elem = t.heapElementType();
                            string elemName = elem ? elem->getFullName() : string("?");
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4024, elemName,
                                           varName, elemName);
                        }
                        if (t.isRef()) {
                            _currentLambdaHasRefCapture = true;
                        }
                    }
                }
            }
        }
        // Phase 3.4.f.2: int 字面量越界 (E3103) — getType 仅返回类型不解析值,
        // 这里主动调 sema::parseIntLiteral 触发越界 / 非法格式校验.
        if (auto intLit = dynamic_cast<p<LiteralIntNode>>(n->literal())) {
            (void)sema::parseIntLiteral(intLit->getValue().getText(), n->getLineNumber(), n->getColumn());
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 单点: 自定义 struct 二元运算符方法解析 (E3073 + byval hint).
        string m = (n->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprMulDivModNode::Op::Mul:
            m = "mul";
            break;
        case ExprMulDivModNode::Op::Div:
            m = "div";
            break;
        case ExprMulDivModNode::Op::Mod:
            m = "mod";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprBinOpNode::Op::And:
            m = "and";
            break;
        case ExprBinOpNode::Op::Or:
            m = "or";
            break;
        case ExprBinOpNode::Op::Xor:
            m = "xor";
            break;
        case ExprBinOpNode::Op::Shl:
            m = "shl";
            break;
        case ExprBinOpNode::Op::Shr:
            m = "shr";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 (CURRENT-check.md): leftType 形态校验 (E3078 Weak ==/!= /
        // E3073 Ptr ordering). leftType getType 抛错 (lambda 形参等) 跳过.
        try {
            TypeInfo leftType = n->left()->getType();
            sema::validateCompareOpForm(leftType, n->op(), n->getLineNumber(), n->getColumn());
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 内部异常: 留 Compiler 兜底
        }
        // Bucket 6 单点: 自定义 struct 比较运算符方法解析 (E3073 + byval hint).
        // AndAnd / OrOr 是逻辑短路, 无方法名映射, 跳过.
        string m;
        switch (n->op()) {
        case ExprCompareNode::Op::Eq:
            m = "eq";
            break;
        case ExprCompareNode::Op::Ne:
            m = "ne";
            break;
        case ExprCompareNode::Op::Lt:
            m = "lt";
            break;
        case ExprCompareNode::Op::Le:
            m = "le";
            break;
        case ExprCompareNode::Op::Gt:
            m = "gt";
            break;
        case ExprCompareNode::Op::Ge:
            m = "ge";
            break;
        default:
            break;
        }
        if (!m.empty()) {
            tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr());
        for (auto& a : n->getArgs())
            visitExpr(a);

        // E4025 (DRAFT-heap-types §8.3a.5.1): 容器构造 turbofish 内嵌 Heap 拦截.
        // 形态: `Rc:<Heap<T>>(...)` / `Weak:<Heap<T>>(...)` / `Array:<Heap<T>>(...)`
        // 以及任意 call 的 turbofish 内出现 `Rc<Heap<T>>` / `Weak<...>` / `Array<...>` 嵌套.
        // 与 compiler_types.cpp:438/464/484 镜像.
        if (!n->getTypeArgs().empty()) {
            int eline = n->getLineNumber();
            int ecol = n->getColumn();
            string calleeName;
            if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                    calleeName = obj->getValue().getText();
                }
            }
            try {
                auto t0 = n->getTypeArgs()[0]->getType();
                if ((calleeName == "Rc" || calleeName == "Weak" || calleeName == "Array") && t0.isHeap()) {
                    auto inner = t0.heapElementType();
                    throw YuxError(eline, ecol, ErrorCode::E4025, calleeName, inner ? inner->name : std::string("?"));
                }
                for (auto& tn : n->getTypeArgs()) {
                    try {
                        validateNoNestedHeap(tn->getType(), eline, ecol);
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
            }
        }

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

                // Phase 3.3.2.f: 自由 intrinsic arity 校验 (E6027).
                // helper 仅对清单内 fnName 实际校验, 其他 fnName 是 no-op,
                // 故无条件调用安全; 与 Compiler 端 compileExternalOrSdkFunctionCall
                sema::validateFreeIntrinsicArity(fnName, n->getArgs().size(), line, col);

                auto* structDecl = _file->getStructDecl(fnName);
                if (!structDecl && _sdkFile) structDecl = _sdkFile->getStructDecl(fnName);

                // Phase 3.3.2.f: Builtin 泛型 intrinsic 的 shape + type-shape 校验.
                // 接管 E6017/E6018/E6026-E6029/E6032 实际抛出点 (与 Compiler::compileGenericFunctionCall
                // 的 #Builtin 分支镜像).
                // 限制:
                //   * 仅在 callee 是 ID-literal 且解析到泛型 fn 且 fn 头部 hasAnno(Builtin) 时接管;
                //   * typeArgs 仅在显式 (`f:<T>(...)`) 时由 SemaPass 取; 无显式 typeArgs (推断路径)
                //     需要 sema::inferGenericFnTypeArgs, 它会抛 E6012/E6013, 而这两码当前仍归 Compiler
                //     兜底 (3.3.1.b 未让 SemaPass 接管). 推断路径整体跳过, 留 Compiler 抛.
                //   * argTypes 经 getType() 计算, 任一 arg 未推断 (lambda 形参) 时跳过.
                if (!structDecl) {
                    auto [genFn, _] = _file->getGenericFunction(fnName);
                    // getGenericFunction 已搜索 wildcardImports，不再需要手动 SDK 回退
                    if (genFn && genFn->header()->hasAnno("Builtin") && hasTypeArgs) {
                        vector<TypeInfo> typeArgs;
                        bool typeArgsOk = true;
                        try {
                            for (auto& tn : n->getTypeArgs())
                                typeArgs.push_back(tn->getType());
                        } catch (...) {
                            typeArgsOk = false;
                        }

                        vector<TypeInfo> argTypes;
                        bool argTypesOk = true;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(a->getType());
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }

                        if (typeArgsOk) {
                            sema::validateBuiltinIntrinsicShape(fnName, typeArgs.size(), n->getArgs().size(), line,
                                                                col);
                            if (argTypesOk) {
                                sema::validateBuiltinIntrinsicTypeShape(fnName, typeArgs, argTypes, n->getArgs(), _file,
                                                                        _sdkFile, line, col);

                                // Phase B-1: copy_of 拒绝 #NoCopy 类型（含 Array<T>，深拷贝统一用 .clone()）
                                if (fnName == "copy_of" && !typeArgs.empty()) {
                                    const auto& T = typeArgs[0];
                                    if (isNoCopyTypeIn(T, _file, _sdkFile)) {
                                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031, T.name,
                                                       "copy_of", T.name);
                                    }
                                }
                            }
                        }
                    }
                }

                // Phase 6D: 同名 ctor 已被 sema E3130 拦截在定义点; 调用点 `Foo(args)`
                // 不再分派 ctor, 形态 / arity 校验全部失效, 整段块移除.

                // Phase B-1: move:<T>(var) — 标记源变量为 moved (E4033 判定依据)
                if (_currentFn && fnName == "move" && !n->getArgs().empty()) {
                    if (auto argLit = dynamic_cast<p<ExprLiteralNode>>(n->getArgs()[0])) {
                        if (auto argObj = dynamic_cast<p<LiteralObjNode>>(argLit->literal())) {
                            _movedVars.insert(argObj->getValue().getText());
                        }
                    }
                }

                // 泛型 fn + 显式 typeArgs 的 arity 校验 (E6010, 与 compileCallExpr 入口一致)
                if (!structDecl && hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn2, _] = _file->getFunctionWithOwner(fnName);
                    if (genFn2 && genFn2->header()->isGeneric()) {
                        sema::validateGenericTypeArgsArity(fnName, genFn2->header()->typeParams().size(),
                                                           n->getTypeArgs().size(), line, col);
                    }
                }

                // Bucket 4 收口 (CURRENT-check.md): 泛型 fn typeArgs 的 spec bound
                // 校验 (E1106, E3032 由 helper 内部抛 draft 名未声明). 显式 typeArgs
                // 直接收取; 隐式 typeArgs 走 sema::inferGenericFnTypeArgs (它抛
                // E6012/E6013, 由内部 try/catch 吞掉留 Compiler 兜底 — 这两码当前
                // 仍归 Compiler, 接管会破坏既有协议).
                if (_yux && !structDecl) {
                    // 收集所有同名泛型重载（如 print<T>(x T) + print<T>(x T&)），
                    // 用 resolveBestGenericOverload 选最佳匹配后再 infer + spec-bound 校验。
                    // collectGenericFunctions 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
                    vector<pair<FnNode*, FileNode*>> genericFns;
                    _file->collectGenericFunctions(fnName, genericFns, _file);
                    FnNode* genericFn = nullptr;
                    p<FileNode> fnOwner = _file;
                    if (!genericFns.empty()) {
                        // 多泛型重载消歧：用实参类型驱动，选 Ref/Ref 匹配最佳者
                        if (genericFns.size() > 1) {
                            vector<TypeInfo> disambigArgTypes;
                            bool disambigOk = true;
                            for (auto& a : n->getArgs()) {
                                try {
                                    disambigArgTypes.push_back(a->getType());
                                } catch (...) {
                                    disambigOk = false;
                                    break;
                                }
                            }
                            if (disambigOk && !disambigArgTypes.empty()) {
                                auto [best, bestOwner] =
                                    sema::resolveBestGenericOverload(genericFns, n, fnName, disambigArgTypes);
                                if (best) {
                                    genericFn = best;
                                    fnOwner = bestOwner;
                                }
                            }
                        }
                        if (!genericFn) {
                            genericFn = genericFns[0].first;
                            fnOwner = genericFns[0].second;
                        }
                    }
                    if (genericFn && genericFn->header()->isGeneric() && !genericFn->header()->hasAnno("Builtin")) {
                        vector<TypeInfo> typeArgs;
                        bool argTypesOk = true;
                        vector<TypeInfo> argTypes;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(a->getType());
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }
                        // BUG5: 非泛型重载优先 (call_fn.cpp Phase 4b)；
                        // 同名存在严格匹配的非泛型时，spec-bound 校验不应越过重载消歧
                        // 触发 E1106。命中非泛型即跳过整段校验。
                        if (argTypesOk && !hasTypeArgs) {
                            auto* nonGen = _file->lookupFnSymbolWithParams(fnName, argTypes);
                            if (!nonGen && _sdkFile && _sdkFile != _file) {
                                nonGen = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                            }
                            if (nonGen) {
                                bool isGenericSym = false;
                                for (auto& tp : genericFn->header()->typeParams()) {
                                    for (auto& p : nonGen->params) {
                                        if (p.name == tp) {
                                            isGenericSym = true;
                                            break;
                                        }
                                    }
                                    if (isGenericSym) break;
                                }
                                if (!isGenericSym) {
                                    argTypesOk = false; // 触发跳过下方校验
                                }
                            }
                        }
                        bool typeArgsOk = true;
                        if (hasTypeArgs) {
                            try {
                                for (auto& tn : n->getTypeArgs()) {
                                    typeArgs.push_back(tn->getType());
                                }
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else if (argTypesOk) {
                            try {
                                sema::inferGenericFnTypeArgs(n, genericFn, fnName, argTypes, typeArgs);
                            } catch (const YuxError&) {
                                // E6012/E6013 留 Compiler 兜底 (3.3.1.b 未让 SemaPass 接管)
                                typeArgsOk = false;
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else {
                            typeArgsOk = false;
                        }
                        if (typeArgsOk && typeArgs.size() == genericFn->header()->typeParams().size()) {
                            sema::validateGenericTypeArgsSpecBound(&_yux->specRegistry(), &_yux->specImplChecker(),
                                                                   fnOwner, genericFn->header(), typeArgs, line, col);
                        }
                    }
                }

                // 仅在无显式 typeArgs + 非泛型路径上才驱动重载解析:
                // 泛型 fn/ctor 走 Compiler 的 substitute 推断, 灵活整数推断由
                // 那条路径自行完成; SemaPass 暂不接入泛型实例化.
                if (!hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn3, _] = _file->getFunctionWithOwner(fnName);
                    if (!genFn3 || !genFn3->header()->isGeneric()) {
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
                                try {
                                    argTypes.push_back(a->getType());
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok) {
                                auto* fnSym = _file->lookupFnSymbolWithParams(fnName, argTypes);
                                if (!fnSym && _sdkFile && _sdkFile != _file) {
                                    fnSym = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                                }
                                sema::validateFnSymbolVisibility(fnSym, _file->moduleName(), fnName, line, col);
                                // Phase B-1: #NoCopy 类型不可按值传参
                                if (fnSym) {
                                    for (size_t i = 0; i < n->getArgs().size() && i < fnSym->params.size(); ++i) {
                                        const auto& pt = fnSym->params[i];
                                        if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                            if (!isFreshHandleExpr(n->getArgs()[i])) {
                                                throw YuxError(n->getLineNumber(), n->getColumn(),
                                                               ErrorCode::E4031, pt.name, "按值传参",
                                                               pt.name);
                                            }
                                        }
                                    }
                                }
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
            // DRAFT-spec-disambig-at §3.3: `$.m@SpecA()` / `obj.m@SpecA()` 显式消歧.
            // 校验三件: (1) baseType T 必须在 #Impl 列表里出现 SpecA; (2) SpecA 必须
            // 含名为 m 的签名; (3) SpecA.m 必须带默认体 (纯抽象签名无法 disambiguate).
            if (dotCallee->hasSpecQualifier()) {
                TypeInfo baseType;
                bool baseOk = true;
                try {
                    baseType = dotCallee->baseExpr()->getType();
                } catch (...) {
                    baseOk = false;
                }
                if (baseOk && !baseType.name.empty()) {
                    const string& specName = dotCallee->specQualifier();
                    const string memberName = dotCallee->member();
                    int dline = dotCallee->resolveLineNumber();
                    int dcol = dotCallee->resolveColumn();
                    // Dyn<D> / Dyn<D&> 上 `d.m@SpecA()`: SpecA 必须等于 D (vtable 只
                    // 携带 D 的槽位, 无法 dispatch 到其它 spec). 等于 D 时直接走常规
                    // Dyn dispatch (codegen 不重写 member).
                    if (baseType.isDyn()) {
                        if (_yux) {
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolvedDyn = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            if (resolvedDyn.decl && resolvedDyn.decl->name().getText() != specName) {
                                throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                            }
                        }
                        // OK: 等价于 d.m(); 走 Dyn dispatch
                    } else {
                        auto* implNode = _file->getStructImpl(baseType.name);
                        if (!implNode && _sdkFile) {
                            implNode = _sdkFile->getStructImpl(baseType.name);
                        }
                        bool inImplList = false;
                        if (implNode) {
                            for (const auto& ref : implNode->specRefs()) {
                                if (ref.name == specName) {
                                    inImplList = true;
                                    break;
                                }
                            }
                        }
                        if (!inImplList) {
                            throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                        }
                        SpecDeclNode* specDecl = nullptr;
                        if (_yux) {
                            auto resolved = _yux->specRegistry().resolve(specName, _file);
                            if (resolved) specDecl = resolved->decl;
                        }
                        if (specDecl) {
                            constexpr size_t kNoIdx = ~size_t{0};
                            auto sigIdx = kNoIdx;
                            for (size_t i = 0; i < specDecl->signatures().size(); ++i) {
                                if (specDecl->signatures()[i]->name().getText() == memberName) {
                                    sigIdx = i;
                                    break;
                                }
                            }
                            if (sigIdx == kNoIdx) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " (referenced via `@" + specName + "` — method missing in spec)");
                            }
                            if (!specDecl->hasDefaultBody(sigIdx)) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " with a default body (`@" + specName +
                                                   "` disambiguation requires a default-body method)");
                            }
                        }
                    } // end else (non-Dyn)
                }
            }
            vector<TypeInfo> argTypes;
            bool ok = true;
            for (auto& a : n->getArgs()) {
                try {
                    argTypes.push_back(a->getType());
                } catch (...) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                auto modCall = sema::resolveModuleFnCall(_file, nullptr, n, dotCallee, argTypes);
                if (!modCall.matched) {
                    // Phase 3.3.2.f: 镜像 Compiler::compileMethodCall 的 baseType 派发,
                    // 主动调用 3.3.2.a / 3.3.2.e 抠出的 helper.
                    //   * baseType.isArrayGeneric() → validateArrayMethodCall (E3055/E6042/E6027)
                    //   * isBuiltinType + isBuiltinMethodIn → validateOperatorMethodCall (E6027/E3070)
                    // baseType 经 getType() 计算; 任一异常 (lambda 形参等) → 跳过, 交 Compiler 兜底.
                    // SemaPass 走非泛型 fn / 非泛型 impl 路径, 不需要 applySubst (替换栈为空).
                    TypeInfo baseType;
                    bool baseOk = true;
                    try {
                        baseType = dotCallee->baseExpr()->getType();
                    } catch (...) {
                        baseOk = false;
                    }
                    if (baseOk) {
                        const string& member = dotCallee->member();
                        size_t argsCount = n->getArgs().size();
                        int dline = n->getLineNumber();
                        int dcol = n->getColumn();
                        if (baseType.isArrayGeneric()) {
                            bool baseIsLvalue = isLvalueArrayBase(dotCallee->baseExpr());
                            sema::validateArrayMethodCall(baseType, member, argsCount, baseIsLvalue, dline, dcol);
                        } else if (isBuiltinType(baseType.name) && isBuiltinMethodIn(_sdkFile, baseType.name, member)) {
                            sema::validateOperatorMethodCall(member, baseType, argsCount, dline, dcol);
                        } else if (baseType.isDyn() && _yux) {
                            // Bucket 4 收口 (CURRENT-check.md): Dyn<D> 方法调用 (E1131/E6016/
                            // E6012/E6015). 镜像 Compiler::compileDynMethodCall 顶部 — 通过
                            // resolveDynCalleeSpec 拿 specDecl, 再 resolveDynMethodSig 校验
                            // member 存在 + arity + 形参类型.
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolved = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            sema::resolveDynMethodSig(resolved.decl, resolved.qualified, baseType, member, argTypes,
                                                      dline, dcol);
                        }
                    }
                }
            }
        }

        // Fn-typed callee 实参类型校验：当 callee 静态类型为 Fn(...)R 时，
        // 检查每个实参与形参类型是否匹配（Ref<T> 不能隐式转为 T 等）。
        // 非 ID-literal callee（如 lambda 变量 f(xs[i])）走 compiler_lambda.cpp
        // 的 compileFnValueCall，此处提前检测避免 "bad signature" LLVM 断言。
        {
            TypeInfo calleeType;
            try {
                calleeType = n->getCalleeExpr()->getType();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }

            if (calleeType.isFn()) {
                const auto& expectedParams = calleeType.fnParamTypes();
                for (size_t idx = 0; idx < n->getArgs().size() && idx < expectedParams.size(); ++idx) {
                    try {
                        auto argType = n->getArgs()[idx]->getType();
                        if (expectedParams[idx] && !argType.name.empty() && argType.name != "Self" &&
                            expectedParams[idx]->name != "Self") {
                            // Ref<T> 实参传给值类型形参 T
                            if (argType.isRef() && !expectedParams[idx]->isRef()) {
                                auto inner = argType.refElementType();
                                throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                               expectedParams[idx]->getFullName(), argType.getFullName())
                                    .withHint(std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                                          "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                                          inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                                          inner ? inner->name : "?", inner ? inner->name : "?"));
                            }
                            // 值类型不匹配
                            if (!argType.isRef() && !expectedParams[idx]->isRef() && argType != *expectedParams[idx]) {
                                throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                               expectedParams[idx]->getFullName(), argType.getFullName())
                                    .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", argType.getFullName(),
                                                          expectedParams[idx]->getFullName()));
                            }
                        }
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                        // getType 失败: 留 Compiler 兜底
                    }
                }
            }
        }
        // struct 方法私有可见性检查（E6007）——因 yux-check 不跑 LLVM
        // codegen，必须在 sema 阶段独立校验。与 codegen compileStructMethodCall
        // 中的 validateStructMethodVisibility 同义，构成双重保障。
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            try {
                TypeInfo baseType = dotCallee->baseExpr()->getType();
                if (baseType.isRef()) {
                    if (auto inner = baseType.refElementType()) baseType = *inner;
                }
                if (baseType.isRc()) {
                    if (auto inner = baseType.rcElementType()) baseType = *inner;
                }
                if (!baseType.name.empty() && !baseType.isDyn() && !isBuiltinType(baseType.name)) {
                    string methodFullName = baseType.name + "." + dotCallee->member();
                    vector<TypeInfo> methodParamTypes;
                    methodParamTypes.push_back(baseType);
                    for (auto& a : n->getArgs()) {
                        try {
                            methodParamTypes.emplace_back(a->getType());
                        } catch (...) {
                            methodParamTypes.emplace_back();
                        }
                    }
                    auto* methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    if (!methodSymbol && _sdkFile && _sdkFile != _file) {
                        methodSymbol = _sdkFile->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    }
                    sema::validateStructMethodVisibility(methodSymbol, _currentStructName, baseType.name,
                                                         dotCallee->member(), n->getLineNumber(), n->getColumn());
                    // Phase B-1: 方法调用的 #NoCopy 按值传参检查
                    if (methodSymbol) {
                        for (size_t i = 0; i < n->getArgs().size() && i < methodSymbol->params.size(); ++i) {
                            const auto& pt = methodSymbol->params[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->getArgs()[i])) {
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031,
                                                   pt.name, "按值传参", pt.name);
                                }
                            }
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败或符号查找失败——留 Compiler 兜底
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        visitExpr(n->baseExpr());

        // DRAFT-spec-reflect Phase 4: 实例形访问 `c.type` / `c.fields` / `c.methods` /
        // `c.variants` / `$.type` / `$.fields` 拦截 (草案 §5 / [#1.AB]);
        // 提示用 `<Type>::field` / `Self::field`.
        // 仅当 base 是已知 struct 且不含同名 instance 字段时触发 (用户若自己声明
        // `type` 字段, 走常规字段访问).
        {
            string mem = n->member();
            if (mem == "type" || mem == "fields" || mem == "methods" || mem == "variants") {
                TypeInfo bt;
                try {
                    bt = n->baseExpr()->getType();
                } catch (...) {
                    bt = TypeInfo();
                }
                if (bt.isRef()) {
                    if (auto inner = bt.refElementType()) bt = *inner;
                }
                if (bt.isRc()) {
                    if (auto inner = bt.rcElementType()) bt = *inner;
                }
                if (bt.kind == TypeKind::Normal && !bt.name.empty()) {
                    auto* sd = _file ? _file->getStructDecl(bt.name) : nullptr;
                    if (!sd && _sdkFile && _sdkFile != _file) sd = _sdkFile->getStructDecl(bt.name);
                    if (sd && sd->fieldIndex(mem) < 0) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E1138, mem, bt.name,
                                       bt.name, mem);
                    }
                }
            }
        }

        // Phase 3.4.d.2: 字段私有可见性 (E3042). safe `?.` 路径在 helper 内
        // 自跳过 (走 getType, kMigratedCodes 已覆盖). 异常静默吞掉, 留 Compiler.
        try {
            sema::validateDotFieldPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        // Phase B-1: 分支 _movedVars 汇合 — 各分支分别从 saved 出发，最后取并集
        auto savedMoved = _movedVars;
        visitBlock(n->thenBlock());
        auto afterThenMoved = std::move(_movedVars);
        _movedVars = savedMoved;

        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            auto savedElif = _movedVars;
            visitBlock(el->block());
            for (auto& v : _movedVars) afterThenMoved.insert(v);
            _movedVars = savedElif;
        }

        if (n->elseBlock()) {
            visitBlock(n->elseBlock());
            for (auto& v : afterThenMoved) _movedVars.insert(v);
        } else {
            _movedVars = std::move(afterThenMoved);
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitExpr(n->trueValue());
        visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElsePreValueNode>>(expr)) {
        visitExpr(n->condition());
        visitExpr(n->trueValue());
        visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices())
            visitExpr(i);
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // v0.16 闭包捕获: sema 下钻 lambda body (策略 2b 宽松模式)。
        // - 形参类型可能缺 (由调用点反推), 不依赖形参类型的检查 deferred 给 codegen。
        // - 不依赖形参类型的检查在此完成: E2030 (捕获写禁) / E4024 (Heap 非空捕获禁) /
        //   E4022 数据收集 (hasRefCapture)。
        // - 下钻前保存外层 lambda 状态, 支持嵌套闭包。
        auto savedLambda = _currentLambda;
        auto savedHasRef = _currentLambdaHasRefCapture;
        _currentLambda = n;
        _currentLambdaHasRefCapture = false;

        if (n->bodyExpr()) {
            visitExpr(n->bodyExpr());
        } else {
            for (auto& stmt : n->bodyStmts()) {
                visitStmt(stmt);
            }
        }

        // 将 hasRefCapture 写回 LambdaExprNode, 供 E4022 检查 (StatementRetNode /
        // StatementDeclareAssignNode) 读取。
        if (_currentLambdaHasRefCapture) {
            n->setHasRefCapture(true);
        }

        _currentLambda = savedLambda;
        _currentLambdaHasRefCapture = savedHasRef;
        return;
    }
    if (auto n = dynamic_cast<p<ExprStructLitNode>>(expr)) {
        // Phase 2d 构造模型重构: `Self { ... }` 字段字面量校验.
        //   * 出现位: 仅 `#Static fn` 体内 (E3124, 仅 Self 形态).
        //   * 完整性: 必须列全所属结构体所有字段 (E3125).
        //   * 已知字段: `.name` 必须是所属结构体的字段 (E3126).
        //   * 唯一: 同名 `.field` 出现两次报 (E3127).
        // DRAFT-const-eval Phase 5: TypeName{...} 形态放行至任意 expr 位.
        // codegen 仍走 E0000 占位 (Phase 3 接管).
        int line = n->resolveLineNumber();
        int col = n->resolveColumn();
        string structName;
        if (n->isSelfForm()) {
            if (!_currentFn || !_currentFn->header()->isStatic() || _currentStructName.empty()) {
                throw YuxError(line, col, ErrorCode::E3124);
            }
            structName = _currentStructName;
        } else {
            structName = n->structName();
        }
        StructDeclNode* decl = _file ? _file->getStructDecl(structName) : nullptr;
        if (!decl && _sdkFile && _sdkFile != _file) {
            decl = _sdkFile->getStructDecl(structName);
        }
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E3124);
        }
        std::set<string> seen;
        for (auto& fi : n->fields()) {
            string fname = fi->name().getText();
            size_t fline = fi->name().getLine();
            int fcol = static_cast<int>(fi->name().getCharPositionInLine());
            if (decl->fieldIndex(fname) < 0) {
                throw YuxError(fline, fcol, ErrorCode::E3126, structName, fname);
            }
            if (!seen.insert(fname).second) {
                throw YuxError(fline, fcol, ErrorCode::E3127, fname);
            }
            visitExpr(fi->value());
            // Phase B-1: #NoCopy 字段不可从现有变量隐式复制
            if (decl) {
                int fieldIdx = decl->fieldIndex(fname);
                if (fieldIdx >= 0) {
                    auto* fieldDecl = decl->fields()[fieldIdx];
                    auto fieldType = fieldDecl->getType();
                    if (isNoCopyTypeIn(fieldType, _file, _sdkFile)) {
                        if (!isFreshHandleExpr(fi->value())) {
                            throw YuxError(fline, fcol, ErrorCode::E4031, fieldType.name,
                                           "struct 字面量字段初始化", fieldType.name);
                        }
                    }
                }
            }
        }
        if (seen.size() != decl->fields().size()) {
            for (auto& f : decl->fields()) {
                if (!seen.count(f->name().getText())) {
                    throw YuxError(line, col, ErrorCode::E3125, structName, f->name().getText());
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprPathCallNode>>(expr)) {
        for (auto& a : n->args())
            visitExpr(a);

        // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` /
        // `<Struct>::methods` / `<Struct>::variants` reflect 静态访问.
        // 优先于 impl-method / enum-ctor 分流 (struct 无需 impl 也能取反射元数据).
        {
            string lhsName = n->enumName().getText();
            string rhsName = n->variantName().getText();
            if (n->args().empty() &&
                (rhsName == "type" || rhsName == "fields" || rhsName == "methods" || rhsName == "variants")) {
                auto* sd = _file ? _file->getStructDecl(lhsName) : nullptr;
                if (!sd && _sdkFile && _sdkFile != _file) sd = _sdkFile->getStructDecl(lhsName);
                if (sd) return;
            }
        }

        // Phase 2c 构造模型重构: `Type::name(...)` 按 LHS 分流.
        //   * LHS 是 struct -> 必须是 #Static 方法 (E3120/E3121); codegen Phase 3 落地.
        //   * LHS 是 enum   -> 走原 validateEnumCtorShape 路径 (E2019/E2020/E2021/E2032).
        // struct/enum 重名在 yux 里非法 (E2017), 此处直接按 lhsName 查 struct 优先.
        {
            string lhsName = n->enumName().getText();

            // DRAFT-static-vars Phase 4: 零参且 LHS 是 struct 且 RHS 是静态字段 → 放行
            if (n->args().empty()) {
                auto* structDecl = _file ? _file->getStructDecl(lhsName) : nullptr;
                if (!structDecl && _sdkFile && _sdkFile != _file) {
                    structDecl = _sdkFile->getStructDecl(lhsName);
                }
                if (structDecl) {
                    if (auto* sf = structDecl->staticField(n->variantName().getText())) {
                        // 设置正确类型（字段类型而非 struct 类型）
                        n->setResolvedType(sf->type->getType());
                        return;
                    }
                }
            }

            auto* structImpl = _file ? _file->getStructImpl(lhsName) : nullptr;
            if (!structImpl && _sdkFile && _sdkFile != _file) {
                structImpl = _sdkFile->getStructImpl(lhsName);
            }
            if (structImpl) {
                string rhsName = n->variantName().getText();

                // DRAFT-static-vars Phase 4: 若零参且 RHS 是静态字段名 → 放行
                if (n->args().empty()) {
                    auto* structDecl = _file ? _file->getStructDecl(lhsName) : nullptr;
                    if (!structDecl && _sdkFile && _sdkFile != _file) {
                        structDecl = _sdkFile->getStructDecl(lhsName);
                    }
                    if (structDecl && structDecl->staticField(rhsName)) {
                        return; // 静态字段读，放行
                    }
                }

                p<FnHeaderNode> methodHeader = nullptr;
                for (auto& m : structImpl->methods()) {
                    if (m->header()->name().getText() == rhsName) {
                        methodHeader = m->header();
                        break;
                    }
                }
                int line = n->resolveLineNumber();
                int col = n->resolveColumn();
                if (!methodHeader) {
                    throw YuxError(line, col, ErrorCode::E3121, lhsName, rhsName);
                }
                if (!methodHeader->isStatic()) {
                    throw YuxError(line, col, ErrorCode::E3120, lhsName, rhsName, rhsName);
                }
                // Bucket 4 收口 (CURRENT-check.md): #Static fn 调用站点的 arity +
                // 类型校验 (E3131). 镜像 compiler_expr.cpp::compileEnumCtorExpr 的
                // #Static fn 分派 (2343-2377). 仅在非泛型 struct + 无 turbofish 时接管;
                // 泛型 struct 的 applySubst 留 Compiler 兜底 (sema 无替换栈).
                bool skipTypeCheck = !n->lhsTypeArgs().empty();
                if (!skipTypeCheck) {
                    auto* structDecl = _file ? _file->getStructDecl(lhsName) : nullptr;
                    if (!structDecl && _sdkFile && _sdkFile != _file) {
                        structDecl = _sdkFile->getStructDecl(lhsName);
                    }
                    if (structDecl && structDecl->isGeneric()) skipTypeCheck = true;
                }
                if (!skipTypeCheck) {
                    vector<TypeInfo> paramTypes;
                    bool paramTypesOk = true;
                    for (auto p : methodHeader->params()) {
                        if (p->type()) {
                            try {
                                paramTypes.push_back(p->type()->getType());
                            } catch (...) {
                                paramTypesOk = false;
                                break;
                            }
                        } else {
                            paramTypesOk = false;
                            break;
                        }
                    }
                    if (paramTypesOk) {
                        // 灵活整数实参按形参类型回填 (与 Compiler 端 2340 一致)
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            tryInferIntType(n->args()[i], paramTypes[i]);
                        }
                        auto renderTypes = [](const vector<TypeInfo>& ts) {
                            string s;
                            for (size_t i = 0; i < ts.size(); ++i) {
                                if (i) s += ", ";
                                s += ts[i].getFullName();
                            }
                            return s;
                        };
                        // arity 校验
                        if (n->args().size() != paramTypes.size()) {
                            string expected = renderTypes(paramTypes);
                            vector<TypeInfo> argTypesRaw;
                            bool ok = true;
                            for (auto& a : n->args()) {
                                try {
                                    argTypesRaw.push_back(a->getType());
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            string got = ok ? renderTypes(argTypesRaw) : string("<unresolved>");
                            throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(), expected,
                                           n->args().size(), got);
                        }
                        // 类型逐位比对
                        vector<TypeInfo> argTypes;
                        bool argOk = true;
                        for (auto& a : n->args()) {
                            try {
                                argTypes.push_back(a->getType());
                            } catch (...) {
                                argOk = false;
                                break;
                            }
                        }
                        if (argOk) {
                            for (size_t i = 0; i < argTypes.size(); ++i) {
                                if (argTypes[i].empty()) continue;
                                if (!(argTypes[i] == paramTypes[i])) {
                                    throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(),
                                                   renderTypes(paramTypes), argTypes.size(), renderTypes(argTypes));
                                }
                            }
                        }
                        // Phase B-1: #NoCopy 类型不可按值传参（#Static fn 调用）
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            const auto& pt = paramTypes[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->args()[i])) {
                                    throw YuxError(line, col, ErrorCode::E4031, pt.name, "按值传参",
                                                   pt.name);
                                }
                            }
                        }
                    }
                }
                return;
            }
        }

        // Phase 3.4.a: SemaPass 接管 E2019/E2020/E2021/E2032.
        // node->setResolvedType 已在 visitExpr 顶部写好 (getType 抛错时已在白名单
        // 重抛, 否则吞掉; 这里能跑到说明 getType 至少没抛已迁移码).
        // 任一异常被 helper 内部 try/catch (E2032 路径) 吞掉; E2019/E2020/E2021
        // 由 helper 主动抛出, SemaPass 实际接管.
        try {
            sema::validateEnumCtorShape(_file, _sdkFile, n);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御: helper 内部异常 (理论不应出现) 跳过, 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms())
            visitExpr(arm->body());

        // Phase 3.4.b: SemaPass 接管 E2019/E2020/E2023/E2024/E2025/E2026/E2027.
        // 仅在 scrut 直接是 enum 名 (非 Rc/E / 非 alias 链) 时接入: 那两条路径
        // Compiler 端走 isFreshHandleExpr / resolveAlias (递归), SemaPass 暂未镜像,
        // 跳过留 Compiler 兜底. scrutType getType 抛错 (lambda 形参等) 时也跳过.
        try {
            TypeInfo scrutType = n->scrutinee()->getType();
            // v0.16: [] 返回 T&——match scrutinee 自动剥 Ref 检查底层 enum 类型
            TypeInfo checkType =
                scrutType.isRef() && scrutType.refElementType() ? *scrutType.refElementType() : scrutType;
            // Rc<E> 自动 deref 走 Compiler 兜底, 不在此处接入
            if (!checkType.isRc()) {
                auto* enumDecl = lookupEnumIn(_file, _sdkFile, checkType.name);
                if (enumDecl) {
                    sema::validateMatchArms(enumDecl, checkType.name, n, _file);
                } else if (isBuiltinType(checkType.name) || checkType.name == "String") {
                    // Bucket 6 (CURRENT-check.md): E2022 scrutinee 非 enum.
                    // 仅在 builtin 原型 / String 时接管 — 复杂路径 (alias 链 /
                    // Box<E> / fresh Rc) 留 Compiler 兜底.
                    int line = n->getLineNumber();
                    int col = n->getColumn();
                    throw YuxError(line, col, ErrorCode::E2022, checkType.name);
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
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
                throw YuxError(aline, acol, ErrorCode::E7011, arm->errName().getText(), errType, errType);
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
                if (ct == seen) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                throw YuxError(line, col, ErrorCode::E7002, seen, string("<unknown>"), seen);
            }
        }

        for (auto& c : n->catches())
            visitBlock(c->body());

        // Bucket 6 (CURRENT-check.md): SemaPass 接管 E7010 (catch arm body 末
        // 表达式类型必须与 try block 末表达式类型一致).
        //
        // 仅在 try block hasResult 且 result expr getType 成功时启用; 任一 arm
        // 的 getType 抛错 (lambda 形参等) 跳过该 arm, 留 Compiler 兜底. 流终止
        // arm 自然 hasResult=false, 此处略过. 与 Compiler 端 (compiler_expr.cpp
        // E7010 throw) 同语义按 .name 比对.
        if (n->tryBlock()->hasResult() && n->tryBlock()->resultExpr()) {
            try {
                auto resultType = n->tryBlock()->resultExpr()->getType();
                for (auto& arm : n->catches()) {
                    if (!arm->body()->hasResult() || !arm->body()->resultExpr()) continue;
                    try {
                        auto armT = arm->body()->resultExpr()->getType();
                        if (armT.name != resultType.name) {
                            int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                            int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                            throw YuxError(aline, acol, ErrorCode::E7010, armT.name, resultType.name);
                        }
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch) — arm getType 失败: 留 Compiler 兜底
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch) — try result getType 失败: 留 Compiler 兜底
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg());
        // Bucket 4 收口 (CURRENT-check.md): Dyn<D>(x) 构造的 E1131/E1132/E1134/E1133
        // 接管. 镜像 compiler_expr.cpp::compileDynCtorExpr 顶部 (line 2497-2576).
        // 仅在 _yux 就绪时校验 (spec 注册表 + impl 检查器都从 Yux 取); SDK 自构建
        // 等无 Yux 场景 skip, 留 Compiler 兜底.
        if (!_yux) return;
        try {
            auto resultType = n->getType();
            int line = n->getLineNumber();
            int col = n->getColumn();
            auto specInner = resultType.dynSpecType();
            string specBareName = specInner ? specInner->name : string();

            auto& reg = _yux->specRegistry();
            SpecDeclNode* specDecl = nullptr;
            string specQualified;
            if (!specBareName.empty()) {
                if (auto resolved = reg.resolve(specBareName, _file)) {
                    specDecl = resolved->decl;
                    specQualified = resolved->qualifiedName;
                }
            }
            if (!specDecl) {
                throw YuxError(line, col, ErrorCode::E1131, specBareName.empty() ? string("?") : specBareName);
            }
            if (specInner && specInner->isDyn()) {
                throw YuxError(line, col, ErrorCode::E1132, resultType.getFullName());
            }
            auto& checker = _yux->specImplChecker();
            if (!checker.specIsObjectSafe(specDecl)) {
                throw YuxError(line, col, ErrorCode::E1134, specQualified, specQualified, specQualified);
            }

            auto argType = n->arg()->getType();
            bool isBorrow = n->isBorrow();
            string concreteBare;
            if (isBorrow) {
                if (argType.isRef()) {
                    if (auto inner = argType.refElementType()) concreteBare = inner->name;
                } else if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            } else {
                if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            }
            if (concreteBare.empty()) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
            TypeInfo concreteTI(concreteBare);
            vector<TypeInfo> specTypeArgs;
            if (!checker.boundSatisfied(concreteTI, specDecl, specQualified, specTypeArgs)) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 等内部异常 (lambda 形参未推断等): 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprHeapCtorNode>>(expr)) {
        // Phase 2.6: Heap:<T>(x) 形态检查 (DRAFT-heap-types §8.3a)
        // - 递归 arg
        // - E3028: arg 类型必须与 turbofish 内层 T 等价
        // - E4025: Rc/Weak/Array<Heap<...>> 在 getLLVMType 容器分支拦截，不在此处
        visitExpr(n->arg());
        auto resultType = n->getType();
        auto innerSp = resultType.heapElementType();
        if (innerSp) {
            const auto& innerT = *innerSp;
            auto argType = n->arg()->getType();
            // Phase 8b: Heap:<T>(p Ptr) FFI take-over (DRAFT-heap-types §8.3a) —
            // T != Ptr 时, argType == Ptr 视作合法 (代表接管裸指针所有权).
            bool takeoverFromPtr = argType.isPtr() && innerT.name != "Ptr";
            if (!takeoverFromPtr && !(argType == innerT)) {
                throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014, innerT.name, argType.name);
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 收口+ (CURRENT-check.md): E3024 (左侧非 Nullable) + E3023 (右侧
        // 类型不匹配). 镜像 compiler_expr.cpp:1955-2000. 复杂路径 (alias / Self) 由
        // getType 抛错时跳过, 留 Compiler 兜底.
        try {
            auto leftType = n->left()->getType();
            if (!leftType.isNullable()) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3024, leftType.name);
            }
            auto innerType = leftType.nullableInnerType();
            if (!innerType) return;
            if (isIntTypeName(innerType->name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), *innerType);
            }
            auto rightType = n->right()->getType();
            if (!(rightType == *innerType)) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3014, innerType->name,
                               rightType.name);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 抛 std::runtime_error 等: 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMoveAssignNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // 校验 left 为合法 lvalue + 类型兼容。
        // left 必须是变量引用 ($ / a.b / a[ ... ]) 等可赋值表达式。
        // right 类型必须能与 left 类型兼容（相同或灵活整数字面量）。
        try {
            auto leftType = n->left()->getType();
            if (isIntTypeName(leftType.name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), leftType);
            }
            auto rightType = n->right()->getType();
            if (leftType != rightType) {
                // 非内置类型允许跨类型形参 (§7.2.3.3)
                if (isBuiltinType(leftType.name)) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3001, "arithmetic",
                                   leftType.name, rightType.name);
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 异常, 留 codegen 兜底
        }
        return;
    }
    // Phase 3.4.d.1: ExprGetRefNode —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprGetRefNode::getType 抛
    // E3040/E3041 (kMigratedCodes 命中, 自动重抛), 由此 Compiler 端
    // compileGetRefExpr 的 1656/1661 内联 throw 在正常 codepath 下不可达。
    // Phase 3.4.d.2: 补 E3042 链式私有字段可见性校验.
    if (auto n = dynamic_cast<p<ExprGetRefNode>>(expr)) {
        // Phase 2e: `&$.x` 在 `#Static fn` 体内禁用 (E3128).
        if (n->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
        }
        try {
            sema::validateGetRefPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }
        return;
    }
    // Phase 3.4.f.1: ExprArrayInitNode 显式化 —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprArrayInitNode::getType 抛 E3009
    // (explicitType vs value 字面量类型不匹配, kMigratedCodes 命中, 自动重抛).
    if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
        (void)n;
        return;
    }
    // 其余未识别节点 3.2 起补 assert。
}

void SemaPass::tryValidateBinOpMethod(p<ExprNode> leftExpr, p<ExprNode> rightExpr, const string& methodName, int line,
                                      int col) {
    // gate 与 Compiler::compileAddSubExpr / MulDivMod / BinOp / Compare 内
    // `!isBuiltinType(leftType.name) → compileCustomTypeBinaryOp` 一致, 但
    // 进一步把容器类排除 (容器走专属 codegen / sema 路径, 不该走到 method 解析):
    //   * Ref / Rc / Array / Heap / Weak / Nullable / Ptr / Tuple
    // 进而要求 struct decl 实际存在且非泛型 — 泛型 struct 需 applySubst, 留
    // Compiler; 模板形参 T 自然 getStructDecl 不到, 也被排除.
    // leftType / rightType getType 抛错 (lambda 形参等) 跳过, 留 Compiler 兜底.
    if (methodName.empty()) return;
    try {
        TypeInfo leftType = leftExpr->getType();
        TypeInfo rightType = rightExpr->getType();
        if (leftType.name.empty() || isBuiltinType(leftType.name)) return;
        // String 走 StringBuilder 特殊 lowering / 其它 builtin-handled 路径,
        // 没有用户可见的 plus/eq/... 方法签名, 不能走 customBinaryOp 解析.
        if (leftType.name == "String") return;
        if (leftType.isRef() || leftType.isRc() || leftType.isArrayGeneric() || leftType.isHeap() ||
            leftType.isWeak() || leftType.isNullable() || leftType.isPtr() || leftType.isTuple())
            return;
        StructDeclNode* decl = _file ? _file->getStructDecl(leftType.name) : nullptr;
        if (!decl && _sdkFile) decl = _sdkFile->getStructDecl(leftType.name);
        if (!decl || decl->isGeneric()) return;
        TypeInfo effRightType =
            (rightType.isRef() && rightType.refElementType()) ? *rightType.refElementType() : rightType;
        sema::validateBinOpMethodResolution(_file, _sdkFile, leftType, effRightType, methodName, line, col);
    } catch (const YuxError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

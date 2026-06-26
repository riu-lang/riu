// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "const_mut_checker.h"

#include "../error_code.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"

#include <set>

namespace {

// 拿到表达式所属作用域：优先 expr 自身的 findNearestScope，失败则回退到附近 stmt。
p<ScopeNode> exprScope(p<ExprNode> e, p<ScopeNode> fallback) {
    if (e) {
        if (auto sc = e->findNearestScope()) return sc;
    }
    return fallback;
}

// 判定一个表达式是否符合 §3.3 的"常量表达式"。
// 不通过时抛 E3104，errExpr 指向最里层不合规子表达式。
// scope 用于解析 LiteralObjNode 的符号引用（必须是 cval）。
void requireConstExpr(p<ExprNode> e, p<ScopeNode> scope);

void throwNonConst(p<ExprNode> e, const std::string& what) {
    int line = e->resolveLineNumber();
    int col = e->resolveColumn();
    throw YuxError(line, col, ErrorCode::E3104, what);
}

void requireConstExpr(p<ExprNode> e, p<ScopeNode> scope) {
    if (!e) return;

    if (auto paren = dynamic_cast<p<ExprParenNode>>(e)) {
        requireConstExpr(paren->expr(), scope);
        return;
    }
    if (auto u = dynamic_cast<p<ExprUnaryNode>>(e)) {
        requireConstExpr(u->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprAddSubNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprMulDivModNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprBinOpNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprCompareNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }

    if (auto le = dynamic_cast<p<ExprLiteralNode>>(e)) {
        auto lit = le->literal();
        if (dynamic_cast<p<LiteralIntNode>>(lit)) return;
        if (dynamic_cast<p<LiteralFloatNode>>(lit)) return;
        if (dynamic_cast<p<LiteralBoolNode>>(lit)) return;
        if (dynamic_cast<p<LiteralNullNode>>(lit)) return;
        if (dynamic_cast<p<LiteralStringNode>>(lit)) return;
        if (dynamic_cast<p<LiteralCodePointNode>>(lit)) return;
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit)) {
            auto name = obj->getValue().getText();
            auto sc = exprScope(e, scope);
            SymbolInfo* sym = sc ? sc->lookupSymbol(name) : nullptr;
            if (sym && sym->isConst) return;
            // Phase 4 (DRAFT-const-eval §4): #Const fn 体内, 参数与默认 (non-cval) 局部
            // 也可作 const 表达式的子项 —— 实际值绑定由 ConstEvaluator 在调用点注入 _env。
            // 这里只放行符号查找; const-evaluability 由后续 eval 真正判定。
            p<Node> cur = e;
            while (cur) {
                if (auto* fn = dynamic_cast<FnNode*>(cur)) {
                    if (fn->header() && fn->header()->hasAnno("Const")) return;
                    break;
                }
                cur = cur->parent();
            }
            throwNonConst(e, "reference to non-`cval` symbol `" + name + "`");
        }
        if (dynamic_cast<p<StringTemplateNode>>(lit)) {
            throwNonConst(e, "string template interpolation");
        }
        throwNonConst(e, "unsupported literal");
    }

    if (auto call = dynamic_cast<p<ExprCallNode>>(e)) {
        // Phase 4 (DRAFT-const-eval §4): #Const fn 调用允许进入 const 表达式。
        // 仅识别裸自由函数形态：callee = LiteralObj("name")。其它形态（方法 / 路径 /
        // lambda）当前不接 const-eval，仍按 E3104 拒。
        bool ok = false;
        if (auto cle = dynamic_cast<p<ExprLiteralNode>>(call->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(cle->literal())) {
                string fname = obj->getValue().getText();
                // 沿 scope 链向上找 FileNode → 查 #Const fn 符号
                p<Node> cur = e;
                FileNode* file = nullptr;
                while (cur) {
                    if (auto* fl = dynamic_cast<FileNode*>(cur)) { file = fl; break; }
                    cur = cur->parent();
                }
                if (file) {
                    FnSymbolInfo* fs = file->lookupFnSymbol(fname);
                    if (!fs) {
                        for (auto* imp : file->wildcardImports()) {
                            fs = imp->lookupFnSymbol(fname);
                            if (fs) break;
                        }
                    }
                    if (fs && fs->isConst) ok = true;
                }
            }
        }
        if (!ok) throwNonConst(e, "function / method call");
        for (auto& a : call->getArgs()) requireConstExpr(a, scope);
        return;
    }
    if (dynamic_cast<p<ExprDotNode>>(e))          throwNonConst(e, "member access");
    if (dynamic_cast<p<ExprGetNode>>(e))          throwNonConst(e, "array indexing");
    if (dynamic_cast<p<ExprGetRefNode>>(e))       throwNonConst(e, "address-of (&) expression");
    if (dynamic_cast<p<ExprArrayNode>>(e))        throwNonConst(e, "array literal");
    if (dynamic_cast<p<ExprArrayInitNode>>(e))    throwNonConst(e, "array fill expression");
    if (dynamic_cast<p<ExprTupleNode>>(e))        throwNonConst(e, "tuple construction");
    if (dynamic_cast<p<ExprIfElseNode>>(e))       throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprOneLineIfElseNode>>(e))   throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprIfElsePreValueNode>>(e))  throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprMatchNode>>(e))        throwNonConst(e, "match expression");
    if (dynamic_cast<p<ExprTryCatchNode>>(e))     throwNonConst(e, "try-catch expression");
    if (dynamic_cast<p<ExprPathCallNode>>(e))     throwNonConst(e, "enum constructor");
    if (dynamic_cast<p<ExprDynCtorNode>>(e))      throwNonConst(e, "Dyn<...> construction");
    if (dynamic_cast<p<ExprNullElseNode>>(e))     throwNonConst(e, "`??` expression");
    if (dynamic_cast<p<LambdaExprNode>>(e))       throwNonConst(e, "lambda expression");

    throwNonConst(e, "unsupported expression");
}

// ==================== §5 frozen helpers ====================
// `e` 是否直接引用了 frozen 符号（不展开 dot / get / call 等表达式）。
// 用于：
//   §5.3 — 拒收 `s.f = ...` / `s[i] = ...`（s 是 frozen）；
//   §5.4 — 拒收"可写槽位"承接 frozen 表达式（顶层 ID 形态）。
// 子表达式（如 `f(frozen)`）暂不深扫，等 callsite 校验上线再补。
bool isFrozenIdRef(p<ExprNode> e, p<ScopeNode> scope) {
    if (!e) return false;
    if (auto paren = dynamic_cast<p<ExprParenNode>>(e)) {
        return isFrozenIdRef(paren->expr(), scope);
    }
    if (auto le = dynamic_cast<p<ExprLiteralNode>>(e)) {
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
            auto sc = exprScope(e, scope);
            SymbolInfo* sym = sc ? sc->lookupSymbol(obj->getValue().getText()) : nullptr;
            return sym && sym->isFrozen;
        }
    }
    return false;
}

// `e` 是否是 `copy_of:<T>(arg)` 调用（§5.3 唯一脱 const 出口）。
bool isCopyOfCall(p<ExprNode> e) {
    auto call = dynamic_cast<p<ExprCallNode>>(e);
    if (!call) return false;
    auto callee = call->getCalleeExpr();
    if (auto le = dynamic_cast<p<ExprLiteralNode>>(callee)) {
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
            return obj->getValue().getText() == "copy_of";
        }
    }
    return false;
}

// §5.4：RHS 是否携带 frozen，且不是 copy_of 脱出。
// P1-3 简化版：仅识别顶层 frozen ID / paren 包裹的 frozen ID；
// 复杂表达式（含调用、运算、字段访问等）暂按"不携带" —— 留 P1-3-followup。
bool carriesFrozenTopLevel(p<ExprNode> e, p<ScopeNode> scope) {
    if (!e) return false;
    if (isCopyOfCall(e)) return false;
    return isFrozenIdRef(e, scope);
}

// 取 LHS（StatementAssignNode）的 SymbolInfo*；找不到返回 nullptr。
SymbolInfo* lookupLhsSym(p<StatementAssignNode> a, p<ScopeNode> scope) {
    auto sc = scope;
    if (!sc) sc = a->findNearestScope();
    return sc ? sc->lookupSymbol(a->obj().getText()) : nullptr;
}

// 解析 fn 所在的结构体上下文（用于 §6.2 字段写白名单）。
// - file:           enclosing FileNode，用于跨模块 getStructOwner 解析字段所属。
// - implStructName: 若 fn 是某 struct impl 的方法，记录 struct 名；否则空。
// - isDestructor:   fn 为该 struct impl 的 _destructor（fn ~()）。
// Phase 6D: 同名 ctor 已砍 (E3130)，不再需要 isConstructor 标记；
// `#Val/#Frozen` 字段的"构造期允许写入"语义转为"`#Static fn` 体内通过
// `Self { .f = v }` 字段字面量产出 `Self`" —— 字段字面量是表达式分支,
// 不走 StatementAssignNode 路径, 无需在此处放行。
struct FnContext {
    FileNode*       file = nullptr;
    StructImplNode* impl = nullptr;
    string          implStructName;
    bool            isDestructor  = false;
};

FnContext resolveFnContext(p<FnNode> fn) {
    FnContext c;
    p<Node> cur = fn->parent();
    while (cur) {
        if (auto* si = dynamic_cast<StructImplNode*>(cur)) {
            if (!c.impl) c.impl = si;
        } else if (auto* fl = dynamic_cast<FileNode*>(cur)) {
            c.file = fl;
            break;
        }
        cur = cur->parent();
    }
    if (c.impl) {
        c.implStructName = c.impl->structName();
        if (c.impl->hasDestructor() && c.impl->destructor() == fn) {
            c.isDestructor = true;
        }
    }
    return c;
}

// 在 file 与其 wildcardImports 范围内查 typeName 对应的 StructDeclNode。
StructDeclNode* findStructDecl(FileNode* file, const string& typeName) {
    if (!file || typeName.empty()) return nullptr;
    if (auto* owner = file->getStructOwner(typeName)) {
        if (auto* sd = owner->getStructDecl(typeName)) return sd;
    }
    return file->getStructDecl(typeName);
}

// 遍历整棵 fn body，命中 cval 局部声明就走 §3.3 校验；
// §5.2 由 writeable=false 默认 E3093 兜底；§5.3 / §5.4 由本 walker 显式抛错；
// §6.2 字段写白名单（非构造函数禁写 #Val/#Frozen 字段）由本 walker 抛 E3109。
class ConstMutWalker {
public:
    void run(p<FnNode> fn) {
        _ctx = resolveFnContext(fn);
        _isConstFn = fn->header()->hasAnno("Const");
        _fnName = fn->header()->name().getText();
        for (auto& s : fn->body()) visitStmt(s);
    }

private:
    FnContext _ctx;
    bool _isConstFn = false;
    string _fnName;
    // §4.2：函数体内声明过的局部名集合（var/val/cval）；params / `$` / 全局不计入。
    // 写入操作的 lhs 命中此集合视为"写本地"，放行；命中外则按 §4.2 拒收。
    std::set<string> _localNames;

    // 在 #Const fn 体内对写操作做 §4.2 (1)/(2)/(3) 校验。
    // - subs.empty(): rebind 形态。lhs 是 param / $ 时按 yux 规则不可重赋（E3093 已兜底），
    //   这里只拦"全局变量重赋"——§4.2.3。
    // - subs.size()>=1: 字段写 / 数组写。lhs 是本地 → 允许；否则按 (1)/(2)/(3) 拒收。
    void checkConstFnWrite(p<StatementAssignNode> as, p<ScopeNode> scope) {
        if (!_isConstFn) return;
        string name = as->obj().getText();
        if (_localNames.contains(name)) return;
        auto sc = scope ? scope : as->findNearestScope();
        SymbolInfo* sym = sc ? sc->lookupSymbol(name) : nullptr;
        const char* what = nullptr;
        string detail;
        if (name == "$") {
            what = as->subs().empty() ? "rebind `$`" : "write field of `$`";
            detail = as->subs().empty() ? "$" : ("$." + as->subs().front().getText());
        } else if (sym && sym->kind == SymbolKind::Variable && !sym->isConst &&
                   !sym->moduleName.empty()) {
            // 全局变量（注册时带 moduleName）
            what = as->subs().empty() ? "rebind global variable" : "write field of global variable";
            detail = name + (as->subs().empty() ? "" : ("." + as->subs().front().getText()));
        } else {
            // 非本地、非 $、非已知全局 —— 视作参数字段写（params 注册无 moduleName）。
            // 只在有 subs 时拒（rebind 走 E3093）。
            if (as->subs().empty()) return;
            what = "write field of parameter";
            detail = name + "." + as->subs().front().getText();
        }
        throw YuxError(as->getLineNumber(), as->getColumn(),
                       ErrorCode::E3110, _fnName, what, detail);
    }

    void checkConstFnSet(p<StatementSetNode> st, p<ScopeNode> scope) {
        if (!_isConstFn) return;
        // arrayExpr 形态：单 ID 时按上面规则；复杂表达式（链式）按"写非本地"判定
        p<ExprNode> ae = st->arrayExpr();
        while (auto paren = dynamic_cast<p<ExprParenNode>>(ae)) ae = paren->expr();
        string name;
        if (auto le = dynamic_cast<p<ExprLiteralNode>>(ae)) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                name = obj->getValue().getText();
            }
        }
        if (!name.empty() && _localNames.contains(name)) return;
        auto sc = scope ? scope : st->findNearestScope();
        SymbolInfo* sym = (sc && !name.empty()) ? sc->lookupSymbol(name) : nullptr;
        const char* what;
        string detail;
        if (name == "$") {
            what = "write element of `$`";
            detail = "$[i]";
        } else if (sym && sym->kind == SymbolKind::Variable && !sym->isConst &&
                   !sym->moduleName.empty()) {
            what = "write element of global variable";
            detail = (name.empty() ? "<expr>" : name) + "[i]";
        } else {
            what = "write element of parameter";
            detail = (name.empty() ? "<expr>" : name) + "[i]";
        }
        throw YuxError(st->getLineNumber(), st->getColumn(),
                       ErrorCode::E3110, _fnName, what, detail);
    }

    // 取 fn 所在 file（沿 parent chain 找 FileNode），用于跨自由函数/方法符号查表。
    FileNode* fileOf(p<Node> n) const {
        p<Node> cur = n;
        while (cur) {
            if (auto* fl = dynamic_cast<FileNode*>(cur)) return fl;
            cur = cur->parent();
        }
        return _ctx.file;
    }

    // 在 file 自身 + wildcardImports 中按 fnName 找一个 FnSymbolInfo。
    FnSymbolInfo* lookupFnSymbolCrossFile(FileNode* file, const string& fnName) const {
        if (!file) return nullptr;
        if (auto* fs = file->lookupFnSymbol(fnName)) return fs;
        for (auto* imp : file->wildcardImports()) {
            if (auto* fs = imp->lookupFnSymbol(fnName)) return fs;
        }
        return nullptr;
    }

    // §4.2 (4)：识别 callee 的目标函数符号；非 #Const 时抛 E3111。
    // 仅识别可静态解析的两种形态：
    //   - 自由函数：callee 是 LiteralObjNode（裸标识符）。
    //   - 方法：callee 是 ExprDotNode，receiver 是变量字面量；按 receiver 类型查 `Type.method`。
    // 其他形态（链式 dot、调用结果再调用、enum ctor、lambda 等）暂按"未知" 放行。
    void checkConstFnCall(p<ExprCallNode> call) {
        if (!_isConstFn) return;
        auto callee = call->getCalleeExpr();
        auto scope = call->findNearestScope();
        FileNode* file = fileOf(call);

        // 自由函数形态
        if (auto le = dynamic_cast<p<ExprLiteralNode>>(callee)) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                string fname = obj->getValue().getText();
                // 若是本地符号引用（Rc/Array 等通过变量调用，不在此处覆盖），跳过
                if (_localNames.contains(fname)) return;
                if (scope) {
                    SymbolInfo* sym = scope->lookupSymbol(fname);
                    if (sym && sym->kind == SymbolKind::Variable) return; // 变量调用，非自由 fn
                }
                FnSymbolInfo* fs = lookupFnSymbolCrossFile(file, fname);
                if (!fs) return; // 内置 / 编译器合成（如 copy_of / panic / println 等），P1-5 不拦
                if (fs->isConst) return;
                throw YuxError(call->resolveLineNumber(), call->resolveColumn(),
                               ErrorCode::E3111, _fnName, fname);
            }
        }
        // 方法形态：receiver.method(...)
        if (auto dot = dynamic_cast<p<ExprDotNode>>(callee)) {
            // receiver 必须是简单变量字面量（包含 `$`），否则跳过
            auto base = dot->baseExpr();
            string recvName;
            if (auto le = dynamic_cast<p<ExprLiteralNode>>(base)) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                    recvName = obj->getValue().getText();
                }
            }
            if (recvName.empty() || !scope) return;
            SymbolInfo* sym = scope->lookupSymbol(recvName);
            if (!sym) return;
            string typeName = sym->type.isRef() && sym->type.refElementType()
                                ? sym->type.refElementType()->name
                                : sym->type.name;
            if (typeName.empty()) return;
            string fullName = typeName + "." + dot->member();
            FnSymbolInfo* fs = lookupFnSymbolCrossFile(file, fullName);
            if (!fs) return; // builtin 方法（Array/String 等 #Builtin）P1-5 不拦，待 SDK 内化
            if (fs->isConst) return;
            throw YuxError(call->resolveLineNumber(), call->resolveColumn(),
                           ErrorCode::E3111, _fnName, fullName);
        }
    }

    // 给 `obj.subs[0] = ...` 形态做 §6.2 + §6.3 字段写校验。
    // 规则：
    //   - subs.size() == 1：写的就是字段本身，#Val 或 #Frozen 一律拒（构造期外）。
    //   - subs.size() >  1：深写，#Frozen 拒（深传染），#Val 放行（浅）。
    // obj 的类型从 scope 取，跨模块走 getStructOwner。
    void checkFieldWrite(p<StatementAssignNode> as, p<ScopeNode> scope) {
        if (_ctx.isDestructor) return;            // 析构不受 const-mut 约束（§6.2 / [#1.L]）
        if (as->subs().empty()) return;
        auto sc = scope ? scope : as->findNearestScope();
        if (!sc) return;
        SymbolInfo* sym = sc->lookupSymbol(as->obj().getText());
        if (!sym) return;
        // `$` 类型为 Ref<StructName>，需要剥一层；其余 obj 直接读 type.name。
        string typeName = sym->type.isRef() && sym->type.refElementType()
                            ? sym->type.refElementType()->name
                            : sym->type.name;
        StructDeclNode* sd = findStructDecl(_ctx.file, typeName);
        if (!sd) return;
        const string& fieldName = as->subs().front().getText();
        const StructFieldNode* fd = sd->field(fieldName);
        if (!fd) return;
        bool deep = as->subs().size() > 1;
        bool reject = deep ? fd->isFrozen() : (fd->isVal() || fd->isFrozen());
        if (!reject) return;
        // Phase 6D: 同名 ctor 已砍, 不再有"构造函数体内放行" 路径;
        // `#Val/#Frozen` 字段的初始化只能走 `#Static fn` 体内 `Self { .f = v }`
        // 字段字面量 (属表达式分支, 不进 StatementAssignNode), 这里一律拒收。
        const char* tag = fd->isFrozen() ? "Frozen" : "Val";
        throw YuxError(as->getLineNumber(), as->getColumn(),
                       ErrorCode::E3109, fieldName, tag, sd->name().getText());
    }

    void visitBlock(p<StatementBlockNode> blk) {
        if (!blk) return;
        for (auto& s : blk->statements()) visitStmt(s);
        if (blk->hasResult() && blk->resultExpr()) visitExpr(blk->resultExpr());
    }

    // Phase 3 (DRAFT-const-eval §3): #Const fn body 控制流白名单。
    // 命中非白名单形态时抛 E3141；允许形态返回后落入现有 dispatch。
    // 允许：StatementRetNode / StatementRetVoidNode / `#Cval` StatementDeclareAssignNode /
    //       StatementDeclareNode(isConst) （后者罕见，留作 defensive）
    // 拒收：loop / break / 局部 mutate / 普通 expr-stmt / nested block / match / try-catch /
    //       非 #Cval 局部 let / 元组解构 let / array set
    void rejectIfDisallowedInConstFn(p<StatementNode> s) {
        if (!_isConstFn) return;
        auto throwE = [&](const char* what) {
            throw YuxError(s->getLineNumber(), s->getColumn(), ErrorCode::E3141, _fnName, what);
        };
        if (dynamic_cast<p<StatementLoopNode>>(s))               throwE("loop");
        if (dynamic_cast<p<StatementBreakNode>>(s))              throwE("break");
        // StatementAssignNode / StatementSetNode 写本地 cval = E3093 (compiler 端);
        // 写参数 / $ / 全局 = E3110 (checkConstFnWrite/Set). 不在此层抢报, 让既有错码生效.
        if (dynamic_cast<p<StatementAssignNode>>(s)) return;
        if (dynamic_cast<p<StatementSetNode>>(s))    return;
        if (auto da = dynamic_cast<p<StatementDeclareAssignNode>>(s)) {
            if (!da->isConst()) throwE("non-`#Cval` local `let`");
            return;
        }
        if (dynamic_cast<p<StatementDeclareAssignTupleNode>>(s)) throwE("tuple destructure `let`");
        if (auto dn = dynamic_cast<p<StatementDeclareNode>>(s)) {
            if (!dn->isConst()) throwE("uninitialized local `let`");
            return;
        }
        if (auto se = dynamic_cast<p<StatementExprNode>>(s)) {
            if (dynamic_cast<p<StatementRetNode>>(s)) return; // ret 允许
            p<ExprNode> e = se->expr();
            if (dynamic_cast<p<ExprMatchNode>>(e))    throwE("match expression");
            if (dynamic_cast<p<ExprTryCatchNode>>(e)) throwE("try-catch expression");
            throwE("expression statement");
        }
        if (dynamic_cast<p<StatementBlockNode>>(s))              throwE("nested block statement");
    }

    void visitStmt(p<StatementNode> s) {
        if (!s) return;

        rejectIfDisallowedInConstFn(s);

        if (auto blk = dynamic_cast<p<StatementBlockNode>>(s)) {
            visitBlock(blk);
            return;
        }
        if (auto loop = dynamic_cast<p<StatementLoopNode>>(s)) {
            visitBlock(loop->block());
            return;
        }
        if (auto dn = dynamic_cast<p<StatementDeclareNode>>(s)) {
            _localNames.insert(dn->name().getText());
            return;
        }
        if (auto da = dynamic_cast<p<StatementDeclareAssignNode>>(s)) {
            _localNames.insert(da->name().getText());
            auto scope = s->findNearestScope();
            if (da->isConst() && da->expr()) {
                requireConstExpr(da->expr(), scope);
            } else if (da->expr() && carriesFrozenTopLevel(da->expr(), scope)) {
                // §5.4：可写局部绑定不可承接 #Frozen 表达式，唯一脱 const 出口是 copy_of。
                int line = s->getLineNumber();
                int col = s->getColumn();
                // RHS 一定是 frozen ID（顶层），取名字塞进消息。
                p<ExprNode> e = da->expr();
                while (auto paren = dynamic_cast<p<ExprParenNode>>(e)) e = paren->expr();
                string srcName = "<frozen>";
                if (auto le = dynamic_cast<p<ExprLiteralNode>>(e)) {
                    if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                        srcName = obj->getValue().getText();
                    }
                }
                throw YuxError(line, col, ErrorCode::E3107, srcName, da->name().getText());
            }
            if (da->expr()) visitExpr(da->expr());
            return;
        }
        if (auto as = dynamic_cast<p<StatementAssignNode>>(s)) {
            auto scope = s->findNearestScope();
            if (!as->subs().empty()) {
                // §5.3：`obj.f.g = rhs` 或 `obj.f = rhs`；obj 为 frozen 时拒收。
                SymbolInfo* sym = lookupLhsSym(as, scope);
                if (sym && sym->isFrozen) {
                    int line = s->getLineNumber();
                    int col = s->getColumn();
                    throw YuxError(line, col, ErrorCode::E3106,
                                   as->subs().front().getText(), as->obj().getText());
                }
                // §6.2 / §6.3：字段层 #Val/#Frozen 在构造期外禁写。
                checkFieldWrite(as, scope);
                // §4.2 (1)/(2)/(3)：#Const fn 体内禁写 $/参数/全局字段。
                checkConstFnWrite(as, scope);
            } else {
                // §4.2 (3)：#Const fn 体内禁重赋全局变量。
                checkConstFnWrite(as, scope);
                // §5.4：可写槽位重赋承接 #Frozen 表达式 → 拒收；
                // §5.2 frozen 自身被重赋 → 走 writeable=false / E3093（compiler 端）。
                SymbolInfo* sym = lookupLhsSym(as, scope);
                bool lhsFrozen = sym && sym->isFrozen;
                if (!lhsFrozen && as->expr() && carriesFrozenTopLevel(as->expr(), scope)) {
                    int line = s->getLineNumber();
                    int col = s->getColumn();
                    p<ExprNode> e = as->expr();
                    while (auto paren = dynamic_cast<p<ExprParenNode>>(e)) e = paren->expr();
                    string srcName = "<frozen>";
                    if (auto le = dynamic_cast<p<ExprLiteralNode>>(e)) {
                        if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                            srcName = obj->getValue().getText();
                        }
                    }
                    throw YuxError(line, col, ErrorCode::E3107, srcName, as->obj().getText());
                }
            }
            if (as->expr()) visitExpr(as->expr());
            return;
        }
        if (auto sfs = dynamic_cast<p<StatementStaticFieldSetNode>>(s)) {
            // DRAFT-static-vars Phase 5：静态字段写 Type::FIELD = expr
            // §4.2：#Const fn 体内禁写静态字段
            if (_isConstFn) {
                throw YuxError(s->getLineNumber(), s->getColumn(), ErrorCode::E3110,
                               _fnName, "write static field",
                               sfs->typeName().getText() + "::" + sfs->fieldName().getText());
            }
            visitExpr(sfs->valueExpr());
            return;
        }
        if (auto st = dynamic_cast<p<StatementSetNode>>(s)) {
            // §5.3：`s[i] = X`；arrayExpr 是 frozen ID 时拒收。
            auto scope = s->findNearestScope();
            if (isFrozenIdRef(st->arrayExpr(), scope)) {
                int line = s->getLineNumber();
                int col = s->getColumn();
                string objName = "<expr>";
                p<ExprNode> ae = st->arrayExpr();
                while (auto paren = dynamic_cast<p<ExprParenNode>>(ae)) ae = paren->expr();
                if (auto le = dynamic_cast<p<ExprLiteralNode>>(ae)) {
                    if (auto obj = dynamic_cast<p<LiteralObjNode>>(le->literal())) {
                        objName = obj->getValue().getText();
                    }
                }
                throw YuxError(line, col, ErrorCode::E3106, "[i]", objName);
            }
            // §4.2：#Const fn 内禁写 $/参数/全局的数组槽。
            checkConstFnSet(st, scope);
            visitExpr(st->arrayExpr());
            for (auto& i : st->indices()) visitExpr(i);
            visitExpr(st->valueExpr());
            return;
        }
        if (auto se = dynamic_cast<p<StatementExprNode>>(s)) {
            if (se->expr()) visitExpr(se->expr());
            return;
        }
        // 其他 stmt 类型（Declare 无 init / Break / Ret / RetVoid）
        // 没有需要检查的 cval / frozen 路径，掠过。
    }

    // 表达式遍历仅深入可能嵌套语句块的结构，便于覆盖 if-else / lambda / 调用实参 / 块表达式
    // 中的 cval 声明。
    void visitExpr(p<ExprNode> e) {
        if (!e) return;
        if (auto ie = dynamic_cast<p<ExprIfElseNode>>(e)) {
            visitExpr(ie->condition());
            visitBlock(ie->thenBlock());
            for (auto& el : ie->elifs()) {
                if (!el) continue;
                visitExpr(el->condition());
                visitBlock(el->block());
            }
            visitBlock(ie->elseBlock());
            return;
        }
        if (auto pe = dynamic_cast<p<ExprIfElsePreValueNode>>(e)) {
            visitExpr(pe->condition());
            visitExpr(pe->trueValue());
            visitExpr(pe->falseValue());
            return;
        }
        if (auto ol = dynamic_cast<p<ExprOneLineIfElseNode>>(e)) {
            visitExpr(ol->condition());
            visitExpr(ol->trueValue());
            visitExpr(ol->falseValue());
            return;
        }
        if (auto call = dynamic_cast<p<ExprCallNode>>(e)) {
            checkConstFnCall(call);
            visitExpr(call->getCalleeExpr());
            for (auto& a : call->getArgs()) visitExpr(a);
            return;
        }
        if (auto lam = dynamic_cast<p<LambdaExprNode>>(e)) {
            if (lam->bodyExpr()) visitExpr(lam->bodyExpr());
            for (auto& s : lam->bodyStmts()) visitStmt(s);
            return;
        }
        if (auto m = dynamic_cast<p<ExprMatchNode>>(e)) {
            visitExpr(m->scrutinee());
            for (auto& arm : m->arms()) {
                if (arm) visitExpr(arm->body());
            }
            return;
        }
        if (auto tc = dynamic_cast<p<ExprTryCatchNode>>(e)) {
            visitBlock(tc->tryBlock());
            for (auto& c : tc->catches()) {
                if (c) visitBlock(c->body());
            }
            return;
        }
        // 其余 expr 不承载嵌套 stmt 块。
    }
};

} // anon namespace

void checkConstMut(p<FnNode> fn) {
    ConstMutWalker w;
    w.run(fn);
}

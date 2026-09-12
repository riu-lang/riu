// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "const_eval.h"

#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "call_resolve.h"
#include "error_code.h"
#include "name_resolver.h"

// DRAFT-const-eval Phase 1. 详 const_eval.h 注释。本文件只负责"叶节点 + 算术"
// 求值, 不接 ast_builder, 不抛错码。

namespace {

// ---- 类型工具 -------------------------------------------------------------

bool isSignedIntType(const TypeInfo& t) {
    return t.name == "i8" || t.name == "i16" || t.name == "i32" || t.name == "i64" || t.name == "isize";
}

bool isUnsignedIntType(const TypeInfo& t) {
    return t.isUnsigned();
}

bool isIntType(const TypeInfo& t) {
    return isSignedIntType(t) || isUnsignedIntType(t);
}

bool isFloatType(const TypeInfo& t) {
    return t.isFloat();
}

int intBitWidth(const TypeInfo& t) {
    if (t.name == "i8" || t.name == "u8") return 8;
    if (t.name == "i16" || t.name == "u16") return 16;
    if (t.name == "i32" || t.name == "u32") return 32;
    if (t.name == "i64" || t.name == "u64") return 64;
    // TODO: 跨平台编译时需根据目标指针宽度动态返回
    if (t.name == "isize" || t.name == "usize") return static_cast<int>(sizeof(void*) * 8);
    return 0;
}

u64 intMask(int width) {
    if (width >= 64) return ~static_cast<u64>(0);
    return (static_cast<u64>(1) << static_cast<unsigned>(width)) - 1;
}

// 按 type 把 bits 解释为有符号 i64（符号扩展）。
i64 signExtend(u64 bits, const TypeInfo& t) {
    int w = intBitWidth(t);
    if (w >= 64) return static_cast<i64>(bits);
    u64 signBit = static_cast<u64>(1) << static_cast<unsigned>(w - 1);
    if (bits & signBit) {
        // 设置高位 1
        return static_cast<i64>(bits | ~intMask(w));
    }
    return static_cast<i64>(bits);
}

// 截断 / 规整 bits 到 type 宽度。
u64 truncateBits(u64 bits, const TypeInfo& t) {
    return bits & intMask(intBitWidth(t));
}

// 二元算术 / 位运算的类型合一规则（Phase 1 极简）：
// - 两侧同名 → 取该名
// - 一侧 i*, 一侧 u* → 失败（要求显式同名, 与既有 compile 期约束对齐, 避免隐式签名扩）
// - 浮点类似
// - 否则失败
std::optional<TypeInfo> unifyArith(const TypeInfo& a, const TypeInfo& b) {
    if (a.name == b.name && !a.name.empty()) return a;
    return std::nullopt;
}

} // namespace

void ConstEvaluator::setNamedConst(const string& name, ConstantValue value) {
    _env[name] = std::move(value);
}

std::optional<ConstantValue> ConstEvaluator::eval(ExprNode* expr) {
    if (!expr) return std::nullopt;

    // 叶 ——
    if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
        return evalLiteral(lit->literal());
    }
    // ExprParenNode —— 透明
    if (auto paren = dynamic_cast<ExprParenNode*>(expr)) {
        return eval(paren->expr());
    }
    // 一元
    if (auto u = dynamic_cast<ExprUnaryNode*>(expr)) {
        return evalUnary(u);
    }
    // 二元算术
    if (auto a = dynamic_cast<ExprAddSubNode*>(expr)) {
        return evalAddSub(a);
    }
    if (auto m = dynamic_cast<ExprMulDivModNode*>(expr)) {
        return evalMulDivMod(m);
    }
    if (auto b = dynamic_cast<ExprBinOpNode*>(expr)) {
        return evalBinOp(b);
    }
    if (auto c = dynamic_cast<ExprCompareNode*>(expr)) {
        return evalCompare(c);
    }
    // Phase 4: #Const fn 调用；整数位方法（and/or/xor/shl/shr/inv）先于自由 fn
    if (auto call = dynamic_cast<ExprCallNode*>(expr)) {
        if (auto bit = evalIntBitMethod(call)) return bit;
        return evalCall(call);
    }
    // Phase 5: struct 字面量 (Self{...} 与 TypeName{...} 同走)
    if (auto sl = dynamic_cast<ExprStructLitNode*>(expr)) {
        return evalStructLit(sl);
    }

    // 其它节点（if-else / dot / get / ...）—— Phase 6+ 不支持
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalLiteral(LiteralNode* lit) {
    if (!lit) return std::nullopt;

    if (auto i = dynamic_cast<LiteralIntNode*>(lit)) {
        // E3103 越界必须上抛（推断 / 后缀 / 声明类型）。其余非法形式仍 nullopt。
        i64 v = sema::parseIntLiteral(i->getValue().getText(), i->getLineNumber(), 0, i->getType().name);
        return ConstantValue::makeInt(truncateBits(static_cast<u64>(v), i->getType()), i->getType());
    }
    if (auto f = dynamic_cast<LiteralFloatNode*>(lit)) {
        // [#4.8.A] host double 简化求值
        string s = f->getValue().getText();
        if (s.size() >= 3) {
            string suf = s.substr(s.size() - 3);
            if (suf == "f32" || suf == "f64") s = s.substr(0, s.size() - 3);
        }
        try {
            f64 v = std::stod(s);
            if (f->getType().name == "f32") v = static_cast<f32>(v);
            return ConstantValue::makeFloat(v, f->getType());
        } catch (...) {
            return std::nullopt;
        }
    }
    if (auto bo = dynamic_cast<LiteralBoolNode*>(lit)) {
        return ConstantValue::makeBool(bo->getValue().getText() == "true");
    }
    if (dynamic_cast<LiteralNullNode*>(lit)) {
        return ConstantValue::makeNull();
    }
    if (auto obj = dynamic_cast<LiteralObjNode*>(lit)) {
        return evalLiteralObj(obj);
    }
    // LiteralCodePoint / LiteralString / StringTemplate —— Phase 1 不支持
    // (String 长期解见 DRAFT §4.6)
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalLiteralObj(LiteralObjNode* obj) {
    string name = obj->getValue().getText();
    if (auto it = _env.find(name); it != _env.end()) {
        return it->second;
    }
    // Phase 1 standalone: 不接 GlobalConstNode 表。Phase 2 接 visitLetGlobal 时
    // caller 负责把 #Cval 全局先注入 _env 再 eval。
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalUnary(ExprUnaryNode* node) {
    if (node->op() == ExprUnaryNode::Op::Neg) {
        ExprNode* r = node->right();
        while (auto* paren = dynamic_cast<ExprParenNode*>(r))
            r = paren->expr();
        if (auto* litExpr = dynamic_cast<ExprLiteralNode*>(r)) {
            if (auto* i = dynamic_cast<LiteralIntNode*>(litExpr->literal())) {
                string text = i->getValue().getText();
                if (text.empty() || (text[0] != '-' && text[0] != '+'))
                    text = "-" + text;
                else if (text[0] == '+')
                    text = "-" + text.substr(1);
                i64 v = sema::parseIntLiteral(text, i->getLineNumber(), 0, i->getType().name);
                return ConstantValue::makeInt(truncateBits(static_cast<u64>(v), i->getType()), i->getType());
            }
        }
    }
    auto inner = eval(node->right());
    if (!inner) return std::nullopt;

    switch (node->op()) {
    case ExprUnaryNode::Op::Neg: {
        if (inner->isInt()) {
            // -x: 取补码后截断
            u64 bits = (~inner->intBits + 1) & intMask(intBitWidth(inner->type));
            return ConstantValue::makeInt(bits, inner->type);
        }
        if (inner->isFloat()) return ConstantValue::makeFloat(-inner->floatVal, inner->type);
        return std::nullopt;
    }
    case ExprUnaryNode::Op::Rev: {
        // 按位取反 ~x
        if (inner->isInt()) {
            u64 bits = (~inner->intBits) & intMask(intBitWidth(inner->type));
            return ConstantValue::makeInt(bits, inner->type);
        }
        return std::nullopt;
    }
    case ExprUnaryNode::Op::Not: {
        // 逻辑非 !x
        if (inner->isBool()) return ConstantValue::makeBool(!inner->boolVal);
        return std::nullopt;
    }
    }
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalAddSub(ExprAddSubNode* node) {
    auto l = eval(node->left());
    auto r = eval(node->right());
    if (!l || !r) return std::nullopt;
    auto t = unifyArith(l->type, r->type);
    if (!t) return std::nullopt;

    if (isIntType(*t)) {
        u64 lb = l->intBits, rb = r->intBits;
        u64 res = node->op() == ExprAddSubNode::Op::Add ? (lb + rb) : (lb - rb);
        // Phase 1 不抛 E3143; 仅截断。溢出检测留 Phase 2 接入时按 §4.7 决议加。
        return ConstantValue::makeInt(truncateBits(res, *t), *t);
    }
    if (isFloatType(*t)) {
        f64 res = node->op() == ExprAddSubNode::Op::Add ? (l->floatVal + r->floatVal) : (l->floatVal - r->floatVal);
        if (t->name == "f32") res = static_cast<f32>(res);
        return ConstantValue::makeFloat(res, *t);
    }
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalMulDivMod(ExprMulDivModNode* node) {
    auto l = eval(node->left());
    auto r = eval(node->right());
    if (!l || !r) return std::nullopt;
    auto t = unifyArith(l->type, r->type);
    if (!t) return std::nullopt;

    if (isIntType(*t)) {
        u64 res = 0;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul:
            res = l->intBits * r->intBits;
            break;
        case ExprMulDivModNode::Op::Div:
            if (r->intBits == 0) return std::nullopt; // 除 0 —— Phase 2 接错码 E3143
            if (isSignedIntType(*t)) {
                i64 a = signExtend(l->intBits, *t);
                i64 b = signExtend(r->intBits, *t);
                res = static_cast<u64>(a / b);
            } else {
                res = l->intBits / r->intBits;
            }
            break;
        case ExprMulDivModNode::Op::Mod:
            if (r->intBits == 0) return std::nullopt;
            if (isSignedIntType(*t)) {
                i64 a = signExtend(l->intBits, *t);
                i64 b = signExtend(r->intBits, *t);
                res = static_cast<u64>(a % b);
            } else {
                res = l->intBits % r->intBits;
            }
            break;
        }
        return ConstantValue::makeInt(truncateBits(res, *t), *t);
    }
    if (isFloatType(*t)) {
        f64 res = 0;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul:
            res = l->floatVal * r->floatVal;
            break;
        case ExprMulDivModNode::Op::Div:
            res = l->floatVal / r->floatVal;
            break;
        case ExprMulDivModNode::Op::Mod:
            return std::nullopt; // 浮点 % 非常量友好
        }
        if (t->name == "f32") res = static_cast<f32>(res);
        return ConstantValue::makeFloat(res, *t);
    }
    return std::nullopt;
}

std::optional<ConstantValue> ConstEvaluator::evalBinOp(ExprBinOpNode* node) {
    auto l = eval(node->left());
    auto r = eval(node->right());
    if (!l || !r) return std::nullopt;

    // 移位允许 RHS 为任意整型, LHS 决定结果类型
    if (node->op() == ExprBinOpNode::Op::Shl || node->op() == ExprBinOpNode::Op::Shr) {
        if (!l->isInt() || !r->isInt()) return std::nullopt;
        u64 shift = r->intBits & 0x3Fu; // 截 6 bit
        u64 res = 0;
        if (node->op() == ExprBinOpNode::Op::Shl) {
            res = l->intBits << shift;
        } else {
            // 右移：unsigned → 逻辑右移; signed → 算术右移
            if (isSignedIntType(l->type)) {
                i64 v = signExtend(l->intBits, l->type);
                res =
                    static_cast<u64>(static_cast<i64>(v) >> static_cast<int>(shift)); // NOLINT(bugprone-signed-bitwise)
            } else {
                res = l->intBits >> shift;
            }
        }
        return ConstantValue::makeInt(truncateBits(res, l->type), l->type);
    }

    auto t = unifyArith(l->type, r->type);
    if (!t || !isIntType(*t)) return std::nullopt;

    u64 res = 0;
    switch (node->op()) {
    case ExprBinOpNode::Op::And:
        res = l->intBits & r->intBits;
        break;
    case ExprBinOpNode::Op::Or:
        res = l->intBits | r->intBits;
        break;
    case ExprBinOpNode::Op::Xor:
        res = l->intBits ^ r->intBits;
        break;
    default:
        return std::nullopt;
    }
    return ConstantValue::makeInt(truncateBits(res, *t), *t);
}

std::optional<ConstantValue> ConstEvaluator::evalIntBitMethod(ExprCallNode* call) {
    if (!call) return std::nullopt;
    auto* dot = dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
    if (!dot || dot->isSafe()) return std::nullopt;
    const string m = dot->member();
    auto recv = eval(dot->baseExpr());
    if (!recv || !recv->isInt()) return std::nullopt;

    if (m == "inv") {
        if (!call->getArgs().empty()) return std::nullopt;
        u64 bits = (~recv->intBits) & intMask(intBitWidth(recv->type));
        return ConstantValue::makeInt(bits, recv->type);
    }

    if (m != "and" && m != "or" && m != "xor" && m != "shl" && m != "shr") return std::nullopt;
    if (call->getArgs().size() != 1) return std::nullopt;
    auto rhs = eval(call->getArgs()[0]);
    if (!rhs || !rhs->isInt()) return std::nullopt;

    if (m == "shl" || m == "shr") {
        u64 shift = rhs->intBits & 0x3Fu;
        u64 res = 0;
        if (m == "shl") {
            res = recv->intBits << shift;
        } else if (isSignedIntType(recv->type)) {
            i64 v = signExtend(recv->intBits, recv->type);
            res = static_cast<u64>(static_cast<i64>(v) >> static_cast<int>(shift)); // NOLINT(bugprone-signed-bitwise)
        } else {
            res = recv->intBits >> shift;
        }
        return ConstantValue::makeInt(truncateBits(res, recv->type), recv->type);
    }

    auto t = unifyArith(recv->type, rhs->type);
    if (!t || !isIntType(*t)) return std::nullopt;
    u64 res = 0;
    if (m == "and")
        res = recv->intBits & rhs->intBits;
    else if (m == "or")
        res = recv->intBits | rhs->intBits;
    else
        res = recv->intBits ^ rhs->intBits;
    return ConstantValue::makeInt(truncateBits(res, *t), *t);
}

// Phase 4: #Const fn 调用求值。
// - callee 必须是裸 LiteralObj 形态（自由函数；方法/路径调用 Phase 5+）。
// - 通过 _file 解析 FnNode；callee 必须有 #Const 注解，否则 nullopt → caller 抛 E3140。
// - 形参 / 返回类型必须在白名单（标量整 / 浮点 / bool），违反抛 E3144。
// - 递归调用同名 fn 即返 nullopt（不含递归 #Const fn，§范围口径）。
// - body 仅识别 #Cval StatementDeclareAssignNode + StatementRetNode；其它统一 nullopt。
//   （此约束由 Phase 3 E3141 在 fn 定义点保障）。
// - 参数 / 局部绑定注入 _env，调用结束完整恢复。
std::optional<ConstantValue> ConstEvaluator::evalCall(ExprCallNode* call) {
    if (!_file || !call) return std::nullopt;

    // callee 形态：仅 LiteralObj（裸自由 fn 名）
    auto le = dynamic_cast<ExprLiteralNode*>(call->getCalleeExpr());
    if (!le) return std::nullopt;
    auto obj = dynamic_cast<LiteralObjNode*>(le->literal());
    if (!obj) return std::nullopt;
    string fname = obj->getValue().getText();

    // 递归保护
    if (_callStack.contains(fname)) return std::nullopt;

    FnNode* fn = _file->getFunction(fname);
    if (!fn) return std::nullopt;
    auto header = fn->header();
    if (!header || !header->hasAnno("Const")) return std::nullopt;

    // 类型白名单
    auto isWhitelisted = [](const TypeInfo& t) {
        return t.name == "bool" || t.name == "i8" || t.name == "i16" || t.name == "i32" || t.name == "i64" ||
               t.name == "u8" || t.name == "u16" || t.name == "u32" || t.name == "u64" || t.name == "f32" ||
               t.name == "f64" || t.name == "isize" || t.name == "usize";
    };
    TypeInfo retT = header->retType() ? header->retType()->getType() : TypeInfo();
    if (!isWhitelisted(retT)) {
        throw YuxError(call->resolveLineNumber(), call->resolveColumn(), ErrorCode::E3144, fname, "return", retT.name);
    }
    auto params = header->params();
    for (auto& pn : params) {
        TypeInfo pt = pn->type() ? pn->type()->getType() : TypeInfo();
        if (!isWhitelisted(pt)) {
            throw YuxError(call->resolveLineNumber(), call->resolveColumn(), ErrorCode::E3144, fname, "parameter",
                           pt.name);
        }
    }

    // arity 校验：args 数必须与 params 数一致（Phase 4 不接默认参 / variadic）
    auto& args = call->getArgs();
    if (args.size() != params.size()) return std::nullopt;

    // arg 求值 + 类型对齐到形参类型（按 truncate / cast 走整数；浮点不强制转换）
    vector<ConstantValue> argVals;
    argVals.reserve(args.size());
    for (auto& a : args) {
        auto v = eval(a);
        if (!v) return std::nullopt;
        argVals.push_back(*v);
    }

    // 保护当前 _env：snapshot 仅被覆盖 / 新增的 key
    std::map<string, std::optional<ConstantValue>> savedEnv;
    auto saveAndSet = [&](const string& name, ConstantValue val) {
        auto it = _env.find(name);
        savedEnv.emplace(name, it != _env.end() ? std::optional<ConstantValue>(it->second) : std::nullopt);
        _env[name] = std::move(val);
    };
    auto restoreEnv = [&]() {
        for (auto& [n, prev] : savedEnv) {
            if (prev)
                _env[n] = *prev;
            else
                _env.erase(n);
        }
    };

    for (size_t i = 0; i < params.size(); ++i) {
        saveAndSet(params[i]->name().getText(), argVals[i]);
    }
    _callStack.insert(fname);

    std::optional<ConstantValue> result;
    for (auto& s : fn->body()) {
        if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
            if (!da->isConst()) {
                result.reset();
                break;
            }
            auto v = eval(da->expr());
            if (!v) {
                result.reset();
                break;
            }
            saveAndSet(da->name().getText(), *v);
            continue;
        }
        if (auto ret = dynamic_cast<StatementRetNode*>(s)) {
            result = eval(ret->expr());
            break;
        }
        // 其它形态 —— Phase 3 已禁；保险起见走 nullopt
        result.reset();
        break;
    }

    _callStack.erase(fname);
    restoreEnv();
    return result;
}

// DRAFT-const-eval Phase 5: struct 字面量求值.
// - 找到 StructDeclNode (本文件 / sdk 兜底); 失败 nullopt (sema 应已拦截)
// - 按声明序填字段 -> ConstantValue::Struct; sema 已保证字段全列 / 无重复 / 无未知,
//   这里再做一次字段名 → 索引映射 (保险)
// - 任一字段子表达式 const 求值失败 -> 整体 nullopt
std::optional<ConstantValue> ConstEvaluator::evalStructLit(ExprStructLitNode* node) {
    if (!node || !_file) return std::nullopt;
    auto r = sema::resolveExprTypeLhs(_file, nullptr, node->isSelfForm() ? TypePath() : node->typePath(),
                                      node->getLineNumber(), node->getColumn());
    StructDeclNode* decl = nullptr;
    TypeInfo sTy;
    if (node->isSelfForm()) {
        const string& sName = node->structName();
        if (sName.empty()) return std::nullopt;
        sTy = TypeInfo(sName);
        sTy.ownerModule = _file->moduleName();
        decl = _file->getStructDecl(sName);
    } else {
        sTy = r.type;
        decl = r.structDecl;
        if (!decl) decl = sema::NameResolver(_file, nullptr).lookupStruct(sTy);
    }
    if (!decl) return std::nullopt;

    const auto& declFields = decl->fields();
    vector<ConstantValue> vals;
    vals.resize(declFields.size());
    vector<bool> filled(declFields.size(), false);

    if (node->positional()) {
        if (declFields.size() != 1) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3129, sTy.name, sTy.name,
                           std::to_string(declFields.size()));
        }
        auto v = eval(node->positional());
        if (!v) return std::nullopt;
        vals[0] = std::move(*v);
        return ConstantValue::makeStruct(std::move(vals), sTy);
    }

    for (auto& fi : node->fields()) {
        int idx = decl->fieldIndex(fi->name().getText());
        if (idx < 0) return std::nullopt;
        auto v = eval(fi->value());
        if (!v) return std::nullopt;
        vals[static_cast<size_t>(idx)] = std::move(*v);
        filled[static_cast<size_t>(idx)] = true;
    }
    for (bool f : filled) {
        if (!f) return std::nullopt;
    }
    return ConstantValue::makeStruct(std::move(vals), sTy);
}

std::optional<ConstantValue> ConstEvaluator::evalCompare(ExprCompareNode* node) {
    using Op = ExprCompareNode::Op;
    Op op = node->op();

    // 短路 AndAnd / OrOr —— 仅 bool
    if (op == Op::AndAnd || op == Op::OrOr) {
        auto l = eval(node->left());
        if (!l || !l->isBool()) return std::nullopt;
        // 短路
        if (op == Op::AndAnd && !l->boolVal) return ConstantValue::makeBool(false);
        if (op == Op::OrOr && l->boolVal) return ConstantValue::makeBool(true);
        auto r = eval(node->right());
        if (!r || !r->isBool()) return std::nullopt;
        return ConstantValue::makeBool(r->boolVal);
    }

    auto l = eval(node->left());
    auto r = eval(node->right());
    if (!l || !r) return std::nullopt;

    // 类型对齐: 双 null → 仅 Eq/Ne 有意义
    if (l->isNull() && r->isNull()) {
        if (op == Op::Eq) return ConstantValue::makeBool(true);
        if (op == Op::Ne) return ConstantValue::makeBool(false);
        return std::nullopt;
    }
    if (l->isBool() && r->isBool()) {
        if (op == Op::Eq) return ConstantValue::makeBool(l->boolVal == r->boolVal);
        if (op == Op::Ne) return ConstantValue::makeBool(l->boolVal != r->boolVal);
        return std::nullopt;
    }

    auto t = unifyArith(l->type, r->type);
    if (!t) return std::nullopt;

    if (isIntType(*t)) {
        if (isSignedIntType(*t)) {
            i64 a = signExtend(l->intBits, *t);
            i64 b = signExtend(r->intBits, *t);
            switch (op) {
            case Op::Eq:
                return ConstantValue::makeBool(a == b);
            case Op::Ne:
                return ConstantValue::makeBool(a != b);
            case Op::Lt:
                return ConstantValue::makeBool(a < b);
            case Op::Le:
                return ConstantValue::makeBool(a <= b);
            case Op::Gt:
                return ConstantValue::makeBool(a > b);
            case Op::Ge:
                return ConstantValue::makeBool(a >= b);
            default:
                return std::nullopt;
            }
        }
        u64 a = l->intBits, b = r->intBits;
        switch (op) {
        case Op::Eq:
            return ConstantValue::makeBool(a == b);
        case Op::Ne:
            return ConstantValue::makeBool(a != b);
        case Op::Lt:
            return ConstantValue::makeBool(a < b);
        case Op::Le:
            return ConstantValue::makeBool(a <= b);
        case Op::Gt:
            return ConstantValue::makeBool(a > b);
        case Op::Ge:
            return ConstantValue::makeBool(a >= b);
        default:
            return std::nullopt;
        }
    }
    if (isFloatType(*t)) {
        f64 a = l->floatVal, b = r->floatVal;
        switch (op) {
        case Op::Eq:
            return ConstantValue::makeBool(a == b);
        case Op::Ne:
            return ConstantValue::makeBool(a != b);
        case Op::Lt:
            return ConstantValue::makeBool(a < b);
        case Op::Le:
            return ConstantValue::makeBool(a <= b);
        case Op::Gt:
            return ConstantValue::makeBool(a > b);
        case Op::Ge:
            return ConstantValue::makeBool(a >= b);
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

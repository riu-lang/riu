// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_EXPR_NODE_H
#define YUX_LANG_EXPR_NODE_H

#include "node.h"

#include <utility>
#include <vector>

#include "literal_node.h"

class StatementNode;
class TypeNode;
class StructDeclNode;

class ExprNode : public Node, public Typed {
protected:
    // Phase 2 Sema/Codegen 拆分：表达式经语义检查后的解析型类型槽位。
    // 与 getType() 并行：getType() 仍是各子类自行就地推导的"无副作用"查询，
    // 用于 ast 层任意时刻调用；resolvedType 是 SemaPass / compileExpr 走过后
    // 留下的"已确认"类型，供后续 pass（codegen / LSP）直接读取，避免重复计算。
    //
    // 当前过渡期：Compiler::compileExpr 在入口写入；行为与 getType() 等价。
    // 后续把推断从 codegen 抽到独立 SemaPass 时，此槽位由 SemaPass 写入，
    // codegen 改为读取（并在 debug 构建里 assert 与 getType() 一致）。
    //
    // 空 optional = 尚未解析（区别于 TypeInfo::empty() 表示的 void 类型）。
    std::optional<TypeInfo> _resolvedType;

    // Phase 2.3 Sema/Codegen 拆分：表达式解析到的符号（变量符号 / 函数符号 / 空）。
    // 详见 node.h ResolvedSymbol 注释。当前过渡期：仅在 compile<Foo>Expr 现场已经查到
    // 符号的位置写入（首批：compileLiteralExpr 的对象字面量分支），其余位置陆续接入；
    // 暂不强制 codegen 改读，2.4 才统一切换读路径。
    std::optional<ResolvedSymbol> _resolvedSymbol;

public:
    ExprNode(const p<Node>& parent) : Node(parent) {}

    void setResolvedType(TypeInfo t) { _resolvedType = std::move(t); }
    [[nodiscard]] bool hasResolvedType() const { return _resolvedType.has_value(); }
    [[nodiscard]] const TypeInfo& resolvedType() const { return *_resolvedType; }
    // 块值汇合（if / match / try）优先读 SemaPass 靶向后的 resolved，避免 `[]` 的
    // getType `[__empty * 0]` 与 `Array<T>` 假阳性失配。
    [[nodiscard]] TypeInfo resolvedOrGetType() const { return hasResolvedType() ? resolvedType() : getType(); }

    void setResolvedSymbol(ResolvedSymbol s) { _resolvedSymbol = s; }
    void setResolvedVar(SymbolInfo* v) { _resolvedSymbol = ResolvedSymbol{.var = v, .fn = nullptr}; }
    void setResolvedFn(FnSymbolInfo* f) { _resolvedSymbol = ResolvedSymbol{.var = nullptr, .fn = f}; }
    [[nodiscard]] bool hasResolvedSymbol() const { return _resolvedSymbol.has_value(); }
    [[nodiscard]] const ResolvedSymbol& resolvedSymbol() const { return *_resolvedSymbol; }
};

// 如果表达式是无后缀的整数字面量且其类型可以推断，则返回 true。
// "灵活"子树只包含无类型后缀的整数字面量、括号、一元运算符和二元运算符。
bool isFlexibleIntExpr(p<ExprNode> expr);

// 尝试将子树中所有无类型后缀的整数字面量的类型设置为 `target`。
// 如果成功则返回 true（子树与 target 兼容——既可以是灵活类型并被传播，
// 也可以是其类型已经等于 target）。
bool tryInferIntType(p<ExprNode> expr, const TypeInfo& target);

// 如果表达式是 null 字面量（或其简单包装，如括号），返回 true。
// "灵活 null"：可以在上下文中推断为任意 Nullable<T>。
bool isFlexibleNullExpr(p<ExprNode> expr);

// 尝试将 null 字面量的类型设置为目标 Nullable<T>。
// 成功返回 true（target 确实是 Nullable<T> 且 expr 是灵活 null）；
// 失败返回 false。
bool tryInferNullType(p<ExprNode> expr, const TypeInfo& nullableTarget);

inline bool isIntTypeName(const string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" || n == "u32" || n == "u64" ||
           n == "isize" || n == "usize";
}

class ExprCallNode : public ExprNode {
protected:
    p<ExprNode> _calleeExpr;
    vector<p<ExprNode>> _args;
    vector<p<TypeNode>> _typeArgs;
    // DRAFT-错误.md [#4.B]：调用点是否带后缀 `!`（错误传播）。
    // 由 ast_builder 从 g4 `errPropagate=SymbolExcl?` 槽位读入。Phase 10e 仅做语义校验
    // （E7001 / E7004 / E7006），实际错误通道路由 codegen 推 10f / 10g。
    bool _errPropagate = false;

public:
    ExprCallNode(const p<Node>& parent, p<ExprNode> callee) : ExprNode(parent), _calleeExpr(callee) {}

    void addArg(p<ExprNode> arg) { _args.push_back(arg); }

    void setTypeArgs(vector<p<TypeNode>> args) { _typeArgs = std::move(args); }
    [[nodiscard]] const vector<p<TypeNode>>& getTypeArgs() const { return _typeArgs; }

    void setErrPropagate(bool v) { _errPropagate = v; }
    [[nodiscard]] bool errPropagate() const { return _errPropagate; }

    [[nodiscard]] const p<ExprNode>& getCalleeExpr() const;
    [[nodiscard]] const std::vector<p<ExprNode>>& getArgs() const;

    [[nodiscard]] TypeInfo getType() const override;
};

class ExprLiteralNode : public ExprNode {
protected:
    p<LiteralNode> _literal;

public:
    explicit ExprLiteralNode(const p<Node>& parent, p<LiteralNode> literal) : ExprNode(parent), _literal(literal) {
        _line = literal->getLineNumber();
    }

    [[nodiscard]] const p<LiteralNode>& literal() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprAddSubNode : public ExprNode {
public:
    enum class Op : u8 { Add, Sub };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprAddSubNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _op(op), _left(left), _right(right) {}

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprMulDivModNode : public ExprNode {
public:
    enum class Op : u8 { Mul, Div, Mod };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprMulDivModNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _op(op), _left(left), _right(right) {}

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprBinOpNode : public ExprNode {
public:
    enum class Op : u8 { And, Or, Xor, Shl, Shr };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprBinOpNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _op(op), _left(left), _right(right) {}

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprParenNode : public ExprNode {
    p<ExprNode> _inner;

public:
    ExprParenNode(const p<Node>& parent, p<ExprNode> inner) : ExprNode(parent), _inner(inner) {}

    [[nodiscard]] const p<ExprNode>& expr() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprDotNode : public ExprNode {
protected:
    p<ExprNode> _baseExpr;
    Token _member;
    // 安全访问标志：true 表示 a?.b（base 为 Nullable<T>，空时整体取 null）
    bool _safe = false;
    // spec 默认体消歧后缀（§12.10 续节 / DRAFT-spec-disambig-at）：a.m@SpecA(...) 中的 SpecA；空表示无后缀
    string _specQualifier;
    // DRAFT-spec-reflect §6: Field.value 编译期解析结果
    mutable bool _reflectFieldResolved = false;
    mutable const StructDeclNode* _reflectStructDecl = nullptr;
    mutable int _reflectFieldIndex = -1;

public:
    ExprDotNode(const p<Node>& parent, p<ExprNode> baseExpr, Token member)
        : ExprNode(parent), _baseExpr(baseExpr), _member(std::move(member)) {}

    ExprDotNode(const p<Node>& parent, p<ExprNode> baseExpr, Token member, bool safe)
        : ExprNode(parent), _baseExpr(baseExpr), _member(std::move(member)), _safe(safe) {}

    ExprDotNode(const p<Node>& parent, p<ExprNode> baseExpr, Token member, bool safe, string specQualifier)
        : ExprNode(parent), _baseExpr(baseExpr), _member(std::move(member)), _safe(safe),
          _specQualifier(std::move(specQualifier)) {}

    [[nodiscard]] const p<ExprNode>& baseExpr() const;
    [[nodiscard]] string member() const;
    [[nodiscard]] bool isSafe() const { return _safe; }
    [[nodiscard]] const string& specQualifier() const { return _specQualifier; }
    [[nodiscard]] bool hasSpecQualifier() const { return !_specQualifier.empty(); }
    [[nodiscard]] TypeInfo getType() const override;
    // 点成员是否为结构体字段（非方法）。方法点 getType 也返回 TypeKind::Fn，
    // 但那只是调用结果编码；真正的 Fn 字段才走 fat-ptr 调用。
    [[nodiscard]] bool isFieldAccess() const;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;

    // DRAFT-spec-reflect §6: Field.value 编译期解析
    [[nodiscard]] bool isReflectFieldValue() const { return _reflectFieldResolved; }
    [[nodiscard]] const StructDeclNode* reflectStructDecl() const { return _reflectStructDecl; }
    [[nodiscard]] int reflectFieldIndex() const { return _reflectFieldIndex; }

    // 解析形如 `aliasLit.s1.s2...sN` 的 Dot 链。
    // 成功时：aliasName 置为根字面量；segments 按顺序存放 [s1, ..., sN]（不含 alias）。
    // 失败返回 false（链底不是字面量对象）。
    static bool parseChain(const ExprDotNode* top, string& aliasName, vector<string>& segments);
};

class ExprCompareNode : public ExprNode {
public:
    enum class Op : u8 { Eq, Ne, Lt, Le, Gt, Ge, AndAnd, OrOr };

protected:
    Op _op;
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprCompareNode(const p<Node>& parent, Op op, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _op(op), _left(left), _right(right) {}

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& left() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class StatementBlockNode : public ScopeNode {
    vector<p<StatementNode>> _statements;
    p<ExprNode> _resultExpr;
    bool _hasResult;

public:
    StatementBlockNode(const p<Node>& parent, vector<p<StatementNode>> statements, p<ExprNode> resultExpr,
                       bool hasResult);

    [[nodiscard]] const vector<p<StatementNode>>& statements() const;
    [[nodiscard]] const p<ExprNode>& resultExpr() const;
    [[nodiscard]] bool hasResult() const;
};

class ExprElIfNode : public Node {
    p<ExprNode> _condition;
    p<StatementBlockNode> _block;

public:
    ExprElIfNode(const p<Node>& parent, p<ExprNode> condition, p<StatementBlockNode> block)
        : Node(parent), _condition(condition), _block(block) {}

    [[nodiscard]] const p<ExprNode>& condition() const;
    [[nodiscard]] const p<StatementBlockNode>& block() const;
};

class ExprIfElseNode : public ExprNode {
    p<ExprNode> _condition;
    p<StatementBlockNode> _thenBlock;
    vector<p<ExprElIfNode>> _elifs;
    p<StatementBlockNode> _elseBlock;

public:
    ExprIfElseNode(const p<Node>& parent, p<ExprNode> condition, p<StatementBlockNode> thenBlock,
                   vector<p<ExprElIfNode>> elifs, p<StatementBlockNode> elseBlock)
        : ExprNode(parent), _condition(condition), _thenBlock(thenBlock), _elifs(std::move(elifs)),
          _elseBlock(elseBlock) {}

    [[nodiscard]] const p<ExprNode>& condition() const;
    [[nodiscard]] const p<StatementBlockNode>& thenBlock() const;
    [[nodiscard]] const vector<p<ExprElIfNode>>& elifs() const;
    [[nodiscard]] const p<StatementBlockNode>& elseBlock() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprOneLineIfElseNode : public ExprNode {
    p<ExprNode> _condition;
    p<ExprNode> _trueValue;
    p<ExprNode> _falseValue;

public:
    ExprOneLineIfElseNode(const p<Node>& parent, p<ExprNode> condition, p<ExprNode> trueValue, p<ExprNode> falseValue)
        : ExprNode(parent), _condition(condition), _trueValue(trueValue), _falseValue(falseValue) {}

    [[nodiscard]] const p<ExprNode>& condition() const { return _condition; }
    [[nodiscard]] const p<ExprNode>& trueValue() const { return _trueValue; }
    [[nodiscard]] const p<ExprNode>& falseValue() const { return _falseValue; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprGetNode : public ExprNode {
    p<ExprNode> _arrayExpr;
    vector<p<ExprNode>> _indices;

public:
    ExprGetNode(const p<Node>& parent, p<ExprNode> arrayExpr, vector<p<ExprNode>> indices)
        : ExprNode(parent), _arrayExpr(arrayExpr), _indices(std::move(indices)) {}

    [[nodiscard]] const p<ExprNode>& arrayExpr() const;
    [[nodiscard]] const vector<p<ExprNode>>& indices() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprArrayNode : public ExprNode {
    vector<p<ExprNode>> _elements;

public:
    ExprArrayNode(const p<Node>& parent, vector<p<ExprNode>> elements)
        : ExprNode(parent), _elements(std::move(elements)) {}

    [[nodiscard]] const vector<p<ExprNode>>& elements() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprArrayInitNode : public ExprNode {
    p<LiteralNode> _value;
    p<TypeNode> _explicitType;

public:
    ExprArrayInitNode(const p<Node>& parent, p<LiteralNode> value, p<TypeNode> explicitType)
        : ExprNode(parent), _value(value), _explicitType(explicitType) {}

    [[nodiscard]] const p<LiteralNode>& value() const;
    [[nodiscard]] const p<TypeNode>& explicitType() const;
    [[nodiscard]] TypeInfo getType() const override;
};

class ExprGetRefNode : public ExprNode {
    Token _obj;
    vector<Token> _subs;

public:
    ExprGetRefNode(const p<Node>& parent, Token obj, vector<Token> subs)
        : ExprNode(parent), _obj(std::move(obj)), _subs(std::move(subs)) {}

    [[nodiscard]] Token obj() const { return _obj; }
    [[nodiscard]] const vector<Token>& subs() const { return _subs; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

class ExprUnaryNode : public ExprNode {
public:
    enum class Op : u8 { Neg, Rev, Not };

protected:
    Op _op;
    p<ExprNode> _right;

public:
    ExprUnaryNode(const p<Node>& parent, Op op, p<ExprNode> right) : ExprNode(parent), _op(op), _right(right) {}

    [[nodiscard]] Op op() const;
    [[nodiscard]] const p<ExprNode>& right() const;
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

// Lambda 形参：name + 可选类型（缺省时 type=nullptr，由调用 / 赋值点的期望
// 函数类型反推后回填，详见 spec §4.1）
struct LambdaParamSlot {
    Token name;
    p<TypeNode> type; // nullptr → 待上下文反推
};

// Lambda 捕获槽位（Phase 4a，spec §6.1 / §6.2）
// 标识 lambda body 内引用到的外层局部变量；emitLambdaFunction 在 body 编译过程中
// 增量 append（compileLiteralExpr 命中外层 local → addCapture），compileLambdaExpr
// 在 fn 回归外层上下文后据此从 _localVarPtrs 加载 + 写入 captures Rc。
//
// Phase 4a 仅支持标量类型；堆句柄 / struct / `T&` 拒入（4a-2 / 4c 接入）。
struct CaptureSlot {
    string name;    // 外层变量名
    TypeInfo type;  // 外层变量类型（值复制语义）
    u64 byteOffset; // 在 captures buffer 中的字节偏移（自然对齐）
};

// Lambda 字面量节点（spec §4.11）
// - Expr： `(args) RetT? => expr`     bodyExpr 单表达式
// - Block：`(args) RetT? => { stmts }` 或调用尾随 `{ (args) RetT? => stmts }`
//
// retType：显式标注则非空；否则 nullptr，由期望函数类型或 body 推断。
class LambdaExprNode : public ExprNode {
public:
    enum class Form : u8 { Expr, Block };

private:
    Form _form;
    vector<LambdaParamSlot> _params;
    p<TypeNode> _retType; // 仅 Paren 显式标注；其余 nullptr
    p<TypeNode> _fallibleErrType = nullptr;
    p<ExprNode> _bodyExpr;               // Form::Expr
    vector<p<StatementNode>> _bodyStmts; // Form::Block
    // body 编译用的内层作用域；持有 lambda 形参符号。AST builder 在构建时填充，
    // 让 body 表达式 / 语句的 parent 链可经此链路向上找到形参（findNearestScope）。
    // body 内的符号引用在 sema 阶段可识别"形参 vs 自由变量"，闭包来到 Phase 4 之前
    // 自由变量直接报错（Phase 2c）。
    p<ScopeNode> _bodyScope{nullptr};
    // Phase 2b：调用 / 赋值点反推后的整体 Fn 类型（getType() 优先返回）
    TypeInfo _inferredFnType;
    // Phase 4a：自由变量捕获槽位（emit 期间增量填充）
    vector<CaptureSlot> _captures;
    u64 _capturesTotalSize = 0;
    // Phase 4c：是否含 T& 捕获（spec §6.3）。
    // 若为 true，captures 走"栈嵌入"路径（alloca + LSB 标 1 标记跳过 RC），lambda 视作"广义 T&"
    // 不可逃逸（不可作 ret / 不可入 var / 字段 / 容器 / Rc）。
    bool _hasRefCapture = false;

public:
    LambdaExprNode(const p<Node>& parent, Form form, vector<LambdaParamSlot> params, p<TypeNode> retType,
                   p<ExprNode> bodyExpr, vector<p<StatementNode>> bodyStmts)
        : ExprNode(parent), _form(form), _params(std::move(params)), _retType(retType), _bodyExpr(bodyExpr),
          _bodyStmts(std::move(bodyStmts)) {}

    void setBodyScope(p<ScopeNode> sc) { _bodyScope = sc; }
    [[nodiscard]] p<ScopeNode> bodyScope() const { return _bodyScope; }

    // Phase 4a：捕获表（emitLambdaFunction 期间增量 append）
    [[nodiscard]] const vector<CaptureSlot>& captures() const { return _captures; }
    [[nodiscard]] u64 capturesTotalSize() const { return _capturesTotalSize; }
    [[nodiscard]] int findCapture(const string& name) const {
        for (size_t i = 0; i < _captures.size(); ++i)
            if (_captures[i].name == name) return static_cast<int>(i);
        return -1;
    }
    // 追加捕获槽位；offset / totalSize 由调用方按对齐规则算好。返回新槽位索引。
    int addCapture(const string& name, TypeInfo type, u64 offset, u64 totalSize) {
        _captures.push_back(CaptureSlot{.name = name, .type = std::move(type), .byteOffset = offset});
        _capturesTotalSize = totalSize;
        return static_cast<int>(_captures.size()) - 1;
    }
    // 重置捕获状态（emitLambdaFunction 缓存命中前的清场，避免重复 append）
    void clearCaptures() {
        _captures.clear();
        _capturesTotalSize = 0;
        _hasRefCapture = false;
    }

    // Phase 4c：T& 捕获标记
    [[nodiscard]] bool hasRefCapture() const { return _hasRefCapture; }
    void setHasRefCapture(bool v) { _hasRefCapture = v; }

    // Phase 2b：调用 / 赋值点反推后的整体 Fn 类型（含已填的 params + retType）。
    // 不破坏源 _params / _retType（它们是源 AST），只在 getType() / 语义查询里优先返回这个。
    void setInferredFnType(TypeInfo t) { _inferredFnType = std::move(t); }
    [[nodiscard]] const TypeInfo& inferredFnType() const { return _inferredFnType; }

    [[nodiscard]] Form form() const { return _form; }
    [[nodiscard]] const vector<LambdaParamSlot>& params() const { return _params; }
    [[nodiscard]] vector<LambdaParamSlot>& mutableParams() { return _params; }
    [[nodiscard]] const p<TypeNode>& retType() const { return _retType; }
    void setFallibleErrType(p<TypeNode> t) { _fallibleErrType = t; }
    [[nodiscard]] const p<TypeNode>& fallibleErrTypeNode() const { return _fallibleErrType; }
    [[nodiscard]] const p<ExprNode>& bodyExpr() const { return _bodyExpr; }
    [[nodiscard]] const vector<p<StatementNode>>& bodyStmts() const { return _bodyStmts; }

    // 形参 name 由 lambdaParam 强制带 ID
    [[nodiscard]] bool hasAllParamTypes() const {
        for (auto& s : _params)
            if (!s.type) return false;
        return true;
    }

    // 返回 Fn TypeInfo；缺失槽位用 empty TypeInfo 占位，等 Phase 2b 上下文反推回填
    [[nodiscard]] TypeInfo getType() const override;
};

// 元组构造表达式 (e1, e2, ...)
// 至少 2 个元素（g4 保证）；类型由各元素类型组合而成的 Tuple TypeInfo
class ExprTupleNode : public ExprNode {
    vector<p<ExprNode>> _elements;

public:
    ExprTupleNode(const p<Node>& parent, vector<p<ExprNode>> elements)
        : ExprNode(parent), _elements(std::move(elements)) {}

    [[nodiscard]] const vector<p<ExprNode>>& elements() const { return _elements; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

// 路径调用表达式 `LHS::RHS(...)` —— 承载两条 sema 分流:
//   1) enum 构造: `E::V` / `E::V()` / `E::V(args)`, 零参 variant 与 E::V() 等价
//   2) 静态调用: `Type::staticFn(args)` (RHS 标 #Static), Phase 2c 起接入
// arity / 变体匹配 / 静态分流均在 sema 阶段校验.
// 访问器名沿用 enumName / variantName, 留待 2c 与 struct 分流一并改名.
class ExprPathCallNode : public ExprNode {
    TypePath _lhsPath; // 限定路径；Self / 裸名时仅一段。末段与 _enumName 相同
    Token _enumName;
    Token _variantName;
    vector<p<ExprNode>> _args;
    // Phase 6E.4: turbofish 形态 `Type:<T>::name:<U>(args)` 的 LHS / RHS 类型实参.
    // 单 Type 名时为空; 用于驱动 Compiler::ensureStructInstance + applySubst.
    vector<p<TypeNode>> _lhsTypeArgs;
    vector<p<TypeNode>> _rhsTypeArgs;

public:
    ExprPathCallNode(const p<Node>& parent, Token enumName, Token variantName)
        : ExprNode(parent), _lhsPath(enumName), _enumName(std::move(enumName)), _variantName(std::move(variantName)) {}

    void addArg(p<ExprNode> a) { _args.push_back(a); }
    void setLhsTypeArgs(vector<p<TypeNode>> a) { _lhsTypeArgs = std::move(a); }
    void setRhsTypeArgs(vector<p<TypeNode>> a) { _rhsTypeArgs = std::move(a); }
    void setLhsPath(TypePath p) {
        _lhsPath = std::move(p);
        if (!_lhsPath.empty()) _enumName = _lhsPath.last();
    }

    [[nodiscard]] const Token& enumName() const { return _enumName; }
    [[nodiscard]] const TypePath& lhsPath() const { return _lhsPath; }
    [[nodiscard]] const Token& variantName() const { return _variantName; }
    [[nodiscard]] const vector<p<ExprNode>>& args() const { return _args; }
    [[nodiscard]] const vector<p<TypeNode>>& lhsTypeArgs() const { return _lhsTypeArgs; }
    [[nodiscard]] const vector<p<TypeNode>>& rhsTypeArgs() const { return _rhsTypeArgs; }
    // `Self::name`：沿 parent 链找到 enclosing StructImplNode 的源码名；
    // 体外仍返回 "Self"（SemaPass 报 E3123）。非 Self LHS 原样返回 token 文本。
    [[nodiscard]] string resolvedLhsName() const;
    // 限定路径走 resolveExprTypeLhs（含 ownerModule）；Self 用 enclosing struct。
    [[nodiscard]] TypeInfo resolvedLhsType() const;
    [[nodiscard]] TypeInfo getType() const override;
};

// 字段初始化项：.name = value（仅出现在 ExprStructLitNode 内）
// Phase 1b：AST 占位；sema / codegen 接管在 Phase 2/3
class FieldInitNode : public Node {
    Token _name;
    p<ExprNode> _value;

public:
    FieldInitNode(const p<Node>& parent, Token name, p<ExprNode> value)
        : Node(parent), _name(std::move(name)), _value(value) {}

    [[nodiscard]] const Token& name() const { return _name; }
    [[nodiscard]] const p<ExprNode>& value() const { return _value; }
};

// 结构体字段字面量 `Self { \n .field = value \n ... }`
// Phase 1b：AST 占位；语义层强制 LHS 为 Self 且出现位限 `#Static fn` 体
// 类型在 sema 阶段绑定为所属结构体类型；当前 getType() 返回 empty
class ExprStructLitNode : public ExprNode {
    Token _selfTok;
    string _structName; // Phase 3b: ast_builder 扫 _scopeStack 填入 (与 TypeSelfNode 同源)
    // DRAFT-const-eval Phase 5: true=Self{...} (受 #Static fn 限制),
    // false=TypeName{...} (任意 expr 位, 含全局 #Cval 初始化器)
    bool _isSelfForm;
    TypePath _typePath; // 非 Self 形态的 LHS；Self 时为空
    vector<p<FieldInitNode>> _fields;

public:
    ExprStructLitNode(const p<Node>& parent, Token selfTok, string structName, bool isSelfForm = true)
        : ExprNode(parent), _selfTok(std::move(selfTok)), _structName(std::move(structName)), _isSelfForm(isSelfForm) {}

    void addField(p<FieldInitNode> f) { _fields.push_back(f); }
    void setTypePath(TypePath p) { _typePath = std::move(p); }

    [[nodiscard]] const Token& selfToken() const { return _selfTok; }
    [[nodiscard]] const string& structName() const { return _structName; }
    [[nodiscard]] bool isSelfForm() const { return _isSelfForm; }
    [[nodiscard]] const TypePath& typePath() const { return _typePath; }
    [[nodiscard]] const vector<p<FieldInitNode>>& fields() const { return _fields; }
    [[nodiscard]] TypeInfo getType() const override;
};

// match arm 模式 v1 子集：
// - isElse=true：兜底分支 `else`，无 enumName/variantName/binds
// - isElse=false：`E::V` / `E::V()` / `E::V(b1, b2, ...)`
class EnumPatternNode : public Node {
    bool _isElse;
    TypePath _enumPath;
    Token _enumName;
    Token _variantName;
    vector<Token> _binds;

public:
    // 兜底分支
    EnumPatternNode(const p<Node>& parent, const Token& elseTok)
        : Node(parent), _isElse(true), _enumName(elseTok), _variantName(elseTok) {}
    // enum 模式
    EnumPatternNode(const p<Node>& parent, Token enumName, Token variantName, vector<Token> binds)
        : Node(parent), _isElse(false), _enumPath(enumName), _enumName(std::move(enumName)),
          _variantName(std::move(variantName)), _binds(std::move(binds)) {}

    void setEnumPath(TypePath p) {
        _enumPath = std::move(p);
        if (!_enumPath.empty()) _enumName = _enumPath.last();
    }

    [[nodiscard]] bool isElse() const { return _isElse; }
    [[nodiscard]] const Token& enumName() const { return _enumName; }
    [[nodiscard]] const TypePath& enumPath() const { return _enumPath; }
    [[nodiscard]] const Token& variantName() const { return _variantName; }
    [[nodiscard]] const vector<Token>& binds() const { return _binds; }
};

// match 单条 arm: pattern => expr  或  pattern => { stmts }
// 表达式体与块体互斥；块体值规则与 if / lambda 同（末表达式无 `;` 即块值）
// MatchArm 自身是 ScopeNode，承载 pattern 中的 binding 符号（让 body 内的
// LiteralObj::getType 能沿 scope 链解析到绑定类型）
class MatchArmNode : public ScopeNode {
    p<EnumPatternNode> _pattern;
    p<ExprNode> _body;            // 表达式体；块体时为 nullptr
    p<StatementBlockNode> _block; // 块体；表达式体时为 nullptr

public:
    MatchArmNode(const p<Node>& parent, p<EnumPatternNode> pattern, p<ExprNode> body,
                 p<StatementBlockNode> block = nullptr)
        : ScopeNode(parent), _pattern(pattern), _body(body), _block(block) {}

    [[nodiscard]] const p<EnumPatternNode>& pattern() const { return _pattern; }
    [[nodiscard]] const p<ExprNode>& body() const { return _body; }
    [[nodiscard]] const p<StatementBlockNode>& block() const { return _block; }
    [[nodiscard]] bool hasBlock() const { return _block != nullptr; }
    [[nodiscard]] TypeInfo resultType() const;
    [[nodiscard]] bool skipsTypeMerge() const;
    [[nodiscard]] int resultLine() const;
    [[nodiscard]] int resultCol() const;
};

// match 表达式: match scrutinee { arm1 ... armN }
// 类型：参与合并的 arm 体类型必须严格一致（流终止臂跳过）；若全为 void 则 match 为 void
class ExprMatchNode : public ExprNode {
    p<ExprNode> _scrutinee;
    vector<p<MatchArmNode>> _arms;

public:
    ExprMatchNode(const p<Node>& parent, p<ExprNode> scrutinee, vector<p<MatchArmNode>> arms)
        : ExprNode(parent), _scrutinee(scrutinee), _arms(std::move(arms)) {}

    [[nodiscard]] const p<ExprNode>& scrutinee() const { return _scrutinee; }
    [[nodiscard]] const vector<p<MatchArmNode>>& arms() const { return _arms; }
    [[nodiscard]] TypeInfo getType() const override;
};

// catch arm: `catch <绑定名> <错误 enum 类型> { body }`
// DRAFT-错误.md [#4.H]：try 块的子句；body 内绑定 `errName` 为捕获的错误值（类型 = errType）
// CatchArmNode 自身是 ScopeNode：承载绑定符号，使 body 内的 LiteralObj::getType
// 可沿 scope 链解析到绑定类型（与 MatchArmNode 同档）
class CatchArmNode : public ScopeNode {
    Token _errName;
    TypeInfo _errType; // typePath 解析后的错误 enum（含 ownerModule）
    p<StatementBlockNode> _body;

public:
    CatchArmNode(const p<Node>& parent, Token errName, TypeInfo errType, p<StatementBlockNode> body)
        : ScopeNode(parent), _errName(std::move(errName)), _errType(std::move(errType)), _body(body) {}

    [[nodiscard]] const Token& errName() const { return _errName; }
    [[nodiscard]] const string& errType() const { return _errType.name; }
    [[nodiscard]] const TypeInfo& errTypeInfo() const { return _errType; }
    [[nodiscard]] const p<StatementBlockNode>& body() const { return _body; }
};

// try { stmts } catch e1 E1 { ... } catch e2 E2 { ... }
// DRAFT-错误.md [#4.H]：跨类型错误形态；try block 内可失败调用错误自动路由到匹配 catch 子句。
// 类型：try block 末表达式类型 + 所有 catch arm body 末表达式类型须一致（流终止 arm 不参与，
// 与 if-else / match 同档）。10f 仅做语义校验；实际错误通道 IR 路由推 10g。
class ExprTryCatchNode : public ExprNode {
    p<StatementBlockNode> _tryBlock;
    vector<p<CatchArmNode>> _catches;

public:
    ExprTryCatchNode(const p<Node>& parent, p<StatementBlockNode> tryBlock, vector<p<CatchArmNode>> catches)
        : ExprNode(parent), _tryBlock(tryBlock), _catches(std::move(catches)) {}

    [[nodiscard]] const p<StatementBlockNode>& tryBlock() const { return _tryBlock; }
    [[nodiscard]] const vector<p<CatchArmNode>>& catches() const { return _catches; }
    [[nodiscard]] TypeInfo getType() const override;
};

// Dyn<D>(x) / Dyn<D&>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
// 形式：把 Rc<U> / U& 提升为 fat pointer { vtable, data }；vtable 槽 0 = dtor，
// 槽 1..N = D 方法按声明序。Phase 1c 仅引入节点与占位 codegen（vtable=null），
// vtable 真值与对象安全检查留 Phase 2 / Phase 3。
//
// ast_builder 在 visitExprCall 命中 `Dyn<D>(x)` / `Dyn<D&>(x)` 形态时改产此节点；
// `_specType` 持原 turbofish 中的 typeArg（D 或 Ref<D>），用于回算 fat ptr 内层；
// `_isBorrow` 由内层 TypeNode 是否为 `Ref<...>` 决定。
class ExprDynCtorNode : public ExprNode {
    p<TypeNode> _specType; // turbofish 内的类型节点（D 或 D&）
    p<ExprNode> _arg;      // 构造源：Rc<U> 或 U&
    bool _isBorrow;        // true = Dyn<D&>(...), false = Dyn<D>(...)

public:
    ExprDynCtorNode(const p<Node>& parent, p<TypeNode> specType, p<ExprNode> arg, bool isBorrow)
        : ExprNode(parent), _specType(specType), _arg(arg), _isBorrow(isBorrow) {}

    [[nodiscard]] const p<TypeNode>& specType() const { return _specType; }
    [[nodiscard]] const p<ExprNode>& arg() const { return _arg; }
    [[nodiscard]] bool isBorrow() const { return _isBorrow; }
    [[nodiscard]] TypeInfo getType() const override;
};

// Heap:<T>(arg) 堆作用域句柄构造（DRAFT-heap-types §8.3a）
// a <- b：移出旧值、替换新值、返回旧值（表达式，右结合，最低优先级）
class ExprMoveAssignNode : public ExprNode {
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprMoveAssignNode(const p<Node>& parent, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _left(left), _right(right) {}

    [[nodiscard]] const p<ExprNode>& left() const { return _left; }
    [[nodiscard]] const p<ExprNode>& right() const { return _right; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

// a ?? b：a 为 Nullable<T> 时，有值取 a.get()，否则取 b
class ExprNullElseNode : public ExprNode {
    p<ExprNode> _left;
    p<ExprNode> _right;

public:
    ExprNullElseNode(const p<Node>& parent, p<ExprNode> left, p<ExprNode> right)
        : ExprNode(parent), _left(left), _right(right) {}

    [[nodiscard]] const p<ExprNode>& left() const { return _left; }
    [[nodiscard]] const p<ExprNode>& right() const { return _right; }
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] int resolveLineNumber() const override;
    [[nodiscard]] int resolveColumn() const override;
};

// §4.9.1.4 / §4.9.3.5：调用点是 `#NoReturn` 时该表达式流终止，不参与 if / match / try 类型合并。
// 只认顶层身份引用（`panic("x")` / `exit(1)`）；方法调用暂不参与（与 E7014 同一保守策略）。
bool callIsNoReturn(p<ScopeNode> scope, p<ExprCallNode> call);
bool exprTerminatesFlow(p<ScopeNode> scope, p<ExprNode> expr);
bool blockTerminatesFlow(p<ScopeNode> scope, p<StatementBlockNode> block);

#endif // YUX_LANG_EXPR_NODE_H

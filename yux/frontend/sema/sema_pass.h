// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_PASS_H
#define YUX_LANG_SEMA_PASS_H

#include "ast/node/file_node.h"
#include "sema/name_resolver.h"

#include <map>

class ExprNode;
class StatementNode;
class StatementBlockNode;
class Yux;

// SemaPass —— Sema/Codegen 拆分骨架（Phase 3.1）
//
// 原设计的流水线 `file → antlr → AST → 语义检查 → llvm → ok` 中"语义检查"
// 这一段早期为赶通混在了 `Compiler` 里。SemaPass 是把它再抠出来的入口：
// 不依赖 LLVM、只看 AST、产出 `resolvedType` / `resolvedSymbol` 等标注。
//
// 3.1：仅骨架 —— walk file→fn→stmt→expr 全树, visitExpr 暂为空。
// 3.2：visitExpr 写 resolvedType, 与 compile<Foo>Expr 入口的写并存校验。
// 3.3+：按子系统（方法分派 / 调用解析 / intrinsic ...）把 throw 从
//      `yux/yux/compiler/compiler_*.cpp` 抠进来, 最终 codegen 不再抛语义错。
//
// 由 `yux_frontend` 静态库提供, 不依赖 LLVM, 给 `yux-lsp` / `yux-check` 共用。
// 实现按文件拆：sema_pass.cpp（入口）/ sema_stmt.cpp / sema_expr.cpp / sema_check.cpp。
class SemaPass {
public:
    // yux：前端环境句柄，提供 sdkFile / modulePath / specRegistry / specImplChecker
    // 等长期生存的服务。单文件 / SDK 自构建场景可传 nullptr（_sdkFile 与 _sourcePath
    // 退化为空, sema 仅做 builtin 范围内的检查, 不抛跨文件符号错）。
    // 改造前 ctor 形如 SemaPass(file, sdkFile, sourcePath) —— sdkFile / sourcePath
    // 都从 Yux 派生, 没必要让 caller 各算一遍。两个真实 caller (Compiler / yux-check)
    // 都有 Yux 实例, 这里统一从 Yux 拿。
    explicit SemaPass(p<FileNode> file, Yux* yux = nullptr);

    void run();

private:
    p<FileNode> _file;
    Yux* _yux;
    // 派生缓存: 进 ctor 时从 _yux 一次性算出, 避免后续 visit* 反复调用 yux 接口。
    p<FileNode> _sdkFile;
    string _sourcePath;
    sema::NameResolver _names;

    // Phase 3.3 前置.4: 当前所在 fn (用于 caller 的 #Fallible(E) 校验) +
    // try block 栈 (用于 ID-callee 错误传播 E7001/E7004/E7006/E7016).
    // 与 Compiler 的 `_currentFnNode` / `_tryCatchStack` 协议同步: 进入
    // visitFn 时 push, 离开时 pop; 进入 ExprTryCatchNode.tryBlock 时 push
    // 新的 seenErrTypes 层, visitBlock 完后 pop (catch arms 不在栈内).
    p<FnNode> _currentFn = nullptr;
    vector<vector<string>> _tryStack;

    // labeled break：label stack 替代 _loopDepth。
    // 进入 loop 时 push label（空 Token = 无 label），离开时 pop；
    // break@label 按 label 从内向外搜索匹配，未命中 → E3025；
    // 无 label 的 break 走最内层 loop（栈非空校验 → E3094）。
    vector<Token> _loopLabelStack;

    // Phase 3.4.d.2: 当前所在 struct impl 名 (用于私有字段可见性 E3042).
    // 与 Compiler 的 `_currentStructName` 同步: 进入 struct impl 方法 visit
    // 时 set, 离开时 clear. Phase C 下钻泛型 impl，名字仍是裸 struct 名（无 `<T>`）.
    // 空串表示自由 fn (任何私有字段访问都报错).
    string _currentStructName;

    // Phase C：当前泛型 fn / impl 的类型参数名。非空 = 正在走模板体。
    // 类型参数当不透明 TypeParam：依赖 T 具体化的 getType 诊断吞掉；
    // 形态检查（#NoCopy / 未定义符号 / arity）仍报。
    std::set<std::string> _currentTypeParams;
    // Phase C：泛型体实例化替换（T → 具体类型）。非空 = 正在复查某次实例。
    // 模板期仍把 T 当不透明；ret / 赋值在 substitute 后再比。
    std::map<std::string, TypeInfo> _instSubst;
    std::set<std::string> _checkedGenericInst;

    // v0.16 闭包捕获: 当前正在遍历的 lambda 节点 (非空 = 在 lambda body 内).
    // 与 Compiler 的 `_currentLambdaForCapture` 功能对等但 0 LLVM 依赖.
    // visitExpr 进入 LambdaExprNode 时 push, 离开时 pop; 支持嵌套闭包.
    p<class LambdaExprNode> _currentLambda = nullptr;
    // 当前 lambda body 遍历期间是否发现了 T& 捕获 (E4022 判定依据).
    // visitExpr 遇 LambdaExprNode 时重置为 false, body 遍历过程中由
    // ExprLiteralNode handler 置 true; 退出 lambda 后据此写 hasRefCapture.
    bool _currentLambdaHasRefCapture = false;
    // 同趟遍历是否捕获了堆句柄（Rc / Weak / Array / String / Heap?）。
    // 与 hasRefCapture 同时成立 → E2029（栈嵌入路径不能混析构字段）。
    bool _currentLambdaHasHandleCapture = false;
    string _currentLambdaHandleCapName;
    string _currentLambdaHandleCapTypeName;

    // Phase B-1: move 追踪 — 已被 move 的变量名 (E4033 判定依据).
    // visitFn 入口 clear，move intrinsic 调用处 insert，if/else 汇合取并集。
    std::set<std::string> _movedVars;
    // 正在访问 Dot 的 base：路径前缀 ident / 中间段不算「当值」。
    bool _inDotBase = false;

    void visitFn(p<FnNode> fn);
    void visitStmt(p<StatementNode> stmt);
    // expected：块末尾值（if / match / try 表达式）的靶向类型，传给 resultExpr。
    void visitBlock(p<StatementBlockNode> block, const TypeInfo* expected = nullptr);
    // expected：赋值 / 声明 / 返回 / 调用实参 / 嵌套数组 / if·match 臂 的目标类型。
    // 非空时数组字面量按靶向类型递归检查（E3009 / E3012），不再走无上下文的 getType。
    // callCallee：当前节点是调用的 callee（`x.foo()` 的 Dot），读路径字段检查跳过。
    void visitExpr(p<ExprNode> expr, const TypeInfo* expected = nullptr, bool callCallee = false);
    // 实参列表：expected 非空且下标有具体类型时带靶向类型下钻。
    void visitExprList(const vector<p<ExprNode>>& args, const vector<TypeInfo>* expected = nullptr);

    // Phase C：t 剥 Ref/Heap/Rc 后是否为当前模板的类型参数。
    [[nodiscard]] bool isCurrentTypeParam(const TypeInfo& t) const;
    // Phase C：替换后是否仍含当前模板形参（含嵌套 genericArgs）。
    [[nodiscard]] bool typeStillTemplate(const TypeInfo& t) const;
    // Phase C：把当前实例化替换应用到类型；无替换时原样返回。
    [[nodiscard]] TypeInfo applyInstSubst(const TypeInfo& t) const;
    // 源码写出的具体 `S<Concrete>`：复查该泛型 struct 方法体（形参 / 返回 / 字段 / let）。
    void noteConcreteGenericType(const TypeInfo& t);
    // 调用点 typeArgs 已知后复查泛型 fn 体（ret / 赋值）。
    void checkGenericFnInst(p<FnNode> fn, const vector<TypeInfo>& typeArgs);
    // 泛型 impl 实例化：复查该 impl 全部非 Builtin 方法体。
    void checkGenericImplInst(class StructImplNode* impl, const std::map<std::string, TypeInfo>& subst);
    void checkGenericBodyInst(p<FnNode> fn, const std::map<std::string, TypeInfo>& subst, const string& structName);

    // Phase C：带 target-type 的数组字面量 / 填充检查（E3009 / E3012）。
    // 填充值须是 int / float / bool 字面量，否则 E3080（与 T 无关，模板期也报）。
    void checkArrayLiteral(p<class ExprArrayNode> n, const TypeInfo& expected);
    void checkArrayInit(p<class ExprArrayInitNode> n, const TypeInfo* expected);
    void checkArrayElemAgainst(p<ExprNode> elem, const TypeInfo& want, int line, int col);
    // 空 `[]`：仅 Array<T> 靶向合法，否则 E3063。模板形参跳过。
    // 与 compileArrayLiteralExpr 对齐（固定数组空字面量先于 E3012）。
    void checkEmptyArrayLiteral(p<class ExprArrayNode> n, const TypeInfo* expected);

    // DRAFT-spec-default-body Phase 2：spec 默认体占位符号校验
    // ([#1.S])。仅识别 `$.method(args)` 形态, 验证 method 在 spec 自身签名集
    // 内（不验类型 / 不查跨 spec / 不下钻 lambda 等复杂形态）; 完整 typecheck
    // 推迟到 Phase 3 单态化时机克隆 + 真实 typecheck。
    void visitSpecDefaults();

    // Bucket 6 单点 (CURRENT-check.md): 二元运算符方法解析的 SemaPass 入口
    // (E3073 + byval hint). 把"算 leftType / rightType + 形态 gate + 调
    // sema::validateBinOpMethodResolution"封进来, 给 ExprAddSub /
    // ExprMulDivMod / ExprBinOp / ExprCompare 四个 visit 分支共用.
    //
    // 仅在 leftType 是非 builtin / 非 Ref/Rc/Array/Heap/Weak/Nullable/Ptr/Tuple
    // 容器 / 已知非泛型 struct 时走方法解析; 泛型 struct 仍跳过。
    // 实例化后内置类型走 E3001（与 getType 对齐）；模板形参 / lambda 形参跳过。
    // methodName 由调用方按 op 映射 (plus/minus/...).
    void tryValidateBinOpMethod(p<ExprNode> leftExpr, p<ExprNode> rightExpr, const string& methodName, int line,
                                int col);

    // 比较形态：subst + peelAutoDeref 后 Weak ==/!= → E3078，Ptr 排序 / Function == → E3073。
    // && / ||：subst 后两侧类型须一致（内置跨类型 → E3001 comparison），
    // 与 ExprCompareNode::getType / compileCompareExpr 对齐。模板形参跳过。
    void tryValidateCompareForm(p<class ExprCompareNode> n);

    // 一元运算符：实例化后内置类型走 E3070/E3071（与 getType 对齐）；
    // 非 builtin / 非容器 / 非泛型 struct 走 E3074（neg/inv/not 方法不存在）。
    // 模板形参 / lambda 形参跳过。methodName 为 neg / inv / not。
    void tryValidateUnaryOpMethod(p<ExprNode> rightExpr, const string& methodName, int line, int col);

    // 索引基类型：subst + peelRef 后须是 [N]T / Array<T>，否则 E3062。
    // 模板形参 / 取类型失败跳过。与 ExprGetNode::getType / compileArraySet 对齐。
    // 赋值另查形态：空下标 E3060（g4 死防御）；非变量 / 非 obj.field → E3061。
    void tryValidateIndexBase(p<ExprNode> arrayExpr, int line, int col);

    // 成员链：subst + peelAutoDeref 后查字段。已知 struct 缺字段 → E3040；
    // 实例写/取静态字段 → E3152；非 struct → E3041；元组 `.N` 越界 → E3100。
    // 模板形参跳过（实例化后再查 E3100）。与 compileAssignStatement /
    // ExprGetRefNode::getType / 读路径 ExprDotNode 对齐。
    void tryValidateFieldChain(const TypeInfo& start, const vector<string>& members, int line, int col);

    // `?.`：subst + 剥 Ref 后须是 Nullable，否则 E3024。内层无 struct → E3044，
    // 缺字段 → E3040。方法 / `to_*` 不按字段查。模板形参跳过。
    // 与 ExprDotNode::getType / compileSafeDotExpr 对齐。
    void tryValidateSafeDot(p<class ExprDotNode> n);

    // match scrut：subst + 别名 / Rc<E> / Heap<E> / E& 剥到 enum。临时 Rc/Heap
    // 直接 match → E2022；非 enum（含用户 struct）→ E2022。模板形参跳过。
    // 实例化为 enum 后走 validateMatchArms。与 compileMatchExpr 对齐。
    void tryValidateMatchScrut(p<class ExprMatchNode> n);

    // 插值 / String+ 叶子：subst 后须是 String 或有 `<T>.to_string`，否则 E3026。
    // 模板形参跳过。与 compileStringTemplate / compileStringPlusChain 对齐。
    void tryValidateToString(p<ExprNode> e);

    // `+` 结果为 String 时沿左脊展开叶子，逐叶 tryValidateToString。
    void tryValidateStringPlus(p<class ExprAddSubNode> n);

    // if / elif / else 块末尾值：subst 后类型须一致，否则 E3005。
    // 模板形参跳过。与 ExprIfElseNode::getType 对齐。
    void tryValidateIfElse(p<class ExprIfElseNode> n);
    // 一行 `if c { a } else { b }`：subst 后 a / b 类型须一致，否则 E3005。
    void tryValidateOneLineIfElse(p<class ExprOneLineIfElseNode> n);

    // Field.value：编译期可定且有 `$` 才合法。否则 E3133 / E3134。
    // 模板形参跳过；与 T 无关的运行期 Field / 无 receiver 模板期也报。
    // 与 ExprDotNode::getType / compileDotExpr / compileAssignStatement 对齐。
    [[nodiscard]] bool hasFieldValueReceiver() const;
    void tryValidateReflectFieldValueRead(p<class ExprDotNode> n);
    void tryValidateReflectFieldValueWrite(const string& objName, const TypeInfo& objType,
                                           const vector<string>& members, int line, int col);

    // §6.6.2 / §7.5.3.4：本文件 extern fn 的用户 struct / enum 白名单（形参+返回）。
    // 类型 kind 禁令已在 ast_builder 抛 E4028/E2031/E1136/E2034。
    void validateExternFns();
    void checkExternCLayoutType(const TypeInfo& t, const string& fnName, const char* where, int line);
    void checkCLayoutFields(const TypeInfo& structTy, StructDeclNode* sd, int line, std::set<string>& visiting);
};

#endif // YUX_LANG_SEMA_PASS_H

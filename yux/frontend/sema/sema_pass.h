// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_PASS_H
#define YUX_LANG_SEMA_PASS_H

#include "ast/node/file_node.h"
#include "sema/name_resolver.h"

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
// 当前 pass 是 no-op, 主要保证拓扑落位; 由 `yux_frontend` 静态库提供,
// 不依赖 LLVM, 给 `yux-lsp` / 未来 `yux-check` 共用。
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

    // v0.16 闭包捕获: 当前正在遍历的 lambda 节点 (非空 = 在 lambda body 内).
    // 与 Compiler 的 `_currentLambdaForCapture` 功能对等但 0 LLVM 依赖.
    // visitExpr 进入 LambdaExprNode 时 push, 离开时 pop; 支持嵌套闭包.
    p<class LambdaExprNode> _currentLambda = nullptr;
    // 当前 lambda body 遍历期间是否发现了 T& 捕获 (E4022 判定依据).
    // visitExpr 遇 LambdaExprNode 时重置为 false, body 遍历过程中由
    // ExprLiteralNode handler 置 true; 退出 lambda 后据此写 hasRefCapture.
    bool _currentLambdaHasRefCapture = false;

    // Phase B-1: move 追踪 — 已被 move 的变量名 (E4033 判定依据).
    // visitFn 入口 clear，move intrinsic 调用处 insert，if/else 汇合取并集。
    std::set<std::string> _movedVars;

    void visitFn(p<FnNode> fn);
    void visitStmt(p<StatementNode> stmt);
    void visitBlock(p<StatementBlockNode> block);
    void visitExpr(p<ExprNode> expr);

    // Phase C：t 剥 Ref/Heap/Rc 后是否为当前模板的类型参数。
    [[nodiscard]] bool isCurrentTypeParam(const TypeInfo& t) const;

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
    // 容器 / 已知非泛型 struct 时调用; 泛型 struct / 模板形参 / lambda 形参 (getType
    // 抛错) 一律跳过, 留 Compiler 兜底. methodName 由调用方按 op 映射 (plus/minus/...).
    void tryValidateBinOpMethod(p<ExprNode> leftExpr, p<ExprNode> rightExpr, const string& methodName, int line,
                                int col);
};

#endif // YUX_LANG_SEMA_PASS_H

// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_PASS_H
#define YUX_LANG_SEMA_PASS_H

#include "ast/node/file_node.h"

class ExprNode;
class StatementNode;
class StatementBlockNode;

// SemaPass —— Sema/Codegen 拆分骨架（Phase 3.1）
//
// 原设计的流水线 `file → antlr → AST → 语义检查 → llvm → ok` 中"语义检查"
// 这一段早期为赶通混在了 `Compiler` 里。SemaPass 是把它再抠出来的入口：
// 不依赖 LLVM、只看 AST、产出 `resolvedType` / `resolvedSymbol` 等标注。
//
// 3.1：仅骨架 —— walk file→fn→stmt→expr 全树, visitExpr 暂为空。
// 3.2：visitExpr 写 resolvedType, 与 compile<Foo>Expr 入口的写并存校验。
// 3.3+：按子系统（方法分派 / 调用解析 / intrinsic ...）把 throw 从
//      `src/compiler/compiler_*.cpp` 抠进来, 最终 codegen 不再抛语义错。
//
// 当前 pass 是 no-op, 主要保证拓扑落位; 由 `yux_frontend` 静态库提供,
// 不依赖 LLVM, 给 `yux-lsp` / 未来 `yux-check` 共用。
class SemaPass {
public:
    // sdkFile 用于跨文件符号解析 (例如调用 SDK 提供的函数 / 构造器). 单文件 / SDK 自构建
    // 时可传 nullptr; Compiler 处实际传入 `_yux ? _yux->sdkFile() : nullptr`.
    // sourcePath：当前文件的绝对路径，仅用于诊断渲染（DiagnosticEngine::emit
    // 渲染 warning 时要前缀 file:line:col）。空串 = LSP / 单测无路径，按 line:col 渲染。
    explicit SemaPass(p<FileNode> file, p<FileNode> sdkFile = nullptr,
                      string sourcePath = "");

    void run();

private:
    p<FileNode> _file;
    p<FileNode> _sdkFile;
    string _sourcePath;

    // Phase 3.3 前置.4: 当前所在 fn (用于 caller 的 #Fallible(E) 校验) +
    // try block 栈 (用于 ID-callee 错误传播 E7001/E7004/E7006/E7016).
    // 与 Compiler 的 `_currentFnNode` / `_tryCatchStack` 协议同步: 进入
    // visitFn 时 push, 离开时 pop; 进入 ExprTryCatchNode.tryBlock 时 push
    // 新的 seenErrTypes 层, visitBlock 完后 pop (catch arms 不在栈内).
    p<FnNode> _currentFn = nullptr;
    vector<vector<string>> _tryStack;

    // Bucket 2 (CURRENT-check.md): break outside loop (E3094) 校验. visitStmt
    // 进入 StatementLoopNode 时 ++, 离开时 --; StatementBreakNode 命中且 depth=0
    // 时抛 E3094. Compiler 端 compileBreakStatement 内 inline throw 保留作幂等
    // 防御性双跑.
    int _loopDepth = 0;

    // Phase 3.4.d.2: 当前所在 struct impl 名 (用于私有字段可见性 E3042).
    // 与 Compiler 的 `_currentStructName` 同步: 进入 struct impl 方法 visit
    // 时 set, 离开时 clear; sema 不下钻泛型 impl, 这里恒为非 `$<...>` 形态.
    // 空串表示自由 fn (任何私有字段访问都报错).
    string _currentStructName;

    void visitFn(p<FnNode> fn);
    void visitStmt(p<StatementNode> stmt);
    void visitBlock(p<StatementBlockNode> block);
    void visitExpr(p<ExprNode> expr);

    // Bucket 6 单点 (CURRENT-check.md): 二元运算符方法解析的 SemaPass 入口
    // (E3073 + byval hint). 把"算 leftType / rightType + 形态 gate + 调
    // sema::validateBinOpMethodResolution"封进来, 给 ExprAddSub /
    // ExprMulDivMod / ExprBinOp / ExprCompare 四个 visit 分支共用.
    //
    // 仅在 leftType 是非 builtin / 非 Ref/Rc/Array/Heap/Weak/Nullable/Ptr/Tuple
    // 容器 / 已知非泛型 struct 时调用; 泛型 struct / 模板形参 / lambda 形参 (getType
    // 抛错) 一律跳过, 留 Compiler 兜底. methodName 由调用方按 op 映射 (plus/minus/...).
    void tryValidateBinOpMethod(p<ExprNode> leftExpr, p<ExprNode> rightExpr,
                                 const string& methodName, int line, int col);
};

#endif //YUX_LANG_SEMA_PASS_H

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
    explicit SemaPass(p<FileNode> file, p<FileNode> sdkFile = nullptr);

    void run();

private:
    p<FileNode> _file;
    p<FileNode> _sdkFile;

    void visitFn(p<FnNode> fn);
    void visitStmt(p<StatementNode> stmt);
    void visitBlock(p<StatementBlockNode> block);
    void visitExpr(p<ExprNode> expr);
};

#endif //YUX_LANG_SEMA_PASS_H

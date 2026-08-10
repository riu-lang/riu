# yux-lang 架构图

本文件汇总 yux-lang 仓库的关键架构图，便于快速上手与跨模块讨论。图示用 Mermaid，配合 [RULES.md](RULES.md) / [rules/directory.md](rules/directory.md) / [rules/sema-codegen.md](rules/sema-codegen.md) 阅读。

权威源：`yux/ast/yux*.g4`（语法）、`yux/` C++ 源码、`xmake.lua`（构建拓扑）。图与代码冲突时以代码为准，**回头更新本文档**而非反过来。

---

## 1. 构建 target 依赖（xmake.lua）

```mermaid
graph LR
    antlr[antlr4_static<br/>C++17 静态库]
    zlib[zlib]
    llvm[llvm<br/>phony, CMake 驱动]

    frontend[yux_frontend<br/>静态库, 0 LLVM]
    codegen[yux_codegen<br/>静态库, 依赖 LLVM]

    yux[yux<br/>主二进制 CLI]
    lsp[yux-lsp<br/>LSP 服务器]
    ast[yux-ast<br/>parse tree dump]
    check[yux-check<br/>快速语义检查]

    antlr --> frontend
    frontend --> codegen
    zlib --> codegen
    llvm --> codegen

    codegen --> yux
    frontend --> lsp
    frontend --> ast
    frontend --> check

    classDef nollvm fill:#e0f3e0,stroke:#3a3
    classDef llvmDep fill:#fde2e2,stroke:#c33
    class frontend,lsp,ast,check nollvm
    class codegen,yux llvmDep
```

绿色 = 0 LLVM 依赖（编译快、可独立分发）；红色 = 链接整个 LLVM/lld。

---

## 2. 编译管线（`yux build` 主路径）

```mermaid
flowchart TB
    src[.yux 源文件] --> lex[yuxLexer<br/>ANTLR4 生成]
    lex --> parse[yuxParser<br/>ANTLR4 生成]
    parse --> tree[ParseTree]
    tree --> builder[ast::AstBuilder<br/>yux/ast/ast_builder.cpp]
    builder --> ast[AST 节点<br/>yux/ast/node/*]

    ast --> sema[SemaPass<br/>yux/frontend/sema/sema_pass.cpp]
    sema -. 双跑防御 .-> compiler

    ast --> analyzer[Analyzer<br/>borrow / const_mut /<br/>flow_terminate /<br/>spec_impl / symbol_suggest]
    analyzer --> compiler

    ast --> compiler[Compiler<br/>yux/yux/compiler/compiler*.cpp]
    compiler --> ir[LLVM IR]
    ir --> llc[LLVM 后端]
    llc --> obj[.obj]
    obj --> lld[lld 链接]
    sdk[(sdk/yux 自举 runtime<br/>yux.lib)] --> lld
    lld --> exe[可执行 / .lib / .dll]

    style sema fill:#e0f3e0
    style analyzer fill:#e0f3e0
    style compiler fill:#fde2e2
```

注意 `SemaPass` 与 `Compiler` 的 throw 关系——见 [§4](#4-sema--codegen-双段分离)。

---

## 3. 各端入口对管线的复用

```mermaid
flowchart LR
    subgraph FE [yux_frontend]
        L[Lexer + Parser]
        B[AstBuilder]
        A[Analyzer]
        S[SemaPass]
        D[diagnostic / format /<br/>build_cache / sdk_loader]
    end

    subgraph CG [yux_codegen]
        C[Compiler + LLVM IR<br/>compiler*.cpp]
    end

    yuxbin[yux<br/>build / run / jit] --> L & B & A & S & C
    lspbin[yux-lsp<br/>completion / diag /<br/>semantic_tokens] --> L & B & A & S & D
    astbin[yux-ast<br/>tools/ast_main.cpp] --> L
    checkbin[yux-check<br/>tools/check_main.cpp] --> L & B & A & S

    style CG fill:#fde2e2
    style FE fill:#e0f3e0
```

`yux-check` 与 `yux-lsp` 都建立在 frontend 之上，**保证语义检查不需要 LLVM**。

---

## 4. Sema / Codegen 双段分离

长期目标：`yux-check` 与 `yux build` 错误集等价。当前是半完成态，新代码必须遵守 [rules/sema-codegen.md](rules/sema-codegen.md)。

```mermaid
flowchart TB
    ast[AST] --> sema{SemaPass.visit*}

    sema -- "已迁移错误码<br/>kMigratedCodes 白名单" --> throwSema[throw YuxError]
    sema -- "未迁移路径<br/>(泛型体 / lambda 体 /<br/>所有 stmt / target-type / alias 环)" --> skip[沉默通过]

    throwSema --> userErr[(诊断输出)]
    skip --> compiler[Compiler<br/>compiler_*.cpp]

    compiler -- "兜底 throw YuxError" --> userErr
    compiler --> ir[LLVM IR]

    classDef green fill:#e0f3e0,stroke:#3a3
    classDef red fill:#fde2e2,stroke:#c33
    class sema,throwSema green
    class compiler,ir red
```

规则要点（写新 C++ 时必看）：

- `yux/frontend/sema/` 禁止 `#include "llvm/..."`，禁止访问 `IRBuilder` / `_module`。
- 让 sema 接管某错误码 → **必须同步更新 `kMigratedCodes` 白名单**，否则 sema 自身 try/catch 吞错、无测试能捕获。
- 新增 AST / 表达式类 → 在 `SemaPass::visitExpr` 加 dispatch 分支（即使是空占位），否则 sema 静默 skip 整个子树。
- sema 接管后 Compiler 端原 inline throw / validate 调用直接删除（v0.16 收尾）。

---

## 5. AST 节点家族

```mermaid
classDiagram
    class Node {
        +Loc loc
    }
    class FileNode {
        +imports
        +decls
    }
    class FnNode
    class StructNode
    class EnumNode
    class SpecNode
    class AliasNode
    class GlobalConstNode
    class ExprNode
    class LiteralNode
    class StatementNode
    class TypeNode

    Node <|-- FileNode
    Node <|-- FnNode
    Node <|-- StructNode
    Node <|-- EnumNode
    Node <|-- SpecNode
    Node <|-- AliasNode
    Node <|-- GlobalConstNode
    Node <|-- ExprNode
    Node <|-- StatementNode
    Node <|-- TypeNode
    ExprNode <|-- LiteralNode

    FileNode "1" o-- "*" FnNode
    FileNode "1" o-- "*" StructNode
    FileNode "1" o-- "*" EnumNode
    FileNode "1" o-- "*" SpecNode
    FileNode "1" o-- "*" AliasNode
    FileNode "1" o-- "*" GlobalConstNode
    FnNode "1" o-- "*" StatementNode
    StatementNode "1" o-- "*" ExprNode
```

定义在 `yux/ast/node/*.h`；新增节点务必同步 `SemaPass::visitExpr` 与 mangler/builder 路径。

---

## 6. LSP 子系统

```mermaid
flowchart LR
    client[VSCode / IDEA / Claude Code] -- JSON-RPC --> io[lsp_io]
    io --> server[lsp_server]
    server --> ws[workspace<br/>多文件状态]
    ws --> doc[document<br/>源文件 + AST 缓存]
    doc --> FE[yux_frontend<br/>Lexer/Parser/Builder/Sema]
    server --> comp[completion]
    server --> sym[symbol_lookup]
    server --> tok[semantic_tokens]
    server --> diag[diagnostics<br/>← sema + analyzer]
    comp & sym & tok & diag --> doc
```

入口 `yux/lsp/lsp_main.cpp`，编为 `yux-lsp`。

---

## 7. 仓库目录速览

详见 [rules/directory.md](rules/directory.md)。一句话版：

- `yux/`：编译器实现（`yux/` 零 LLVM；`yux/yux/compiler/` 全 LLVM；`yux/analyzer/` 语义检查；`yux/lsp/` LSP；`yux/frontend/tools/` 工具）
- `sdk/yux/`：自举 runtime（独立 yux 项目 → `yux.lib`）
- `yux/ast/gen/`：ANTLR 生成代码（不要手改）
- `docs/`：中文教程 + `docs/spec/` 规范草案
- `tests/`：单文件用例 + 项目用例（前缀分组）
- `plugins/`：编辑器 / Claude Code 插件

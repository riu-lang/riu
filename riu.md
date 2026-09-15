# riu-lang 架构图

本文件汇总 riu-lang 仓库的关键架构图，便于快速上手与跨模块讨论。图示用 Mermaid，配合 [RULES.md](RULES.md) / [rules/sema-codegen.md](rules/sema-codegen.md) 阅读。

权威源：`riu/ast/riu*.g4`（语法）、`riu/` C++ 源码、`BUILD.gn` / `build/`（构建拓扑）。图与代码冲突时以代码为准，**回头更新本文档**而非反过来。

---

## 1. 构建 target 依赖（GN）

```mermaid
graph LR
    antlr[antlr4_static<br/>C++17 静态库]
    zlib[zlib]
    llvm[llvm<br/>GN + Ninja]
    astlib[riu_ast]
    analyzer[riu_analyzer]
    generic[riu_generic<br/>静态库, 0 LLVM]
    rt[riurt<br/>C99]

    frontend[riu_frontend<br/>静态库, 0 LLVM]

    riu[riu<br/>主二进制 CLI]
    lsp[riu-lsp<br/>LSP 服务器]
    ast[riu-ast<br/>parse tree dump]
    check[riu-check<br/>快速语义检查]
    runner[riu-test-runner]

    antlr --> astlib
    astlib --> analyzer
    astlib --> generic
    astlib --> frontend
    analyzer --> frontend
    generic --> frontend
    generic --> riu
    frontend --> riu
    zlib --> riu
    llvm --> riu
    rt --> riu
    frontend --> lsp
    frontend --> ast
    frontend --> check

    classDef nollvm fill:#e0f3e0,stroke:#3a3
    classDef llvmDep fill:#fde2e2,stroke:#c33
    class frontend,lsp,ast,check,astlib,analyzer,generic,antlr,runner nollvm
    class llvm,riu llvmDep
```

绿色 = 0 LLVM 依赖（编译快、可独立分发）；红色 = 链接整个 LLVM/lld。

GN 箭头是「被谁链接」。include 方向相反：`riu_ast` 不得 `#include "sema/..."` / `"analyzer/..."`，也不得在 `BUILD.gn` 加 `include_dirs = [ "//riu/frontend" ]`。

```
AST（数据 + 注解槽）  ←  Generic（替换栈；实例表仍在 Compiler）  ←  Sema / Analyzer
                              ↑
                          Codegen（LLVM）
```

名字查找：`riu/ast/name_lookup.h`（已加载模块图，0 LLVM）。类型计算由 Sema 写槽；codegen 读槽，泛型 subst 帧用 `structuralType()` 回退。`riu_generic` 0 LLVM；`applySubst` / 替换栈在 generic，struct/fn 实例表 2.3 起迁入。

---

## 2. 编译管线（`riu build` 主路径）

```mermaid
flowchart TB
    src[.ut 源文件] --> lex[riuLexer<br/>ANTLR4 生成]
    lex --> parse[riuParser<br/>ANTLR4 生成]
    parse --> tree[ParseTree]
    tree --> builder[ast::AstBuilder<br/>riu/ast/ast_builder.cpp]
    builder --> ast[AST 节点<br/>riu/ast/node/*]

    ast --> sema[SemaPass<br/>riu/frontend/sema/sema_pass.cpp]
    sema -. 双跑防御 .-> compiler

    ast --> analyzer[Analyzer<br/>borrow / const_mut /<br/>flow_terminate /<br/>spec_impl / symbol_suggest]
    analyzer --> compiler

    ast --> compiler[Compiler<br/>riu/riu/compiler/compiler*.cpp]
    compiler --> ir[LLVM IR]
    ir --> llc[LLVM 后端]
    llc --> obj[.obj]
    obj --> lld[lld 链接]
    sdk[(sdk/riu 自举 runtime<br/>riu.lib)] --> lld
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
    subgraph FE [riu_frontend]
        L[Lexer + Parser]
        B[AstBuilder]
        A[Analyzer]
        S[SemaPass]
        D[diagnostic / format /<br/>build_cache / sdk_loader]
    end

    subgraph CG [riu_codegen]
        C[Compiler + LLVM IR<br/>compiler*.cpp]
    end

    riubin[riu<br/>build / run / jit] --> L & B & A & S & C
    lspbin[riu-lsp<br/>completion / diag /<br/>semantic_tokens] --> L & B & A & S & D
    astbin[riu-ast<br/>tools/ast_main.cpp] --> L
    checkbin[riu-check<br/>tools/check_main.cpp] --> L & B & A & S

    style CG fill:#fde2e2
    style FE fill:#e0f3e0
```

`riu-check` 与 `riu-lsp` 都建立在 frontend 之上，**保证语义检查不需要 LLVM**。

---

## 4. Sema / Codegen 双段分离

长期目标：`riu-check` 与 `riu build` 错误集等价。当前是半完成态，新代码必须遵守 [rules/sema-codegen.md](rules/sema-codegen.md)。

```mermaid
flowchart TB
    ast[AST] --> sema{SemaPass.visit*}

    sema -- "getType RiuError<br/>默认重抛" --> throwSema[throw RiuError]
    sema -- "缺口<br/>(无具体实例的泛型 struct 体)" --> skip[两边都不查]

    throwSema --> userErr[(诊断输出)]
    skip --> compiler[Compiler<br/>compiler_*.cpp]

    compiler -- "兜底 throw RiuError" --> userErr
    compiler --> ir[LLVM IR]

    classDef green fill:#e0f3e0,stroke:#3a3
    classDef red fill:#fde2e2,stroke:#c33
    class sema,throwSema green
    class compiler,ir red
```

规则要点（写新 C++ 时必看）：

- `riu/ast/` 禁止 `#include "sema/..."` / `"analyzer/..."` / `"tools/..."`（frontend 路径）。
- `riu/frontend/sema/` 禁止 `#include "llvm/..."`，禁止访问 `IRBuilder` / `_module`。
- 让 sema 接管某错误码 → **默认即由 SemaPass 重抛**。若必须暂留 Compiler（假阳性），加入 `kDeferredCodes`，禁止静默吞。
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

定义在 `riu/ast/node/*.h`；新增节点务必同步 `SemaPass::visitExpr` 与 mangler/builder 路径。

---

## 6. LSP 子系统

```mermaid
flowchart LR
    client[VSCode / IDEA / Claude Code] -- JSON-RPC --> io[lsp_io]
    io --> server[lsp_server]
    server --> ws[workspace<br/>多文件状态]
    ws --> doc[document<br/>源文件 + AST 缓存]
    doc --> FE[riu_frontend<br/>Lexer/Parser/Builder/Sema]
    server --> comp[completion]
    server --> sym[symbol_lookup]
    server --> tok[semantic_tokens]
    server --> diag[diagnostics<br/>← sema + analyzer]
    comp & sym & tok & diag --> doc
```

入口 `riu/lsp/lsp_main.cpp`，编为 `riu-lsp`。

---

## 7. 仓库目录速览

详见 [RULES.md](RULES.md) 目录。一句话版：

- `riu/`：编译器实现（`riu/` 零 LLVM；`riu/riu/compiler/` 全 LLVM；`riu/analyzer/` 语义检查；`riu/lsp/` LSP；`riu/frontend/tools/` 工具）
- `sdk/riu/`：自举 runtime（独立 riu 项目 → `riu.lib`）
- `riu/ast/gen/`：ANTLR 生成代码（不要手改）
- `docs/`：中文教程 + `docs/spec/` 规范草案
- `tests/`：单文件用例 + 项目用例（前缀分组）
- `plugins/`：编辑器 / Claude Code 插件

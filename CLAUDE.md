# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: [AGENTS.md](AGENTS.md) for detailed project structure, [语法.md](语法.md) for language syntax, [README.md](README.md) for user-facing overview.

## Hard rules

- **Do not modify `src/yux.g4`** (the ANTLR4 grammar). If a task seems to require grammar changes, stop and ask the user — list the problems and what would need to change.
- Environment is Windows PowerShell with **Clang only** (no MSVC env vars). LLVM is expected on PATH via `llvm/bin`.
- `build/windows/x64/debug` should be on PATH so the `yux` compiler can be invoked directly after building.

## Common commands

```powershell
# Sync third_party dependencies (run once after clone)
./sync-deps.ps1

# Build the compiler
xmake build yux

# Compile a .yux source (outputs to build/)
yux main.yux
yux --emit-ir input.yux    # also emit .ll
yux -d input.yux           # debug IR dump (Debug builds only; voluminous — pipe through tail)

# Smoke-test the compiler on a single source
yux main.yux
./build/main.exe
```

**Testing status:** the compiler is incomplete — `tests/cases/*.yux` and the `yux_test` googletest suite are **not** currently usable as a regression harness. For now, verify changes by compiling `main.yux` (or a small ad-hoc `.yux` written for the specific feature under test) and running the resulting `.exe`. Only revisit `tests/` once the user says the suite is back in scope.

## Architecture

The compiler is a single binary (`yux.exe`) that takes a `.yux` source through the full pipeline to a linked executable — no external assembler/linker invocation:

```
.yux → ANTLR4 Lexer/Parser → ASTBuilder → semantic analysis
     → Compiler (LLVM IR) → LLVM codegen → LLD link → .exe
```

Key source boundaries in `src/`:
- `main.cpp` — CLI entry, arg parsing
- `yux.cpp/h` — top-level compiler driver orchestrating the pipeline
- `ast_builder.cpp/h` — walks the ANTLR parse tree into AST nodes under `src/node/` (`expr_node`, `fn_node`, `struct_node`, `statement_node`)
- `compiler.cpp/h` — AST → LLVM IR
- `build_cache.cpp/h` — source-file mtime+size cache stored at `build/build.cache`; skips recompilation of unchanged `.yux` files
- ANTLR-generated code lives in `gen/` (not `src/`)

The `sdk/` directory contains the bootstrap runtime (written in yux itself) — it compiles to `build/sdk.ll` / `build/sdk.obj` and is linked into every yux program.

## Build output layout — careful when cleaning

`build/` is **shared between xmake and the yux compiler itself**:

- xmake writes to `build/windows/x64/debug/` and the dotted dirs (`.objs/`, `.deps/`, `.build_cache/`, etc.)
- yux writes `build/*.exe`, `build/*.ll`, `build/*.obj`, `build/build.cache` directly

Safe targeted cleanup: delete `build/*.exe build/*.ll build/*.obj build/build.cache`. For a full reset use `xmake clean -a`. Do not nuke `build/` wholesale.

## Language conventions that affect codegen/tests

- No implicit type conversions anywhere — use `.to_<type>()` methods.
- Comments: line comments match `^\s*/.*` (leading `/`, indent allowed); trailing comments use ` ;`. A trailing `/` is **not** a comment.
- Spacing is enforced: space after keywords, around binary operators, after `,`; no space inside `()` / `[]`.
- Keywords: `fn var val cval if elif else ret break null true false loop struct`.
- Generics/runtime types baked into the language: `Ref<T>`, `Box<T>` (refcounted), `Ptr<T>`, `Array<T>`.

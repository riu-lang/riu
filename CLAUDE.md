# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: [AGENTS.md](AGENTS.md) for detailed project structure, [语法.md](语法.md) for language syntax, [README.md](README.md) for user-facing overview.

## Hard rules

- **Do not modify `src/yux.g4`** (the ANTLR4 grammar). If a task seems to require grammar changes, stop and ask the user — list the problems and what would need to change.
- Environment is Windows PowerShell with **Clang only** (no MSVC env vars). LLVM is expected on PATH via `llvm/bin`.
- `build/windows/x64/debug` defaults on PATH so the `yux` compiler can be invoked directly after building.
- **Task/bug tracking** — use both files, don't mix:
  - `CURRENT.md` holds the *in-progress multi-step task*. **When given a multi-step task, write the phased plan into `CURRENT.md` first** to track it. Read before starting, update as phases complete, remove the entry when the whole task is done.
  - `BUGS.md` holds *newly discovered* bugs (unrelated to the current change, requiring heavy investigation, or temporarily worked around). Follow the template, pause the related task, and tell the user.

## Common commands

```powershell
# Sync third_party dependencies (run once after clone)
./sync-deps.ps1

# Build the compiler
xmake build yux

# Single-file mode — flat output in <srcDir>/build/
yux main.yux               # produces <srcDir>/build/main.exe
yux --emit-ir input.yux    # also emit .ll
yux -d input.yux           # debug IR dump (Debug builds only; voluminous — pipe through tail)

# Project mode — run at project root (must contain yux.toml)
yux build <name>           # <name> must match `name` in yux.toml; entry comes from toml `entry`
                           # outputs <projectRoot>/build/<name>/<name>.exe

# Smoke-test the compiler on a single source
yux main.yux
./build/main.exe
```

`yux.toml` fields (see 语法.md): `name` (project / exe name), `entry` (entry .yux relative to project root), `version`. Single-file mode ignores yux.toml entirely — no project name subdir.

**Testing:**
1. **Smoke test first**: Use `yux main.yux && ./build/main.exe` ( or create new yux file) for quick validation after changes.
2. **Full suite**: Once smoke test passes, run `xmake test` to verify all test cases.

The regression suite runs via xmake's native test mechanism (no googletest / CMake). Use `xmake test` to run all cases under `tests/cases/`, or `xmake test yux_tests/<name>` for a single case. The runner (`tests/xmake.lua`, `yux_tests` target) invokes the built `yux` on each `.yux` in **single-file mode** and compares stdout to the paired `.expected`; for `error/err_*.yux` it expects compilation to fail. Per-case products land at `tests/cases/build/<stem>.exe` (or `tests/cases/error/build/...`). Project-mode tests will be a separate harness later. When a case and the language disagree, update the case — `src/yux.g4` and `语法.md` are authoritative.

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
- yux single-file mode writes `<srcDir>/build/*.exe`, `*.ll`, `*.obj`, `*.obj.cache` flat (multi-segment modules `A.B.C` go to `<buildDir>/A/B/C.obj`)
- yux project mode writes under `<projectRoot>/build/<projectName>/` for the main module + single-segment imports; multi-segment modules follow their dotted path under `build/`

Safe targeted cleanup: delete `build/*.exe build/*.ll build/*.obj build/*.obj.cache`. For a full reset use `xmake clean -a`. Do not nuke `build/` wholesale.

## Language conventions that affect codegen/tests

- No implicit type conversions anywhere — use `.to_<type>()` methods.
- Untyped int literals default to `i32` but are inferred from context (binop peer type, declared variable type, return type, or unique overload match). Ambiguous overload matches error — add a type suffix (e.g. `2u8`) to disambiguate.
- Comments: line comments match `^\s*/.*` (leading `/`, indent allowed); trailing comments use ` ;`. A trailing `/` is **not** a comment.
- Mandatory trailing `;` rules:
  - `ret;` for early return in void functions must end with `;`
  - `break;` for loop exit must end with `;`
  - `expression ends with `;` is denoted empty return, else it returns the expression value.
- Spacing is enforced: space after keywords, around binary operators, after `,`; no space inside `()` / `[]`.
- Keywords: `fn var val cval if elif else ret break null true false loop struct`.
- Generics/runtime types baked into the language: `Ref<T>`, `Box<T>` (refcounted), `Ptr<T>`, `Array<T>`.
- String literals: `"..."` supports escapes `\n \t \\ \" \'`; raw strings `r"..."` treat content verbatim (no escape processing).
- Codepoint literals: `c'X'` is a single Unicode codepoint of type `u32`. Only one character or one escape allowed between the quotes; supported escapes are `\n \r \t \v \b \0 \\ \'`. Numeric escapes (`\xNN`, `\uNNNN`) are **not** supported.

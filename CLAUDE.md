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

# Project mode — run at project root (must contain yux.toml)
yux build <name>           # <name> must match `name` in yux.toml; entry comes from toml `entry`
                           # outputs <projectRoot>/build/<name>/<name>.exe
yux build <name> --emit-ir # also emit .ll
yux build <name> -d        # debug IR dump (Debug builds only; voluminous — pipe through tail)

# Smoke-test the compiler against examples/main (has yux.toml with name="test")
cd examples/main && yux build test && ./build/test/test.exe
```

`yux.toml` fields (see 语法.md): `name` (project / exe name), `entry` (entry .yux relative to project root), `version`.

Single-file mode (`yux <file>.yux`) still exists in the binary and is what the test harness drives internally, but it is deprecated for user-facing use and will be removed — model new work and examples on project mode. Do not add new docs or examples that invoke `yux` on a bare `.yux` file.

**Testing:**
1. **Smoke test first**: build & run `examples/main` (or a small throwaway project) for quick validation after changes.
2. **Full suite**: once the smoke test passes, run `xmake test` to verify all test cases.

The regression suite runs via xmake's native test mechanism (no googletest / CMake). Use `xmake test` to run all cases under `tests/cases/`, or `xmake test yux_tests/<name>` for a single case. The runner (`tests/xmake.lua`, `yux_tests` target) currently invokes the built `yux` on each `.yux` in single-file mode (this is the last remaining internal use of that mode — a project-per-case harness will replace it) and compares stdout to the paired `.expected`; for `error/err_*.yux` it expects compilation to fail. Per-case products land at `tests/cases/build/<stem>.exe` (or `tests/cases/error/build/...`). When a case and the language disagree, update the case — `src/yux.g4` and `语法.md` are authoritative.

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
- `build_cache.cpp/h` — source-file mtime+size cache written as `<objPath>.cache` next to each object; skips recompilation when both match
- ANTLR-generated code lives in `gen/` (not `src/`)

The `sdk/` directory contains the bootstrap runtime (written in yux itself) — it compiles to `build/sdk.ll` / `build/sdk.obj` and is linked into every yux program.

## Build output layout — careful when cleaning

`build/` is **shared between xmake and the yux compiler itself**:

- xmake writes to `build/windows/x64/debug/` and the dotted dirs (`.objs/`, `.deps/`, `.build_cache/`, etc.)
- yux single-file mode writes `<srcDir>/build/*.exe`, `*.ll`, `*.obj`, `*.obj.cache` flat (multi-segment modules `A.B.C` go to `<buildDir>/A/B/C.obj`)
- yux project mode writes under `<projectRoot>/build/<projectName>/` for the main module + single-segment imports; multi-segment modules follow their dotted path under `build/`

Safe targeted cleanup: delete `build/*.exe build/*.ll build/*.obj build/*.obj.cache`. For a full reset use `xmake clean -a`. Do not nuke `build/` wholesale.

## Writing yux code

When generating or modifying yux source code, follow these rules:

**Documentation priority:**
1. **Read docs first** — Always consult `docs/*.md` (especially `基础语法.md`, `类型系统.md`, `函数.md`, `结构体.md`, `控制流.md`) before writing yux code
2. **Grammar second** — If docs are insufficient, refer to `src/yux.g4` for precise syntax rules
3. **Compiler code last** — Only read compiler source (`src/*.cpp`, `src/node/*.cpp`) as a last resort to understand behavior
4. **Do NOT reference Rust** — yux is its own language with different semantics; do not assume Rust-like behavior

**Code style requirements:**
- **Comments**: Line comments start with `;` (can be indented); trailing comments use ` ;`
- **Spacing**: Space after keywords, around binary operators, after `,`; no space inside `()` / `[]`
- **Mandatory trailing `;`**:
  - `ret;` for early return in void functions must end with `;`
  - `break;` for loop exit must end with `;`
  - Expression statements ending with `;` return void; without `;` they return the expression value
- **No implicit conversions** — Use `.to_<type>()` methods explicitly

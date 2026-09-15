# Copyright (c) 2026. Yin-Jinlong@github
# MPL-2.0
"""LLVM GN compiler_wrapper：给 clang-cl 补上 /MD 或 /MDd，与 riu CRT 对齐。"""

from __future__ import annotations

import os
import subprocess
import sys


def _is_flag(arg: str) -> bool:
    # MSVC (/nologo) or GNU (-W...); path fragments like "Files/..." are not flags.
    return arg.startswith("-") or arg.startswith("/")


def _take_compiler(argv: list[str]) -> tuple[str, list[str]]:
    """Compiler path may be split on spaces when GN/ninja omit quotes."""
    if not argv:
        raise ValueError("missing compiler path")
    parts: list[str] = []
    for i, a in enumerate(argv):
        if parts and _is_flag(a):
            break
        parts.append(a)
        candidate = " ".join(parts)
        for path in (candidate, candidate + ".exe"):
            if os.path.isfile(path):
                return path, argv[i + 1 :]
    joined = " ".join(parts)
    raise FileNotFoundError(f"compiler not found: {joined!r}")


def main() -> int:
    argv = sys.argv[1:]
    crt: str | None = None
    i = 0
    while i < len(argv):
        if argv[i] == "--crt" and i + 1 < len(argv):
            crt = argv[i + 1]
            i += 2
            continue
        break
    try:
        compiler, rest = _take_compiler(argv[i:])
    except (ValueError, FileNotFoundError) as e:
        sys.stderr.write(f"llvm_cl_wrapper: {e}\n")
        return 2
    cmd = [compiler]
    if crt:
        cmd.append(crt)
    cmd.extend(rest)
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())

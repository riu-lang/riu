# Copyright (c) 2026. Yin-Jinlong@github
# MPL-2.0
"""LLVM GN compiler_wrapper：给 clang-cl 补上 /MD 或 /MDd，与 yux CRT 对齐。"""

from __future__ import annotations

import subprocess
import sys


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
    if i >= len(argv):
        sys.stderr.write("llvm_cl_wrapper: missing compiler path\n")
        return 2
    cmd = [argv[i]]
    if crt:
        cmd.append(crt)
    cmd.extend(argv[i + 1 :])
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())

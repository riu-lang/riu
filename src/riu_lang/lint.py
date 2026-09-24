"""clang-tidy wrapper via compile_commands.json."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

from riu_lang._console import err, info, log, ok
from riu_lang._cxx import excluded_lint, filter_cxx
from riu_lang._git import git_changed_files, relative_to_root
from riu_lang._paths import project_root

OUT_DIR = "build/windows/x64/debug"


def _compile_commands_files(root: Path, compile_commands: Path) -> list[str]:
    data = json.loads(compile_commands.read_text(encoding="utf-8"))
    out: list[str] = []
    seen: set[str] = set()
    for entry in data:
        rel = relative_to_root(root, entry["file"])
        if excluded_lint(rel):
            continue
        key = rel.lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(rel)
    return out


def show_help() -> None:
    print(
        """用法:
  ./lint              仅 git 变动/未跟踪的 C/C++
  ./lint --all        compile_commands.json 中的项目源
  ./lint riu/x.cpp    指定文件
  ./lint -h / --help  帮助

固定使用 build/windows/x64/debug/compile_commands.json（./build --gen-only）。提交须 0 warnings。"""
    )


def main(argv: list[str] | None = None) -> int:
    args = list(argv if argv is not None else sys.argv[1:])
    if any(a in ("-h", "--help", "-Help", "/?") for a in args):
        show_help()
        return 0

    root = project_root()
    out_dir = root / OUT_DIR.replace("/", "\\")
    compile_commands = out_dir / "compile_commands.json"
    flag_all = "--all" in args
    positional = [a for a in args if not a.startswith("--")]

    if positional:
        files = filter_cxx(root, positional, exclude=excluded_lint)
        if not files:
            log("no matching files.", dim=True)
            return 0
        info(f"scope: {len(files)} explicit file(s)")
    elif flag_all:
        if not compile_commands.is_file():
            err(f"Debug compile_commands.json not found at {compile_commands}. Run ./build --gen-only first.")
            return 2
        files = _compile_commands_files(root, compile_commands)
        info(f"scope: --all ({len(files)} file(s) from compile_commands.json)")
    else:
        files = filter_cxx(root, git_changed_files(root), exclude=excluded_lint)
        if not files:
            log("no git-changed C/C++ files; nothing to lint.", dim=True)
            return 0
        info(f"scope: git changed ({len(files)} file(s))")

    tidy = shutil.which("clang-tidy")
    if not tidy:
        err("clang-tidy not found in PATH; aborting.")
        return 2
    if not compile_commands.is_file():
        err(f"Debug compile_commands.json missing at {compile_commands}. Run ./build --gen-only")
        return 2

    log(f"clang-tidy: {tidy}", dim=True)
    log(f"compile_commands: {compile_commands}\n", dim=True)
    code = subprocess.run([tidy, f"-p{out_dir}", *files], cwd=root).returncode
    if code != 0:
        err(f"\nclang-tidy exited with status {code}")
        return code
    ok("\nclang-tidy done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

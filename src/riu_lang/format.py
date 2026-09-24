"""clang-format wrapper。"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from riu_lang._console import err, info, log, ok
from riu_lang._cxx import excluded_format, filter_cxx, SRC_EXTS
from riu_lang._git import git_changed_files, relative_to_root
from riu_lang._paths import project_root


def _walk_dir(root: Path, dir_path: Path, out: list[str]) -> None:
    if not dir_path.is_dir():
        return
    for item in dir_path.iterdir():
        rel = relative_to_root(root, item)
        if excluded_format(rel):
            continue
        if item.is_dir():
            _walk_dir(root, item, out)
        elif item.suffix.lower() in SRC_EXTS:
            out.append(str(item))


def _all_repo_files(root: Path) -> list[str]:
    out: list[str] = []
    for name in ("riu", "include", "sdk"):
        _walk_dir(root, root / name, out)
    return out


def show_help() -> None:
    print(
        """用法:
  ./format              仅 git 变动/未跟踪的 C/C++（clang-format -i）
  ./format --all        riu/ 下 C/C++
  ./format --check      dry-run + -Werror（pre-commit）
  ./format riu/x.cpp    指定文件
  ./format -h / --help  帮助"""
    )


def main(argv: list[str] | None = None) -> int:
    args = list(argv if argv is not None else sys.argv[1:])
    if any(a in ("-h", "--help", "-Help", "/?") for a in args):
        show_help()
        return 0

    root = project_root()
    flag_all = "--all" in args
    flag_check = "--check" in args
    positional = [a for a in args if not a.startswith("--")]

    if positional:
        candidates = positional
        info(f"scope: {len(positional)} explicit file(s)")
    elif flag_all:
        candidates = [relative_to_root(root, p) for p in _all_repo_files(root)]
        info(f"scope: --all ({len(candidates)} candidate file(s))")
    else:
        candidates = git_changed_files(root)
        info(f"scope: git changed ({len(candidates)} candidate file(s))")

    files = filter_cxx(root, candidates, exclude=excluded_format)
    if not files:
        log("nothing to do (no matching C/C++ source files).", dim=True)
        return 0

    clang = shutil.which("clang-format")
    if not clang:
        err("clang-format not found in PATH; aborting.")
        return 2
    log(f"clang-format: {clang}", dim=True)

    failed = 0
    batch_size = 50
    for i in range(0, len(files), batch_size):
        batch = files[i : i + batch_size]
        full_paths = [str(root / f.replace("/", "\\")) for f in batch]
        cf_args = (["--dry-run", "-Werror"] if flag_check else ["-i"]) + full_paths
        code = subprocess.run([clang, *cf_args], cwd=root).returncode
        if code != 0:
            failed += 1

    if flag_check:
        if failed:
            err(f"\n{failed} batch(es) report format diff. Run ``./format`` to fix.")
            return 1
        ok(f"\nall {len(files)} file(s) match clang-format style.")
    else:
        ok(f"\nformatted {len(files)} file(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

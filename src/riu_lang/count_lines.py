"""cloc 行数统计。"""

from __future__ import annotations

import subprocess
import sys

from riu_lang._paths import project_root


def main(argv: list[str] | None = None) -> int:
    args = list(argv if argv is not None else sys.argv[1:])
    root = project_root()
    commit = args[0] if args else "HEAD"
    lang_def = root / "riu_lang_def.txt"
    cloc = root / "bin" / "cloc-2.10.exe"
    if not cloc.is_file():
        print(f"cloc not found: {cloc}", file=sys.stderr)
        print("Run ./sync-deps.ps1 first to download cloc.", file=sys.stderr)
        return 1
    return subprocess.run(
        [str(cloc), commit, "--read-lang-def", str(lang_def), "--not-match-f", r"lock\."],
        cwd=root,
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())

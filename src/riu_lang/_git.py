"""git status 解析（lint / format 共用）。"""

from __future__ import annotations

import subprocess
from pathlib import Path


def git_changed_files(root: Path) -> list[str]:
    out = subprocess.check_output(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"],
        text=True,
        errors="replace",
    )
    files: list[str] = []
    for line in out.splitlines():
        if len(line) < 4:
            continue
        status = line[:2]
        rest = line[3:]
        if status[0] in "RC" or status[1] in "RC":
            arrow = " -> "
            if arrow in rest:
                rest = rest.split(arrow, 1)[1]
        if "D" in status:
            continue
        if rest.startswith('"') and rest.endswith('"'):
            rest = rest[1:-1].replace('\\"', '"')
        files.append(rest)
    return files


def relative_to_root(root: Path, path: str | Path) -> str:
    p = Path(path)
    if not p.is_absolute():
        return p.as_posix().lstrip("./")
    try:
        return p.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return p.as_posix()

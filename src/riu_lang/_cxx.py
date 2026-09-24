"""C/C++ 源文件筛选（lint / format 共用）。"""

from __future__ import annotations

from pathlib import Path

from riu_lang._git import relative_to_root

SRC_EXTS = {".h", ".hpp", ".cpp", ".cc", ".cxx"}


def excluded_lint(rel: str) -> bool:
    n = rel.replace("/", "\\")
    parts = ("gen", "third_party", "build", ".cache")
    for part in parts:
        if n == part or n.startswith(part + "\\") or ("\\" + part + "\\") in n:
            return True
    return False


def excluded_format(rel: str) -> bool:
    n = rel.replace("/", "\\")
    return (
        n.startswith("gen\\")
        or n.startswith("third_party\\")
        or "\\build\\" in n
        or n.startswith("build\\")
        or "\\.cache\\" in n
    )


def filter_cxx(
    root: Path,
    files: list[str],
    *,
    exclude,
) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for f in files:
        rel = relative_to_root(root, f)
        ext = Path(rel).suffix.lower()
        if ext not in SRC_EXTS:
            continue
        if exclude(rel):
            continue
        full = root / rel.replace("/", "\\")
        if not full.is_file():
            continue
        key = rel.lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(rel)
    return out

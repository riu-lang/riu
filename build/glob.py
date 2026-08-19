# Copyright (c) 2026. Yin-Jinlong@github
# MPL-2.0
"""Print //relative source paths for GN exec_script(..., "list lines")."""

from __future__ import annotations

import os
import sys


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write("usage: glob.py <repo_root> <rel_dir> <ext> [--flat]\n")
        return 2
    repo = os.path.normpath(sys.argv[1])
    rel = sys.argv[2].replace("\\", "/").strip("/")
    ext = sys.argv[3]
    if not ext.startswith("."):
        ext = "." + ext
    flat = "--flat" in sys.argv[4:]

    base = os.path.join(repo, *rel.split("/"))
    if not os.path.isdir(base):
        sys.stderr.write(f"glob.py: not a directory: {base}\n")
        return 1

    out: list[str] = []
    if flat:
        for name in sorted(os.listdir(base)):
            full = os.path.join(base, name)
            if os.path.isfile(full) and name.endswith(ext):
                out.append(f"//{rel}/{name}")
    else:
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for name in sorted(filenames):
                if not name.endswith(ext):
                    continue
                full = os.path.join(dirpath, name)
                rel_path = os.path.relpath(full, repo).replace("\\", "/")
                out.append(f"//{rel_path}")

    sys.stdout.write("\n".join(out))
    if out:
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""仓库路径与工具定位。"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


def project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def find_uv(root: Path) -> str:
    local = root / "bin" / "uv.exe"
    if local.is_file():
        return str(local)
    found = shutil.which("uv")
    if found:
        return found
    raise SystemExit("uv not found (run ./sync-deps.ps1)")


def find_tool(root: Path, name: str) -> Path:
    local = root / "bin" / f"{name}.exe"
    if local.is_file():
        return local
    found = shutil.which(name)
    if found:
        return Path(found)
    raise SystemExit(
        f"{name} not found (run ./sync-deps.ps1 for {name}, or put {name}.exe in bin/ / PATH)"
    )


def venv_python(root: Path) -> Path:
    py = root / ".venv" / "Scripts" / "python.exe"
    if not py.is_file():
        raise SystemExit("Python venv not found (run ./sync-deps.ps1)")
    return py


def gn_script_executable(root: Path) -> Path:
    """GN --script-executable（须为可执行文件，不能用 .cmd）。"""
    return venv_python(root)


def gn_path(path: Path | str) -> str:
    return Path(path).as_posix()

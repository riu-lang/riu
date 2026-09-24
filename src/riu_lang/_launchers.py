"""生成根目录 cmd / sh 便携壳（uv run <command>）。"""

from __future__ import annotations

from pathlib import Path

from riu_lang._console import log, warn

# sync-deps.ps1 手写保留；其余工具生成 cmd/sh。
TOOL_COMMANDS = ("build", "lint", "format", "count-lines")


def _cmd_body(command: str) -> str:
    return (
        "@echo off\r\n"
        "setlocal\r\n"
        'set "ROOT=%~dp0"\r\n'
        'set "UV=%ROOT%bin\\uv.exe"\r\n'
        'if not exist "%UV%" set "UV=uv"\r\n'
        f'"%UV%" run {command} %*\r\n'
        "exit /b %ERRORLEVEL%\r\n"
    )


def _sh_body(command: str) -> str:
    return (
        "#!/usr/bin/env sh\n"
        "set -e\n"
        'ROOT="$(cd "$(dirname "$0")" && pwd)"\n'
        'UV="$ROOT/bin/uv"\n'
        '[ -x "$UV" ] || UV=uv\n'
        f'exec "$UV" run {command} "$@"\n'
    )


def write_launchers(root: Path, *, dry_run: bool = False) -> None:
    for command in TOOL_COMMANDS:
        cmd_path = root / f"{command}.cmd"
        sh_path = root / f"{command}.sh"
        cmd_text = _cmd_body(command)
        sh_text = _sh_body(command)
        if dry_run:
            log(f"Would write {cmd_path.name}, {sh_path.name}")
            continue
        if not cmd_path.is_file() or cmd_path.read_text(encoding="utf-8", errors="replace") != cmd_text:
            cmd_path.write_text(cmd_text, encoding="utf-8", newline="\r\n")
            warn(f"updated {cmd_path.name}")
        if not sh_path.is_file() or sh_path.read_text(encoding="utf-8", errors="replace") != sh_text:
            sh_path.write_text(sh_text, encoding="utf-8", newline="\n")
            warn(f"updated {sh_path.name}")

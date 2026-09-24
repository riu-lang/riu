"""按 DEPS.json 同步 third_party / bin（ps-sync-deps、uv venv）。"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from riu_lang._console import info, log, ok, warn
from riu_lang._launchers import write_launchers
from riu_lang._paths import project_root

PYTHON_MINOR = "3.12"


def _llvm_head(llvm_dir: Path) -> str | None:
    if not (llvm_dir / ".git").is_dir():
        return None
    try:
        out = subprocess.check_output(
            ["git", "-C", str(llvm_dir), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return out.strip() or None


def _write_llvm_stamp(root: Path, commit: str) -> None:
    if not commit.strip():
        return
    stamp = root / "third_party" / "llvm.sync_commit"
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(commit.strip() + "\n", encoding="utf-8")


def _venv_minor(venv_python: Path) -> str | None:
    if not venv_python.is_file():
        return None
    try:
        out = subprocess.check_output(
            [str(venv_python), "-c", "import sys; print(f'{sys.version_info[0]}.{sys.version_info[1]}')"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return out.strip() or None


def ensure_python_shim(root: Path, *, dry_run: bool) -> None:
    bin_dir = root / "bin"
    cmd_path = bin_dir / "python.cmd"
    content = '@echo off\r\n"%~dp0..\\.venv\\Scripts\\python.exe" %*\r\n'
    if dry_run:
        warn(f"Would write {cmd_path}")
        return
    bin_dir.mkdir(parents=True, exist_ok=True)
    if not cmd_path.is_file() or cmd_path.read_text(encoding="utf-8") != content:
        cmd_path.write_text(content, encoding="utf-8", newline="\r\n")


def ensure_python_venv(root: Path, *, dry_run: bool) -> None:
    venv_dir = root / ".venv"
    venv_python = venv_dir / "Scripts" / "python.exe"
    uv_exe = root / "bin" / "uv.exe"
    if _venv_minor(venv_python) == PYTHON_MINOR:
        log(f"python venv: {venv_dir} ({PYTHON_MINOR}, up to date)", dim=True)
        ensure_python_shim(root, dry_run=dry_run)
        return
    info(f"\n=== Syncing python venv ({PYTHON_MINOR}) ===")
    if not uv_exe.is_file():
        raise SystemExit(f"uv not found at {uv_exe} (run ./sync-deps.ps1)")
    if dry_run:
        warn(f"Would run: uv venv {venv_dir} --python {PYTHON_MINOR}")
        ensure_python_shim(root, dry_run=dry_run)
        return
    if venv_dir.exists():
        shutil.rmtree(venv_dir)
    subprocess.run([str(uv_exe), "venv", str(venv_dir), "--python", PYTHON_MINOR], check=True)
    if _venv_minor(venv_python) != PYTHON_MINOR:
        raise SystemExit(f"venv python is not {PYTHON_MINOR} (got {_venv_minor(venv_python)})")
    ensure_python_shim(root, dry_run=False)
    ok(f"Synced python venv -> {venv_python}")


def run_ps_sync_deps(root: Path, names: list[str], *, dry_run: bool) -> None:
    # DEPS.json 含 sync_dir/bin_dir/download_dir 时只传 DepsFile；按名过滤必须用 -Name（见 ps-sync-deps README）。
    tool = root / "scripts" / "ps-sync-deps" / "Sync-Deps.ps1"
    if not tool.is_file():
        err = (
            "ps-sync-deps submodule missing. Run:\n"
            "  git submodule update --init scripts/ps-sync-deps"
        )
        raise SystemExit(err)
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        raise SystemExit("pwsh/powershell not found (required for ps-sync-deps)")
    deps = str(root / "DEPS.json").replace("'", "''")
    tool_ps = str(tool).replace("'", "''")
    lines = [
        "$ErrorActionPreference = 'Stop'",
        f"$params = @{{DepsFile = '{deps}'}}",
    ]
    if names:
        quoted = ",".join(f"'{n}'" for n in names)
        lines.append(f"$params['Name'] = @({quoted})")
    if dry_run:
        lines.append("$params['DryRun'] = $true")
    lines.append(f"& '{tool_ps}' @params")
    script = "; ".join(lines)
    subprocess.run([pwsh, "-NoProfile", "-Command", script], cwd=root, check=True)


def _normalize_argv(argv: list[str]) -> list[str]:
    out: list[str] = []
    for a in argv:
        if a in ("-DryRun", "-n"):
            out.append("--dry-run")
        else:
            out.append(a)
    return out


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Sync third_party / bin from DEPS.json")
    p.add_argument("-n", "--dry-run", action="store_true")
    p.add_argument("names", nargs="*", help="dependencies / binaries; uv 另建 .venv")
    return p.parse_args(_normalize_argv(argv if argv is not None else sys.argv[1:]))


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = project_root()
    dry_run = args.dry_run
    names = list(args.names)

    sync_all = len(names) == 0
    want_python = sync_all or "uv" in names
    run_ps = sync_all or len(names) > 0
    touches_llvm = sync_all or "llvm" in names
    llvm_before = _llvm_head(root / "third_party" / "llvm") if touches_llvm and not dry_run else None

    if run_ps:
        run_ps_sync_deps(root, names, dry_run=dry_run)
    if want_python:
        ensure_python_venv(root, dry_run=dry_run)
    write_launchers(root, dry_run=dry_run)

    if touches_llvm and not dry_run:
        llvm_after = _llvm_head(root / "third_party" / "llvm")
        if llvm_after:
            _write_llvm_stamp(root, llvm_after)
        if llvm_after and llvm_after != llvm_before:
            before = (llvm_before or "none")[:8]
            after = llvm_after[:8]
            warn("")
            warn(f"llvm source updated ({before} → {after})")
            warn("  next: ./build llvm   (or ./build riu)")
            log("  GN will reconfigure + rebuild LLVM when the source stamp differs.", dim=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

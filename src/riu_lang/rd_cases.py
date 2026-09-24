"""rd 金样回归（原 tests/rd-cases/run.ps1）。"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from riu_lang._console import err, info, ok
from riu_lang._paths import project_root


def _norm(text: str) -> str:
    return text.replace("\r\n", "\n").rstrip() + "\n"


def _read_utf8(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8")


def find_riu_ast(root: Path, explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise SystemExit(f"riu-ast not found: {explicit}")
    cand = root / "build" / "windows" / "x64" / "debug" / "bin" / "riu-ast.exe"
    if cand.is_file():
        return cand
    found = shutil.which("riu-ast")
    if found:
        return Path(found)
    raise SystemExit("riu-ast not found (build with ./build riu-ast or pass --riu-ast)")


def _run_riu_ast(riu_ast: Path, args: list[str], err_path: Path) -> int:
    with err_path.open("wb") as err_f:
        proc = subprocess.run(
            [str(riu_ast), *args],
            stdout=subprocess.DEVNULL,
            stderr=err_f,
        )
    return proc.returncode


def run_cases(cases_dir: Path, riu_ast: Path) -> int:
    fails = 0
    total = 0
    with tempfile.TemporaryDirectory(prefix="rd-cases-") as tmp:
        tmp_dir = Path(tmp)
        for ut in sorted(cases_dir.glob("*.ut")):
            total += 1
            base = ut.stem
            gold_tokens = cases_dir / f"{base}.tokens.txt"
            gold_rd = cases_dir / f"{base}.rd.txt"
            gold_diag = cases_dir / f"{base}.diag.txt"

            if gold_tokens.is_file():
                out = tmp_dir / f"{base}.rd"
                err_path = tmp_dir / f"{base}.tok.err"
                code = _run_riu_ast(riu_ast, [str(ut), "--rd-tokens", "-o", str(out)], err_path)
                if code != 0:
                    err(f"FAIL {ut.name} riu-ast --rd-tokens")
                    fails += 1
                    continue
                if _norm(_read_utf8(out)) != _norm(_read_utf8(gold_tokens)):
                    err(f"FAIL {ut.name} --rd-tokens != {base}.tokens.txt")
                    fails += 1
                    continue

            if gold_rd.is_file() or gold_diag.is_file():
                out = tmp_dir / f"{base}.flat"
                err_path = tmp_dir / f"{base}.err"
                code = _run_riu_ast(riu_ast, [str(ut), "--rd", "-o", str(out)], err_path)
                if code != 0:
                    err(f"FAIL {ut.name} riu-ast --rd")
                    fails += 1
                    continue
                if gold_rd.is_file():
                    if _norm(_read_utf8(out)) != _norm(_read_utf8(gold_rd)):
                        err(f"FAIL {ut.name} --rd != {base}.rd.txt")
                        fails += 1
                        continue
                err_text = _read_utf8(err_path)
                if gold_diag.is_file():
                    if _norm(err_text) != _norm(_read_utf8(gold_diag)):
                        err(f"FAIL {ut.name} --rd stderr != {base}.diag.txt")
                        fails += 1
                        continue
                elif err_text.strip():
                    err(f"FAIL {ut.name} unexpected --rd stderr")
                    fails += 1
                    continue

            info(f"OK  {ut.name}")

    if fails:
        err(f"rd-cases: {fails} / {total} failed")
        return 1
    ok(f"rd-cases: {total} ok")
    return 0


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run tests/rd-cases golden files")
    p.add_argument("--riu-ast", dest="riu_ast", help="path to riu-ast.exe")
    p.add_argument(
        "--dir",
        dest="cases_dir",
        type=Path,
        help="cases directory (default: tests/rd-cases)",
    )
    return p.parse_args(argv if argv is not None else sys.argv[1:])


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = project_root()
    cases_dir = args.cases_dir or (root / "tests" / "rd-cases")
    riu_ast = find_riu_ast(root, args.riu_ast)
    return run_cases(cases_dir, riu_ast)


if __name__ == "__main__":
    raise SystemExit(main())

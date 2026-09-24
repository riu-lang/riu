"""项目编译+运行 / 格式化回归 / 预期编译失败（原 tests/run.ps1）。"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

from riu_lang._console import err, info, log
from riu_lang._paths import project_root, venv_python


@dataclass(frozen=True)
class Case:
    name: str
    dir: Path
    kind: str  # project | format | fail


def _strip_cr(text: str) -> str:
    return text.replace("\r", "") if text else ""


def _capture(
    cmd: list[str],
    cwd: Path,
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )


def _rmtree(path: Path) -> None:
    if not path.exists():
        return

    def _onerror(func, p, _exc_info):
        os.chmod(p, stat.S_IWUSR)
        func(p)

    shutil.rmtree(path, onerror=_onerror)


def _read_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return [
        line.rstrip("\r\n")
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]


def find_riu_exe(root: Path, explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise SystemExit(f"riu.exe not found: {explicit}")
    cand = root / "build" / "windows" / "x64" / "debug" / "bin" / "riu.exe"
    if cand.is_file():
        return cand
    found = shutil.which("riu")
    if found:
        return Path(found)
    raise SystemExit("riu.exe not found. Build with ./build riu first, or pass --riu.")


def get_cases(projects_dir: Path) -> list[Case]:
    cases: list[Case] = []
    if not projects_dir.is_dir():
        return cases
    for d in sorted(projects_dir.iterdir()):
        if not d.is_dir() or not (d / "riu.toml").is_file():
            continue
        kind = None
        if (d / "expected.txt").is_file():
            kind = "project"
        elif (d / "expected_format").is_file():
            kind = "format"
        elif (d / "expected_fail.txt").is_file():
            kind = "fail"
        if kind:
            cases.append(Case(name=d.name, dir=d, kind=kind))
    return cases


def invoke_prebuild(case_dir: Path, riu_exe: Path, root: Path) -> str:
    pre = case_dir / "prebuild.py"
    if not pre.is_file():
        return ""
    env = os.environ.copy()
    env["RiuExe"] = str(riu_exe)
    env["PYTHONPATH"] = str(root / "src")
    py = venv_python(root)
    r = _capture([str(py), str(pre)], case_dir, env=env)
    if r.returncode != 0:
        return f"prebuild failed\n{r.stderr}{r.stdout}"
    return ""


def _restore_toml_in(case_dir: Path) -> None:
    toml_in = case_dir / "riu.toml.in"
    if toml_in.is_file():
        shutil.copy2(toml_in, case_dir / "riu.toml")


def invoke_one_case(case: Case, riu_exe: Path, root: Path) -> tuple[bool, str]:
    ok = False
    err_msg = ""
    try:
        if case.kind == "project":
            build_dir = case.dir / "build"
            _rmtree(build_dir)
            err_msg = invoke_prebuild(case.dir, riu_exe, root)
            if err_msg:
                return False, err_msg
            run_exes = case.dir / "run_exes.txt"
            expect_files = case.dir / "expect_files.txt"
            build_all = run_exes.is_file() or expect_files.is_file()
            build_args = ["build"] if build_all else ["build", case.name]
            r = _capture([str(riu_exe), *build_args], case.dir)
            exe_stems = _read_lines(run_exes) if run_exes.is_file() else [case.name]
            missing: list[str] = []
            for stem in exe_stems:
                if not (build_dir / f"{stem}.exe").is_file():
                    missing.append(f"{stem}.exe")
            for rel in _read_lines(expect_files):
                if not (build_dir / rel).exists():
                    missing.append(rel)
            if r.returncode != 0 or missing:
                miss = f"missing: {', '.join(missing)}\n" if missing else ""
                return False, f"compile failed\n{miss}{r.stderr}{r.stdout}"
            run_args = _read_lines(case.dir / "run_args.txt")
            stdout = ""
            for stem in exe_stems:
                exe = build_dir / f"{stem}.exe"
                run = _capture([str(exe), *run_args], case.dir)
                if run.returncode != 0:
                    return False, f"run failed {stem}.exe exit {run.returncode}\n{run.stderr}{run.stdout}"
                stdout += run.stdout
            expected = (case.dir / "expected.txt").read_text(encoding="utf-8")
            if stdout != expected:
                return False, f"output mismatch\n--- expected ---\n{expected}\n--- actual ---\n{stdout}"
            ok = True
            _rmtree(build_dir)
        elif case.kind == "fail":
            build_dir = case.dir / "build"
            _rmtree(build_dir)
            err_msg = invoke_prebuild(case.dir, riu_exe, root)
            if err_msg:
                return False, err_msg
            r = _capture([str(riu_exe), "build", case.name], case.dir)
            combined = _strip_cr(f"{r.stderr}{r.stdout}")
            if r.returncode == 0:
                return False, f"expected compile failure, got exit 0\n{combined}"
            needles = _read_lines(case.dir / "expected_fail.txt")
            missing = [n for n in needles if n not in combined]
            if missing:
                return False, f"stderr missing:\n" + "\n".join(missing) + f"\n--- actual ---\n{combined}"
            ok = True
            _rmtree(build_dir)
        else:
            toml = (case.dir / "riu.toml").read_text(encoding="utf-8")
            m = re.search(r'entry\s*=\s*"([^"]+)"', toml)
            if not m:
                return False, "riu.toml missing entry"
            src_file = case.dir / "src" / m.group(1)
            if not src_file.is_file():
                return False, f"source not found: {src_file}"
            r = _capture([str(riu_exe), "format", str(src_file)], case.dir)
            if r.returncode != 0:
                return False, f"format failed\n{r.stderr}{r.stdout}"
            expected = (case.dir / "expected_format").read_text(encoding="utf-8")
            if _strip_cr(r.stdout) != _strip_cr(expected):
                return False, f"format mismatch\n--- expected ---\n{expected}\n--- actual ---\n{r.stdout}"
            ok = True
    except Exception as exc:
        return False, str(exc)
    finally:
        _restore_toml_in(case.dir)
    return ok, err_msg


def _normalize_name(name: str) -> str:
    for prefix in ("project_", "riu_tests/"):
        if name.startswith(prefix):
            name = name[len(prefix) :]
    if name.startswith("project_"):
        name = name[8:]
    return name


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run tests/projects regression")
    p.add_argument("--riu", dest="riu_exe", help="path to riu.exe")
    p.add_argument("-g", "--group", choices=("project", "format", "riu/project", "riu/format"))
    p.add_argument("-j", "--jobs", type=int, default=0)
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("names", nargs="*", help="case directory names")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    root = project_root()
    riu_exe = find_riu_exe(root, args.riu_exe)
    projects_dir = root / "tests" / "projects"
    cases = get_cases(projects_dir)

    if args.group:
        g = args.group.lower().replace("riu/", "")
        cases = [c for c in cases if c.kind == g]

    if args.names:
        want = {_normalize_name(n) for n in args.names}
        cases = [c for c in cases if c.name in want]
        if not cases:
            raise SystemExit(f"no matching tests: {', '.join(args.names)}")

    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 1)
    jobs = max(1, min(jobs, len(cases) or 1))

    log(f"riu: {riu_exe}", dim=True)
    info(f"cases: {len(cases)}  jobs: {jobs}\n")

    passed = 0
    failed: list[str] = []

    def report(case: Case, ok: bool, err_text: str) -> None:
        nonlocal passed
        if ok:
            passed += 1
            info(f"[pass] {case.kind}/{case.name}")
        else:
            failed.append(f"{case.kind}/{case.name}")
            err(f"[fail] {case.kind}/{case.name}")
            if err_text:
                print(err_text, flush=True)

    def run_one(case: Case) -> tuple[Case, bool, str]:
        ok, err_text = invoke_one_case(case, riu_exe, root)
        return case, ok, err_text

    if jobs <= 1 or len(cases) <= 1:
        for c in cases:
            case, ok, err_text = run_one(c)
            report(case, ok, err_text)
    else:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = [pool.submit(run_one, c) for c in cases]
            for fut in as_completed(futs):
                case, ok, err_text = fut.result()
                report(case, ok, err_text)

    fail_count = len(failed)
    color = info if fail_count == 0 else err
    color(f"\n{passed} passed, {fail_count} failed, {len(cases)} total")
    if failed:
        err("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

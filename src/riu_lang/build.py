"""GN + Ninja 构建入口。"""

from __future__ import annotations

import subprocess
import sys
from datetime import date
from pathlib import Path

from riu_lang._console import info, log, ok
from riu_lang._paths import find_tool, gn_path, gn_script_executable, project_root
from riu_lang.pack import pack


def _riu_version(root: Path) -> str:
    gni = root / "build" / "version.gni"
    for line in gni.read_text(encoding="utf-8").splitlines():
        if 'riu_version = "' in line:
            start = line.index('"') + 1
            end = line.index('"', start)
            return line[start:end]
    raise SystemExit("riu_version not found in build/version.gni")


def _ensure_sdk_link(out_dir: Path, root: Path) -> None:
    link = out_dir / "sdk"
    src = (root / "sdk").resolve()
    ok_link = False
    if link.exists():
        try:
            ok_link = link.resolve().samefile(src)
        except OSError:
            ok_link = False
        if not ok_link:
            log(f"sdk junction retarget: {link} → {src}", dim=True)
            subprocess.run(["cmd", "/c", "rmdir", str(link)], check=False)
            if link.exists():
                raise SystemExit(f"failed to remove stale sdk path at {link}")
    if ok_link:
        return
    log(f"sdk junction: {link} → sdk/", dim=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(src)], check=True)
    if not link.exists():
        raise SystemExit(f"failed to create sdk junction at {link}")


def _write_args_gn(out_dir: Path, llvm_dir: Path, version_str: str, is_debug: bool) -> bool:
    args_file = out_dir / "args.gn"
    content = (
        f"is_debug = {'true' if is_debug else 'false'}\n"
        f'riu_version_str = "{version_str}"\n'
        f'llvm_build_dir = "{gn_path(llvm_dir)}"\n'
    )
    old = args_file.read_text(encoding="utf-8") if args_file.is_file() else ""
    norm_new = content.replace("\r\n", "\n").strip() + "\n"
    norm_old = old.replace("\r\n", "\n").strip() + "\n" if old else ""
    if norm_old != norm_new:
        args_file.write_text(norm_new, encoding="utf-8", newline="\n")
        return True
    return False


def _script_exe_stale(out_dir: Path, script_exe: str) -> bool:
    stamp = out_dir / ".gn_script_executable"
    if not stamp.is_file():
        return True
    return stamp.read_text(encoding="utf-8").strip() != script_exe


def _write_script_exe_stamp(out_dir: Path, script_exe: str) -> None:
    (out_dir / ".gn_script_executable").write_text(script_exe + "\n", encoding="utf-8")


def _build_sdk_packages(root: Path, riu_exe: Path) -> None:
    if not riu_exe.is_file():
        return
    for pkg in ("core", "stdlib"):
        pkg_dir = root / "sdk" / pkg
        if not (pkg_dir / "riu.toml").is_file():
            continue
        info(f"\n=== riu build sdk/{pkg} ===")
        subprocess.run([str(riu_exe), "build"], cwd=pkg_dir, check=True)


def _run_tests(root: Path, out_dir: Path, ninja: Path, forward: list[str], targets: list[str]) -> int:
    if not targets:
        info("\n=== ninja riu ===")
        subprocess.run([str(ninja), "-C", str(out_dir), "riu"], check=True)
    riu_exe = out_dir / "bin" / "riu.exe"
    _build_sdk_packages(root, riu_exe)
    from riu_lang.test_projects import main as run_test_projects

    test_argv = ["--riu", str(riu_exe), *forward]
    return run_test_projects(test_argv)


def show_help() -> None:
    print(
        """用法:
  ./build [options] [ninja-targets...]
  ./build test [test-args...]
  ./build test -Jobs 8
  ./build pack

选项:
  -Release / --release   release 配置（默认 debug）
  -GenOnly / --gen-only  只 gn gen，不 ninja
  -h / --help            帮助

test 参数（转发到 test-projects）:
  -Jobs / --jobs / -j N  并行用例数（默认 CPU 核数；1 = 串行）
  -Group project|format  只跑一类
  <name>                 只跑指定用例目录

常用目标: riu  riu-lsp  riu-ast  riu-check  riu-test-runner  riurt  llvm
无目标时构建 default（全部 exe）。"""
    )


def main(argv: list[str] | None = None) -> int:
    args_list = list(argv or sys.argv[1:])
    if any(a in ("-h", "--help", "-Help", "/?") for a in args_list):
        show_help()
        return 0

    root = project_root()
    mode = "debug"
    gen_only = False
    do_test = False
    do_pack = False
    targets: list[str] = []
    forward: list[str] = []

    i = 0
    while i < len(args_list):
        a = args_list[i]
        if a in ("-Release", "--release"):
            mode = "release"
        elif a in ("-GenOnly", "--gen-only"):
            gen_only = True
        elif a in ("test", "-Test", "--test"):
            do_test = True
        elif a in ("pack", "-Pack", "--pack"):
            do_pack = True
        elif do_test:
            forward.append(a)
        else:
            targets.append(a)
        i += 1

    out_dir = root / "build" / "windows" / "x64" / mode
    out_dir.mkdir(parents=True, exist_ok=True)
    llvm_dir = out_dir / "llvm"
    version = _riu_version(root)
    version_str = f"v{version}-{date.today().isoformat()}"
    script_exe = gn_path(gn_script_executable(root))
    args_changed = _write_args_gn(out_dir, llvm_dir, version_str, mode == "debug")

    gn = find_tool(root, "gn")
    ninja = find_tool(root, "ninja")
    build_ninja = out_dir / "build.ninja"
    need_gen = args_changed or not build_ninja.is_file() or _script_exe_stale(out_dir, script_exe)

    if need_gen:
        info(f"\n=== gn gen {out_dir} ===")
        subprocess.run(
            [str(gn), "gen", str(out_dir), "--export-compile-commands", f"--script-executable={script_exe}"],
            cwd=root,
            check=True,
        )
        _write_script_exe_stamp(out_dir, script_exe)
    else:
        log(f"gn: {out_dir} (up to date)", dim=True)

    _ensure_sdk_link(out_dir, root)

    if gen_only:
        ok("gen-only; skip ninja.")
        return 0

    if do_pack:
        pack(out_dir, version)
        return 0

    if do_test:
        return _run_tests(root, out_dir, ninja, forward, targets)

    ninja_args = ["-C", str(out_dir), *targets]
    info(f"\n=== ninja {' '.join(ninja_args)} ===")
    code = subprocess.run([str(ninja), *ninja_args]).returncode
    if code != 0:
        return code
    riu_exe = out_dir / "bin" / "riu.exe"
    _build_sdk_packages(root, riu_exe)
    ok(f"\nbuild ok: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

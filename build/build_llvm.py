# Copyright (c) 2026. Yin-Jinlong@github
# MPL-2.0
"""Configure + build LLVM/LLD via LLVM's GN (llvm/utils/gn). Stamp = source git HEAD."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys


def run(cmd: list[str], cwd: str | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=cwd, env=env)


def git_head(repo: str) -> str:
    out = subprocess.check_output(["git", "-C", repo, "rev-parse", "HEAD"], text=True)
    return out.strip()


def read_stamp(path: str) -> str | None:
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip() or None
    except FileNotFoundError:
        return None


def write_stamp(path: str, commit: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(commit + "\n")


_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def which_or_die(name: str) -> str:
    local = os.path.join(_REPO_ROOT, "bin", f"{name}.exe")
    if os.path.isfile(local):
        return os.path.normpath(local)
    p = shutil.which(name)
    if not p:
        sys.stderr.write(f"{name} not found (bin/{name}.exe or PATH)\n")
        sys.exit(1)
    return os.path.normpath(p)


def to_gn_path(path: str) -> str:
    return os.path.normpath(path).replace("\\", "/")


def without_spaces(path: str) -> str:
    """LLVM's GN toolchain does not quote compiler/linker paths.

    Paths under e.g. ``C:\\Program Files\\...`` break ninja argv splitting.
    Prefer the Windows 8.3 short name when available.
    """
    if os.name != "nt" or " " not in path:
        return path
    import ctypes

    buf = ctypes.create_unicode_buffer(512)
    n = ctypes.windll.kernel32.GetShortPathNameW(path, buf, len(buf))
    if n and n < len(buf) and buf.value and " " not in buf.value:
        return buf.value
    sys.stderr.write(
        f"path has spaces and no 8.3 short name: {path}\n"
        "Install LLVM to a path without spaces, or enable 8.3 names on this volume.\n"
    )
    sys.exit(1)


def clang_prefix_from_clang_cl(clang_cl: str) -> str:
    # D:/LLVM/latest/bin/clang-cl.exe → D:/LLVM/latest
    bindir = os.path.dirname(clang_cl)
    if os.path.basename(bindir).lower() != "bin":
        sys.stderr.write(f"clang-cl is not in a bin/ directory: {clang_cl}\n")
        sys.exit(1)
    return without_spaces(os.path.dirname(bindir))


def write_args_gn(
    path: str,
    *,
    is_debug: bool,
    clang_base: str,
    python: str,
    wrapper: str,
    crt: str,
) -> bool:
    content = (
        f"is_debug = {'true' if is_debug else 'false'}\n"
        f'llvm_targets_to_build = ["X86"]\n'
        f'clang_base_path = "{to_gn_path(clang_base)}"\n'
        f'compiler_wrapper = "\\"{to_gn_path(python)}\\" \\"{to_gn_path(wrapper)}\\" --crt {crt}"\n'
    )
    old = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    if old.replace("\r\n", "\n") == content:
        return False
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return True


def collect_libs(lib_dir: str) -> list[str]:
    if not os.path.isdir(lib_dir):
        return []
    names: list[str] = []
    for fn in os.listdir(lib_dir):
        if not fn.lower().endswith(".lib"):
            continue
        stem = fn[:-4]
        if stem.startswith("LLVMTesting") or stem.startswith("LLVMFuzz"):
            continue
        if stem.startswith("LLVM") or stem.startswith("lld"):
            names.append(fn)
    names.sort(key=str.lower)
    return names


def write_link_rsp(path: str, lib_names: list[str]) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    body = "\n".join(lib_names) + ("\n" if lib_names else "")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)


def wipe_cmake_tree(build: str) -> None:
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return
    print("[llvm] removing leftover CMake tree (switching to GN)", flush=True)
    shutil.rmtree(build)


def wipe_llvm_build_tree(build: str) -> None:
    if not os.path.isdir(build):
        return
    print("[llvm] removing stale GN build tree (source commit changed)", flush=True)
    shutil.rmtree(build)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="third_party/llvm (monorepo root)")
    ap.add_argument("--build", required=True, help="LLVM GN out dir")
    ap.add_argument("--stamp", required=True, help="GN stamp output (commit id)")
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--wrapper", required=True, help="llvm_cl_wrapper.py")
    ap.add_argument("--rsp", required=True, help="link rsp written after ninja")
    args = ap.parse_args()

    src = os.path.abspath(os.path.normpath(args.src))
    build = os.path.abspath(os.path.normpath(args.build))
    stamp = os.path.abspath(os.path.normpath(args.stamp))
    wrapper = os.path.abspath(os.path.normpath(args.wrapper))
    rsp = os.path.abspath(os.path.normpath(args.rsp))
    gn_src = os.path.join(src, "llvm", "utils", "gn")
    dotfile = os.path.join(gn_src, ".gn")
    lib_dir = os.path.join(build, "lib")
    src_stamp = os.path.join(build, "riu-llvm-src.stamp")
    ninja_file = os.path.join(build, "build.ninja")

    if not os.path.isfile(dotfile):
        sys.stderr.write(f"LLVM GN not found: {dotfile}\n")
        sys.stderr.write("Run ./sync-deps.ps1 first.\n")
        return 1

    gn = which_or_die("gn")
    ninja = which_or_die("ninja")
    clang_cl = which_or_die("clang-cl")
    clang_base = clang_prefix_from_clang_cl(clang_cl)
    python = os.path.normpath(sys.executable)
    crt = "/MDd" if args.debug else "/MD"

    src_commit = git_head(src)
    stamped = read_stamp(stamp) or read_stamp(src_stamp)
    src_dirty = stamped != src_commit

    if src_dirty:
        if stamped:
            print(
                f"[llvm] source commit changed: {stamped[:8]} → {src_commit[:8]}",
                flush=True,
            )
        else:
            print("[llvm] no source stamp; will gn gen / ninja", flush=True)
        wipe_cmake_tree(build)
        wipe_llvm_build_tree(build)
    else:
        wipe_cmake_tree(build)

    args_gn = os.path.join(build, "args.gn")
    args_changed = write_args_gn(
        args_gn,
        is_debug=args.debug,
        clang_base=clang_base,
        python=python,
        wrapper=wrapper,
        crt=crt,
    )

    os.makedirs(build, exist_ok=True)
    need_gen = args_changed or not os.path.isfile(ninja_file) or src_dirty
    if need_gen:
        run(
            [
                gn,
                f"--root={src}",
                f"--dotfile={dotfile}",
                f"--script-executable={python}",
                "gen",
                build,
            ]
        )

    ncpu = os.cpu_count() or 4
    jobs = max(1, ncpu - 2)
    print("[llvm] ninja lld/COFF (X86 + LLD COFF libs)", flush=True)
    run([ninja, "-C", build, "-j", str(jobs), "lld/COFF"])
    libs = collect_libs(lib_dir)
    if "lldCOFF.lib" not in libs:
        sys.stderr.write(f"[llvm] missing lldCOFF.lib under {lib_dir}\n")
        return 1
    write_stamp(src_stamp, src_commit)
    print(f"[llvm] stamped {src_commit[:8]} ({len(libs)} libs)", flush=True)

    write_link_rsp(rsp, libs)
    write_stamp(stamp, src_commit)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

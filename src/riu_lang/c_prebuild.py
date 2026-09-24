"""clang-cl / lld-link 编 C 静态库（FFI prebuild 共用）。"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


def find_llvm_bin(name: str, project_dir: Path, *, riu_exe: str | None = None) -> Path:
    found = shutil.which(name)
    if found:
        return Path(found)
    if riu_exe:
        bin_dir = Path(riu_exe).resolve().parent
        cand = bin_dir.parent / "llvm" / "bin" / f"{name}.exe"
        if cand.is_file():
            return cand
    here = project_dir.resolve()
    for _ in range(6):
        cand = here / "build" / "windows" / "x64" / "debug" / "llvm" / "bin" / f"{name}.exe"
        if cand.is_file():
            return cand
        if here.parent == here:
            break
        here = here.parent
    raise SystemExit(f"{name} not found (need clang-cl / lld-link on PATH or next to riu.exe)")


def build_c_static_lib(
    project_dir: Path,
    *,
    c_file: str,
    lib_name: str,
    extra_includes: tuple[Path, ...] = (),
) -> Path:
    project_dir = project_dir.resolve()
    c_dir = project_dir / "c"
    lib_dir = project_dir / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    riu_exe = os.environ.get("RiuExe")
    clang = find_llvm_bin("clang-cl", project_dir, riu_exe=riu_exe)
    lld = find_llvm_bin("lld-link", project_dir, riu_exe=riu_exe)
    src = project_dir / c_file
    if not src.is_file():
        raise SystemExit(f"source not found: {src}")
    obj = lib_dir / f"{lib_name}.obj"
    lib = lib_dir / f"{lib_name}.lib"
    inc_args = [f"/I{inc}" for inc in (c_dir, *extra_includes)]
    subprocess.run(
        [str(clang), "/c", "/GS-", *inc_args, f"/Fo{obj}", str(src)],
        check=True,
    )
    subprocess.run([str(lld), "/lib", f"/out:{lib}", str(obj)], check=True)
    print(f"wrote {lib}")
    return lib

#!/usr/bin/env python3
"""clang-cl 把 c/ffi_demo.c 编成 lib/ffi_demo.lib（无 CRT）。"""

from pathlib import Path

from riu_lang._paths import project_root
from riu_lang.c_prebuild import build_c_static_lib

root = Path(__file__).resolve().parent
build_c_static_lib(
    root,
    c_file="c/ffi_demo.c",
    lib_name="ffi_demo",
    extra_includes=(project_root() / "riu" / "rt",),
)

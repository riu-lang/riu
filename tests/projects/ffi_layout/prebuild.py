#!/usr/bin/env python3
"""clang-cl 把 c/layout_demo.c 编成 lib/layout_demo.lib（无 CRT）。"""

from pathlib import Path

from riu_lang.c_prebuild import build_c_static_lib

build_c_static_lib(
    Path(__file__).resolve().parent,
    c_file="c/layout_demo.c",
    lib_name="layout_demo",
)

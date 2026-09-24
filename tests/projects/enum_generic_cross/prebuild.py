#!/usr/bin/env python3
"""先编一次写出 box.ud，再删主模块 obj / exe，让正式 build 从 .ud 加载泛型 enum。"""

import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
riu = os.environ.get("RiuExe")
if not riu:
    raise SystemExit("RiuExe not set")
subprocess.run([riu, "build", "enum_generic_cross"], check=True, cwd=root)
main_obj = root / "build" / "src" / "main.obj"
exe = root / "build" / "enum_generic_cross.exe"
if main_obj.is_file():
    main_obj.unlink()
if exe.is_file():
    exe.unlink()

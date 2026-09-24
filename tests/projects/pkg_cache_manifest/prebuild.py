#!/usr/bin/env python3

import os
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
pkg_path = root / "src" / "pkg"
build_dir = root / "build"
final_pkg = "helper as public_helper\n"
riu = os.environ.get("RiuExe")
if not riu:
    raise SystemExit("RiuExe not set")

try:
    pkg_path.write_text("helper\n", encoding="utf-8", newline="\n")
    subprocess.run([riu, "build", "pkg_cache_manifest"], cwd=root, check=True, capture_output=True, text=True)
    pkg_path.write_text(final_pkg, encoding="utf-8", newline="\n")
    second = subprocess.run(
        [riu, "build", "pkg_cache_manifest"],
        cwd=root,
        capture_output=True,
        text=True,
    )
    if second.returncode != 0:
        raise SystemExit(f"incremental build failed\n{second.stderr}{second.stdout}")
    combined = f"{second.stderr}{second.stdout}"
    if "Compile IR" not in combined:
        raise SystemExit(f"changing src/pkg did not invalidate package objects\n{combined}")
finally:
    pkg_path.write_text(final_pkg, encoding="utf-8", newline="\n")
    if build_dir.exists():
        shutil.rmtree(build_dir)

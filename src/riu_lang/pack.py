"""打包 riu zip。"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

from riu_lang._console import ok
from riu_lang._paths import project_root


def pack(out_dir: Path, version: str) -> None:
    root = project_root()
    stage = out_dir / "pack-stage" / "riu"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    bin_dst = stage / "bin"
    bin_dst.mkdir()
    is_debug = "debug" in out_dir.as_posix().replace("\\", "/")
    for exe in (
        "riu.exe",
        "riu-lsp.exe",
        "riu-ast.exe",
        "riu-check.exe",
        "riu-test-runner.exe",
    ):
        src = out_dir / "bin" / exe
        if not src.is_file():
            raise SystemExit(f"missing {src} — build all targets first")
        shutil.copy2(src, bin_dst / exe)
        pdb = src.with_suffix(".pdb")
        if pdb.is_file():
            shutil.copy2(pdb, bin_dst / pdb.name)
        elif is_debug:
            raise SystemExit(f"missing {pdb} — debug pack requires PDB next to each exe")

    lib_src = out_dir / "lib" / "riurt.lib"
    if not lib_src.is_file():
        raise SystemExit(f"missing {lib_src}")
    (stage / "lib").mkdir()
    shutil.copy2(lib_src, stage / "lib" / "riurt.lib")

    sdk_dst = stage / "sdk"
    shutil.copytree(root / "sdk", sdk_dst)
    for name in ("build", ".ut"):
        for d in list(sdk_dst.rglob(name)):
            if d.is_dir() and d.name == name:
                shutil.rmtree(d, ignore_errors=True)

    shutil.copytree(root / "examples", stage / "examples")
    shutil.copytree(root / "docs", stage / "docs")
    shutil.copy2(root / "README.md", stage / "README.md")
    shutil.copy2(root / "LICENSE.txt", stage / "LICENSE.txt")

    licenses = (
        ("cli11", "cli11/LICENSE"),
        ("llvm", "llvm/llvm/LICENSE.TXT"),
        ("utfcpp", "utfcpp/LICENSE"),
        ("zlib", "zlib/LICENSE"),
        ("toml11", "toml11/LICENSE"),
        ("nlohmann_json", "nlohmann_json/LICENSE.MIT"),
    )
    for lib, rel in licenses:
        src = root / "third_party" / rel
        if src.is_file():
            dst_dir = stage / "shared" / "licenses" / lib
            dst_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst_dir / Path(rel).name)

    zip_name = f"riu-{version}.zip"
    zip_path = out_dir / zip_name
    if zip_path.is_file():
        zip_path.unlink()
    stage_parent = stage.parent
    subprocess.run(
        ["tar", "-a", "-c", "-f", str(zip_path), stage.name],
        cwd=stage_parent,
        check=True,
    )
    ok(f"packed {zip_path}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Pack riu zip")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--version", required=True)
    args = p.parse_args(argv)
    pack(Path(args.out_dir), args.version)
    return 0

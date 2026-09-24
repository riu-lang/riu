#!/usr/bin/env python3

import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
work = root / "build" / "_git_work"
origin = root / "build" / "origin.git"
if work.exists():
    shutil.rmtree(work)
(work / "src").mkdir(parents=True)

(work / "riu.toml").write_text(
    'name="utils"\nversion="1.0.0"\n\n[library]\nlib_mod="utils"\n',
    encoding="utf-8",
    newline="\n",
)
(work / "src" / "utils.ut").write_text("fn git_hi() {}\n", encoding="utf-8", newline="\n")

git = ["git", "-c", "user.name=riu-test", "-c", "user.email=riu-test@example.invalid", "-c", "core.autocrlf=false"]
subprocess.run([*git, "init", "-q"], cwd=work, check=True)
subprocess.run([*git, "add", "-A"], cwd=work, check=True)
subprocess.run([*git, "commit", "-q", "-m", "init"], cwd=work, check=True)
rev1 = subprocess.check_output([*git, "rev-parse", "HEAD"], cwd=work, text=True).strip()
with (work / "src" / "utils.ut").open("a", encoding="utf-8", newline="\n") as f:
    f.write("; v2\n")
subprocess.run([*git, "add", "-A"], cwd=work, check=True)
subprocess.run([*git, "commit", "-q", "-m", "v2"], cwd=work, check=True)
rev2 = subprocess.check_output([*git, "rev-parse", "HEAD"], cwd=work, text=True).strip()
if len(rev1) != 40 or len(rev2) != 40 or rev1 == rev2:
    raise SystemExit(f"need two commits: {rev1} / {rev2}")

subprocess.run(["git", "-c", "core.autocrlf=false", "clone", "-q", "--bare", str(work), str(origin)], check=True)


def write_side(name: str, rev: str) -> None:
    d = root / "build" / name
    (d / "src").mkdir(parents=True, exist_ok=True)
    (d / "riu.toml").write_text(
        f'''name="{name}"
version="1.0.0"

[dependencies]
utils = {{ git = "build/origin.git", rev = "{rev}" }}

[library]
lib_mod="{name}"
''',
        encoding="utf-8",
        newline="\n",
    )
    (d / "src" / f"{name}.ut").write_text(f"fn {name}_hi() {{}}\n", encoding="utf-8", newline="\n")


write_side("a", rev1)
write_side("b", rev2)

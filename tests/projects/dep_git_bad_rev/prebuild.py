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
subprocess.run(["git", "-c", "core.autocrlf=false", "clone", "-q", "--bare", str(work), str(origin)], check=True)

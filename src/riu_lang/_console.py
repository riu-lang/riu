"""简单终端输出。"""

from __future__ import annotations


def log(message: str, *, dim: bool = False) -> None:
    if dim:
        print(f"\033[90m{message}\033[0m")
    else:
        print(message)


def info(message: str) -> None:
    print(f"\033[36m{message}\033[0m")


def ok(message: str) -> None:
    print(f"\033[32m{message}\033[0m")


def warn(message: str) -> None:
    print(f"\033[33m{message}\033[0m")


def err(message: str) -> None:
    print(f"\033[31m{message}\033[0m", file=__import__("sys").stderr)

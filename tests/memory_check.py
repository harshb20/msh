#!/usr/bin/env python3
"""Use glibc's allocation tracer to find allocations not released at exit."""

from __future__ import annotations

import argparse
import ctypes.util
import os
import pathlib
import subprocess
import sys
import tempfile


def outstanding_allocations(trace: pathlib.Path) -> dict[str, str]:
    outstanding: dict[str, str] = {}
    for line in trace.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) < 4 or parts[0] != "@":
            continue
        operation = parts[2]
        pointer = parts[3]
        if operation in {"+", ">"}:
            outstanding[pointer] = line
        elif operation in {"-", "<"}:
            outstanding.pop(pointer, None)
    return outstanding


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    args = parser.parse_args()
    shell = args.shell.resolve()
    debug_library = ctypes.util.find_library("c_malloc_debug")
    if debug_library is None:
        print("SKIP glibc malloc tracer is unavailable")
        return 0

    with tempfile.TemporaryDirectory(prefix="msh-memory-") as temporary:
        root = pathlib.Path(temporary)
        (root / "input.txt").write_text("beta\nalpha\nbeta\n", encoding="utf-8")
        trace = root / "allocations.log"
        environment = os.environ.copy()
        environment["LD_PRELOAD"] = debug_library
        environment["MALLOC_TRACE"] = str(trace)
        environment["LC_ALL"] = "C"
        script = "\n".join(
            [
                "echo start",
                "cat input.txt | sort | uniq -c | sed 's/^ *//' > counts.txt",
                "cat counts.txt",
                "false || echo recovered",
                "printf '<%s>\\n' \"$HOME\" > home.txt",
                "cat < home.txt > /dev/null",
                "echo done",
                "",
            ]
        )
        completed = subprocess.run(
            [str(shell)],
            cwd=root,
            env=environment,
            input=script.encode(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert completed.returncode == 0, (
            completed.returncode,
            completed.stdout,
            completed.stderr,
        )
        assert trace.exists() and trace.stat().st_size > 0, "mtrace produced no data"
        outstanding = outstanding_allocations(trace)
        assert not outstanding, "unreleased allocations:\n" + "\n".join(
            outstanding.values()
        )
    print("Memory result: 0 outstanding allocations in representative session")
    return 0


if __name__ == "__main__":
    sys.exit(main())

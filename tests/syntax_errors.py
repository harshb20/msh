#!/usr/bin/env python3
"""Ensure malformed input is rejected cleanly with status 2."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


CASES = [
    "echo 'unterminated",
    'echo "unterminated',
    "echo trailing\\",
    "echo hi |",
    "| echo hi",
    "echo >",
    "echo hi &&",
    "echo hi ||",
    "echo hi ;; echo no",
    "echo hi & & echo no",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    args = parser.parse_args()
    shell = args.shell.resolve()

    for command in CASES:
        completed = subprocess.run(
            [str(shell), "-c", command],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=3,
            check=False,
        )
        assert completed.returncode == 2, (
            command,
            completed.returncode,
            completed.stdout,
            completed.stderr,
        )
        assert b"syntax error" in completed.stderr, (command, completed.stderr)
    print(f"Syntax-error result: {len(CASES)}/{len(CASES)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())


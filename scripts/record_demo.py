#!/usr/bin/env python3
"""Record an asciinema v2 cast by driving msh through a pseudo-terminal."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import pty
import select
import signal
import sys
import tempfile
import time


PROMPT = b"msh$ "


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    shell = args.shell.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    demo_directory = tempfile.TemporaryDirectory(prefix="msh-demo-")

    pid, master = pty.fork()
    if pid == 0:
        os.chdir(demo_directory.name)
        os.environ["MSH_PROMPT"] = PROMPT.decode()
        os.environ["LC_ALL"] = "C"
        os.execv(str(shell), [str(shell)])

    started = time.monotonic()
    events: list[list[object]] = []
    collected = bytearray()

    def capture(duration: float = 0.4, until: bytes | None = None) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], 0.05)
            if not readable:
                continue
            try:
                data = os.read(master, 8192)
            except OSError:
                return
            if not data:
                return
            collected.extend(data)
            events.append([round(time.monotonic() - started, 6), "o", data.decode("utf-8", "replace")])
            if until is not None and until in collected:
                return

    def send(data: bytes, pause: float = 0.5) -> None:
        os.write(master, data)
        capture(pause)

    try:
        capture(2.0, PROMPT)
        send(b"printf 'pear\\napple\\npear\\n' | sort | uniq -c\n", 0.8)
        send(b"echo diagnostic > result.txt && cat < result.txt\n", 0.8)
        send(b"sleep 5\n", 0.35)
        send(b"\x1a", 0.8)
        send(b"jobs\n", 0.7)
        send(b"bg\n", 0.7)
        send(b"fg\n", 0.35)
        send(b"\x03", 0.8)
        send(b"sleep 0.3 &\n", 0.8)
        capture(1.0)
        send(b"exit\n", 0.3)
    finally:
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            waited, _ = os.waitpid(pid, os.WNOHANG)
            if waited == pid:
                break
            time.sleep(0.05)
        else:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(master)

    header = {
        "version": 2,
        "width": 100,
        "height": 28,
        "timestamp": int(time.time()),
        "env": {"SHELL": "msh", "TERM": "xterm-256color"},
        "title": "msh: pipelines, redirection, and job control",
    }
    with args.output.open("w", encoding="utf-8") as cast:
        cast.write(json.dumps(header) + "\n")
        for event in events:
            cast.write(json.dumps(event) + "\n")
    demo_directory.cleanup()
    print(f"wrote {args.output} ({len(events)} frames)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

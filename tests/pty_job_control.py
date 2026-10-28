#!/usr/bin/env python3
"""Exercise terminal ownership and signals through a real pseudo-terminal."""

from __future__ import annotations

import argparse
import os
import pathlib
import pty
import re
import select
import signal
import sys
import time


ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
PROMPT = b"MSH_TEST> "


class Session:
    def __init__(self, shell: pathlib.Path) -> None:
        pid, master = pty.fork()
        if pid == 0:
            os.environ["MSH_PROMPT"] = PROMPT.decode()
            os.environ["LC_ALL"] = "C"
            os.execv(str(shell), [str(shell)])
        self.pid = pid
        self.master = master
        self.transcript = bytearray()

    def send(self, data: bytes) -> None:
        os.write(self.master, data)

    def read_until(self, needle: bytes, timeout: float = 4.0) -> bytes:
        deadline = time.monotonic() + timeout
        chunk = bytearray()
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], 0.1)
            if not readable:
                continue
            try:
                data = os.read(self.master, 8192)
            except OSError:
                break
            if not data:
                break
            chunk.extend(data)
            self.transcript.extend(data)
            if needle in self.clean(bytes(chunk)):
                return self.clean(bytes(chunk))
        raise AssertionError(
            f"timed out waiting for {needle!r}; got {self.clean(bytes(chunk))!r}"
        )

    def read_prompt(self, timeout: float = 4.0) -> bytes:
        deadline = time.monotonic() + timeout
        chunk = bytearray()
        last_data = time.monotonic()
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], 0.05)
            if readable:
                try:
                    data = os.read(self.master, 8192)
                except OSError:
                    break
                if not data:
                    break
                chunk.extend(data)
                self.transcript.extend(data)
                last_data = time.monotonic()
                continue
            cleaned = self.clean(bytes(chunk))
            if time.monotonic() - last_data >= 0.1 and cleaned.endswith(PROMPT):
                return cleaned
        raise AssertionError(
            f"timed out waiting for completed prompt; got {self.clean(bytes(chunk))!r}"
        )

    @staticmethod
    def clean(data: bytes) -> bytes:
        return ANSI.sub(b"", data).replace(b"\r", b"")

    def command(self, command: str, timeout: float = 4.0) -> bytes:
        self.send(command.encode() + b"\n")
        return self.read_prompt(timeout)

    def close(self) -> None:
        try:
            self.send(b"exit\n")
        except OSError:
            pass
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            waited, _ = os.waitpid(self.pid, os.WNOHANG)
            if waited == self.pid:
                break
            time.sleep(0.05)
        else:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        os.close(self.master)


def require(data: bytes, expected: bytes, label: str) -> None:
    if expected not in data:
        raise AssertionError(f"{label}: expected {expected!r} in {data!r}")
    print(f"PASS {label}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    args = parser.parse_args()
    session = Session(args.shell.resolve())
    try:
        session.read_until(PROMPT)

        output = session.command("printf 'b\\na\\nb\\n' | sort | uniq -c")
        require(output, b"2 b", "concurrent pipeline")

        session.send(b"sleep 10\n")
        time.sleep(0.25)
        session.send(b"\x03")
        output = session.read_prompt()
        output += session.command("echo $?")
        require(output, b"130", "Ctrl+C reaches foreground job")

        session.send(b"sleep 10\n")
        time.sleep(0.25)
        session.send(b"\x1a")
        output = session.read_prompt()
        require(output, b"Stopped", "Ctrl+Z stops foreground job")

        output = session.command("jobs")
        require(output, b"Stopped", "jobs reports stopped state")
        output = session.command("bg")
        require(output, b"sleep 10", "bg resumes stopped job")
        output = session.command("jobs")
        require(output, b"Running", "jobs reports running state")

        session.send(b"fg\n")
        time.sleep(0.25)
        session.send(b"\x03")
        output = session.read_prompt()
        require(output, b"sleep 10", "fg transfers terminal ownership")

        output = session.command("sleep 0.2 &")
        require(output, b"[", "background launch reports job id")
        output = session.read_until(b"Done", timeout=3.0)
        require(output, b"Done", "background completion notification")

        output = session.command("echo terminal-ok")
        require(output, b"terminal-ok", "shell survives terminal signals")

        output = session.command("echo history-entry")
        require(output, b"history-entry", "history seed command")
        session.send(b"\x1b[A\n")
        output = session.read_prompt()
        require(output, b"history-entry", "up arrow recalls history")

        session.send(b"echo ac\x1b[Db\n")
        output = session.read_prompt()
        require(output, b"abc", "left arrow edits within a line")
    finally:
        session.close()
    print("\nPTY and line-editing result: 13/13 passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

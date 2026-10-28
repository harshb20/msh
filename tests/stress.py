#!/usr/bin/env python3
"""Check for descriptor drift and unreaped children under repeated work."""

from __future__ import annotations

import argparse
import os
import pathlib
import select
import subprocess
import sys
import time


def proc_path(pid: int, parent_host_pid: int) -> pathlib.Path:
    direct = pathlib.Path(f"/proc/{pid}")
    if direct.exists():
        return direct
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        for item in pathlib.Path("/proc").iterdir():
            if not item.name.isdigit():
                continue
            try:
                status = (item / "status").read_text(encoding="utf-8")
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                continue
            fields = dict(
                line.split(":", 1) for line in status.splitlines() if ":" in line
            )
            namespace_pids = fields.get("NSpid", "").split()
            parent = fields.get("PPid", "").strip()
            if namespace_pids and namespace_pids[-1] == str(pid) and parent == str(parent_host_pid):
                return item
        time.sleep(0.01)
    raise AssertionError(f"/proc entry for shell namespace pid {pid} is unavailable")


def fd_count(process_path: pathlib.Path) -> int:
    return len(list((process_path / "fd").iterdir()))


def read_until(stream, marker: bytes, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    fd = stream.fileno()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.1)
        if not readable:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        data.extend(chunk)
        if marker in data:
            return bytes(data)
    raise AssertionError(f"timeout waiting for {marker!r}")


def child_states(process_path: pathlib.Path) -> list[str]:
    parent_host_pid = process_path.name
    states: list[str] = []
    for item in pathlib.Path("/proc").iterdir():
        if not item.name.isdigit():
            continue
        try:
            status = (item / "status").read_text(encoding="utf-8")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        fields = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)
        namespace_pids = fields.get("NSpid", "").split()
        if not namespace_pids or namespace_pids[0] != item.name:
            continue
        if fields.get("PPid", "").strip() == parent_host_pid:
            state = fields.get("State", "").strip().split(maxsplit=1)[0]
            states.append(state)
    return states


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    args = parser.parse_args()
    shell = args.shell.resolve()

    process = subprocess.Popen(
        [str(shell)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    time.sleep(0.05)
    if process.poll() is not None:
        error = b"" if process.stderr is None else process.stderr.read()
        raise AssertionError(
            f"shell exited before stress test with {process.returncode}: {error!r}"
        )
    process.stdin.write(b"echo __MSH_STRESS_READY__\n")
    process.stdin.flush()
    read_until(process.stdout, b"__MSH_STRESS_READY__", 5.0)
    parent_host_pid = int(pathlib.Path("/proc/self").resolve().name)
    shell_process_path = proc_path(process.pid, parent_host_pid)
    baseline = fd_count(shell_process_path)

    workload = []
    for _ in range(100):
        workload.append("printf x | cat | cat | wc -c > /dev/null\n")
    for _ in range(100):
        workload.append("true &\n")
    workload.append("sleep 0.5\n")
    workload.append("echo __MSH_STRESS_DONE__\n")
    process.stdin.write("".join(workload).encode())
    process.stdin.flush()
    read_until(process.stdout, b"__MSH_STRESS_DONE__", 20.0)
    time.sleep(0.2)

    after = fd_count(shell_process_path)
    states = child_states(shell_process_path)
    zombies = [state for state in states if state.startswith("Z")]
    assert after == baseline, f"fd count changed: {baseline} -> {after}"
    assert not zombies, f"zombie children remain: {zombies}"
    print(f"PASS fd count stable across 100 pipelines: {baseline} -> {after}")
    print("PASS no zombies after 100 foreground pipelines and 100 background jobs")

    process.stdin.write(b"exit\n")
    process.stdin.flush()
    process.stdin.close()
    status = process.wait(timeout=5)
    assert status == 0, f"shell exited with {status}"
    print("\nStress result: 2/2 passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

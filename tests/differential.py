#!/usr/bin/env python3
"""Run the same commands through bash and msh, comparing observable results."""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    command: str


def cases() -> list[Case]:
    result: list[Case] = []

    def add(group: str, commands: Iterable[str]) -> None:
        for index, command in enumerate(commands, 1):
            result.append(Case(f"{group}-{index:02d}", command))

    add(
        "simple",
        [
            "echo hello",
            "echo hello world",
            "printf abc",
            "printf '%s\\n' one two three",
            "true",
            "false",
            ":",
            "echo -n no-newline",
            "echo first; echo second",
            "echo one; echo two; echo three",
            "printf '%s:%s\\n' left right",
            "printf '[%s]\\n' ''",
        ],
    )
    add(
        "quotes",
        [
            "echo \"a  b\"",
            "echo 'a  b'",
            "echo a\\ \\ b",
            "echo abc\"def\"'ghi'",
            "echo '\"double\"'",
            "echo \"'single'\"",
            "echo \"a\\\"b\"",
            "echo 'a$B'",
            "echo \"a$B\"",
            "echo a\\$B",
            "echo \"a\\$B\"",
            "printf '<%s>\\n' \"\"",
            "printf '<%s>\\n' ''",
            "printf '%s\\n' one\" two\"three",
            "echo back\\\\slash",
            "echo semi\\;colon",
            "echo pipe\\|literal",
            "echo amp\\&literal",
        ],
    )
    add(
        "variables",
        [
            "echo $B",
            "echo ${B}",
            "echo pre${B}post",
            "echo '$B'",
            "echo \"$B\"",
            "printf '<%s>\\n' $SPLIT",
            "printf '<%s>\\n' \"$SPLIT\"",
            "printf '<%s>\\n' x$SPLIT",
            "printf '<%s>\\n' $SPLIT-x",
            "printf '<%s>\\n' $UNSET",
            "printf '<%s>\\n' \"$UNSET\"",
            "false; echo $?",
            "true; echo $?",
            "sh -c 'exit 23'; echo $?",
            "export LOCAL=value; echo $LOCAL",
            "export LOCAL='two words'; echo \"$LOCAL\"",
            "export LOCAL=value; unset LOCAL; echo x${LOCAL}y",
            "echo $$ | grep '^[0-9][0-9]*$' > /dev/null",
        ],
    )
    add(
        "pipelines",
        [
            "printf 'b\\na\\n' | sort",
            "printf 'a\\nb\\na\\n' | grep a | wc -l",
            "printf 'pear\\napple\\npear\\n' | sort | uniq -c | sed 's/^ *//'",
            "cat input.txt | grep alpha",
            "cat input.txt | grep -v alpha | sort",
            "cat input.txt | sort | uniq | wc -l",
            "printf abc | cat | cat | cat | wc -c",
            "printf 'x y\\nz\\n' | tr ' ' '\\n' | sort | head -n 2",
            "false | true; echo $?",
            "true | false; echo $?",
            "printf 'one\\ntwo\\nthree\\n' | head -n 2 | tail -n 1",
            "seq 5 | grep '[24]' | paste -sd, -",
            "yes x | head -n 1",
        ],
    )
    add(
        "redirect",
        [
            "echo hello > out.txt; cat out.txt",
            "echo first > out.txt; echo second >> out.txt; cat out.txt",
            "cat < input.txt",
            "grep alpha < input.txt > out.txt; cat out.txt",
            "printf abc | wc -c > count.txt; cat count.txt",
            "cat input.txt | grep beta > out.txt; cat out.txt",
            "echo replaced > existing.txt; cat existing.txt",
            "echo appended >> existing.txt; cat existing.txt",
            "sh -c 'echo problem >&2' 2> err.txt; cat err.txt",
            "sh -c 'echo first >&2' 2> err.txt; sh -c 'echo second >&2' 2>> err.txt; cat err.txt",
            "sh -c 'echo out; echo err >&2' > both.txt 2>&1; cat both.txt",
            "sh -c 'echo out; echo err >&2' &> both.txt; cat both.txt",
            "sh -c 'echo out; echo err >&2' &>> both.txt; cat both.txt",
            "> empty.txt; wc -c < empty.txt",
            "echo spaced > 'space name.txt'; cat 'space name.txt'",
            "echo before > out.txt; cat < out.txt | tr a-z A-Z",
            "echo data 1> numbered.txt; cat numbered.txt",
            "sh -c 'echo err >&2' 2>numbered.err; cat numbered.err",
            "sh -c 'echo out; echo err >&2' 2>&1 > ordered.txt; cat ordered.txt",
        ],
    )
    add(
        "conditionals",
        [
            "true && echo yes",
            "false && echo no",
            "false || echo fallback",
            "true || echo no",
            "true && echo first && echo second",
            "false || echo first || echo second",
            "false && echo no || echo yes",
            "true || echo no && echo yes",
            "sh -c 'exit 7' || echo recovered",
            "sh -c 'exit 7' && echo no; echo $?",
            "true && false || echo final",
            "false || true && echo final",
        ],
    )
    add(
        "glob",
        [
            "printf '%s\\n' *.txt",
            "printf '%s\\n' a*.txt",
            "printf '%s\\n' ?.log",
            "printf '%s\\n' [ab].txt",
            "printf '%s\\n' z*.missing",
            "printf '%s\\n' '*.txt'",
            "printf '%s\\n' \\*.txt",
            "printf '%s\\n' \"*.txt\"",
            "printf '%s\\n' sub/*.txt",
            "printf '%s\\n' no-match-?",
        ],
    )
    add(
        "comments",
        [
            "echo visible # hidden words",
            "echo visible#not-comment",
            "# entire line",
            "echo hash\\#literal",
            "echo '# literal'",
            "echo \"# literal\"",
        ],
    )
    add(
        "builtins",
        [
            "pwd",
            "cd sub; pwd",
            "cd sub; cd ..; pwd",
            "cd sub; cd -; pwd",
            "export A=1; export C=2; echo $A$C",
            "export A=old; export A=new; echo $A",
            "export A=1; unset A; printf '<%s>\\n' \"$A\"",
            "echo one; history",
        ],
    )
    add(
        "status",
        [
            "exit 0",
            "exit 7",
            "exit 255",
            "exit 256",
            "sh -c 'exit 42'",
            "sh -c 'kill -TERM $$'; echo $?",
            "sh -c 'kill -INT $$'; echo $?",
            "false; exit $?",
            "true; exit $?",
            "false || exit 19",
        ],
    )
    add(
        "tilde-and-sequence",
        [
            "echo ~",
            "echo ~/folder",
            "printf '%s\\n' ~",
            "echo one; true; echo two",
            "echo one; false; echo two",
            "false || echo recovered; echo done",
            "true && echo passed; echo done",
            "echo x > out.txt; cat out.txt; echo y >> out.txt; tail -n 1 out.txt",
        ],
    )
    return result


def populate(directory: pathlib.Path) -> None:
    directory.mkdir(parents=True)
    (directory / "input.txt").write_text("alpha\nbeta\nalpha\ngamma\n", encoding="utf-8")
    (directory / "existing.txt").write_text("original\n", encoding="utf-8")
    (directory / "a.txt").write_text("a\n", encoding="utf-8")
    (directory / "b.txt").write_text("b\n", encoding="utf-8")
    (directory / "c.log").write_text("c\n", encoding="utf-8")
    (directory / "sub").mkdir()
    (directory / "sub" / "nested.txt").write_text("nested\n", encoding="utf-8")


def normalize(data: bytes, cwd: pathlib.Path) -> str:
    text = data.decode("utf-8", errors="replace").replace("\r\n", "\n")
    return text.replace(str(cwd), "<WORKDIR>")


def run_one(executable: list[str], command: str, cwd: pathlib.Path) -> dict[str, object]:
    env = os.environ.copy()
    env.update(
        {
            "LC_ALL": "C",
            "LANG": "C",
            "HOME": str(cwd),
            "B": "bee",
            "SPLIT": "x y",
            "EMPTY": "",
        }
    )
    completed = subprocess.run(
        [*executable, command],
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    return {
        "stdout": normalize(completed.stdout, cwd),
        "stderr": normalize(completed.stderr, cwd),
        "status": completed.returncode,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("shell", type=pathlib.Path)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--json", type=pathlib.Path, default=pathlib.Path("tests/last-results.json"))
    args = parser.parse_args()
    shell = args.shell.resolve()
    all_cases = cases()
    outcomes: list[dict[str, object]] = []

    with tempfile.TemporaryDirectory(prefix="msh-diff-") as temporary:
        root = pathlib.Path(temporary)
        for case in all_cases:
            case_root = root / case.name
            bash_dir = case_root / "bash"
            msh_dir = case_root / "msh"
            populate(bash_dir)
            shutil.copytree(bash_dir, msh_dir)

            try:
                bash_result = run_one(["bash", "--noprofile", "--norc", "-c"], case.command, bash_dir)
                msh_result = run_one([str(shell), "-c"], case.command, msh_dir)
                passed = bash_result == msh_result
                reason = "" if passed else ", ".join(
                    key for key in ("stdout", "stderr", "status")
                    if bash_result[key] != msh_result[key]
                )
            except subprocess.TimeoutExpired:
                bash_result = {}
                msh_result = {}
                passed = False
                reason = "timeout"

            outcomes.append(
                {
                    "name": case.name,
                    "command": case.command,
                    "passed": passed,
                    "difference": reason,
                    "bash": bash_result,
                    "msh": msh_result,
                }
            )
            marker = "PASS" if passed else "FAIL"
            if args.verbose or not passed:
                print(f"{marker:4} {case.name:28} {case.command}")
                if not passed and args.verbose:
                    print(f"     bash={bash_result!r}")
                    print(f"     msh ={msh_result!r}")

    passed_count = sum(bool(item["passed"]) for item in outcomes)
    report = {
        "total": len(outcomes),
        "passed": passed_count,
        "failed": len(outcomes) - passed_count,
        "pass_rate": round(100.0 * passed_count / len(outcomes), 2),
        "cases": outcomes,
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\nDifferential result: {passed_count}/{len(outcomes)} ({report['pass_rate']:.2f}%)")
    print(f"Detailed report: {args.json}")
    return 0 if passed_count == len(outcomes) else 1


if __name__ == "__main__":
    sys.exit(main())

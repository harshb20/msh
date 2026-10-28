# msh — a Unix shell in C

**Bash differential result: 134/134 cases passed (100.00%).** The suite compares stdout, stderr, and exit status for every case; its complete case list is in [`tests/differential.py`](tests/differential.py), and the latest machine-readable run is in [`tests/last-results.json`](tests/last-results.json).

`msh` is a small Unix shell built directly on `fork`, `execvp`, `waitpid`, process groups, terminal ownership, signals, and file descriptors. It deliberately avoids a shell framework and Readline so the process-control machinery remains visible.

## Feature status

| Area | Implemented behavior |
|---|---|
| Execution | `fork()` / `execvp()` / signal-aware waiting; `$?` and signal statuses |
| Pipelines | Arbitrary length, concurrent launch, deterministic closure of every unused pipe end |
| Redirection | `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`, `&>`, `&>>`, including ordered combinations with pipes |
| Job control | `&`, `jobs`, `fg`, `bg`, process groups, `tcsetpgrp()`, stopped/running/done states |
| Signals | Ctrl+C targets the foreground job; Ctrl+Z stops it; Ctrl+D exits an empty prompt |
| Builtins | `cd` (including `cd -`), `pwd`, `exit`, `export`, `unset`, `echo`, `jobs`, `fg`, `bg`, `history` |
| Parsing | Single/double quotes, shell escapes, comments, `;`, `&&`, `||` |
| Expansion | `$VAR`, `${VAR}`, `$?`, `$$`, tilde, field splitting, and `glob()` pathname expansion |
| Line editing | Dependency-free `termios` editor with left/right/up/down, Home/End, Delete, and history recall |

## Build and run

Requirements are a Linux/POSIX environment, a C11 compiler, `make`, and Python 3 for tests.

```sh
make
./msh
```

It also accepts a single command string:

```sh
./msh -c 'printf "pear\napple\npear\n" | sort | uniq -c'
```

Example interactive session:

```console
msh$ printf 'pear\napple\npear\n' | sort | uniq -c
      1 apple
      2 pear
msh$ sleep 30
^Z
[2]+  Stopped  sleep 30
msh$ bg
[2]+ sleep 30 &
msh$ fg
sleep 30
^C
msh$
```

The recorded terminal session covers pipelines, redirection, Ctrl+Z, `jobs`, `bg`, `fg`, Ctrl+C, and asynchronous completion:

```sh
asciinema play demo/msh.cast
```

The cast uses asciinema v2's open JSON-lines format; recording it does not require asciinema itself:

```sh
make demo
```

## Verification

Run every release check with:

```sh
make test
```

| Check | Latest result | What it establishes |
|---|---:|---|
| Bash differential | 134/134 | stdout, stderr, and status parity across commands, quotes, variables, globs, pipes, redirections, short-circuiting, builtins, and signals |
| Real pseudo-terminal | 13/13 | foreground terminal ownership, Ctrl+C, Ctrl+Z, `jobs`/`fg`/`bg`, background notification, history, and cursor editing |
| Syntax rejection | 10/10 | malformed quotes, pipes, lists, and redirections fail cleanly with status 2 |
| Pipeline/child stress | Passed | fd count remains **5 → 5** after 100 four-process pipelines; no zombies after those plus 100 background jobs |
| Allocation trace | Passed | **0 outstanding allocations** after a representative multi-command session using glibc `mtrace` |
| ASan + UBSan | Passed | full differential, PTY, and stress suites complete without sanitizer findings |

To build with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/differential.py ./msh
```

LeakSanitizer cannot initialize under some ptrace-based runners. `make test-memory` therefore performs an independent glibc allocation trace and requires zero outstanding allocations. Run `make clean && make` after a sanitizer build to restore the normal binary.

## Design highlights

- The parent and child both call `setpgid()`, closing the classic race over process-group creation.
- A launch barrier holds children until the parent has assembled the whole pipeline and transferred the terminal, preventing an early reader from receiving `SIGTTIN`.
- `SIGCHLD` is blocked around fork/job-table critical sections. The handler performs nonblocking `waitpid()` calls, appends compact events to a fixed signal-safe queue, and wakes the main loop through a self-pipe.
- Pipeline children close all pipe descriptors after their two possible `dup2()` operations. The parent closes its copies immediately after forking.
- Redirections are stored and applied in lexical order, so `>file 2>&1` and `2>&1 >file` behave differently, as they do in Bash.
- Standalone builtins execute in the parent with save/apply/restore redirection. Builtins in a pipeline execute in a child, matching normal shell state semantics.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full data flow and invariants. [`docs/DIVERGENCES.md`](docs/DIVERGENCES.md) lists intentional limits rather than pretending this educational shell implements Bash's entire language.

## Repository layout

| Path | Responsibility |
|---|---|
| `include/shell.h` | Shared AST, job, and shell state |
| `src/lexer.c` | Quote-preserving tokenizer |
| `src/parser.c` | Recursive-descent parser and AST ownership |
| `src/expand.c` | Variables, fields, tilde, and glob expansion |
| `src/exec.c` | Redirection, pipelines, and process launch |
| `src/jobs.c` | Process groups, SIGCHLD queue, and `fg`/`bg` |
| `src/builtins.c` | Parent-state and ordinary builtins |
| `src/line_editor.c` | Raw terminal editor and asynchronous redraw |
| `src/main.c` | Shell lifecycle and read/execute loop |
| `tests/` | Differential, PTY, stress, syntax, and memory checks |
| `demo/msh.cast` | Reproducible terminal recording |

## Resume bullet

> Built a Unix shell in C with concurrent pipelines, ordered I/O redirection, process-group job control, terminal signal routing, quote-aware expansion, and a raw-mode line editor; achieved 134/134 Bash differential cases with PTY, zombie, fd-leak, and allocation-trace verification.

## Interview starting points

- Why does one leaked pipe write end prevent a reader from observing EOF?
- Why must `cd`, `export`, and job-control builtins run in the parent?
- Why are `setpgid()` and `tcsetpgrp()` separate operations?
- Why does the parent call `setpgid()` even though every child does too?
- Why does a signal handler feed an event queue instead of mutating linked job structures?
- What does a shell return when a foreground process dies from signal *N*?

## License

MIT

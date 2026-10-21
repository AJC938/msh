# msh

A minimal Unix-style interactive shell written in C11 against POSIX
interfaces. Built as a learning portfolio project, with correctness and clear
architecture prioritised over feature count.

`msh` is **not** a Bash replacement and does not claim POSIX shell
compliance.

## Repository history

This repository was **reconstructed on 2026-10-21**. An earlier milestone-1
repository existed at `C:\Users\96650\dev\msh` with initial commit `b6c110c`,
but that directory and its Git history could not be located and are **not
recovered**. The history here starts fresh; `b6c110c` is not an ancestor of
anything in this repository.

The source was reconstructed from the project requirements and the original
implementation, so the code is equivalent — but the commit history, authorship
dates and that commit object are gone.

## Current state: milestone 1 of 10

Implemented:

- An interactive read-eval-print loop with an `msh$ ` prompt.
- Input lines of unbounded length — no fixed-size buffer, no truncation.
- Blank and whitespace-only lines treated as no-ops that leave the exit
  status untouched.
- Clean termination at end of input (Ctrl+D at a terminal), with no spin or
  infinite loop.
- A final line with no trailing newline handled as a line in its own right.
- The prompt suppressed when stdin is not a terminal, so `msh` can be driven
  from a pipe or a file in tests.
- CRLF-terminated input tolerated.
- Unsupported command-line arguments rejected rather than ignored.

**Not implemented yet** — these belong to later milestones and are
deliberately absent:

| Feature | Milestone |
| --- | --- |
| External command execution (`fork`/`execvp`/`waitpid`) | 3 |
| Exit-status tracking for real commands | 4 |
| Built-ins: `cd`, `pwd`, `exit`, `help` | 5 |
| Quoting and structured parsing | 6 |
| I/O redirection | 7 |
| Pipelines | 8 |
| Background jobs, signals, job control | 9 |

Typing a non-blank command today prints a clear "not implemented yet"
message on stderr and yields exit status 127.

## Build status

Verified on native Linux (Ubuntu 24.04.5 LTS under WSL2, gcc 13.3.0) on
2026-10-21: clean build with zero warnings under the full warning set, and
**28 automated checks passing** — 13 unit, 12 integration, 3 at a real
terminal — with ASan, UBSan and Valgrind all clean and no file-descriptor
leaks. See [docs/architecture.md](docs/architecture.md) for the table and the
exact versions.

```sh
make            # optimised build -> out/msh
make test       # unit + integration tests
make test-pty   # Ctrl+D at a real terminal (Linux only)
./out/msh       # start an interactive shell; press Ctrl+D to exit
```

CI has never run: no remote is configured, by instruction.

## Requirements

- Linux (including WSL2 on Windows)
- GCC with C11 support, or Clang
- GNU Make

Optional for the development workflow: GDB, AddressSanitizer and
UndefinedBehaviorSanitizer (both ship with GCC on Linux), and Valgrind.

## Targets

```sh
make            # optimised build -> out/msh
make debug      # -O0 -g3, for GDB
make test       # unit + integration tests
make test-pty   # Ctrl+D at a real terminal (Linux only)
make asan       # AddressSanitizer + UBSan build, then the test suite
make clean      # remove out/
```

`make debug` and `make asan` keep the full warning set and `-std=c11`; they
change only the optimisation and instrumentation flags.

## Tests

- **Unit tests** (`tests/unit/`) cover the line reader directly: newline
  stripping, blank lines, EOF, unterminated final lines, buffer growth past
  64 KiB, CRLF, embedded NUL bytes, and invalid arguments. Plus shell-level
  blank-line classification and dispatch status. 13 checks.
- **Integration tests** (`tests/integration/test_repl.sh`) drive the built
  binary through pipes: exit codes, prompt suppression on a non-tty, a
  200 000-character input line, and argument rejection. 12 checks.
- **Terminal test** (`tests/integration/test_pty.c`, `make test-pty`) uses
  `forkpty()` to confirm the prompt appears at a real terminal and that
  Ctrl+D (0x04) produces a clean exit. This is the only test that exercises
  the genuine Ctrl+D path; the piped tests reach end-of-input a different
  way and are not equivalent.

## Layout

```
src/
  main.c        entry point and argument handling
  shell.c/.h    shell state and the REPL loop
  lineread.c/.h unbounded single-line reader
tests/
  unit/         assertion-based unit tests (no external framework)
  integration/  black-box tests against the built binary
docs/
  architecture.md
```

Modules for parsing, execution, built-ins, redirection, pipelines and job
control are added in the milestones that need them, rather than created
empty up front.

## Licence

Not yet chosen.

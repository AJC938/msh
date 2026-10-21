# msh architecture

This document describes what is actually implemented. It is updated as each
milestone lands, not ahead of it.

## Provenance

This repository was reconstructed on 2026-10-21 at
`/home/mshuser/projects/msh`. A previous milestone-1 repository existed at
`C:\Users\96650\dev\msh` with initial commit `b6c110c`; that directory and its
Git history were searched for and **not found**. The history here is new. No
claim is made that `b6c110c` or any earlier commit is preserved.

What was reconstructed is the source, tests, build system and documentation —
equivalent in behaviour to the original implementation. What is permanently
lost is the commit history.

## Current scope (milestone 1)

A read-eval-print loop that reads lines and terminates cleanly. No command
execution, parsing, redirection, pipelines or job control yet.

## Module boundaries

```
main.c  ->  shell.c  ->  lineread.c
```

| Module | Responsibility | Deliberately does not |
| --- | --- | --- |
| `main.c` | Process entry, argument rejection, owns the `msh_shell` value | Contain loop or input logic |
| `shell.c` | Shell state, the REPL, prompting, blank-line policy | Know how bytes become a line |
| `lineread.c` | Turn a byte stream into one heap line of any length | Know anything about shells |

The split exists for one practical reason: `lineread` takes a `FILE *`, so
input behaviour is unit-testable without a terminal and without spawning the
binary. `shell.c` holds the policy decisions (what a blank line means, when
to prompt) that are not interesting to test at byte level.

`msh_dispatch` in `shell.c` is the single seam where command execution will
attach in milestone 3. The REPL does not need to change for that.

## Memory ownership

The rule is one-way and explicit:

- `msh_read_line` **allocates** and transfers ownership to the caller. On
  `MSH_READ_OK` the caller must `free()` the buffer. On `MSH_READ_EOF` and
  `MSH_READ_ERROR` the out-pointer is set to `NULL` and there is nothing to
  free — so a caller that always frees is still correct.
- On allocation or stream failure, `msh_read_line` frees its own partial
  buffer before returning. There is no partially-owned state.
- `msh_dispatch` **borrows** its `line` argument. It never frees or retains
  it; the REPL frees the line after dispatch returns.
- `msh_shell` holds no heap-allocated members and needs no destructor. If a
  later milestone adds one, a `msh_shell_free` is introduced with it.

No file descriptors are opened in this milestone, so there is no FD
lifecycle to manage yet. `stdin`/`stdout`/`stderr` are used but never closed.

## Buffer growth

`msh_read_line` starts at 128 bytes and doubles. The doubling is guarded
against `size_t` overflow before it happens, so a pathological input fails
with `MSH_READ_ERROR` rather than wrapping around and under-allocating.
There is no maximum line length other than available memory.

Growth is covered by test, not by inspection:
`test_very_long_line_grows_buffer` reads a 64 KiB line, and the integration
suite pushes a 200 000-character line through the real binary.

## Input semantics

| Input | Behaviour |
| --- | --- |
| `cmd\n` | line is `cmd` |
| `cmd\r\n` | line is `cmd` — CRLF tolerated |
| `\n` | empty line; no-op, exit status unchanged |
| whitespace only | no-op, exit status unchanged |
| `cmd` then EOF | line is `cmd`, then EOF on the next read |
| EOF with nothing pending | loop ends, shell exits |
| stream error | message on stderr, exit status 1 |

Embedded NUL bytes are preserved in the buffer and reported through the
`out_len` parameter, since C string functions would otherwise hide them. No
milestone-1 behaviour depends on this, but discarding the information
silently would be a trap for the parser in milestone 6.

## Exit status

The shell exits with the status of the last line it handled. A blank line
does not disturb it, matching the behaviour of `$?` at an `sh` prompt. A
not-yet-implemented command yields 127, the conventional "command not found"
status, so the value is already meaningful when real execution arrives.

## Error-handling policy

System and library calls are checked. Specifically:

- `isatty` is used for its value only; a false result simply means "not
  interactive".
- `fgetc` is followed by a `ferror` check so a genuine read error is not
  silently reported as end of input.
- `malloc`/`realloc` failures propagate as `MSH_READ_ERROR` and the REPL
  reports them and exits non-zero.
- The prompt is flushed explicitly, because it has no trailing newline and
  stdout buffering is not guaranteed to be line-buffered in every
  environment.

## Verification status

Verified on native Linux on 2026-10-21. Every row below was produced by
running the command and reading its output.

Environment:

| Component | Version |
| --- | --- |
| OS | Ubuntu 24.04.5 LTS (WSL2) |
| Kernel | 6.18.54.1-microsoft-standard-WSL2 |
| Compiler | gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Make | GNU Make 4.3 |
| Debugger | GNU gdb 15.1 |
| Valgrind | valgrind-3.22.0 |
| Git | git 2.43.0 |

| Check | Result |
| --- | --- |
| Clean build, full warning set | **PASS** — 0 warnings, 0 errors; `out/msh` is an ELF 64-bit x86-64 PIE executable |
| Unit tests — `test_lineread` (9) | **PASS** |
| Unit tests — `test_shell` (4) | **PASS** |
| Integration tests, piped stdin (12) | **PASS** |
| `make test-pty` — Ctrl+D at a real terminal (3) | **PASS** |
| `make asan` — ASan + UBSan, full suite | **PASS** — no sanitizer diagnostics |
| Valgrind memcheck, 7 REPL input cases | **PASS** — 0 errors from 0 contexts |
| Valgrind memcheck, unit test binaries | **PASS** |
| File-descriptor leaks | **PASS** — `FILE DESCRIPTORS: 3 open (3 std) at exit` |
| CI workflow | **NEVER EXECUTED** — no remote configured, by instruction |

Total: **28 automated checks, all passing** on native Linux.

### Ctrl+D is now genuinely verified

This was the one gap that survived every earlier round, because a pipe
reaching its end is not the same event as a user pressing Ctrl+D.
`tests/integration/test_pty.c` closes it: it allocates a pseudo-terminal with
`forkpty()`, confirms the prompt is written (which can only happen if
`isatty()` returned true), writes a literal `0x04`, and requires a clean exit
status of 0. The line discipline turns that byte into a zero-length read, so
this exercises the real end-of-file path rather than an analogue of it.

### Controls run against the test suite itself

Passing tests are only evidence if they are capable of failing:

- The integration harness was previously checked by corrupting one expected
  exit status; it correctly reported `1 of 12 checks FAILED` and exited 1.
- The ASan build was confirmed to be genuinely instrumented — `nm out/msh`
  shows 15 `asan` symbols — so the clean sanitizer result is not a silently
  skipped build. The `make debug` binary used for Valgrind shows 0, confirming
  the two tools were not mixed.

### Not covered

- No remote and no CI run, by instruction.
- `make test-pty` is Linux-only; it needs `forkpty()` and `-lutil`, so it is
  deliberately excluded from `make test`.
- Milestone 1 opens no files and spawns no processes, so the Valgrind
  file-descriptor result is a baseline, not yet an interesting one. It becomes
  meaningful in milestones 7 and 8.

## Next milestone

Milestone 1 is complete and verified on native Linux; milestone 2 (the REPL
itself) is covered by the same work.

Milestone 3 is external command execution: `fork()`, `execvp()`, `waitpid()`,
and honest reporting of spawn failures. The seam for it already exists —
`msh_dispatch` in `shell.c` — so the REPL does not need to change.

Two things to carry into that work:

- `msh_shell` gains no heap members in milestone 1. If execution adds any, a
  `msh_shell_free` must be introduced alongside them.
- The Valgrind file-descriptor baseline (`3 open (3 std) at exit`) only starts
  being informative once processes are spawned and files are opened. Keep
  `--track-fds=yes` in the loop from milestone 3 onward.

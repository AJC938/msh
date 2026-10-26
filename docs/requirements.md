# msh — Requirements

What msh is required to do, and the boundary of what it is not. Each
requirement is numbered so a test or a review can cite it. Requirements marked
**OUT** are explicitly out of scope: they are listed so the boundary is a
decision on record rather than an omission.

The verification column names the suite that covers the requirement:

| Tag | Suite |
| --- | --- |
| U | `tests/unit/` — `make test-unit` |
| I | `tests/integration/*.sh` — `make test-integration` |
| P | `tests/integration/test_pty*.c` — `make test-pty` |
| M | `make valgrind` and `make asan` |

---

## 1. Scope and intent

| # | Requirement |
| --- | --- |
| 1.1 | msh is an **educational minimal shell**, not a Bash replacement and not an implementation of the POSIX shell command language. |
| 1.2 | It implements a small subset, chosen deliberately, with the boundary documented rather than discovered. |
| 1.3 | It is written in **C11** against **POSIX.1-2008** interfaces, with no third-party libraries. |
| 1.4 | It must not delegate any of its own work — parsing, redirection, pipelines, job control — to `system()`, `popen()`, or another shell. |
| 1.5 | It must compile with no warnings under the project's full warning set, on both GCC and Clang. |

## 2. Input handling

| # | Requirement | Verified |
| --- | --- | --- |
| 2.1 | Read one line of input at a time, of **unbounded length**; no fixed buffer may truncate a line. | U, I |
| 2.2 | Print a prompt (`msh$ `) before each line **only when standard input is a terminal**. | I, P |
| 2.3 | End-of-file (Ctrl+D at a terminal, or a closed pipe) terminates the shell with the last command's status. | I, P |
| 2.4 | A blank or whitespace-only line runs nothing and leaves the exit status unchanged. | I |
| 2.5 | A read interrupted by a signal mid-line must resume, not discard the partial line. | U |
| 2.6 | An interrupt with nothing typed yet returns to a fresh prompt without the prompt being printed twice. | P |
| 2.7 | Memory for input must be released on every path, including error paths. | M |

## 3. Parsing

| # | Requirement | Verified |
| --- | --- | --- |
| 3.1 | Split a line into words on unquoted whitespace. `strtok()` is not an acceptable implementation. | U, I |
| 3.2 | `'...'` quotes everything literally, including backslashes and operators. | U, I |
| 3.3 | `"..."` groups text; `\"` and `\\` are the **only** escapes recognised inside it, and any other backslash is literal. | U, I |
| 3.4 | A backslash outside quotes escapes the next character, including a space, an operator, and a quote. | U, I |
| 3.5 | Adjacent quoted and unquoted runs concatenate into one word: `ab"cd"'ef'` is one argument. | U, I |
| 3.6 | `''` and `""` produce one argument of length zero, which must survive to `execvp`. | U, I |
| 3.7 | Operators (`<`, `>`, `>>`, `\|`, `&`) are recognised **only** when unquoted and unescaped, and need no surrounding whitespace. | U, I |
| 3.8 | A trailing unmatched `'`, unmatched `"`, or trailing backslash is a syntax error, not a silently accepted line. | U, I |
| 3.9 | Every syntax error reports a specific diagnostic naming what was wrong, and yields status **2**. | U, I |
| 3.10 | A parse that fails part-way must free everything already allocated. | U, M |

## 4. External command execution

| # | Requirement | Verified |
| --- | --- | --- |
| 4.1 | Execute an external command by `fork()` + `execvp()`, searching `PATH`. | I |
| 4.2 | Wait for a foreground command and adopt its exit code as the shell's status. | I |
| 4.3 | A command killed by signal `n` yields status **128 + n**. | I, P |
| 4.4 | A command that cannot be found yields **127**; one found but not executable yields **126**. | I |
| 4.5 | A failure of the **shell's own machinery** (`fork`, `waitpid`, a redirection, a built-in) yields **1** — never 126 or 127, which describe the command. | U, I |
| 4.6 | A failed `execvp` in the child must report the reason and `_exit()` without running shell code or flushing a duplicated buffer. | U, I |
| 4.7 | No zombie processes may remain after any command completes. | I, P |

## 5. Built-in commands

| # | Requirement | Verified |
| --- | --- | --- |
| 5.1 | `cd [dir]` changes the shell's own working directory; with no argument it uses `$HOME`. | U, I |
| 5.2 | `pwd` prints the shell's working directory, and reports an error rather than lying if it cannot be determined. | U, I |
| 5.3 | `help` lists every built-in and the supported syntax. | U, I |
| 5.4 | `exit [n]` leaves with `n & 255`; with no argument, the last status. A non-numeric or out-of-range argument is an error with status **2** and does **not** exit. | U, I |
| 5.5 | `jobs` lists tracked jobs with id, state, and the command text. | I, P |
| 5.6 | `fg [%n]` / `bg [%n]` resume a job in the foreground / background. | I, P |
| 5.7 | A built-in run **on its own in the foreground** executes in the shell process, so `cd` and `exit` take effect. | U, I |
| 5.8 | A built-in used as a **pipeline stage** or **backgrounded** runs in a child, so it cannot change or end the shell. Its output must still be flushed before the child exits. | I |
| 5.9 | A built-in's redirections apply to it and must be undone afterwards, leaving the shell's descriptors as they were. | U, I, M |

## 6. I/O redirection

| # | Requirement | Verified |
| --- | --- | --- |
| 6.1 | `< file` connects standard input to `file`, which must exist. | I |
| 6.2 | `> file` connects standard output to `file`, creating or truncating it. | I |
| 6.3 | `>> file` connects standard output to `file`, creating or appending. | I |
| 6.4 | Redirections are performed with `open`/`dup2`/`close` in the **child**, after `fork` and before `exec`. | I, U |
| 6.5 | Created files obey the process umask (mode `0666 & ~umask`). | I |
| 6.6 | At most one input and one output redirection per stage; a second of either is a syntax error. | U, I |
| 6.7 | Redirections may appear in any order and anywhere after the command word. | U, I |
| 6.8 | A redirection target is subject to the quoting rules, so a path may contain spaces. | U, I |
| 6.9 | A failed redirection must not run the command, must report the reason, and yields **1**. | I |
| 6.10 | An explicit redirection on a pipeline stage **overrides** that stage's pipe endpoint. | I |
| 6.11 | No descriptor may leak: the shell's open-descriptor set must be unchanged after every command. | U, I, M |
| 6.12 | **OUT:** stderr redirection (`2>`, `>&`), heredocs (`<<`), and descriptor duplication. |

## 7. Pipelines

| # | Requirement | Verified |
| --- | --- | --- |
| 7.1 | `a \| b \| c` runs every stage **concurrently**, connected by `pipe()`. | I |
| 7.2 | Each stage carries its own arguments and its own redirections. | I |
| 7.3 | The pipeline's status is the **last stage's** status. | I |
| 7.4 | Every pipe descriptor must be closed in every process that does not need it, so a reader sees EOF. | I, M |
| 7.5 | The shell must wait for **all** stages, leaving no zombies. | I |
| 7.6 | An empty stage (`a \|\| b`, `a \|`, `\| a`) is a syntax error with status **2**. | U, I |
| 7.7 | Pipelines of substantial length (hundreds of stages) must work or fail cleanly, never corrupt state. | U |
| 7.8 | **OUT:** `pipefail`. The last-stage rule is the whole contract. |

## 8. Signals and job control

| # | Requirement | Verified |
| --- | --- | --- |
| 8.1 | Every job — a single command or a whole pipeline — runs in **its own process group**, set by both the parent and the child to avoid the race. | P, I |
| 8.2 | A foreground job owns the terminal (`tcsetpgrp`) while it runs, and the terminal returns to the shell afterwards, on every exit path. | P |
| 8.3 | A stage must not begin executing before the terminal has been arranged, so a signal cannot be delivered to the wrong group. | P |
| 8.4 | The interactive shell **ignores** SIGINT, SIGQUIT, SIGTSTP, SIGTTIN and SIGTTOU, so it can never be interrupted or stopped by its own terminal. | P |
| 8.5 | Children **reset** every signal disposition to default before `exec`. | P, I |
| 8.6 | Ctrl+C and Ctrl+Z reach the **whole foreground process group**, so every stage of a pipeline receives them. | P |
| 8.7 | The SIGCHLD handler must do nothing but set a `volatile sig_atomic_t` flag; no non-async-signal-safe call may run in a handler. | review, M |
| 8.8 | The handler must **not** use `SA_RESTART`, so a blocked read is interrupted and the shell can react. | P |
| 8.9 | The job table must be mutated with SIGCHLD blocked. | review |
| 8.10 | `&` runs the whole pipeline in the background, prints `[n] pgid`, and reports status **0**. | I |
| 8.11 | `&` must be the last token on the line; `a & b` is a syntax error, since msh has no command lists. | U, I |
| 8.12 | A finished background job is reported **before the next prompt**, not asynchronously mid-line. | I, P |
| 8.13 | A foreground job stopped by Ctrl+Z yields status `128 + SIGTSTP` (**148**) and is recorded as stopped. | P |
| 8.14 | `fg`/`bg` with no argument pick the most recently stopped job, else the most recent active one; an invalid or finished id is reported without disturbing the table. | I, P |
| 8.15 | `exit` **refuses once** while jobs are running and says so; a second consecutive `exit` leaves anyway. Any command in between clears the warning. | I |
| 8.16 | **Without a terminal** (a pipe, a script, CI) msh performs no terminal control at all and lets terminal-generated signals keep their default behaviour, so it dies like any other program in a pipeline. Jobs are still tracked. | I |
| 8.17 | If the shell cannot obtain the terminal at startup, it must **disable job control and carry on**, never spin and never stop itself with no way to be continued. | P |
| 8.18 | **OUT:** `kill`, `wait`, `disown`, job specs beyond `%n`/`n`, and `SIGHUP`-on-exit semantics. |

## 9. Quality gates

| # | Requirement |
| --- | --- |
| 9.1 | A clean build must produce **zero warnings** under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings -Wpointer-arith -Wcast-qual -Wundef -Wvla`, on GCC **and** Clang. |
| 9.2 | `make test` (unit + integration) must pass with no failures and no skips. |
| 9.3 | `make test-pty` must exercise terminal behaviour through a **real pty** (`forkpty`), not a simulated one. |
| 9.4 | `make asan` (ASan + UBSan, `detect_leaks=1`) must report nothing. |
| 9.5 | `make valgrind` must be clean **with no suppression file**, so no msh defect can be hidden by a suppression. |
| 9.6 | Every msh invocation in a test must run under a **bounded timeout**, and a timeout must be reported as a failure, so a deadlock fails the suite instead of hanging it. |
| 9.7 | A test that cannot reliably induce the condition it claims to check must be documented as unverified rather than reported as passing. |
| 9.8 | The repository must contain no generated binaries, no secrets, and no paths from a developer's own machine. |

## 10. Non-requirements

Stated so their absence is a decision, not an oversight:

- Logical operators and command lists: `&&`, `||`, `;`
- Variable expansion (`$VAR`, `$?`), command substitution, arithmetic
- Globbing (`*`, `?`, `[...]`), brace and tilde expansion
- Comments (`#`)
- Shell functions, aliases, `source`, scripts as arguments
- Command history, line editing, tab completion
- Environment manipulation (`export`, `env` as a built-in)
- `cd -` / `$OLDPWD`, `pwd -L` / `-P`
- Multi-line continuation, interactive line joining

---

## Deliberate divergences from Bash

Where msh differs from Bash in a way a reader could mistake for a bug:

| Behaviour | Bash | msh | Why |
| --- | --- | --- | --- |
| `a & b` | runs `a` in background, then `b` | **syntax error (2)** | msh has no command lists; accepting it would half-run a line |
| `a \|\| b` | logical OR | **syntax error**: an empty pipeline stage | `\|\|` is two pipes with nothing between them |
| `> out` with no command | creates/truncates `out` | **syntax error (2)** | a redirection with nothing to redirect is more likely a typo than an intent |
| `cmd > a < b` ordering | applies left to right | input is opened before output | documented ordering; only observable when both targets are the same file |
| `;` | command separator | **ordinary argument text** | no command lists |
| `pipefail` | available via `set -o` | not implemented | the last-stage rule is the whole contract |
| Exit status of a shell-internal failure | varies | always **1** | 126/127 are reserved for statements about the command |

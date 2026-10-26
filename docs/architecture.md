# MSH — Minimal Shell: architecture

This document describes what is actually implemented. It is updated as each
milestone lands, not ahead of it.

## Provenance

An earlier milestone-1 repository existed on the author's machine but its
working tree and Git history could not be located and were **not recovered**.
The source was reconstructed and this repository's history begins at its own
root commit. No earlier commits are preserved here, and none have been
fabricated to look as though they were.

## Current scope

A read-eval-print loop that parses one line into a pipeline of commands with
quoting, redirection and an optional background marker; runs a lone built-in
in the shell itself; runs everything else as a job in its own process group;
and, at a terminal, manages terminal ownership, signals, and stopped and
background jobs.

## Module boundaries

```
main.c  ->  shell.c  ->  lineread.c
                     ->  parser.c
                     ->  builtins.c  ->  jobs.c
                     ->  redirect.c
                     ->  signals.c
                     ->  jobs.c      ->  executor.c
                     ->  executor.c  ->  redirect.c, signals.c
```

| Module | Responsibility | Deliberately does not |
| --- | --- | --- |
| `main.c` | Entry, argument rejection, owns `msh_shell` | Contain loop logic |
| `shell.c` | State, the REPL, dispatch, job/foreground policy | Parse, or manage descriptors |
| `lineread.c` | A byte stream into one heap line | Know about shells |
| `parser.c` | A line into a pipeline of commands | Know about processes |
| `builtins.c` | cd, pwd, help, exit, jobs, fg, bg | Fork, parse, redirect |
| `redirect.c` | open/dup2/close, child side and shell side | Decide what to run |
| `executor.c` | fork/pipe/execvp/waitpid, process groups | Parse, or own a terminal |
| `jobs.c` | The job table, process groups, terminal ownership | Parse, or exec |
| `signals.c` | The signal policy, and only that | Anything a handler cannot do |

`msh_dispatch` is still the single seam. It now chooses between two cases: a
lone, foreground built-in runs in this process; everything else becomes a
**job**.

**Why `signals.c` is separate**: the correctness argument for the handler is
"it does one thing and that thing is async-signal-safe". That is much easier
to keep true when the handler lives alone in a file whose header states the
rule, than when it sits beside code that is free to call `printf`.

**Why `executor.c` does not know about jobs**: it starts processes and
reports pids. Whether those are waited for (a foreground job) or recorded and
left running (a background one) is a policy decision, and policy lives in
`shell.c`. That split is what let the pipeline code from the previous
milestone be reused unchanged.

## The job model

A **job is one whole pipeline**, however many stages it has. That is the unit
the user names (`fg %2`), the unit that shares a process group, and the unit a
terminal signal reaches.

```c
msh_job {
    int       id;          /* stable, user-facing                */
    pid_t     pgid;        /* every stage is in this group       */
    char     *command;     /* the line, for `jobs`               */
    msh_proc *procs;       /* one entry per stage, with state    */
    size_t    nprocs;
    int       background, notified, stop_notified;
    struct termios modes;  /* saved when it stopped              */
};
```

**Every process is tracked, and the job's state is derived from all of them**
— never inferred from one pid. A pipeline whose first stage has exited while
its last is stopped is a *stopped* job, not a finished one, and only a
per-process view gets that right. The job's exit status is the **last stage's**,
preserving the pipeline rule from the previous milestone.

Completed jobs are reported once, just before the next prompt, and then
dropped. A foreground job is never announced — the user watched it run.

## Process groups and the terminal

At startup, when interactive, the shell:

1. waits until it really is the foreground job, stopping itself with SIGTTIN
   if it is not — the correct behaviour for a shell started in the background;
2. puts itself in a process group of its own;
3. takes the terminal with `tcsetpgrp`;
4. saves its terminal modes.

For each job, **both the parent and the child call `setpgid`** with the same
arguments. This is deliberate duplication, not redundancy: if only the child
did it, the shell could hand the terminal to a group the child had not joined
yet; if only the parent did it, the child could exec and miss a signal first.
One call wins and the other fails harmlessly, so the group exists before
either side depends on it. In the parent, `EACCES` (the child already exec'd)
and `ESRCH` (it already exited) are expected and distinguished from real
failures.

A foreground job is given the terminal, waited for, and the terminal is taken
back **on every exit path** — normal completion, signal death, and a stop.
`tcsetpgrp` is called with SIGTTOU blocked, because a process not in the
foreground group would otherwise be stopped by its own attempt to take it.

When a job stops, **its** terminal modes are saved into the job and the
shell's are restored; `fg` puts the job's modes back before resuming it. A
program that changed the terminal — an editor, a pager — therefore finds it
as it left it, and the shell is never left with someone else's settings.

When not interactive, none of this happens. There is no `tcsetpgrp`, no
process group of our own, and the terminal-generated signals keep their
defaults so msh dies like any other program in a pipeline. This is tested:
CI and piped use are the common case, not an afterthought.

## Signal policy

| Signal | Shell | Child before exec |
| --- | --- | --- |
| SIGINT, SIGQUIT, SIGTSTP | ignored (interactive only) | SIG_DFL |
| SIGTTIN, SIGTTOU | ignored (interactive only) | SIG_DFL |
| SIGCHLD | handled, flag only | SIG_DFL |
| SIGPIPE | default | SIG_DFL |

**The shell ignores rather than catches** the terminal signals. A caught
signal would still interrupt system calls and would have to be forwarded by
hand; ignoring leaves the terminal driver to deliver Ctrl+C and Ctrl+Z to the
foreground process group, which *is* the job. The shell is simply not in that
group.

**Every child restores SIG_DFL and clears the signal mask before exec.** A
child that inherited SIG_IGN could not be interrupted at all — the classic way
a shell leaves unkillable processes behind. A blocked signal survives exec, so
the mask is reset too.

### The SIGCHLD handler does one thing

```c
static void sigchld_handler(int signo) { (void)signo; sigchld_pending = 1; }
```

No `waitpid`, no stdio, no allocation, no job-table mutation — none of which
is safe to interleave with the main loop. The shell drains child status
**synchronously, with SIGCHLD blocked**, at points of its own choosing: before
each prompt, at the start of `jobs`/`fg`/`bg`, and while waiting for a
foreground job. The table is therefore only ever mutated by the main loop.

The handler is installed **without `SA_RESTART`**, which matters: a `read()`
blocked at the prompt is interrupted, so the shell notices a finished
background job while it is waiting for input rather than only after the user
presses return. `msh_read_line` reports that as `MSH_READ_INTERRUPTED` when
nothing has been typed yet, and the REPL collects the job and reads again
**without reprinting the prompt**, so a notification appears above the line
being typed instead of leaving a second prompt behind. A signal that arrives
mid-line is retried inside the reader instead, because the characters already
entered would otherwise be lost.

Adding a status to the table and reaping it are both done with SIGCHLD
blocked, so a fast command that exits before its job is recorded cannot be
lost: the drain simply happens after the entry exists.

## Status semantics

| Outcome | Status |
| --- | --- |
| Normal exit | `WEXITSTATUS` |
| Killed by a signal | `128 + signal` |
| Pipeline | the last stage's status |
| Foreground job stopped | `128 + SIGTSTP` (148) |
| Background launch | 0 |
| No such command / not executable | 127 / 126 |
| Syntax error, bad `exit` argument | 2 |
| Redirection, built-in or shell failure | 1 |
| Blank input | previous status, unchanged |

`exit` with jobs running refuses once, which is itself a failed command and
sets the status to 1; a following bare `exit` therefore carries 1, exactly as
`exit` always carries the last status. `exit 3` after the warning still exits
3. Both are tested.

## Memory and descriptor ownership

- `msh_parse` owns every stage, string and path; on any non-OK outcome it owns
  nothing and leaves the value zeroed.
- The job table owns each job's `command` and `procs`; `msh_jobctl_shutdown`
  releases them, and `main` calls it. Completed jobs are released as they are
  dropped.
- `run_as_job` owns the pid array for the duration of the call.
- If the table cannot take a job, the processes still exist, so they are
  waited for rather than abandoned.
- The executor closes every pipe end in both parent and children; the parent
  holds none while waiting.
- Saved terminal descriptors use `F_DUPFD_CLOEXEC` at fd 10 or above.

## Verification status

Verified on native Linux on 2026-10-26. Every row was produced by running the
command and reading its output.

Environment: Ubuntu 24.04.5 LTS (WSL2), kernel 6.18.54.1-microsoft-standard-WSL2,
gcc 13.3.0, GNU Make 4.3, GNU gdb 15.1, valgrind-3.22.0, git 2.43.0.

| Check | Result |
| --- | --- |
| Clean build, full warning set **+ `-Werror`**, GCC | **PASS** — 0 warnings, 0 errors |
| Unit — `test_parser` (43) | **PASS** |
| Unit — `test_builtins` (18) | **PASS** |
| Unit — `test_lineread` (9) | **PASS** |
| Unit — `test_redirect` (6) | **PASS** |
| Unit — `test_shell` (5) | **PASS** |
| Unit — `test_executor` (3) | **PASS** |
| Integration — `test_jobs.sh` (49) | **PASS** |
| Integration — `test_pipeline.sh` (66) | **PASS** |
| Integration — `test_redirect.sh` (63) | **PASS** |
| Integration — `test_builtins.sh` (53) | **PASS** |
| Integration — `test_quoting.sh` (49) | **PASS** |
| Integration — `test_exec.sh` (25) | **PASS** |
| Integration — `test_repl.sh` (12) | **PASS** |
| `make test-pty` — `test_pty` (3) | **PASS** |
| `make test-pty` — `test_pty_jobs` (30) | **PASS** |
| `test_pty_jobs` **run 20× back to back** | **PASS — 20 of 20, 600/600 checks, 0 failures, 0 timeouts** |
| `make valgrind` (80) | **PASS** — 0 errors, no suppressions |
| `make asan` — ASan + UBSan, full suite | **PASS** — no diagnostics |
| Zombies after the suite | **none** |
| Generated files ignored | **PASS** — `git check-ignore` |
| Secret scan / machine paths / committed binaries | **PASS** — none found |
| Valgrind suppression file absent | **PASS** |
| Clean build + full suite, **Clang** | **NOT VERIFIED LOCALLY** — see below |
| CI workflow (GCC **and** Clang, ASan/UBSan, Valgrind) | runs on GitHub Actions; the Actions tab carries the live result |

Totals: **401 checks in `make test`** (84 unit, 317 integration), plus 33 in
`make test-pty` and 80 in `make valgrind`. **No test reported a skip.**

**Clang was not verified locally.** Clang is not installed in the development
environment and installing it needs privileges that were not available, so no
local Clang result is claimed. The CI workflow builds with Clang and runs the
whole suite against it; that job is the evidence, not an assertion made here.

Unit counts, per binary: `test_parser` 43, `test_builtins` 18,
`test_lineread` 9, `test_redirect` 6, `test_shell` 5, `test_executor` 3.
Integration counts: `test_redirect.sh` 63, `test_pipeline.sh` 66,
`test_builtins.sh` 53, `test_quoting.sh` 49, `test_jobs.sh` 49,
`test_exec.sh` 25, `test_repl.sh` 12.

### Why the interactive tests use a pty

Ctrl+C and Ctrl+Z are not characters a program reads. The terminal line
discipline turns them into SIGINT and SIGTSTP delivered to the terminal's
*foreground process group*. A shell that never called `tcsetpgrp` would pass
any piped test and still be unusable, so `test_pty_jobs.c` drives msh through
a real pseudo-terminal and asserts terminal ownership directly with
`tcgetpgrp` before, during and after every transition.

**A lesson the suite now encodes.** The first version of that test
synchronised by waiting for a marker the command printed — but the terminal
*echoes what the test types*, so the marker matched the echo of the command
being typed and the test raced ahead of the shell, producing five confusing
failures that looked like job-control bugs. The markers are now written split
(`REA''DY1`) and rejoined by `sh`, so only the running program's output can
match. Every read has a bounded timeout and a timeout is a failure.

### Failure paths induced for real, not assumed

- Ctrl+C on a foreground command and on a foreground pipeline.
- Ctrl+Z, then `jobs`, `bg`, `fg`, then Ctrl+C on the resumed job.
- Terminal ownership checked after each of those transitions.
- A pipeline stage proven to be in a group of its own, not the shell's.
- `fg`/`bg` with no jobs, a missing job, a non-numeric spec, too many
  arguments, and `bg` on an already-running job.
- `exit` refused once with jobs running, then honoured.
- Backgrounded `cd` and `exit` proven not to affect the shell.
- Every malformed `&` form, with nothing of the line executed.
- `fork()` failure via `RLIMIT_NPROC`; `getcwd()` failure by removing the cwd.

### Controls run against the test suite itself

- The integration harness was checked by corrupting an expected status.
- The ASan build is confirmed instrumented; the Valgrind build is not.
- The descriptor check was confirmed able to fire on a deliberate leak.
- Zombie checks fail if `ps` produced no output rather than passing
  vacuously; descriptor counts are taken from inside msh.
- Argument boundaries are measured with helper scripts, because `echo` joins
  its arguments.

### Valgrind settings

No suppression file. `--errors-for-leak-kinds=definite,indirect` rather than
`all`, because "still reachable" is not a leak and the forked child
deliberately holds inherited allocations until `_exit()`.
`--trace-children=no`, so memcheck audits the shell's own bookkeeping — the
job table, the pid arrays, the command strings — which is what this milestone
adds.

The intermittent `4 open (3 std)` noted since milestone 7 has now been seen
four times in roughly forty runs, always inside `make valgrind`, never
reproducible. The attribution-based check passed every time.

### Tests changed rather than added

- `test_parser.c`: `test_other_operators_are_plain_text` asserted that `&` and
  `&&` were ordinary text. This milestone makes `&` an operator, so that case
  now covers `;` alone and six background tests were added beside it.
- `test_shell.c`: dispatching now records a job, and the job table is owned by
  the shell, so each test calls `msh_shell_shutdown`. **ASan found this** —
  the leak was in the test, not the shell, which frees the table in `main`.
- `help` text was corrected: it listed job control under "not implemented".

### Real defects this project's testing exposed

All of these were found by the pty suite, and all are fixed:

1. **The startup loop could hang the shell outright.** The textbook sequence
   is "loop until we are the foreground group, sending ourselves SIGTTIN".
   msh ignored SIGTTIN *before* that loop, so the signal did nothing and the
   loop spun for ever; moving the ignore after it merely turned the spin into
   a permanent self-stop that nothing would ever continue. The shell now
   **claims** the terminal instead, and switches job control off if it
   cannot, because a wedged shell with no way to continue it is a far worse
   failure than the one that loop guards against.

2. **A job could run before it owned the terminal.** The stages are forked
   before the parent calls `tcsetpgrp`, so a stage could print output — and
   receive a Ctrl+C — while the terminal still belonged to the shell, which
   ignores it. The signal was simply lost. Stages now wait at a **start
   barrier**: a pipe they block on until the shell has arranged the terminal,
   released in `msh_job_start`. In a focused reproducer this took Ctrl+C from
   intermittent to 12 of 12.

3. **A test that raced the shell.** The pty harness synchronised on a marker
   the command printed — but the terminal echoes what the test types, so the
   marker matched the echo of the command being typed and the test ran ahead
   of msh. Markers are now split (`REA''DY1`) and rejoined by `sh`, so only
   the program's own output can match.

### The flaky pty suite, diagnosed and fixed

`test_pty_jobs` used to pass only about half the time when run back to back.
It was treated as a release blocker and traced to the end. Two independent
causes were found; **both were in the test, not in the shell**, and both are
fixed. The suite stayed in `make test-pty` throughout — it was never moved
aside or skipped to make a gate look green.

**Cause 1 — a `fork` wedged in the kernel.** The harness started its test job
as `/bin/sh -c "echo READY; sleep 30"`. dash forks a second process for the
`sleep`, and on this kernel that fork intermittently never returns. Catching
a hung run in the act and reading `/proc` showed it exactly:

| pid | what | state | `wchan` | pending signals |
| --- | --- | --- | --- | --- |
| 489 | `/bin/sh`, the job's group leader | **D** (uninterruptible) | **`kernel_clone`** | SIGTSTP **+** SIGCHLD, undeliverable |
| 490 | the process it forked | `T` (stopped) | `do_signal_stop` | SIGTSTP — stopped correctly |
| 481 | `msh` | `S` | `do_wait` | `SigIgn` = INT/QUIT/TSTP/TTIN/TTOU |

Read that top row carefully, because it settles the question. The terminal
*did* generate SIGTSTP and the kernel *did* queue it to the whole process
group — it is sitting in the leader's `ShdPnd`. The child took it and stopped.
The leader could not, because it was in **uninterruptible** sleep inside
`fork()`. A process in state D cannot be stopped, continued, or killed, not
even by SIGKILL. msh was in `do_wait` with precisely the right signal
dispositions, waiting for a job member that was never coming back.

So msh was correct and no shell could have done better. The fix was to stop
provoking the kernel: the harness now runs `/bin/sh -c "echo READY; exec
sleep 30"`, so dash replaces itself instead of forking and the window
disappears. This removed every timeout from the suite.

A **control experiment** had already pointed the same way. A shell-agnostic
pty program sent Ctrl+C to an arbitrary shell and checked whether the prompt
returned: `/bin/bash` — a production job-control shell — succeeded in **9 of
12** runs under this harness, against **11 of 12** for msh. When the
reference implementation is the *less* reliable of the two, the harness and
the environment are the thing to examine, not the shell.

**Cause 2 — a synchronisation defect in the test.** With the timeouts gone,
one check still failed about one run in four: *"a background job never takes
the terminal"*. It sampled `tcgetpgrp()` as soon as `/bin/echo alive5`
printed its output — but `/bin/echo` is itself an external **foreground** job
and owns the terminal while it prints. Seeing its output proves only that it
ran, not that the shell has reaped it and taken the terminal back. msh prints
its prompt only *after* reclaiming the terminal, so the prompt is the
synchronisation point; every stable ownership check in the file already waited
for it, and these two did not. They now do.

**What was deliberately not done:** no timeout was raised to paper over
either cause, no sleep was inserted to hope the race went the right way, and
the suite was not taken out of `make test-pty`. A bounded retry of a control
key remains — the bash control shows this environment does drop an INTR byte
occasionally — but it is capped at three attempts and still fails the check if
the key never has any effect.

**The honest residual risk.** Cause 1 was removed by not triggering it, not by
making it survivable, because it is not survivable: nothing in user space can
rescue a process stuck in D state. A different test, or a user's own command
that forks under load, could hit the same WSL2 stall. On a mainline kernel
this is not expected to arise, and CI runs the same suite on ordinary Linux.

### Not covered, and honestly so

- **A `setpgid` or `tcsetpgrp` failure** is not induced. Both are written to
  distinguish the benign races (`EACCES`, `ESRCH`) from real failures, but
  provoking a genuine failure needs a contrived session setup that would test
  the harness more than the shell. Reviewed, not test-verified.
- **A mid-pipeline `pipe()` or `fork()` failure** remains uninduced, for the
  reason given in the previous milestone.
- **`dup2()` failure**, **parser allocation failure** and **`waitpid()`
  failure** remain uninduced, as recorded earlier.
- **SIGQUIT (Ctrl+\\)** is given the same disposition as SIGINT and is covered
  by the same code path, but there is no separate pty test for it.
- **A job stopped by something other than Ctrl+Z** (a `kill -STOP` from
  outside) is handled by the same `WUNTRACED` path but is not separately
  tested.
- The job table grows and is compacted but never shrinks its allocation;
  with no `kill` built-in a long-lived shell could accumulate capacity. Bounded
  by the number of concurrent jobs, not by jobs ever run.

## Known limitations

- No command lists or logical operators; `;` is argument text and `&&` is a
  syntax error.
- No `pipefail`, by documented choice.
- stderr cannot be redirected; no heredocs.
- No expansion of any kind, so `$?` cannot be printed.
- No `kill`, `disown` or `wait` built-ins, and job specs are only `%n`.
- No `cd -`/`$OLDPWD`, and no `pwd -L`/`-P`.

## Where this stands

The roadmap's finishing work — automated tests, sanitizers, CI and
documentation — was done continuously rather than left to the end, so what
closed this project out was the final QA pass: diagnosing the flaky pty suite
to its root cause, building with warnings as errors, running the suite under
both sanitizers and Valgrind with no suppressions, and publishing with CI that
is allowed to go red.

What remains genuinely open:

- The unverified failure paths listed above, each needing a harness that
  would mostly test itself.
- Clang, verified in CI rather than locally.
- The WSL2 `fork` stall described earlier is avoided rather than survivable,
  because nothing in user space can rescue a process stuck in state D.

The natural feature follow-ups, in order of how much they would be used:
variable expansion (`$?`, `$HOME`) and with it a `kill` built-in; then command
lists (`;`, `&&`, `||`), which the parser is already shaped for — a list of
pipelines rather than a list of commands.

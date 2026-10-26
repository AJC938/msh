/*
 * executor - run external programs with fork(), execvp() and waitpid(), and
 * run pipelines with pipe() on top of that.
 *
 * This module owns the process lifecycle and nothing else: it does not parse,
 * and it does not touch shell state. The caller decides what to do with the
 * status it returns.
 */
#ifndef MSH_EXECUTOR_H
#define MSH_EXECUTOR_H

#include "parser.h"

#include <stddef.h>
#include <sys/types.h>

/*
 * Status values.
 *
 * 126 and 127 are conventional, and both describe THE COMMAND: 127 means no
 * such command, 126 means it was found but could not be executed. They are
 * deliberately not used for failures of the shell itself, because a caller
 * testing for 126 is asking "is that file executable?" — answering yes to
 * that when the real problem was that the machine could not fork, or that a
 * pipe could not be created, would be a lie. No standard assigns a status to
 * a fork(), pipe(), waitpid() or redirection failure, so those use a generic
 * failure value instead.
 */
#define MSH_STATUS_INTERNAL_ERROR 1 /* the shell could not run the line      */
#define MSH_STATUS_SYNTAX_ERROR   2 /* malformed quoting, redirect or pipe   */
#define MSH_STATUS_CANNOT_EXEC  126 /* found but could not be executed       */
#define MSH_STATUS_NOT_FOUND    127 /* exec failed: no such command          */
#define MSH_STATUS_SIGNAL_BASE  128 /* add the signal number for a kill      */

/*
 * Execute `cmd` as a single external program and wait for it.
 *
 * `cmd` is borrowed. PATH lookup and the environment come from execvp(), so
 * the child inherits the shell's environment. Any redirections on `cmd` are
 * installed in the child, so the shell's own descriptors are untouched.
 *
 * Returns the integer status to record as the shell's last status:
 *   - the child's own exit code when it exited normally;
 *   - 128 + signal number when a signal killed it;
 *   - 127 when the command does not exist (ENOENT, ENOTDIR);
 *   - 126 when it exists but could not be executed;
 *   - 1 when the shell itself failed.
 *
 * The shell always remains in the parent process: the child either execs or
 * leaves via _exit(), so it can never return into the REPL.
 */
int msh_execute(const msh_command *cmd);

/*
 * Run a built-in inside a pipeline stage's own child process.
 *
 * Supplied by the caller so that this module does not have to know about
 * shell state or the built-in table. Returns 1 when `argv` named a built-in,
 * having put its result in `*status`; 0 when it did not, in which case the
 * stage is executed as an external program.
 *
 * It is called only in a child, so anything it changes -- the working
 * directory, a request to exit -- affects that child alone.
 */
typedef int (*msh_stage_builtin_fn)(void *ctx, char *const *argv, int *status);

/*
 * Execute a whole pipeline and wait for every stage.
 *
 * All stages are created first and run concurrently; they are never run one
 * after another, which would deadlock as soon as a stage produced more than
 * one pipe buffer of output.
 *
 * Each stage's standard output is connected to the next stage's standard
 * input. A stage's own redirections are applied *after* its pipe endpoints,
 * so an explicit `>` overrides the pipe: in `producer > out.txt | consumer`
 * the producer writes to the file and the consumer sees end of file.
 *
 * `run_builtin` may be NULL, in which case every stage is executed as an
 * external program.
 *
 * Returns the status of the LAST stage, which is this shell's documented
 * convention; there is no pipefail. A setup failure -- pipe(), fork(), or an
 * allocation -- returns MSH_STATUS_INTERNAL_ERROR after closing every
 * descriptor it owns and reaping every child it had already started.
 *
 * The shell's own standard input and output are never altered.
 */
int msh_execute_pipeline(const msh_pipeline *pipeline,
                         msh_stage_builtin_fn run_builtin, void *ctx);

/*
 * Start every stage and return immediately, without waiting for any of
 * them. This is the half of pipeline execution that job control needs:
 * the caller decides whether to wait (a foreground job) or to record the
 * processes and carry on (a background job).
 *
 * `pids` must have room for `nstages` entries and receives one pid per
 * stage, in order. With `job_control` set, every stage is placed in a new
 * process group whose id is returned through `pgid_out`, so the group can
 * be signalled and given the terminal as a unit.
 *
 * With `job_control`, the stages are held at a START BARRIER: each one has
 * joined the process group but will not exec until the caller releases it
 * through `*start_fd_out`. Without that barrier a stage could run -- and
 * produce output, and be sent a terminal signal -- before the shell had
 * handed the terminal to its group, and that signal would go to the shell
 * instead and be ignored. The caller closes the descriptor once the
 * terminal is arranged; -1 means there is no barrier to release.
 *
 * Returns 0 with every stage started, or -1 after reporting the reason,
 * having closed every descriptor it owned and reaped any stage it had
 * already started. There is never a half-started pipeline left behind.
 */
int msh_spawn_stages(const msh_command *stages, size_t nstages,
                     msh_stage_builtin_fn run_builtin, void *ctx,
                     int job_control, pid_t *pids, pid_t *pgid_out,
                     int *start_fd_out);

/*
 * Translate a waitpid() status word into the shell's integer status.
 *
 * Separated out and exported so the mapping — in particular the 128 + signal
 * rule — can be unit-tested directly, without having to kill a real child to
 * observe it.
 *
 * Returns MSH_STATUS_INTERNAL_ERROR for a status word that is neither a
 * normal exit nor a signal death.
 */
int msh_status_from_wait(int wait_status);

#endif /* MSH_EXECUTOR_H */

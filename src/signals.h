/*
 * signals - the shell's signal policy.
 *
 * Two dispositions exist, and the distinction is the whole point of this
 * module:
 *
 *   - the SHELL ignores the terminal-generated signals, so Ctrl+C and Ctrl+Z
 *     reach the foreground job instead of killing or stopping the shell;
 *   - a CHILD restores every one of them to SIG_DFL before exec, so the
 *     program it becomes behaves normally. A child that inherited SIG_IGN
 *     could not be interrupted at all, which is the classic way a shell
 *     leaves unkillable processes behind.
 *
 * SIGCHLD is handled rather than ignored, and the handler does as little as
 * possible: it sets one `volatile sig_atomic_t` flag. No waitpid(), no stdio,
 * no allocation, no job-table mutation -- none of which is async-signal-safe
 * to mix with the main loop. The shell drains child status synchronously,
 * with SIGCHLD blocked, at a point of its own choosing.
 *
 * The handler is installed WITHOUT SA_RESTART on purpose. A blocked read() at
 * the prompt is interrupted, which is what lets the shell notice a finished
 * background job while it is waiting for input rather than only after the
 * user types a line.
 */
#ifndef MSH_SIGNALS_H
#define MSH_SIGNALS_H

#include <signal.h>

/*
 * Install the SIGCHLD handler. Done whether or not the shell is
 * interactive, because jobs are tracked either way.
 */
void msh_signals_init_shell(int interactive);

/*
 * Ignore the terminal-generated signals, so Ctrl+C and Ctrl+Z reach the
 * foreground job rather than the shell.
 *
 * Called by job control AFTER it has acquired the terminal, and never
 * before. The acquisition loop stops a shell that was started in the
 * background by sending itself SIGTTIN, which only works while SIGTTIN
 * still has its default disposition -- ignoring it first turns that loop
 * into a spin that never ends.
 */
void msh_signals_ignore_job_control(void);

/*
 * Restore SIG_DFL for every signal this shell touches, and unblock them.
 * Called in a child after fork() and before exec, so the new program starts
 * with a clean slate.
 */
void msh_signals_reset_child(void);

/* True once since the last call: test and clear the SIGCHLD flag. */
int msh_signals_take_sigchld(void);

/*
 * Block or restore SIGCHLD around a critical section. The job table is
 * mutated with SIGCHLD blocked so the flag cannot be set and acted on
 * half-way through an update.
 */
void msh_signals_block_sigchld(sigset_t *saved);
void msh_signals_restore_mask(const sigset_t *saved);

#endif /* MSH_SIGNALS_H */

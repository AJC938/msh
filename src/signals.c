#include "signals.h"

#include <stddef.h>

/*
 * Set by the handler, read and cleared by the main loop. `volatile
 * sig_atomic_t` is the only type the standard lets a handler and the rest of
 * the program share like this.
 */
static volatile sig_atomic_t sigchld_pending;

/* The terminal-generated signals a shell must not be killed or stopped by. */
static const int job_control_signals[] = {
    SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU
};

#define JOB_CONTROL_SIGNAL_COUNT \
    (sizeof job_control_signals / sizeof job_control_signals[0])

/*
 * The whole handler. Setting a flag is async-signal-safe; almost nothing else
 * that would be useful here is.
 */
static void sigchld_handler(int signo)
{
    (void)signo;
    sigchld_pending = 1;
}

void msh_signals_ignore_job_control(void)
{
    struct sigaction sa;
    size_t i;

    /*
     * Ignore, do not catch. A caught signal would still interrupt system
     * calls and would have to be forwarded by hand; ignoring leaves the
     * terminal driver to deliver Ctrl+C and Ctrl+Z to the foreground
     * process group, which is exactly the job, not the shell.
     */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (i = 0; i < JOB_CONTROL_SIGNAL_COUNT; i++)
        (void)sigaction(job_control_signals[i], &sa, NULL);
}

void msh_signals_init_shell(int interactive)
{
    struct sigaction sa;

    (void)interactive;

    /*
     * No SA_RESTART: a read() blocked at the prompt is interrupted so the
     * shell can notice a finished job promptly instead of waiting for the
     * user to press return. The line reader is written to expect that.
     */
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGCHLD, &sa, NULL);
}

void msh_signals_reset_child(void)
{
    struct sigaction sa;
    sigset_t empty;
    size_t i;

    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    for (i = 0; i < JOB_CONTROL_SIGNAL_COUNT; i++)
        (void)sigaction(job_control_signals[i], &sa, NULL);
    (void)sigaction(SIGCHLD, &sa, NULL);
    (void)sigaction(SIGPIPE, &sa, NULL);

    /* A blocked signal survives exec, so the mask has to be cleared too. */
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
}

int msh_signals_take_sigchld(void)
{
    if (!sigchld_pending)
        return 0;
    sigchld_pending = 0;
    return 1;
}

void msh_signals_block_sigchld(sigset_t *saved)
{
    sigset_t block;

    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    (void)sigprocmask(SIG_BLOCK, &block, saved);
}

void msh_signals_restore_mask(const sigset_t *saved)
{
    (void)sigprocmask(SIG_SETMASK, saved, NULL);
}

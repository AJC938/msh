#include "executor.h"

#include "redirect.h"
#include "signals.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * write() everything, tolerating short writes and EINTR.
 *
 * Used instead of stdio on the child's failure paths. stdio there would
 * append to buffers inherited from the parent, and anything already pending
 * in them would then be emitted a second time by the child.
 */
static void write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR)
            continue;
        /* Nothing useful remains to be done about a failing stderr. */
        return;
    }
}

/*
 * Report a failed exec from inside the child.
 *
 * Everything here is chosen to be safe between fork() and _exit(): write()
 * rather than stdio, strerror_r() rather than strerror(), no malloc, and no
 * cleanup of the parent's allocations.
 */
static void report_exec_failure(const char *cmd, int err)
{
    char buf[256];
    const char *reason = buf;

    if (strerror_r(err, buf, sizeof buf) != 0)
        reason = "unknown error";

    write_all(STDERR_FILENO, "msh: ", 5);
    write_all(STDERR_FILENO, cmd, strlen(cmd));
    write_all(STDERR_FILENO, ": ", 2);
    write_all(STDERR_FILENO, reason, strlen(reason));
    write_all(STDERR_FILENO, "\n", 1);
}

/* close() retrying on EINTR, ignoring a descriptor that is not open. */
static void close_fd(int fd)
{
    if (fd < 0)
        return;
    while (close(fd) == -1) {
        if (errno != EINTR)
            return;
    }
}

/* dup2() retrying on EINTR. Returns 0, or -1 with errno set. */
static int dup2_fd(int oldfd, int newfd)
{
    while (dup2(oldfd, newfd) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int msh_status_from_wait(int wait_status)
{
    if (WIFEXITED(wait_status))
        return WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status))
        return MSH_STATUS_SIGNAL_BASE + WTERMSIG(wait_status);
    if (WIFSTOPPED(wait_status))
        return MSH_STATUS_SIGNAL_BASE + WSTOPSIG(wait_status);

    /* Neither exited, signalled nor stopped: nothing truthful to report. */
    return MSH_STATUS_INTERNAL_ERROR;
}

/*
 * The tail of a child process: restore signal dispositions, install the
 * stage's own redirections, run it as a built-in if it is one, otherwise
 * exec it. Never returns.
 */
static void run_stage_child(const msh_command *cmd,
                            msh_stage_builtin_fn run_builtin, void *ctx)
{
    int err;
    int status = 0;

    /*
     * The shell ignores the terminal signals; the program about to run here
     * must not inherit that, or it could never be interrupted.
     */
    msh_signals_reset_child();

    /*
     * Applied after any pipe wiring, so an explicit redirection overrides the
     * pipe endpoint rather than the other way round.
     */
    if (msh_redirect_apply_child(cmd) != 0)
        _exit(MSH_STATUS_INTERNAL_ERROR);

    /*
     * A built-in in a pipeline, or a backgrounded built-in, runs here in the
     * child. Its effects -- a changed directory, a request to exit -- cannot
     * reach the shell, which is what makes `cd /tmp | cat` and `exit &`
     * harmless.
     */
    if (run_builtin != NULL && run_builtin(ctx, cmd->argv, &status) != 0) {
        /*
         * _exit() does not flush stdio, and a built-in writes through it, so
         * its output would be discarded without this. The parent flushed
         * before forking, so nothing inherited can be duplicated here.
         */
        fflush(NULL);
        _exit(status & 0xff);
    }

    execvp(cmd->argv[0], cmd->argv);

    err = errno;
    report_exec_failure(cmd->argv[0], err);
    _exit((err == ENOENT || err == ENOTDIR) ? MSH_STATUS_NOT_FOUND
                                            : MSH_STATUS_CANNOT_EXEC);
}

int msh_spawn_stages(const msh_command *stages, size_t nstages,
                     msh_stage_builtin_fn run_builtin, void *ctx,
                     int job_control, pid_t *pids, pid_t *pgid_out,
                     int *start_fd_out)
{
    size_t i;
    size_t started = 0;
    int prev_read = -1;
    pid_t pgid = 0;
    int barrier[2];

    barrier[0] = -1;
    barrier[1] = -1;
    if (start_fd_out != NULL)
        *start_fd_out = -1;

    if (stages == NULL || nstages == 0 || pids == NULL)
        return -1;
    for (i = 0; i < nstages; i++) {
        if (stages[i].argv == NULL || stages[i].argv[0] == NULL)
            return -1;
    }

    /*
     * The start barrier. Children block on it until the caller has put the
     * job's process group in charge of the terminal, so a Ctrl+C typed the
     * instant a job produces output still reaches the job rather than the
     * shell. Close-on-exec so it can never reach the program itself.
     */
    if (job_control && start_fd_out != NULL) {
        if (pipe(barrier) != 0) {
            fprintf(stderr, "msh: pipe: %s\n", strerror(errno));
            return -1;
        }
        (void)fcntl(barrier[1], F_SETFD, FD_CLOEXEC);
    }

    /* Flush once, before any fork: every child would otherwise inherit a
     * copy of whatever is still buffered. */
    fflush(NULL);

    for (i = 0; i < nstages; i++) {
        int fds[2];
        pid_t pid;

        fds[0] = -1;
        fds[1] = -1;

        /* Every stage but the last writes into a new pipe. */
        if (i + 1 < nstages && pipe(fds) != 0) {
            fprintf(stderr, "msh: pipe: %s\n", strerror(errno));
            goto fail;
        }

        pid = fork();
        if (pid == -1) {
            fprintf(stderr, "msh: fork: %s\n", strerror(errno));
            close_fd(fds[0]);
            close_fd(fds[1]);
            goto fail;
        }

        if (pid == 0) {
            /*
             * Join the job's process group before anything else, so a signal
             * sent to the group reaches this stage even if it is slow to
             * exec. The parent does the same call for the same child; one of
             * the two wins and the other fails harmlessly. Doing it in only
             * one place is the classic race: the shell could hand the
             * terminal to a group this process had not joined yet.
             */
            if (job_control) {
                pid_t self = getpid();

                (void)setpgid(self, (pgid == 0) ? self : pgid);
            }

            /*
             * Wait at the barrier: the group exists now, but the terminal
             * may not have been handed to it yet. The parent closing its
             * end ends the read.
             */
            if (barrier[1] != -1)
                close_fd(barrier[1]);
            if (barrier[0] != -1) {
                char ignored;

                while (read(barrier[0], &ignored, 1) == -1 &&
                       errno == EINTR)
                    ;
                close_fd(barrier[0]);
            }

            /*
             * This child holds at most three descriptors of interest: the
             * read end of the pipe from the previous stage, and both ends of
             * the pipe to the next one. Each is moved onto a standard
             * descriptor or closed, so no pipe end survives to keep a writer
             * alive and stop EOF propagating.
             */
            if (prev_read != -1 && prev_read != STDIN_FILENO) {
                if (dup2_fd(prev_read, STDIN_FILENO) != 0)
                    _exit(MSH_STATUS_INTERNAL_ERROR);
                close_fd(prev_read);
            }
            if (fds[1] != -1 && fds[1] != STDOUT_FILENO) {
                if (dup2_fd(fds[1], STDOUT_FILENO) != 0)
                    _exit(MSH_STATUS_INTERNAL_ERROR);
                close_fd(fds[1]);
            }
            close_fd(fds[0]);

            run_stage_child(&stages[i], run_builtin, ctx);
            /* not reached */
        }

        /*
         * Parent. The same setpgid, for the same reason; EACCES means the
         * child already exec'd and ESRCH that it already exited, both of
         * which are benign here and distinct from a real failure.
         */
        if (job_control) {
            if (pgid == 0)
                pgid = pid;
            if (setpgid(pid, pgid) == -1 && errno != EACCES && errno != ESRCH) {
                fprintf(stderr, "msh: setpgid: %s\n", strerror(errno));
            }
        }

        pids[started] = pid;
        started++;

        /*
         * The parent keeps no pipe end longer than it takes to hand it to
         * the next child: holding one open would stop a reader ever seeing
         * EOF.
         */
        close_fd(prev_read);
        close_fd(fds[1]);
        prev_read = fds[0];
    }

    close_fd(prev_read);
    close_fd(barrier[0]); /* the parent never reads the barrier */
    if (pgid_out != NULL)
        *pgid_out = job_control ? pgid : 0;
    if (start_fd_out != NULL)
        *start_fd_out = barrier[1];
    return 0;

fail:
    close_fd(prev_read);
    /* Releasing the barrier lets any started stage run on to its exec and
     * then die from the signal below, rather than blocking for ever. */
    close_fd(barrier[0]);
    close_fd(barrier[1]);
    /*
     * Some stages may already be running. Their descriptors are closed, so
     * most will end on EOF or EPIPE by themselves; a stage that ignores its
     * input would not, so they are asked to terminate rather than left for
     * the shell to block on. Every one is then reaped.
     */
    for (i = 0; i < started; i++)
        (void)kill(pids[i], SIGTERM);
    for (i = 0; i < started; i++) {
        int wait_status;
        pid_t waited;

        do {
            waited = waitpid(pids[i], &wait_status, 0);
        } while (waited == -1 && errno == EINTR);
    }
    return -1;
}

/*
 * Wait for `count` children, returning the status of the one at
 * `status_index`. Every child is waited for regardless, so none is left a
 * zombie even when an earlier stage failed.
 */
static int wait_for_all(const pid_t *pids, size_t count, size_t status_index)
{
    int result = MSH_STATUS_INTERNAL_ERROR;
    size_t i;

    for (i = 0; i < count; i++) {
        int wait_status;
        pid_t waited;

        do {
            waited = waitpid(pids[i], &wait_status, 0);
        } while (waited == -1 && errno == EINTR);

        if (waited == -1) {
            fprintf(stderr, "msh: waitpid: %s\n", strerror(errno));
            continue;
        }
        if (i == status_index)
            result = msh_status_from_wait(wait_status);
    }

    return result;
}

int msh_execute(const msh_command *cmd)
{
    pid_t pid;
    pid_t pgid;

    if (cmd == NULL)
        return MSH_STATUS_INTERNAL_ERROR;

    if (msh_spawn_stages(cmd, 1, NULL, NULL, 0, &pid, &pgid, NULL) != 0)
        return MSH_STATUS_INTERNAL_ERROR;

    return wait_for_all(&pid, 1, 0);
}

int msh_execute_pipeline(const msh_pipeline *pipeline,
                         msh_stage_builtin_fn run_builtin, void *ctx)
{
    pid_t *pids;
    pid_t pgid;
    size_t n;
    int status;

    if (pipeline == NULL || pipeline->stage_count == 0)
        return MSH_STATUS_INTERNAL_ERROR;

    n = pipeline->stage_count;
    pids = malloc(n * sizeof *pids);
    if (pids == NULL) {
        fprintf(stderr, "msh: out of memory starting a pipeline\n");
        return MSH_STATUS_INTERNAL_ERROR;
    }

    if (msh_spawn_stages(pipeline->stages, n, run_builtin, ctx, 0, pids,
                         &pgid, NULL) != 0) {
        free(pids);
        return MSH_STATUS_INTERNAL_ERROR;
    }

    status = wait_for_all(pids, n, n - 1);
    free(pids);
    return status;
}

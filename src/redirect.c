#include "redirect.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Permissions for a created file: readable and writable by everyone, which
 * the process umask then narrows. This is the conventional choice, and it is
 * why the shell does not need a mode of its own.
 */
#define MSH_CREATE_MODE 0666

/* The lowest descriptor number a saved copy may take, chosen to stay clear
 * of the standard streams. */
#define MSH_SAVE_FD_BASE 10

/* --- shared helpers ----------------------------------------------------- */

/* Open `path` for reading, or -1 with errno set. */
static int open_input(const char *path)
{
    return open(path, O_RDONLY);
}

/* Open `path` for writing in the requested mode, or -1 with errno set. */
static int open_output(const char *path, msh_out_mode mode)
{
    int flags = O_WRONLY | O_CREAT;

    flags |= (mode == MSH_OUT_APPEND) ? O_APPEND : O_TRUNC;
    return open(path, flags, (mode_t)MSH_CREATE_MODE);
}

/* close() retrying on EINTR, so a descriptor is never silently left open. */
static void close_fd(int fd)
{
    if (fd < 0)
        return;
    while (close(fd) == -1) {
        if (errno != EINTR)
            return; /* EBADF or EIO: nothing further is useful here */
    }
}

/*
 * dup2() retrying on EINTR. Returns 0 or -1 with errno set.
 *
 * `newfd` is never dup2'd onto itself here, because the sources are freshly
 * opened files and saved copies living at MSH_SAVE_FD_BASE or above.
 */
static int dup2_fd(int oldfd, int newfd)
{
    while (dup2(oldfd, newfd) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* --- child side --------------------------------------------------------- */

/*
 * write() everything, tolerating short writes and EINTR. Used instead of
 * stdio in the child, which must not append to the parent's buffers.
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
        return;
    }
}

/*
 * Report a redirection failure from inside the child.
 *
 * strerror_r is the XSI variant, formatting into our own stack buffer rather
 * than libc's shared static one, so nothing process-wide is disturbed between
 * fork() and _exit().
 */
static void report_child_failure(const char *path, int err)
{
    char buf[256];
    const char *reason = buf;

    if (strerror_r(err, buf, sizeof buf) != 0)
        reason = "unknown error";

    write_all(STDERR_FILENO, "msh: ", 5);
    write_all(STDERR_FILENO, path, strlen(path));
    write_all(STDERR_FILENO, ": ", 2);
    write_all(STDERR_FILENO, reason, strlen(reason));
    write_all(STDERR_FILENO, "\n", 1);
}

int msh_redirect_apply_child(const msh_command *cmd)
{
    if (cmd == NULL)
        return -1;

    if (cmd->infile != NULL) {
        int fd = open_input(cmd->infile);

        if (fd == -1) {
            report_child_failure(cmd->infile, errno);
            return -1;
        }
        if (dup2_fd(fd, STDIN_FILENO) != 0) {
            int err = errno;

            close_fd(fd);
            report_child_failure(cmd->infile, err);
            return -1;
        }
        /* The original descriptor is redundant once it is on fd 0, and would
         * otherwise survive exec. */
        close_fd(fd);
    }

    if (cmd->outfile != NULL) {
        int fd = open_output(cmd->outfile, cmd->outmode);

        if (fd == -1) {
            report_child_failure(cmd->outfile, errno);
            return -1;
        }
        if (dup2_fd(fd, STDOUT_FILENO) != 0) {
            int err = errno;

            close_fd(fd);
            report_child_failure(cmd->outfile, err);
            return -1;
        }
        close_fd(fd);
    }

    return 0;
}

/* --- parent side -------------------------------------------------------- */

/*
 * Duplicate `fd` to a high descriptor marked close-on-exec, so the saved copy
 * cannot reach any program the shell later executes.
 */
static int save_fd(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, MSH_SAVE_FD_BASE);
}

int msh_redirect_apply_parent(const msh_command *cmd, msh_saved_fds *saved)
{
    if (cmd == NULL || saved == NULL)
        return -1;

    saved->stdin_copy = -1;
    saved->stdout_copy = -1;

    if (cmd->infile != NULL) {
        int fd = open_input(cmd->infile);

        if (fd == -1) {
            fprintf(stderr, "msh: %s: %s\n", cmd->infile, strerror(errno));
            return -1;
        }
        saved->stdin_copy = save_fd(STDIN_FILENO);
        if (saved->stdin_copy == -1) {
            fprintf(stderr, "msh: cannot save stdin: %s\n", strerror(errno));
            close_fd(fd);
            return -1;
        }
        if (dup2_fd(fd, STDIN_FILENO) != 0) {
            fprintf(stderr, "msh: %s: %s\n", cmd->infile, strerror(errno));
            close_fd(fd);
            msh_redirect_restore_parent(saved);
            return -1;
        }
        close_fd(fd);
    }

    if (cmd->outfile != NULL) {
        int fd;

        /* Anything already buffered belongs on the real stdout, not in the
         * file that is about to replace it. */
        fflush(stdout);

        fd = open_output(cmd->outfile, cmd->outmode);
        if (fd == -1) {
            fprintf(stderr, "msh: %s: %s\n", cmd->outfile, strerror(errno));
            /* Undo the input redirection, if any, so the shell is not left
             * half-redirected. */
            msh_redirect_restore_parent(saved);
            return -1;
        }
        saved->stdout_copy = save_fd(STDOUT_FILENO);
        if (saved->stdout_copy == -1) {
            fprintf(stderr, "msh: cannot save stdout: %s\n", strerror(errno));
            close_fd(fd);
            msh_redirect_restore_parent(saved);
            return -1;
        }
        if (dup2_fd(fd, STDOUT_FILENO) != 0) {
            fprintf(stderr, "msh: %s: %s\n", cmd->outfile, strerror(errno));
            close_fd(fd);
            msh_redirect_restore_parent(saved);
            return -1;
        }
        close_fd(fd);
    }

    return 0;
}

void msh_redirect_restore_parent(msh_saved_fds *saved)
{
    if (saved == NULL)
        return;

    /* The built-in's output has to reach the file before stdout goes back to
     * the terminal. */
    fflush(stdout);

    if (saved->stdout_copy != -1) {
        (void)dup2_fd(saved->stdout_copy, STDOUT_FILENO);
        close_fd(saved->stdout_copy);
        saved->stdout_copy = -1;
    }
    if (saved->stdin_copy != -1) {
        (void)dup2_fd(saved->stdin_copy, STDIN_FILENO);
        close_fd(saved->stdin_copy);
        saved->stdin_copy = -1;
    }
}

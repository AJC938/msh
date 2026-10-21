/*
 * Ctrl+D at a real terminal.
 *
 * Every other test in this suite drives msh through a pipe. A pipe reaching
 * its end is not the same event as a user pressing Ctrl+D at a terminal: the
 * terminal line discipline turns the EOT character (0x04) at the start of a
 * line into a zero-length read, and only a pty exercises that path.
 *
 * This test therefore allocates a pseudo-terminal, runs msh as its session
 * leader, confirms the prompt is written (which requires isatty() to be true),
 * sends 0x04, and requires a clean exit.
 *
 * Linux-specific: forkpty() lives in <pty.h> and needs -lutil. Built and run
 * by `make test-pty`, not by `make test`, so the portable suite stays
 * portable.
 *
 * STATUS: compiled and passing on native Linux (gcc 13.3.0, Ubuntu 24.04);
 * see docs/architecture.md for the full verification table.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROMPT "msh$ "
#define TIMEOUT_SECONDS 10

static int failures;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  %-48s ok\n", what);
    } else {
        printf("  %-48s FAILED\n", what);
        failures++;
    }
}

/*
 * Read from `fd` until `want` has been seen or the stream ends. Returns 1 if
 * found. A closed pty master reports EIO rather than end of file on Linux, so
 * that is treated as the stream ending, not as an error.
 */
static int read_until(int fd, const char *want)
{
    char seen[4096];
    size_t total = 0;

    memset(seen, 0, sizeof seen);
    for (;;) {
        ssize_t n;
        char buf[256];

        n = read(fd, buf, sizeof buf);
        if (n > 0) {
            size_t room = sizeof seen - total - 1;
            size_t copy = ((size_t)n < room) ? (size_t)n : room;

            if (copy > 0) {
                memcpy(seen + total, buf, copy);
                total += copy;
                seen[total] = '\0';
            }
            if (strstr(seen, want) != NULL)
                return 1;
            continue;
        }
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EIO)
            return 0; /* slave side closed */
        perror("read");
        return 0;
    }
}

int main(int argc, char **argv)
{
    const char *msh_path;
    int master = -1;
    pid_t child;
    int status = 0;
    char eot = 0x04;

    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/msh\n", argv[0]);
        return 2;
    }
    msh_path = argv[1];

    printf("test_pty (real terminal, Ctrl+D)\n");

    /* A hung shell must fail the test rather than block forever. */
    alarm(TIMEOUT_SECONDS);

    child = forkpty(&master, NULL, NULL, NULL);
    if (child == -1) {
        perror("forkpty");
        return 2;
    }

    if (child == 0) {
        /* Child: stdin/stdout/stderr are already the pty slave. */
        execl(msh_path, msh_path, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    /* The prompt is only written when msh believes stdin is a terminal, so
     * seeing it at all is the assertion that isatty() succeeded. */
    check(read_until(master, PROMPT), "prompt written at a real tty");

    /* Ctrl+D on an empty line: the line discipline turns this into a
     * zero-length read, which is the genuine end-of-file event. */
    if (write(master, &eot, 1) != 1) {
        perror("write");
        failures++;
    }

    if (waitpid(child, &status, 0) == -1) {
        perror("waitpid");
        failures++;
    } else {
        check(WIFEXITED(status), "exited normally (not killed by a signal)");
        if (WIFEXITED(status))
            check(WEXITSTATUS(status) == 0, "exit status 0 after Ctrl+D");
    }

    close(master);

    if (failures == 0) {
        printf("test_pty: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "test_pty: %d check(s) FAILED\n", failures);
    return 1;
}

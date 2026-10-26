/*
 * Unit tests for the executor: the wait-status mapping, argument validation,
 * and the fork()-failure path.
 *
 * _DEFAULT_SOURCE exposes RLIMIT_NPROC, which glibc hides under a strict
 * _POSIX_C_SOURCE. It is needed only by the fork-failure test.
 */
#define _DEFAULT_SOURCE

#include "executor.h"
#include "parser.h"
#include "test_helpers.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exit codes the helper child uses to report that it could not set up the
 * condition, chosen to not collide with any status under test. */
#define SETUP_FAILED 201
#define FORK_SUCCEEDED_UNEXPECTEDLY 202

/* A command with no redirections, borrowing `argv`. */
static void plain_command(msh_command *cmd, char **argv, size_t argc)
{
    cmd->argv = argv;
    cmd->argc = argc;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->outmode = MSH_OUT_NONE;
}

/*
 * The 128 + signal rule, tested without killing anything.
 *
 * The status word encoding used here is the Linux/glibc one: a normal exit
 * puts the code in the high byte, a signal death puts the signal number in
 * the low 7 bits. The encoding assumption is itself asserted below rather
 * than trusted.
 */
static void test_wait_status_mapping(void)
{
    CHECK(msh_status_from_wait(0 << 8) == 0);
    CHECK(msh_status_from_wait(1 << 8) == 1);
    CHECK(msh_status_from_wait(42 << 8) == 42);
    CHECK(msh_status_from_wait(127 << 8) == 127);
    CHECK(msh_status_from_wait(255 << 8) == 255);

    /* SIGINT (2) -> 130, SIGKILL (9) -> 137, SIGTERM (15) -> 143. */
    CHECK(msh_status_from_wait(2) == 130);
    CHECK(msh_status_from_wait(9) == 137);
    CHECK(msh_status_from_wait(15) == 143);

    /* Sanity-check the encoding assumption rather than relying on it. */
    CHECK(WIFEXITED(42 << 8) && WEXITSTATUS(42 << 8) == 42);
    CHECK(WIFSIGNALED(15) && WTERMSIG(15) == 15);
}

/*
 * A broken command must never reach fork(), and must not be reported as 126:
 * nothing was found, and nothing was tried.
 */
static void test_rejects_empty_argv(void)
{
    msh_command cmd;
    char *empty[] = { NULL };

    CHECK(msh_execute(NULL) == MSH_STATUS_INTERNAL_ERROR);

    plain_command(&cmd, empty, 0);
    CHECK(msh_execute(&cmd) == MSH_STATUS_INTERNAL_ERROR);
}

/*
 * fork() failure, induced for real.
 *
 * RLIMIT_NPROC is lowered to zero inside a disposable child of this test, so
 * only that child loses the ability to fork; the rest of the suite is
 * unaffected. The child then calls msh_execute() for a command that certainly
 * exists and reports what it returned.
 *
 * This distinguishes the two categories that matter: /bin/true exists and is
 * executable, so a 126 here would be a false statement about the command.
 * The expected answer is the shell-side failure status.
 *
 * If the condition cannot be induced -- RLIMIT_NPROC is not enforced for a
 * process with CAP_SYS_RESOURCE, so this is a no-op as root -- the test says
 * so instead of passing silently or failing.
 */
static void test_fork_failure_is_not_reported_as_cannot_exec(void)
{
    pid_t pid;
    int wait_status = 0;
    int code;

    /*
     * Flush before forking, for the same reason msh_execute() does: when this
     * test's stdout is a pipe it is block-buffered, so the helper child would
     * inherit a copy of the results printed so far and emit them a second
     * time as soon as anything flushed in the child.
     */
    fflush(NULL);

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "  FAIL: could not fork the helper child: %s\n",
                strerror(errno));
        msh_test_failures++;
        return;
    }

    if (pid == 0) {
        struct rlimit rl;
        char prog[] = "/bin/true";
        char *argv[2];
        msh_command cmd;

        rl.rlim_cur = 0;
        rl.rlim_max = 0;
        if (setrlimit(RLIMIT_NPROC, &rl) != 0)
            _exit(SETUP_FAILED);

        argv[0] = prog;
        argv[1] = NULL;
        plain_command(&cmd, argv, 1);

        /* Expected to fail inside fork(); msh_execute must still return. */
        code = msh_execute(&cmd);
        if (code == 0)
            _exit(FORK_SUCCEEDED_UNEXPECTEDLY);
        _exit(code & 0xff);
    }

    while (waitpid(pid, &wait_status, 0) == -1) {
        if (errno != EINTR) {
            fprintf(stderr, "  FAIL: waitpid on helper child: %s\n",
                    strerror(errno));
            msh_test_failures++;
            return;
        }
    }

    if (!WIFEXITED(wait_status)) {
        fprintf(stderr, "  FAIL: helper child did not exit normally\n");
        msh_test_failures++;
        return;
    }

    code = WEXITSTATUS(wait_status);
    if (code == SETUP_FAILED) {
        printf("    (skipped: could not lower RLIMIT_NPROC)\n");
        return;
    }
    if (code == FORK_SUCCEEDED_UNEXPECTEDLY) {
        printf("    (skipped: fork still succeeded; RLIMIT_NPROC not enforced)\n");
        return;
    }

    CHECK(code == MSH_STATUS_INTERNAL_ERROR);
    /* The point of the fix: a fork failure must not masquerade as 126. */
    CHECK(code != MSH_STATUS_CANNOT_EXEC);
    CHECK(code != MSH_STATUS_NOT_FOUND);
}

int main(void)
{
    printf("test_executor\n");
    RUN(test_wait_status_mapping);
    RUN(test_rejects_empty_argv);
    RUN(test_fork_failure_is_not_reported_as_cannot_exec);
    return TEST_SUMMARY();
}

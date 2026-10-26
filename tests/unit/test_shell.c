/* Unit tests for shell-level input classification and dispatch. */
#include "executor.h"
#include "shell.h"
#include "test_helpers.h"

static void test_blank_detection(void)
{
    CHECK(msh_line_is_blank(""));
    CHECK(msh_line_is_blank("   "));
    CHECK(msh_line_is_blank("\t"));
    CHECK(msh_line_is_blank("  \t  \v\f"));
    CHECK(msh_line_is_blank(NULL)); /* defensive: treated as nothing to do */

    CHECK(!msh_line_is_blank("ls"));
    CHECK(!msh_line_is_blank("  ls  "));
    CHECK(!msh_line_is_blank("#comment"));
}

/*
 * A blank line must leave the recorded status untouched, the way an empty
 * line at an sh prompt does not disturb the shell's last status. It must also
 * not fork: nothing is executed, so the previous value survives unchanged.
 */
static void test_blank_line_preserves_status(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    sh.last_status = 42;
    CHECK(msh_dispatch(&sh, "") == 42);
    CHECK(msh_dispatch(&sh, "    ") == 42);
    CHECK(msh_dispatch(&sh, "\t\t") == 42);
    CHECK(msh_dispatch(&sh, NULL) == 42);
    CHECK(!sh.should_exit);

    /* A different previous value must be preserved just as faithfully. */
    sh.last_status = 7;
    CHECK(msh_dispatch(&sh, "   ") == 7);

    msh_shell_shutdown(&sh);
}

/*
 * Milestone 3 replaced the old "not implemented" stub with real execution,
 * so dispatch now returns the child's status.
 */
static void test_dispatch_runs_external_command(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    CHECK(msh_dispatch(&sh, "/bin/true") == 0);
    CHECK(msh_dispatch(&sh, "/bin/false") == 1);

    /* Leading and trailing blanks must not defeat the lookup. */
    CHECK(msh_dispatch(&sh, "   /bin/true   ") == 0);

    /* Found via PATH rather than by absolute path. */
    CHECK(msh_dispatch(&sh, "true") == 0);

    /* Dispatch records a job, which the shell owns until shutdown. */
    msh_shell_shutdown(&sh);
}

/* A nonexistent command is reported and yields the conventional 127. */
static void test_dispatch_nonexistent_command(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    CHECK(msh_dispatch(&sh, "msh-no-such-command-xyz") == MSH_STATUS_NOT_FOUND);

    /* The shell must still be usable afterwards. */
    CHECK(msh_dispatch(&sh, "/bin/true") == 0);

    msh_shell_shutdown(&sh);
}

static void test_init_sets_defaults(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    CHECK(sh.last_status == 0);
    CHECK(!sh.should_exit);

    msh_shell_shutdown(&sh);
}

int main(void)
{
    printf("test_shell\n");
    RUN(test_blank_detection);
    RUN(test_blank_line_preserves_status);
    RUN(test_dispatch_runs_external_command);
    RUN(test_dispatch_nonexistent_command);
    RUN(test_init_sets_defaults);
    return TEST_SUMMARY();
}

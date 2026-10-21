/* Unit tests for shell-level input classification. */
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

/* A blank line must leave the recorded status untouched, the way an empty
 * line at an sh prompt does not disturb the shell's last status. */
static void test_blank_line_preserves_status(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    sh.last_status = 42;
    CHECK(msh_dispatch(&sh, "") == 42);
    CHECK(msh_dispatch(&sh, "    ") == 42);
    CHECK(!sh.should_exit);
}

/* Until milestone 3 lands, a real command is reported as not implemented
 * rather than silently ignored. */
static void test_command_reports_not_implemented(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    CHECK(msh_dispatch(&sh, "ls -l") == 127);
}

static void test_init_sets_defaults(void)
{
    msh_shell sh;
    msh_shell_init(&sh);

    CHECK(sh.last_status == 0);
    CHECK(!sh.should_exit);
}

int main(void)
{
    printf("test_shell\n");
    RUN(test_blank_detection);
    RUN(test_blank_line_preserves_status);
    RUN(test_command_reports_not_implemented);
    RUN(test_init_sets_defaults);
    return TEST_SUMMARY();
}

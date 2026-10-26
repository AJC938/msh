/*
 * Unit tests for the built-in commands.
 *
 * These call msh_builtin_run() directly, so they observe the shell process's
 * own state — which is the whole point of a built-in. Every test that moves
 * the working directory restores it, so ordering cannot leak between tests.
 */
#include "builtins.h"
#include "executor.h"
#include "test_helpers.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Build a borrowed argv from up to three literals. The strings are arrays,
 * not string literals, because -Wwrite-strings makes literals const. */
#define ARGV1(a)        char a0[] = a; char *argv[] = { a0, NULL }
#define ARGV2(a, b)     char a0[] = a; char a1[] = b; \
                        char *argv[] = { a0, a1, NULL }
#define ARGV3(a, b, c)  char a0[] = a; char a1[] = b; char a2[] = c; \
                        char *argv[] = { a0, a1, a2, NULL }

static char *saved_cwd;

static void save_cwd(void)
{
    saved_cwd = getcwd(NULL, 0); /* glibc extension, test-only convenience */
    if (saved_cwd == NULL) {
        perror("getcwd");
        exit(2);
    }
}

static void restore_cwd(void)
{
    if (saved_cwd != NULL) {
        if (chdir(saved_cwd) != 0)
            perror("chdir back");
        free(saved_cwd);
        saved_cwd = NULL;
    }
}

/* Current directory as a heap string, or NULL. */
static char *cwd_now(void)
{
    return getcwd(NULL, 0);
}

static void test_recognises_exactly_the_documented_builtins(void)
{
    CHECK(msh_builtin_exists("cd"));
    CHECK(msh_builtin_exists("pwd"));
    CHECK(msh_builtin_exists("help"));
    CHECK(msh_builtin_exists("exit"));

    CHECK(!msh_builtin_exists("ls"));
    CHECK(!msh_builtin_exists("CD"));      /* names are case-sensitive */
    CHECK(!msh_builtin_exists(""));
    CHECK(!msh_builtin_exists(NULL));
    CHECK(!msh_builtin_exists("cdx"));
    CHECK(!msh_builtin_exists("/bin/pwd")); /* a path is an external command */
}

/* An external command must fall through untouched. */
static void test_non_builtin_falls_through(void)
{
    msh_shell sh;
    int status = -12345;
    ARGV1("/bin/true");

    msh_shell_init(&sh);
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_NOT_FOUND);
    CHECK(status == -12345); /* must be left untouched */
}

/* cd changes THIS process's directory - the reason it is a built-in. */
static void test_cd_changes_the_shell_process_directory(void)
{
    msh_shell sh;
    int status = -1;
    char *before;
    char *after;
    ARGV2("cd", "/tmp");

    msh_shell_init(&sh);
    save_cwd();
    before = cwd_now();

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 0);

    after = cwd_now();
    CHECK(after != NULL);
    if (after != NULL && before != NULL) {
        CHECK(strcmp(after, before) != 0);
        /* /tmp may be a symlink; accept either spelling. */
        CHECK(strcmp(after, "/tmp") == 0 || strstr(after, "tmp") != NULL);
    }
    free(before);
    free(after);
    restore_cwd();
}

static void test_cd_with_no_argument_uses_home(void)
{
    msh_shell sh;
    int status = -1;
    const char *home = getenv("HOME");
    char *after;
    ARGV1("cd");

    if (home == NULL || *home == '\0') {
        printf("    (skipped: HOME is not set in this environment)\n");
        return;
    }

    msh_shell_init(&sh);
    save_cwd();
    CHECK(chdir("/") == 0); /* move away first so the change is observable */

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 0);

    after = cwd_now();
    CHECK(after != NULL);
    if (after != NULL)
        CHECK(strcmp(after, home) == 0);
    free(after);
    restore_cwd();
}

/* A failed cd must report and leave the directory exactly as it was. */
static void test_cd_invalid_target_preserves_directory(void)
{
    msh_shell sh;
    int status = -1;
    char *before;
    char *after;
    ARGV2("cd", "/msh-no-such-directory-xyz");

    msh_shell_init(&sh);
    save_cwd();
    before = cwd_now();

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);
    CHECK(status != MSH_STATUS_NOT_FOUND);   /* not a statement about a file */
    CHECK(status != MSH_STATUS_CANNOT_EXEC);

    after = cwd_now();
    if (after != NULL && before != NULL)
        CHECK(strcmp(after, before) == 0);
    free(before);
    free(after);
    restore_cwd();
}

/* A file is not a directory: chdir fails with ENOTDIR. */
static void test_cd_onto_a_file_fails(void)
{
    msh_shell sh;
    int status = -1;
    char *before;
    char *after;
    ARGV2("cd", "/etc/hostname");

    msh_shell_init(&sh);
    save_cwd();
    before = cwd_now();

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);

    after = cwd_now();
    if (after != NULL && before != NULL)
        CHECK(strcmp(after, before) == 0);
    free(before);
    free(after);
    restore_cwd();
}

static void test_cd_too_many_arguments_preserves_directory(void)
{
    msh_shell sh;
    int status = -1;
    char *before;
    char *after;
    ARGV3("cd", "/tmp", "/etc");

    msh_shell_init(&sh);
    save_cwd();
    before = cwd_now();

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);

    after = cwd_now();
    if (after != NULL && before != NULL)
        CHECK(strcmp(after, before) == 0); /* must NOT have moved to /tmp */
    free(before);
    free(after);
    restore_cwd();
}

static void test_cd_empty_path_fails(void)
{
    msh_shell sh;
    int status = -1;
    ARGV2("cd", "");

    msh_shell_init(&sh);
    save_cwd();
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);
    restore_cwd();
}

static void test_pwd_succeeds_and_tracks_cd(void)
{
    msh_shell sh;
    int status = -1;
    int cd_status = -1;
    char pwd_name[] = "pwd";
    char cd_name[] = "cd";
    char cd_target[] = "/tmp";
    char *pwd_argv[] = { pwd_name, NULL };
    char *cd_argv[] = { cd_name, cd_target, NULL };
    char *dir;

    msh_shell_init(&sh);
    save_cwd();

    CHECK(msh_builtin_run(&sh, pwd_argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 0);

    /* After a cd, pwd must still succeed and getcwd must agree it moved. */
    CHECK(msh_builtin_run(&sh, cd_argv, &cd_status) == MSH_BUILTIN_HANDLED);
    CHECK(cd_status == 0);

    status = -1;
    CHECK(msh_builtin_run(&sh, pwd_argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 0);

    dir = cwd_now();
    CHECK(dir != NULL);
    if (dir != NULL)
        CHECK(strstr(dir, "tmp") != NULL);
    free(dir);

    restore_cwd();
}

static void test_pwd_too_many_arguments(void)
{
    msh_shell sh;
    int status = -1;
    ARGV2("pwd", "extra");

    msh_shell_init(&sh);
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);
}

/*
 * pwd when the working directory no longer exists.
 *
 * Done inside a forked child: the directory is removed while it is the cwd,
 * which leaves that process in an unnamed directory. Doing it in the test
 * process itself would poison every later test.
 */
static void test_pwd_reports_getcwd_failure(void)
{
    char template[] = "/tmp/msh-pwd-test-XXXXXX";
    char *dir;
    pid_t pid;
    int wait_status = 0;

    dir = mkdtemp(template);
    if (dir == NULL) {
        printf("    (skipped: mkdtemp failed: %s)\n", strerror(errno));
        return;
    }

    fflush(NULL); /* do not let the child inherit pending test output */
    pid = fork();
    if (pid == -1) {
        printf("    (skipped: fork failed: %s)\n", strerror(errno));
        rmdir(dir);
        return;
    }

    if (pid == 0) {
        msh_shell sh;
        int status = -1;
        ARGV1("pwd");

        msh_shell_init(&sh);
        if (chdir(dir) != 0)
            _exit(201);
        if (rmdir(dir) != 0)
            _exit(202);
        /* getcwd() now fails with ENOENT. */
        if (msh_builtin_run(&sh, argv, &status) != MSH_BUILTIN_HANDLED)
            _exit(203);
        _exit(status == 0 ? 204 : 0); /* 0 means "reported a failure" */
    }

    while (waitpid(pid, &wait_status, 0) == -1) {
        if (errno != EINTR) {
            printf("    (skipped: waitpid failed)\n");
            rmdir(dir);
            return;
        }
    }
    rmdir(dir); /* harmless if the child already removed it */

    CHECK(WIFEXITED(wait_status));
    if (WIFEXITED(wait_status)) {
        int code = WEXITSTATUS(wait_status);

        if (code == 201 || code == 202 || code == 203) {
            printf("    (skipped: could not induce getcwd failure, code %d)\n",
                   code);
            return;
        }
        /* 0 = pwd reported a nonzero status, which is what we require. */
        CHECK(code == 0);
    }
}

static void test_help_succeeds(void)
{
    msh_shell sh;
    int status = -1;
    ARGV1("help");

    msh_shell_init(&sh);
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 0);
}

static void test_help_too_many_arguments(void)
{
    msh_shell sh;
    int status = -1;
    ARGV2("help", "extra");

    msh_shell_init(&sh);
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);
}

/* exit with no argument carries the previous command's status. */
static void test_exit_default_uses_last_status(void)
{
    msh_shell sh;
    int status = -1;
    ARGV1("exit");

    msh_shell_init(&sh);
    sh.last_status = 42;

    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status == 42);
    CHECK(sh.should_exit);
}

static void test_exit_with_explicit_status(void)
{
    msh_shell sh;
    int status = -1;

    {
        ARGV2("exit", "7");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 7);
        CHECK(sh.should_exit);
    }
    {
        ARGV2("exit", "0");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 0);
        CHECK(sh.should_exit);
    }
    {
        ARGV2("exit", "255");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 255);
    }
}

/* Only the low 8 bits survive a process exit status, so exit truncates. */
static void test_exit_status_is_masked_to_8_bits(void)
{
    msh_shell sh;
    int status = -1;

    {
        ARGV2("exit", "256");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 0);
        CHECK(sh.should_exit);
    }
    {
        ARGV2("exit", "-1");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 255);
    }
    {
        ARGV2("exit", "300");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 44);
    }
}

/*
 * A malformed status is a usage error and the shell does NOT exit. Acting on
 * a request it could not parse would be worse than refusing it.
 */
static void test_exit_invalid_argument_does_not_exit(void)
{
    msh_shell sh;
    int status = -1;
    const char *bad[] = { "abc", "12x", "x12", "1.5", "", "0x10", "--5", "+ 1" };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char buf[16];
        char name[] = "exit";
        char *argv[] = { name, buf, NULL };

        snprintf(buf, sizeof buf, "%s", bad[i]);
        msh_shell_init(&sh);
        sh.last_status = 3;

        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        if (sh.should_exit) {
            fprintf(stderr, "  FAIL: exit \"%s\" should not have exited\n",
                    bad[i]);
            msh_test_failures++;
        }
        CHECK(status != 0);
    }

    /* A leading plus is a valid integer and must still be accepted. */
    {
        ARGV2("exit", "+9");
        msh_shell_init(&sh);
        CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
        CHECK(status == 9);
        CHECK(sh.should_exit);
    }
}

static void test_exit_too_many_arguments_does_not_exit(void)
{
    msh_shell sh;
    int status = -1;
    ARGV3("exit", "1", "2");

    msh_shell_init(&sh);
    CHECK(msh_builtin_run(&sh, argv, &status) == MSH_BUILTIN_HANDLED);
    CHECK(status != 0);
    CHECK(!sh.should_exit); /* the shell stays usable */
}

int main(void)
{
    printf("test_builtins\n");
    RUN(test_recognises_exactly_the_documented_builtins);
    RUN(test_non_builtin_falls_through);
    RUN(test_cd_changes_the_shell_process_directory);
    RUN(test_cd_with_no_argument_uses_home);
    RUN(test_cd_invalid_target_preserves_directory);
    RUN(test_cd_onto_a_file_fails);
    RUN(test_cd_too_many_arguments_preserves_directory);
    RUN(test_cd_empty_path_fails);
    RUN(test_pwd_succeeds_and_tracks_cd);
    RUN(test_pwd_too_many_arguments);
    RUN(test_pwd_reports_getcwd_failure);
    RUN(test_help_succeeds);
    RUN(test_help_too_many_arguments);
    RUN(test_exit_default_uses_last_status);
    RUN(test_exit_with_explicit_status);
    RUN(test_exit_status_is_masked_to_8_bits);
    RUN(test_exit_invalid_argument_does_not_exit);
    RUN(test_exit_too_many_arguments_does_not_exit);
    return TEST_SUMMARY();
}

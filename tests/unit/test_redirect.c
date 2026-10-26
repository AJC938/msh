/*
 * Unit tests for the parent-side redirection used by built-ins.
 *
 * The decisive questions here cannot be answered by looking at output: did
 * the shell's own descriptors really come back, and did anything leak? Both
 * are answered directly:
 *
 *   - identity, by comparing fstat() device and inode on fd 0 and fd 1
 *     before and after. If stdout were left pointing at the file, the inode
 *     would differ.
 *   - leaks, by counting the entries in /proc/self/fd.
 */
#include "redirect.h"
#include "test_helpers.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How many descriptors this process has open, or (size_t)-1 if unknown. */
static size_t open_fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    size_t n = 0;
    struct dirent *e;

    if (d == NULL)
        return (size_t)-1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    /* The directory handle itself was open while counting. */
    return n > 0 ? n - 1 : 0;
}

/* Identity of what a descriptor currently refers to. */
struct fd_identity {
    dev_t dev;
    ino_t ino;
    int   ok;
};

static struct fd_identity identify(int fd)
{
    struct fd_identity id;
    struct stat st;

    if (fstat(fd, &st) == 0) {
        id.dev = st.st_dev;
        id.ino = st.st_ino;
        id.ok = 1;
    } else {
        id.dev = 0;
        id.ino = 0;
        id.ok = 0;
    }
    return id;
}

static int same_identity(struct fd_identity a, struct fd_identity b)
{
    return a.ok && b.ok && a.dev == b.dev && a.ino == b.ino;
}

/* Build a command carrying only redirections; argv is never executed here. */
static void redirect_command(msh_command *cmd, char **argv, char *in,
                             char *out, msh_out_mode mode)
{
    cmd->argv = argv;
    cmd->argc = 1;
    cmd->infile = in;   /* borrowed: msh_command_free is not called here */
    cmd->outfile = out;
    cmd->outmode = mode;
}

static char *make_temp_dir(char *tmpl)
{
    return mkdtemp(tmpl);
}

/* Applying an output redirection and restoring must leave fd 1 as it was. */
static void test_output_redirect_restores_stdout(void)
{
    char tmpl[] = "/tmp/msh-redir-XXXXXX";
    char *dir = make_temp_dir(tmpl);
    char path[256];
    char name[] = "pwd";
    char *argv[] = { name, NULL };
    msh_command cmd;
    struct fd_identity before;
    struct fd_identity during;
    struct fd_identity after;
    size_t fds_before;
    size_t fds_after;
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;

    if (dir == NULL) {
        printf("    (skipped: mkdtemp failed: %s)\n", strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/out.txt", dir);

    before = identify(STDOUT_FILENO);
    fds_before = open_fd_count();

    redirect_command(&cmd, argv, NULL, path, MSH_OUT_TRUNCATE);
    CHECK(msh_redirect_apply_parent(&cmd, &saved) == 0);

    during = identify(STDOUT_FILENO);
    /* While redirected, fd 1 must NOT be what it was. */
    CHECK(!same_identity(before, during));

    printf("written while redirected\n");
    msh_redirect_restore_parent(&saved);

    after = identify(STDOUT_FILENO);
    CHECK(same_identity(before, after));

    fds_after = open_fd_count();
    if (fds_before != (size_t)-1 && fds_after != (size_t)-1)
        CHECK(fds_before == fds_after);

    /* The text really went to the file. */
    {
        FILE *f = fopen(path, "r");
        char buf[128] = { 0 };

        CHECK(f != NULL);
        if (f != NULL) {
            if (fgets(buf, (int)sizeof buf, f) != NULL)
                CHECK(strncmp(buf, "written while redirected", 24) == 0);
            fclose(f);
        }
    }

    unlink(path);
    rmdir(dir);
}

/* The same for input, on fd 0. */
static void test_input_redirect_restores_stdin(void)
{
    char tmpl[] = "/tmp/msh-redir-XXXXXX";
    char *dir = make_temp_dir(tmpl);
    char path[256];
    char name[] = "cat";
    char *argv[] = { name, NULL };
    msh_command cmd;
    struct fd_identity before;
    struct fd_identity during;
    struct fd_identity after;
    size_t fds_before;
    size_t fds_after;
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;
    FILE *f;

    if (dir == NULL) {
        printf("    (skipped: mkdtemp failed: %s)\n", strerror(errno));
        return;
    }
    snprintf(path, sizeof path, "%s/in.txt", dir);
    f = fopen(path, "w");
    if (f == NULL) {
        printf("    (skipped: cannot create the input file)\n");
        rmdir(dir);
        return;
    }
    fputs("data\n", f);
    fclose(f);

    before = identify(STDIN_FILENO);
    fds_before = open_fd_count();

    redirect_command(&cmd, argv, path, NULL, MSH_OUT_NONE);
    CHECK(msh_redirect_apply_parent(&cmd, &saved) == 0);

    during = identify(STDIN_FILENO);
    CHECK(!same_identity(before, during));

    msh_redirect_restore_parent(&saved);

    after = identify(STDIN_FILENO);
    CHECK(same_identity(before, after));

    fds_after = open_fd_count();
    if (fds_before != (size_t)-1 && fds_after != (size_t)-1)
        CHECK(fds_before == fds_after);

    unlink(path);
    rmdir(dir);
}

/*
 * A failed open must leave the shell completely unredirected, with nothing
 * saved and nothing leaked. This is the path where a half-applied
 * redirection would be most damaging.
 */
static void test_failed_open_leaves_shell_untouched(void)
{
    char name[] = "pwd";
    char badpath[] = "/msh-no-such-directory-xyz/out.txt";
    char *argv[] = { name, NULL };
    msh_command cmd;
    struct fd_identity out_before;
    struct fd_identity in_before;
    struct fd_identity out_after;
    struct fd_identity in_after;
    size_t fds_before;
    size_t fds_after;
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;

    out_before = identify(STDOUT_FILENO);
    in_before = identify(STDIN_FILENO);
    fds_before = open_fd_count();

    /* A directory that does not exist, so open() fails with ENOENT. */
    redirect_command(&cmd, argv, NULL, badpath, MSH_OUT_TRUNCATE);
    CHECK(msh_redirect_apply_parent(&cmd, &saved) == -1);

    out_after = identify(STDOUT_FILENO);
    in_after = identify(STDIN_FILENO);
    CHECK(same_identity(out_before, out_after));
    CHECK(same_identity(in_before, in_after));

    fds_after = open_fd_count();
    if (fds_before != (size_t)-1 && fds_after != (size_t)-1)
        CHECK(fds_before == fds_after);

    /* Restoring after a failure must be harmless. */
    msh_redirect_restore_parent(&saved);
    CHECK(same_identity(out_before, identify(STDOUT_FILENO)));
}

/*
 * An input that opens followed by an output that fails: the input redirection
 * already applied must be undone, not left in place.
 */
static void test_second_failure_undoes_the_first(void)
{
    char tmpl[] = "/tmp/msh-redir-XXXXXX";
    char *dir = make_temp_dir(tmpl);
    char path[256];
    char name[] = "cat";
    char badpath[] = "/msh-no-such-directory-xyz/out.txt";
    char *argv[] = { name, NULL };
    msh_command cmd;
    struct fd_identity in_before;
    struct fd_identity in_after;
    size_t fds_before;
    size_t fds_after;
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;
    FILE *f;

    if (dir == NULL) {
        printf("    (skipped: mkdtemp failed)\n");
        return;
    }
    snprintf(path, sizeof path, "%s/in.txt", dir);
    f = fopen(path, "w");
    if (f == NULL) {
        printf("    (skipped: cannot create the input file)\n");
        rmdir(dir);
        return;
    }
    fputs("data\n", f);
    fclose(f);

    in_before = identify(STDIN_FILENO);
    fds_before = open_fd_count();

    redirect_command(&cmd, argv, path, badpath, MSH_OUT_TRUNCATE);
    CHECK(msh_redirect_apply_parent(&cmd, &saved) == -1);

    in_after = identify(STDIN_FILENO);
    CHECK(same_identity(in_before, in_after)); /* stdin must be back */

    fds_after = open_fd_count();
    if (fds_before != (size_t)-1 && fds_after != (size_t)-1)
        CHECK(fds_before == fds_after);

    unlink(path);
    rmdir(dir);
}

/* Repeated apply/restore cycles must not accumulate descriptors. */
static void test_repeated_cycles_do_not_leak(void)
{
    char tmpl[] = "/tmp/msh-redir-XXXXXX";
    char *dir = make_temp_dir(tmpl);
    char path[256];
    char name[] = "pwd";
    char *argv[] = { name, NULL };
    size_t fds_before;
    size_t fds_after;
    int i;

    if (dir == NULL) {
        printf("    (skipped: mkdtemp failed)\n");
        return;
    }
    snprintf(path, sizeof path, "%s/out.txt", dir);

    fds_before = open_fd_count();

    for (i = 0; i < 50; i++) {
        msh_command cmd;
        msh_saved_fds saved = MSH_SAVED_FDS_INIT;

        redirect_command(&cmd, argv, NULL, path,
                         (i % 2 == 0) ? MSH_OUT_TRUNCATE : MSH_OUT_APPEND);
        if (msh_redirect_apply_parent(&cmd, &saved) != 0) {
            msh_test_failures++;
            break;
        }
        msh_redirect_restore_parent(&saved);
    }

    fds_after = open_fd_count();
    if (fds_before != (size_t)-1 && fds_after != (size_t)-1)
        CHECK(fds_before == fds_after);

    unlink(path);
    rmdir(dir);
}

/* Restoring a never-applied value must be safe, and safe twice. */
static void test_restore_without_apply_is_safe(void)
{
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;
    struct fd_identity before = identify(STDOUT_FILENO);

    msh_redirect_restore_parent(&saved);
    msh_redirect_restore_parent(&saved);
    msh_redirect_restore_parent(NULL);

    CHECK(same_identity(before, identify(STDOUT_FILENO)));
}

int main(void)
{
    printf("test_redirect\n");
    RUN(test_output_redirect_restores_stdout);
    RUN(test_input_redirect_restores_stdin);
    RUN(test_failed_open_leaves_shell_untouched);
    RUN(test_second_failure_undoes_the_first);
    RUN(test_repeated_cycles_do_not_leak);
    RUN(test_restore_without_apply_is_safe);
    return TEST_SUMMARY();
}

#include "builtins.h"

#include "executor.h"
#include "jobs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Status used when a built-in is asked to do something it cannot: a bad
 * argument count, an unusable path, a failed system call. 1 is the ordinary
 * "this command failed" value; 126 and 127 are reserved for statements about
 * an external command's executability and would be untrue here.
 */
#define MSH_BUILTIN_FAILURE 1

/*
 * Status for a malformed numeric argument to `exit`, which is a usage error
 * rather than a runtime failure. 2 matches what msh already returns for
 * unusable command-line arguments in main().
 */
#define MSH_BUILTIN_USAGE_ERROR 2

/*
 * Return the shell's current directory as a heap string, or NULL with errno
 * set.
 *
 * No PATH_MAX buffer: the length is discovered by growing on ERANGE, which is
 * the same approach the line reader takes and avoids a limit that is not
 * guaranteed to be meaningful on Linux.
 */
static char *current_directory(void)
{
    size_t cap = 128;

    for (;;) {
        char *buf = malloc(cap);

        if (buf == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        if (getcwd(buf, cap) != NULL)
            return buf;

        free(buf);
        if (errno != ERANGE)
            return NULL; /* a real failure, e.g. the directory was removed */

        if (cap > ((size_t)-1) / 2) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        cap *= 2;
    }
}

static size_t count_args(char *const *argv)
{
    size_t n = 0;

    while (argv[n] != NULL)
        n++;
    return n;
}

/* --- cd ----------------------------------------------------------------- */

static int builtin_cd(msh_shell *sh, char *const *argv, size_t argc)
{
    const char *target;

    (void)sh;

    if (argc > 2) {
        /* Reported, and the current directory is left exactly as it was. */
        fprintf(stderr, "msh: cd: too many arguments\n");
        return MSH_BUILTIN_FAILURE;
    }

    if (argc == 1) {
        target = getenv("HOME");
        if (target == NULL || *target == '\0') {
            fprintf(stderr, "msh: cd: HOME is not set\n");
            return MSH_BUILTIN_FAILURE;
        }
    } else {
        target = argv[1];
        if (*target == '\0') {
            /* chdir("") fails with ENOENT; say something more useful. */
            fprintf(stderr, "msh: cd: empty path\n");
            return MSH_BUILTIN_FAILURE;
        }
    }

    if (chdir(target) != 0) {
        fprintf(stderr, "msh: cd: %s: %s\n", target, strerror(errno));
        return MSH_BUILTIN_FAILURE;
    }

    return 0;
}

/* --- pwd ---------------------------------------------------------------- */

static int builtin_pwd(msh_shell *sh, char *const *argv, size_t argc)
{
    char *dir;

    (void)sh;
    (void)argv;

    if (argc > 1) {
        fprintf(stderr, "msh: pwd: too many arguments\n");
        return MSH_BUILTIN_FAILURE;
    }

    dir = current_directory();
    if (dir == NULL) {
        fprintf(stderr, "msh: pwd: %s\n", strerror(errno));
        return MSH_BUILTIN_FAILURE;
    }

    printf("%s\n", dir);
    free(dir);
    return 0;
}

/* --- help --------------------------------------------------------------- */

static int builtin_help(msh_shell *sh, char *const *argv, size_t argc)
{
    (void)sh;
    (void)argv;

    if (argc > 1) {
        fprintf(stderr, "msh: help: too many arguments\n");
        return MSH_BUILTIN_FAILURE;
    }

    printf("msh - a minimal Unix-style shell\n"
           "\n"
           "msh is a learning project. It is not bash and does not implement\n"
           "the POSIX shell language.\n"
           "\n"
           "Built-in commands (run inside the shell itself):\n"
           "  cd [dir]    change the shell's working directory; with no\n"
           "              argument, change to $HOME\n"
           "  pwd         print the shell's working directory\n"
           "  help        print this text\n"
           "  exit [n]    leave the shell with status n, or with the status of\n"
           "              the last command if n is omitted\n"
           "  jobs        list the jobs this shell is tracking\n"
           "  fg [%%n]     bring a job to the foreground and wait for it\n"
           "  bg [%%n]     resume a stopped job in the background\n"
           "With no %%n, fg and bg pick the most recently stopped job, or the\n"
           "most recent job still running.\n"
           "\n"
           "Everything else is run as an external program, found through PATH,\n"
           "with the shell's environment.\n"
           "\n"
           "Quoting:\n"
           "  'text'      every character literal; no escapes inside\n"
           "  \"text\"      groups spaces; \\\" and \\\\ are the only escapes\n"
           "  \\c          outside quotes, c is taken literally\n"
           "  ab\"cd\"'ef'  adjacent segments join into one argument\n"
           "  '' or \"\"    an empty argument\n"
           "\n"
           "Redirection (for built-ins and external commands alike):\n"
           "  cmd < file    read standard input from file\n"
           "  cmd > file    write standard output to file, creating or\n"
           "                truncating it\n"
           "  cmd >> file   write standard output to file, creating it or\n"
           "                appending to it\n"
           "One input and one output redirection per command, in either order.\n"
           "Standard error is never redirected; 2> is not implemented.\n"
           "\n"
           "Pipelines:\n"
           "  a | b | c     each stage's output becomes the next stage's input\n"
           "Every stage runs at the same time, in one process group, each with\n"
           "its own arguments and redirections. An explicit redirection\n"
           "overrides the pipe. The pipeline's status is its LAST stage's;\n"
           "there is no pipefail.\n"
           "\n"
           "Background jobs:\n"
           "  cmd &         run the whole pipeline in the background\n"
           "The job number and its process group are printed, the prompt comes\n"
           "straight back, and the job is reported when it finishes. `&` must\n"
           "be the last thing on the line and must follow a command, so `a & b`\n"
           "and `a && b` are errors rather than half-run command lists.\n"
           "\n"
           "At a terminal:\n"
           "  Ctrl+C        interrupt the foreground job\n"
           "  Ctrl+Z        stop the foreground job; fg or bg resumes it\n"
           "Signals reach the job's whole process group, so every stage of a\n"
           "pipeline gets them. The shell itself is not interrupted or stopped.\n"
           "A built-in used as a pipeline stage or backgrounded runs in a child,\n"
           "so `cd /tmp | cat` and `exit &` cannot change or end this shell.\n"
           "\n"
           "`exit` refuses once while jobs are still running and tells you so;\n"
           "a second `exit` leaves anyway.\n"
           "\n"
           "A malformed line is reported and nothing on it runs, so a bad\n"
           "command never creates a file and never starts part of a pipeline.\n"
           "\n"
           "Exit status:\n"
           "  0-255       the command's own exit status\n"
           "  128+n       the command was killed by signal n\n"
           "  126         the command exists but could not be executed\n"
           "  127         no such command\n"
           "  2           a syntax error, or a bad argument to exit\n"
           "  1           a built-in failed, a redirection failed, or the\n"
           "              shell itself could not run the command\n"
           "Launching a background job succeeds, so it reports 0.\n"
           "\n"
           "Not implemented yet:\n"
           "  logical operators and lists        (&& || ;)\n"
           "  stderr redirection and heredocs    (2>, <<)\n"
           "  variable expansion and globbing    ($VAR, $?, *)\n"
           "  command history and aliases\n"
           "\n"
           "Characters such as ; are NOT operators here: they are passed to the\n"
           "command as ordinary argument text.\n"
           "\n"
           "Press Ctrl+D at the prompt, or type exit, to leave.\n");

    return 0;
}

/* --- exit --------------------------------------------------------------- */

/*
 * Parse an `exit` argument into a status.
 *
 * Accepts a complete decimal integer, with an optional sign and nothing else
 * in the token. The value is reduced to the low 8 bits, which is what a
 * process exit status can actually carry: `exit 256` leaves 0 and `exit -1`
 * leaves 255. Returns 0 on success, -1 if the token is not a number.
 */
static int parse_exit_status(const char *text, int *out)
{
    char *end;
    long value;

    if (text == NULL || *text == '\0')
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);

    if (end == text || *end != '\0')
        return -1;
    if (errno == ERANGE)
        return -1;

    *out = (int)((unsigned long)value & 0xffUL);
    return 0;
}

static int builtin_exit(msh_shell *sh, char *const *argv, size_t argc)
{
    int status;

    if (argc > 2) {
        /*
         * Documented choice: report and do NOT exit, leaving the shell usable
         * and the previous status untouched. Acting on an ambiguous request to
         * terminate is worse than refusing it, and this matches how cd and pwd
         * treat a bad argument count.
         */
        fprintf(stderr, "msh: exit: too many arguments\n");
        return MSH_BUILTIN_FAILURE;
    }

    if (argc == 1) {
        status = sh->last_status;
    } else if (parse_exit_status(argv[1], &status) != 0) {
        /*
         * Documented choice: a malformed status is a usage error, and the
         * shell does NOT exit. The alternative (exit anyway with 2) would act
         * on a request it could not understand.
         */
        fprintf(stderr, "msh: exit: %s: numeric argument required\n", argv[1]);
        return MSH_BUILTIN_USAGE_ERROR;
    }

    /*
     * Leaving with jobs still running would abandon them with no shell to
     * report to. Documented policy: refuse once, say so, and honour a second
     * `exit` -- which is the convention a user expects and avoids losing work
     * to a mistyped command. Any other command in between clears the warning,
     * so the two attempts have to be deliberate.
     */
    if (!sh->exit_warned && msh_jobctl_active_count(&sh->jc) > 0) {
        fprintf(stderr, "msh: exit: there are jobs still running\n");
        msh_jobctl_list(&sh->jc);
        sh->exit_warned = true;
        return MSH_BUILTIN_FAILURE;
    }

    /*
     * Do not call exit() here. Setting the flag lets msh_shell_run leave its
     * loop normally, so the current line buffer is freed on the ordinary path
     * and cleanup happens exactly once.
     */
    sh->should_exit = true;
    return status;
}

/* --- jobs ---------------------------------------------------------------- */

static int builtin_jobs(msh_shell *sh, char *const *argv, size_t argc)
{
    (void)argv;

    if (argc > 1) {
        fprintf(stderr, "msh: jobs: too many arguments\n");
        return MSH_BUILTIN_FAILURE;
    }

    msh_jobctl_list(&sh->jc);
    return 0;
}

/* --- fg and bg ----------------------------------------------------------- */

/*
 * Resolve the job a `fg`/`bg` invocation names.
 *
 * With no argument the documented default applies: the most recently stopped
 * job, else the most recently started one still active. With an argument it
 * must be a job id, written `%2` or `2`.
 *
 * Returns NULL having already reported why, so the callers stay short.
 */
static msh_job *select_job(msh_shell *sh, char *const *argv, size_t argc,
                           const char *name)
{
    msh_job *job;
    int id;

    if (argc > 2) {
        fprintf(stderr, "msh: %s: too many arguments\n", name);
        return NULL;
    }

    if (argc == 1) {
        job = msh_jobctl_current(&sh->jc);
        if (job == NULL)
            fprintf(stderr, "msh: %s: no current job\n", name);
        return job;
    }

    if (msh_job_parse_spec(argv[1], &id) != 0) {
        fprintf(stderr, "msh: %s: %s: not a job id\n", name, argv[1]);
        return NULL;
    }

    job = msh_jobctl_find(&sh->jc, id);
    if (job == NULL || msh_job_state_of(job) == MSH_JOB_DONE) {
        fprintf(stderr, "msh: %s: %%%d: no such job\n", name, id);
        return NULL;
    }
    return job;
}

static int builtin_fg(msh_shell *sh, char *const *argv, size_t argc)
{
    msh_job *job;

    /* Act on what is actually true now, not on a stale table. */
    msh_jobctl_poll(&sh->jc);

    job = select_job(sh, argv, argc, "fg");
    if (job == NULL)
        return MSH_BUILTIN_FAILURE;

    /* Conventional: say which command is coming back to the foreground. */
    printf("%s\n", job->command);
    fflush(stdout);

    return msh_jobctl_foreground(&sh->jc, job);
}

static int builtin_bg(msh_shell *sh, char *const *argv, size_t argc)
{
    msh_job *job;

    msh_jobctl_poll(&sh->jc);

    job = select_job(sh, argv, argc, "bg");
    if (job == NULL)
        return MSH_BUILTIN_FAILURE;

    if (msh_job_state_of(job) == MSH_JOB_RUNNING) {
        fprintf(stderr, "msh: bg: job %d is already running\n", job->id);
        return MSH_BUILTIN_FAILURE;
    }

    if (msh_jobctl_background_resume(&sh->jc, job) != 0)
        return MSH_BUILTIN_FAILURE;

    printf("[%d] %s &\n", job->id, job->command);
    fflush(stdout);
    return 0;
}

/* --- table and dispatch ------------------------------------------------- */

typedef int (*builtin_fn)(msh_shell *sh, char *const *argv, size_t argc);

struct builtin_entry {
    const char *name;
    builtin_fn  fn;
};

static const struct builtin_entry builtins[] = {
    { "cd",   builtin_cd   },
    { "pwd",  builtin_pwd  },
    { "help", builtin_help },
    { "exit", builtin_exit },
    { "jobs", builtin_jobs },
    { "fg",   builtin_fg   },
    { "bg",   builtin_bg   }
};

#define BUILTIN_COUNT (sizeof builtins / sizeof builtins[0])

bool msh_builtin_exists(const char *name)
{
    size_t i;

    if (name == NULL)
        return false;
    for (i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(builtins[i].name, name) == 0)
            return true;
    }
    return false;
}

msh_builtin_result msh_builtin_run(msh_shell *sh, char *const *argv,
                                   int *status)
{
    size_t i;

    if (sh == NULL || argv == NULL || argv[0] == NULL || status == NULL)
        return MSH_BUILTIN_NOT_FOUND;

    for (i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(builtins[i].name, argv[0]) == 0) {
            *status = builtins[i].fn(sh, argv, count_args(argv));
            return MSH_BUILTIN_HANDLED;
        }
    }

    return MSH_BUILTIN_NOT_FOUND;
}

#include "shell.h"
#include "builtins.h"
#include "executor.h"
#include "jobs.h"
#include "lineread.h"
#include "parser.h"
#include "redirect.h"
#include "signals.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MSH_PROMPT "msh$ "

void msh_shell_init(msh_shell *sh)
{
    sh->interactive = isatty(STDIN_FILENO) ? true : false;
    sh->last_status = 0;
    sh->should_exit = false;
    sh->exit_warned = false;

    /*
     * Signal policy first, then job control: taking the terminal can send
     * SIGTTIN/SIGTTOU at us, and those must already be ignored by then.
     */
    msh_signals_init_shell(sh->interactive ? 1 : 0);
    msh_jobctl_init(&sh->jc, sh->interactive ? 1 : 0);
}

void msh_shell_shutdown(msh_shell *sh)
{
    msh_jobctl_shutdown(&sh->jc);
}

bool msh_line_is_blank(const char *line)
{
    if (line == NULL)
        return true;
    for (; *line != '\0'; line++) {
        /* Cast via unsigned char: isspace() on a negative char is undefined. */
        if (!isspace((unsigned char)*line))
            return false;
    }
    return true;
}

/*
 * Run a built-in, with the command's redirections applied to this process for
 * exactly the duration of the call.
 *
 * This is the STANDALONE path: a built-in that is the whole line and is not
 * backgrounded. It cannot run in a child -- `cd` has to move this process,
 * `exit` has to end it, and `fg` has to own the terminal -- so its
 * redirections are installed here and then undone, on every path.
 */
static void run_builtin_redirected(msh_shell *sh, const msh_command *cmd,
                                   int *status)
{
    msh_saved_fds saved = MSH_SAVED_FDS_INIT;
    int redirected = (cmd->infile != NULL || cmd->outfile != NULL);

    if (redirected && msh_redirect_apply_parent(cmd, &saved) != 0) {
        *status = MSH_STATUS_INTERNAL_ERROR;
        return;
    }

    (void)msh_builtin_run(sh, cmd->argv, status);

    if (redirected)
        msh_redirect_restore_parent(&saved);
}

/*
 * Run a built-in as a stage inside a child the executor has already forked.
 *
 * The shell state handed over is that child's own copy, so `cd` and `exit`
 * affect the child alone and the real shell is untouched.
 */
static int stage_builtin(void *ctx, char *const *argv, int *status)
{
    msh_shell *sh = ctx;

    return msh_builtin_run(sh, argv, status) == MSH_BUILTIN_HANDLED ? 1 : 0;
}

/*
 * Start a pipeline as a job and either wait for it or leave it running.
 *
 * Every stage goes into one process group, so a terminal signal reaches the
 * whole pipeline and `fg`, `bg` and `kill` can address it as a unit.
 */
static int run_as_job(msh_shell *sh, const msh_pipeline *pipeline,
                      const char *line)
{
    pid_t *pids;
    pid_t pgid = 0;
    int start_fd = -1;
    size_t n = pipeline->stage_count;
    msh_job *job;
    int job_control = sh->jc.interactive;

    pids = malloc(n * sizeof *pids);
    if (pids == NULL) {
        fprintf(stderr, "msh: out of memory starting a job\n");
        return MSH_STATUS_INTERNAL_ERROR;
    }

    if (msh_spawn_stages(pipeline->stages, n, stage_builtin, sh, job_control,
                         pids, &pgid, &start_fd) != 0) {
        free(pids);
        return MSH_STATUS_INTERNAL_ERROR;
    }
    if (pgid == 0)
        pgid = pids[0]; /* no job control: name the job by its first pid */

    job = msh_jobctl_add(&sh->jc, pgid, pids, n, line,
                         pipeline->background ? 1 : 0);
    free(pids);

    if (job == NULL) {
        /*
         * The table could not take the job. The processes exist and must not
         * be abandoned, so release them and wait here rather than leaving
         * zombies or stages blocked at the barrier.
         */
        fprintf(stderr, "msh: out of memory tracking a job\n");
        if (start_fd >= 0)
            close(start_fd);
        while (waitpid(-1, NULL, 0) > 0)
            ;
        return MSH_STATUS_INTERNAL_ERROR;
    }

    job->start_fd = start_fd;

    if (pipeline->background) {
        msh_jobctl_announce(&sh->jc, job);
        /* A background job never gets the terminal, so it may run at once. */
        msh_job_start(job);
        /*
         * Documented convention: launching a background job succeeds, so the
         * status is 0. The job's own status is reported when it finishes.
         */
        return 0;
    }

    return msh_jobctl_foreground(&sh->jc, job);
}

int msh_dispatch(msh_shell *sh, const char *line)
{
    msh_pipeline pipeline;
    int status;

    /*
     * Blank input is not a command: nothing is forked, no file is opened,
     * and the previous command's status is left alone, as at an sh prompt.
     */
    if (msh_line_is_blank(line))
        return sh->last_status;

    switch (msh_parse(line, &pipeline)) {
    case MSH_PARSE_OK:
        break;

    case MSH_PARSE_EMPTY:
        return sh->last_status;

    case MSH_PARSE_SYNTAX:
        /*
         * Malformed quoting, redirection, pipeline or background operator.
         * Nothing is executed, no file is opened, and no stage is started.
         */
        fprintf(stderr, "msh: syntax error: %s\n",
                msh_syntax_error_text(pipeline.error));
        return MSH_STATUS_SYNTAX_ERROR;

    case MSH_PARSE_NOMEM:
        fprintf(stderr, "msh: cannot parse input: out of memory\n");
        return MSH_STATUS_INTERNAL_ERROR;

    default:
        fprintf(stderr, "msh: internal error: unknown parse result\n");
        return MSH_STATUS_INTERNAL_ERROR;
    }

    /*
     * A lone built-in that is not backgrounded runs in this process: `cd`
     * moves the shell, `exit` ends it, `fg` takes the terminal. Everything
     * else -- a pipeline, or a backgrounded built-in -- becomes a job whose
     * stages all run in children.
     */
    if (pipeline.stage_count == 1 && !pipeline.background &&
        msh_builtin_exists(pipeline.stages[0].argv[0]))
        run_builtin_redirected(sh, &pipeline.stages[0], &status);
    else
        status = run_as_job(sh, &pipeline, line);

    msh_pipeline_free(&pipeline);
    return status;
}

static void write_prompt(const msh_shell *sh)
{
    if (!sh->interactive)
        return;
    fputs(MSH_PROMPT, stdout);
    /* stdout is not line-buffered to a terminal in every environment, and the
     * prompt has no newline, so flush explicitly or it may not appear. */
    fflush(stdout);
}

int msh_shell_run(msh_shell *sh)
{
    int prompt_needed = 1;

    while (!sh->should_exit) {
        char *line = NULL;
        msh_read_status status;

        /*
         * Finished and newly stopped jobs are reported here, just before the
         * prompt, which is the conventional moment and the only one where
         * writing to the terminal cannot interleave with a running job.
         */
        msh_jobctl_notify(&sh->jc);

        if (prompt_needed)
            write_prompt(sh);
        prompt_needed = 1;

        status = msh_read_line(stdin, &line, NULL);

        if (status == MSH_READ_INTERRUPTED) {
            /*
             * A signal arrived before anything was typed -- in practice
             * SIGCHLD for a finished background job. Collect it and read
             * again WITHOUT reprinting the prompt, so a notification appears
             * above the line the user is still typing rather than leaving a
             * second prompt behind.
             */
            prompt_needed = 0;
            continue;
        }
        if (status == MSH_READ_EOF) {
            /* Ctrl+D at an interactive prompt: leave the cursor on a fresh
             * line so the user's next shell prompt is not glued to ours. */
            if (sh->interactive)
                fputs("\n", stdout);
            break;
        }
        if (status == MSH_READ_ERROR) {
            fprintf(stderr, "msh: failed to read input\n");
            return 1;
        }

        sh->last_status = msh_dispatch(sh, line);

        /*
         * Any command other than a refused `exit` clears the warning, so the
         * two attempts needed to leave with running jobs have to be
         * consecutive and deliberate.
         */
        if (!msh_line_is_blank(line) && !sh->should_exit) {
            const char *p = line;

            while (*p != '\0' && isspace((unsigned char)*p))
                p++;
            if (strncmp(p, "exit", 4) != 0)
                sh->exit_warned = false;
        }

        free(line);
    }

    return sh->last_status;
}

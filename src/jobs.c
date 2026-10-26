#include "jobs.h"

#include "executor.h"
#include "signals.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MSH_JOBS_INITIAL_CAP 8

/* --- small helpers ------------------------------------------------------ */

static char *dup_string(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL)
        s = "";
    n = strlen(s) + 1;
    copy = malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

static void job_release(msh_job *job)
{
    free(job->command);
    free(job->procs);
    job->command = NULL;
    job->procs = NULL;
    job->nprocs = 0;
}

msh_job_state msh_job_state_of(const msh_job *job)
{
    size_t i;
    int running = 0;
    int stopped = 0;

    if (job == NULL || job->nprocs == 0)
        return MSH_JOB_DONE;

    for (i = 0; i < job->nprocs; i++) {
        if (job->procs[i].state == MSH_PROC_RUNNING)
            running = 1;
        else if (job->procs[i].state == MSH_PROC_STOPPED)
            stopped = 1;
    }

    /*
     * Derived from every member, never from one. A pipeline whose first stage
     * has exited but whose last stage is stopped is a stopped job, not a
     * finished one.
     */
    if (running)
        return MSH_JOB_RUNNING;
    if (stopped)
        return MSH_JOB_STOPPED;
    return MSH_JOB_DONE;
}

int msh_job_exit_status(const msh_job *job)
{
    const msh_proc *last;

    if (job == NULL || job->nprocs == 0)
        return MSH_STATUS_INTERNAL_ERROR;

    /* The shell's established rule: a pipeline reports its last stage. */
    last = &job->procs[job->nprocs - 1];
    if (last->state != MSH_PROC_DONE)
        return MSH_STATUS_INTERNAL_ERROR;
    return msh_status_from_wait(last->status);
}

/* --- the table ---------------------------------------------------------- */

static void table_init(msh_job_table *t)
{
    t->jobs = NULL;
    t->count = 0;
    t->cap = 0;
    t->next_id = 1;
}

void msh_jobctl_init(msh_jobctl *jc, int interactive)
{
    table_init(&jc->table);
    jc->interactive = 0;
    jc->term_fd = -1;
    jc->shell_pgid = getpgrp();
    jc->have_modes = 0;

    if (!interactive)
        return;

    /*
     * Only take the terminal if we actually have one and can read its
     * foreground group. In CI, or under a harness that gave us a pipe, this
     * fails and job control simply stays off.
     */
    if (!isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) == -1)
        return;

    jc->term_fd = STDIN_FILENO;

    /*
     * Ignore the terminal signals before touching the terminal: tcsetpgrp()
     * from a process that is not already the foreground group raises SIGTTOU,
     * which would otherwise stop the shell during its own startup.
     */
    msh_signals_ignore_job_control();

    /* A process group of our own, so jobs can have theirs. */
    jc->shell_pgid = getpid();
    if (setpgid(jc->shell_pgid, jc->shell_pgid) == -1 && errno != EPERM) {
        /* EPERM means we are already a session leader, which is fine. */
        jc->shell_pgid = getpgrp();
    }

    /*
     * Claim the terminal rather than waiting to be given it.
     *
     * The textbook shell loops here sending itself SIGTTIN until a user
     * brings it to the foreground. msh deliberately does not: that loop
     * hangs the shell outright whenever the condition is true for any other
     * reason -- an automated harness, a terminal whose foreground group is
     * not yet settled -- and a wedged shell with no way to continue it is a
     * far worse failure than the one it guards against. If the terminal
     * cannot be claimed, job control is simply switched off and msh runs the
     * way it does without a terminal at all.
     */
    if (tcsetpgrp(jc->term_fd, jc->shell_pgid) == -1) {
        jc->term_fd = -1;
        return;
    }
    if (tcgetattr(jc->term_fd, &jc->shell_modes) == 0)
        jc->have_modes = 1;

    jc->interactive = 1;
}

void msh_jobctl_shutdown(msh_jobctl *jc)
{
    size_t i;

    if (jc == NULL)
        return;
    for (i = 0; i < jc->table.count; i++) {
        msh_job_start(&jc->table.jobs[i]); /* never leave one blocked */
        job_release(&jc->table.jobs[i]);
    }
    free(jc->table.jobs);
    table_init(&jc->table);
}

msh_job *msh_jobctl_add(msh_jobctl *jc, pid_t pgid, const pid_t *pids,
                        size_t nprocs, const char *command, int background)
{
    msh_job_table *t = &jc->table;
    msh_job *job;
    sigset_t saved;
    size_t i;

    if (nprocs == 0)
        return NULL;

    if (t->count == t->cap) {
        size_t new_cap = (t->cap == 0) ? MSH_JOBS_INITIAL_CAP : t->cap * 2;
        msh_job *grown = realloc(t->jobs, new_cap * sizeof *t->jobs);

        if (grown == NULL)
            return NULL;
        t->jobs = grown;
        t->cap = new_cap;
    }

    job = &t->jobs[t->count];
    job->procs = malloc(nprocs * sizeof *job->procs);
    if (job->procs == NULL)
        return NULL;
    job->command = dup_string(command);
    if (job->command == NULL) {
        free(job->procs);
        job->procs = NULL;
        return NULL;
    }

    job->id = t->next_id;
    job->pgid = pgid;
    job->nprocs = nprocs;
    job->background = background;
    job->notified = 0;
    job->stop_notified = 0;
    job->have_modes = 0;
    job->start_fd = -1;
    for (i = 0; i < nprocs; i++) {
        job->procs[i].pid = pids[i];
        job->procs[i].state = MSH_PROC_RUNNING;
        job->procs[i].status = 0;
    }

    /*
     * The table is published with SIGCHLD blocked. A child can exit before
     * this point -- a fast command often does -- and the drain must not see
     * a half-built entry, nor miss one because the entry did not exist yet.
     * Blocking here and draining later makes the order irrelevant.
     */
    msh_signals_block_sigchld(&saved);
    t->count++;
    t->next_id++;
    msh_signals_restore_mask(&saved);

    return job;
}

void msh_job_start(msh_job *job)
{
    if (job == NULL || job->start_fd < 0)
        return;
    /* Closing the write end ends the children's blocking read. */
    while (close(job->start_fd) == -1) {
        if (errno != EINTR)
            break;
    }
    job->start_fd = -1;
}

msh_job *msh_jobctl_find(msh_jobctl *jc, int id)
{
    size_t i;

    for (i = 0; i < jc->table.count; i++) {
        if (jc->table.jobs[i].id == id)
            return &jc->table.jobs[i];
    }
    return NULL;
}

msh_job *msh_jobctl_current(msh_jobctl *jc)
{
    msh_job *stopped = NULL;
    msh_job *active = NULL;
    size_t i;

    /*
     * Documented default: the most recently stopped job, else the most
     * recently started job that is still active. That is what makes a bare
     * `fg` after Ctrl+Z do the obvious thing.
     */
    for (i = 0; i < jc->table.count; i++) {
        msh_job *job = &jc->table.jobs[i];
        msh_job_state st = msh_job_state_of(job);

        if (st == MSH_JOB_STOPPED)
            stopped = job;
        if (st != MSH_JOB_DONE)
            active = job;
    }
    return (stopped != NULL) ? stopped : active;
}

size_t msh_jobctl_active_count(const msh_jobctl *jc)
{
    size_t i;
    size_t n = 0;

    for (i = 0; i < jc->table.count; i++) {
        if (msh_job_state_of(&jc->table.jobs[i]) != MSH_JOB_DONE)
            n++;
    }
    return n;
}

int msh_job_parse_spec(const char *text, int *id)
{
    const char *p = text;
    long value;
    char *end;

    if (p == NULL || *p == '\0')
        return -1;
    if (*p == '%')
        p++;
    if (*p == '\0')
        return -1;

    errno = 0;
    value = strtol(p, &end, 10);
    if (end == p || *end != '\0' || errno == ERANGE || value <= 0 ||
        value > 1000000L)
        return -1;

    *id = (int)value;
    return 0;
}

/* --- status collection -------------------------------------------------- */

/* Record one wait result against whichever job owns that pid. */
static void record_status(msh_jobctl *jc, pid_t pid, int status)
{
    size_t i;
    size_t k;

    for (i = 0; i < jc->table.count; i++) {
        msh_job *job = &jc->table.jobs[i];

        for (k = 0; k < job->nprocs; k++) {
            if (job->procs[k].pid != pid)
                continue;

            if (WIFSTOPPED(status)) {
                job->procs[k].state = MSH_PROC_STOPPED;
            } else if (WIFCONTINUED(status)) {
                job->procs[k].state = MSH_PROC_RUNNING;
            } else {
                job->procs[k].state = MSH_PROC_DONE;
                job->procs[k].status = status;
            }
            return;
        }
    }
    /* A pid we do not know: a child of a child, or already-reported. Ignore. */
}

void msh_jobctl_poll(msh_jobctl *jc)
{
    sigset_t saved;
    pid_t pid;
    int status;

    if (!msh_signals_take_sigchld())
        return;

    /*
     * Reap synchronously, with SIGCHLD blocked, so the table is never mutated
     * from a handler and no status is processed twice.
     */
    msh_signals_block_sigchld(&saved);
    for (;;) {
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid > 0) {
            record_status(jc, pid, status);
            continue;
        }
        if (pid == -1 && errno == EINTR)
            continue;
        break; /* 0 means "still running", -1/ECHILD means "none left" */
    }
    msh_signals_restore_mask(&saved);
}

static void print_job_line(const msh_job *job, const char *state_text)
{
    printf("[%d] %-8s %s\n", job->id, state_text, job->command);
}

static const char *state_text_of(const msh_job *job)
{
    switch (msh_job_state_of(job)) {
    case MSH_JOB_RUNNING:
        return "Running";
    case MSH_JOB_STOPPED:
        return "Stopped";
    case MSH_JOB_DONE:
    default:
        return "Done";
    }
}

void msh_jobctl_notify(msh_jobctl *jc)
{
    size_t i;
    size_t kept = 0;

    msh_jobctl_poll(jc);

    for (i = 0; i < jc->table.count; i++) {
        msh_job *job = &jc->table.jobs[i];
        msh_job_state st = msh_job_state_of(job);

        if (st == MSH_JOB_DONE) {
            /*
             * Documented policy: a finished BACKGROUND job is reported once,
             * just before the next prompt, and then forgotten. A finished
             * foreground job is never announced -- the user watched it run.
             */
            if (job->background && !job->notified)
                print_job_line(job, "Done");
            job_release(job);
            continue;
        }
        if (st == MSH_JOB_STOPPED && !job->stop_notified) {
            job->stop_notified = 1;
            print_job_line(job, "Stopped");
        }

        if (kept != i)
            jc->table.jobs[kept] = *job;
        kept++;
    }
    jc->table.count = kept;
    fflush(stdout);
}

void msh_jobctl_list(msh_jobctl *jc)
{
    size_t i;

    msh_jobctl_poll(jc);

    for (i = 0; i < jc->table.count; i++) {
        msh_job *job = &jc->table.jobs[i];

        /* A finished job is shown once here too, then dropped by notify. */
        print_job_line(job, state_text_of(job));
        if (msh_job_state_of(job) == MSH_JOB_DONE)
            job->notified = 1;
    }
    fflush(stdout);
}

void msh_jobctl_announce(const msh_jobctl *jc, const msh_job *job)
{
    (void)jc;
    /* The conventional form: job number, then the process group id. */
    printf("[%d] %ld\n", job->id, (long)job->pgid);
    fflush(stdout);
}

/* --- terminal ownership ------------------------------------------------- */

static void give_terminal_to(msh_jobctl *jc, pid_t pgid)
{
    sigset_t block;
    sigset_t saved;

    if (!jc->interactive || jc->term_fd < 0)
        return;

    /*
     * tcsetpgrp() from a process that is not in the foreground group raises
     * SIGTTOU. The shell ignores it, but blocking it as well is the portable
     * habit and keeps the call from being interrupted.
     */
    sigemptyset(&block);
    sigaddset(&block, SIGTTOU);
    sigprocmask(SIG_BLOCK, &block, &saved);
    (void)tcsetpgrp(jc->term_fd, pgid);
    sigprocmask(SIG_SETMASK, &saved, NULL);
}

static void restore_shell_terminal(msh_jobctl *jc)
{
    if (!jc->interactive || jc->term_fd < 0)
        return;
    give_terminal_to(jc, jc->shell_pgid);
    if (jc->have_modes)
        (void)tcsetattr(jc->term_fd, TCSADRAIN, &jc->shell_modes);
}

/* --- running a job in the foreground ------------------------------------ */

/* True while any member of the job is still running. */
static int job_has_running(const msh_job *job)
{
    size_t i;

    for (i = 0; i < job->nprocs; i++) {
        if (job->procs[i].state == MSH_PROC_RUNNING)
            return 1;
    }
    return 0;
}

int msh_jobctl_foreground(msh_jobctl *jc, msh_job *job)
{
    sigset_t saved;
    int status;

    if (job == NULL || job->nprocs == 0)
        return MSH_STATUS_INTERNAL_ERROR;

    job->background = 0;
    job->stop_notified = 0;

    give_terminal_to(jc, job->pgid);
    if (job->have_modes && jc->interactive && jc->term_fd >= 0) {
        /* Put back whatever the program had set up before it stopped. */
        (void)tcsetattr(jc->term_fd, TCSADRAIN, &job->modes);
    }

    /*
     * If the job was stopped, every member has to be told to continue --
     * the whole group, not one member.
     */
    if (msh_job_state_of(job) == MSH_JOB_STOPPED) {
        size_t i;

        for (i = 0; i < job->nprocs; i++) {
            if (job->procs[i].state == MSH_PROC_STOPPED)
                job->procs[i].state = MSH_PROC_RUNNING;
        }
        if (kill(-job->pgid, SIGCONT) == -1 && errno != ESRCH) {
            restore_shell_terminal(jc);
            fprintf(stderr, "msh: fg: cannot continue job %d: %s\n",
                    job->id, strerror(errno));
            return MSH_STATUS_INTERNAL_ERROR;
        }
    }

    /*
     * The terminal is the job's now, so the stages may run. Until this
     * point a terminal signal would have gone to the shell.
     */
    msh_job_start(job);

    /*
     * Wait with SIGCHLD blocked and reap by hand: the handler's flag is
     * irrelevant here, and blocking removes any race between a child
     * changing state and this loop deciding whether to keep waiting.
     */
    msh_signals_block_sigchld(&saved);
    while (job_has_running(job)) {
        pid_t pid;
        int wait_status;

        /*
         * With job control the whole process group is waited for. Without
         * it there are no groups of our own, so wait for any child and let
         * record_status route each result to its job.
         */
        pid = waitpid(jc->interactive ? -job->pgid : -1, &wait_status,
                      WUNTRACED);
        if (pid > 0) {
            record_status(jc, pid, wait_status);
            continue;
        }
        if (pid == -1 && errno == EINTR)
            continue;
        if (pid == -1) {
            /*
             * ECHILD: everything has already been reaped elsewhere. Treat the
             * remaining members as finished rather than spinning.
             */
            size_t i;

            for (i = 0; i < job->nprocs; i++) {
                if (job->procs[i].state == MSH_PROC_RUNNING)
                    job->procs[i].state = MSH_PROC_DONE;
            }
            break;
        }
    }
    msh_signals_restore_mask(&saved);

    if (msh_job_state_of(job) == MSH_JOB_STOPPED) {
        /*
         * Keep the job's view of the terminal so `fg` can put it back, then
         * restore the shell's. A program that changed the modes -- an editor,
         * a pager -- would otherwise leave the shell's terminal wrong.
         */
        if (jc->interactive && jc->term_fd >= 0 &&
            tcgetattr(jc->term_fd, &job->modes) == 0) {
            job->have_modes = 1;
        }
        restore_shell_terminal(jc);
        if (!job->stop_notified) {
            job->stop_notified = 1;
            printf("[%d] %-8s %s\n", job->id, "Stopped", job->command);
            fflush(stdout);
        }
        /* Conventional status for "stopped by a signal". */
        return MSH_STATUS_SIGNAL_BASE + SIGTSTP;
    }

    restore_shell_terminal(jc);
    status = msh_job_exit_status(job);
    job->notified = 1; /* a foreground job needs no completion message */
    return status;
}

int msh_jobctl_background_resume(msh_jobctl *jc, msh_job *job)
{
    size_t i;

    (void)jc;
    if (job == NULL)
        return -1;

    for (i = 0; i < job->nprocs; i++) {
        if (job->procs[i].state == MSH_PROC_STOPPED)
            job->procs[i].state = MSH_PROC_RUNNING;
    }
    job->background = 1;
    job->stop_notified = 0;

    /* The whole group, so every stage of a pipeline resumes. */
    if (kill(-job->pgid, SIGCONT) == -1 && errno != ESRCH) {
        fprintf(stderr, "msh: bg: cannot continue job %d: %s\n",
                job->id, strerror(errno));
        return -1;
    }
    return 0;
}

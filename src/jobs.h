/*
 * jobs - the job table, process groups and terminal ownership.
 *
 * A "job" is one whole pipeline, however many stages it has. That is the unit
 * the user names (`fg %2`), the unit that shares a process group, and the
 * unit a terminal signal reaches. Tracking a pipeline by a single member pid
 * would get all three wrong: Ctrl+C has to reach every stage, and a pipeline
 * is only finished when every stage is.
 *
 * Each job therefore records every one of its processes and that process's
 * own state. The job's state is derived from them, never guessed from one.
 */
#ifndef MSH_JOBS_H
#define MSH_JOBS_H

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

typedef enum {
    MSH_PROC_RUNNING,
    MSH_PROC_STOPPED,
    MSH_PROC_DONE
} msh_proc_state;

typedef struct {
    pid_t          pid;
    msh_proc_state state;
    int            status; /* the raw wait status, meaningful when DONE */
} msh_proc;

typedef enum {
    MSH_JOB_RUNNING, /* at least one process still running          */
    MSH_JOB_STOPPED, /* none running, at least one stopped          */
    MSH_JOB_DONE     /* every process finished                      */
} msh_job_state;

typedef struct {
    int       id;         /* stable, user-facing job number          */
    pid_t     pgid;       /* the process group every stage belongs to */
    char     *command;    /* owned copy of the line, for `jobs`       */
    msh_proc *procs;      /* owned, one entry per stage               */
    size_t    nprocs;
    int       background; /* started with & rather than run in front  */
    int       notified;   /* its completion has been reported         */
    int       stop_notified; /* its stop has been reported            */
    struct termios modes;    /* the job's terminal settings when it   */
    int       have_modes;    /* stopped, so fg can put them back      */
    int       start_fd;      /* the start barrier, until released     */
} msh_job;

typedef struct {
    msh_job *jobs;
    size_t   count;
    size_t   cap;
    int      next_id;
} msh_job_table;

/*
 * Everything the shell needs to own a terminal, kept together so the
 * non-interactive case can simply leave it switched off.
 */
typedef struct {
    msh_job_table  table;
    int            interactive;  /* terminal control is in use        */
    int            term_fd;      /* the controlling terminal, or -1   */
    pid_t          shell_pgid;
    struct termios shell_modes;  /* the shell's own terminal settings */
    int            have_modes;
} msh_jobctl;

/*
 * Prepare job control.
 *
 * When `interactive`, this waits until the shell really is the foreground
 * job (stopping itself with SIGTTIN if it is not, exactly as a shell started
 * in the background should), puts the shell in a process group of its own,
 * takes the terminal, and saves its terminal modes.
 *
 * When not interactive it does none of that: a shell reading a pipe or a
 * script has no business calling tcsetpgrp(), and in CI there may be no
 * controlling terminal at all. Jobs are still tracked.
 */
void msh_jobctl_init(msh_jobctl *jc, int interactive);

/* Release the table. Does not signal anything: see msh_jobctl_warn_active. */
void msh_jobctl_shutdown(msh_jobctl *jc);

/*
 * Record a newly started job. `pids` holds one pid per stage, in order; the
 * last is the stage whose status becomes the job's. Returns the new job, or
 * NULL if it could not be stored (in which case the caller still owns the
 * processes and should wait for them itself).
 */
msh_job *msh_jobctl_add(msh_jobctl *jc, pid_t pgid, const pid_t *pids,
                        size_t nprocs, const char *command, int background);

/*
 * Run `job` in the foreground: give it the terminal, continue it if it was
 * stopped, and wait until it finishes or stops.
 *
 * Returns the status the shell should record -- the last stage's status, or
 * 128 + signal if that stage was killed, or the stopped status if the job
 * stopped. The terminal always comes back to the shell before this returns,
 * and a stopped job's terminal modes are saved first so `fg` can restore
 * them.
 */
int msh_jobctl_foreground(msh_jobctl *jc, msh_job *job);

/*
 * Release the job's start barrier, letting its stages exec.
 *
 * Called once the job's process group owns the terminal (a foreground job)
 * or once it has been announced (a background one). Idempotent.
 */
void msh_job_start(msh_job *job);

/* Announce a background job in the conventional `[1] 12345` form. */
void msh_jobctl_announce(const msh_jobctl *jc, const msh_job *job);

/*
 * Collect whatever has changed: a non-blocking drain of child statuses into
 * the table. Safe to call at any time; does nothing if no SIGCHLD arrived.
 * SIGCHLD is blocked internally while the table is updated.
 */
void msh_jobctl_poll(msh_jobctl *jc);

/*
 * Report finished and newly stopped background jobs on stdout, then forget
 * the finished ones. Called just before the prompt, which is where a shell
 * conventionally tells the user about them.
 */
void msh_jobctl_notify(msh_jobctl *jc);

/* Print the job table, as the `jobs` built-in. */
void msh_jobctl_list(msh_jobctl *jc);

/* Derived state and the status a finished job should report. */
msh_job_state msh_job_state_of(const msh_job *job);
int msh_job_exit_status(const msh_job *job);

/*
 * Look a job up. `msh_jobctl_current` implements the default for `fg` and
 * `bg` with no argument: the most recently stopped job, or failing that the
 * most recently started one that is still active.
 */
msh_job *msh_jobctl_find(msh_jobctl *jc, int id);
msh_job *msh_jobctl_current(msh_jobctl *jc);

/*
 * Parse a job specification as `fg`/`bg` accept it: "%2" or plain "2".
 * Returns 0 and fills `*id` on success, -1 if it is not a job id at all.
 */
int msh_job_parse_spec(const char *text, int *id);

/* Continue a stopped job in the background: SIGCONT to the whole group. */
int msh_jobctl_background_resume(msh_jobctl *jc, msh_job *job);

/* How many jobs are running or stopped, for the exit-with-jobs policy. */
size_t msh_jobctl_active_count(const msh_jobctl *jc);

#endif /* MSH_JOBS_H */

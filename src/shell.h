/*
 * shell - shell state and the read-eval-print loop.
 *
 * The loop reads lines, treats blank input as a no-op, runs built-ins and
 * external commands, and terminates cleanly at end of input. `msh_dispatch`
 * is the single seam where evaluation attaches: built-ins are recognised
 * there, before any fork, and everything else becomes an external command.
 */
#ifndef MSH_SHELL_H
#define MSH_SHELL_H

#include "jobs.h"

#include <stdbool.h>

/*
 * Shell-wide state. Owned by main() and passed by pointer; there is no global
 * instance, so the loop stays testable and reentrant.
 *
 * Holds no heap-allocated members, so it needs no destructor. If a later
 * milestone adds one, a msh_shell_free must be introduced alongside it.
 */
typedef struct {
    bool interactive;  /* stdin is a terminal: prompt and echo a farewell */
    int  last_status;  /* exit status of the most recent command          */
    bool should_exit;  /* set by the `exit` built-in; ends the REPL loop   */
    bool exit_warned;  /* `exit` already refused once over active jobs     */
    msh_jobctl jc;     /* the job table, process groups and the terminal   */
} msh_shell;

/*
 * Initialise `sh`, detecting whether stdin is an interactive terminal, and
 * set up the signal policy and job control that follow from that.
 */
void msh_shell_init(msh_shell *sh);

/* Release what msh_shell_init acquired. */
void msh_shell_shutdown(msh_shell *sh);

/*
 * Run the read-eval-print loop until end of input, an unrecoverable input
 * error, or `should_exit`. Returns the status the process should exit with.
 */
int msh_shell_run(msh_shell *sh);

/*
 * Act on one input line (newline already stripped). `line` is borrowed: this
 * function does not take ownership and must not free it.
 *
 * Blank input is a no-op that forks nothing and preserves the previous
 * status. Anything else is split on whitespace and run as a built-in if
 * argv[0] names one, otherwise as an external command. Returns the status to
 * record in `sh->last_status`.
 */
int msh_dispatch(msh_shell *sh, const char *line);

/* True if `line` is empty or contains only blanks, tabs and other spaces. */
bool msh_line_is_blank(const char *line);

#endif /* MSH_SHELL_H */

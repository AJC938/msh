/*
 * shell - shell state and the read-eval-print loop.
 *
 * Milestone 1 scope: the loop reads lines, recognises blank input, and
 * terminates cleanly at end of input. Evaluation of commands is introduced in
 * later milestones; `msh_dispatch` is the single seam where it will attach.
 */
#ifndef MSH_SHELL_H
#define MSH_SHELL_H

#include <stdbool.h>

/*
 * Shell-wide state. Owned by main() and passed by pointer; there is no global
 * instance, so the loop stays testable and reentrant.
 */
typedef struct {
    bool interactive;  /* stdin is a terminal: prompt and echo a farewell */
    int  last_status;  /* exit status of the most recent command          */
    bool should_exit;  /* set by the `exit` built-in (milestone 5)        */
} msh_shell;

/* Initialise `sh`, detecting whether stdin is an interactive terminal. */
void msh_shell_init(msh_shell *sh);

/*
 * Run the read-eval-print loop until end of input, an unrecoverable input
 * error, or `should_exit`. Returns the status the process should exit with.
 */
int msh_shell_run(msh_shell *sh);

/*
 * Act on one input line (newline already stripped). `line` is borrowed: this
 * function does not take ownership and must not free it.
 *
 * Milestone 1 recognises only blank input, which is a no-op. Returns the
 * status to record in `sh->last_status`.
 */
int msh_dispatch(msh_shell *sh, const char *line);

/* True if `line` is empty or contains only blanks, tabs and other spaces. */
bool msh_line_is_blank(const char *line);

#endif /* MSH_SHELL_H */

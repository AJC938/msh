/*
 * redirect - install a command's redirections onto the standard descriptors.
 *
 * There are two callers with different lifetimes, and this module serves both
 * so the open() flags and the error messages exist in exactly one place:
 *
 *   - an external command redirects in the forked child, where the change is
 *     permanent and the process is about to exec;
 *   - a built-in redirects in the shell itself, where the change must be
 *     undone afterwards on every path.
 *
 * stderr is never touched: 2> is out of scope for this milestone.
 */
#ifndef MSH_REDIRECT_H
#define MSH_REDIRECT_H

#include "parser.h"

/*
 * The shell's own descriptors, saved so they can be put back.
 *
 * Always initialise with MSH_SAVED_FDS_INIT. A member is -1 when that stream
 * was not redirected and so has nothing to restore.
 */
typedef struct {
    int stdin_copy;
    int stdout_copy;
} msh_saved_fds;

#define MSH_SAVED_FDS_INIT { -1, -1 }

/*
 * Child side: apply `cmd`'s redirections permanently to this process.
 *
 * Returns 0 on success and -1 on failure, having already reported the reason
 * on stderr with write(), not stdio -- the child must not append to buffers
 * it inherited from the parent. The caller must then _exit() rather than
 * exec: a partially configured command is never run.
 */
int msh_redirect_apply_child(const msh_command *cmd);

/*
 * Parent side: apply `cmd`'s redirections to this process, saving whatever is
 * displaced into `*saved`.
 *
 * Returns 0 on success, or -1 after reporting the reason on stderr. On
 * failure anything already applied is put back before returning, so the shell
 * is never left partially redirected; `*saved` is then clean and restoring it
 * again is harmless.
 *
 * stdout is flushed before it is replaced, so output produced earlier cannot
 * land in the redirection target.
 */
int msh_redirect_apply_parent(const msh_command *cmd, msh_saved_fds *saved);

/*
 * Parent side: put the shell's descriptors back and release the copies.
 *
 * Flushes stdout first, so output the built-in produced reaches the file
 * rather than appearing on the terminal afterwards. Safe to call when nothing
 * was redirected, and safe to call twice.
 */
void msh_redirect_restore_parent(msh_saved_fds *saved);

#endif /* MSH_REDIRECT_H */

/*
 * builtins - commands that run inside the shell process.
 *
 * A built-in exists when running the command in a forked child would be
 * pointless or wrong. `cd` is the clearest case: a child changing its own
 * working directory cannot affect the shell, so `cd` must run here. `exit`
 * likewise has to end this process, not a child.
 *
 * `pwd` and `help` could be external programs, but are built in so the shell
 * is usable with no PATH at all and so `pwd` reports the shell's own idea of
 * where it is.
 */
#ifndef MSH_BUILTINS_H
#define MSH_BUILTINS_H

#include "shell.h"

typedef enum {
    MSH_BUILTIN_NOT_FOUND, /* argv[0] is not a built-in; run it externally */
    MSH_BUILTIN_HANDLED    /* it ran; *status holds the result            */
} msh_builtin_result;

/*
 * Run `argv` as a built-in if argv[0] names one.
 *
 * `argv` must be NULL-terminated with a non-NULL argv[0], and is borrowed:
 * this function neither frees nor retains it.
 *
 * Returns MSH_BUILTIN_NOT_FOUND and leaves `*status` untouched when argv[0]
 * is not a built-in. Otherwise returns MSH_BUILTIN_HANDLED and sets `*status`
 * to the status the shell should record.
 *
 * `exit` does not terminate the process here. It sets `sh->should_exit` and
 * lets the REPL unwind normally, so the line buffer is freed exactly once on
 * the ordinary path rather than being leaked by a call to exit() from deep
 * inside dispatch.
 */
msh_builtin_result msh_builtin_run(msh_shell *sh, char *const *argv,
                                   int *status);

/* True if `name` is a built-in. Exposed for tests and for documentation. */
bool msh_builtin_exists(const char *name);

#endif /* MSH_BUILTINS_H */

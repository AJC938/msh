#include "shell.h"
#include "lineread.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MSH_PROMPT "msh$ "

void msh_shell_init(msh_shell *sh)
{
    sh->interactive = isatty(STDIN_FILENO) ? true : false;
    sh->last_status = 0;
    sh->should_exit = false;
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

int msh_dispatch(msh_shell *sh, const char *line)
{
    if (msh_line_is_blank(line))
        return sh->last_status; /* a blank line changes nothing, as in sh */

    /*
     * Milestone 1 does not execute commands. Report clearly rather than
     * failing silently, and use 127 ("command not found") so the status is
     * already meaningful once execution lands in milestone 3.
     */
    fprintf(stderr, "msh: command execution is not implemented yet: %s\n", line);
    return 127;
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
    while (!sh->should_exit) {
        char *line = NULL;
        msh_read_status status;

        write_prompt(sh);
        status = msh_read_line(stdin, &line, NULL);

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
        free(line);
    }

    return sh->last_status;
}

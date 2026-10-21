#include "shell.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    msh_shell sh;

    /*
     * Milestone 1 takes no options and does not read scripts. Reject extra
     * arguments rather than ignoring them, so the contract stays honest as
     * flags and script files are added later.
     */
    if (argc > 1) {
        fprintf(stderr, "usage: %s\n", argv[0]);
        fprintf(stderr, "msh: arguments are not supported yet\n");
        return 2;
    }

    msh_shell_init(&sh);
    return msh_shell_run(&sh);
}

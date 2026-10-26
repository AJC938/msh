/*
 * parser - turn one input line into a pipeline: one or more simple commands,
 * each with its own arguments and redirections.
 *
 * It is a small explicit state machine over the line, and a pure function of
 * its input: no globals, no process handling, nothing about fork(), pipe() or
 * open(). That separation is deliberate, and it is what makes every rule
 * below testable without spawning anything or touching the filesystem.
 *
 * It does NOT use strtok(): strtok mutates its input, keeps hidden static
 * state, and cannot express quoting at all.
 *
 * ---------------------------------------------------------------------------
 * Quoting rules, in full. These are msh's own deliberately limited rules, not
 * an implementation of the POSIX shell grammar.
 * ---------------------------------------------------------------------------
 *
 * 1. Spaces and tabs outside quotes separate arguments. Leading, trailing and
 *    repeated separators produce no empty arguments.
 *
 * 2. Single quotes preserve every character up to the matching single quote.
 *    Nothing is special inside them, backslash and operators included:
 *        'a\b'   -> a\b
 *        'a|b'   -> a|b     (one ordinary argument)
 *
 * 3. Double quotes group whitespace into one argument, and operators inside
 *    them are ordinary text. A backslash is special ONLY before a double
 *    quote or another backslash:
 *        "\""    -> "
 *        "\\"    -> \
 *        "a\b"   -> a\b     (any other backslash is kept as data)
 *        "a|b"   -> a|b     (one ordinary argument)
 *
 * 4. Outside quotes a backslash escapes exactly the next character, which is
 *    then taken literally and is never an operator:
 *        a\ b    -> one argument "a b"
 *        a\|b    -> a|b     (one ordinary argument)
 *
 * 5. Adjacent quoted and unquoted segments concatenate into one argument:
 *        ab"cd"'ef'  -> abcdef
 *
 * 6. An empty quoted string is a real zero-length argument:
 *        ''  and  ""  -> one argument of length 0
 *
 * ---------------------------------------------------------------------------
 * Redirection
 * ---------------------------------------------------------------------------
 *
 * 7. Outside quotes and unescaped, these are operators rather than text:
 *        < path    read standard input from path
 *        > path    write standard output to path, creating or truncating it
 *        >> path   write standard output to path, creating or appending
 *
 *    An operator ends the word being built, so no whitespace is required
 *    around it: `echo hello>out` works. The target is the next word, and the
 *    ordinary quoting rules apply to it. Redirections may appear in either
 *    order, and each belongs to the stage it appears in.
 *
 *    At most one input and one output redirection per stage. stderr is never
 *    redirected: 2> is out of scope.
 *
 * ---------------------------------------------------------------------------
 * Pipelines
 * ---------------------------------------------------------------------------
 *
 * 8. Outside quotes and unescaped, `|` separates pipeline stages:
 *        printf 'hi\n' | cat
 *        cat f | grep e | wc -l
 *
 *    Like the redirection operators it ends the word being built, so
 *    `a|b` is two stages. Every stage must name a command, and each stage
 *    carries its own arguments and its own redirections.
 *
 *    There is no limit on the number of stages other than memory and the
 *    process and descriptor limits of the system.
 *
 * 9. These are syntax errors, reported without producing a pipeline, and
 *    detected before anything is executed and before any file is opened --
 *    so a malformed line never creates or truncates its target and never
 *    launches a partial pipeline:
 *        - a single or double quote that is never closed
 *        - a backslash as the last character of the line
 *        - an operator with no target: `cat <`, `echo hi >`, `ls > >`
 *        - two input redirections, or two output redirections, in one stage
 *        - a redirection with no command: `> out`
 *        - a pipeline stage with no command: `| cat`, `cat |`, `cat || grep`,
 *          `cat | | grep`
 *
 *    `||` is not an operator here. It is simply two pipes with nothing
 *    between them, which is the "empty stage" error. `;` and `&&` are not
 *    operators either and are passed through as ordinary text -- `&&` is
 *    two background operators in a row, which is the error below.
 *
 * 9b. Outside quotes and unescaped, a trailing `|` -- sorry, `&` -- runs the
 *    whole pipeline in the background:
 *        sleep 10 &
 *        cat f | grep e | wc -l &
 *
 *    `&` must be the LAST thing on the line and must follow a command.
 *    Anything after it, including a second `&`, is a syntax error; msh does
 *    not implement command lists or `&&`.
 *
 * 10. Not implemented in this milestone, and therefore NOT special:
 *     background execution (&), command lists (;), the logical operators
 *     (&& ||), stderr redirection (2>), heredocs (<<), variable expansion
 *     ($VAR, $?), command substitution, arithmetic, globbing, tilde
 *     expansion, and comments (#).
 */
#ifndef MSH_PARSER_H
#define MSH_PARSER_H

#include <stddef.h>

/* Which malformed construct ended the parse. */
typedef enum {
    MSH_SYNTAX_NONE,
    MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE,
    MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE,
    MSH_SYNTAX_TRAILING_BACKSLASH,
    MSH_SYNTAX_MISSING_REDIRECT_TARGET,
    MSH_SYNTAX_DUPLICATE_INPUT,
    MSH_SYNTAX_DUPLICATE_OUTPUT,
    MSH_SYNTAX_MISSING_COMMAND,
    MSH_SYNTAX_EMPTY_PIPELINE_STAGE,
    MSH_SYNTAX_BACKGROUND_NOT_LAST
} msh_syntax_error;

/* How an output redirection opens its file. */
typedef enum {
    MSH_OUT_NONE,     /* no output redirection */
    MSH_OUT_TRUNCATE, /* >   create or truncate */
    MSH_OUT_APPEND    /* >>  create or append   */
} msh_out_mode;

/*
 * One simple command: a single pipeline stage, fully owned.
 *
 * `argv` is NULL-terminated so it can be handed straight to execvp(); `argc`
 * counts the real entries, excluding the terminator. A stage in a successful
 * parse always has argc >= 1.
 *
 * `infile` and `outfile` are NULL when the stage has no such redirection, and
 * otherwise point to owned strings. `outmode` is MSH_OUT_NONE exactly when
 * `outfile` is NULL.
 */
typedef struct {
    char       **argv;
    size_t       argc;
    char        *infile;
    char        *outfile;
    msh_out_mode outmode;
} msh_command;

/*
 * A whole line: one or more stages joined by `|`, fully owned.
 *
 * `stage_count` is at least 1 for a successful parse. A line with no pipe is
 * simply a pipeline of one stage, so callers have a single shape to handle.
 *
 * `error` is meaningful only when the parse returned MSH_PARSE_SYNTAX.
 */
typedef struct {
    msh_command     *stages;
    size_t           stage_count;
    int              background; /* the line ended in an unquoted & */
    msh_syntax_error error;
} msh_pipeline;

typedef enum {
    MSH_PARSE_OK,     /* a pipeline; *out owns every stage and string */
    MSH_PARSE_EMPTY,  /* blank or whitespace only; *out owns nothing  */
    MSH_PARSE_SYNTAX, /* malformed input; *out owns nothing           */
    MSH_PARSE_NOMEM   /* allocation failure; *out owns nothing        */
} msh_parse_status;

/*
 * Parse `line` into `*out` according to the rules above.
 *
 * On MSH_PARSE_OK the caller owns the result and must release it with
 * msh_pipeline_free(). On every other outcome nothing is owned and `out` is
 * left zeroed, so msh_pipeline_free() is safe to call regardless and a caller
 * that always frees stays correct. In particular a syntax error leaks
 * nothing: stages already completed, the stage being built, and any
 * redirection path already stored are all released before returning.
 *
 * A NULL `line` is treated as empty. A NULL `out` returns MSH_PARSE_NOMEM,
 * since there is nowhere to report anything.
 */
msh_parse_status msh_parse(const char *line, msh_pipeline *out);

/*
 * Release every stage and the stage array, then zero `pl`. Safe on an
 * already-zeroed or already-freed value, and safe on NULL.
 */
void msh_pipeline_free(msh_pipeline *pl);

/*
 * Release one stage's strings and vector, then zero it. Exposed because the
 * executor and the tests work with individual stages.
 */
void msh_command_free(msh_command *cmd);

/* A short human-readable description of `error`, for a diagnostic. Never
 * returns NULL. */
const char *msh_syntax_error_text(msh_syntax_error error);

#endif /* MSH_PARSER_H */

#include "parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Initial sizes. Every structure grows geometrically, so these affect only
 * the common case and never correctness.
 */
#define MSH_WORD_INITIAL_CAP   32 /* bytes in one argument */
#define MSH_ARGV_INITIAL_CAP    8 /* slots in one stage's vector */
#define MSH_STAGE_INITIAL_CAP   4 /* stages in a pipeline */

/* --- the growable buffer for one word ----------------------------------- */

struct wordbuf {
    char  *data;
    size_t len;
    size_t cap;
    int    started; /* a word exists, even with len 0: this is what makes
                     * '' and "" produce a real empty argument */
};

static void wordbuf_init(struct wordbuf *w)
{
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    w->started = 0;
}

static void wordbuf_reset(struct wordbuf *w)
{
    free(w->data);
    wordbuf_init(w);
}

/* Ensure room for `extra` more bytes plus the terminating NUL. */
static int wordbuf_reserve(struct wordbuf *w, size_t extra)
{
    size_t need = w->len + extra + 1;
    size_t new_cap;
    char *grown;

    if (need <= w->cap)
        return 0;

    new_cap = (w->cap == 0) ? MSH_WORD_INITIAL_CAP : w->cap;
    while (new_cap < need) {
        if (new_cap > ((size_t)-1) / 2)
            return -1; /* refuse rather than wrap around */
        new_cap *= 2;
    }

    grown = realloc(w->data, new_cap);
    if (grown == NULL)
        return -1;

    w->data = grown;
    w->cap = new_cap;
    return 0;
}

static int wordbuf_push(struct wordbuf *w, char c)
{
    if (wordbuf_reserve(w, 1) != 0)
        return -1;
    w->data[w->len] = c;
    w->len++;
    w->started = 1;
    return 0;
}

/* Mark a word as existing without adding a character, for '' and "". */
static int wordbuf_begin(struct wordbuf *w)
{
    if (wordbuf_reserve(w, 0) != 0)
        return -1;
    w->started = 1;
    return 0;
}

/*
 * Hand the accumulated bytes over as a NUL-terminated string and reset the
 * buffer. Ownership moves to the caller; NULL means allocation failure, and
 * the buffer is left intact for wordbuf_reset to release.
 */
static char *wordbuf_take(struct wordbuf *w)
{
    char *out;

    if (wordbuf_reserve(w, 0) != 0)
        return NULL;

    w->data[w->len] = '\0';
    out = w->data;
    wordbuf_init(w); /* the caller owns `out` now; do not free it here */
    return out;
}

/* --- one stage ---------------------------------------------------------- */

static void command_init(msh_command *cmd)
{
    cmd->argv = NULL;
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->outmode = MSH_OUT_NONE;
}

void msh_command_free(msh_command *cmd)
{
    size_t i;

    if (cmd == NULL)
        return;

    if (cmd->argv != NULL) {
        for (i = 0; i < cmd->argc; i++)
            free(cmd->argv[i]);
        free(cmd->argv);
    }
    free(cmd->infile);
    free(cmd->outfile);

    command_init(cmd);
}

/*
 * Append `word` (ownership transferred on success) to a stage and
 * re-terminate the vector. On failure the caller still owns `word`.
 */
static int argv_push(msh_command *cmd, size_t *cap, char *word)
{
    if (cmd->argc + 2 > *cap) {
        size_t new_cap = (*cap == 0) ? MSH_ARGV_INITIAL_CAP : *cap;
        char **grown;

        while (cmd->argc + 2 > new_cap) {
            if (new_cap > (((size_t)-1) / 2) / sizeof *cmd->argv)
                return -1;
            new_cap *= 2;
        }

        grown = realloc(cmd->argv, new_cap * sizeof *cmd->argv);
        if (grown == NULL)
            return -1;

        cmd->argv = grown;
        *cap = new_cap;
    }

    cmd->argv[cmd->argc] = word;
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return 0;
}

/* --- the pipeline ------------------------------------------------------- */

void msh_pipeline_free(msh_pipeline *pl)
{
    size_t i;

    if (pl == NULL)
        return;

    if (pl->stages != NULL) {
        for (i = 0; i < pl->stage_count; i++)
            msh_command_free(&pl->stages[i]);
        free(pl->stages);
    }

    pl->stages = NULL;
    pl->stage_count = 0;
    pl->background = 0;
    pl->error = MSH_SYNTAX_NONE;
}

/*
 * Move the finished stage into the pipeline. Ownership transfers on success
 * and `cur` is reset; on failure the caller still owns `cur`.
 */
static int pipeline_push(msh_pipeline *pl, size_t *cap, msh_command *cur)
{
    if (pl->stage_count + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? MSH_STAGE_INITIAL_CAP : *cap;
        msh_command *grown;

        while (pl->stage_count + 1 > new_cap) {
            if (new_cap > (((size_t)-1) / 2) / sizeof *pl->stages)
                return -1;
            new_cap *= 2;
        }

        grown = realloc(pl->stages, new_cap * sizeof *pl->stages);
        if (grown == NULL)
            return -1;

        pl->stages = grown;
        *cap = new_cap;
    }

    pl->stages[pl->stage_count] = *cur; /* shallow move: cur no longer owns */
    pl->stage_count++;
    command_init(cur);
    return 0;
}

const char *msh_syntax_error_text(msh_syntax_error error)
{
    switch (error) {
    case MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE:
        return "unmatched '";
    case MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE:
        return "unmatched \"";
    case MSH_SYNTAX_TRAILING_BACKSLASH:
        return "trailing backslash";
    case MSH_SYNTAX_MISSING_REDIRECT_TARGET:
        return "missing redirection target";
    case MSH_SYNTAX_DUPLICATE_INPUT:
        return "more than one input redirection";
    case MSH_SYNTAX_DUPLICATE_OUTPUT:
        return "more than one output redirection";
    case MSH_SYNTAX_MISSING_COMMAND:
        return "redirection without a command";
    case MSH_SYNTAX_EMPTY_PIPELINE_STAGE:
        return "missing command around |";
    case MSH_SYNTAX_BACKGROUND_NOT_LAST:
        return "& must follow a command and end the line";
    case MSH_SYNTAX_NONE:
    default:
        return "no error";
    }
}

/* --- the scanner -------------------------------------------------------- */

typedef enum {
    STATE_NORMAL, /* outside any quote */
    STATE_SINGLE, /* inside '...'      */
    STATE_DOUBLE  /* inside "..."      */
} scan_state;

/*
 * What the next completed word belongs to. A redirection operator sets this;
 * the word that follows consumes it.
 */
typedef enum {
    PENDING_NONE,
    PENDING_INPUT,
    PENDING_OUTPUT_TRUNCATE,
    PENDING_OUTPUT_APPEND
} pending_target;

/* Separators outside quotes. */
static int is_separator(char c)
{
    return isspace((unsigned char)c) ? 1 : 0;
}

msh_parse_status msh_parse(const char *line, msh_pipeline *out)
{
    struct wordbuf word;
    msh_command cur;
    size_t argv_cap = 0;
    size_t stage_cap = 0;
    scan_state state = STATE_NORMAL;
    pending_target pending = PENDING_NONE;
    int saw_pipe = 0;
    int background = 0;
    const char *p;

    if (out == NULL)
        return MSH_PARSE_NOMEM;

    out->stages = NULL;
    out->stage_count = 0;
    out->background = 0;
    out->error = MSH_SYNTAX_NONE;

    if (line == NULL)
        return MSH_PARSE_EMPTY;

    wordbuf_init(&word);
    command_init(&cur);

/*
 * Release everything built so far -- the word being assembled, the stage
 * being assembled, and every completed stage -- and leave `out` owning
 * nothing.
 */
#define FAIL(status, syntax)                                \
    do {                                                    \
        wordbuf_reset(&word);                               \
        msh_command_free(&cur);                             \
        msh_pipeline_free(out);                             \
        out->error = (syntax);                              \
        return (status);                                    \
    } while (0)

/*
 * Move the finished word where it belongs: an ordinary argument of the
 * current stage, or the target of the operator that preceded it.
 */
#define FINISH_WORD()                                                       \
    do {                                                                    \
        char *taken = wordbuf_take(&word);                                  \
                                                                            \
        if (taken == NULL)                                                  \
            FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);                         \
        if (pending == PENDING_NONE) {                                      \
            if (argv_push(&cur, &argv_cap, taken) != 0) {                   \
                free(taken);                                                \
                FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);                     \
            }                                                               \
        } else if (pending == PENDING_INPUT) {                              \
            if (cur.infile != NULL) {                                       \
                free(taken);                                                \
                FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_DUPLICATE_INPUT);         \
            }                                                               \
            cur.infile = taken;                                             \
            pending = PENDING_NONE;                                         \
        } else {                                                            \
            if (cur.outfile != NULL) {                                      \
                free(taken);                                                \
                FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_DUPLICATE_OUTPUT);        \
            }                                                               \
            cur.outfile = taken;                                            \
            cur.outmode = (pending == PENDING_OUTPUT_APPEND)                \
                              ? MSH_OUT_APPEND                              \
                              : MSH_OUT_TRUNCATE;                           \
            pending = PENDING_NONE;                                         \
        }                                                                   \
    } while (0)

/*
 * Close the stage being assembled and move it into the pipeline. A stage
 * must name a command: a redirection on its own is MISSING_COMMAND, and
 * nothing at all beside a `|` is an empty stage.
 */
#define FINISH_STAGE()                                                      \
    do {                                                                    \
        if (cur.argc == 0) {                                                \
            if (cur.infile != NULL || cur.outfile != NULL)                  \
                FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_MISSING_COMMAND);         \
            FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_EMPTY_PIPELINE_STAGE);        \
        }                                                                   \
        if (pipeline_push(out, &stage_cap, &cur) != 0)                      \
            FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);                         \
        argv_cap = 0;                                                       \
    } while (0)

    for (p = line; *p != '\0'; p++) {
        char c = *p;

        switch (state) {
        case STATE_NORMAL:
            /*
             * `&` must end the line, so nothing but blanks may follow it.
             * This is what rejects `cmd & other` and `cmd && next`.
             */
            if (background && !is_separator(c))
                FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_BACKGROUND_NOT_LAST);
            if (c == '\\') {
                if (p[1] == '\0')
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_TRAILING_BACKSLASH);
                /* The next character is literal, and never an operator. */
                p++;
                if (wordbuf_push(&word, *p) != 0)
                    FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
            } else if (c == '\'') {
                if (wordbuf_begin(&word) != 0)
                    FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
                state = STATE_SINGLE;
            } else if (c == '"') {
                if (wordbuf_begin(&word) != 0)
                    FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
                state = STATE_DOUBLE;
            } else if (c == '|') {
                /*
                 * A pipe ends the word and the stage. `||` is not an
                 * operator: it is simply a second pipe with nothing between,
                 * which FINISH_STAGE reports as an empty stage.
                 */
                if (word.started)
                    FINISH_WORD();
                if (pending != PENDING_NONE)
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_MISSING_REDIRECT_TARGET);
                FINISH_STAGE();
                saw_pipe = 1;
            } else if (c == '&') {
                /*
                 * Background the whole pipeline. Like the other operators
                 * it ends the word being built; unlike them it must be the
                 * last thing on the line.
                 */
                if (word.started)
                    FINISH_WORD();
                if (pending != PENDING_NONE)
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_MISSING_REDIRECT_TARGET);
                if (cur.argc == 0 && cur.infile == NULL &&
                    cur.outfile == NULL && !saw_pipe) {
                    /* `&` with nothing at all before it. */
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_BACKGROUND_NOT_LAST);
                }
                FINISH_STAGE();
                background = 1;
            } else if (c == '<' || c == '>') {
                /*
                 * An operator ends the current word, which is what makes
                 * `echo hello>out` work without surrounding whitespace.
                 */
                if (word.started)
                    FINISH_WORD();
                if (pending != PENDING_NONE) {
                    /* `> >` and friends: the first operator never got a
                     * target. */
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_MISSING_REDIRECT_TARGET);
                }
                if (c == '<') {
                    pending = PENDING_INPUT;
                } else if (p[1] == '>') {
                    pending = PENDING_OUTPUT_APPEND;
                    p++; /* consume the second '>' */
                } else {
                    pending = PENDING_OUTPUT_TRUNCATE;
                }
            } else if (is_separator(c)) {
                if (word.started)
                    FINISH_WORD();
            } else {
                if (wordbuf_push(&word, c) != 0)
                    FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
            }
            break;

        case STATE_SINGLE:
            /* Everything is literal until the closing quote, operators
             * included. */
            if (c == '\'') {
                state = STATE_NORMAL;
            } else if (wordbuf_push(&word, c) != 0) {
                FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
            }
            break;

        case STATE_DOUBLE:
            if (c == '"') {
                state = STATE_NORMAL;
            } else if (c == '\\') {
                char next = p[1];

                if (next == '\0')
                    FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_TRAILING_BACKSLASH);
                if (next == '"' || next == '\\') {
                    /* The only two sequences that mean something here. */
                    p++;
                    if (wordbuf_push(&word, next) != 0)
                        FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
                } else {
                    /* Keep the backslash as data and let `next` be handled
                     * normally on the following iteration. */
                    if (wordbuf_push(&word, '\\') != 0)
                        FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
                }
            } else if (wordbuf_push(&word, c) != 0) {
                FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
            }
            break;

        default:
            /* Unreachable; refuse rather than guess. */
            FAIL(MSH_PARSE_NOMEM, MSH_SYNTAX_NONE);
        }
    }

    if (state == STATE_SINGLE)
        FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE);
    if (state == STATE_DOUBLE)
        FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE);

    if (word.started)
        FINISH_WORD();

    wordbuf_reset(&word); /* nothing left, but keep ownership explicit */

    /* An operator that never received its target. */
    if (pending != PENDING_NONE)
        FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_MISSING_REDIRECT_TARGET);

    if (cur.argc == 0 && cur.infile == NULL && cur.outfile == NULL) {
        /*
         * Nothing in the final stage. After `&` that is expected: the stage
         * was already closed there. After a pipe it is an empty stage
         * (`cat |`); with neither it means the line was blank.
         */
        if (background) {
            out->background = 1;
            return MSH_PARSE_OK;
        }
        if (saw_pipe)
            FAIL(MSH_PARSE_SYNTAX, MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
        msh_pipeline_free(out);
        return MSH_PARSE_EMPTY;
    }

    FINISH_STAGE();
    out->background = background;

    return MSH_PARSE_OK;

#undef FINISH_STAGE
#undef FINISH_WORD
#undef FAIL
}

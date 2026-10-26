/*
 * Unit tests for the quote-aware parser: words, redirection and pipelines.
 *
 * Executor behaviour lives in test_executor.c, built-ins in test_builtins.c
 * and descriptor handling in test_redirect.c; this file is about turning a
 * line into a pipeline of commands.
 */
#include "parser.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

/* Parse and require exactly one stage, returning it through `out`. The
 * pipeline still owns everything; the caller frees it. */
static int parse_one_stage(const char *line, msh_pipeline *pl,
                           const msh_command **out)
{
    msh_parse_status st = msh_parse(line, pl);

    if (st != MSH_PARSE_OK) {
        fprintf(stderr, "  FAIL: parse(<%s>) returned %d, wanted OK\n",
                line, (int)st);
        msh_test_failures++;
        return 0;
    }
    if (pl->stage_count != 1) {
        fprintf(stderr, "  FAIL: parse(<%s>) gave %zu stages, wanted 1\n",
                line, pl->stage_count);
        msh_test_failures++;
        msh_pipeline_free(pl);
        return 0;
    }
    *out = &pl->stages[0];
    return 1;
}

/* Compare one stage's argv against a NULL-terminated expectation. */
static void check_argv(const char *line, const msh_command *c,
                       const char *const *expected, size_t stage)
{
    size_t n = 0;
    size_t i;

    while (expected[n] != NULL)
        n++;

    if (c->argc != n) {
        fprintf(stderr, "  FAIL: <%s> stage %zu: %zu words, wanted %zu\n",
                line, stage, c->argc, n);
        msh_test_failures++;
    }
    for (i = 0; i < n && i < c->argc; i++) {
        if (c->argv[i] == NULL || strcmp(c->argv[i], expected[i]) != 0) {
            fprintf(stderr, "  FAIL: <%s> stage %zu word %zu = <%s>, wanted <%s>\n",
                    line, stage, i, c->argv[i] == NULL ? "(null)" : c->argv[i],
                    expected[i]);
            msh_test_failures++;
        }
    }
    /* execvp() requires the terminator, so this is not a detail. */
    if (c->argv != NULL)
        CHECK(c->argv[c->argc] == NULL);
}

/* A single-stage line with no redirections. */
static void expect_words(const char *line, const char *const *expected)
{
    msh_pipeline pl;
    const msh_command *c;

    if (!parse_one_stage(line, &pl, &c))
        return;

    CHECK(pl.error == MSH_SYNTAX_NONE);
    check_argv(line, c, expected, 0);
    CHECK(c->infile == NULL);
    CHECK(c->outfile == NULL);
    CHECK(c->outmode == MSH_OUT_NONE);

    msh_pipeline_free(&pl);
    CHECK(pl.stages == NULL);
    CHECK(pl.stage_count == 0);
    msh_pipeline_free(&pl); /* must be safe twice */
}

/* A single-stage line with the given redirections. */
static void expect_command(const char *line, const char *const *expected,
                           const char *in, const char *out,
                           msh_out_mode mode)
{
    msh_pipeline pl;
    const msh_command *c;

    if (!parse_one_stage(line, &pl, &c))
        return;

    check_argv(line, c, expected, 0);

    if (in == NULL) {
        CHECK(c->infile == NULL);
    } else if (c->infile == NULL || strcmp(c->infile, in) != 0) {
        fprintf(stderr, "  FAIL: <%s> infile = <%s>, wanted <%s>\n", line,
                c->infile == NULL ? "(null)" : c->infile, in);
        msh_test_failures++;
    }

    if (out == NULL) {
        CHECK(c->outfile == NULL);
        CHECK(c->outmode == MSH_OUT_NONE);
    } else if (c->outfile == NULL || strcmp(c->outfile, out) != 0) {
        fprintf(stderr, "  FAIL: <%s> outfile = <%s>, wanted <%s>\n", line,
                c->outfile == NULL ? "(null)" : c->outfile, out);
        msh_test_failures++;
    } else {
        CHECK(c->outmode == mode);
    }

    msh_pipeline_free(&pl);
}

/* Assert a syntax error of a particular kind, owning nothing afterwards. */
static void expect_syntax(const char *line, msh_syntax_error want)
{
    msh_pipeline pl;
    msh_parse_status st = msh_parse(line, &pl);

    if (st != MSH_PARSE_SYNTAX) {
        fprintf(stderr, "  FAIL: parse(<%s>) returned %d, wanted SYNTAX\n",
                line, (int)st);
        msh_test_failures++;
        msh_pipeline_free(&pl);
        return;
    }
    if (pl.error != want) {
        fprintf(stderr, "  FAIL: parse(<%s>) error %d, wanted %d\n",
                line, (int)pl.error, (int)want);
        msh_test_failures++;
    }
    /* A failed parse must own nothing: no stages, words or paths left. */
    CHECK(pl.stages == NULL);
    CHECK(pl.stage_count == 0);
    CHECK(msh_syntax_error_text(pl.error) != NULL);
    msh_pipeline_free(&pl); /* safe although nothing was allocated */
}

static void expect_empty(const char *line)
{
    msh_pipeline pl;

    if (msh_parse(line, &pl) != MSH_PARSE_EMPTY) {
        fprintf(stderr, "  FAIL: parse(<%s>) was not EMPTY\n",
                line == NULL ? "(null)" : line);
        msh_test_failures++;
    }
    CHECK(pl.stages == NULL);
    CHECK(pl.stage_count == 0);
    msh_pipeline_free(&pl);
}

/* --- unquoted basics ---------------------------------------------------- */

static void test_simple_command(void)
{
    const char *const want[] = { "ls", NULL };
    expect_words("ls", want);
}

static void test_command_with_arguments(void)
{
    const char *const want[] = { "/bin/echo", "hello", "world", NULL };
    expect_words("/bin/echo hello world", want);
}

static void test_argument_order_preserved(void)
{
    const char *const want[] = { "cmd", "first", "second", "third", NULL };
    expect_words("cmd first second third", want);
}

static void test_leading_and_trailing_blanks(void)
{
    const char *const want[] = { "ls", "-l", NULL };
    expect_words("   ls -l   ", want);
    expect_words("\tls\t-l\t", want);
}

static void test_repeated_separators_collapse(void)
{
    const char *const want[] = { "a", "b", "c", NULL };
    expect_words("a     b\t\tc", want);
}

static void test_empty_input(void)
{
    expect_empty("");
    expect_empty("   ");
    expect_empty("\t\t");
    expect_empty(NULL);
}

static void test_null_out_is_rejected(void)
{
    CHECK(msh_parse("ls", NULL) == MSH_PARSE_NOMEM);
}

/* --- quoting ------------------------------------------------------------ */

static void test_single_quotes_group_and_preserve(void)
{
    const char *const spaces[] = { "echo", "hello world", NULL };
    const char *const literal[] = { "echo", "a\\b", NULL };
    const char *const dollar[] = { "echo", "$HOME", NULL };

    expect_words("echo 'hello world'", spaces);
    expect_words("echo 'a\\b'", literal);
    expect_words("echo '$HOME'", dollar);
}

static void test_double_quotes_group(void)
{
    const char *const spaces[] = { "echo", "hello world", NULL };
    const char *const squote[] = { "echo", "it's", NULL };

    expect_words("echo \"hello world\"", spaces);
    expect_words("echo \"it's\"", squote);
}

static void test_backslash_inside_double_quotes(void)
{
    const char *const esc_quote[] = { "echo", "a\"b", NULL };
    const char *const esc_slash[] = { "echo", "a\\b", NULL };
    const char *const kept[] = { "echo", "a\\nb", NULL };

    expect_words("echo \"a\\\"b\"", esc_quote);
    expect_words("echo \"a\\\\b\"", esc_slash);
    expect_words("echo \"a\\nb\"", kept);
}

static void test_backslash_outside_quotes(void)
{
    const char *const spaced[] = { "a b", NULL };
    const char *const slash[] = { "echo", "\\", NULL };

    expect_words("a\\ b", spaced);
    expect_words("echo \\\\", slash);
}

static void test_empty_quoted_arguments(void)
{
    const char *const single[] = { "echo", "", NULL };
    const char *const joined[] = { "echo", "ab", NULL };

    expect_words("echo ''", single);
    expect_words("echo \"\"", single);
    expect_words("echo a''b", joined);
}

static void test_adjacent_segments_concatenate(void)
{
    const char *const mixed[] = { "abcdef", NULL };
    const char *const spacey[] = { "a b c", NULL };

    expect_words("ab\"cd\"'ef'", mixed);
    expect_words("'a '\"b \"c", spacey);
}

static void test_quote_syntax_errors(void)
{
    expect_syntax("echo 'abc", MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE);
    expect_syntax("echo \"abc", MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE);
    expect_syntax("echo a\\", MSH_SYNTAX_TRAILING_BACKSLASH);
}

/* --- redirection -------------------------------------------------------- */

static void test_input_redirection(void)
{
    const char *const want[] = { "cat", NULL };

    expect_command("cat < in.txt", want, "in.txt", NULL, MSH_OUT_NONE);
    expect_command("cat <in.txt", want, "in.txt", NULL, MSH_OUT_NONE);
    expect_command("< in.txt cat", want, "in.txt", NULL, MSH_OUT_NONE);
}

static void test_output_truncate_and_append(void)
{
    const char *const want[] = { "echo", "hi", NULL };

    expect_command("echo hi > out.txt", want, NULL, "out.txt",
                   MSH_OUT_TRUNCATE);
    expect_command("echo hi >out.txt", want, NULL, "out.txt",
                   MSH_OUT_TRUNCATE);
    expect_command("echo hi >> out.txt", want, NULL, "out.txt",
                   MSH_OUT_APPEND);
    expect_command("echo hi>>out.txt", want, NULL, "out.txt",
                   MSH_OUT_APPEND);
}

static void test_operators_adjacent_to_arguments(void)
{
    const char *const echo_hello[] = { "echo", "hello", NULL };
    const char *const just_a[] = { "a", NULL };

    expect_command("echo hello>output.txt", echo_hello, NULL, "output.txt",
                   MSH_OUT_TRUNCATE);
    expect_command("a>b", just_a, NULL, "b", MSH_OUT_TRUNCATE);
    expect_command("a<b", just_a, "b", NULL, MSH_OUT_NONE);
}

static void test_both_redirections_in_either_order(void)
{
    const char *const want[] = { "sort", NULL };

    expect_command("sort < in > out", want, "in", "out", MSH_OUT_TRUNCATE);
    expect_command("sort > out < in", want, "in", "out", MSH_OUT_TRUNCATE);
    expect_command("< in sort >> out", want, "in", "out", MSH_OUT_APPEND);
}

static void test_quoted_redirection_targets(void)
{
    const char *const cat[] = { "cat", NULL };
    const char *const echo[] = { "echo", "hello world", NULL };

    expect_command("cat < \"input file.txt\"", cat, "input file.txt", NULL,
                   MSH_OUT_NONE);
    expect_command("cat < 'input file.txt'", cat, "input file.txt", NULL,
                   MSH_OUT_NONE);
    expect_command("cat < in\\ file.txt", cat, "in file.txt", NULL,
                   MSH_OUT_NONE);
    expect_command("echo \"hello world\" > \"output file.txt\"", echo, NULL,
                   "output file.txt", MSH_OUT_TRUNCATE);
}

static void test_quoted_operators_are_literal(void)
{
    const char *const gt[] = { "echo", ">", NULL };
    const char *const agtb[] = { "echo", "a>b", NULL };
    const char *const lt[] = { "echo", "<", NULL };

    expect_words("echo \">\"", gt);
    expect_words("echo 'a>b'", agtb);
    expect_words("echo a\\>b", agtb);
    expect_words("echo \"<\"", lt);
    expect_words("echo \\>", gt);
}

static void test_redirection_syntax_errors(void)
{
    expect_syntax("cat <", MSH_SYNTAX_MISSING_REDIRECT_TARGET);
    expect_syntax("echo hello >", MSH_SYNTAX_MISSING_REDIRECT_TARGET);
    expect_syntax("echo hello >>", MSH_SYNTAX_MISSING_REDIRECT_TARGET);
    expect_syntax("ls > >", MSH_SYNTAX_MISSING_REDIRECT_TARGET);
    expect_syntax("cat < a < b", MSH_SYNTAX_DUPLICATE_INPUT);
    expect_syntax("echo hi > a > b", MSH_SYNTAX_DUPLICATE_OUTPUT);
    expect_syntax("echo hi > a >> b", MSH_SYNTAX_DUPLICATE_OUTPUT);
    expect_syntax("> out", MSH_SYNTAX_MISSING_COMMAND);
    expect_syntax("< in", MSH_SYNTAX_MISSING_COMMAND);
    expect_syntax("cat < \"in file", MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE);
}

/* --- pipelines: the new syntax ------------------------------------------ */

/* Assert a whole pipeline: stage count, then each stage's argv. */
static void expect_stages(const char *line, size_t count,
                          const char *const *const *expected)
{
    msh_pipeline pl;
    size_t i;

    if (msh_parse(line, &pl) != MSH_PARSE_OK) {
        fprintf(stderr, "  FAIL: parse(<%s>) did not return OK\n", line);
        msh_test_failures++;
        return;
    }
    if (pl.stage_count != count) {
        fprintf(stderr, "  FAIL: <%s> gave %zu stages, wanted %zu\n",
                line, pl.stage_count, count);
        msh_test_failures++;
        msh_pipeline_free(&pl);
        return;
    }
    for (i = 0; i < count; i++)
        check_argv(line, &pl.stages[i], expected[i], i);

    msh_pipeline_free(&pl);
}

static void test_two_stage_pipeline(void)
{
    const char *const s0[] = { "printf", "hi", NULL };
    const char *const s1[] = { "cat", NULL };
    const char *const *const all[] = { s0, s1 };

    expect_stages("printf hi | cat", 2, all);
    expect_stages("printf hi|cat", 2, all);          /* no spaces needed */
    expect_stages("  printf hi   |   cat  ", 2, all); /* blanks around    */
    expect_stages("printf hi\t|\tcat", 2, all);
}

static void test_three_stage_pipeline(void)
{
    const char *const s0[] = { "cat", "f", NULL };
    const char *const s1[] = { "grep", "e", NULL };
    const char *const s2[] = { "wc", "-l", NULL };
    const char *const *const all[] = { s0, s1, s2 };

    expect_stages("cat f | grep e | wc -l", 3, all);
}

static void test_many_stage_pipeline(void)
{
    msh_pipeline pl;
    char line[512];
    size_t pos = 0;
    size_t i;

    for (i = 0; i < 16; i++) {
        int n = snprintf(line + pos, sizeof line - pos, "%scat",
                         i == 0 ? "" : " | ");
        if (n < 0)
            break;
        pos += (size_t)n;
    }

    if (msh_parse(line, &pl) != MSH_PARSE_OK) {
        msh_test_failures++;
        return;
    }
    CHECK(pl.stage_count == 16);
    for (i = 0; i < pl.stage_count; i++) {
        CHECK(pl.stages[i].argc == 1);
        if (pl.stages[i].argc == 1)
            CHECK(strcmp(pl.stages[i].argv[0], "cat") == 0);
    }
    msh_pipeline_free(&pl);
}

/* A line with no pipe is a pipeline of one stage, so callers have one shape. */
static void test_single_command_is_a_one_stage_pipeline(void)
{
    const char *const s0[] = { "ls", "-l", NULL };
    const char *const *const all[] = { s0 };

    expect_stages("ls -l", 1, all);
}

/* Each stage keeps its own redirections. */
static void test_pipeline_stages_keep_their_redirections(void)
{
    msh_pipeline pl;

    if (msh_parse("cat < in | grep e | wc -l > out", &pl) != MSH_PARSE_OK) {
        msh_test_failures++;
        return;
    }
    CHECK(pl.stage_count == 3);
    if (pl.stage_count == 3) {
        /* first stage: input only */
        CHECK(pl.stages[0].infile != NULL);
        if (pl.stages[0].infile != NULL)
            CHECK(strcmp(pl.stages[0].infile, "in") == 0);
        CHECK(pl.stages[0].outfile == NULL);

        /* middle stage: neither */
        CHECK(pl.stages[1].infile == NULL);
        CHECK(pl.stages[1].outfile == NULL);

        /* last stage: output only */
        CHECK(pl.stages[2].infile == NULL);
        CHECK(pl.stages[2].outfile != NULL);
        if (pl.stages[2].outfile != NULL)
            CHECK(strcmp(pl.stages[2].outfile, "out") == 0);
        CHECK(pl.stages[2].outmode == MSH_OUT_TRUNCATE);
    }
    msh_pipeline_free(&pl);
}

static void test_redirection_in_the_middle_of_a_pipeline(void)
{
    msh_pipeline pl;

    if (msh_parse("a > f | b", &pl) != MSH_PARSE_OK) {
        msh_test_failures++;
        return;
    }
    CHECK(pl.stage_count == 2);
    if (pl.stage_count == 2) {
        CHECK(pl.stages[0].outfile != NULL);
        CHECK(pl.stages[1].outfile == NULL);
    }
    msh_pipeline_free(&pl);
}

/* A pipe is syntax only when unquoted and unescaped. */
static void test_quoted_and_escaped_pipes_are_literal(void)
{
    const char *const dq[] = { "echo", "a|b", NULL };
    const char *const bar[] = { "echo", "|", NULL };
    const char *const doublebar[] = { "echo", "||", NULL };

    expect_words("echo \"a|b\"", dq);
    expect_words("echo 'a|b'", dq);
    expect_words("echo a\\|b", dq);
    expect_words("echo \"|\"", bar);
    expect_words("echo '|'", bar);
    expect_words("echo \\|", bar);
    expect_words("echo '||'", doublebar);
}

/* A quoted pipe must not be confused with a real one on the same line. */
static void test_quoted_and_real_pipe_together(void)
{
    const char *const s0[] = { "echo", "a|b", NULL };
    const char *const s1[] = { "cat", NULL };
    const char *const *const all[] = { s0, s1 };

    expect_stages("echo 'a|b' | cat", 2, all);
}

static void test_pipeline_syntax_errors(void)
{
    expect_syntax("| cat", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("cat |", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("cat | ", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("|", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("||", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    /* `||` is not an operator here: it is two pipes with nothing between. */
    expect_syntax("cat || grep", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("cat | | grep", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("cat | grep |", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
    expect_syntax("  |  cat  ", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
}

/* A stage with a redirection but no command is still a missing command. */
static void test_pipeline_stage_with_only_a_redirection(void)
{
    expect_syntax("cat | > out", MSH_SYNTAX_MISSING_COMMAND);
    expect_syntax("> out | cat", MSH_SYNTAX_MISSING_COMMAND);
}

/* A redirection fault inside a pipeline is still caught, before anything
 * runs and with every earlier stage released. */
static void test_redirection_errors_inside_a_pipeline(void)
{
    expect_syntax("cat | grep e >", MSH_SYNTAX_MISSING_REDIRECT_TARGET);
    expect_syntax("cat < a | grep e < b < c", MSH_SYNTAX_DUPLICATE_INPUT);
    expect_syntax("cat | grep 'unterminated", MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE);
    expect_syntax("cat | grep e\\", MSH_SYNTAX_TRAILING_BACKSLASH);
}

static void test_syntax_error_text_is_distinct(void)
{
    const msh_syntax_error all[] = {
        MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE,
        MSH_SYNTAX_UNMATCHED_DOUBLE_QUOTE,
        MSH_SYNTAX_TRAILING_BACKSLASH,
        MSH_SYNTAX_MISSING_REDIRECT_TARGET,
        MSH_SYNTAX_DUPLICATE_INPUT,
        MSH_SYNTAX_DUPLICATE_OUTPUT,
        MSH_SYNTAX_MISSING_COMMAND,
        MSH_SYNTAX_EMPTY_PIPELINE_STAGE
    };
    size_t n = sizeof all / sizeof all[0];
    size_t i;
    size_t j;

    for (i = 0; i < n; i++) {
        const char *a = msh_syntax_error_text(all[i]);

        CHECK(a != NULL);
        CHECK(strlen(a) > 0);
        for (j = i + 1; j < n; j++)
            CHECK(strcmp(a, msh_syntax_error_text(all[j])) != 0);
    }
    CHECK(msh_syntax_error_text(MSH_SYNTAX_NONE) != NULL);
}

/* --- operators still NOT implemented ------------------------------------ */

static void test_other_operators_are_plain_text(void)
{
    const char *const semi[] = { "a", ";", "b", NULL };

    /*
     * `;` is still ordinary text: msh has no command lists. `&` and `&&` used
     * to be text too, until this milestone made `&` an operator -- see the
     * background tests below.
     */
    expect_words("a ; b", semi);
}

/* --- background: the new operator --------------------------------------- */

/* Parse a background line and check its stages. */
static void expect_background(const char *line, size_t stages,
                              const char *const *const *expected)
{
    msh_pipeline pl;
    size_t i;

    if (msh_parse(line, &pl) != MSH_PARSE_OK) {
        fprintf(stderr, "  FAIL: parse(<%s>) did not return OK\n", line);
        msh_test_failures++;
        return;
    }
    if (!pl.background) {
        fprintf(stderr, "  FAIL: <%s> was not marked background\n", line);
        msh_test_failures++;
    }
    if (pl.stage_count != stages) {
        fprintf(stderr, "  FAIL: <%s> gave %zu stages, wanted %zu\n",
                line, pl.stage_count, stages);
        msh_test_failures++;
        msh_pipeline_free(&pl);
        return;
    }
    for (i = 0; i < stages; i++)
        check_argv(line, &pl.stages[i], expected[i], i);
    msh_pipeline_free(&pl);
}

static void test_background_single_command(void)
{
    const char *const s0[] = { "sleep", "10", NULL };
    const char *const *const all[] = { s0 };

    expect_background("sleep 10 &", 1, all);
    expect_background("sleep 10&", 1, all);     /* no space needed */
    expect_background("sleep 10 &   ", 1, all); /* blanks may follow */
}

static void test_background_pipeline(void)
{
    const char *const s0[] = { "cat", "f", NULL };
    const char *const s1[] = { "grep", "e", NULL };
    const char *const s2[] = { "wc", "-l", NULL };
    const char *const *const all[] = { s0, s1, s2 };

    /* The & backgrounds the whole pipeline, not just its last stage. */
    expect_background("cat f | grep e | wc -l &", 3, all);
}

static void test_background_with_redirection(void)
{
    msh_pipeline pl;

    if (msh_parse("sort < in > out &", &pl) != MSH_PARSE_OK) {
        msh_test_failures++;
        return;
    }
    CHECK(pl.background);
    CHECK(pl.stage_count == 1);
    if (pl.stage_count == 1) {
        CHECK(pl.stages[0].infile != NULL);
        CHECK(pl.stages[0].outfile != NULL);
    }
    msh_pipeline_free(&pl);
}

/* Without & the flag must stay clear, or every command would detach. */
static void test_foreground_is_not_background(void)
{
    msh_pipeline pl;

    if (msh_parse("sleep 10", &pl) == MSH_PARSE_OK) {
        CHECK(!pl.background);
        msh_pipeline_free(&pl);
    } else {
        msh_test_failures++;
    }
    if (msh_parse("a | b", &pl) == MSH_PARSE_OK) {
        CHECK(!pl.background);
        msh_pipeline_free(&pl);
    } else {
        msh_test_failures++;
    }
}

/* A quoted or escaped ampersand is an ordinary character. */
static void test_quoted_and_escaped_ampersands(void)
{
    const char *const amp[] = { "echo", "&", NULL };
    const char *const ab[] = { "echo", "a&b", NULL };
    const char *const dbl[] = { "echo", "&&", NULL };

    expect_words("echo \"&\"", amp);
    expect_words("echo '&'", amp);
    expect_words("echo \\&", amp);
    expect_words("echo 'a&b'", ab);
    expect_words("echo \"a&b\"", ab);
    expect_words("echo a\\&b", ab);
    expect_words("echo '&&'", dbl);
}

/*
 * `&` must end the line and must follow a command. This is what rejects the
 * command lists and logical operators msh does not implement, rather than
 * silently running half of them.
 */
static void test_background_syntax_errors(void)
{
    expect_syntax("cmd & other", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("cmd && next", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("a & b & c", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("&", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("  &  ", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("cmd & | other", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    expect_syntax("cmd & > out", MSH_SYNTAX_BACKGROUND_NOT_LAST);
    /* An empty stage is still an empty stage. */
    expect_syntax("cat | &", MSH_SYNTAX_EMPTY_PIPELINE_STAGE);
}

/* --- size limits -------------------------------------------------------- */

static void test_many_words(void)
{
    msh_pipeline pl;
    char line[2048];
    size_t i;
    size_t pos = 0;

    for (i = 0; i < 128; i++) {
        int n = snprintf(line + pos, sizeof line - pos, "%sw%zu",
                         i == 0 ? "" : " ", i);
        if (n < 0)
            break;
        pos += (size_t)n;
    }

    if (msh_parse(line, &pl) != MSH_PARSE_OK) {
        msh_test_failures++;
        return;
    }
    CHECK(pl.stage_count == 1);
    if (pl.stage_count == 1) {
        CHECK(pl.stages[0].argc == 128);
        if (pl.stages[0].argc == 128) {
            CHECK(strcmp(pl.stages[0].argv[0], "w0") == 0);
            CHECK(strcmp(pl.stages[0].argv[127], "w127") == 0);
            CHECK(pl.stages[0].argv[128] == NULL);
        }
    }
    msh_pipeline_free(&pl);
}

static void test_very_long_argument(void)
{
    size_t n = 100000;
    char *line = malloc(n + 8);
    msh_pipeline pl;

    if (line == NULL) {
        msh_test_failures++;
        return;
    }
    memcpy(line, "echo ", 5);
    memset(line + 5, 'x', n);
    line[5 + n] = '\0';

    if (msh_parse(line, &pl) == MSH_PARSE_OK) {
        CHECK(pl.stage_count == 1);
        if (pl.stage_count == 1) {
            CHECK(pl.stages[0].argc == 2);
            if (pl.stages[0].argc == 2)
                CHECK(strlen(pl.stages[0].argv[1]) == n);
        }
        msh_pipeline_free(&pl);
    } else {
        msh_test_failures++;
    }
    free(line);
}

/*
 * A long pipeline that fails in its last stage must release every earlier
 * stage. Under ASan and Valgrind this is where a leaky error path shows up,
 * and it is the one the stage array adds.
 */
static void test_late_pipeline_failure_cleans_up(void)
{
    size_t stages = 200;
    size_t cap = stages * 8 + 32;
    char *line = malloc(cap);
    size_t pos = 0;
    size_t i;

    if (line == NULL) {
        msh_test_failures++;
        return;
    }
    for (i = 0; i < stages; i++) {
        int n = snprintf(line + pos, cap - pos, "%scat", i == 0 ? "" : " | ");

        if (n < 0)
            break;
        pos += (size_t)n;
    }
    /* A dangling quote after 200 completed stages. */
    snprintf(line + pos, cap - pos, " | grep 'oops");
    expect_syntax(line, MSH_SYNTAX_UNMATCHED_SINGLE_QUOTE);

    /* And an empty final stage after the same 200. */
    snprintf(line + pos, cap - pos, " | ");
    expect_syntax(line, MSH_SYNTAX_EMPTY_PIPELINE_STAGE);

    free(line);
}

int main(void)
{
    printf("test_parser\n");
    RUN(test_simple_command);
    RUN(test_command_with_arguments);
    RUN(test_argument_order_preserved);
    RUN(test_leading_and_trailing_blanks);
    RUN(test_repeated_separators_collapse);
    RUN(test_empty_input);
    RUN(test_null_out_is_rejected);
    RUN(test_single_quotes_group_and_preserve);
    RUN(test_double_quotes_group);
    RUN(test_backslash_inside_double_quotes);
    RUN(test_backslash_outside_quotes);
    RUN(test_empty_quoted_arguments);
    RUN(test_adjacent_segments_concatenate);
    RUN(test_quote_syntax_errors);
    RUN(test_input_redirection);
    RUN(test_output_truncate_and_append);
    RUN(test_operators_adjacent_to_arguments);
    RUN(test_both_redirections_in_either_order);
    RUN(test_quoted_redirection_targets);
    RUN(test_quoted_operators_are_literal);
    RUN(test_redirection_syntax_errors);
    RUN(test_two_stage_pipeline);
    RUN(test_three_stage_pipeline);
    RUN(test_many_stage_pipeline);
    RUN(test_single_command_is_a_one_stage_pipeline);
    RUN(test_pipeline_stages_keep_their_redirections);
    RUN(test_redirection_in_the_middle_of_a_pipeline);
    RUN(test_quoted_and_escaped_pipes_are_literal);
    RUN(test_quoted_and_real_pipe_together);
    RUN(test_pipeline_syntax_errors);
    RUN(test_pipeline_stage_with_only_a_redirection);
    RUN(test_redirection_errors_inside_a_pipeline);
    RUN(test_syntax_error_text_is_distinct);
    RUN(test_other_operators_are_plain_text);
    RUN(test_background_single_command);
    RUN(test_background_pipeline);
    RUN(test_background_with_redirection);
    RUN(test_foreground_is_not_background);
    RUN(test_quoted_and_escaped_ampersands);
    RUN(test_background_syntax_errors);
    RUN(test_many_words);
    RUN(test_very_long_argument);
    RUN(test_late_pipeline_failure_cleans_up);
    return TEST_SUMMARY();
}

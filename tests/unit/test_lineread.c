/* Unit tests for the unbounded line reader.
 *
 * tmpfile() is used rather than fmemopen() so the tests build on any
 * conforming C implementation, not just glibc. */
#include "lineread.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

/* Build a stream holding `bytes` and rewind it for reading. */
static FILE *stream_of(const char *bytes, size_t len)
{
    FILE *f = tmpfile();
    if (f == NULL) {
        perror("tmpfile");
        exit(2);
    }
    if (len > 0 && fwrite(bytes, 1, len, f) != len) {
        perror("fwrite");
        exit(2);
    }
    rewind(f);
    return f;
}

static FILE *stream_of_str(const char *s)
{
    return stream_of(s, strlen(s));
}

static void test_reads_single_line_and_strips_newline(void)
{
    FILE *f = stream_of_str("echo hello\n");
    char *line = NULL;
    size_t len = 0;

    CHECK(msh_read_line(f, &line, &len) == MSH_READ_OK);
    CHECK(line != NULL);
    if (line != NULL) {
        CHECK(strcmp(line, "echo hello") == 0);
        CHECK(len == 10);
    }
    free(line);
    fclose(f);
}

static void test_blank_line_is_empty_string(void)
{
    FILE *f = stream_of_str("\n");
    char *line = NULL;

    CHECK(msh_read_line(f, &line, NULL) == MSH_READ_OK);
    if (line != NULL)
        CHECK(strcmp(line, "") == 0);
    free(line);
    fclose(f);
}

static void test_eof_on_empty_stream(void)
{
    FILE *f = stream_of_str("");
    char *line = NULL;
    size_t len = 123; /* must be reset by the call */

    CHECK(msh_read_line(f, &line, &len) == MSH_READ_EOF);
    CHECK(line == NULL);
    CHECK(len == 0);
    fclose(f);
}

/* A final line with no newline is returned, then EOF: this is the piped
 * equivalent of typing text and pressing Ctrl+D. */
static void test_unterminated_final_line_then_eof(void)
{
    FILE *f = stream_of_str("pwd");
    char *line = NULL;

    CHECK(msh_read_line(f, &line, NULL) == MSH_READ_OK);
    if (line != NULL)
        CHECK(strcmp(line, "pwd") == 0);
    free(line);

    line = NULL;
    CHECK(msh_read_line(f, &line, NULL) == MSH_READ_EOF);
    CHECK(line == NULL);
    fclose(f);
}

static void test_reads_successive_lines(void)
{
    FILE *f = stream_of_str("one\ntwo\n\nthree\n");
    const char *expected[] = { "one", "two", "", "three" };
    char *line = NULL;
    size_t i;

    for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        CHECK(msh_read_line(f, &line, NULL) == MSH_READ_OK);
        if (line != NULL)
            CHECK(strcmp(line, expected[i]) == 0);
        free(line);
        line = NULL;
    }
    CHECK(msh_read_line(f, &line, NULL) == MSH_READ_EOF);
    fclose(f);
}

/* The reader must not rely on a fixed-size buffer: 64 KiB is far past the
 * 128-byte initial capacity, so this exercises repeated growth. */
static void test_very_long_line_grows_buffer(void)
{
    size_t n = 65536;
    char *input = malloc(n + 2);
    char *line = NULL;
    size_t len = 0;
    FILE *f;

    if (input == NULL) {
        fprintf(stderr, "  FAIL: out of memory building input\n");
        msh_test_failures++;
        return;
    }
    memset(input, 'x', n);
    input[n] = '\n';
    input[n + 1] = '\0';

    f = stream_of(input, n + 1);
    CHECK(msh_read_line(f, &line, &len) == MSH_READ_OK);
    CHECK(len == n);
    if (line != NULL) {
        CHECK(strlen(line) == n);
        CHECK(line[0] == 'x' && line[n - 1] == 'x');
    }
    free(line);
    free(input);
    fclose(f);
}

static void test_crlf_is_tolerated(void)
{
    FILE *f = stream_of_str("ls -l\r\n");
    char *line = NULL;
    size_t len = 0;

    CHECK(msh_read_line(f, &line, &len) == MSH_READ_OK);
    if (line != NULL) {
        CHECK(strcmp(line, "ls -l") == 0);
        CHECK(len == 5);
    }
    free(line);
    fclose(f);
}

/* Embedded NUL is preserved in the buffer and reported through out_len, even
 * though string functions see a shorter value. */
static void test_embedded_nul_reported_in_length(void)
{
    FILE *f = stream_of("a\0b\n", 4);
    char *line = NULL;
    size_t len = 0;

    CHECK(msh_read_line(f, &line, &len) == MSH_READ_OK);
    CHECK(len == 3);
    if (line != NULL) {
        CHECK(line[0] == 'a' && line[1] == '\0' && line[2] == 'b');
        CHECK(strlen(line) == 1);
    }
    free(line);
    fclose(f);
}

static void test_rejects_bad_arguments(void)
{
    char *line = NULL;
    CHECK(msh_read_line(NULL, &line, NULL) == MSH_READ_ERROR);
    CHECK(line == NULL);
    CHECK(msh_read_line(stdin, NULL, NULL) == MSH_READ_ERROR);
}

int main(void)
{
    printf("test_lineread\n");
    RUN(test_reads_single_line_and_strips_newline);
    RUN(test_blank_line_is_empty_string);
    RUN(test_eof_on_empty_stream);
    RUN(test_unterminated_final_line_then_eof);
    RUN(test_reads_successive_lines);
    RUN(test_very_long_line_grows_buffer);
    RUN(test_crlf_is_tolerated);
    RUN(test_embedded_nul_reported_in_length);
    RUN(test_rejects_bad_arguments);
    return TEST_SUMMARY();
}

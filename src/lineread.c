#include "lineread.h"

#include <stdlib.h>
#include <string.h>

/*
 * Initial capacity of the line buffer. Chosen to hold an ordinary command
 * without a realloc while staying small; growth is geometric, so the value
 * only affects the common case, never correctness.
 */
#define MSH_LINE_INITIAL_CAP 128

/* Grow *buf to at least `need` bytes. Returns 0 on success, -1 on failure.
 * On failure the original buffer is left intact for the caller to free. */
static int grow_to(char **buf, size_t *cap, size_t need)
{
    size_t new_cap = *cap;
    char *resized;

    while (new_cap < need) {
        /* Guard against overflow before doubling. */
        if (new_cap > (size_t)-1 / 2)
            return -1;
        new_cap *= 2;
    }
    if (new_cap == *cap)
        return 0;

    resized = realloc(*buf, new_cap);
    if (resized == NULL)
        return -1;

    *buf = resized;
    *cap = new_cap;
    return 0;
}

msh_read_status msh_read_line(FILE *stream, char **out, size_t *out_len)
{
    char *buf;
    size_t cap = MSH_LINE_INITIAL_CAP;
    size_t len = 0;
    int c;

    if (out == NULL)
        return MSH_READ_ERROR;
    *out = NULL;
    if (out_len != NULL)
        *out_len = 0;
    if (stream == NULL)
        return MSH_READ_ERROR;

    buf = malloc(cap);
    if (buf == NULL)
        return MSH_READ_ERROR;

    while ((c = fgetc(stream)) != EOF) {
        if (c == '\n')
            break;
        /* +1 keeps room for the terminating NUL written after the loop. */
        if (grow_to(&buf, &cap, len + 1 + 1) != 0) {
            free(buf);
            return MSH_READ_ERROR;
        }
        buf[len++] = (char)c;
    }

    if (c == EOF) {
        /* Distinguish a real stream error from a clean end of input. Note
         * that data already read is still worth returning: only report EOF
         * when nothing was pending. */
        if (ferror(stream)) {
            free(buf);
            return MSH_READ_ERROR;
        }
        if (len == 0) {
            free(buf);
            return MSH_READ_EOF;
        }
    }

    /* Tolerate CRLF input (files authored on Windows, piped input). */
    if (len > 0 && buf[len - 1] == '\r')
        len--;

    buf[len] = '\0';
    *out = buf;
    if (out_len != NULL)
        *out_len = len;
    return MSH_READ_OK;
}

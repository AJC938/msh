/*
 * lineread - read a single logical input line of unbounded length.
 *
 * This module is deliberately free of shell policy: it knows how to turn a
 * byte stream into one heap-allocated, NUL-terminated line, and nothing else.
 * Keeping it separate is what makes input handling unit-testable without a
 * terminal.
 */
#ifndef MSH_LINEREAD_H
#define MSH_LINEREAD_H

#include <stdio.h>

/* Outcome of an attempt to read one line. */
typedef enum {
    MSH_READ_OK,    /* a line was read; *out owns a NUL-terminated buffer */
    MSH_READ_EOF,   /* end of input with no pending data; *out is NULL     */
    MSH_READ_ERROR  /* stream error or allocation failure; *out is NULL    */
} msh_read_status;

/*
 * Read one line from `stream`.
 *
 * On MSH_READ_OK, *out points to a heap buffer the caller must free(); the
 * trailing newline (and a preceding carriage return, so CRLF input behaves)
 * is stripped, so a blank line yields an empty string rather than "\n".
 *
 * A final line not terminated by a newline is returned as MSH_READ_OK; the
 * following call then reports MSH_READ_EOF. This is what makes Ctrl+D after
 * partial input behave the way a shell user expects.
 *
 * On MSH_READ_EOF and MSH_READ_ERROR, *out is set to NULL and there is
 * nothing for the caller to free.
 *
 * Embedded NUL bytes are preserved in the buffer but, as with any C string,
 * truncate the value as seen by string functions. `out_len`, when non-NULL,
 * receives the true byte count so callers can detect this.
 */
msh_read_status msh_read_line(FILE *stream, char **out, size_t *out_len);

#endif /* MSH_LINEREAD_H */

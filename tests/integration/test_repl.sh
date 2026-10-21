#!/bin/sh
# Integration tests for the msh REPL, driven through pipes and files.
#
# IMPORTANT: piped stdin is NOT a terminal, so these tests cover the
# non-interactive path only. Ctrl+D at a real terminal is covered separately
# by tests/integration/test_pty.c (`make test-pty`, Linux only).

set -u

MSH_BIN=${MSH_BIN:-out/msh}
failures=0
tests=0

if [ ! -x "$MSH_BIN" ] && [ ! -x "$MSH_BIN.exe" ]; then
    echo "test_repl: cannot find executable at $MSH_BIN" >&2
    exit 2
fi

# check_status NAME EXPECTED_STATUS ACTUAL_STATUS
check_status() {
    tests=$((tests + 1))
    if [ "$2" = "$3" ]; then
        printf '  %-48s ok\n' "$1"
    else
        printf '  %-48s FAILED (want status %s, got %s)\n' "$1" "$2" "$3"
        failures=$((failures + 1))
    fi
}

check_empty() {
    tests=$((tests + 1))
    if [ -s "$2" ]; then
        printf '  %-48s FAILED (expected no output)\n' "$1"
        echo "    got: $(cat "$2")"
        failures=$((failures + 1))
    else
        printf '  %-48s ok\n' "$1"
    fi
}

check_contains() {
    tests=$((tests + 1))
    if grep -q "$3" "$2" 2>/dev/null; then
        printf '  %-48s ok\n' "$1"
    else
        printf '  %-48s FAILED (missing %s)\n' "$1" "$3"
        failures=$((failures + 1))
    fi
}

tmpout=$(mktemp) || exit 2
tmperr=$(mktemp) || exit 2
trap 'rm -f "$tmpout" "$tmperr"' EXIT INT TERM

echo "test_repl (non-interactive, piped stdin)"

# Immediate end of input must terminate cleanly, not spin forever.
printf '' | "$MSH_BIN" >"$tmpout" 2>"$tmperr"
check_status "eof on empty input exits 0" 0 $?
check_empty  "eof on empty input prints nothing" "$tmpout"

# Blank and whitespace-only lines are no-ops and must not change the status.
printf '\n\n   \n\t\n\n' | "$MSH_BIN" >"$tmpout" 2>"$tmperr"
check_status "blank lines only exit 0" 0 $?
check_empty  "blank lines produce no stdout" "$tmpout"
check_empty  "blank lines produce no stderr" "$tmperr"

# No prompt may be written when stdin is not a terminal.
printf '\n' | "$MSH_BIN" >"$tmpout" 2>/dev/null
check_empty "no prompt when stdin is not a tty" "$tmpout"

# A final line without a trailing newline is still a line (piped analogue of
# typing text then pressing Ctrl+D), so it reaches dispatch: status 127.
printf 'somecommand' | "$MSH_BIN" >"$tmpout" 2>"$tmperr"
check_status "unterminated final line is dispatched" 127 $?
check_contains "not-implemented message on stderr" "$tmperr" "not implemented"

# Status of the last line is the shell's exit status.
printf 'somecommand\n\n' | "$MSH_BIN" >"$tmpout" 2>"$tmperr"
check_status "blank line after command keeps status" 127 $?

# A line far longer than the initial buffer must not overflow or crash.
awk 'BEGIN { for (i = 0; i < 200000; i++) printf "a" ; printf "\n" }' \
    | "$MSH_BIN" >"$tmpout" 2>"$tmperr"
check_status "200000-char line handled without crash" 127 $?

# Unsupported arguments are rejected rather than ignored.
"$MSH_BIN" --nope </dev/null >"$tmpout" 2>"$tmperr"
check_status "arguments rejected with status 2" 2 $?
check_contains "usage printed on stderr" "$tmperr" "usage"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_repl: $tests checks passed"
    exit 0
fi
echo "test_repl: $failures of $tests checks FAILED" >&2
exit 1

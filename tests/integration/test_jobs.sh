#!/bin/sh
# Background execution and the job table, driven through pipes.
#
# stdin is not a terminal here, so msh deliberately does NOT take terminal
# control: no tcsetpgrp, no process groups of its own. That is itself one of
# the things worth testing -- a shell in a pipeline or in CI must not try to
# own a terminal it does not have. Ctrl+C, Ctrl+Z, fg and bg need a real
# terminal and live in test_pty_jobs.c instead.
#
# Every msh run is wrapped in a timeout, and a timeout is a failure: a shell
# that waited for a background job would otherwise hang the suite rather than
# fail it.

set -u

MSH_BIN=${MSH_BIN:-out/msh}

if [ ! -x "$MSH_BIN" ]; then
    echo "test_jobs: cannot find executable at $MSH_BIN" >&2
    exit 2
fi

case "$MSH_BIN" in
    /*) MSH_ABS="$MSH_BIN" ;;
    *)  MSH_ABS="$(pwd)/$MSH_BIN" ;;
esac

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT="timeout 20"
    GUARDED=yes
else
    TIMEOUT=""
    GUARDED=no
fi

failures=0
tests=0

tmpout=$(mktemp) || exit 2
tmperr=$(mktemp) || exit 2
W=$(mktemp -d) || exit 2
trap 'rm -rf "$tmpout" "$tmperr" "$W"' EXIT INT TERM

printf '#!/bin/sh\nfor a in "$@"; do printf "[%%s]" "$a"; done; echo\n' > "$W/args"
chmod 755 "$W/args"
ARGS="$W/args"

pass() { tests=$((tests + 1)); printf '  %-56s ok\n' "$1"; }
fail() {
    tests=$((tests + 1))
    failures=$((failures + 1))
    printf '  %-56s FAILED (%s)\n' "$1" "$2"
}

run() {
    printf '%s' "$1" | $TIMEOUT "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
}

timed_out() { [ "$st" -eq 124 ] && [ "$GUARDED" = yes ]; }

expect_out() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT"; return; fi
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "stdout [$got], wanted [$2]"
    fi
}

expect_status() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT"; return; fi
    if [ "$st" -eq "$2" ]; then pass "$1"; else
        fail "$1" "want status $2, got $st"
        sed -n '1,2p' "$tmperr" | sed 's/^/      stderr: /'
    fi
}

expect_match() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT"; return; fi
    if grep -qE "$2" "$tmpout"; then pass "$1"; else
        fail "$1" "stdout lacks /$2/: [$(cat "$tmpout")]"
    fi
}

# expect_line NAME REGEX INPUT -- stdout must contain a matching LINE. Used
# wherever a background announcement ("[1] 1234") shares stdout with the
# output being checked.
expect_line() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT"; return; fi
    if grep -qE "$2" "$tmpout"; then pass "$1"; else
        fail "$1" "stdout has no line matching /$2/: [$(tr '\n' ' ' < "$tmpout")]"
    fi
}

expect_err() {
    run "$3"
    if grep -qE "$2" "$tmperr"; then pass "$1"; else
        fail "$1" "stderr lacks /$2/: [$(cat "$tmperr")]"
    fi
}

echo "test_jobs (background execution and the job table)"
if [ "$GUARDED" = no ]; then
    echo "  NOTE: 'timeout' is not installed; a hang would stall this suite"
fi

# --- a background job does not block the shell ---------------------------
# The whole point: a 30-second sleep must not delay the next command. The
# timeout wrapper turns "it blocked" into a failure rather than a long wait.
expect_line "a background job does not block the next command" '^after$' \
    "/bin/sleep 30 &
/bin/echo after
"
expect_status "backgrounding succeeds immediately" 0 \
    "/bin/sleep 30 &
"

# The announcement carries a job number and a process-group id.
expect_match "a background launch is announced" '^\[1\] [0-9]+$' \
    "/bin/sleep 30 &
"

# --- jobs -----------------------------------------------------------------
expect_match "jobs lists a running background job" '^\[1\] Running' \
    "/bin/sleep 30 &
jobs
"
expect_match "jobs shows the command text" 'sleep 30' \
    "/bin/sleep 30 &
jobs
"
expect_out "jobs prints nothing when there are none" "" \
    "jobs
"
expect_status "jobs rejects extra arguments" 1 \
    "jobs x
"
expect_match "two background jobs get distinct ids" '^\[2\] Running' \
    "/bin/sleep 30 &
/bin/sleep 30 &
jobs
"

# --- completion is reported and reaped ------------------------------------
# The job finishes while the shell waits for the next line; the report has to
# appear before a later prompt, and the child must be reaped.
expect_match "a finished background job is reported Done" 'Done' \
    "/bin/true &
/bin/sleep 0.3
jobs
"

# --- background pipelines -------------------------------------------------
expect_line "a background pipeline does not block" '^after$' \
    "/bin/sleep 30 | /bin/cat &
/bin/echo after
"
expect_match "a background pipeline is announced" '^\[1\] [0-9]+$' \
    "/bin/cat /etc/hostname | /usr/bin/wc -l &
"
expect_match "a background pipeline appears in jobs" 'Running' \
    "/bin/sleep 30 | /bin/cat &
jobs
"

# --- background built-ins run in a child ---------------------------------
# cd in the background must not move the shell, and exit must not end it.
expect_line "a backgrounded cd does not move the shell" '^/tmp$' \
    "cd /tmp
cd / &
/bin/sleep 0.2
pwd
"
expect_line "a backgrounded exit does not end the shell" '^still here$' \
    "exit &
/bin/sleep 0.2
/bin/echo still here
"

# --- quoted and escaped ampersands ---------------------------------------
expect_out "a quoted & is an argument"          "[&]"   "$ARGS \"&\"
"
expect_out "a single-quoted & is an argument"   "[&]"   "$ARGS '&'
"
expect_out "an escaped & is an argument"        "[&]"   "$ARGS \\&
"
expect_out "a quoted a&b is one argument"       "[a&b]" "$ARGS 'a&b'
"
expect_out "a quoted && is an argument"         "[&&]"  "$ARGS '&&'
"

# --- malformed & ----------------------------------------------------------
expect_status "& followed by a command is a syntax error" 2 \
    "/bin/echo a & /bin/echo b
"
expect_status "&& is a syntax error" 2 \
    "/bin/true && /bin/true
"
expect_status "a bare & is a syntax error" 2 \
    "&
"
expect_err "the & error is reported" "syntax error" \
    "/bin/echo a & b
"
expect_out "a malformed & runs no part of the line" "" \
    "/bin/echo visible & /bin/echo other
"
expect_out "the shell survives a malformed &" "alive" \
    "/bin/echo a & b
/bin/echo alive
"

# --- fg and bg argument handling (no terminal here) ----------------------
# Without a terminal these cannot do their real work, but they must still
# validate their arguments and never corrupt the table or crash.
expect_status "fg with no jobs fails cleanly" 1 \
    "fg
"
expect_err "fg with no jobs says so" "no current job" \
    "fg
"
expect_status "bg with no jobs fails cleanly" 1 \
    "bg
"
expect_status "fg with a bad job id fails cleanly" 1 \
    "fg %99
"
expect_err "fg names the missing job" "no such job" \
    "fg %99
"
expect_status "fg with a non-numeric spec fails cleanly" 1 \
    "fg abc
"
expect_err "fg rejects a non-job argument" "not a job id" \
    "fg abc
"
expect_status "fg with too many arguments fails cleanly" 1 \
    "fg %1 %2
"
expect_status "bg with too many arguments fails cleanly" 1 \
    "bg %1 %2
"
expect_out "the shell is usable after fg and bg errors" "alive" \
    "fg
bg %99
fg abc
/bin/echo alive
"
expect_status "bg on a running job is refused" 1 \
    "/bin/sleep 30 &
bg %1
"

# --- exit with active jobs ------------------------------------------------
# Documented policy: refuse once, honour the second attempt.
expect_err "exit warns while a job is running" "jobs still running" \
    "/bin/sleep 30 &
exit
"
# The refused exit is itself a failed command, so it sets the last status to
# 1; a following bare `exit` carries that, exactly as `exit` always does.
expect_status "a second exit leaves anyway" 1 \
    "/bin/sleep 30 &
exit
exit
"
expect_status "an explicit status on the second exit is honoured" 3 \
    "/bin/sleep 30 &
exit
exit 3
"
expect_line "a command between exits clears the warning" '^alive$' \
    "/bin/sleep 30 &
exit
/bin/echo alive
exit
"

# --- no terminal control when not interactive ----------------------------
# A shell reading a pipe must not try to own a terminal. If it did, the
# tcsetpgrp would fail or it would stop itself with SIGTTIN and this would
# hang or report an error.
run "/bin/echo plain
"
if [ "$st" -eq 0 ] && [ "$(cat "$tmpout")" = "plain" ] && [ ! -s "$tmperr" ]; then
    pass "non-interactive mode touches no terminal"
else
    fail "non-interactive mode touches no terminal" \
         "status $st, stderr [$(cat "$tmperr")]"
fi

# --- resource hygiene -----------------------------------------------------
# Repeated background launches must not accumulate zombies.
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$W/zcheck"
chmod 755 "$W/zcheck"
printf '/bin/true &\n/bin/true &\n/bin/true &\n/bin/true &\n/bin/sleep 0.4\njobs\n%s\n' \
    "$W/zcheck" | $TIMEOUT "$MSH_ABS" >"$tmpout" 2>"$tmperr"
if grep -q 'Z' "$tmpout"; then
    fail "repeated background jobs leave no zombies" \
         "ps reported: $(tr '\n' ' ' < "$tmpout")"
elif ! grep -q 'Done\|R' "$tmpout"; then
    fail "repeated background jobs leave no zombies" "ps produced nothing usable"
else
    pass "repeated background jobs leave no zombies"
fi

printf '#!/bin/sh\nls /proc/self/fd | wc -l\n' > "$W/fdcount"
chmod 755 "$W/fdcount"
base=$(printf '%s\n' "$W/fdcount" | $TIMEOUT "$MSH_ABS" 2>/dev/null)
after=$(printf '/bin/true &\n/bin/true &\n/bin/true | /bin/cat &\n/bin/sleep 0.4\n%s\n' \
    "$W/fdcount" | $TIMEOUT "$MSH_ABS" 2>/dev/null | tail -1)
if [ -z "$base" ] || [ -z "$after" ]; then
    fail "background jobs leak no descriptors" "could not count descriptors"
elif [ "$base" = "$after" ]; then
    pass "background jobs leak no descriptors"
else
    fail "background jobs leak no descriptors" "was $base, now $after"
fi

# --- everything else still works -----------------------------------------
expect_status "foreground commands still work" 0 "/bin/true
"
expect_status "foreground failure status still works" 1 "/bin/false
"
expect_out "pipelines still work" "2" \
    "/usr/bin/printf 'a\\nb\\n' | /usr/bin/wc -l
"
expect_out "quoting still works" "hello world" \
    '/bin/echo "hello world"
'
expect_out "redirection still works" "x" \
    "/bin/echo x > $W/r
/bin/cat $W/r
"
expect_out "standalone cd still moves the shell" "/tmp" \
    "cd /tmp
pwd
"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_jobs: $tests checks passed"
    exit 0
fi
echo "test_jobs: $failures of $tests checks FAILED" >&2
exit 1

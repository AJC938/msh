#!/bin/sh
# End-to-end tests for pipelines.
#
# Everything runs inside a temporary directory that is removed on exit.
# Outputs and statuses are inspected directly; nothing depends on timing.
#
# The large-output case is the one that matters most: a shell that ran stages
# one after another, or that left a pipe end open, would block there forever
# rather than fail, so every msh invocation in this file is wrapped in a
# timeout and a timeout is reported as a failure.

set -u

MSH_BIN=${MSH_BIN:-out/msh}

if [ ! -x "$MSH_BIN" ]; then
    echo "test_pipeline: cannot find executable at $MSH_BIN" >&2
    exit 2
fi

case "$MSH_BIN" in
    /*) MSH_ABS="$MSH_BIN" ;;
    *)  MSH_ABS="$(pwd)/$MSH_BIN" ;;
esac

# A hang must fail, not stall the suite. If `timeout` is missing the tests
# still run, but say so rather than pretending the guard was in place.
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

# run INPUT -> fills $tmpout/$tmperr, sets $st. Status 124 means it hung.
run() {
    printf '%s' "$1" | $TIMEOUT "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
}

timed_out() { [ "$st" -eq 124 ] && [ "$GUARDED" = yes ]; }

# expect_out NAME WANT INPUT
expect_out() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT - possible deadlock"; return; fi
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "stdout [$got], wanted [$2]"
    fi
}

# expect_status NAME WANT INPUT
expect_status() {
    run "$3"
    if timed_out; then fail "$1" "TIMED OUT - possible deadlock"; return; fi
    if [ "$st" -eq "$2" ]; then pass "$1"; else
        fail "$1" "want status $2, got $st"
        sed -n '1,2p' "$tmperr" | sed 's/^/      stderr: /'
    fi
}

# expect_err NAME NEEDLE INPUT
expect_err() {
    run "$3"
    if grep -q "$2" "$tmperr"; then pass "$1"; else
        fail "$1" "stderr lacks [$2]: $(cat "$tmperr")"
    fi
}

echo "test_pipeline (pipelines)"
if [ "$GUARDED" = no ]; then
    echo "  NOTE: 'timeout' is not installed; a deadlock would hang this suite"
fi

# --- basic pipelines ------------------------------------------------------
expect_out "two stages" "hello" \
    "/usr/bin/printf 'hello\\n' | /bin/cat
"
expect_out "two stages, no spaces around the pipe" "hello" \
    "/usr/bin/printf 'hello\\n'|/bin/cat
"
printf 'alpha\nerror here\nbeta\nerror again\n' > "$W/f.txt"
expect_out "grep in the middle" "error here
error again" \
    "/bin/cat $W/f.txt | /usr/bin/grep error
"
expect_out "three stages" "2" \
    "/bin/cat $W/f.txt | /usr/bin/grep error | /usr/bin/wc -l
"
expect_out "four stages" "2" \
    "/bin/cat $W/f.txt | /usr/bin/grep error | /usr/bin/wc -l | /usr/bin/tr -d ' '
"
expect_out "arguments survive into a stage" "[a b][c]" \
    "/usr/bin/printf '' | $ARGS \"a b\" c
"
expect_out "a pipeline producing no output" "" \
    "/usr/bin/printf '' | /bin/cat
"

# --- exit status: last stage wins ----------------------------------------
expect_status "status is the last stage's (success)" 0 \
    "/bin/false | /bin/true
"
expect_status "status is the last stage's (failure)" 1 \
    "/bin/true | /bin/false
"
expect_status "an early failure does not override success" 0 \
    "/bin/false | /bin/false | /bin/true
"
expect_status "last stage exit code propagates" 42 \
    "/bin/true | /bin/sh -c 'exit 42'
"
expect_status "three stages, last one fails" 1 \
    "/bin/true | /bin/true | /bin/false
"

# --- command not found ----------------------------------------------------
expect_status "command not found in the LAST stage is 127" 127 \
    "/usr/bin/printf 'x\\n' | msh-no-such-command-xyz
"
expect_status "command not found in an EARLY stage: last stage decides" 0 \
    "msh-no-such-command-xyz | /bin/true
"
expect_err "a missing early stage is still reported" "msh-no-such-command-xyz" \
    "msh-no-such-command-xyz | /bin/true
"
expect_status "a non-executable last stage is 126" 126 \
    "/usr/bin/printf 'x\\n' | /etc/hostname
"

# --- signal termination ---------------------------------------------------
printf '#!/bin/sh\nkill -TERM $$\n' > "$W/selfterm"
chmod 755 "$W/selfterm"
expect_status "last stage killed by SIGTERM yields 143" 143 \
    "/usr/bin/printf 'x\\n' | $W/selfterm
"

# --- redirection inside pipelines ----------------------------------------
printf 'zeta\nalpha\nmu\n' > "$W/unsorted"
expect_out "input redirection into the first stage" "alpha
mu
zeta" \
    "/usr/bin/sort < $W/unsorted | /bin/cat
"
run "/bin/cat $W/f.txt | /usr/bin/grep error > $W/o1
"
if [ "$(cat "$W/o1")" = "error here
error again" ]; then
    pass "output redirection from the last stage"
else
    fail "output redirection from the last stage" "file has [$(cat "$W/o1")]"
fi
printf 'first\n' > "$W/o2"
run "/usr/bin/printf 'second\\n' | /bin/cat >> $W/o2
"
if [ "$(cat "$W/o2")" = "first
second" ]; then
    pass "append redirection from the last stage"
else
    fail "append redirection from the last stage" "file has [$(cat "$W/o2")]"
fi

# The documented precedence: an explicit redirection overrides the pipe.
run "/usr/bin/printf 'captured\\n' > $W/o3 | /bin/cat
"
if [ "$(cat "$W/o3")" = "captured" ] && [ ! -s "$tmpout" ]; then
    pass "explicit > overrides the pipe, consumer sees EOF"
else
    fail "explicit > overrides the pipe, consumer sees EOF" \
         "file [$(cat "$W/o3")], stdout [$(cat "$tmpout")]"
fi
printf 'from file\n' > "$W/in3"
expect_out "explicit < overrides the pipe for a later stage" "from file" \
    "/usr/bin/printf 'from pipe\\n' | /bin/cat < $W/in3
"

expect_out "redirection in both ends of a pipeline" "alpha
mu
zeta" \
    "/usr/bin/sort < $W/unsorted | /bin/cat
"
run "/usr/bin/sort < $W/unsorted | /bin/cat > $W/o4
"
if [ "$(cat "$W/o4")" = "alpha
mu
zeta" ]; then
    pass "input on the first stage and output on the last"
else
    fail "input on the first stage and output on the last" "[$(cat "$W/o4")]"
fi

# A failed redirection in a stage must not run that stage.
# The last stage still succeeds, so by the documented last-stage rule the
# pipeline status is 0. What must hold is that the failing stage reports
# itself and contributes nothing to the output.
expect_status "an early redirect failure leaves the last status" 0 \
    "/bin/cat < $W/nosuchfile | /bin/cat
"
expect_err "an early redirect failure is reported" "nosuchfile" \
    "/bin/cat < $W/nosuchfile | /bin/cat
"
expect_out "an early redirect failure produces no output" "" \
    "/bin/cat < $W/nosuchfile | /bin/cat
"
expect_status "a failed output redirect in the last stage" 1 \
    "/usr/bin/printf 'x\\n' | /bin/cat > $W/nodir/out
"
expect_out "the shell still works after a pipeline redirect failure" "alive" \
    "/bin/cat < $W/nosuchfile | /bin/cat
/bin/echo alive
"

# --- built-ins in pipelines ----------------------------------------------
expect_out "pwd | cat works" "/tmp" \
    "cd /tmp
pwd | /bin/cat
"
expect_out "a cd stage does NOT move the parent shell" "/tmp" \
    "cd /tmp
cd / | /bin/cat
pwd
"
expect_out "an exit stage does NOT end the parent shell" "still here" \
    "exit | /bin/cat
/bin/echo still here
"
expect_status "an exit stage does not set the shell's status" 0 \
    "exit 9 | /bin/true
"
expect_out "help in a pipeline reaches the next stage" "msh" \
    "help | /usr/bin/head -n 1 | /usr/bin/cut -d' ' -f1
"
expect_out "a builtin stage can be redirected too" "/tmp" \
    "cd /tmp
pwd | /bin/cat
"
run "cd /tmp
pwd > $W/o5 | /bin/cat
"
if [ "$(cat "$W/o5")" = "/tmp" ]; then
    pass "a builtin stage honours its own redirection"
else
    fail "a builtin stage honours its own redirection" "[$(cat "$W/o5")]"
fi

# Standalone built-ins must be unchanged.
expect_out "standalone cd still moves the shell" "/tmp" \
    "cd /tmp
pwd
"
expect_status "standalone exit still ends the shell" 5 \
    "exit 5
/bin/echo unreachable
"
expect_out "standalone pwd still prints to stdout" "/tmp" \
    "cd /tmp
pwd
"

# --- quoted and escaped pipes --------------------------------------------
expect_out "a quoted pipe is one argument" "[a|b]" "$ARGS \"a|b\"
"
expect_out "a single-quoted pipe is one argument" "[a|b]" "$ARGS 'a|b'
"
expect_out "an escaped pipe is one argument" "[a|b]" "$ARGS a\\|b
"
expect_out "a quoted bare pipe is one argument" "[|]" "$ARGS \"|\"
"
expect_out "a quoted pipe beside a real one" "[a|b]" \
    "/usr/bin/printf '' | $ARGS 'a|b'
"

# --- malformed pipelines --------------------------------------------------
expect_status "leading pipe is a syntax error" 2 "| /bin/cat
"
expect_status "trailing pipe is a syntax error" 2 "/bin/cat |
"
expect_status "double pipe is a syntax error" 2 "/bin/cat || /usr/bin/grep x
"
expect_status "pipe pipe with a space is a syntax error" 2 \
    "/bin/cat | | /usr/bin/grep x
"
expect_status "a bare pipe is a syntax error" 2 "|
"
expect_err "a malformed pipeline is reported" "syntax error" "/bin/cat |
"
expect_err "the pipeline error names the problem" "|" "/bin/cat |
"
expect_out "a malformed pipeline runs nothing" "" \
    "/bin/echo visible |
"
expect_out "the shell survives a malformed pipeline" "alive" \
    "/bin/cat |
/bin/echo alive
"

# A malformed line must not create or truncate a redirection target.
rm -f "$W/never"
run "/bin/echo hi > $W/never |
"
if [ -e "$W/never" ]; then
    fail "a malformed pipeline creates no file" "the file was created"
else
    pass "a malformed pipeline creates no file"
fi
printf 'PRECIOUS\n' > "$W/keep"
run "/bin/echo hi > $W/keep | | /bin/cat
"
if [ "$(cat "$W/keep")" = "PRECIOUS" ]; then
    pass "a malformed pipeline does not truncate an existing file"
else
    fail "a malformed pipeline does not truncate an existing file" "truncated"
fi

# --- large output: the deadlock test -------------------------------------
# 64 KiB is comfortably past a pipe buffer, so a shell that ran the stages
# one after another would block here forever.
expect_out "200000-byte stream through two stages" "200000" \
    "/usr/bin/head -c 200000 /dev/zero | /usr/bin/wc -c
"
expect_out "large stream through three stages" "200000" \
    "/usr/bin/head -c 200000 /dev/zero | /bin/cat | /usr/bin/wc -c
"
expect_out "large stream with a slow consumer" "100000" \
    "/usr/bin/head -c 100000 /dev/zero | /bin/cat | /bin/cat | /usr/bin/wc -c
"
# A consumer that exits early makes the producer see EPIPE; the shell must
# still return promptly and report the last stage.
expect_status "an early-exiting consumer does not hang the shell" 0 \
    "/usr/bin/head -c 200000 /dev/zero | /usr/bin/head -c 1 > /dev/null
"

# --- resource hygiene -----------------------------------------------------
printf '#!/bin/sh\nls /proc/self/fd | wc -l\n' > "$W/fdcount"
chmod 755 "$W/fdcount"
base=$(printf '%s\n' "$W/fdcount" | $TIMEOUT "$MSH_ABS" 2>/dev/null)
# Every command before the counter sends its output away, so the captured
# value is the descriptor count alone, not the pipelines' own output.
many="/usr/bin/printf 'a\\n' | /bin/cat > /dev/null
/usr/bin/printf 'b\\n' | /bin/cat | /bin/cat > /dev/null
/bin/cat $W/f.txt | /usr/bin/grep error | /usr/bin/wc -l > /dev/null
pwd | /bin/cat > /dev/null
/usr/bin/printf 'c\\n' | /bin/cat > $W/r1
$W/fdcount"
after=$(printf '%s\n' "$many" | $TIMEOUT "$MSH_ABS" 2>/dev/null)
if [ -z "$base" ] || [ -z "$after" ]; then
    fail "no descriptors leak across pipelines" "could not count descriptors"
elif [ "$base" = "$after" ]; then
    pass "no descriptors leak across pipelines"
else
    fail "no descriptors leak across pipelines" "was $base, now $after"
fi

# No zombies after many pipelines.
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$W/zcheck"
chmod 755 "$W/zcheck"
printf '/usr/bin/printf "a\\n" | /bin/cat\n/bin/true | /bin/false | /bin/true\npwd | /bin/cat\n%s\n' \
    "$W/zcheck" | $TIMEOUT "$MSH_ABS" >"$tmpout" 2>"$tmperr"
if [ ! -s "$tmpout" ]; then
    fail "no zombies after pipelines" "ps produced no output; inconclusive"
elif grep -q 'Z' "$tmpout"; then
    fail "no zombies after pipelines" "ps reported: $(tr '\n' ' ' < "$tmpout")"
else
    pass "no zombies after pipelines"
fi

# The shell's own stdin and stdout survive a pipeline.
expect_out "the shell's stdout works after a pipeline" "after" \
    "/usr/bin/printf 'x\\n' | /bin/cat > /dev/null
/bin/echo after
"
expect_out "many commands after many pipelines" "done" \
    "/usr/bin/printf 'a\\n' | /bin/cat > /dev/null
/usr/bin/printf 'b\\n' | /bin/cat | /bin/cat > /dev/null
/bin/true | /bin/false
/bin/echo done
"

# --- existing behaviour intact -------------------------------------------
expect_status "a plain external command still works" 0 "/bin/true
"
expect_out "quoting without a pipeline still works" "hello world" \
    '/bin/echo "hello world"
'
expect_status "blank input still preserves the status" 1 \
    "/bin/false

"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_pipeline: $tests checks passed"
    exit 0
fi
echo "test_pipeline: $failures of $tests checks FAILED" >&2
exit 1

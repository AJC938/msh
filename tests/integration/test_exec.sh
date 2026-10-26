#!/bin/sh
# Integration tests for external command execution (milestone 3).
#
# Driven through pipes, so stdin is not a terminal. Because this milestone has
# no quote-aware parser, every command here is whitespace-separated only; any
# case needing a multi-word argument uses a generated helper script invoked as
# a single token instead.

set -u

MSH_BIN=${MSH_BIN:-out/msh}
failures=0
tests=0

if [ ! -x "$MSH_BIN" ]; then
    echo "test_exec: cannot find executable at $MSH_BIN" >&2
    exit 2
fi

# Absolute path: some cases run msh with a different working directory.
case "$MSH_BIN" in
    /*) MSH_ABS="$MSH_BIN" ;;
    *)  MSH_ABS="$(pwd)/$MSH_BIN" ;;
esac

workdir=$(mktemp -d) || exit 2
tmpout=$(mktemp) || exit 2
tmperr=$(mktemp) || exit 2
trap 'rm -rf "$workdir" "$tmpout" "$tmperr"' EXIT INT TERM

pass() { tests=$((tests + 1)); printf '  %-50s ok\n' "$1"; }
fail() {
    tests=$((tests + 1))
    failures=$((failures + 1))
    printf '  %-50s FAILED (%s)\n' "$1" "$2"
}

# run_status NAME EXPECTED_STATUS INPUT
run_status() {
    printf '%s\n' "$3" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
    if [ "$st" -eq "$2" ]; then pass "$1"; else
        fail "$1" "want status $2, got $st"
        sed -n '1,3p' "$tmperr" | sed 's/^/      stderr: /'
    fi
}

# run_output NAME EXPECTED_STDOUT INPUT
run_output() {
    printf '%s\n' "$3" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "want stdout [$2], got [$got]"
    fi
}

echo "test_exec (external command execution)"

# --- basic execution and exit status -------------------------------------
run_status "/bin/true exits 0"                        0   "/bin/true"
run_status "/bin/false exits 1"                       1   "/bin/false"
run_status "command found via PATH (true)"            0   "true"
run_status "command found via PATH (false)"           1   "false"

# --- arguments ------------------------------------------------------------
run_output "/bin/echo hello world prints both words"  "hello world"  "/bin/echo hello world"
run_output "argument order is preserved"              "a b c"        "/bin/echo a b c"
run_output "repeated spaces collapse, not duplicate"  "a b"          "/bin/echo    a     b"
run_output "leading/trailing spaces are ignored"      "x"            "   /bin/echo x   "
run_output "echo with no arguments prints a blank"    ""             "/bin/echo"

# --- nonexistent command --------------------------------------------------
printf 'msh-no-such-command-xyz\n' | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
st=$?
[ "$st" -eq 127 ] && pass "nonexistent command exits 127" \
                  || fail "nonexistent command exits 127" "got $st"
if grep -q "msh-no-such-command-xyz" "$tmperr"; then
    pass "nonexistent command names itself on stderr"
else
    fail "nonexistent command names itself on stderr" "stderr: $(cat "$tmperr")"
fi
if [ -s "$tmpout" ]; then
    fail "nonexistent command writes nothing to stdout" "stdout: $(cat "$tmpout")"
else
    pass "nonexistent command writes nothing to stdout"
fi

# --- non-executable file and a directory ----------------------------------
printf '#!/bin/sh\necho should not run\n' > "$workdir/noexec"
chmod 644 "$workdir/noexec"
run_status "non-executable file exits 126"            126 "$workdir/noexec"

if grep -q "noexec" "$tmperr"; then
    pass "non-executable file is reported on stderr"
else
    fail "non-executable file is reported on stderr" "stderr: $(cat "$tmperr")"
fi

run_status "a directory as a command is not run"      126 "$workdir"

# --- the shell survives failures ------------------------------------------
printf '/bin/false\n/bin/echo still-alive\n' | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
st=$?
if [ "$st" -eq 0 ] && [ "$(cat "$tmpout")" = "still-alive" ]; then
    pass "shell continues after a child exits nonzero"
else
    fail "shell continues after a child exits nonzero" "status $st, stdout [$(cat "$tmpout")]"
fi

printf 'msh-no-such-command-xyz\n/bin/echo recovered\n' | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
st=$?
if [ "$st" -eq 0 ] && [ "$(cat "$tmpout")" = "recovered" ]; then
    pass "shell continues after a failed exec"
else
    fail "shell continues after a failed exec" "status $st, stdout [$(cat "$tmpout")]"
fi

# The exit status of the LAST command is the shell's status.
run_status "last command determines shell status"     1   "/bin/true
/bin/false"

# A blank line must not disturb the preceding status.
printf '/bin/false\n\n   \n' | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
st=$?
[ "$st" -eq 1 ] && pass "blank lines preserve previous status" \
                || fail "blank lines preserve previous status" "got $st"

# --- death by signal ------------------------------------------------------
# A helper script is used because msh cannot yet pass a quoted multi-word
# argument such as: sh -c 'kill -TERM $$'
printf '#!/bin/sh\nkill -TERM $$\n' > "$workdir/selfterm"
chmod 755 "$workdir/selfterm"
run_status "child killed by SIGTERM yields 128+15=143" 143 "$workdir/selfterm"

printf '#!/bin/sh\nkill -KILL $$\n' > "$workdir/selfkill"
chmod 755 "$workdir/selfkill"
run_status "child killed by SIGKILL yields 128+9=137"  137 "$workdir/selfkill"

# --- exit codes propagate faithfully --------------------------------------
printf '#!/bin/sh\nexit 42\n' > "$workdir/exit42"
chmod 755 "$workdir/exit42"
run_status "child exit code 42 propagates"            42  "$workdir/exit42"

# --- no duplicated output across fork -------------------------------------
# With stdout redirected to a file it is block-buffered, so an unflushed
# parent buffer would be inherited by the child and written twice.
printf '/bin/echo one\n/bin/echo two\n' | "$MSH_ABS" >"$tmpout" 2>/dev/null
if [ "$(cat "$tmpout")" = "one
two" ]; then
    pass "no duplicated buffered output through a pipe"
else
    fail "no duplicated buffered output through a pipe" "got [$(cat "$tmpout")]"
fi

count=$(printf '/bin/echo dup\n' | "$MSH_ABS" 2>/dev/null | grep -c '^dup$')
[ "$count" -eq 1 ] && pass "output appears exactly once" \
                   || fail "output appears exactly once" "appeared $count times"

# --- no zombies left behind -----------------------------------------------
# After many foreground commands, msh must have reaped every child. Checked
# from inside msh itself: ps runs as the last command, so any unreaped
# sibling would still be visible as a zombie.
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$workdir/zcheck"
chmod 755 "$workdir/zcheck"
printf '/bin/true\n/bin/false\n/bin/true\n%s\n' "$workdir/zcheck" \
    | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
# Guard against a vacuous pass: if ps did not run (not installed, or msh
# failed to exec it) the output is empty and a plain grep for 'Z' would
# "succeed" while having checked nothing. Normally ps reports at least its
# own state, so empty output means the check itself is broken.
if [ ! -s "$tmpout" ]; then
    fail "no zombie children remain" "ps produced no output; check is inconclusive"
elif grep -q 'Z' "$tmpout"; then
    fail "no zombie children remain" "ps reported: $(tr '
' ' ' < "$tmpout")"
else
    pass "no zombie children remain"
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_exec: $tests checks passed"
    exit 0
fi
echo "test_exec: $failures of $tests checks FAILED" >&2
exit 1

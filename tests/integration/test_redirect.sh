#!/bin/sh
# End-to-end tests for I/O redirection.
#
# Everything runs inside a temporary directory that is removed on exit, so the
# tests are deterministic and leave nothing behind.
#
# File contents are checked directly rather than inferred from terminal
# output: that is the only way to tell a working redirection from one that
# merely printed nothing.

set -u

MSH_BIN=${MSH_BIN:-out/msh}

if [ ! -x "$MSH_BIN" ]; then
    echo "test_redirect: cannot find executable at $MSH_BIN" >&2
    exit 2
fi

case "$MSH_BIN" in
    /*) MSH_ABS="$MSH_BIN" ;;
    *)  MSH_ABS="$(pwd)/$MSH_BIN" ;;
esac

failures=0
tests=0

tmpout=$(mktemp) || exit 2
tmperr=$(mktemp) || exit 2
W=$(mktemp -d) || exit 2
trap 'rm -rf "$tmpout" "$tmperr" "$W"' EXIT INT TERM

# A helper that brackets its arguments, for checking argument boundaries.
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
    printf '%s' "$1" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
}

# expect_file NAME PATH WANT INPUT
expect_file() {
    run "$4"
    if [ ! -f "$2" ]; then
        fail "$1" "file $2 was not created"
        return
    fi
    got=$(cat "$2")
    if [ "$got" = "$3" ]; then pass "$1"; else
        fail "$1" "file has [$got], wanted [$3]"
    fi
}

# expect_status NAME WANT INPUT
expect_status() {
    run "$3"
    if [ "$st" -eq "$2" ]; then pass "$1"; else
        fail "$1" "want status $2, got $st"
        sed -n '1,2p' "$tmperr" | sed 's/^/      stderr: /'
    fi
}

# expect_out NAME WANT INPUT   (checks msh's own stdout)
expect_out() {
    run "$3"
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "stdout [$got], wanted [$2]"
    fi
}

# expect_err NAME NEEDLE INPUT
expect_err() {
    run "$3"
    if grep -q "$2" "$tmperr"; then pass "$1"; else
        fail "$1" "stderr lacks [$2]: $(cat "$tmperr")"
    fi
}

echo "test_redirect (I/O redirection)"

# --- output redirection: create and truncate -----------------------------
rm -f "$W/o1"
expect_file "> creates a new file" "$W/o1" "hello" \
    "/bin/echo hello > $W/o1
"
printf 'PREEXISTING CONTENT\n' > "$W/o2"
expect_file "> truncates an existing file" "$W/o2" "new" \
    "/bin/echo new > $W/o2
"

# --- append ---------------------------------------------------------------
printf 'first\n' > "$W/o3"
expect_file ">> appends and keeps existing content" "$W/o3" "first
second" \
    "/bin/echo second >> $W/o3
"
rm -f "$W/o4"
expect_file ">> creates the file when absent" "$W/o4" "only" \
    "/bin/echo only >> $W/o4
"
printf 'a\n' > "$W/o5"
expect_file ">> twice appends twice" "$W/o5" "a
b
c" \
    "/bin/echo b >> $W/o5
/bin/echo c >> $W/o5
"

# --- input redirection ----------------------------------------------------
printf 'line one\nline two\n' > "$W/in1"
expect_out "< reads from an existing file" "line one
line two" \
    "/bin/cat < $W/in1
"
expect_status "< from an existing file succeeds" 0 \
    "/bin/cat < $W/in1
"
expect_status "< from a missing file fails" 1 \
    "/bin/cat < $W/nosuchfile
"
expect_err "< from a missing file is reported" "nosuchfile" \
    "/bin/cat < $W/nosuchfile
"
expect_out "a failed input redirection runs nothing" "" \
    "/bin/cat < $W/nosuchfile
"

# --- both redirections ----------------------------------------------------
printf 'z\ny\nx\n' > "$W/in2"
expect_file "< and > together" "$W/o6" "x
y
z" \
    "/usr/bin/sort < $W/in2 > $W/o6
"
expect_file "> and < in the other order" "$W/o7" "x
y
z" \
    "/usr/bin/sort > $W/o7 < $W/in2
"

# --- adjacency ------------------------------------------------------------
rm -f "$W/o8"
expect_file "operator adjacent to the argument" "$W/o8" "hello" \
    "/bin/echo hello>$W/o8
"
rm -f "$W/o9"
expect_file "append operator adjacent" "$W/o9" "hi" \
    "/bin/echo hi>>$W/o9
"
printf 'data\n' > "$W/in3"
expect_out "input operator adjacent" "data" \
    "/bin/cat<$W/in3
"

# --- redirection before the command --------------------------------------
rm -f "$W/o10"
expect_file "redirection before the command" "$W/o10" "after" \
    "> $W/o10 /bin/echo after
"

# --- quoted paths with spaces --------------------------------------------
expect_file "quoted output path with spaces" "$W/out file.txt" "hello world" \
    "/bin/echo \"hello world\" > \"$W/out file.txt\"
"
printf 'quoted input\n' > "$W/in file.txt"
expect_out "quoted input path with spaces" "quoted input" \
    "/bin/cat < \"$W/in file.txt\"
"
expect_out "single-quoted input path with spaces" "quoted input" \
    "/bin/cat < '$W/in file.txt'
"
expect_out "backslash-escaped path with spaces" "quoted input" \
    "/bin/cat < $W/in\\ file.txt
"

# --- quoted and escaped operators stay literal ---------------------------
expect_out "a quoted > is an argument"    "[>]"       "$ARGS \">\"
"
expect_out "a quoted a>b is an argument"  "[a>b]"     "$ARGS 'a>b'
"
expect_out "an escaped > is an argument"  "[a>b]"     "$ARGS a\\>b
"
expect_out "a quoted < is an argument"    "[<]"       "$ARGS \"<\"
"
expect_out "a quoted >> is an argument"   "[>>]"      "$ARGS '>>'
"
# A quoted operator must not create a file named after it.
rm -f "$W/shouldnotexist"
run "$ARGS \">\" \"$W/shouldnotexist\"
"
if [ -e "$W/shouldnotexist" ]; then
    fail "a quoted operator creates no file" "the file was created"
else
    pass "a quoted operator creates no file"
fi

# --- built-in redirection -------------------------------------------------
rm -f "$W/p1"
expect_file "pwd redirected to a file" "$W/p1" "/tmp" \
    "cd /tmp
pwd > $W/p1
"
rm -f "$W/h1"
run "help > $W/h1
"
if [ -s "$W/h1" ] && grep -q "Built-in commands" "$W/h1"; then
    pass "help redirected to a file"
else
    fail "help redirected to a file" "file missing or lacks the help text"
fi
if [ -s "$tmpout" ]; then
    fail "redirected help writes nothing to the terminal" "$(cat "$tmpout")"
else
    pass "redirected help writes nothing to the terminal"
fi

rm -f "$W/p2"
expect_file "pwd append to a file" "$W/p2" "/tmp
/tmp" \
    "cd /tmp
pwd >> $W/p2
pwd >> $W/p2
"

# The decisive one: after a redirected built-in, the shell's own stdout must
# be back. The second pwd has to reach the terminal.
expect_out "stdout is restored after a redirected builtin" "/tmp" \
    "cd /tmp
pwd > $W/p3
pwd
"
expect_out "stdout restored after a redirected external command" "visible" \
    "/bin/echo hidden > $W/p4
/bin/echo visible
"

# cd is still a built-in when redirected: it must affect the shell.
rm -f "$W/p5"
expect_out "cd still moves the shell when redirected" "/tmp" \
    "cd /tmp > $W/p5
pwd
"

# A redirected built-in that fails must still restore stdout.
expect_out "stdout restored when a redirected builtin fails" "recovered" \
    "cd /msh-no-such-dir-xyz > $W/p6
/bin/echo recovered
"
expect_status "a redirected failing builtin keeps its status" 1 \
    "cd /msh-no-such-dir-xyz > $W/p7
"

# exit works with redirection and still ends the shell.
expect_status "exit with redirection still exits" 7 \
    "exit 7 > $W/p8
/bin/echo unreachable
"
run "exit 7 > $W/p9
/bin/echo unreachable
"
if [ -s "$tmpout" ]; then
    fail "nothing after exit runs even when redirected" "$(cat "$tmpout")"
else
    pass "nothing after exit runs even when redirected"
fi

# --- redirection failures -------------------------------------------------
expect_status "output in a missing directory fails" 1 \
    "/bin/echo hi > $W/nodir/out
"
expect_err "output failure is reported" "nodir" \
    "/bin/echo hi > $W/nodir/out
"
expect_status "builtin output in a missing directory fails" 1 \
    "pwd > $W/nodir/out
"
expect_out "the shell continues after a redirection failure" "alive" \
    "/bin/echo hi > $W/nodir/out
/bin/echo alive
"
expect_out "the shell continues after a builtin redirect failure" "alive" \
    "pwd > $W/nodir/out
/bin/echo alive
"

# A failed input redirection must not create the output file either.
rm -f "$W/o11"
run "/bin/cat < $W/nosuchfile > $W/o11
"
# Redirections are applied input-first, so a failing input means the output
# file is never opened and therefore never created.
if [ -e "$W/o11" ]; then
    fail "a failed input redirect does not create the output file" "it was created"
else
    pass "a failed input redirect does not create the output file"
fi

# --- malformed syntax creates nothing ------------------------------------
rm -f "$W/never"
expect_status "missing output path is a syntax error" 2 \
    "/bin/echo hello >
"
expect_status "missing input path is a syntax error" 2 \
    "/bin/cat <
"
expect_status "missing append path is a syntax error" 2 \
    "/bin/echo hello >>
"
expect_status "duplicate output is a syntax error" 2 \
    "/bin/echo hi > $W/never > $W/never2
"
expect_status "duplicate input is a syntax error" 2 \
    "/bin/cat < $W/in1 < $W/in1
"
expect_status "operator instead of a path is a syntax error" 2 \
    "/bin/echo hi > >
"
expect_status "unterminated quoted path is a syntax error" 2 \
    "/bin/echo hi > \"$W/never
"
expect_status "redirection with no command is a syntax error" 2 \
    "> $W/never
"
expect_err "a redirection syntax error is reported" "syntax error" \
    "/bin/echo hello >
"

# The whole point: none of those touched the filesystem.
if [ -e "$W/never" ] || [ -e "$W/never2" ]; then
    fail "malformed lines create no files" "a target file was created"
else
    pass "malformed lines create no files"
fi

# A syntactically invalid line must not truncate an EXISTING target.
printf 'PRECIOUS\n' > "$W/keep"
run "/bin/echo hi > $W/keep > $W/other
"
if [ "$(cat "$W/keep")" = "PRECIOUS" ]; then
    pass "a malformed line does not truncate an existing file"
else
    fail "a malformed line does not truncate an existing file" "it was truncated"
fi
printf 'PRECIOUS\n' > "$W/keep2"
run "/bin/echo hi > $W/keep2 'unterminated
"
if [ "$(cat "$W/keep2")" = "PRECIOUS" ]; then
    pass "an unterminated quote does not truncate the target"
else
    fail "an unterminated quote does not truncate the target" "it was truncated"
fi

expect_out "the shell continues after a redirection syntax error" "alive" \
    "/bin/echo hi >
/bin/echo alive
"

# --- existing behaviour still intact -------------------------------------
expect_status "external commands still work" 0 "/bin/true
"
expect_status "external failure status still works" 1 "/bin/false
"
expect_status "nonexistent command is still 127" 127 "msh-no-such-cmd-xyz
"
expect_out "quoting still works without redirection" "hello world" \
    '/bin/echo "hello world"
'
expect_status "blank input still preserves the status" 1 \
    "/bin/false


"

# --- descriptor hygiene ---------------------------------------------------
# After many redirections, the shell must not have accumulated descriptors.
# Checked from inside msh: ls runs as the last command and reports what it
# inherited, which is the shell's own descriptor set plus its own.
printf '#!/bin/sh\nls /proc/self/fd | wc -l\n' > "$W/fdcount"
chmod 755 "$W/fdcount"
base=$(printf '%s\n' "$W/fdcount" | "$MSH_ABS" 2>/dev/null)
many="/bin/echo a > $W/r1
/bin/echo b >> $W/r1
/bin/cat < $W/r1 > $W/r2
pwd > $W/r3
help > $W/r4
/bin/echo c > $W/r5
$W/fdcount"
after=$(printf '%s\n' "$many" | "$MSH_ABS" 2>/dev/null)
if [ -z "$base" ] || [ -z "$after" ]; then
    fail "no descriptors leak across many redirections" "could not count descriptors"
elif [ "$base" = "$after" ]; then
    pass "no descriptors leak across many redirections"
else
    fail "no descriptors leak across many redirections" "was $base, now $after"
fi

# No zombies after a session full of redirections.
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$W/zcheck"
chmod 755 "$W/zcheck"
printf '/bin/echo a > %s/z1\npwd > %s/z2\n/bin/cat < %s/z1\n%s\n' \
    "$W" "$W" "$W" "$W/zcheck" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
if [ ! -s "$tmpout" ]; then
    fail "no zombies after redirected commands" "ps produced no output; inconclusive"
elif grep -q 'Z' "$tmpout"; then
    fail "no zombies after redirected commands" "ps reported: $(tr '\n' ' ' < "$tmpout")"
else
    pass "no zombies after redirected commands"
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_redirect: $tests checks passed"
    exit 0
fi
echo "test_redirect: $failures of $tests checks FAILED" >&2
exit 1

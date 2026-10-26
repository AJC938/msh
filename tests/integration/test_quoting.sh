#!/bin/sh
# End-to-end tests for quoting, escaping and syntax errors.
#
# Driven through pipes, so stdin is not a terminal. These complement the
# parser unit tests by proving the parsed words survive all the way into
# execvp() and into the built-ins.
#
# Argument boundaries are checked with two generated helper scripts rather
# than with echo: echo joins its arguments with a single space, so it cannot
# distinguish one argument containing a space from several arguments.

set -u

MSH_BIN=${MSH_BIN:-out/msh}

if [ ! -x "$MSH_BIN" ]; then
    echo "test_quoting: cannot find executable at $MSH_BIN" >&2
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
workdir=$(mktemp -d) || exit 2
trap 'rm -rf "$tmpout" "$tmperr" "$workdir"' EXIT INT TERM

# argc prints how many arguments it received; args brackets each one.
printf '#!/bin/sh\necho $#\n' > "$workdir/argc"
printf '#!/bin/sh\nfor a in "$@"; do printf "[%%s]" "$a"; done; echo\n' \
    > "$workdir/args"
chmod 755 "$workdir/argc" "$workdir/args"
ARGC="$workdir/argc"
ARGS="$workdir/args"

pass() { tests=$((tests + 1)); printf '  %-54s ok\n' "$1"; }
fail() {
    tests=$((tests + 1))
    failures=$((failures + 1))
    printf '  %-54s FAILED (%s)\n' "$1" "$2"
}

run() {
    printf '%s' "$1" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
}

# expect_out NAME WANT INPUT
expect_out() {
    run "$3"
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "want [$2], got [$got]"
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

# expect_err NAME NEEDLE INPUT
expect_err() {
    run "$3"
    if grep -q "$2" "$tmperr"; then pass "$1"; else
        fail "$1" "stderr lacks [$2]: $(cat "$tmperr")"
    fi
}

echo "test_quoting (quoting, escaping, syntax errors)"

# --- argument boundaries: the decisive checks ----------------------------
expect_out "double-quoted text is ONE argument"   "1"  "$ARGC \"a b c\"
"
expect_out "the same text unquoted is THREE"      "3"  "$ARGC a b c
"
expect_out "single-quoted text is ONE argument"   "1"  "$ARGC 'a b c'
"
expect_out "quoted argument arrives byte-exact"   "[a b c]"  "$ARGS \"a b c\"
"
expect_out "two quoted arguments stay separate"   "[a b][c d]" \
    "$ARGS \"a b\" \"c d\"
"
expect_out "empty quotes are a real argument"     "1"  "$ARGC \"\"
"
expect_out "empty argument arrives empty"         "[]"  "$ARGS ''
"

# --- echo output ----------------------------------------------------------
expect_out "double-quoted argument with a space"  "hello world" \
    '/bin/echo "hello world"
'
expect_out "single-quoted argument with a space"  "hello world" \
    "/bin/echo 'hello world'
"
expect_out "single quotes pass text literally"    '$HOME *.c \n' \
    "/bin/echo '\$HOME *.c \\n'
"

# --- concatenation --------------------------------------------------------
expect_out "adjacent segments concatenate"        "abcdef" \
    '/bin/echo ab"cd"'"'"'ef'"'"'
'
expect_out "concatenation is still one argument"  "1" \
    "$ARGC ab\"cd\"'ef'
"
expect_out "quoted and unquoted mix"              "[a b c]" \
    "$ARGS 'a '\"b \"c
"
expect_out "empty quotes between characters"      "[ab]" \
    "$ARGS a''b
"

# --- backslash ------------------------------------------------------------
expect_out "backslash escapes a space"            "[a b]" \
    "$ARGS a\\ b
"
expect_out "backslash escapes a quote"            'a"b' \
    '/bin/echo a\"b
'
expect_out "escaped backslash"                    '\' \
    '/bin/echo \\
'
expect_out "backslash inside double quotes"       'a\b' \
    '/bin/echo "a\b"
'
expect_out "escaped quote inside double quotes"   'a"b' \
    '/bin/echo "a\"b"
'

# --- a quoted path reaches an external command intact --------------------
spacey="$workdir/two words"
: > "$spacey"
expect_out "a quoted path with spaces reaches the command"  "[$spacey]" \
    "$ARGS \"$spacey\"
"
expect_out "the same path unquoted splits in two"           "2" \
    "$ARGC $spacey
"

# --- cd with a quoted directory containing spaces ------------------------
mkdir -p "$workdir/dir with spaces"
expect_out "cd into a quoted path with spaces, then pwd"  "$workdir/dir with spaces" \
    "cd \"$workdir/dir with spaces\"
pwd
"
expect_out "cd into a single-quoted path with spaces"     "$workdir/dir with spaces" \
    "cd '$workdir/dir with spaces'
pwd
"
expect_out "cd with backslash-escaped spaces"             "$workdir/dir with spaces" \
    "cd $workdir/dir\\ with\\ spaces
pwd
"
expect_status "cd to a quoted missing path still fails"    1 \
    'cd "/msh no such dir"
'
expect_status "an unquoted path with spaces is too many args"  1 \
    "cd $workdir/dir with spaces
"

# --- quoted built-in arguments -------------------------------------------
expect_status "quoted exit status is parsed"               7  'exit "7"
'
expect_status "single-quoted exit status is parsed"        5  "exit '5'
"
expect_status "quoted non-numeric exit is a usage error"   2  'exit "abc"
'
expect_status "quoted empty exit argument is rejected"     2  'exit ""
'
expect_status "exit with two quoted arguments is rejected" 1  'exit "1" "2"
'
expect_status "quoted pwd argument is still too many"      1  'pwd "x"
'
expect_status "quoted help argument is still too many"     1  'help "x"
'

# --- syntax errors --------------------------------------------------------
expect_status "unmatched single quote is status 2"         2  "/bin/echo 'abc
"
expect_status "unmatched double quote is status 2"         2  '/bin/echo "abc
'
expect_status "trailing backslash is status 2"             2  '/bin/echo abc\
'
expect_err "syntax error is reported on stderr"  "syntax error"  "/bin/echo 'abc
"
expect_err "unmatched quote is named"            "unmatched"     '/bin/echo "abc
'
expect_err "trailing backslash is named"         "backslash"     '/bin/echo abc\
'

# A syntax error must not run any part of the line.
expect_out "no partial command runs on a syntax error"     "" \
    '/bin/echo visible "abc
'

# The shell must survive and keep working.
expect_out "a valid command after a syntax error runs"     "recovered" \
    '/bin/echo "oops
/bin/echo recovered
'
expect_status "status after recovery is the later command's"  0 \
    '/bin/echo "oops
/bin/true
'
expect_out "a builtin works after a syntax error"          "/tmp" \
    'cd "oops
cd /tmp
pwd
'
expect_status "a syntax error does not terminate the shell"  0 \
    '/bin/echo "oops
exit 0
'
expect_status "blank input after a syntax error keeps 2"   2 \
    '/bin/echo "oops


'

# --- a syntax error must not fork ----------------------------------------
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$workdir/zcheck"
chmod 755 "$workdir/zcheck"
printf '/bin/echo "unterminated\n%s\n' "$workdir/zcheck" \
    | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
if [ ! -s "$tmpout" ]; then
    fail "no stray children after a syntax error" "ps produced no output; inconclusive"
elif grep -q 'Z' "$tmpout"; then
    fail "no stray children after a syntax error" "ps reported: $(tr '\n' ' ' < "$tmpout")"
else
    pass "no stray children after a syntax error"
fi

# --- long quoted input ----------------------------------------------------
long=$(awk 'BEGIN { for (i = 0; i < 5000; i++) printf "q" }')
expect_out "5000-char quoted argument survives"  "$long" \
    "/bin/echo \"$long\"
"
expect_out "a long quoted argument is still one argument"  "1" \
    "$ARGC \"$long\"
"
expect_status "long line with an unterminated quote is status 2"  2 \
    "/bin/echo \"$long
"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_quoting: $tests checks passed"
    exit 0
fi
echo "test_quoting: $failures of $tests checks FAILED" >&2
exit 1

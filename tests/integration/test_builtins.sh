#!/bin/sh
# End-to-end tests for the built-in commands.
#
# Driven through pipes, so stdin is not a terminal. Because quoting is not
# implemented, every argument here is a single whitespace-free token.
#
# These tests deliberately exercise msh as a whole process: a built-in that
# only worked inside a child would pass a unit test for cd and fail here.

set -u

MSH_BIN=${MSH_BIN:-out/msh}

if [ ! -x "$MSH_BIN" ]; then
    echo "test_builtins: cannot find executable at $MSH_BIN" >&2
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

pass() { tests=$((tests + 1)); printf '  %-52s ok\n' "$1"; }
fail() {
    tests=$((tests + 1))
    failures=$((failures + 1))
    printf '  %-52s FAILED (%s)\n' "$1" "$2"
}

# run INPUT -> fills $tmpout/$tmperr, sets $st
run() {
    printf '%s' "$1" | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
    st=$?
}

# expect_status NAME WANT INPUT
expect_status() {
    run "$3"
    if [ "$st" -eq "$2" ]; then pass "$1"; else
        fail "$1" "want status $2, got $st"
        sed -n '1,3p' "$tmperr" | sed 's/^/      stderr: /'
    fi
}

# expect_stdout NAME WANT INPUT
expect_stdout() {
    run "$3"
    got=$(cat "$tmpout")
    if [ "$got" = "$2" ]; then pass "$1"; else
        fail "$1" "want stdout [$2], got [$got]"
    fi
}

# expect_stderr_has NAME NEEDLE INPUT
expect_stderr_has() {
    run "$3"
    if grep -q "$2" "$tmperr"; then pass "$1"; else
        fail "$1" "stderr lacks [$2]: $(cat "$tmperr")"
    fi
}

echo "test_builtins (built-ins end to end)"

# --- pwd ------------------------------------------------------------------
here=$(pwd)
expect_stdout "pwd prints the working directory"        "$here"   'pwd
'
expect_status "pwd succeeds"                            0         'pwd
'
expect_status "pwd rejects extra arguments"             1         'pwd extra
'
expect_stderr_has "pwd extra args reported on stderr"   "pwd"     'pwd extra
'

# --- cd: the shell's own directory ---------------------------------------
# The decisive test: cd then pwd, in the SAME msh process. If cd ran in a
# child, pwd would still report the original directory.
expect_stdout "cd changes the shell's own directory"    "/tmp"    'cd /tmp
pwd
'

# Relative movement works and compounds.
mkdir -p "$workdir/a/b"
expect_stdout "cd into a nested path"  "$workdir/a/b"  "cd $workdir/a/b
pwd
"
expect_stdout "cd .. moves up"         "$workdir/a"    "cd $workdir/a/b
cd ..
pwd
"

# cd with no argument goes to HOME.
expect_stdout "cd with no argument uses HOME"  "$HOME"  'cd /tmp
cd
pwd
'

expect_status "cd succeeds"                             0         'cd /tmp
'

# --- cd failures preserve the directory ----------------------------------
expect_status "cd to a missing directory fails"         1         'cd /msh-no-such-dir-xyz
'
expect_stderr_has "cd failure names the path"  "msh-no-such-dir-xyz"  'cd /msh-no-such-dir-xyz
'
expect_stdout "failed cd leaves the directory unchanged" "$here"  'cd /msh-no-such-dir-xyz
pwd
'
expect_status "cd onto a file fails"                    1         'cd /etc/hostname
'
expect_status "cd with too many arguments fails"        1         'cd /tmp /etc
'
expect_stdout "too-many-args cd does not move"          "$here"   'cd /tmp /etc
pwd
'
expect_stderr_has "cd too many arguments reported"  "too many"    'cd /tmp /etc
'

# --- help -----------------------------------------------------------------
expect_status "help succeeds"                           0         'help
'
run 'help
'
for word in cd pwd exit help PATH; do
    if grep -q "$word" "$tmpout"; then
        pass "help mentions $word"
    else
        fail "help mentions $word" "not found in help output"
    fi
done
# help must be honest about what msh is not.
if grep -qi "not bash" "$tmpout"; then
    pass "help states it is not bash"
else
    fail "help states it is not bash" "no such disclaimer"
fi
if grep -qiE "compatib" "$tmpout"; then
    fail "help avoids compatibility claims" "mentions compatibility"
else
    pass "help avoids compatibility claims"
fi
expect_status "help rejects extra arguments"            1         'help extra
'

# --- exit -----------------------------------------------------------------
expect_status "exit with no argument uses last status"  0         '/bin/true
exit
'
expect_status "exit after a failure carries its status" 1         '/bin/false
exit
'
expect_status "exit 7"                                  7         'exit 7
'
expect_status "exit 0"                                  0         'exit 0
'
expect_status "exit 255"                                255       'exit 255
'
expect_status "exit 256 masks to 0"                     0         'exit 256
'
expect_status "exit 300 masks to 44"                    44        'exit 300
'

# Invalid status: a usage error, and the shell does NOT exit.
expect_status "exit with a non-numeric argument"        2         'exit abc
'
expect_stderr_has "exit reports numeric requirement"  "numeric"   'exit abc
'
expect_stdout "shell survives an invalid exit argument" "alive"   'exit abc
/bin/echo alive
'

# Too many arguments: reported, and the shell stays usable.
expect_status "exit with too many arguments"            1         'exit 1 2
'
expect_stderr_has "exit too many arguments reported" "too many"   'exit 1 2
'
expect_stdout "shell survives too-many-args exit"       "alive"   'exit 1 2
/bin/echo alive
'

# --- exit actually stops the shell ---------------------------------------
expect_stdout "commands before exit run, after exit do not"  "before"  '/bin/echo before
exit 0
/bin/echo after
'
expect_status "status comes from exit, not later lines"      3     '/bin/echo before
exit 3
/bin/echo after
'

# --- built-ins and external commands coexist -----------------------------
expect_status "external command still works"            0         '/bin/true
'
expect_status "external failure status still works"     1         '/bin/false
'
expect_status "nonexistent command still 127"           127       'msh-no-such-command-xyz
'
expect_stdout "external echo still works"               "hi"      '/bin/echo hi
'
expect_stdout "builtin then external then builtin"      "/tmp
mixed"  'cd /tmp
pwd
/bin/echo mixed
'
expect_status "a path is external, not the builtin"     0         '/bin/pwd
'

# --- blank input preserves the last status -------------------------------
expect_status "blank lines preserve a builtin's status" 1         'cd /msh-no-such-dir-xyz


'
expect_status "blank lines preserve a success status"   0         'pwd

'

# --- EOF ------------------------------------------------------------------
expect_status "eof with no input exits 0"               0         ''
expect_status "eof after a builtin keeps its status"    0         'pwd
'

# --- stderr hygiene -------------------------------------------------------
run 'pwd
'
if [ -s "$tmperr" ]; then
    fail "successful builtins write nothing to stderr" "$(cat "$tmperr")"
else
    pass "successful builtins write nothing to stderr"
fi

run 'cd /msh-no-such-dir-xyz
'
if [ -s "$tmpout" ]; then
    fail "failed cd writes nothing to stdout" "$(cat "$tmpout")"
else
    pass "failed cd writes nothing to stdout"
fi

# --- no zombies from a session mixing builtins and externals -------------
printf '#!/bin/sh\nps -o stat= --ppid %s\n' '$PPID' > "$workdir/zcheck"
chmod 755 "$workdir/zcheck"
printf 'pwd\n/bin/true\ncd /tmp\n/bin/false\n%s\n' "$workdir/zcheck" \
    | "$MSH_ABS" >"$tmpout" 2>"$tmperr"
if [ ! -s "$tmpout" ]; then
    fail "no zombies after builtins and externals" "ps produced no output; inconclusive"
elif grep -q 'Z' "$tmpout"; then
    fail "no zombies after builtins and externals" "ps reported: $(tr '\n' ' ' < "$tmpout")"
else
    pass "no zombies after builtins and externals"
fi

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_builtins: $tests checks passed"
    exit 0
fi
echo "test_builtins: $failures of $tests checks FAILED" >&2
exit 1

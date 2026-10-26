#!/bin/sh
# Valgrind memcheck over the execution paths.
#
# Run via `make valgrind`. Kept out of `make test` because Valgrind is not
# installed everywhere, and it is far too slow for the ordinary edit loop.
#
# NO SUPPRESSION FILE IS USED, deliberately. An earlier version suppressed a
# glibc report of uninitialised bytes passed to execve() on the ENAMETOOLONG
# path. That suppression was removed because it could not be made safe: the
# only things a suppression can match on are the error kind and the call
# stack, and an msh bug that passed an uninitialised filename to execvp()
# would produce exactly the same kind with exactly the same frames. It would
# have masked the very class of defect most worth catching here.
#
# The pathological over-long name is therefore not exercised under Valgrind.
# Nothing is lost: its memory safety is covered by ASan, which runs the whole
# suite — including the 200000-character line in test_repl.sh — with no
# suppressions of any kind, and its functional behaviour is covered there too.
#
# Two flag choices that matter:
#
#   --errors-for-leak-kinds=definite,indirect
#       "Still reachable" is deliberately NOT an error. msh forks, and the
#       child inherits the parent's live allocations; after a failed exec it
#       leaves via _exit() without freeing them, because free() is not
#       async-signal-safe and the memory dies with the process anyway.
#       Counting those as errors also corrupts the test: combined with
#       --error-exitcode it replaces the child's real _exit(127) with the
#       Valgrind error code, so the shell then reports the wrong status.
#
#   --trace-children=no
#       Once the child execs, it is a different program (/bin/true and
#       friends); auditing those is not this project's business.

set -u

MSH_BIN=${MSH_BIN:-out/msh}
failures=0
tests=0

if [ ! -x "$MSH_BIN" ]; then
    echo "test_valgrind: cannot find executable at $MSH_BIN" >&2
    exit 2
fi
if ! command -v valgrind >/dev/null 2>&1; then
    # Exits 0 so a machine without Valgrind is not a build failure, but says
    # SKIPPED on both streams and never prints a "passed" line.
    echo "test_valgrind: SKIPPED - valgrind is not installed" >&2
    echo "test_valgrind: SKIPPED - valgrind is not installed"
    exit 0
fi

VG_COMMON="--error-exitcode=99 --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --track-fds=yes --trace-children=no --quiet"

# run_case NAME EXPECTED_STATUS INPUT
run_case() {
    tests=$((tests + 1))
    log=$(mktemp)
    # shellcheck disable=SC2086
    printf '%s' "$3" | valgrind $VG_COMMON "$MSH_BIN" >/dev/null 2>"$log"
    st=$?
    if [ "$st" -eq 99 ]; then
        printf '  %-46s FAILED (valgrind reported errors)\n' "$1"
        sed -n '1,25p' "$log" | sed 's/^/      /'
        failures=$((failures + 1))
    elif [ "$st" -ne "$2" ]; then
        printf '  %-46s FAILED (want status %s, got %s)\n' "$1" "$2" "$st"
        sed -n '1,25p' "$log" | sed 's/^/      /'
        failures=$((failures + 1))
    else
        printf '  %-46s ok\n' "$1"
    fi
    rm -f "$log"
}

echo "test_valgrind (memcheck + fd tracking, no suppressions)"

run_case "immediate EOF"                0   ''
run_case "blank line forks nothing"     0   '
'
run_case "/bin/true"                    0   '/bin/true
'
run_case "/bin/false"                   1   '/bin/false
'
run_case "echo with arguments"          0   '/bin/echo hello world
'
run_case "PATH lookup"                  0   'true
'
run_case "nonexistent command"          127 'msh-no-such-command-xyz
'
run_case "non-executable file"          126 '/etc/hostname
'
run_case "a directory as a command"     126 '/tmp
'
run_case "many arguments"               0   '/bin/echo a b c d e f g h i j k l m
'
run_case "mixed success and failure"    0   '/bin/false
msh-nope-xyz
/bin/true
'

# Repeated execution must not accumulate anything.
many=$(i=0; while [ "$i" -lt 25 ]; do echo /bin/true; i=$((i + 1)); done)
run_case "25 commands in one session"   0   "$many"

# A long argument, well short of the libc PATH-length limit so that glibc's
# ENAMETOOLONG path is not involved.
longarg=$(awk 'BEGIN { for (i = 0; i < 2000; i++) printf "b" }')
run_case "2000-char argument"           0   "/bin/echo $longarg
"

# Built-ins run in this process, so pwd's getcwd buffer and every other
# allocation they make is msh's own and must be accounted for here.
run_case "pwd"                          0   'pwd
'
run_case "cd then pwd"                  0   'cd /tmp
pwd
'
run_case "cd to a missing directory"    1   'cd /msh-no-such-dir-xyz
'
run_case "cd with too many arguments"   1   'cd /tmp /etc
'
run_case "cd with no argument (HOME)"   0   'cd
'
run_case "help"                         0   'help
'
run_case "exit with an explicit status" 5   'exit 5
'
run_case "exit with no argument"        0   'exit
'
run_case "exit with an invalid status"  2   'exit abc
'
run_case "builtins mixed with externals" 0  'pwd
/bin/true
cd /tmp
/bin/echo x
'
run_case "exit stops the session early" 0   'pwd
exit 0
/bin/echo unreachable
'

# Quoting and syntax errors. The syntax-error cases matter most here: the
# parser allocates words before discovering the fault, so a leaky error path
# would show up as definitely-lost bytes.
run_case "double-quoted argument"       0   '/bin/echo "hello world"
'
run_case "single-quoted argument"       0   "/bin/echo 'hello world'
"
run_case "concatenated segments"        0   '/bin/echo ab"cd"'"'"'ef'"'"'
'
run_case "empty quoted argument"        0   '/bin/echo ""
'
run_case "backslash escapes"            0   '/bin/echo a\ b
'
run_case "quoted cd then pwd"           0   'cd "/tmp"
pwd
'
run_case "unmatched single quote"       2   "/bin/echo 'abc
"
run_case "unmatched double quote"       2   '/bin/echo "abc
'
run_case "trailing backslash"           2   '/bin/echo abc\
'
run_case "syntax error then a command"  0   '/bin/echo "oops
/bin/true
'
run_case "many words with quotes"       0   '/bin/echo a "b c" '"'"'d e'"'"' f "g h" i
'

# Redirection. The failure paths matter most: they allocate, open and then
# abandon, which is where a leaked descriptor or buffer would hide. The
# builtin cases also exercise the parent-side save/restore, which is the only
# code here that keeps descriptors of its own.
run_case "output redirection"           0   '/bin/echo hi > /tmp/msh-vg-out
'
run_case "append redirection"           0   '/bin/echo hi >> /tmp/msh-vg-out
'
run_case "input redirection"            0   '/bin/cat < /tmp/msh-vg-out
'
run_case "both redirections"            0   '/bin/cat < /tmp/msh-vg-out > /tmp/msh-vg-out2
'
run_case "builtin redirected to a file" 0   'pwd > /tmp/msh-vg-out
'
run_case "builtin appended to a file"   0   'help >> /tmp/msh-vg-out
'
run_case "builtin redirect then normal" 0   'pwd > /tmp/msh-vg-out
pwd
'
run_case "input open failure"           1   '/bin/cat < /tmp/msh-vg-nosuch
'
run_case "output open failure"          1   '/bin/echo hi > /tmp/msh-vg-nodir/x
'
run_case "builtin redirect failure"     1   'pwd > /tmp/msh-vg-nodir/x
'
run_case "missing redirect target"      2   '/bin/echo hi >
'
run_case "duplicate output redirect"    2   '/bin/echo hi > a > b
'
run_case "redirect with no command"     2   '> /tmp/msh-vg-never
'
run_case "quoted operator is literal"   0   '/bin/echo ">"
'
run_case "recovery after a bad redirect" 0  '/bin/echo hi > /tmp/msh-vg-nodir/x
/bin/true
'

# Pipelines. Memcheck follows the shell only (--trace-children=no), so what
# is audited here is the parent's own bookkeeping: the pid array, the stage
# vector, and the pipe descriptors it opens and must close before waiting.
run_case "two-stage pipeline"           0   '/usr/bin/printf x | /bin/cat
'
run_case "three-stage pipeline"         0   '/usr/bin/printf x | /bin/cat | /bin/cat
'
run_case "pipeline status is the last" 1   '/bin/true | /bin/false
'
run_case "early failure, last succeeds" 0   '/bin/false | /bin/true
'
run_case "builtin as a pipeline stage"  0   'pwd | /bin/cat
'
run_case "cd stage does not move shell" 0   'cd / | /bin/cat
'
run_case "pipeline with redirection"    0   '/usr/bin/printf x | /bin/cat > /tmp/msh-vg-pipe
'
run_case "pipeline reading a file"      0   '/bin/cat /tmp/msh-vg-pipe | /bin/cat
'
run_case "missing command in a stage"   127 '/usr/bin/printf x | msh-no-such-xyz
'
run_case "empty pipeline stage"         2   '/bin/cat |
'
run_case "leading pipe"                 2   '| /bin/cat
'
run_case "double pipe"                  2   '/bin/cat || /bin/cat
'
run_case "large stream through a pipe"  0   '/usr/bin/head -c 200000 /dev/zero | /usr/bin/wc -c
'
run_case "pipeline then a plain command" 0  '/usr/bin/printf x | /bin/cat
/bin/true
'

# Background jobs and the job table. Memcheck follows the shell only, which
# is the right audit here: what is being checked is the job table, the pid
# arrays and the command strings the shell allocates and must release.
run_case "background launch"            0   '/bin/true &
'
run_case "background then a command"    0   '/bin/sleep 30 &
/bin/true
'
run_case "background pipeline"          0   '/bin/true | /bin/cat &
'
run_case "jobs with one job"            0   '/bin/sleep 30 &
jobs
'
run_case "jobs with none"               0   'jobs
'
run_case "several background jobs"      0   '/bin/true &
/bin/true &
/bin/true &
jobs
'
run_case "a finished job is collected"  0   '/bin/true &
/bin/sleep 0.3
jobs
'
run_case "backgrounded builtin"         0   'cd / &
'
run_case "fg with no jobs"              1   'fg
'
run_case "bg with no jobs"              1   'bg
'
run_case "fg with a bad spec"           1   'fg abc
'
run_case "fg with a missing job"        1   'fg %99
'
run_case "malformed background"         2   '/bin/echo a & b
'
run_case "quoted ampersand"             0   '/bin/echo "&"
'
run_case "exit warns then leaves"       1   '/bin/sleep 30 &
exit
exit
'

# --- file descriptors: a real check, not just a printed number -------------
# Valgrind's --track-fds findings are NOT counted in ERROR SUMMARY, so a
# leaked descriptor would not have failed any check above.
#
# The absolute count is deliberately not asserted: descriptors can be
# inherited from whatever invoked this script, which would make an exact
# number flaky. What is asserted is that msh itself never left one open,
# which is what a leak would require. Valgrind prints an "Open file
# descriptor" block with a stack trace for every non-std descriptor it can
# attribute, so the check is that no such block names msh code.
echo ""
echo "--- file descriptors over 25 commands ---"
fdlog=$(mktemp)
printf '%s' "$many" | valgrind --track-fds=yes --leak-check=full "$MSH_BIN" \
    >/dev/null 2>"$fdlog"
grep -E "FILE DESCRIPTORS|definitely lost|indirectly lost|ERROR SUMMARY" "$fdlog" \
    | sed 's/^/  /'

tests=$((tests + 1))
if grep -q "Open file descriptor" "$fdlog" &&
   grep -A12 "Open file descriptor" "$fdlog" | grep -qE "msh_|src/"; then
    printf '  %-46s FAILED (msh left a descriptor open)\n' \
        "no descriptor opened by msh is left open"
    sed -n '/Open file descriptor/,/^==.*== *$/p' "$fdlog" | sed 's/^/      /' | head -20
    failures=$((failures + 1))
else
    printf '  %-46s ok\n' "no descriptor opened by msh is left open"
fi
rm -f "$fdlog"

echo ""
if [ "$failures" -eq 0 ]; then
    echo "test_valgrind: $tests checks passed"
    exit 0
fi
echo "test_valgrind: $failures of $tests checks FAILED" >&2
exit 1

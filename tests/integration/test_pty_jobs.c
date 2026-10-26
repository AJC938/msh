/*
 * Interactive job control, at a real terminal.
 *
 * None of this can be tested through a pipe. Ctrl+C and Ctrl+Z are not
 * characters a program reads: the terminal line discipline turns them into
 * SIGINT and SIGTSTP delivered to the terminal's FOREGROUND PROCESS GROUP.
 * A shell that never called tcsetpgrp() would therefore pass a piped test
 * and still be unusable, so this suite drives msh through a pty.
 *
 * Synchronisation is by waiting for output, never by sleeping for a guessed
 * interval: commands announce themselves with a marker, and every read has a
 * bounded timeout whose expiry is a failure. A deadlocked or unresponsive
 * shell fails the test instead of hanging the suite.
 *
 * Linux-specific: forkpty() lives in <pty.h> and needs -lutil.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PROMPT "msh$ "
#define READ_TIMEOUT_MS 6000
#define KEY_ATTEMPTS 3
#define KEY_ATTEMPT_MS 1500
#define BUFFER_SIZE 65536

static int failures;
static int checks;

static void ok(const char *what)
{
    checks++;
    printf("  %-56s ok\n", what);
}

static char seen[65536];
static size_t seen_len;

static void bad(const char *what, const char *why)
{
    const char *tail = seen;

    checks++;
    failures++;
    printf("  %-56s FAILED (%s)\n", what, why);
    /* The last of what arrived, so a timeout is diagnosable. */
    if (seen_len > 300)
        tail = seen + seen_len - 300;
    printf("      last output: [%s]\n", tail);
}

static void check(int cond, const char *what)
{
    if (cond)
        ok(what);
    else
        bad(what, "condition false");
}

/* Everything read so far, kept so a check can look back over it. */
static void seen_reset(void)
{
    seen_len = 0;
    seen[0] = '\0';
}

/*
 * Read until `want` appears or the timeout expires. Returns 1 on success.
 * A timeout is always a failure: it means the shell stopped responding.
 */
static int wait_for_ms(int fd, const char *want, int budget_ms)
{
    struct timeval deadline_tv;
    fd_set set;
    int elapsed_ms = 0;

    if (want != NULL && strstr(seen, want) != NULL)
        return 1;

    while (elapsed_ms < budget_ms) {
        char buf[1024];
        ssize_t n;
        int ready;

        FD_ZERO(&set);
        FD_SET(fd, &set);
        deadline_tv.tv_sec = 0;
        deadline_tv.tv_usec = 100000; /* 100 ms slices */

        ready = select(fd + 1, &set, NULL, NULL, &deadline_tv);
        if (ready == -1) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (ready == 0) {
            elapsed_ms += 100;
            continue;
        }

        n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            if (n == -1 && errno == EINTR)
                continue;
            return 0; /* the slave side closed */
        }
        if (seen_len + (size_t)n < sizeof seen - 1) {
            memcpy(seen + seen_len, buf, (size_t)n);
            seen_len += (size_t)n;
            seen[seen_len] = '\0';
        }
        if (want != NULL && strstr(seen, want) != NULL)
            return 1;
    }
    return 0;
}

static int wait_for(int fd, const char *want)
{
    return wait_for_ms(fd, want, READ_TIMEOUT_MS);
}

/* Wait for `want`, reporting the check by name. */
static int expect(int fd, const char *want, const char *what)
{
    if (wait_for(fd, want)) {
        ok(what);
        return 1;
    }
    bad(what, "timed out waiting for expected output");
    return 0;
}

static void send_text(int fd, const char *text)
{
    size_t len = strlen(text);
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, text + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == -1 && errno == EINTR)
            continue;
        return;
    }
}

static void send_key(int fd, char c)
{
    while (write(fd, &c, 1) == -1 && errno == EINTR)
        ;
}

/*
 * Send a control key and wait for `want`, retrying the key a bounded number
 * of times.
 *
 * This is NOT a longer timeout hiding a defect. In this environment the INTR
 * byte written to a pty master is intermittently dropped before the line
 * discipline turns it into a signal: a control run of /bin/bash through this
 * same harness had Ctrl+C take effect in 9 of 12 attempts, against 11 of 12
 * for msh. A user whose Ctrl+C does nothing presses it again, so the test
 * does too. The total time stays bounded, and a shell that genuinely ignored
 * the signal, or failed to give the terminal to the job, still fails here
 * because none of the attempts would ever have an effect.
 */
static int send_key_until(int fd, char key, const char *want, const char *what)
{
    int attempt;

    for (attempt = 0; attempt < KEY_ATTEMPTS; attempt++) {
        send_key(fd, key);
        if (wait_for_ms(fd, want, KEY_ATTEMPT_MS)) {
            ok(what);
            return 1;
        }
    }
    bad(what, "the key had no effect after several attempts");
    return 0;
}

/*
 * Run a command that announces itself before blocking, so the test can wait
 * for the marker instead of guessing how long the shell needs.
 *
 * The marker is written split -- REA''DY1 -- so that the terminal's echo of
 * this very command does NOT contain it. sh rejoins the halves, so only the
 * running program's output can match, which is the difference between
 * synchronising on the shell and racing it.
 *
 * `exec sleep` matters. Without it dash forks a second process for the sleep,
 * and on this kernel (WSL2) that fork intermittently wedges in
 * uninterruptible sleep -- /proc showed the group leader in state D at
 * wchan `kernel_clone` with SIGTSTP sitting undeliverable in ShdPnd, while
 * its child had stopped normally. A process in D state cannot be stopped,
 * continued or killed, so the job never finishes becoming stopped and no
 * shell on earth can report it as such. Execing keeps the job to one process
 * per stage and removes that window, rather than waiting longer for a
 * process that is never coming back.
 */
static void start_marked_sleep(int fd, int n)
{
    char line[256];

    snprintf(line, sizeof line,
             "/bin/sh -c \"echo REA''DY%d; exec sleep 30\"\n", n);
    send_text(fd, line);
}

int main(int argc, char **argv)
{
    const char *msh_path;
    int master = -1;
    pid_t child;
    int status = 0;
    pid_t shell_pgid = -1;
    pid_t fg_pgid = -1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s path/to/msh\n", argv[0]);
        return 2;
    }
    msh_path = argv[1];

    printf("test_pty_jobs (interactive job control at a real terminal)\n");

    child = forkpty(&master, NULL, NULL, NULL);
    if (child == -1) {
        perror("forkpty");
        return 2;
    }
    if (child == 0) {
        /* A fresh session with the pty as controlling terminal: forkpty has
         * already done setsid() and the ioctl for us. */
        execl(msh_path, msh_path, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    seen_reset();
    if (!expect(master, PROMPT, "the shell prompts at a real terminal")) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return 1;
    }

    /* The shell must own the terminal before it reads a command. */
    shell_pgid = tcgetpgrp(master);
    check(shell_pgid > 0, "the shell owns the terminal at the prompt");

    /* --- Ctrl+C interrupts a foreground command -------------------------- */
    seen_reset();
    start_marked_sleep(master, 1);
    if (expect(master, "READY1", "a foreground command starts")) {
        /* While it runs the terminal belongs to the JOB, not the shell. */
        fg_pgid = tcgetpgrp(master);
        check(fg_pgid > 0 && fg_pgid != shell_pgid,
              "a foreground job owns the terminal while it runs");

        seen_reset();
        send_key_until(master, 0x03, PROMPT,
                       "Ctrl+C interrupts it and the prompt returns");
        check(tcgetpgrp(master) == shell_pgid,
              "the terminal comes back to the shell after Ctrl+C");
    }

    seen_reset();
    send_text(master, "/bin/echo alive1\n");
    expect(master, "alive1", "the shell still runs commands after Ctrl+C");

    /* --- Ctrl+C interrupts a whole foreground pipeline ------------------- */
    seen_reset();
    send_text(master,
              "/bin/sh -c \"echo REA''DY2; exec sleep 30\" | /bin/cat\n");
    if (expect(master, "READY2", "a foreground pipeline starts")) {
        seen_reset();
        send_key_until(master, 0x03, PROMPT,
                       "Ctrl+C interrupts the whole pipeline");
    }
    seen_reset();
    send_text(master, "/bin/echo alive2\n");
    expect(master, "alive2", "the shell survives interrupting a pipeline");

    /* --- Ctrl+Z stops a foreground job ----------------------------------- */
    seen_reset();
    start_marked_sleep(master, 3);
    if (expect(master, "READY3", "a job to stop starts")) {
        seen_reset();
        send_key_until(master, 0x1a, "Stopped",
                       "Ctrl+Z stops the job and reports it");
        expect(master, PROMPT, "the prompt returns after Ctrl+Z");
        check(tcgetpgrp(master) == shell_pgid,
              "the terminal comes back to the shell after a stop");
    }

    seen_reset();
    send_text(master, "/bin/echo alive3\n");
    expect(master, "alive3", "the shell still runs commands after Ctrl+Z");

    /* --- jobs reports the stopped job ------------------------------------ */
    seen_reset();
    send_text(master, "jobs\n");
    expect(master, "Stopped", "jobs reports the stopped job");

    /* --- bg resumes it in the background --------------------------------- */
    seen_reset();
    send_text(master, "bg\n");
    expect(master, "sleep 30", "bg reports the job it resumed");
    seen_reset();
    send_text(master, "jobs\n");
    expect(master, "Running", "jobs shows the job running again after bg");
    /* The prompt, not the output, is what proves the shell holds the tty. */
    if (wait_for(master, PROMPT)) {
        check(tcgetpgrp(master) == shell_pgid,
              "a backgrounded job does not take the terminal");
    } else {
        bad("a backgrounded job does not take the terminal",
            "the prompt never came back");
    }

    /* --- fg brings it back, Ctrl+C ends it ------------------------------- */
    seen_reset();
    send_text(master, "fg\n");
    /* fg echoes the command it is resuming. */
    if (expect(master, "sleep 30", "fg reports the job it resumed")) {
        seen_reset();
        send_key_until(master, 0x03, PROMPT,
                       "Ctrl+C ends the resumed foreground job");
        check(tcgetpgrp(master) == shell_pgid,
              "the terminal is restored after fg plus Ctrl+C");
    }

    seen_reset();
    send_text(master, "/bin/echo alive4\n");
    expect(master, "alive4", "the shell is usable after the fg transition");

    seen_reset();
    send_text(master, "jobs\n");
    send_text(master, "/bin/echo listed\n");
    if (wait_for(master, "listed")) {
        check(strstr(seen, "Stopped") == NULL && strstr(seen, "Running") == NULL,
              "the finished job is gone from the table");
    } else {
        bad("the finished job is gone from the table", "timed out");
    }

    /* --- every pipeline stage shares one process group ------------------- */
    seen_reset();
    send_text(master,
              "/bin/sh -c \"ps -o pgid= -p $$\" | /bin/cat\n");
    if (wait_for(master, PROMPT)) {
        /*
         * One stage printed its process group through the pipe. It must not
         * be the shell's: a job gets a group of its own.
         */
        long printed = -1;
        const char *p = seen;

        while (*p != '\0' && (*p < '0' || *p > '9'))
            p++;
        if (*p != '\0')
            printed = strtol(p, NULL, 10);
        check(printed > 0 && printed != (long)shell_pgid,
              "a pipeline stage runs in its own process group");
    } else {
        bad("a pipeline stage runs in its own process group", "timed out");
    }

    /* --- a background job is announced and does not block ---------------- */
    seen_reset();
    send_text(master, "/bin/sleep 30 &\n");
    expect(master, "[", "a background job is announced");
    seen_reset();
    send_text(master, "/bin/echo alive5\n");
    expect(master, "alive5", "the prompt is usable while a job runs");
    /*
     * Wait for the prompt before sampling ownership. `/bin/echo` is an
     * external foreground job: it owns the terminal while it prints, so its
     * output arriving says nothing about who owns the terminal now. msh
     * prints the prompt only after reaping the job and calling tcsetpgrp
     * back to itself, so the prompt is the synchronisation point. Sampling on
     * the output instead made this check fail about one run in four.
     */
    if (wait_for(master, PROMPT)) {
        check(tcgetpgrp(master) == shell_pgid,
              "a background job never takes the terminal");
    } else {
        bad("a background job never takes the terminal",
            "the prompt never came back");
    }

    /* --- exit refuses once while a job is running ------------------------ */
    seen_reset();
    send_text(master, "exit\n");
    expect(master, "still running", "exit warns about running jobs");
    seen_reset();
    send_text(master, "exit\n");

    /* The shell should now leave. Reap it, with a bounded wait. */
    {
        int waited_ms = 0;
        int done = 0;

        while (waited_ms < READ_TIMEOUT_MS) {
            pid_t w = waitpid(child, &status, WNOHANG);

            if (w == child) {
                done = 1;
                break;
            }
            if (w == -1 && errno == EINTR)
                continue;
            if (w == -1)
                break;
            (void)wait_for(master, NULL); /* drain, 100 ms */
            waited_ms += 100;
        }
        if (!done) {
            bad("a second exit ends the shell", "it did not exit in time");
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        } else {
            ok("a second exit ends the shell");
        }
    }

    close(master);

    printf("\n");
    if (failures == 0) {
        printf("test_pty_jobs: %d checks passed\n", checks);
        return 0;
    }
    fprintf(stderr, "test_pty_jobs: %d of %d checks FAILED\n", failures, checks);
    return 1;
}

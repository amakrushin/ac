#include "greatest.h"
#include "proc.h"

#include <string.h>
#include <unistd.h>

static ssize_t read_until_eof(Proc *p, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = proc_read_stdout(p, buf + total, cap - 1 - total);
        if (n == 0) break;
        if (n < 0) return -1;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

TEST proc_spawn_echo_reads_stdout_and_exits(void) {
    const char *argv[] = {"/bin/sh", "-c", "echo hello", NULL};
    ProcSpawnOpts opts = { .cwd = NULL, .argv = argv, .env = NULL };
    Proc *p = proc_spawn(&opts);
    ASSERT(p != NULL);
    ASSERT(proc_pid(p) > 0);

    char out[64];
    ssize_t n = read_until_eof(p, out, sizeof(out));
    ASSERT(n >= 6); /* "hello\n" plus possibly more */
    ASSERT(strstr(out, "hello") != NULL);

    int status = -42;
    ASSERT_EQ(0, proc_wait(p, &status));
    ASSERT_EQ(0, status);

    proc_free(p);
    PASS();
}

TEST proc_write_stdin_then_read_stdout(void) {
    /* cat with no args echoes stdin to stdout until EOF. */
    const char *argv[] = {"/bin/cat", NULL};
    ProcSpawnOpts opts = { .argv = argv };
    Proc *p = proc_spawn(&opts);
    ASSERT(p != NULL);

    const char msg[] = "round-trip\n";
    ssize_t w = proc_write_stdin(p, msg, sizeof(msg) - 1);
    ASSERT_EQ((ssize_t)(sizeof(msg) - 1), w);

    ASSERT_EQ(0, proc_close_stdin(p)); /* signals EOF to cat */

    char out[64];
    ssize_t n = read_until_eof(p, out, sizeof(out));
    ASSERT(n >= (ssize_t)(sizeof(msg) - 1));
    ASSERT(strstr(out, "round-trip") != NULL);

    int status = -42;
    ASSERT_EQ(0, proc_wait(p, &status));
    ASSERT_EQ(0, status);

    proc_free(p);
    PASS();
}

TEST proc_spawn_nonexistent_returns_null(void) {
    const char *argv[] = {"/no/such/binary-xyz", NULL};
    ProcSpawnOpts opts = { .argv = argv };
    Proc *p = proc_spawn(&opts);
    /* posix_spawnp may report ENOENT either at spawn or via child exit status;
     * accept either: NULL here, or non-NULL with non-zero exit. */
    if (p) {
        int status = 0;
        proc_wait(p, &status);
        ASSERT(status != 0);
        proc_free(p);
    }
    PASS();
}

TEST proc_kill_terminates_long_running(void) {
    const char *argv[] = {"/bin/sh", "-c", "sleep 30", NULL};
    ProcSpawnOpts opts = { .argv = argv };
    Proc *p = proc_spawn(&opts);
    ASSERT(p != NULL);

    ASSERT_EQ(0, proc_kill(p, 15 /*SIGTERM*/));

    int status = 0;
    ASSERT_EQ(0, proc_wait(p, &status));
    /* terminated by signal => negative status by our convention */
    ASSERT(status < 0);
    proc_free(p);
    PASS();
}

SUITE(proc) {
    RUN_TEST(proc_spawn_echo_reads_stdout_and_exits);
    RUN_TEST(proc_write_stdin_then_read_stdout);
    RUN_TEST(proc_spawn_nonexistent_returns_null);
    RUN_TEST(proc_kill_terminates_long_running);
}

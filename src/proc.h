#ifndef AC_PROC_H
#define AC_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct Proc Proc;

typedef struct {
    const char *cwd;
    const char *const *argv;
    const char *const *env;
} ProcSpawnOpts;

Proc *proc_spawn(const ProcSpawnOpts *opts);

/* Wrap a pre-existing pair of file descriptors as a Proc. Used by tests that
 * drive a "child" via a pipe pair without actually fork/exec'ing. The Proc
 * takes ownership of both fds. proc_kill/proc_wait become no-ops (pid is -1). */
Proc *proc_attach(int in_fd, int out_fd);

ssize_t proc_read_stdout(Proc *p, void *buf, size_t cap);
ssize_t proc_write_stdin (Proc *p, const void *buf, size_t len);
int     proc_close_stdin (Proc *p);

int  proc_kill(Proc *p, int signal);
int  proc_wait(Proc *p, int *exit_status_out);

pid_t proc_pid(const Proc *p);

void proc_free(Proc *p);

#endif

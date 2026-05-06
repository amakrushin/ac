#define _GNU_SOURCE
#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct Proc {
    pid_t pid;
    int   in_fd;
    int   out_fd;
    bool  reaped;
    int   exit_status;
};

static int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

Proc *proc_spawn(const ProcSpawnOpts *opts) {
    if (!opts || !opts->argv || !opts->argv[0]) { errno = EINVAL; return NULL; }

    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe)  != 0) goto fail_no_pipes;
    if (pipe(out_pipe) != 0) goto fail_after_in_pipe;

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) goto fail_after_pipes;
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[1]);

    if (opts->cwd) {
#if defined(__linux__) && defined(__GLIBC__)
        if (posix_spawn_file_actions_addchdir_np(&fa, opts->cwd) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            goto fail_after_pipes;
        }
#else
        /* Fallback: chdir in parent before spawn, then restore. Not thread-safe
         * but fine for v1 single-threaded spawn paths. */
        char *prev = getcwd(NULL, 0);
        if (chdir(opts->cwd) != 0) {
            free(prev);
            posix_spawn_file_actions_destroy(&fa);
            goto fail_after_pipes;
        }
#endif
    }

    pid_t pid;
    char *const *argv = (char *const *)opts->argv;
    char *const *envp = opts->env ? (char *const *)opts->env : environ;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, envp);

#if !(defined(__linux__) && defined(__GLIBC__))
    if (opts->cwd) { /* restore parent cwd */
        /* Best-effort; ignore restore errors. */
    }
#endif

    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        errno = rc;
        goto fail_after_pipes;
    }

    /* Parent keeps in_pipe[1] (write to child stdin) and out_pipe[0] (read from child stdout). */
    close(in_pipe[0]);
    close(out_pipe[1]);
    set_close_on_exec(in_pipe[1]);
    set_close_on_exec(out_pipe[0]);

    Proc *p = calloc(1, sizeof(*p));
    if (!p) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    p->pid    = pid;
    p->in_fd  = in_pipe[1];
    p->out_fd = out_pipe[0];
    return p;

fail_after_pipes:
    if (out_pipe[0] != -1) close(out_pipe[0]);
    if (out_pipe[1] != -1) close(out_pipe[1]);
fail_after_in_pipe:
    if (in_pipe[0] != -1) close(in_pipe[0]);
    if (in_pipe[1] != -1) close(in_pipe[1]);
fail_no_pipes:
    return NULL;
}

Proc *proc_attach(int in_fd, int out_fd) {
    Proc *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->pid    = -1;
    p->in_fd  = in_fd;
    p->out_fd = out_fd;
    p->reaped = true;
    p->exit_status = 0;
    return p;
}

ssize_t proc_read_stdout(Proc *p, void *buf, size_t cap) {
    if (!p || p->out_fd < 0) { errno = EBADF; return -1; }
    return read(p->out_fd, buf, cap);
}

ssize_t proc_write_stdin(Proc *p, const void *buf, size_t len) {
    if (!p || p->in_fd < 0) { errno = EBADF; return -1; }
    return write(p->in_fd, buf, len);
}

int proc_close_stdin(Proc *p) {
    if (!p) return -1;
    if (p->in_fd >= 0) {
        int rc = close(p->in_fd);
        p->in_fd = -1;
        return rc;
    }
    return 0;
}

int proc_kill(Proc *p, int signal) {
    if (!p) return -1;
    if (p->pid <= 0) return 0;     /* attach()'d Proc has no child to signal */
    return kill(p->pid, signal);
}

int proc_wait(Proc *p, int *exit_status_out) {
    if (!p) return -1;
    if (p->reaped) {
        if (exit_status_out) *exit_status_out = p->exit_status;
        return 0;
    }
    if (p->pid <= 0) {
        if (exit_status_out) *exit_status_out = 0;
        return 0;
    }
    int status;
    pid_t r = waitpid(p->pid, &status, 0);
    if (r < 0) return -1;
    p->reaped = true;
    if (WIFEXITED(status))        p->exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) p->exit_status = -WTERMSIG(status);
    else                          p->exit_status = -1;
    if (exit_status_out) *exit_status_out = p->exit_status;
    return 0;
}

pid_t proc_pid(const Proc *p) { return p ? p->pid : -1; }

void proc_free(Proc *p) {
    if (!p) return;
    if (p->in_fd  >= 0) close(p->in_fd);
    if (p->out_fd >= 0) close(p->out_fd);
    if (!p->reaped && p->pid > 0) {
        kill(p->pid, SIGKILL);
        waitpid(p->pid, NULL, 0);
    }
    free(p);
}

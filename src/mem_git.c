#define _GNU_SOURCE
#include "mem_git.h"
#include "nash_limits.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>    /* fork, execvp, dup2, chdir, _exit */
#include <sys/wait.h>  /* waitpid */
#include <fcntl.h>     /* open, O_WRONLY */
#include <signal.h>    /* kill, SIGKILL */
#include <time.h>      /* clock_gettime, nanosleep */

/* ── git version control for memory store ──────────────────────── */

/* FIX: Added 30s timeout to prevent TUI hang when git blocks on
 * lock contention, network (if remote), or filesystem issues.
 * Uses WNOHANG polling with 100ms sleep intervals. */
int memory_git_run(memory_t *m, const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: chdir to memory dir, suppress output */
        setsid();
        if (chdir(m->dir) != 0) _exit(1);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Wait with 30s timeout */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status;
    while (1) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed > 30.0) {
            kill(-pid, SIGKILL);   /* negative pid = entire process group */
            waitpid(pid, &status, 0);
            return -1;
        }
        struct timespec sl = {0, 100000000};  /* 100ms */
        nanosleep(&sl, NULL);
    }
}

void memory_git_init(memory_t *m) {
    if (!m) return;
    char git_path[NASH_PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", m->dir);
    struct stat st;
    if (stat(git_path, &st) == 0) return;  /* already initialized */

    const char *init_argv[] = {"git", "init", "-q", NULL};
    if (memory_git_run(m, init_argv) != 0) return;

    /* Configure user for commits (required by git) */
    const char *name_argv[] = {"git", "config", "user.name", "nash", NULL};
    memory_git_run(m, name_argv);
    const char *email_argv[] = {"git", "config", "user.email", "nash@localhost", NULL};
    memory_git_run(m, email_argv);

    /* Initial commit with any existing files */
    const char *add_argv[] = {"git", "add", "-A", NULL};
    memory_git_run(m, add_argv);
    const char *commit_argv[] = {"git", "commit", "-q", "--allow-empty",
                                  "-m", "memory: initialize memory store", NULL};
    memory_git_run(m, commit_argv);
}

void memory_git_commit(memory_t *m, const char *msg) {
    if (!m || !msg) return;
    /* In deferred mode, skip individual commits — they'll be batched. */
    if (m->git_deferred) { m->git_deferred_count++; return; }
    char git_path[NASH_PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", m->dir);
    struct stat st;
    if (stat(git_path, &st) != 0) return;  /* no git repo */

    const char *add_argv[] = {"git", "add", "-A", NULL};
    memory_git_run(m, add_argv);

    /* Append model signoff if available (like /dream's Consolidated-by:) */
    char full_msg[1024];
    if (m->model) {
        snprintf(full_msg, sizeof(full_msg), "%s\n\nStored-by: %s", msg, m->model);
    } else {
        snprintf(full_msg, sizeof(full_msg), "%s", msg);
    }

    const char *commit_argv[] = {"git", "commit", "-q", "--allow-empty-message",
                                  "-m", full_msg, NULL};
    memory_git_run(m, commit_argv);
}

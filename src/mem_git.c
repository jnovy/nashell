#define _GNU_SOURCE
#include "mem_git.h"
#include "subprocess.h"
#include "nash_limits.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ── git version control for memory store ──────────────────────── */

int memory_git_run(memory_t *m, const char *const argv[]) {
    return subprocess_run_silent((char *const *)argv, m->dir, 30);
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

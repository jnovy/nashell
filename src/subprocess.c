#define _GNU_SOURCE
#include "subprocess.h"
#include "nash_limits.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* ── sensitive environment variables to scrub in child ───────────── */

static const char *const sensitive_env_vars[] = {
    "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GOOGLE_API_KEY",
    "AZURE_API_KEY", "MISTRAL_API_KEY", "GROQ_API_KEY",
    "TOGETHER_API_KEY", "DEEPSEEK_API_KEY", "XAI_API_KEY",
    "OPENROUTER_API_KEY", "REPLICATE_API_TOKEN",
    "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
    "TELEGRAM_BOT_TOKEN", "NASH_API_KEY",
    "GH_TOKEN", "GITHUB_TOKEN",
    NULL
};

static void scrub_env(void) {
    for (int i = 0; sensitive_env_vars[i]; i++)
        unsetenv(sensitive_env_vars[i]);
}

/* ── close all FDs above stderr (except keep_fd if >= 0) ─────────── */

static void close_extra_fds(int keep_fd) {
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    for (int fd = STDERR_FILENO + 1; fd < maxfd; fd++) {
        if (fd != keep_fd)
            close(fd);
    }
}

/* ── child setup (shared by both helpers) ────────────────────────── */

static void child_setup(int pipe_wr, unsigned flags, const char *workdir) {
    setsid();
    if (workdir && chdir(workdir) != 0)
        _exit(1);

    /* stdin from /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0)
        _exit(1);  /* cannot sandbox I/O - abort child */

    dup2(devnull, STDIN_FILENO);

    if (pipe_wr >= 0) {
        /* Capture mode: stdout to pipe, stderr to pipe or /dev/null */
        dup2(pipe_wr, STDOUT_FILENO);
        if (flags & SUBPROCESS_PIPE_STDERR)
            dup2(pipe_wr, STDERR_FILENO);
        else
            dup2(devnull, STDERR_FILENO);
        close(pipe_wr);
    } else {
        /* Silent mode: all output to /dev/null */
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }
    close(devnull);

    /* Close all leaked FDs (pipe_wr already closed/dup'd above) */
    close_extra_fds(-1);

    /* Scrub sensitive environment variables */
    scrub_env();
}

/* ── kill + reap ─────────────────────────────────────────────────── */

static void kill_and_reap(pid_t pid, int *status) {
    kill(-pid, SIGKILL);   /* negative pid = entire process group */
    waitpid(pid, status, 0);
}

/* ── subprocess_run: capture output ──────────────────────────────── */

subprocess_result_t subprocess_run(char *const argv[],
                                   const char *workdir,
                                   int timeout_sec,
                                   int max_bytes,
                                   int max_lines,
                                   unsigned flags,
                                   str_t *out) {
    subprocess_result_t r = { .exit_code = -1 };

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return r;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return r; }

    if (pid == 0) {
        close(pipefd[0]);
        child_setup(pipefd[1], flags, workdir);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    /* Non-blocking for poll-based reading */
    int fl = fcntl(pipefd[0], F_GETFL, 0);
    if (fl >= 0)
        fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        /* Check timeout */
        if (timeout_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) +
                             (now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed > timeout_sec) {
                r.timed_out = 1;
                break;
            }
        }

        /* Check caps */
        if (max_bytes > 0 && out->len >= (size_t)max_bytes) {
            r.output_capped = 1;
            break;
        }
        if (max_lines > 0 && r.line_count >= max_lines) {
            r.output_capped = 1;
            break;
        }

        /* Poll for data with 100ms timeout */
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 100);

        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[NASH_PATH_MAX];
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) break;  /* EOF or error */

            /* Enforce byte cap with partial write */
            if (max_bytes > 0 && (out->len + (size_t)n) > (size_t)max_bytes) {
                size_t remaining = (size_t)max_bytes - out->len;
                if (remaining > 0) str_append(out, buf, remaining);
                /* Count lines in the accepted portion */
                for (size_t i = 0; i < remaining; i++)
                    if (buf[i] == '\n') r.line_count++;
                r.output_capped = 1;
                break;
            }

            str_append(out, buf, (size_t)n);
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] == '\n') r.line_count++;

            /* Check line cap after counting */
            if (max_lines > 0 && r.line_count >= max_lines) {
                r.output_capped = 1;
                break;
            }
        } else if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            /* Pipe closed or error -- drain remaining */
            char buf[NASH_PATH_MAX];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                if (max_bytes > 0 && (out->len + (size_t)n) > (size_t)max_bytes) {
                    size_t remaining = (size_t)max_bytes - out->len;
                    if (remaining > 0) str_append(out, buf, remaining);
                    break;
                }
                str_append(out, buf, (size_t)n);
                for (ssize_t i = 0; i < n; i++)
                    if (buf[i] == '\n') r.line_count++;
            }
            break;
        } else if (pr == 0) {
            /* Poll timeout -- check if child exited */
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) {
                /* Child exited -- drain remaining output */
                char buf[NASH_PATH_MAX];
                ssize_t n;
                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    if (max_bytes > 0 && (out->len + (size_t)n) > (size_t)max_bytes) {
                        size_t remaining = (size_t)max_bytes - out->len;
                        if (remaining > 0) str_append(out, buf, remaining);
                        break;
                    }
                    str_append(out, buf, (size_t)n);
                    for (ssize_t i = 0; i < n; i++)
                        if (buf[i] == '\n') r.line_count++;
                }
                close(pipefd[0]);
                r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                return r;
            }
        } else if (pr < 0 && errno != EINTR) {
            break;  /* poll error */
        }
    }

    close(pipefd[0]);

    /* Kill if we broke out early (timeout or cap) */
    if (r.timed_out || r.output_capped) {
        int status;
        kill_and_reap(pid, &status);
        r.exit_code = r.timed_out ? -2 : 0;  /* output was capped but command was OK */
        return r;
    }

    int status;
    waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

/* ── subprocess_run_silent: no output capture ────────────────────── */

int subprocess_run_silent(char *const argv[],
                          const char *workdir,
                          int timeout_sec) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        child_setup(-1, 0, workdir);
        execvp(argv[0], argv);
        _exit(127);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status;

    while (1) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (timeout_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) +
                             (now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed > timeout_sec) {
                kill_and_reap(pid, &status);
                return -2;
            }
        }

        struct timespec sl = {0, 100000000};  /* 100ms */
        nanosleep(&sl, NULL);
    }
}

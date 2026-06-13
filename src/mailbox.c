#include "mailbox.h"
#include "nash_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

/* Forward declaration of tui_on_event from frontend_tui.c */
extern void tui_on_event(const react_event_t *ev, void *userdata);


/* ── Helpers ──────────────────────────────────────────── */

static int mkdirp(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    if (mkdir(path, 0700) == 0) return 0;
    return -1;
}

const char *mailbox_gen_id(void) {
    static char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%lx%04lx",
             (long)ts.tv_sec, ts.tv_nsec / 100000L);
    return buf;
}

/* Read entire file contents, strip trailing newline/whitespace.
 * Caller frees. Returns NULL on error or empty file. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    /* Strip trailing whitespace/newlines */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) {
        buf[--n] = '\0';
    }
    if (n == 0) { free(buf); return NULL; }
    return buf;
}

/* Atomic write: write to .tmp then rename */
static int write_file_atomic(const char *path, const char *content) {
    char tmp[NASH_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(content, f);
    fputc('\n', f);  /* ensure trailing newline */
    fclose(f);
    return rename(tmp, path);
}


/* ── Init ─────────────────────────────────────────────── */

int mailbox_init(const char *nash_dir, char *mailbox_dir_out, size_t out_size) {
    snprintf(mailbox_dir_out, out_size, "%s/mailbox", nash_dir);
    if (mkdirp(mailbox_dir_out) != 0) return -1;

    char inbox[NASH_PATH_MAX], outbox[NASH_PATH_MAX];
    snprintf(inbox, sizeof(inbox), "%s/inbox", mailbox_dir_out);
    snprintf(outbox, sizeof(outbox), "%s/outbox", mailbox_dir_out);
    if (mkdirp(inbox) != 0) return -1;
    if (mkdirp(outbox) != 0) return -1;
    return 0;
}


/* ── Ask / Answer (user_ask support) ─────────────────── */

char *mailbox_ask(const char *mailbox_dir, const char *question, int timeout_sec) {
    const char *msg_id = mailbox_gen_id();

    /* Write question to outbox as plain text */
    char outpath[NASH_PATH_MAX];
    snprintf(outpath, sizeof(outpath), "%s/outbox/ask_%s", mailbox_dir, msg_id);
    write_file_atomic(outpath, question);

    fprintf(stderr, "[mailbox] question written: %s\n", outpath);
    fprintf(stderr, "[mailbox] waiting for answer: inbox/ask_%s\n", msg_id);

    /* Wait for answer file in inbox via inotify */
    char inbox_dir[NASH_PATH_MAX];
    snprintf(inbox_dir, sizeof(inbox_dir), "%s/inbox", mailbox_dir);

    char answer_file[NASH_PATH_MAX];
    snprintf(answer_file, sizeof(answer_file), "%s/inbox/ask_%s",
             mailbox_dir, msg_id);

    /* Check if answer already exists (race-safe) */
    char *answer = NULL;
    if (access(answer_file, F_OK) == 0) {
        goto read_answer;
    }

    /* Set up inotify */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        fprintf(stderr, "[mailbox] inotify_init failed: %s, falling back to poll\n",
                strerror(errno));
        /* Fallback: poll with stat() every second */
        time_t deadline = timeout_sec > 0 ? time(NULL) + timeout_sec : 0;
        while (1) {
            if (access(answer_file, F_OK) == 0) goto read_answer;
            if (deadline > 0 && time(NULL) >= deadline) {
                fprintf(stderr, "[mailbox] timeout waiting for answer\n");
                return NULL;
            }
            sleep(1);
        }
    }

    int wd = inotify_add_watch(ifd, inbox_dir, IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        fprintf(stderr, "[mailbox] inotify_add_watch failed: %s\n",
                strerror(errno));
        close(ifd);
        return NULL;
    }

    /* Re-check after adding watch (close race window) */
    if (access(answer_file, F_OK) == 0) {
        inotify_rm_watch(ifd, wd);
        close(ifd);
        goto read_answer;
    }

    /* Poll loop with timeout */
    {
        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        int timeout_ms = timeout_sec > 0 ? timeout_sec * 1000 : -1;
        time_t start = time(NULL);

        char expected_name[256];
        snprintf(expected_name, sizeof(expected_name), "ask_%s", msg_id);

        while (1) {
            int remaining_ms = -1;
            if (timeout_ms > 0) {
                int elapsed = (int)(time(NULL) - start);
                remaining_ms = timeout_ms - elapsed * 1000;
                if (remaining_ms <= 0) {
                    fprintf(stderr, "[mailbox] timeout waiting for answer\n");
                    inotify_rm_watch(ifd, wd);
                    close(ifd);
                    return NULL;
                }
            }

            int ret = poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 5000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (ret > 0) {
                /* Drain inotify events */
                char evbuf[4096]
                    __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len = read(ifd, evbuf, sizeof(evbuf));
                if (len > 0) {
                    for (char *ptr = evbuf; ptr < evbuf + len; ) {
                        struct inotify_event *iev = (struct inotify_event *)ptr;
                        if (iev->len > 0 &&
                            strcmp(iev->name, expected_name) == 0) {
                            inotify_rm_watch(ifd, wd);
                            close(ifd);
                            /* Small delay for atomic write to complete */
                            usleep(50000);
                            goto read_answer;
                        }
                        ptr += sizeof(struct inotify_event) + iev->len;
                    }
                }
            }

            /* Periodic check (handles edge cases) */
            if (access(answer_file, F_OK) == 0) {
                inotify_rm_watch(ifd, wd);
                close(ifd);
                goto read_answer;
            }
        }
        inotify_rm_watch(ifd, wd);
        close(ifd);
    }
    return NULL;

read_answer:
    /* Small delay to ensure writer has finished */
    usleep(50000);

    /* Answer is just plain text — the entire file content IS the answer */
    answer = read_file(answer_file);
    if (!answer) {
        fprintf(stderr, "[mailbox] failed to read answer file: %s\n",
                answer_file);
        return NULL;
    }

    /* Clean up processed files */
    unlink(answer_file);
    unlink(outpath);

    fprintf(stderr, "[mailbox] received answer: %.80s%s\n",
            answer, strlen(answer) > 80 ? "..." : "");
    return answer;
}


/* ── Notifications (fire-and-forget) ──────────────────── */

void mailbox_notify(const char *mailbox_dir, const char *event_type,
                    const char *message) {
    const char *msg_id = mailbox_gen_id();
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/outbox/status_%s",
             mailbox_dir, msg_id);

    /* Plain text: "type: message" */
    char content[4096];
    snprintf(content, sizeof(content), "[%s] %s",
             event_type ? event_type : "status",
             message ? message : "");
    write_file_atomic(path, content);
}


/* ── Task result (write back to outbox) ───────────────── */

void mailbox_write_result(const char *mailbox_dir, const char *task_id,
                          const char *result) {
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/outbox/result_%s",
             mailbox_dir, task_id);

    /* Plain text: just the result */
    write_file_atomic(path, result ? result : "(no result)");
    fprintf(stderr, "[mailbox] result written: %s\n", path);
}


/* ── Wait for task (daemon mode) ──────────────────────── */

char *mailbox_wait_task(const char *mailbox_dir, char **task_id_out,
                        int timeout_sec) {
    char inbox_dir[NASH_PATH_MAX];
    snprintf(inbox_dir, sizeof(inbox_dir), "%s/inbox", mailbox_dir);

    /* First check for existing task files */
    DIR *dir = opendir(inbox_dir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "task_", 5) != 0) continue;
            /* Skip .tmp files (partial writes) */
            if (strstr(de->d_name, ".tmp")) continue;
            char path[NASH_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", inbox_dir, de->d_name);
            char *query = read_file(path);
            if (query) {
                /* Extract task ID from filename: task_{id} */
                char *tid = strdup(de->d_name + 5);  /* skip "task_" */
                unlink(path);
                closedir(dir);
                if (task_id_out) *task_id_out = tid;
                else free(tid);
                return query;
            }
        }
        closedir(dir);
    }

    /* No existing tasks — watch with inotify */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        fprintf(stderr, "[mailbox] inotify_init failed: %s\n", strerror(errno));
        return NULL;
    }

    int wd = inotify_add_watch(ifd, inbox_dir, IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        fprintf(stderr, "[mailbox] inotify_add_watch failed: %s\n",
                strerror(errno));
        close(ifd);
        return NULL;
    }

    fprintf(stderr, "[mailbox] daemon: watching %s for tasks...\n", inbox_dir);

    struct pollfd pfd = { .fd = ifd, .events = POLLIN };
    time_t start = time(NULL);

    while (1) {
        int remaining_ms = -1;
        if (timeout_sec > 0) {
            int elapsed = (int)(time(NULL) - start);
            remaining_ms = (timeout_sec - elapsed) * 1000;
            if (remaining_ms <= 0) break;
        }

        int ret = poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 5000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0) {
            char evbuf[4096]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(ifd, evbuf, sizeof(evbuf));
            if (len > 0) {
                for (char *ptr = evbuf; ptr < evbuf + len; ) {
                    struct inotify_event *iev = (struct inotify_event *)ptr;
                    if (iev->len > 0 &&
                        strncmp(iev->name, "task_", 5) == 0 &&
                        !strstr(iev->name, ".tmp")) {
                        char path[NASH_PATH_MAX];
                        snprintf(path, sizeof(path), "%s/%s",
                                 inbox_dir, iev->name);
                        /* Small delay for atomic write */
                        usleep(50000);
                        char *query = read_file(path);
                        if (query) {
                            char *tid = strdup(iev->name + 5);
                            unlink(path);
                            inotify_rm_watch(ifd, wd);
                            close(ifd);
                            if (task_id_out) *task_id_out = tid;
                            else free(tid);
                            return query;
                        }
                    }
                    ptr += sizeof(struct inotify_event) + iev->len;
                }
            }
        }

        /* Periodic directory scan (handles edge cases) */
        dir = opendir(inbox_dir);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, "task_", 5) != 0) continue;
                if (strstr(de->d_name, ".tmp")) continue;
                char path[NASH_PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", inbox_dir, de->d_name);
                char *query = read_file(path);
                if (query) {
                    char *tid = strdup(de->d_name + 5);
                    unlink(path);
                    closedir(dir);
                    inotify_rm_watch(ifd, wd);
                    close(ifd);
                    if (task_id_out) *task_id_out = tid;
                    else free(tid);
                    return query;
                }
            }
            closedir(dir);
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    return NULL;
}


/* ── Event handler (wraps tui_on_event) ───────────────── */

void mailbox_on_event(const react_event_t *ev, void *userdata) {
    mailbox_ctx_t *mbox = (mailbox_ctx_t *)userdata;
    if (!mbox) return;

    switch (ev->type) {
    case REACT_EVENT_USER_ASK: {
        /* Intercept user_ask: write to outbox, block until answer in inbox */
        fprintf(stderr, "\n[mailbox/user_ask] %s\n",
                ev->message ? ev->message : "?");

        char *answer = mailbox_ask(mbox->mailbox_dir, ev->message,
                                   mbox->timeout_sec);
        if (answer && mbox->react_ctx) {
            /* Set answer directly — we're called from the react thread,
             * before the cond_wait, so setting pending=0 makes the
             * react loop skip the wait entirely. */
            free(mbox->react_ctx->user_ask_answer);
            mbox->react_ctx->user_ask_answer = answer;
            mbox->react_ctx->user_ask_pending = 0;
        } else {
            /* Timeout or error — provide empty answer to unblock */
            free(mbox->react_ctx->user_ask_answer);
            mbox->react_ctx->user_ask_answer = strdup(
                answer ? answer : "(no answer — mailbox timeout)");
            mbox->react_ctx->user_ask_pending = 0;
            free(answer);
        }
        return;  /* Don't pass to tui_on_event */
    }

    case REACT_EVENT_DONE: {
        /* Notify that task is done */
        mailbox_notify(mbox->mailbox_dir, "done",
                       ev->message ? ev->message : "task completed");
        break;
    }

    case REACT_EVENT_ERROR: {
        /* Notify on errors */
        mailbox_notify(mbox->mailbox_dir, "error",
                       ev->message ? ev->message : "unknown error");
        break;
    }

    default:
        break;
    }

    /* Pass all events (except intercepted USER_ASK) to tui_on_event
     * for terminal output */
    tui_on_event(ev, (void *)mbox->session_dir);
}

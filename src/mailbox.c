#include "mailbox.h"
#include "nash_limits.h"
#include "nash_log.h"
#include "str.h"
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



const char *mailbox_gen_id(void) {
    static _Thread_local char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof(buf), "%lx%09lx",
             (long)ts.tv_sec, (long)ts.tv_nsec);
    return buf;
}

/* Read entire file contents, strip trailing newline/whitespace.
 * Caller frees. Returns NULL on error or empty file. */
static char *read_file(const char *path) {
    size_t n = 0;
    char *buf = slurp_file(path, &n);
    if (!buf) return NULL;
    /* Strip trailing whitespace/newlines */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) {
        buf[--n] = '\0';
    }
    if (n == 0) { free(buf); return NULL; }
    return buf;
}

/* Atomic write with trailing newline, delegates to write_file() (str.h). */
static int write_file_atomic(const char *path, const char *content) {
    size_t clen = strlen(content);
    char *buf = xmalloc(clen + 2);
    memcpy(buf, content, clen);
    buf[clen] = '\n';
    buf[clen + 1] = '\0';
    int rc = write_file(path, buf, clen + 1);
    free(buf);
    return rc;
}


/* ── Init ─────────────────────────────────────────────── */

int mailbox_init(const char *nash_dir, char *mailbox_dir_out, size_t out_size) {
    snprintf(mailbox_dir_out, out_size, "%s/mailbox", nash_dir);
    if (mkdir_p(mailbox_dir_out, 0700) != 0) return -1;

    char inbox[NASH_PATH_MAX], outbox[NASH_PATH_MAX];
    snprintf(inbox, sizeof(inbox), "%s/inbox", mailbox_dir_out);
    snprintf(outbox, sizeof(outbox), "%s/outbox", mailbox_dir_out);
    if (mkdir_p(inbox, 0700) != 0) return -1;
    if (mkdir_p(outbox, 0700) != 0) return -1;
    return 0;
}


/* ── Ask / Answer (user_ask support) ─────────────────── */

char *mailbox_ask(const char *mailbox_dir, const char *question, int timeout_sec) {
    const char *msg_id = mailbox_gen_id();

    /* Write question to outbox as plain text */
    char outpath[NASH_PATH_MAX];
    snprintf(outpath, sizeof(outpath), "%s/outbox/ask_%s", mailbox_dir, msg_id);
    write_file_atomic(outpath, question);

    nash_log("[mailbox] question written: %s", outpath);
    nash_log("[mailbox] waiting for answer: inbox/ask_%s", msg_id);

    /* Wait for answer file in inbox via inotify */
    char inbox_dir[NASH_PATH_MAX];
    snprintf(inbox_dir, sizeof(inbox_dir), "%s/inbox", mailbox_dir);

    char answer_file[NASH_PATH_MAX];
    snprintf(answer_file, sizeof(answer_file), "%s/inbox/ask_%s",
             mailbox_dir, msg_id);

    /* Check if answer already exists (race-safe: read directly, no TOCTOU) */
    char *answer = NULL;
    answer = read_file(answer_file);
    if (answer) goto got_answer;

    /* Set up inotify */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        nash_log("[mailbox] inotify_init failed: %s, falling back to poll",
                strerror(errno));
        /* Fallback: poll with stat() every second */
        time_t deadline = timeout_sec > 0 ? time(NULL) + timeout_sec : 0;
        while (1) {
            answer = read_file(answer_file);
            if (answer) goto got_answer;
            if (deadline > 0 && time(NULL) >= deadline) {
                nash_log("[mailbox] timeout waiting for answer");
                return NULL;
            }
            sleep(1);
        }
    }

    int wd = inotify_add_watch(ifd, inbox_dir, IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        nash_log("[mailbox] inotify_add_watch failed: %s",
                strerror(errno));
        close(ifd);
        return NULL;
    }

    /* Re-check after adding watch (close race window — read directly) */
    answer = read_file(answer_file);
    if (answer) {
        inotify_rm_watch(ifd, wd);
        close(ifd);
        goto got_answer;
    }

    /* Poll loop with timeout */
    {
        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        time_t start = time(NULL);

        char expected_name[256];
        snprintf(expected_name, sizeof(expected_name), "ask_%s", msg_id);

        while (1) {
            int remaining_ms = -1;
            if (timeout_sec > 0) {
                time_t elapsed = time(NULL) - start;
                long remaining_sec = (long)timeout_sec - (long)elapsed;
                remaining_ms = remaining_sec > 0 ? (int)(remaining_sec > 2000000 ? 2000000000 : remaining_sec * 1000) : 0;
                if (remaining_ms <= 0) {
                    nash_log("[mailbox] timeout waiting for answer");
                    inotify_rm_watch(ifd, wd);
                    close(ifd);
                    return NULL;
                }
            }

            int ret = poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 5000);
            if (ret < 0) {
                if (errno == EINTR) break;  /* signal received — let caller check shutdown */
                break;
            }

            if (ret > 0) {
                /* Drain inotify events */
                char evbuf[NASH_PATH_MAX]
                    __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len = read(ifd, evbuf, sizeof(evbuf));
                if (len > 0) {
                    for (char *ptr = evbuf; ptr < evbuf + len; ) {
                        struct inotify_event *iev = (struct inotify_event *)ptr;
                        if (iev->len > 0 &&
                            strcmp(iev->name, expected_name) == 0) {
                            inotify_rm_watch(ifd, wd);
                            close(ifd);
                            goto read_answer;
                        }
                        ptr += sizeof(struct inotify_event) + iev->len;
                    }
                }
            }

            /* Periodic check (handles edge cases — read directly, no TOCTOU) */
            answer = read_file(answer_file);
            if (answer) {
                inotify_rm_watch(ifd, wd);
                close(ifd);
                goto got_answer;
            }
        }
        inotify_rm_watch(ifd, wd);
        close(ifd);
    }
    return NULL;

read_answer:
    /* Answer is just plain text — the entire file content IS the answer */
    answer = read_file(answer_file);
    if (!answer) {
        nash_log("[mailbox] failed to read answer file: %s",
                answer_file);
        return NULL;
    }

got_answer:
    /* Clean up processed files */
    unlink(answer_file);
    unlink(outpath);

    nash_log("[mailbox] received answer: %.80s%s",
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
    char content[NASH_PATH_MAX];
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
    nash_log("[mailbox] result written: %s", path);
}


/* ── Wait for task (daemon mode) ──────────────────────── */

char *mailbox_wait_task(const char *mailbox_dir, char **task_id_out,
                        int timeout_sec) {
    char inbox_dir[NASH_PATH_MAX];
    snprintf(inbox_dir, sizeof(inbox_dir), "%s/inbox", mailbox_dir);

    /* First check for command files (cmd_*) — return immediately so
     * the daemon loop can handle session reset before processing tasks. */
    DIR *dir = opendir(inbox_dir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "cmd_", 4) == 0 &&
                !strstr(de->d_name, ".tmp")) {
                closedir(dir);
                return NULL;  /* signal daemon to check commands */
            }
        }
        closedir(dir);
    }

    /* Check for existing task files */
    dir = opendir(inbox_dir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "task_", 5) != 0) continue;
            /* Skip .tmp files (partial writes) */
            if (strstr(de->d_name, ".tmp")) continue;
            char path[NASH_PATH_MAX];
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", inbox_dir, de->d_name) >= sizeof(path))
                continue;
            char *query = read_file(path);
            if (query) {
                /* Extract task ID from filename: task_{id} */
                char *tid = xstrdup(de->d_name + 5);  /* skip "task_" */
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
        nash_log("[mailbox] inotify_init failed: %s", strerror(errno));
        return NULL;
    }

    int wd = inotify_add_watch(ifd, inbox_dir, IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        nash_log("[mailbox] inotify_add_watch failed: %s",
                strerror(errno));
        close(ifd);
        return NULL;
    }

    nash_log("[mailbox] daemon: watching %s for tasks...", inbox_dir);

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
            if (errno == EINTR) break;  /* signal received — let caller check shutdown */
            break;
        }

        if (ret > 0) {
            char evbuf[NASH_PATH_MAX]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(ifd, evbuf, sizeof(evbuf));
            if (len > 0) {
                for (char *ptr = evbuf; ptr < evbuf + len; ) {
                    struct inotify_event *iev = (struct inotify_event *)ptr;
                    if (iev->len > 0 &&
                        strncmp(iev->name, "cmd_", 4) == 0 &&
                        !strstr(iev->name, ".tmp")) {
                        /* Command file detected — return NULL to let
                         * the daemon loop handle it. */
                        inotify_rm_watch(ifd, wd);
                        close(ifd);
                        return NULL;
                    }
                    if (iev->len > 0 &&
                        strncmp(iev->name, "task_", 5) == 0 &&
                        !strstr(iev->name, ".tmp")) {
                        char path[NASH_PATH_MAX];
                        if ((size_t)snprintf(path, sizeof(path), "%s/%s",
                                 inbox_dir, iev->name) >= sizeof(path))
                            continue;
                        /* Small delay for atomic write */
                        usleep(50000);
                        char *query = read_file(path);
                        if (query) {
                            char *tid = xstrdup(iev->name + 5);
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
            int has_cmd = 0;
            while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, "cmd_", 4) == 0 &&
                    !strstr(de->d_name, ".tmp")) {
                    has_cmd = 1;
                    break;
                }
            }
            closedir(dir);
            if (has_cmd) {
                inotify_rm_watch(ifd, wd);
                close(ifd);
                return NULL;
            }
        }
        dir = opendir(inbox_dir);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, "task_", 5) != 0) continue;
                if (strstr(de->d_name, ".tmp")) continue;
                char path[NASH_PATH_MAX];
                if ((size_t)snprintf(path, sizeof(path), "%s/%s", inbox_dir, de->d_name) >= sizeof(path))
                    continue;
                char *query = read_file(path);
                if (query) {
                    char *tid = xstrdup(de->d_name + 5);
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
        nash_log("[mailbox/user_ask] %s",
                ev->message ? ev->message : "?");

        char *answer = mailbox_ask(mbox->mailbox_dir, ev->message,
                                   mbox->timeout_sec);
        if (mbox->react_ctx) {
            /* Set answer directly — we're called from the react thread,
             * before the cond_wait, so setting pending=0 makes the
             * react loop skip the wait entirely. */
            free(mbox->react_ctx->user_ask_answer);
            mbox->react_ctx->user_ask_answer = answer ? answer : xstrdup(
                "(no answer — mailbox timeout)");
            mbox->react_ctx->user_ask_pending = 0;
        } else {
            /* No react context — can't deliver answer, just clean up */
            nash_log("[mailbox] user_ask: no react context to deliver answer");
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
        /* Suppress transient retry/recovery notifications — these are
         * expected recovery attempts that resolve on their own.  Showing
         * them in Matrix/Telegram just creates noise for the user.
         * Only forward terminal errors ("giving up", auth failures). */
        if (ev->message) {
            if (strstr(ev->message, "plain retry"))
                break;  /* tier 0 retry — suppress */
            if (strstr(ev->message, "evicted") ||
                strstr(ev->message, "removing last exchange") ||
                strstr(ev->message, "stripping") ||
                strstr(ev->message, "skipping tier"))
                break;  /* recovery tier in progress — suppress */
        }
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


/* ── Extended mailbox protocol (workspace routing) ────────── */

char *mailbox_parse_headers(char *content, char **workspace_out,
                            char **route_token_out,
                            char **user_query_out) {
    if (workspace_out) *workspace_out = NULL;
    if (route_token_out) *route_token_out = NULL;
    if (user_query_out) *user_query_out = NULL;
    if (!content) return content;

    /* Quick check: does this look like it has headers?
     * Headers must start with "X-" at the beginning of the content. */
    if (strncmp(content, "X-", 2) != 0)
        return content;  /* no headers — entire content is the query */

    /* Parse lines until we hit "---" separator or a non-header line */
    char *p = content;
    while (*p) {
        /* Find end of current line */
        char *eol = strchr(p, '\n');
        if (!eol) break;  /* no newline found — treat rest as query */

        /* Check for --- separator */
        if (p[0] == '-' && p[1] == '-' && p[2] == '-' &&
            (p[3] == '\n' || p[3] == '\r' || p[3] == '\0')) {
            /* Skip past separator + newline */
            char *query = eol + 1;
            return query;
        }

        /* Parse "X-Key: value" header */
        if (strncmp(p, "X-", 2) == 0) {
            char *colon = strchr(p, ':');
            if (colon && colon < eol) {
                /* Extract key */
                size_t klen = (size_t)(colon - p);
                /* Extract value (skip ": ") */
                char *val = colon + 1;
                while (*val == ' ') val++;
                size_t vlen = (size_t)(eol - val);
                /* Strip trailing whitespace */
                while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == ' '))
                    vlen--;

                if (klen == 11 && strncmp(p, "X-Workspace", 11) == 0) {
                    if (workspace_out && vlen > 0)
                        *workspace_out = strndup(val, vlen);
                } else if (klen == 13 && strncmp(p, "X-Route-Token", 13) == 0) {
                    if (route_token_out && vlen > 0)
                        *route_token_out = strndup(val, vlen);
                } else if (klen == 12 && strncmp(p, "X-User-Query", 12) == 0) {
                    if (user_query_out && vlen > 0)
                        *user_query_out = strndup(val, vlen);
                }
            }
        } else {
            /* Not a header line and not --- — no headers present.
             * Return entire content as query. */
            return content;
        }

        p = eol + 1;
    }

    /* Reached end without --- separator — treat entire content as query */
    return content;
}

void mailbox_task_free(mailbox_task_t *task) {
    if (!task) return;
    free(task->task_id);
    free(task->workspace);
    free(task->route_token);
    /* task->query points into the same allocation as the original content,
     * but we strdup it in wait_task_ex, so free it */
    free(task->query);
    free(task);
}

mailbox_task_t *mailbox_wait_task_ex(const char *mailbox_dir, int timeout_sec) {
    char *task_id = NULL;
    char *raw_query = mailbox_wait_task(mailbox_dir, &task_id, timeout_sec);
    if (!raw_query) return NULL;

    mailbox_task_t *task = xcalloc(1, sizeof(*task));
    if (!task) {
        free(raw_query);
        free(task_id);
        return NULL;
    }
    task->task_id = task_id;

    /* Parse metadata headers from the raw content */
    char *ws = NULL, *rt = NULL;
    char *query_start = mailbox_parse_headers(raw_query, &ws, &rt, NULL);
    task->workspace = ws;
    task->route_token = rt;

    /* query_start points into raw_query — strdup for independent ownership */
    task->query = xstrdup(query_start);
    free(raw_query);

    return task;
}

void mailbox_write_result_routed(const char *mailbox_dir, const char *task_id,
                                 const char *result, const char *route_token,
                                 const char *workspace,
                                 const char *user_query) {
    if ((!route_token || !route_token[0]) &&
        (!workspace || !workspace[0])) {
        /* No routing info at all — use plain write */
        mailbox_write_result(mailbox_dir, task_id, result);
        return;
    }

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/outbox/result_%s",
             mailbox_dir, task_id);

    /* Build content with metadata headers.
     * Format:
     *   X-Route-Token: <room_id>     (optional)
     *   X-Workspace: <name>          (optional)
     *   X-User-Query: <first line>   (optional, truncated for header safety)
     *   ---
     *   <result text>
     */
    const char *res = result ? result : "(no result)";
    str_t hdr = str_new(512);
    if (route_token && route_token[0])
        str_appendf(&hdr, "X-Route-Token: %s\n", route_token);
    if (workspace && workspace[0])
        str_appendf(&hdr, "X-Workspace: %s\n", workspace);
    if (user_query && user_query[0]) {
        /* Truncate query to first line, max 200 chars for header safety */
        char qbuf[201];
        size_t qlen = strlen(user_query);
        const char *nl = strchr(user_query, '\n');
        if (nl && (size_t)(nl - user_query) < qlen) qlen = (size_t)(nl - user_query);
        if (qlen > 200) qlen = 200;
        memcpy(qbuf, user_query, qlen);
        qbuf[qlen] = '\0';
        str_appendf(&hdr, "X-User-Query: %s\n", qbuf);
    }
    str_append_cstr(&hdr, "---\n");

    size_t total = hdr.len + strlen(res) + 2;
    char *buf = xmalloc(total);
    if (!buf) {
        str_free(&hdr);
        mailbox_write_result(mailbox_dir, task_id, result);
        return;
    }
    snprintf(buf, total, "%s%s", hdr.data, res);
    str_free(&hdr);

    write_file_atomic(path, buf);
    free(buf);
    nash_log("[mailbox] routed result written: %s (token=%s, ws=%s)",
            path, route_token ? route_token : "(none)",
            workspace ? workspace : "(none)");
}


/* ── Extended header parsing (session threading) ──────────── */

char *mailbox_parse_headers_full(char *content, char **workspace_out,
                                 char **route_token_out,
                                 char **user_query_out,
                                 char **thread_action_out,
                                 char **source_out,
                                 char **agent_name_out) {
    if (thread_action_out) *thread_action_out = NULL;
    if (source_out) *source_out = NULL;
    if (agent_name_out) *agent_name_out = NULL;
    if (workspace_out) *workspace_out = NULL;
    if (route_token_out) *route_token_out = NULL;
    if (user_query_out) *user_query_out = NULL;
    if (!content) return content;

    if (strncmp(content, "X-", 2) != 0)
        return content;

    char *p = content;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (!eol) break;

        if (p[0] == '-' && p[1] == '-' && p[2] == '-' &&
            (p[3] == '\n' || p[3] == '\r' || p[3] == '\0')) {
            return eol + 1;
        }

        if (strncmp(p, "X-", 2) == 0) {
            char *colon = strchr(p, ':');
            if (colon && colon < eol) {
                size_t klen = (size_t)(colon - p);
                char *val = colon + 1;
                while (*val == ' ') val++;
                size_t vlen = (size_t)(eol - val);
                while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == ' '))
                    vlen--;

                if (klen == 11 && strncmp(p, "X-Workspace", 11) == 0) {
                    if (workspace_out && vlen > 0)
                        *workspace_out = strndup(val, vlen);
                } else if (klen == 13 && strncmp(p, "X-Route-Token", 13) == 0) {
                    if (route_token_out && vlen > 0)
                        *route_token_out = strndup(val, vlen);
                } else if (klen == 12 && strncmp(p, "X-User-Query", 12) == 0) {
                    if (user_query_out && vlen > 0)
                        *user_query_out = strndup(val, vlen);
                } else if (klen == 15 && strncmp(p, "X-Thread-Action", 15) == 0) {
                    if (thread_action_out && vlen > 0)
                        *thread_action_out = strndup(val, vlen);
                } else if (klen == 8 && strncmp(p, "X-Source", 8) == 0) {
                    if (source_out && vlen > 0)
                        *source_out = strndup(val, vlen);
                } else if (klen == 12 && strncmp(p, "X-Agent-Name", 12) == 0) {
                    if (agent_name_out && vlen > 0)
                        *agent_name_out = strndup(val, vlen);
                }
            }
        } else {
            return content;
        }

        p = eol + 1;
    }

    return content;
}


/* ── Query notification for session threading ─────────────── */

void mailbox_write_query(const char *mailbox_dir, const char *query_id,
                         const char *query_text, const char *workspace,
                         const char *route_token, const char *thread_action,
                         const char *source, const char *agent_name) {
    if (!mailbox_dir || !query_id) return;

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/outbox/query_%s",
             mailbox_dir, query_id);

    const char *text = query_text ? query_text : "";
    str_t hdr = str_new(256);
    if (workspace && workspace[0])
        str_appendf(&hdr, "X-Workspace: %s\n", workspace);
    if (route_token && route_token[0])
        str_appendf(&hdr, "X-Route-Token: %s\n", route_token);
    if (thread_action && thread_action[0])
        str_appendf(&hdr, "X-Thread-Action: %s\n", thread_action);
    if (source && source[0])
        str_appendf(&hdr, "X-Source: %s\n", source);
    if (agent_name && agent_name[0])
        str_appendf(&hdr, "X-Agent-Name: %s\n", agent_name);
    str_append_cstr(&hdr, "---\n");

    size_t total = hdr.len + strlen(text) + 2;
    char *buf = xmalloc(total);
    if (!buf) {
        str_free(&hdr);
        return;
    }
    snprintf(buf, total, "%s%s", hdr.data, text);
    str_free(&hdr);

    write_file_atomic(path, buf);
    free(buf);
    nash_log("[mailbox] query notification written: %s (action=%s)",
            path, thread_action ? thread_action : "none");
}

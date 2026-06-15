#include "tools.h"
#include "tools_registry.h"
#include "nash_limits.h"
#include "memory.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <curl/curl.h>
#include <regex.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

/* ── helpers ─────────────────────────────────────────── */

/* Inject the current step's thought into a params cJSON before journal_append.
 * The thought is stored in ctx->thought by react.c before calling tool_execute.
 * Skip whitespace-only thoughts (e.g. "\n\n" emitted before tool calls). */
static inline void inject_thought(tool_ctx_t *ctx, cJSON *params) {
    if (!ctx->thought || !ctx->thought[0] || !params ||
        cJSON_GetObjectItem(params, "thought"))
        return;
    /* Skip whitespace-only thoughts */
    if (is_whitespace_only(ctx->thought)) return;
    cJSON_AddStringToObject(params, "thought", ctx->thought);
}

static tool_result_t make_result(int success, cJSON *meta, char *ref) {
    return (tool_result_t){ .meta = meta, .store_ref = ref, .success = success };
}

static tool_result_t make_error(const char *msg) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "error", msg);
    return make_result(0, m, NULL);
}

/* ── alias hash map implementation ───────────────────────
 * Chained hash table. Grows when load factor > 0.75.
 * Aliases are short strings like "R1S0", "R1S1", etc.
 * Hash is DJB2 on the alias string.
 */

static unsigned int alias_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s;
        s++;
    }
    return h;
}

static void alias_map_grow(alias_map_t *map) {
    int new_cap = map->capacity * 2;
    if (new_cap == 0) new_cap = 16;
    alias_node_t **new_buckets = calloc((size_t)new_cap, sizeof(alias_node_t *));
    if (!new_buckets) return;  /* keep old table, just keep growing count */

    /* Rehash all entries */
    for (int i = 0; i < map->capacity; i++) {
        alias_node_t *node = map->buckets[i];
        while (node) {
            unsigned int h = alias_hash(node->alias) % (unsigned int)new_cap;
            alias_node_t *next = node->next;
            node->next = new_buckets[h];
            new_buckets[h] = node;
            node = next;
        }
    }
    free(map->buckets);
    map->buckets = new_buckets;
    map->capacity = new_cap;
}

alias_map_t *alias_map_new(void) {
    alias_map_t *map = calloc(1, sizeof(alias_map_t));
    if (!map) return NULL;
    map->capacity = 16;
    map->buckets = calloc((size_t)map->capacity, sizeof(alias_node_t *));
    if (!map->buckets) { free(map); return NULL; }
    map->count = 0;
    map->next_seq = 0;
    return map;
}

void alias_map_free(alias_map_t *map) {
    if (!map) return;
    for (int i = 0; i < map->capacity; i++) {
        alias_node_t *node = map->buckets[i];
        while (node) {
            alias_node_t *next = node->next;
            free(node->alias);
            free(node->hash);
            free(node);
            node = next;
        }
    }
    free(map->buckets);
    free(map);
}

void alias_map_clear(alias_map_t *map) {
    if (!map) return;
    for (int i = 0; i < map->capacity; i++) {
        alias_node_t *node = map->buckets[i];
        while (node) {
            alias_node_t *next = node->next;
            free(node->alias);
            free(node->hash);
            free(node);
            node = next;
        }
        map->buckets[i] = NULL;
    }
    map->count = 0;
    map->next_seq = 0;
}

void *alias_map_insert(alias_map_t *map, const char *alias, const char *hash) {
    if (!map || !alias) return NULL;

    /* Check if alias already exists — update in place */
    unsigned int h = alias_hash(alias) % (unsigned int)map->capacity;
    alias_node_t *node = map->buckets[h];
    while (node) {
        if (strcmp(node->alias, alias) == 0) {
            free(node->hash);
            node->hash = hash ? strdup(hash) : strdup("");
            return NULL;  /* updated, not inserted */
        }
        node = node->next;
    }

    /* Insert new node at head of chain */
    alias_node_t *new_node = malloc(sizeof(alias_node_t));
    if (!new_node) return NULL;
    new_node->alias = strdup(alias);
    new_node->hash = hash ? strdup(hash) : strdup("");
    new_node->next = map->buckets[h];
    map->buckets[h] = new_node;
    map->count++;

    /* Grow if load factor exceeds 0.75 */
    if (map->count > map->capacity * 3 / 4) {
        alias_map_grow(map);
    }

    return new_node;  /* non-NULL means new insertion */
}

const char *alias_map_lookup(alias_map_t *map, const char *alias) {
    if (!map || !alias) return NULL;
    unsigned int h = alias_hash(alias) % (unsigned int)map->capacity;
    alias_node_t *node = map->buckets[h];
    while (node) {
        if (strcmp(node->alias, alias) == 0) {
            return node->hash;
        }
        node = node->next;
    }
    return NULL;
}

/* ── public alias API (thin wrappers over hash map) ───── */

char *tool_register_alias(tool_ctx_t *ctx, const char *hash) {
    if (!ctx || !ctx->aliases) {
        nash_log("[tools] CRITICAL: tool_register_alias called with NULL ctx/aliases");
        return strdup("R?S?");
    }

    char alias_buf[32];
    snprintf(alias_buf, sizeof(alias_buf), "R%dS%d", ctx->react_loop, ctx->aliases->next_seq);
    ctx->aliases->next_seq++;

    alias_map_insert(ctx->aliases, alias_buf, hash ? hash : "");

    /* Create symlink in session directory: R1S0 → ../store/hash */
    if (ctx->session_dir && hash && hash[0]) {
        char link_path[NASH_PATH_MAX];
        char target[NASH_PATH_MAX];
        snprintf(link_path, sizeof(link_path), "%s/%s", ctx->session_dir, alias_buf);
        snprintf(target, sizeof(target), "../../store/%s", hash);
        symlink(target, link_path);  /* ignore EEXIST */
    }

    /* Return a copy of the alias string. Caller must free.
     * Previously returned node->alias which dangled after alias_map_free/clear. */
    return strdup(alias_buf);
}

/* Returns heap-allocated path string — caller MUST free.
 * Returns NULL if alias doesn't resolve. */
char *tool_resolve_alias(tool_ctx_t *ctx, const char *alias) {
    /* Check if it looks like an alias: R1S1, R1S2, R2S1, ... */
    if (!ctx || !alias || alias[0] != 'R')
        return NULL;
    const char *h = alias_map_lookup(ctx->aliases, alias);
    if (h) {
        return store_resolve(ctx->store, h);  /* heap-allocated, caller frees */
    }
    return NULL;
}

/* ── run_command: fork/execve helper ─────────────────── */

/* ── recalled key tracking (validation scoring) ──────────────── */

/* Cap recalled keys to prevent unbounded growth in long sessions.
 * 512 keys is generous — typical sessions recall <50 distinct memories. */
#define RECALLED_KEYS_MAX 512

void tool_track_recalled_key(tool_ctx_t *ctx, const char *key) {
    if (!ctx || !key) return;
    /* Deduplicate: don't track the same key twice */
    for (int i = 0; i < ctx->n_recalled_keys; i++)
        if (strcmp(ctx->recalled_keys[i], key) == 0) return;
    /* Cap: stop tracking new keys once we reach the limit.
     * The most important keys (recalled earliest) are already tracked. */
    if (ctx->n_recalled_keys >= RECALLED_KEYS_MAX) return;
    /* Grow if needed */
    if (ctx->n_recalled_keys >= ctx->recalled_keys_cap) {
        int new_cap = ctx->recalled_keys_cap ? ctx->recalled_keys_cap * 2 : 16;
        if (new_cap > RECALLED_KEYS_MAX) new_cap = RECALLED_KEYS_MAX;
        char **new_keys = realloc(ctx->recalled_keys,
                                   (size_t)new_cap * sizeof(char *));
        if (!new_keys) return;
        ctx->recalled_keys = new_keys;
        ctx->recalled_keys_cap = new_cap;
    }
    ctx->recalled_keys[ctx->n_recalled_keys++] = strdup(key);
}

/* Run a command with timeout and output cap.
 * timeout_sec: max wall-clock seconds (0 = no limit)
 * max_output:  max bytes to capture (0 = no limit)
 * Returns exit code, or -1 on error, -2 on timeout. */
static int run_command_argv_limited(char *const argv[], str_t *out,
                                    int timeout_sec, int max_output) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    /* Set pipe to non-blocking for poll-based reading */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int timed_out = 0;
    int output_capped = 0;

    while (1) {
        /* Check timeout */
        if (timeout_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) +
                             (now.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed > timeout_sec) {
                timed_out = 1;
                break;
            }
        }

        /* Check output cap */
        if (max_output > 0 && (int)out->len >= max_output) {
            output_capped = 1;
            break;
        }

        /* Poll for data with 100ms timeout */
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[NASH_PATH_MAX];
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) break;  /* EOF or error */
            /* Respect output cap */
            if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                size_t remaining = (size_t)max_output - out->len;
                if (remaining > 0) str_append(out, buf, remaining);
                output_capped = 1;
                break;
            }
            str_append(out, buf, (size_t)n);
        } else if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            /* Pipe closed (child exited) or error — drain any remaining data */
            char buf[NASH_PATH_MAX];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                    size_t remaining = (size_t)max_output - out->len;
                    if (remaining > 0) str_append(out, buf, remaining);
                    break;
                }
                str_append(out, buf, (size_t)n);
            }
            break;
        } else if (pr == 0) {
            /* Timeout on poll — check if child exited */
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) {
                /* Child exited — drain remaining output */
                char buf[NASH_PATH_MAX];
                ssize_t n;
                while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                    if (max_output > 0 && (int)(out->len + (size_t)n) > max_output) {
                        size_t remaining = (size_t)max_output - out->len;
                        if (remaining > 0) str_append(out, buf, remaining);
                        break;
                    }
                    str_append(out, buf, (size_t)n);
                }
                close(pipefd[0]);
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        } else if (pr < 0 && errno != EINTR) {
            break;  /* poll error */
        }
    }

    close(pipefd[0]);

    /* Kill the child if we broke out early */
    if (timed_out || output_capped) {
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        if (timed_out) {
            str_appendf(out, "\n[TIMEOUT: killed after %ds]\n", timeout_sec);
            return -2;
        }
        if (output_capped) {
            str_appendf(out, "\n[OUTPUT CAPPED at %d bytes]\n", max_output);
        }
        /* FIX B1: When output was capped, the child was killed by SIGKILL,
         * so WIFEXITED is false and WEXITSTATUS is undefined (-1).
         * But the command was running successfully — only its output was
         * truncated.  Return 0 (success) instead of the misleading -1. */
        if (output_capped)
            return 0;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── shell_exec ──────────────────────────────────────── */

static tool_result_t tool_shell_exec(tool_ctx_t *ctx, cJSON *params) {
    cJSON *cmd_j = cJSON_GetObjectItem(params, "command");
    if (!cmd_j || !cmd_j->valuestring || !cmd_j->valuestring[0])
        return make_error("shell_exec requires a non-empty 'command' string. "
                          "Provide the shell command to execute.");

    const char *command = cmd_j->valuestring;

    str_t out = str_new(4096);
    char *argv[] = { "sh", "-c", (char *)command, NULL };
    int timeout = ctx->cfg ? ctx->cfg->shell_timeout : 300;
    int max_out = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    int exit_code = run_command_argv_limited(argv, &out, timeout, max_out);

    /* Store to shared store */
    char *hash = store_save(ctx->store, out.data);

    /* Register alias */
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "exit_code", exit_code);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(out.data));
    cJSON_AddStringToObject(meta, "ref", alias);

    /* Fix 3: Conditional preview — saves file_read steps for small outputs */
    if (out.len > 0 && out.len < 500) {
        /* Small output: include full content inline.
         * #19: Check for embedded NUL or invalid bytes that crash JSON. */
        int valid = 1;
        for (size_t vi = 0; vi < out.len; vi++) {
            unsigned char c = (unsigned char)out.data[vi];
            if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
                valid = 0; break;
            }
        }
        if (valid) {
            cJSON_AddStringToObject(meta, "preview", out.data);
        } else {
            /* FIX #11: Use strncat with explicit size guard instead of
             * strcat to prevent overflow if truncation limit is changed. */
            char preview[256];
            utf8_truncate(preview, out.data, 200);
            strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
            cJSON_AddStringToObject(meta, "preview", preview);
        }
    } else if (out.len >= 500) {
        /* Large output: first ~200 bytes with ... suffix (UTF-8 safe) */
        char preview[256];
        utf8_truncate(preview, out.data, 200);
        strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
        cJSON_AddStringToObject(meta, "preview", preview);
    }

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "shell_exec", params, alias,
                   out.len, count_lines(out.data), exit_code == 0 ? NULL : "non-zero exit", NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    return make_result(exit_code == 0, meta, ref_copy);
}

/* ── file_read ───────────────────────────────────────── */

static tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return make_error("file_read requires a non-empty 'path' string. "
                          "Provide the file path to read.");

    const char *path = path_j->valuestring;

    /* Resolve step aliases (S0, S1, S2...) */
    char *resolved = tool_resolve_alias(ctx, path);  /* heap-allocated, must free */
    if (resolved) path = resolved;

    /* Resolve store/ paths relative to session directory (legacy) */
    char resolved_buf[NASH_PATH_MAX];
    if (strncmp(path, "store/", 6) == 0 && ctx->session_dir) {
        snprintf(resolved_buf, sizeof(resolved_buf), "%s/%s", ctx->session_dir, path);
        path = resolved_buf;
    }

    /* Safety: check file type and size before reading */
    struct stat st;
    if (stat(path, &st) != 0) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot stat '%.4095s': %s", path, strerror(errno));
        return make_error(msg);
    }
    if (!S_ISREG(st.st_mode)) {
        return make_error("not a regular file (refusing to read pipes, devices, etc.)");
    }
    int max_file = ctx->cfg ? ctx->cfg->file_max_size : 52428800;  /* 50MB default */
    if (st.st_size > max_file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "file too large (%ld bytes, limit %d)", (long)st.st_size, max_file);
        return make_error(msg);
    }

    size_t len = 0;
    char *content = slurp_file(path, &len);
    if (!content) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot read '%.4095s': %s", path, strerror(errno));
        return make_error(msg);
    }

    int total_lines = count_lines(content);

    /* Line range support: start_line and end_line parameters.
     * - start_line: 1-based (default: 1). Negative = from end (tail).
     * - end_line: 1-based inclusive (default: EOF).
     * - No params = full file (backward compatible).
     * Eliminates the need for shell_exec sed/head/tail hacks. */
    cJSON *sl_j = cJSON_GetObjectItem(params, "start_line");
    cJSON *el_j = cJSON_GetObjectItem(params, "end_line");
    int start_line = sl_j ? (int)cJSON_GetNumberValue(sl_j) : 0;
    int end_line = el_j ? (int)cJSON_GetNumberValue(el_j) : 0;

    char *display_content = NULL;  /* content to show (with line numbers if range) */
    int display_lines = total_lines;
    size_t display_len = len;
    int range_start = 1, range_end = total_lines;

    if (start_line != 0 || end_line != 0) {
        /* Resolve line range */
        if (start_line < 0) {
            /* Negative = from end: start_line=-20 means last 20 lines */
            range_start = total_lines + start_line + 1;
            if (range_start < 1) range_start = 1;
            range_end = total_lines;
        } else {
            range_start = start_line > 0 ? start_line : 1;
            range_end = end_line > 0 ? end_line : total_lines;
        }
        if (range_start > total_lines) range_start = total_lines;
        if (range_end > total_lines) range_end = total_lines;
        if (range_end < range_start) range_end = range_start;

        /* Extract lines and prepend line numbers */
        str_t out = str_new(4096);
        const char *p = content;
        int line_num = 1;
        while (*p) {
            const char *eol = strchr(p, '\n');
            int line_len = eol ? (int)(eol - p) : (int)strlen(p);

            if (line_num >= range_start && line_num <= range_end) {
                /* Prepend line number for precise file_edit targeting */
                str_appendf(&out, "%4d: ", line_num);
                str_append(&out, p, line_len);
                str_append_cstr(&out, "\n");
            }

            if (!eol) break;
            p = eol + 1;
            line_num++;
            if (line_num > range_end) break;  /* early exit */
        }

        display_content = str_steal(&out);
        display_len = strlen(display_content);
        display_lines = range_end - range_start + 1;
    }

    const char *store_content = display_content ? display_content : content;
    char *hash = store_save(ctx->store, store_content);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddNumberToObject(meta, "total_lines", total_lines);
    if (display_content) {
        /* Range mode: show which lines */
        char showing[64];
        snprintf(showing, sizeof(showing), "%d-%d of %d",
                 range_start, range_end, total_lines);
        cJSON_AddStringToObject(meta, "showing", showing);
        cJSON_AddNumberToObject(meta, "lines", display_lines);
    } else {
        cJSON_AddNumberToObject(meta, "lines", total_lines);
    }
    cJSON_AddNumberToObject(meta, "chars", (double)display_len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* file_read MUST return content — that's its purpose */
    if (display_len <= 50000) {
        cJSON_AddStringToObject(meta, "content", store_content);
    } else {
        char *trunc = malloc(50001);
        if (trunc) {
            utf8_truncate(trunc, store_content, 50000);
            cJSON_AddStringToObject(meta, "content", trunc);
            cJSON_AddBoolToObject(meta, "truncated", 1);
            free(trunc);
        }
    }

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_read", params, alias,
                   display_len, display_lines, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(content);
    free(display_content);  /* NULL-safe: free line-range extracted content */
    free(hash);
    free(resolved);  /* BUG 2 fix: free heap-allocated alias resolution */
    return make_result(1, meta, ref_copy);
}

/* ── file_write ──────────────────────────────────────── */

static tool_result_t tool_file_write(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return make_error("file_write requires a non-empty 'path' string.");
    if (!content_j || !content_j->valuestring)
        return make_error("file_write requires a 'content' parameter.");

    const char *path = path_j->valuestring;
    const char *content = content_j->valuestring;

    /* FIX 5b: Create parent directories if they don't exist.
     * Previously file_write to a non-existent directory path failed
     * with a cryptic errno message. */
    {
        char parent[NASH_PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", path);
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) {
            *last_slash = '\0';
            struct stat st;
            if (stat(parent, &st) != 0) {
                /* Recursive mkdir: walk forward creating each component */
                for (char *p = parent + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(parent, 0755);
                        *p = '/';
                    }
                }
                mkdir(parent, 0755);
            }
        }
    }

    size_t len = strlen(content);
    if (write_file(path, content, len) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    /* Store written content for full audit trail */
    char *hash = store_save(ctx->store, content);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddNumberToObject(meta, "bytes", (double)len);
    cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_write", params, alias,
                   len, count_lines(content), NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── file_edit ───────────────────────────────────────── */

static tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j     = cJSON_GetObjectItem(params, "path");
    cJSON *old_text_j = cJSON_GetObjectItem(params, "old_text");
    cJSON *new_text_j = cJSON_GetObjectItem(params, "new_text");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return make_error("file_edit requires a non-empty 'path' string.");
    if (!old_text_j || !old_text_j->valuestring || !old_text_j->valuestring[0])
        return make_error("file_edit requires a non-empty 'old_text' string. "
                          "Copy the exact text to replace from the file. "
                          "Use file_read first to see the current content.");
    if (!new_text_j || !new_text_j->valuestring)
        return make_error("file_edit requires a 'new_text' parameter.");

    const char *path = path_j->valuestring;
    const char *old_text = old_text_j->valuestring;
    const char *new_text = new_text_j->valuestring;

    size_t flen = 0;
    char *content = slurp_file(path, &flen);
    if (!content) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    /* Store pre-edit content */
    char *pre_hash = store_save(ctx->store, content);
    char *pre_alias = tool_register_alias(ctx, pre_hash ? pre_hash : "");

    size_t old_len = strlen(old_text);
    char *pos = strstr(content, old_text);
    if (!pos) {
        free(content);
        free(pre_hash);
        free(pre_alias);
        return make_error("old_text not found in file");
    }

    /* FIX #1: Detect multiple matches — if old_text appears more than once,
     * the caller must provide more context to disambiguate. Silently replacing
     * only the first match causes data corruption when the intent was to edit
     * a specific occurrence. */
    {
        char *second = strstr(pos + old_len, old_text);
        if (second) {
            /* Count total matches for a helpful error message */
            int match_count = 2;
            char *scan = second;
            while ((scan = strstr(scan + old_len, old_text)) != NULL)
                match_count++;
            free(content);
            free(pre_hash);
            free(pre_alias);
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg),
                     "old_text matches %d locations in %s — provide more "
                     "surrounding context to disambiguate", match_count, path);
            return make_error(errmsg);
        }
    }

    /* Build new content */
    size_t new_len = strlen(new_text);
    size_t result_len = flen - old_len + new_len;
    char *result = malloc(result_len + 1);
    if (!result) { free(content); free(pre_hash); free(pre_alias); return make_error("malloc failed"); }

    size_t prefix_len = (size_t)(pos - content);
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, new_text, new_len);
    memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
    result[result_len] = '\0';

    if (write_file(path, result, result_len) < 0) {
        free(content); free(result); free(pre_hash); free(pre_alias);
        return make_error("cannot write file");
    }

    /* Store post-edit content */
    char *post_hash = store_save(ctx->store, result);
    char *post_alias = tool_register_alias(ctx, post_hash ? post_hash : "");

    /* Generate diff output with line numbers (matching nashell format).
     * Format: "  Added N lines, removed M lines\n"
     *         "  {lnum:5d} +added_line\n"
     *         "  {lnum:5d} -removed_line\n"
     *         "  {lnum:5d}  context_line\n"
     * The TUI's md_render detects this format in ```diff blocks. */
    {
        /* Count lines before the edit for context — walk backward from pos */
        /* First, find the start of the line containing pos */
        char *line_start = pos;
        {
            char *scan = pos - 1;
            while (scan >= content && *scan != '\n') scan--;
            line_start = scan + 1;
        }

        /* Compute 1-based line number of line_start */
        int start_lnum = 1;
        for (const char *p = content; p < line_start; p++)
            if (*p == '\n') start_lnum++;

        /* Walk backward from the byte before line_start, skipping the
         * newline that terminates the line before the edit line. Then
         * count ctx_before more newlines to find our context boundary. */
        int ctx_before = 3;
        char *ctx_start = line_start;
        char *scan = line_start - 1;

        /* Skip the newline immediately before line_start (it belongs to
         * the previous line, not to our context count) */
        if (scan >= content && *scan == '\n') scan--;

        /* Now count ctx_before newlines walking backward */
        int found = 0;
        while (scan >= content && found < ctx_before) {
            if (*scan == '\n') {
                found++;
                if (found == ctx_before) {
                    ctx_start = scan + 1;
                    break;
                }
            }
            scan--;
        }
        if (scan < content && found < ctx_before) {
            ctx_start = content;
        }

        /* Compute 1-based line number of ctx_start */
        int ctx_start_lnum = 1;
        for (const char *p = content; p < ctx_start; p++)
            if (*p == '\n') ctx_start_lnum++;

        /* Find context after the edit */
        char *after_edit = result + (size_t)(pos - content) + new_len;
        size_t after_len = (result + result_len) - after_edit;
        char *ctx_end = after_edit;
        for (int i = 0; i < ctx_before; i++) {
            char *nl = memchr(ctx_end, '\n', after_len > (size_t)(ctx_end - after_edit) ? after_len - (size_t)(ctx_end - after_edit) : 0);
            if (!nl) break;
            ctx_end = nl + 1;
        }
        if (ctx_end > result + result_len) ctx_end = result + result_len;

        /* Build diff output — summary is prepended after computing actual diff. */
        size_t diff_cap = 4096;
        char *diff = malloc(diff_cap);
        int diff_len = 0;
        int actual_added = 0, actual_removed = 0;  /* filled by diff algorithm */

        /* Track line numbers: old_lnum for removed, new_lnum for added */
        int old_lnum = ctx_start_lnum;
        int new_lnum = ctx_start_lnum;

        /* Context before — stop at line_start (start of edit line), not pos */
        char *cl = ctx_start;
        while (cl < line_start) {
            char *nl = strchr(cl, '\n');
            int llen = nl ? (int)(nl - cl) : (int)(line_start - cl);
            if ((size_t)(diff_len + llen + 16) >= diff_cap) {
                diff_cap *= 2;
                diff = realloc(diff, diff_cap);
            }
            diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                 "  %5d  %.*s\n", new_lnum, llen, cl);
            old_lnum++;
            new_lnum++;
            cl = nl ? nl + 1 : cl + llen;
        }

        /* Line-level diff: find common prefix/suffix between old and new,
         * emit shared lines as context, only truly changed lines as +/-.
         * This avoids showing identical boundary lines as removed+added. */
        {
            /* Split old_text and new_text into line arrays */
            typedef struct { const char *s; int len; } dline_t;
            int old_cap = 64, new_cap = 64;
            int old_cnt = 0, new_cnt = 0;
            dline_t *old_lines = malloc((size_t)old_cap * sizeof(dline_t));
            dline_t *new_lines = malloc((size_t)new_cap * sizeof(dline_t));

            /* Parse old_text into lines */
            const char *p = pos;
            while (p < pos + old_len) {
                const char *nl = memchr(p, '\n', old_len - (size_t)(p - pos));
                int llen = nl ? (int)(nl - p) : (int)(pos + old_len - p);
                if (old_cnt >= old_cap) {
                    old_cap *= 2;
                    old_lines = realloc(old_lines, (size_t)old_cap * sizeof(dline_t));
                }
                old_lines[old_cnt++] = (dline_t){ p, llen };
                p = nl ? nl + 1 : p + llen;
            }

            /* Parse new_text into lines */
            p = new_text;
            while (p < new_text + new_len) {
                const char *nl = memchr(p, '\n', new_len - (size_t)(p - new_text));
                int llen = nl ? (int)(nl - p) : (int)(new_text + new_len - p);
                if (new_cnt >= new_cap) {
                    new_cap *= 2;
                    new_lines = realloc(new_lines, (size_t)new_cap * sizeof(dline_t));
                }
                new_lines[new_cnt++] = (dline_t){ p, llen };
                p = nl ? nl + 1 : p + llen;
            }

            /* Find common prefix lines */
            int prefix = 0;
            while (prefix < old_cnt && prefix < new_cnt &&
                   old_lines[prefix].len == new_lines[prefix].len &&
                   memcmp(old_lines[prefix].s, new_lines[prefix].s,
                          (size_t)old_lines[prefix].len) == 0)
                prefix++;

            /* Find common suffix lines (don't overlap with prefix) */
            int suffix = 0;
            while (suffix < (old_cnt - prefix) && suffix < (new_cnt - prefix) &&
                   old_lines[old_cnt - 1 - suffix].len == new_lines[new_cnt - 1 - suffix].len &&
                   memcmp(old_lines[old_cnt - 1 - suffix].s,
                          new_lines[new_cnt - 1 - suffix].s,
                          (size_t)old_lines[old_cnt - 1 - suffix].len) == 0)
                suffix++;

            /* Emit common prefix as context */
            for (int i = 0; i < prefix; i++) {
                dline_t *dl = &new_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d  %.*s\n", new_lnum, dl->len, dl->s);
                old_lnum++;
                new_lnum++;
            }

            /* Emit removed lines (middle section of old) */
            for (int i = prefix; i < old_cnt - suffix; i++) {
                dline_t *dl = &old_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d -%.*s\n", old_lnum, dl->len, dl->s);
                old_lnum++;
                actual_removed++;
            }

            /* Emit added lines (middle section of new) */
            for (int i = prefix; i < new_cnt - suffix; i++) {
                dline_t *dl = &new_lines[i];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d +%.*s\n", new_lnum, dl->len, dl->s);
                new_lnum++;
                actual_added++;
            }

            /* Emit common suffix as context */
            for (int i = old_cnt - suffix; i < old_cnt; i++) {
                dline_t *dl = &new_lines[new_cnt - suffix + (i - (old_cnt - suffix))];
                if ((size_t)(diff_len + dl->len + 16) >= diff_cap) {
                    diff_cap *= 2;
                    diff = realloc(diff, diff_cap);
                }
                diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                     "  %5d  %.*s\n", new_lnum, dl->len, dl->s);
                old_lnum++;
                new_lnum++;
            }

            free(old_lines);
            free(new_lines);
        }

        /* Context after — use new_lnum (post-edit line numbers) */
        cl = after_edit;
        while (cl < ctx_end) {
            char *nl = strchr(cl, '\n');
            int llen = nl ? (int)(nl - cl) : (int)(ctx_end - cl);
            if ((size_t)(diff_len + llen + 16) >= diff_cap) {
                diff_cap *= 2;
                diff = realloc(diff, diff_cap);
            }
            diff_len += snprintf(diff + diff_len, diff_cap - (size_t)diff_len,
                                 "  %5d  %.*s\n", new_lnum, llen, cl);
            new_lnum++;
            cl = nl ? nl + 1 : cl + llen;
        }

        /* Prepend summary header with actual counts */
        {
            char hdr[64];
            int hlen = snprintf(hdr, sizeof(hdr),
                                "  Added %d lines, removed %d lines\n",
                                actual_added, actual_removed);
            /* Grow buffer if needed, then shift body right and insert header */
            if ((size_t)(diff_len + hlen + 1) >= diff_cap) {
                diff_cap = (size_t)(diff_len + hlen + 64);
                diff = realloc(diff, diff_cap);
            }
            memmove(diff + hlen, diff, (size_t)diff_len + 1);  /* +1 for NUL */
            memcpy(diff, hdr, (size_t)hlen);
            diff_len += hlen;
        }

        /* Store diff as the display ref */
        char *diff_hash = store_save(ctx->store, diff);
        char *diff_alias = tool_register_alias(ctx, diff_hash ? diff_hash : "");
        free(diff);
        free(diff_hash);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "path", path);
        cJSON_AddStringToObject(meta, "pre_ref", pre_alias);
        cJSON_AddStringToObject(meta, "post_ref", post_alias);

        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_edit", params, diff_alias,
                       diff_len, 0, NULL, NULL);

        free(content);
        free(result);
        free(pre_alias);
        free(pre_hash);
        free(post_hash);
        free(post_alias);
        return make_result(1, meta, diff_alias);
    }
}

/* ── grep_search ─────────────────────────────────────── */

static tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    cJSON *path_j    = cJSON_GetObjectItem(params, "path");
    if (!pattern_j || !pattern_j->valuestring || !pattern_j->valuestring[0])
        return make_error("grep_search requires a non-empty 'pattern' string. "
                          "Provide a regex pattern to search for.");

    const char *pattern = pattern_j->valuestring;
    const char *path = path_j && path_j->valuestring && path_j->valuestring[0]
                       ? path_j->valuestring : ".";

    /* Resolve step aliases (returns heap-allocated string, caller must free) */
    char *resolved = tool_resolve_alias(ctx, path);
    if (resolved) path = resolved;

    /* Resolve store/ paths (legacy) */
    char resolved_path[NASH_PATH_MAX];
    if (strncmp(path, "store/", 6) == 0 && ctx->session_dir) {
        snprintf(resolved_path, sizeof(resolved_path), "%s/%s", ctx->session_dir, path);
        path = resolved_path;
    }

    /* use fork/execvp to avoid shell injection */
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe failed");

    int max_matches = ctx->cfg ? ctx->cfg->grep_max_matches : 50;
    if (max_matches <= 0) max_matches = 50;
    char max_matches_str[16];
    snprintf(max_matches_str, sizeof(max_matches_str), "%d", max_matches);

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return make_error("fork failed"); }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("grep", "grep", "-rn",
               "--binary-files=without-match",
               "--exclude-dir=.git", "--exclude-dir=node_modules",
               "--exclude-dir=__pycache__", "--exclude-dir=.tox",
               "--exclude-dir=vendor", "--exclude-dir=target",
               "--exclude-dir=build", "--exclude-dir=dist",
               "--exclude=*.o", "--exclude=*.a", "--exclude=*.so",
               "--exclude=*.dylib", "--exclude=*.pyc",
               "-m", max_matches_str,
               pattern, path, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[NASH_PATH_MAX];
    ssize_t n;

    /* Read with timeout + output cap.
     * FIX B4: grep_max_matches controls line count (grep -m), while this
     * byte cap prevents unbounded output from long-line matches.
     * Uses shell_max_output as default; a dedicated grep_max_output config
     * could be added if finer control is needed. */
    int grep_timeout = ctx->cfg ? ctx->cfg->grep_timeout : 60;
    int grep_max = ctx->cfg ? ctx->cfg->shell_max_output : 512000;
    time_t start = time(NULL);
    int timed_out = 0;

    /* Set pipe to non-blocking for timeout support */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    while (1) {
        n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            str_append(&out, buf, (size_t)n);
            if ((int)out.len >= grep_max) {
                str_append_cstr(&out, "\n... [output truncated at limit]\n");
                break;
            }
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            /* EAGAIN — no data yet */
            if (time(NULL) - start >= grep_timeout) {
                timed_out = 1;
                break;
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms poll */
        }
    }
    close(pipefd[0]);

    if (timed_out) kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    int matches = count_lines(out.data);
    char *hash = store_save(ctx->store, out.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    cJSON_AddStringToObject(meta, "path", path_j && path_j->valuestring ? path_j->valuestring : ".");
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "grep_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    free(resolved);  /* BUG 2 fix: free heap-allocated resolved path */
    return make_result(1, meta, ref_copy);
}

/* ── glob_search ──────────────────────────────────────── */

/* Parse glob pattern: extracts directory path and glob name.
 * Patterns supported: bare globs, dir/glob, doublestar globs, exact paths
 * Outputs: root (search directory), name (glob pattern), recursive/exact flags
 */

static void parse_glob_pattern(const char *pattern,
                               char *root, size_t root_sz,
                               char *name, size_t name_sz,
                               int *recursive, int *exact,
                               char *exact_path, size_t exact_path_sz) {
    (void)root_sz;  /* used for bounds in strncpy calls */
    const char *p = pattern;
    int has_doublestar = 0;
    *recursive = 0;
    *exact = 0;
    root[0] = '.';
    root[1] = '\0';
    name[0] = '\0';
    exact_path[0] = '\0';

    /* Strip leading ./ */
    if (p[0] == '.' && p[1] == '/')
        p += 2;

    /* Handle doublestar/ prefix (recursive from root) */
    if (p[0] == '*' && p[1] == '*' && p[2] == '/') {
        has_doublestar = 1;
        *recursive = 1;
        p += 3;
    }

    /* Find the last '/' to separate directory from basename */
    const char *last_slash = strrchr(p, '/');

    if (last_slash != NULL) {
        /* Check if the part after last slash contains glob chars */
        const char *after_slash = last_slash + 1;
        int glob_after = (strchr(after_slash, '*') != NULL
                        || strchr(after_slash, '?') != NULL
                        || strchr(after_slash, '[') != NULL);

        if (glob_after) {
            /* Directory prefix with glob basename */
            size_t dir_len = (size_t)(last_slash - p);

            /* Check if directory part ends with doublestar/ (recursive within dir) */
            if (dir_len >= 3 && last_slash[-1] == '*'
                && last_slash[-2] == '*' && last_slash[-3] == '/') {
                dir_len -= 3;  /* strip the doublestar/ from directory part */
                *recursive = 1;
            }

            if (dir_len > 0) {
                strncpy(root, p, dir_len);
                root[dir_len] = '\0';
            }
            strncpy(name, after_slash, name_sz - 1);
            name[name_sz - 1] = '\0';
        } else {
            /* Exact file path, e.g. src/main.c */
            *exact = 1;
            strncpy(exact_path, p, exact_path_sz - 1);
            exact_path[exact_path_sz - 1] = '\0';
        }
    } else {
        /* No slash — bare glob pattern, search recursively from root */
        *recursive = 1;
        strncpy(name, p, name_sz - 1);
        name[name_sz - 1] = '\0';
    }
    (void)has_doublestar;
}

static tool_result_t tool_glob_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *pattern_j = cJSON_GetObjectItem(params, "pattern");
    if (!pattern_j || !pattern_j->valuestring || !pattern_j->valuestring[0])
        return make_error("glob_search requires a non-empty 'pattern' string. "
                          "Use wildcards like **/*.c or src/**/*.h");

    const char *pattern = pattern_j->valuestring;
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    const char *search_path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Parse the glob pattern into components */
    char root[1024], name[1024], exact_path[1024];
    int recursive, exact;
    parse_glob_pattern(pattern, root, sizeof(root), name, sizeof(name),
                       &recursive, &exact, exact_path, sizeof(exact_path));

    /* Build find arguments — use fork/execvp to avoid shell injection.
     * (grep_search already uses fork/execlp for the same reason.) */
    char full_path[NASH_PATH_MAX];

    /* For exact file path, just check existence directly */
    if (exact) {
        if (exact_path[0] == '/')
            snprintf(full_path, sizeof(full_path), "%s", exact_path);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", search_path, exact_path);
        str_t out = str_new(256);
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            str_append_cstr(&out, full_path);
            str_append_cstr(&out, "\n");
        }

        int matches = count_lines(out.data);
        char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
        char *alias = tool_register_alias(ctx, hash ? hash : "");

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "pattern", pattern);
        if (strcmp(search_path, ".") != 0)
            cJSON_AddStringToObject(meta, "path", search_path);
        cJSON_AddNumberToObject(meta, "matches", matches);
        cJSON_AddStringToObject(meta, "ref", alias);

        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "glob_search", params, alias,
                       out.len, matches, NULL, NULL);

        char *ref_copy = strdup(alias);
        free(alias);
        str_free(&out);
        free(hash);
        return make_result(1, meta, ref_copy);
    }

    /* Build find_path: the directory to search in.
     * If root is an absolute path (starts with /), use it directly
     * instead of prepending search_path — avoids creating invalid
     * paths like "./home/user/dir". */
    if (strcmp(root, ".") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", search_path);
    } else if (root[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", root);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", search_path, root);
    }

    /* Use fork/execvp for find — avoids shell injection via pattern/path */
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return make_error("fork failed"); }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Redirect stderr to /dev/null (like 2>/dev/null in original) */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        if (recursive) {
            execlp("find", "find", full_path,
                   "-type", "f", "-name", name,
                   "!", "-path", "*/.git/*",
                   "!", "-path", "*/node_modules/*",
                   "!", "-path", "*/__pycache__/*",
                   "!", "-name", "*.o",
                   (char *)NULL);
        } else {
            execlp("find", "find", full_path,
                   "-maxdepth", "1",
                   "-type", "f", "-name", name,
                   "!", "-path", "*/.git/*",
                   "!", "-path", "*/node_modules/*",
                   "!", "-path", "*/__pycache__/*",
                   "!", "-name", "*.o",
                   (char *)NULL);
        }
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[NASH_PATH_MAX];
    ssize_t n;
    int line_count = 0;

    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        str_append(&out, buf, (size_t)n);
        /* Count lines for limit (head -200 equivalent) */
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\n') line_count++;
        if (line_count >= 200) break;
    }
    close(pipefd[0]);

    /* Kill find if we hit the line limit */
    if (line_count >= 200) kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    /* Truncate to 200 lines if we overshot */
    if (line_count > 200 && out.data) {
        int seen = 0;
        for (size_t i = 0; i < out.len; i++) {
            if (out.data[i] == '\n') {
                seen++;
                if (seen >= 200) {
                    out.len = i + 1;
                    out.data[out.len] = '\0';  /* str_t guarantees space for NUL at data[len] */
                    break;
                }
            }
        }
    }

    int matches = count_lines(out.data);
    char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    if (strcmp(search_path, ".") != 0)
        cJSON_AddStringToObject(meta, "path", search_path);
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "glob_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── scratchpad section operations (GDN-2 inspired) ─── */

/* ── notes (section-based scratchpad) ────────────────── */

static void scratchpad_persist(tool_ctx_t *ctx) {
    scratchpad_save(&ctx->scratch, ctx->session_dir);
}

static tool_result_t tool_notes(tool_ctx_t *ctx, cJSON *params) {
    cJSON *content_j = cJSON_GetObjectItem(params, "content");

    /* ── Legacy mode: notes(content="...") ── */
    /* If only 'content' is provided (no 'op'), treat as legacy full-replacement.
     * This maintains backward compatibility with existing prompts/models. */
    cJSON *op_j = cJSON_GetObjectItem(params, "op");
    if (!op_j && content_j && content_j->valuestring) {
        /* Parse content for section headers (## name) and split into sections */
        const char *text = content_j->valuestring;
        const char *p = text;
        int found_sections = 0;

        /* Quick scan: does the content contain "## " section headers?
         * FIX B7: Only parse sections if the text STARTS with "## " — this
         * means it was generated by scratchpad_serialize(). Content that
         * merely contains "\n## " in the body (e.g., markdown with nested
         * headers) should not be split into sections. */
        if (strncmp(text, "## ", 3) == 0) {
            /* Parse structured content into sections */
            scratchpad_free(&ctx->scratch);

            while (p && *p) {
                if (strncmp(p, "## ", 3) == 0 || (p > text && strncmp(p, "\n## ", 4) == 0)) {
                    if (*p == '\n') p++;
                    const char *hdr = p + 3;
                    const char *hdr_end = strchr(hdr, '\n');
                    if (!hdr_end) hdr_end = hdr + strlen(hdr);

                    char sec_name[256];
                    size_t nlen = (size_t)(hdr_end - hdr);
                    if (nlen >= sizeof(sec_name)) nlen = sizeof(sec_name) - 1;
                    memcpy(sec_name, hdr, nlen);
                    sec_name[nlen] = '\0';

                    const char *body = (*hdr_end) ? hdr_end + 1 : hdr_end;
                    /* Find end of section (next ## or end) */
                    const char *body_end = strstr(body, "\n## ");
                    if (!body_end) body_end = body + strlen(body);

                    /* Trim trailing whitespace */
                    while (body_end > body && (*(body_end-1) == '\n' || *(body_end-1) == ' '))
                        body_end--;

                    size_t blen = (size_t)(body_end - body);
                    char *sec_content = malloc(blen + 1);
                    if (sec_content) {
                        memcpy(sec_content, body, blen);
                        sec_content[blen] = '\0';
                        scratchpad_write(&ctx->scratch, sec_name, sec_content, 5);
                        free(sec_content);
                        found_sections++;
                    }

                    p = (body_end < body + strlen(body)) ? strstr(body, "\n## ") : NULL;
                    if (!p) break;
                } else {
                    p = strstr(p, "\n## ");
                }
            }
        }

        if (!found_sections) {
            /* No section headers — store as single "default" section */
            scratchpad_free(&ctx->scratch);
            scratchpad_write(&ctx->scratch, "default", text, 5);
        }

        scratchpad_persist(ctx);

        char *full = scratchpad_serialize(&ctx->scratch);
        char *hash = store_save(ctx->store, full ? full : "");
        char *alias = tool_register_alias(ctx, hash ? hash : "");

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);
        cJSON_AddStringToObject(meta, "ref", alias);

        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, alias,
                       full ? strlen(full) : 0, 0, NULL, NULL);

        char *ref_copy = strdup(alias);
        free(alias);
        free(hash);
        free(full);
        return make_result(1, meta, ref_copy);
    }

    /* ── Section-based mode: notes(op, section, content, priority) ── */
    if (!op_j || !op_j->valuestring)
        return make_error("missing 'op' parameter (write|append|read|clear|list) or 'content' for legacy mode");

    const char *op = op_j->valuestring;
    cJSON *section_j = cJSON_GetObjectItem(params, "section");
    const char *section = section_j && section_j->valuestring ? section_j->valuestring : NULL;
    const char *content = content_j && content_j->valuestring ? content_j->valuestring : NULL;
    cJSON *priority_j = cJSON_GetObjectItem(params, "priority");
    int priority = priority_j ? (int)cJSON_GetNumberValue(priority_j) : 5;

    if (strcmp(op, "write") == 0) {
        if (!section) return make_error("'write' requires 'section' parameter");
        if (!content) return make_error("'write' requires 'content' parameter");

        int rc = scratchpad_write(&ctx->scratch, section, content, priority);
        if (rc != 0) return make_error("scratchpad full (max 32 sections)");

        scratchpad_persist(ctx);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "write");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);

        /* Store content for full audit trail */
        char *w_hash = store_save(ctx->store, content);
        char *w_alias = tool_register_alias(ctx, w_hash ? w_hash : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, w_alias,
                       strlen(content), 0, NULL, NULL);
        free(w_alias); free(w_hash);
        return make_result(1, meta, NULL);

    } else if (strcmp(op, "append") == 0) {
        if (!section) return make_error("'append' requires 'section' parameter");
        if (!content) return make_error("'append' requires 'content' parameter");

        int rc = scratchpad_append(&ctx->scratch, section, content, priority);
        if (rc != 0) return make_error("scratchpad full");

        scratchpad_persist(ctx);

        int idx = scratchpad_find(&ctx->scratch, section);
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "append");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "total_chars",
                                idx >= 0 ? (double)strlen(ctx->scratch.sections[idx].content) : 0);

        /* Store content for full audit trail */
        char *a_hash = store_save(ctx->store, content);
        char *a_alias = tool_register_alias(ctx, a_hash ? a_hash : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, a_alias,
                       strlen(content), 0, NULL, NULL);
        free(a_alias); free(a_hash);
        return make_result(1, meta, NULL);

    } else if (strcmp(op, "read") == 0) {
        if (!section) return make_error("'read' requires 'section' parameter");

        int idx = scratchpad_find(&ctx->scratch, section);
        if (idx < 0) return make_error("section not found");

        const char *sec_content = ctx->scratch.sections[idx].content;
        char *hash = store_save(ctx->store, sec_content);
        char *alias = tool_register_alias(ctx, hash ? hash : "");

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "chars", (double)strlen(sec_content));
        cJSON_AddNumberToObject(meta, "priority", ctx->scratch.sections[idx].priority);
        cJSON_AddStringToObject(meta, "ref", alias);

        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, alias,
                       strlen(sec_content), 0, NULL, NULL);

        char *ref_copy = strdup(alias);
        free(alias);
        free(hash);
        return make_result(1, meta, ref_copy);

    } else if (strcmp(op, "clear") == 0) {
        if (!section) return make_error("'clear' requires 'section' parameter");

        int rc = scratchpad_clear(&ctx->scratch, section);
        if (rc != 0) return make_error("section not found");

        scratchpad_persist(ctx);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "clear");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);

        /* Store clear/delete status for audit trail */
        {
            char *c_str = cJSON_Print(meta);
            char *c_hash = store_save(ctx->store, c_str ? c_str : "{}");
            char *c_alias = tool_register_alias(ctx, c_hash ? c_hash : "");
            inject_thought(ctx, params);
            journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, c_alias,
                           0, 0, NULL, NULL);
            free(c_alias); free(c_hash); free(c_str);
        }
        return make_result(1, meta, NULL);

    } else if (strcmp(op, "list") == 0) {
        str_t out = str_new(512);
        for (int i = 0; i < ctx->scratch.count; i++) {
            str_appendf(&out, "  %s (priority:%d, %zu chars)\n",
                        ctx->scratch.sections[i].name,
                        ctx->scratch.sections[i].priority,
                        strlen(ctx->scratch.sections[i].content));
        }

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "op", "list");
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);
        if (out.len > 0)
            cJSON_AddStringToObject(meta, "listing", out.data);

        /* Store listing for audit trail */
        {
            char *l_hash = store_save(ctx->store, out.len > 0 ? out.data : "{}");
            char *l_alias = tool_register_alias(ctx, l_hash ? l_hash : "");
            inject_thought(ctx, params);
            journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, l_alias,
                           out.len, ctx->scratch.count, NULL, NULL);
            free(l_alias); free(l_hash);
        }

        str_free(&out);
        return make_result(1, meta, NULL);

    } else {
        return make_error("unknown op (use: write, append, read, clear, list)");
    }
}

/* ── done ────────────────────────────────────────────── */

static tool_result_t tool_done(tool_ctx_t *ctx, cJSON *params) {
    cJSON *result_j = cJSON_GetObjectItem(params, "result");
    const char *result = result_j && result_j->valuestring
                         ? result_j->valuestring : "(no result)";

    /* Store result for full audit */
    char *hash = store_save(ctx->store, result);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "result", result);
    cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "done", params, alias,
                   strlen(result), 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}


/* ── user_ask stub ────────────────────────────────────── */
/* user_ask is handled by react.c before reaching tool_execute().
 * This stub exists only so the dispatch table has an entry.
 * If reached, it means react.c's special-casing was bypassed. */
static tool_result_t tool_user_ask_stub(tool_ctx_t *ctx, cJSON *params) {
    (void)ctx; (void)params;
    return make_error("user_ask must be handled by react loop, not tool dispatch");
}

/* ── plan ──────────────────────────────────────────────── */

static tool_result_t tool_plan(tool_ctx_t *ctx, cJSON *params) {
    const char *result = NULL;
    cJSON *r = cJSON_GetObjectItem(params, "result");
    if (r && r->valuestring) result = r->valuestring;
    if (!result || !result[0])
        return make_error("missing 'result' parameter with the plan text");

    /* Write plan to scratchpad as a high-priority section.
     * The plan survives context eviction and is visible to the model
     * throughout the react loop via the scratchpad injection. */
    scratchpad_write(&ctx->scratch, "plan", result, 1);  /* priority 1 = high */
    scratchpad_persist(ctx);

    /* Store in content-addressed store for audit trail */
    char *hash = store_save(ctx->store, result);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    /* Count plan steps (lines starting with a digit) */
    int steps = 0;
    const char *p = result;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p >= '1' && *p <= '9') steps++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "plan saved to scratchpad");
    cJSON_AddNumberToObject(meta, "steps", steps);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "plan",
                   params, alias, strlen(result), steps, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── dispatcher ──────────────────────────────────────── */

/* ── memory_store ──────────────────────────────────────── */

/* ── LLM-based memory consolidation (GDN-2 P2) ──────── */
/* After storing a memory, check for semantically similar existing memories.
 * If found, concatenate both and call the LLM to produce a consolidated
 * version. This implements the "subtract-before-write" principle from GDN-2:
 * related content is merged rather than duplicated. */


/* Carry forward validation scores from a deleted/old entry to the surviving
 * entry.  When consolidation merges or replaces entries, the old entry's
 * recall evidence (hits/misses) must be summed into the survivor to prevent
 * a well-tested memory from losing its credibility after consolidation.
 * Without this, a merged entry starts at vscore=0.50 (uninformed prior)
 * and becomes vulnerable to immediate pruning. */
static void consolidation_carry_scores(memory_t *m,
                                       const char *survivor_key,
                                       int old_hits, int old_misses) {
    if (!m || !survivor_key || (old_hits == 0 && old_misses == 0)) return;

    /* Build JSON path from key using shared key_to_path() */
    char fname[512];
    key_to_path(survivor_key, ".json", fname, sizeof(fname));
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = slurp_json(path);
    if (!entry) return;

    cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
    cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
    int cur_hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
    int cur_misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;

    if (rh) cJSON_SetNumberValue(rh, cur_hits + old_hits);
    else    cJSON_AddNumberToObject(entry, "recall_hits", old_hits);
    if (rm) cJSON_SetNumberValue(rm, cur_misses + old_misses);
    else    cJSON_AddNumberToObject(entry, "recall_misses", old_misses);

    /* Write back */
    char *json = cJSON_Print(entry);
    if (json) {
        write_file(path, json, strlen(json));
        free(json);
    }
    cJSON_Delete(entry);

    /* FIX BUG3: Also update the in-memory index so recall scoring
     * sees the carried-forward evidence immediately (without restart).
     * Without this, the merged entry has stale scores (typically 0/0)
     * in the index and may be ranked lower than it should be. */
    memory_update_scores(m, survivor_key, old_hits, old_misses);
}

/* FIX D4: Returns strdup'd key to delete (caller collects for batch),
 * or NULL if no deletion needed.  Previously called memory_delete()
 * inline, causing O(N) gc_refs scan per delete.  Now the caller
 * batches all deletions into a single memory_delete_batch() call. */
static char *memory_try_consolidate(tool_ctx_t *ctx, const char *new_key,
                                     const char *new_value) {
    if (!ctx->provider || !ctx->memory) return NULL;
    if (!ctx->memory->embed || !ctx->memory->embed->available) return NULL;

    /* Load the multi-vec embedding for the new entry (just stored by
     * memory_embed_entry, which already produced chunked embeddings). */
    char new_emb_fname[512];
    key_to_path(new_key, "", new_emb_fname, sizeof(new_emb_fname));
    char new_emb_path[NASH_PATH_MAX];
    snprintf(new_emb_path, sizeof(new_emb_path), "%s/%s.emb",
             ctx->memory->dir, new_emb_fname);
    embed_multi_vec_t new_emb = embed_multi_vec_load(new_emb_path);
    if (!new_emb.data) return NULL;

    /* Scan in-memory index for high similarity (FIX P1-3).
     *
     * Previous: for_each_dir_entry() scanned .emb files on disk, creating
     * a consistency window where deleted-but-not-flushed entries could match,
     * or newly-stored entries without embeddings would be missed.
     *
     * Now: iterate over the authoritative in-memory index (m->idx), using
     * cached embeddings when available, falling back to disk load.
     *
     * Consolidation threshold: cosine similarity above which two memories
     * are considered near-duplicates and merged. 0.82 is conservative —
     * only genuinely redundant entries trigger consolidation.
     * Configurable via config.toml [limits] consolidation_threshold.
     *
     * Research basis: IR literature places "semantically equivalent"
     * text at cosine similarity 0.80-0.90 depending on embedding model.
     * See also: MemForest [arXiv:2605.23986] for temporal dedup. */
    float cons_threshold = ctx->cfg ? ctx->cfg->consolidation_threshold : 0.82f;
    char best_key[512] = {0};
    char best_path[NASH_PATH_MAX] = {0};
    float best_sim = 0.0f;

    memory_t *m = ctx->memory;
    for (int i = 0; i < m->idx.count; i++) {
        mem_index_entry_t *e = &m->idx.entries[i];

        /* Skip self — the entry we just stored */
        if (strcmp(e->key, new_key) == 0) continue;

        /* Get embedding: prefer cached, fall back to disk */
        embed_multi_vec_t *emb_ptr = NULL;
        embed_multi_vec_t loaded_emb = {0};
        if (e->has_emb && e->emb.data) {
            emb_ptr = &e->emb;
        } else if (e->path) {
            char emb_path[NASH_PATH_MAX];
            snprintf(emb_path, sizeof(emb_path), "%s", e->path);
            size_t plen = strlen(emb_path);
            if (plen >= 5 && strcmp(emb_path + plen - 5, ".json") == 0)
                strcpy(emb_path + plen - 5, ".emb");
            loaded_emb = embed_multi_vec_load(emb_path);
            if (loaded_emb.data) emb_ptr = &loaded_emb;
        }
        if (!emb_ptr) continue;

        /* Dimension check: skip stale embeddings from a different model */
        if (emb_ptr->dim != new_emb.dim) {
            if (loaded_emb.data) {
                /* Delete stale .emb file so memory_embed_all() regenerates it */
                char emb_path[NASH_PATH_MAX];
                snprintf(emb_path, sizeof(emb_path), "%s", e->path);
                size_t plen = strlen(emb_path);
                if (plen >= 5 && strcmp(emb_path + plen - 5, ".json") == 0)
                    strcpy(emb_path + plen - 5, ".emb");
                unlink(emb_path);
                embed_multi_vec_free(&loaded_emb);
            }
            continue;
        }

        /* MaxSim across all chunk pairs */
        float sim = embed_cosine_sim_multi_multi(&new_emb, emb_ptr);
        if (loaded_emb.data) embed_multi_vec_free(&loaded_emb);

        if (sim > best_sim && sim > cons_threshold) {
            best_sim = sim;
            snprintf(best_key, sizeof(best_key), "%s", e->key);
            if (e->path)
                snprintf(best_path, sizeof(best_path), "%s", e->path);
        }
    }

    embed_multi_vec_free(&new_emb);

    if (best_key[0] == '\0') return NULL;  /* no similar memory found */

    /* Load the similar memory's value */
    cJSON *old_entry = slurp_json(best_path);
    if (!old_entry) return NULL;

    cJSON *old_key_j = cJSON_GetObjectItem(old_entry, "key");
    cJSON *old_val_j = cJSON_GetObjectItem(old_entry, "value");
    if (!old_key_j || !old_val_j ||
        !old_key_j->valuestring || !old_val_j->valuestring) {
        cJSON_Delete(old_entry);
        return NULL;
    }

    const char *old_key = old_key_j->valuestring;
    const char *old_value = old_val_j->valuestring;

    /* Don't consolidate pinned memories */
    cJSON *pinned_j = cJSON_GetObjectItem(old_entry, "pinned");
    if (pinned_j && cJSON_IsTrue(pinned_j)) {
        cJSON_Delete(old_entry);
        return NULL;
    }

    /* Extract old entry's validation scores BEFORE any branch deletes it.
     * These will be carried forward to the surviving entry. */
    cJSON *old_rh = cJSON_GetObjectItem(old_entry, "recall_hits");
    cJSON *old_rm = cJSON_GetObjectItem(old_entry, "recall_misses");
    int old_hits = old_rh ? (int)cJSON_GetNumberValue(old_rh) : 0;
    int old_misses = old_rm ? (int)cJSON_GetNumberValue(old_rm) : 0;

    /* Build classify-then-act prompt.
     * Instead of blindly merging, ask the LLM to classify the relationship:
     * - SUPERSEDES: new entry corrects/updates old → delete old, keep new
     * - COMPLEMENTARY: entries cover different aspects → keep both
     * - REDUNDANT: entries say the same thing → merge into one
     * This prevents contradictory entries from being merged into incoherent
     * mush, and allows corrective insights to properly replace outdated ones.
     *
     * FIX B10: Include key lengths in allocation — the format string
     * interpolates new_key and old_key too, which could be up to 256
     * chars each. */
    size_t prompt_sz = strlen(new_value) + strlen(old_value) +
                     strlen(new_key) + strlen(old_key) + 2048;
    char *prompt = malloc(prompt_sz);
    if (!prompt) { cJSON_Delete(old_entry); return NULL; }
    snprintf(prompt, prompt_sz,
        "Two memory entries are semantically similar. Classify their relationship "
        "and act accordingly.\n\n"
        "--- NEW entry (key: %s) ---\n%s\n\n"
        "--- EXISTING entry (key: %s) ---\n%s\n\n"
        "First, classify the relationship as exactly one of:\n"
        "SUPERSEDES — the NEW entry corrects, updates, or invalidates the EXISTING entry "
        "(e.g. opposite advice, updated procedure, refined understanding)\n"
        "COMPLEMENTARY — the entries cover different aspects of the same topic "
        "(e.g. different failure modes, different contexts, different techniques)\n"
        "REDUNDANT — the entries say essentially the same thing with different wording\n\n"
        "Output format:\n"
        "Line 1: SUPERSEDES or COMPLEMENTARY or REDUNDANT\n"
        "Line 2+: If REDUNDANT, output the merged text (concise, preserve unique info). "
        "If SUPERSEDES or COMPLEMENTARY, output nothing more.",
        new_key, new_value, old_key, old_value);

    /* Call LLM for classification + optional merge */
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system",
        "You are a memory consistency assistant. Classify the relationship between "
        "two memory entries and, if redundant, merge them. Be precise: entries that "
        "give OPPOSITE advice for the same situation are SUPERSEDES, not REDUNDANT.");
    llm_chat_add(chat, "user", prompt);
    free(prompt);

    llm_stats_t stats = {0};
    char *response = NULL;

    /* Use a separate provider for consolidation to avoid mutating the
     * shared provider config (thread-safety + signal-safety). */
    provider_config_t cons_cfg = ctx->provider->cfg;
    cons_cfg.max_tokens = 2048;
    cons_cfg.temperature = 0.1f;
    cons_cfg.enable_thinking = 0;
    cons_cfg.thinking_budget = 0;
    provider_t *cons_provider = provider_create(&cons_cfg);
    if (cons_provider) {
        response = provider_complete(cons_provider, chat, &stats);
        provider_free(cons_provider);
    }
    llm_chat_free(chat);

    if (!response || strlen(response) < 5) {
        free(response);
        cJSON_Delete(old_entry);
        return NULL;
    }

    /* Parse classification from first line of response */
    if (strncmp(response, "COMPLEMENTARY", 13) == 0) {
        /* Entries cover different aspects — keep both, do nothing */
        free(response);
        cJSON_Delete(old_entry);
        return NULL;
    }

    if (strncmp(response, "SUPERSEDES", 10) == 0) {
        /* New entry corrects/updates old — delete old, keep new as-is.
         * FIX D4: Return key for batch deletion instead of inline delete. */
        char *del_key = (strcmp(old_key, new_key) != 0) ? strdup(old_key) : NULL;
        /* Carry forward old entry's validation evidence to the new entry.
         * The new insight earned the old one's credibility by replacing it. */
        consolidation_carry_scores(ctx->memory, new_key,
                                   old_hits, old_misses);
        free(response);
        cJSON_Delete(old_entry);
        return del_key;
    }

    /* Default: REDUNDANT — merge (extract text after first newline) */
    {
        char *merged = NULL;
        char *newline = strchr(response, '\n');
        if (newline) {
            /* Skip the classification line and any leading whitespace */
            newline++;
            while (*newline == '\n' || *newline == '\r' || *newline == ' ')
                newline++;
            if (strlen(newline) >= 20) {
                merged = strdup(newline);
            }
        }
        if (!merged) {
            /* Couldn't extract merged text — skip */
            free(response);
            cJSON_Delete(old_entry);
            return NULL;
        }
        free(response);

        /* Preserve journal_ref provenance from the new entry.
         * FIX B1: Without this, consolidated memories lose provenance. */
        char new_fname[512];
        key_to_path(new_key, ".json", new_fname, sizeof(new_fname));
        char new_json_path[NASH_PATH_MAX];
        snprintf(new_json_path, sizeof(new_json_path), "%s/%s",
                 ctx->memory->dir, new_fname);

        const char *new_jref = NULL;
        cJSON *new_entry_json = slurp_json(new_json_path);
        if (new_entry_json) {
            cJSON *jr = cJSON_GetObjectItem(new_entry_json, "journal_ref");
            if (jr && jr->valuestring) new_jref = jr->valuestring;
        }

        /* Store merged version under the new key */
        memory_store(ctx->memory, new_key, merged,
                     0, new_jref, NULL, 0);

        cJSON_Delete(new_entry_json);

        /* FIX D4: Return key for batch deletion instead of inline delete. */
        char *del_key = (strcmp(old_key, new_key) != 0) ? strdup(old_key) : NULL;

        /* Carry forward old entry's validation evidence to the merged result.
         * memory_store() above preserved the new entry's counters (same key
         * overwrite), but the old entry's counters were lost via delete.
         * Sum both entries' evidence into the survivor. */
        consolidation_carry_scores(ctx->memory, new_key,
                                   old_hits, old_misses);

        free(merged);
        cJSON_Delete(old_entry);
        return del_key;
    }
    /* FIX BUG#3: removed unreachable cJSON_Delete that was after the
     * unconditional return in the REDUNDANT block above. */
    return NULL;  /* unreachable — silences compiler warning */
}

static tool_result_t tool_memory_store(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    cJSON *val_j = cJSON_GetObjectItem(params, "value");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0])
        return make_error("memory_store requires a non-empty 'key' string. "
                          "Use format 'type:descriptive-name' (e.g. lesson:config-sentinel-values).");
    if (!val_j || !val_j->valuestring || !val_j->valuestring[0])
        return make_error("memory_store requires a non-empty 'value' string. "
                          "Provide the knowledge to store.");

    const char *key = key_j->valuestring;
    const char *value = val_j->valuestring;

    int pinned = 0;
    cJSON *pin_j = cJSON_GetObjectItem(params, "pinned");
    if (pin_j && cJSON_IsTrue(pin_j)) pinned = 1;

    /* Build journal provenance reference: "session_dir/journal.jsonl:R<loop>" */
    char jref[NASH_PATH_MAX];
    if (ctx->session_dir && ctx->journal) {
        snprintf(jref, sizeof(jref), "%s/journal.jsonl:R%d",
                 ctx->session_dir, ctx->react_loop);
    } else {
        jref[0] = '\0';
    }

    /* Parse refs (comma-separated string of related memory keys).
     * Inter-memory relationships for "see also" links.
     * Research: MemForest [arXiv:2605.23986], ActiveGraph [arXiv:2605.21997],
     * MemIR [arXiv:2605.25869] — all validate graph-structured memory. */
    const char *refs_arr[32];
    int n_refs = 0;
    cJSON *refs_j = cJSON_GetObjectItem(params, "refs");
    char *refs_copy = NULL;
    if (refs_j) {
        if (cJSON_IsArray(refs_j)) {
            /* Handle JSON array format: ["key1", "key2"] */
            cJSON *item;
            cJSON_ArrayForEach(item, refs_j) {
                if (item->valuestring && n_refs < 32)
                    refs_arr[n_refs++] = item->valuestring;
            }
        } else if (refs_j->valuestring) {
            /* Handle legacy comma-separated string format */
            refs_copy = strdup(refs_j->valuestring);
            char *saveptr = NULL;
            char *tok = strtok_r(refs_copy, ",", &saveptr);
            while (tok && n_refs < 32) {
                while (*tok == ' ') tok++;  /* trim leading space */
                refs_arr[n_refs++] = tok;
                tok = strtok_r(NULL, ",", &saveptr);
            }
        }
    }

    int rc = memory_store(ctx->memory, key, value, pinned,
                          jref[0] ? jref : NULL,
                          n_refs > 0 ? refs_arr : NULL, n_refs);
    free(refs_copy);

    if (rc != 0) return make_error("failed to store memory");

    /* P2: Lesson lineage — if 'supersedes' is provided, set the lineage chain.
     * Self-Harness [arXiv:2606.09498] — harness lineage h₀→h₁→h₂. */
    cJSON *sup_j = cJSON_GetObjectItem(params, "supersedes");
    if (sup_j && sup_j->valuestring && sup_j->valuestring[0]) {
        memory_set_supersedes(ctx->memory, key, sup_j->valuestring);
    }

    /* FIX CRIT1: Defer consolidation to post-task instead of blocking inline.
     * Previously, memory_try_consolidate() ran a synchronous LLM call here,
     * adding 5-30s latency on the hot path. Now we queue the key+value pair
     * and process them all in tool_flush_deferred_consolidations() after
     * the react loop completes. */
    if (!pinned && !ctx->memory->consolidating) {
        /* FIX BUG#13: Cap deferred queue at 64 entries to bound memory usage.
         * Oldest entries are dropped if the queue is full — they'll be
         * consolidated on the next session anyway via memory_embed_all. */
        #define DEFERRED_CONSOL_MAX 64
        if (ctx->n_deferred_consol < DEFERRED_CONSOL_MAX) {
            /* Grow deferred queue if needed */
            if (ctx->n_deferred_consol >= ctx->cap_deferred_consol) {
                int new_cap = ctx->cap_deferred_consol ? ctx->cap_deferred_consol * 2 : 16;
                if (new_cap > DEFERRED_CONSOL_MAX) new_cap = DEFERRED_CONSOL_MAX;
                void *tmp = realloc(ctx->deferred_consol,
                                    (size_t)new_cap * sizeof(ctx->deferred_consol[0]));
                if (tmp) {
                    ctx->deferred_consol = tmp;
                    ctx->cap_deferred_consol = new_cap;
                }
            }
            if (ctx->n_deferred_consol < ctx->cap_deferred_consol) {
                ctx->deferred_consol[ctx->n_deferred_consol].key = strdup(key);
                ctx->deferred_consol[ctx->n_deferred_consol].value = strdup(value);
                ctx->n_deferred_consol++;
            }
        }
    }

    /* Store for audit */
    char *hash = store_save(ctx->store, value);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "key", key);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_store",
                   params, alias, strlen(value), 0, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── memory_recall ─────────────────────────────────────── */

static tool_result_t tool_memory_recall(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring || !query_j->valuestring[0])
        return make_error("memory_recall requires a non-empty 'query' string. "
                          "Describe what you're looking for.");

    const char *query = query_j->valuestring;
    memory_results_t results = memory_recall(ctx->memory, query, 5);

    /* Build result string + track recalled keys for validation scoring */
    str_t out = str_new(1024);
    for (int i = 0; i < results.count; i++) {
        memory_entry_t *e = &results.entries[i];
        str_appendf(&out, "--- %s ---\n%s\n\n", e->key, e->value);
        tool_track_recalled_key(ctx, e->key);
    }

    /* Always store result (even empty) so journal gets a ref and the
     * reactRX.md renderer can produce a clickable hyperlink. */
    char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "matches", results.count);
    cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_recall",
                   params, alias, out.len, results.count, NULL, NULL);

    memory_results_free(&results);
    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    str_free(&out);
    return make_result(1, meta, ref_copy);
}



/* ── memory_pin ─────────────────────────────────────────── */

static tool_result_t tool_memory_pin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0])
        return make_error("memory_pin requires a non-empty 'key' string. "
                          "Use memory_list to see available keys.");

    int rc = memory_pin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "pinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);
    /* P3: Regression-gated lessons advisory — pinning changes the system
     * prompt and can degrade performance. Suggest validation. */
    cJSON_AddStringToObject(meta, "harness_note",
        "Pinned memories alter system prompt for all future sessions. "
        "Validate with: nash --regression --validate-harness compare");

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_pin",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── memory_unpin ───────────────────────────────────────── */

static tool_result_t tool_memory_unpin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0])
        return make_error("memory_unpin requires a non-empty 'key' string.");

    int rc = memory_unpin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "unpinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_unpin",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── memory_delete ──────────────────────────────────────── */

static tool_result_t tool_memory_delete(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0])
        return make_error("memory_delete requires a non-empty 'key' string.");

    int rc = memory_delete(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "deleted");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_delete",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── memory_list ────────────────────────────────────────── */

static tool_result_t tool_memory_list(tool_ctx_t *ctx, cJSON *params) {
    if (!ctx->memory)
        return make_error("memory not available");

    const char *type_filter = NULL;
    cJSON *type_j = cJSON_GetObjectItem(params, "type");
    if (type_j && type_j->valuestring && type_j->valuestring[0])
        type_filter = type_j->valuestring;

    char *listing = memory_build_listing(ctx->memory, type_filter);
    if (!listing)
        return make_error("no memory entries found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    if (type_filter)
        cJSON_AddStringToObject(meta, "filter", type_filter);

    char *ref = store_save(ctx->store, listing);
    char *alias = tool_register_alias(ctx, ref ? ref : "");
    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_list",
                   params, alias, listing ? strlen(listing) : 0, 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(ref);
    free(listing);
    return make_result(1, meta, ref_copy);
}

/* ── web_fetch ──────────────────────────────────────────── */

/* ── HTML text extraction ──────────────────────────────── */

/* Case-insensitive prefix match. Returns 1 if s starts with prefix (ASCII). */
static int html_ci_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (!*s) return 0;  /* FIX BUG#6: don't read past end of s */
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Decode one HTML entity at &...; Returns decoded char count written to out.
 * *advance = bytes consumed from src (including & and ;). */
static int html_decode_entity(const char *src, char *out, int *advance) {
    const char *p = src + 1; /* skip '&' */
    const char *semi = NULL;
    for (const char *q = p; q < src + 12 && *q; q++) {
        if (*q == ';') { semi = q; break; }
    }
    if (!semi) { *advance = 1; out[0] = '&'; return 1; }

    int elen = (int)(semi - p);
    *advance = (int)(semi - src) + 1;

    if (elen == 2 && p[0] == 'l' && p[1] == 't') { out[0] = '<'; return 1; }
    if (elen == 2 && p[0] == 'g' && p[1] == 't') { out[0] = '>'; return 1; }
    if (elen == 3 && p[0] == 'a' && p[1] == 'm' && p[2] == 'p') { out[0] = '&'; return 1; }
    if (elen == 4 && p[0] == 'q' && p[1] == 'u' && p[2] == 'o' && p[3] == 't') { out[0] = '"'; return 1; }
    if (elen == 4 && p[0] == 'a' && p[1] == 'p' && p[2] == 'o' && p[3] == 's') { out[0] = '\''; return 1; }
    if (elen == 4 && p[0] == 'n' && p[1] == 'b' && p[2] == 's' && p[3] == 'p') { out[0] = ' '; return 1; }

    /* &#NNN; or &#xHHH; */
    if (p[0] == '#') {
        unsigned long cp = 0;
        if (p[1] == 'x' || p[1] == 'X')
            cp = strtoul(p + 2, NULL, 16);
        else
            cp = strtoul(p + 1, NULL, 10);
        if (cp > 0 && cp < 128) { out[0] = (char)cp; return 1; }
        if (cp >= 128 && cp <= 0x7FF) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp >= 0x800 && cp <= 0xFFFF) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            out[0] = (char)(0xF0 | (cp >> 18));
            out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (char)(0x80 | (cp & 0x3F));
            return 4;
        }
        out[0] = '?'; return 1;
    }

    /* Unknown entity — pass through as-is */
    out[0] = '&'; *advance = 1; return 1;
}

/* Extract readable text from HTML. Strips tags, preserves <a href> as markdown
 * links [text](url), strips <script>/<style> blocks entirely, decodes entities,
 * collapses whitespace. Returns malloc'd string; caller frees. */
static char *html_extract_text(const char *html, size_t len) {
    if (!html || len == 0) return NULL;

    str_t out = str_new(len / 3);  /* text is typically 1/3 to 1/10 of HTML */
    const char *p = html;
    const char *end = html + len;
    int in_skip = 0;       /* inside <script> or <style> block */
    int prev_space = 0;    /* previous char was whitespace (for collapsing) */
    int newline_count = 0; /* consecutive newlines (cap at 2) */
    int in_pre = 0;        /* inside <pre> block (preserve whitespace) */

    /* Link state: when we encounter <a href="...">, we buffer the link text
     * and emit [text](url) when we see </a> */
    char link_url[2048];
    str_t link_text = {0};
    int in_link = 0;

    while (p < end) {
        if (*p == '<') {
            const char *tag_start = p + 1;

            /* Skip HTML comments <!-- ... --> */
            if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
                const char *cend = strstr(p + 4, "-->");
                if (cend) { p = cend + 3; continue; }
                else { p = end; break; }
            }

            /* Find end of tag */
            const char *gt = memchr(p, '>', end - p);
            if (!gt) break;

            /* Check for skip blocks: <script>, <style> */
            if (html_ci_prefix(tag_start, "script") &&
                (tag_start[6] == '>' || tag_start[6] == ' ' || tag_start[6] == '\t')) {
                in_skip = 1;
                p = gt + 1;
                continue;
            }
            if (html_ci_prefix(tag_start, "style") &&
                (tag_start[5] == '>' || tag_start[5] == ' ' || tag_start[5] == '\t')) {
                in_skip = 1;
                p = gt + 1;
                continue;
            }
            if (in_skip) {
                if (html_ci_prefix(tag_start, "/script") || html_ci_prefix(tag_start, "/style")) {
                    in_skip = 0;
                }
                p = gt + 1;
                continue;
            }

            /* <pre> tracking */
            if (html_ci_prefix(tag_start, "pre") &&
                (tag_start[3] == '>' || tag_start[3] == ' '))
                in_pre = 1;
            if (html_ci_prefix(tag_start, "/pre"))
                in_pre = 0;

            /* <a href="..."> — start capturing link */
            if (html_ci_prefix(tag_start, "a ") || html_ci_prefix(tag_start, "a\t")) {
                /* Extract href */
                const char *href = NULL;
                for (const char *q = tag_start; q < gt - 4; q++) {
                    if (html_ci_prefix(q, "href")) {
                        q += 4;
                        while (q < gt && (*q == ' ' || *q == '=')) q++;
                        if (q < gt && (*q == '"' || *q == '\'')) {
                            char quote = *q++;
                            href = q;
                            while (q < gt && *q != quote) q++;
                            size_t hlen = (size_t)(q - href);
                            if (hlen > 0 && hlen < sizeof(link_url)) {
                                memcpy(link_url, href, hlen);
                                link_url[hlen] = '\0';
                                /* Decode entities in URL (e.g. &amp; → &) */
                                char *rp = link_url, *wp = link_url;
                                while (*rp) {
                                    if (*rp == '&') {
                                        char dec[4]; int adv = 0;
                                        int dl = html_decode_entity(rp, dec, &adv);
                                        for (int di = 0; di < dl; di++) *wp++ = dec[di];
                                        rp += adv;
                                    } else {
                                        *wp++ = *rp++;
                                    }
                                }
                                *wp = '\0';
                                /* Free previous link_text if nested <a> (malformed HTML) */
                                if (in_link) str_free(&link_text);
                                in_link = 1;
                                link_text = str_new(64);
                            }
                        }
                        break;
                    }
                }
                p = gt + 1;
                continue;
            }

            /* </a> — emit markdown link */
            if (html_ci_prefix(tag_start, "/a")) {
                if (in_link) {
                    /* Emit [text](url) */
                    if (link_text.len > 0) {
                        str_append_cstr(&out, "[");
                        str_append(&out, link_text.data, link_text.len);
                        str_append_cstr(&out, "](");
                        str_append_cstr(&out, link_url);
                        str_append_cstr(&out, ")");
                        prev_space = 0;
                        newline_count = 0;
                    }
                    str_free(&link_text);
                    in_link = 0;
                }
                p = gt + 1;
                continue;
            }

            /* Block-level tags → insert newline */
            int is_block = 0;
            const char *btags[] = {"p", "div", "br", "h1", "h2", "h3", "h4",
                                   "h5", "h6", "li", "tr", "dt", "dd",
                                   "blockquote", "section", "article",
                                   "header", "footer", "nav", "figure",
                                   "figcaption", "main", "aside",
                                   "/p", "/div", "/h1", "/h2", "/h3", "/h4",
                                   "/h5", "/h6", "/li", "/tr", "/ul", "/ol",
                                   "/table", "/blockquote", "/section",
                                   "/article", "/header", "/footer",
                                   NULL};
            for (int i = 0; btags[i]; i++) {
                size_t blen = strlen(btags[i]);
                if (html_ci_prefix(tag_start, btags[i]) &&
                    (tag_start[blen] == '>' || tag_start[blen] == ' ' ||
                     tag_start[blen] == '/' || tag_start[blen] == '\t')) {
                    is_block = 1;
                    break;
                }
            }

            /* <br> and <br/> always produce a newline */
            if (html_ci_prefix(tag_start, "br") &&
                (tag_start[2] == '>' || tag_start[2] == ' ' ||
                 tag_start[2] == '/' || tag_start[2] == '\t'))
                is_block = 1;

            if (is_block && out.len > 0 && newline_count < 2) {
                str_append(&out, "\n", 1);
                newline_count++;
                prev_space = 1;
            }

            p = gt + 1;
            continue;
        }

        /* Inside a skip block — ignore all text */
        if (in_skip) { p++; continue; }

        /* Entity decoding */
        if (*p == '&') {
            char decoded[4];
            int advance = 0;
            int dlen = html_decode_entity(p, decoded, &advance);
            for (int i = 0; i < dlen; i++) {
                char c = decoded[i];
                int is_ws = (c == ' ' || c == '\t' || c == '\r');
                int is_nl = (c == '\n');

                if (!in_pre && (is_ws || is_nl)) {
                    if (is_nl) {
                        if (newline_count < 2) {
                            if (in_link)
                                str_append(&link_text, "\n", 1);
                            else
                                str_append(&out, "\n", 1);
                            newline_count++;
                        }
                    } else if (!prev_space) {
                        if (in_link)
                            str_append(&link_text, " ", 1);
                        else
                            str_append(&out, " ", 1);
                    }
                    prev_space = 1;
                } else {
                    if (in_link)
                        str_append(&link_text, &c, 1);
                    else
                        str_append(&out, &c, 1);
                    prev_space = 0;
                    newline_count = 0;
                }
            }
            p += advance;
            continue;
        }

        /* Regular text character */
        char c = *p;
        int is_ws = (c == ' ' || c == '\t' || c == '\r');
        int is_nl = (c == '\n');

        if (!in_pre && (is_ws || is_nl)) {
            if (is_nl) {
                if (newline_count < 2) {
                    if (in_link)
                        str_append(&link_text, "\n", 1);
                    else
                        str_append(&out, "\n", 1);
                    newline_count++;
                }
            } else if (!prev_space) {
                if (in_link)
                    str_append(&link_text, " ", 1);
                else
                    str_append(&out, " ", 1);
            }
            prev_space = 1;
        } else {
            if (in_link)
                str_append(&link_text, &c, 1);
            else
                str_append(&out, &c, 1);
            prev_space = 0;
            if (!is_nl) newline_count = 0;
        }

        p++;
    }

    /* Clean up any unclosed link */
    if (in_link && link_text.len > 0)
        str_append(&out, link_text.data, link_text.len);
    str_free(&link_text);  /* safe even if str_new was never called ({0} → free(NULL)) */

    if (out.len == 0) { str_free(&out); return NULL; }
    return str_steal(&out);
}

static size_t web_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *buf = userdata;
    size_t total = size * nmemb;
    /* Cap at 500KB to prevent memory explosion */
    if (buf->len + total > 512000) {
        size_t remaining = 512000 - buf->len;
        if (remaining > 0) str_append(buf, ptr, remaining);
        return total;  /* signal curl we're done; excess data is silently dropped */
    }
    str_append(buf, ptr, total);
    return total;
}

static tool_result_t tool_web_fetch(tool_ctx_t *ctx, cJSON *params) {
    cJSON *url_j = cJSON_GetObjectItem(params, "url");
    if (!url_j || !url_j->valuestring || !url_j->valuestring[0])
        return make_error("web_fetch requires a non-empty 'url' string. "
                          "Provide the full URL (https://...) to fetch.");

    const char *url = url_j->valuestring;

    CURL *curl = curl_easy_init();
    if (!curl) return make_error("curl_easy_init failed");

    str_t body = str_new(8192);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    long web_timeout = (ctx->cfg && ctx->cfg->web_timeout > 0)
                       ? (long)ctx->cfg->web_timeout : 30L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, web_timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char *content_type = NULL;
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) content_type = strdup(ct);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "fetch failed: %s", curl_easy_strerror(res));
        str_free(&body);
        free(content_type);
        return make_error(msg);
    }

    /* For HTML content, extract text to save context tokens */
    char *store_data = body.data;
    size_t store_len = body.len;
    char *extracted = NULL;
    if (content_type && strstr(content_type, "text/html")) {
        extracted = html_extract_text(body.data, body.len);
        if (extracted) {
            store_data = extracted;
            store_len = strlen(extracted);
        }
    }

    /* Store to shared store */
    char *hash = store_save(ctx->store, store_data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    /* Build metadata */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "url", url);
    cJSON_AddNumberToObject(meta, "http_code", http_code);
    cJSON_AddNumberToObject(meta, "chars", (double)store_len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(store_data));
    if (content_type) cJSON_AddStringToObject(meta, "content_type", content_type);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);
    if (extracted)
        cJSON_AddNumberToObject(meta, "original_chars", (double)body.len);

    /* Content stored to .store/ — model reads via file_read(ref) */

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_fetch",
                   params, alias, store_len, count_lines(store_data), NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(extracted);
    free(content_type);
    str_free(&body);
    return make_result(http_code >= 200 && http_code < 400, meta, ref_copy);
}

/* ── web_search ────────────────────────────────────────── */

/* Track whether we auto-started a SearXNG container so we can tear it down
 * on nash exit.  0 = not started, 1 = we started it. */
static int searxng_auto_started = 0;

/* Check if a URL is reachable (HTTP GET, expect 2xx). Returns 1 if up. */
static int searxng_is_running(const char *base_url) {
    str_t body = str_new(256);
    int rc = http_get(base_url, 3, &body);
    str_free(&body);
    return (rc == 0);
}

/* Extract host:port from a URL like "http://localhost:8888/search".
 * Returns the port number, or 8888 as default. */
static int searxng_port_from_url(const char *url) {
    /* Find "://", skip it, then find ":" for port */
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *colon = strchr(p, ':');
    if (colon) {
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) return port;
    }
    return 8888;
}

/* Build the SearXNG base URL (without /search path) from the configured URL.
 * e.g. "http://localhost:8888/search" → "http://localhost:8888"
 * Caller must free the returned string. */
static char *searxng_base_url(const char *url) {
    /* Find the path component after host:port */
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t len = (size_t)(slash - url);
        char *base = malloc(len + 1);
        memcpy(base, url, len);
        base[len] = '\0';
        return base;
    }
    return strdup(url);
}

/* Ensure the persistent SearXNG config directory exists at ~/.nash/searxng/
 * with a settings.yml that enables JSON format.  This directory is bind-mounted
 * into the container so the setting survives container recreation.
 * Returns the path to the config directory (static buffer, do not free). */
static const char *ensure_searxng_config_dir(void) {
    static char cfg_dir[512] = {0};
    if (cfg_dir[0]) return cfg_dir;

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/.nash/searxng", home);

    /* Create the directory */
    mkdir_p(cfg_dir, 0755);

    /* Write settings.yml if it doesn't exist or is missing json format */
    char settings_path[600];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.yml", cfg_dir);

    /* Check if settings.yml already exists and has json format enabled */
    int needs_write = 0;
    FILE *f = fopen(settings_path, "r");
    if (!f) {
        needs_write = 1;
    } else {
        /* Check if it contains "- json" in formats */
        char line[256];
        int has_json = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "- json")) { has_json = 1; break; }
        }
        fclose(f);
        if (!has_json) needs_write = 1;
    }

    if (needs_write) {
        f = fopen(settings_path, "w");
        if (f) {
            fprintf(f,
                "# Nash auto-generated SearXNG settings\n"
                "# This file is bind-mounted into the SearXNG container.\n"
                "# It uses use_default_settings to inherit all defaults\n"
                "# and only overrides what nash needs (JSON API format).\n"
                "\n"
                "use_default_settings: true\n"
                "\n"
                "search:\n"
                "  formats:\n"
                "    - html\n"
                "    - json\n"
                "\n"
                "server:\n"
                "  secret_key: \"nash-searxng-auto-generated-key\"\n"
            );
            fclose(f);
            nash_log("[nash] Created SearXNG settings at %s", settings_path);
        } else {
            nash_log("[nash] Warning: could not write SearXNG settings to %s", settings_path);
        }
    }

    return cfg_dir;
}

/* Start a SearXNG container using podman/docker.
 * Bind-mounts ~/.nash/searxng/ into the container for persistent config.
 * Returns 0 on success, -1 on failure. */
/* Forward declaration — run_container_cmd is defined below (near web_search_cleanup). */
static int run_container_cmd(const char *runtime, const char *action, const char *name);

/* FIX #1: Use fork/exec instead of system() to avoid shell injection via cfg_dir.
 * Previously used system() with snprintf-constructed commands — if $HOME contained
 * shell metacharacters, the command could be exploited. The cleanup function
 * (run_container_cmd/web_search_cleanup) was already migrated; this aligns startup. */
static int searxng_start_with_runtime(const char *runtime, int port, const char *cfg_dir,
                                       const char *vol_suffix) {
    char port_map[32], base_url_env[160], vol_mount[640];
    snprintf(port_map, sizeof(port_map), "%d:8080", port);
    snprintf(base_url_env, sizeof(base_url_env),
             "SEARXNG_BASE_URL=http://localhost:%d/", port);
    snprintf(vol_mount, sizeof(vol_mount), "%s:/etc/searxng%s", cfg_dir, vol_suffix);

    /* Remove any existing container (ignore failure) */
    run_container_cmd(runtime, "rm", "nash-searxng");

    /* Run new container */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(runtime, runtime, "run", "-d", "--name", "nash-searxng",
               "-p", port_map,
               "-e", base_url_env,
               "-v", vol_mount,
               "docker.io/searxng/searxng:latest",
               (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int searxng_start_container(int port) {
    /* Ensure persistent config directory with JSON format enabled */
    const char *cfg_dir = ensure_searxng_config_dir();

    /* Try podman first (with :rw,Z SELinux label), then docker (without Z) */
    int rc = searxng_start_with_runtime("podman", port, cfg_dir, ":rw,Z");
    if (rc != 0) {
        rc = searxng_start_with_runtime("docker", port, cfg_dir, ":rw");
    }
    if (rc != 0) return -1;

    searxng_auto_started = 1;

    /* Wait for SearXNG to become ready (up to 30 seconds) */
    char health_url[256];
    snprintf(health_url, sizeof(health_url), "http://localhost:%d/", port);
    int ready = 0;
    for (int i = 0; i < 30; i++) {
        sleep(1);
        if (searxng_is_running(health_url)) { ready = 1; break; }
    }
    if (!ready) return -1;  /* timed out */

    return 0;
}

/* Ensure SearXNG is running. Starts container if needed.
 * Returns 0 if SearXNG is available, -1 on failure. */
static int ensure_searxng(const char *searxng_url) {
    char *base = searxng_base_url(searxng_url);

    /* First check if it's already running (user-managed or previously started) */
    if (searxng_is_running(base)) {
        free(base);
        return 0;
    }

    /* Not running — auto-start a container */
    int port = searxng_port_from_url(searxng_url);
    nash_log("[nash] SearXNG not running at %s — starting container on port %d...",
             base, port);
    free(base);

    if (searxng_start_container(port) != 0) {
        nash_log("[nash] Failed to start SearXNG container");
        return -1;
    }
    nash_log("[nash] SearXNG container started successfully");
    return 0;
}

/* Perform a search using SearXNG JSON API.
 * Returns a formatted results string (caller frees), or NULL on failure.
 * *out_count receives the number of results. */
static char *searxng_search(const char *searxng_url, const char *query,
                            int *out_count, long timeout) {
    /* URL-encode the query using a temporary curl handle */
    CURL *enc = curl_easy_init();
    if (!enc) return NULL;
    char *encoded_q = curl_easy_escape(enc, query, 0);
    curl_easy_cleanup(enc);

    char url[2048];
    snprintf(url, sizeof(url), "%s?q=%s&format=json&categories=general",
             searxng_url, encoded_q);
    curl_free(encoded_q);

    str_t body = str_new(NASH_INITIAL_BUF);
    long http_code = 0;
    if (http_get_web(url, timeout, &body, &http_code) != 0) {
        str_free(&body);
        return NULL;
    }

    if (http_code == 403) {
        nash_log(
                "[nash] SearXNG returned 403 Forbidden for JSON format. "
                "Check ~/.nash/searxng/settings.yml has 'json' in "
                "search.formats and restart the container.");
        str_free(&body);
        return NULL;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(body.data);
    str_free(&body);
    if (!root) return NULL;

    cJSON *results_arr = cJSON_GetObjectItem(root, "results");
    if (!results_arr || !cJSON_IsArray(results_arr)) {
        cJSON_Delete(root);
        return NULL;
    }

    str_t results = str_new(4096);
    int count = 0;
    int arr_size = cJSON_GetArraySize(results_arr);

    for (int i = 0; i < arr_size && count < 10; i++) {
        cJSON *item = cJSON_GetArrayItem(results_arr, i);
        if (!item) continue;

        cJSON *title_j   = cJSON_GetObjectItem(item, "title");
        cJSON *url_j     = cJSON_GetObjectItem(item, "url");
        cJSON *content_j = cJSON_GetObjectItem(item, "content");

        const char *title   = (title_j && title_j->valuestring) ? title_j->valuestring : "";
        const char *item_url = (url_j && url_j->valuestring) ? url_j->valuestring : "";
        const char *content = (content_j && content_j->valuestring) ? content_j->valuestring : "";

        if (!item_url[0]) continue;

        count++;
        if (title[0] && content[0]) {
            str_appendf(&results, "%d. [%s](%s)\n   %s\n\n", count, title, item_url, content);
        } else if (title[0]) {
            str_appendf(&results, "%d. [%s](%s)\n\n", count, title, item_url);
        } else {
            str_appendf(&results, "%d. %s\n\n", count, item_url);
        }
    }

    cJSON_Delete(root);
    *out_count = count;

    if (count == 0) {
        str_free(&results);
        return NULL;
    }

    return str_steal(&results);
}

static tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring || !query_j->valuestring[0])
        return make_error("web_search requires a non-empty 'query' string. "
                          "Provide specific search terms.");

    const char *query = query_j->valuestring;
    const char *searxng_url = (ctx->cfg) ? ctx->cfg->searxng_url : NULL;
    char *results_text = NULL;
    int result_count = 0;
    long search_timeout = (ctx->cfg && ctx->cfg->web_timeout > 0)
                          ? (long)ctx->cfg->web_timeout : 30L;

    /* SearXNG is the sole search backend. Auto-start if not running. */
    if (ensure_searxng(searxng_url) != 0) {
        return make_error("SearXNG not available and could not be auto-started. "
                          "Install podman/docker or configure a running SearXNG instance "
                          "in ~/.nash/config.toml [search] section.");
    }
    results_text = searxng_search(searxng_url, query, &result_count, search_timeout);

    if (!results_text || result_count == 0) {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "no results found for query: %s", query);
        /* Store error to .store/ so it gets a ref for reactRX.md hyperlink */
        char *err_hash = store_save(ctx->store, errmsg);
        char *err_alias = tool_register_alias(ctx, err_hash ? err_hash : "");
        inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                       params, err_alias, strlen(errmsg), 0, errmsg, NULL);
        free(err_hash);
        free(err_alias);
        free(results_text);
        return make_error(errmsg);
    }

    /* Store results */
    char *hash = store_save(ctx->store, results_text);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "query", query);
    cJSON_AddNumberToObject(meta, "results", result_count);
    cJSON_AddNumberToObject(meta, "chars", (double)strlen(results_text));
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                   params, alias, strlen(results_text), result_count, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(results_text);
    return make_result(1, meta, ref_copy);
}

/* FIX CRIT1: Process all deferred consolidations after task completion.
 * This moves the LLM-based classification + merge calls out of the hot path.
 * Each queued entry gets consolidated against existing memories. */
void tool_flush_deferred_consolidations(tool_ctx_t *ctx) {
    if (!ctx || ctx->n_deferred_consol == 0) return;

    /* FIX D4: Collect keys to delete during consolidation, then batch-delete.
     * Previously each memory_try_consolidate() called memory_delete() inline,
     * causing O(N) gc_refs scan per delete.  Now we collect all delete keys
     * and do a single memory_delete_batch() at the end — O(K+N) total. */
    char **del_keys = NULL;
    int n_del = 0, del_cap = 0;

    ctx->memory->consolidating = 1;  /* prevent recursive consolidation */
    for (int i = 0; i < ctx->n_deferred_consol; i++) {
        if (ctx->deferred_consol[i].key && ctx->deferred_consol[i].value) {
            char *dk = memory_try_consolidate(ctx, ctx->deferred_consol[i].key,
                                              ctx->deferred_consol[i].value);
            if (dk) {
                if (n_del >= del_cap) {
                    del_cap = del_cap ? del_cap * 2 : 16;
                    del_keys = realloc(del_keys, sizeof(char *) * (size_t)del_cap);
                }
                if (del_keys) del_keys[n_del++] = dk;
                else free(dk);
            }
        }
    }
    ctx->memory->consolidating = 0;

    /* Batch delete all keys collected during consolidation */
    if (n_del > 0 && del_keys) {
        memory_delete_batch(ctx->memory, (const char **)del_keys, n_del);
        for (int i = 0; i < n_del; i++) free(del_keys[i]);
    }
    free(del_keys);

    /* Free the queue */
    tool_free_deferred_consolidations(ctx);
}

/* FIX CRIT1: Free the deferred consolidation queue without processing. */
void tool_free_deferred_consolidations(tool_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->n_deferred_consol; i++) {
        free(ctx->deferred_consol[i].key);
        free(ctx->deferred_consol[i].value);
    }
    free(ctx->deferred_consol);
    ctx->deferred_consol = NULL;
    ctx->n_deferred_consol = 0;
    ctx->cap_deferred_consol = 0;
}

/* Tear down auto-started SearXNG container. Called on nash exit. */
/* FIX BUG#12: Use fork/exec instead of system() which is not signal-safe
 * and invokes /bin/sh unnecessarily. */
static int run_container_cmd(const char *runtime, const char *action, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(runtime, runtime, action, name, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void web_search_cleanup(void) {
    if (!searxng_auto_started) return;
    nash_log("[nash] Stopping auto-started SearXNG container...");
    int rc = run_container_cmd("podman", "stop", "nash-searxng");
    if (rc == 0) run_container_cmd("podman", "rm", "nash-searxng");
    if (rc != 0) {
        /* Try docker as fallback */
        run_container_cmd("docker", "stop", "nash-searxng");
        run_container_cmd("docker", "rm", "nash-searxng");
    }
    searxng_auto_started = 0;
}


/* Tool dispatch table — maps tool names to handler functions.
 * Fix #11: Handlers are listed in the SAME ORDER as TOOL_REGISTRY
 * entries in tools_registry.c.  A compile-time assertion ensures the
 * counts stay in sync, so adding a new tool to TOOL_REGISTRY without
 * adding a handler here (or vice versa) is a build error.
 *
 * Adding a new tool:
 *   1. Add handler function above (static tool_result_t tool_xxx(...))
 *   2. Add entry to TOOL_HANDLERS[] below  (same position as in TOOL_REGISTRY)
 *   3. Add entry to TOOL_REGISTRY[] in tools_registry.c (same position)
 *   4. Increment TOOL_REGISTRY_COUNT in tools_registry.c */
typedef tool_result_t (*tool_handler_fn)(tool_ctx_t *, cJSON *);

/* Handlers only — names come from TOOL_REGISTRY[i].name at dispatch time.
 * Order must match TOOL_REGISTRY exactly. */
static const tool_handler_fn TOOL_HANDLERS[] = {
    tool_shell_exec,       /* shell_exec    */
    tool_file_read,        /* file_read     */
    tool_file_write,       /* file_write    */
    tool_file_edit,        /* file_edit     */
    tool_grep_search,      /* grep_search   */
    tool_web_fetch,        /* web_fetch     */
    tool_web_search,       /* web_search    */
    tool_glob_search,      /* glob_search   */
    tool_memory_store,     /* memory_store  */
    tool_memory_recall,    /* memory_recall */
    tool_memory_pin,       /* memory_pin    */
    tool_memory_unpin,     /* memory_unpin  */
    tool_done,             /* done          */
    tool_plan,             /* plan          */
    tool_notes,            /* notes         */
    tool_user_ask_stub,    /* user_ask      */
    tool_memory_delete,    /* memory_delete */
    tool_memory_list,      /* memory_list   */
};

/* Compile-time assertion: handler count must match registry count.
 * If this fails, you added a tool to one table but not the other. */
/* FIX #14: Use TOOL_REGISTRY_COUNT macro instead of magic number 18.
 * Adding a tool to one table but not the other now produces a clear
 * compile-time error referencing the macro name. */
_Static_assert(sizeof(TOOL_HANDLERS) / sizeof(TOOL_HANDLERS[0]) == TOOL_REGISTRY_COUNT,
               "TOOL_HANDLERS count must match TOOL_REGISTRY_COUNT");

tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    /* Tool filter: check whitelist/blacklist before dispatch */
    if (ctx->tool_filter.allowed) {
        int found = 0;
        for (int i = 0; i < ctx->tool_filter.n_allowed; i++)
            if (strcmp(action, ctx->tool_filter.allowed[i]) == 0) { found = 1; break; }
        if (!found) return make_error("tool not available in this context");
    }
    if (ctx->tool_filter.blocked) {
        for (int i = 0; i < ctx->tool_filter.n_blocked; i++)
            if (strcmp(action, ctx->tool_filter.blocked[i]) == 0)
                return make_error("tool not available in this context");
    }

    /* Dispatch via unified registry lookup (Fix #11).
     * TOOL_REGISTRY[i].name provides the name, TOOL_HANDLERS[i] the handler. */
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        if (strcmp(action, TOOL_REGISTRY[i].name) == 0)
            return TOOL_HANDLERS[i](ctx, params);
    }

    /* Concatenated tool name recovery: when the model emits a garbled name
     * like "file_readfile_read" or "shell_execmemory_recall", try to find
     * a known tool name as a prefix.  Pick the longest matching prefix to
     * avoid false positives (e.g. "done" matching "donefile_read"). */
    {
        const char *best_name = NULL;
        int best_idx = -1;
        size_t best_len = 0;
        size_t action_len = strlen(action);

        for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
            size_t nlen = strlen(TOOL_REGISTRY[i].name);
            if (nlen < action_len && nlen > best_len &&
                strncmp(action, TOOL_REGISTRY[i].name, nlen) == 0) {
                best_name = TOOL_REGISTRY[i].name;
                best_idx = i;
                best_len = nlen;
            }
        }

        if (best_idx >= 0) {
            /* Re-check tool filter for the recovered name — without this,
             * a blocked tool could be reached by concatenating its name
             * with another tool name (e.g. "web_searchfile_read" bypasses
             * block=["web_search"]). */
            if (ctx->tool_filter.allowed) {
                int found = 0;
                for (int i = 0; i < ctx->tool_filter.n_allowed; i++)
                    if (strcmp(best_name, ctx->tool_filter.allowed[i]) == 0) { found = 1; break; }
                if (!found) return make_error("tool not available in this context");
            }
            if (ctx->tool_filter.blocked) {
                for (int i = 0; i < ctx->tool_filter.n_blocked; i++)
                    if (strcmp(best_name, ctx->tool_filter.blocked[i]) == 0)
                        return make_error("tool not available in this context");
            }
            fprintf(stderr, "[tool] recovered concatenated tool name: "
                    "'%s' → '%s' (dropped suffix: '%s')\n",
                    action, best_name, action + best_len);
            return TOOL_HANDLERS[best_idx](ctx, params);
        }
    }

    /* Truly unknown tool — build available tools list from the unified registry. */
    char msg[1024];
    int pos = snprintf(msg, sizeof(msg), "unknown tool: '%.100s'. Available: ", action);
    for (int i = 0; i < TOOL_REGISTRY_COUNT && pos < (int)sizeof(msg) - 32; i++) {
        if (i > 0) pos += snprintf(msg + pos, sizeof(msg) - pos, ", ");
        pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", TOOL_REGISTRY[i].name);
    }
    return make_error(msg);
}

void tool_result_free(tool_result_t *r) {
    if (r->meta) cJSON_Delete(r->meta);
    free(r->store_ref);
    r->meta = NULL;
    r->store_ref = NULL;
}

/* ── system prompt ───────────────────────────────────── */

char *tools_system_prompt(void) {
    /* Returns a newly heap-allocated string. Caller must free(). */

    /* UTC timestamp (gmtime_r is thread-safe unlike gmtime) */
    time_t now = time(NULL);
    struct tm utc_buf;
    struct tm *utc = gmtime_r(&now, &utc_buf);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M UTC", utc);

    /* Current working directory */
    char cwdbuf[1024];
    if (!getcwd(cwdbuf, sizeof(cwdbuf)))
        snprintf(cwdbuf, sizeof(cwdbuf), "(unknown)");

    char *buf = malloc(NASH_PATH_MAX);
    if (!buf) return strdup("");

    snprintf(buf, NASH_PATH_MAX,
        "You are an autonomous coding agent. Solve the user's task step by step "
        "using the available tools.\n"
        "\n"
        "Now is %s. CWD: %s\n"
        "\n"
        "Store-and-reference pattern:\n"
        "- Most tool outputs are stored to disk. You see only metadata with a ref "
        "alias (R0S1, R0S2, etc.).\n"
        "- To read the actual content, call file_read(path=\"R0S1\").\n"
        "- You MUST file_read the ref if you need to see what a command produced "
        "or what a file contains.\n"
        "\n"
        "Rules:\n"
        "- Never invoke tools speculatively. Every tool call must have a clear reason "
        "and you MUST read the result (file_read the ref) before proceeding.\n"
        "- Never guess tool results. Wait for actual output.\n"
        "- file_edit: old_text must match exactly. Always file_read first.\n"
        "- Use dedicated tools (file_read, grep_search, glob_search) instead of "
        "shell_exec equivalents (cat, grep, find, sed, head, tail). "
        "file_read supports start_line/end_line for reading specific line ranges.\n"
        "- Record key findings in notes — they survive context eviction.\n"
        "- Call done with the final answer when finished.\n",
        timebuf, cwdbuf);

    return buf;
}

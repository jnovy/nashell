#include "tools.h"
#include "memory.h"
#include "str.h"
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
    if (!ctx || !ctx->aliases) return strdup("R?S?");

    char alias_buf[32];
    snprintf(alias_buf, sizeof(alias_buf), "R%dS%d", ctx->react_loop, ctx->aliases->next_seq);
    ctx->aliases->next_seq++;

    alias_map_insert(ctx->aliases, alias_buf, hash ? hash : "");

    /* Create symlink in session directory: R1S0 → ../store/hash */
    if (ctx->session_dir && hash && hash[0]) {
        char link_path[4096];
        char target[4096];
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

void tool_track_recalled_key(tool_ctx_t *ctx, const char *key) {
    if (!ctx || !key) return;
    /* Deduplicate: don't track the same key twice */
    for (int i = 0; i < ctx->n_recalled_keys; i++)
        if (strcmp(ctx->recalled_keys[i], key) == 0) return;
    /* Grow if needed */
    if (ctx->n_recalled_keys >= ctx->recalled_keys_cap) {
        int new_cap = ctx->recalled_keys_cap ? ctx->recalled_keys_cap * 2 : 16;
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
            char buf[4096];
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
            char buf[4096];
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
                char buf[4096];
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
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── shell_exec ──────────────────────────────────────── */

static tool_result_t tool_shell_exec(tool_ctx_t *ctx, cJSON *params) {
    cJSON *cmd_j = cJSON_GetObjectItem(params, "command");
    if (!cmd_j || !cmd_j->valuestring)
        return make_error("missing 'command' parameter");

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
            char preview[256];
            utf8_truncate(preview, out.data, 200);
            strcat(preview, "...");
            cJSON_AddStringToObject(meta, "preview", preview);
        }
    } else if (out.len >= 500) {
        /* Large output: first ~200 bytes with ... suffix (UTF-8 safe) */
        char preview[256];
        utf8_truncate(preview, out.data, 200);
        strcat(preview, "...");
        cJSON_AddStringToObject(meta, "preview", preview);
    }

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
    if (!path_j || !path_j->valuestring)
        return make_error("missing 'path' parameter");

    const char *path = path_j->valuestring;

    /* Resolve step aliases (S0, S1, S2...) */
    char *resolved = tool_resolve_alias(ctx, path);  /* heap-allocated, must free */
    if (resolved) path = resolved;

    /* Resolve store/ paths relative to session directory (legacy) */
    char resolved_buf[4096];
    if (strncmp(path, "store/", 6) == 0) {
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
    if (!path_j || !path_j->valuestring || !content_j || !content_j->valuestring)
        return make_error("missing 'path' or 'content' parameter");

    const char *path = path_j->valuestring;
    const char *content = content_j->valuestring;

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
    if (!path_j || !path_j->valuestring ||
        !old_text_j || !old_text_j->valuestring ||
        !new_text_j || !new_text_j->valuestring)
        return make_error("missing 'path', 'old_text', or 'new_text' parameter");

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

    char *pos = strstr(content, old_text);
    if (!pos) {
        free(content);
        free(pre_hash);
        return make_error("old_text not found in file");
    }

    /* Build new content */
    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t result_len = flen - old_len + new_len;
    char *result = malloc(result_len + 1);
    if (!result) { free(content); free(pre_hash); return make_error("malloc failed"); }

    size_t prefix_len = (size_t)(pos - content);
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, new_text, new_len);
    memcpy(result + prefix_len + new_len, pos + old_len, flen - prefix_len - old_len);
    result[result_len] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) { free(content); free(result); free(pre_hash); return make_error("cannot write file"); }
    fwrite(result, 1, result_len, f);
    fclose(f);

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
    if (!pattern_j || !pattern_j->valuestring)
        return make_error("missing 'pattern' parameter");

    const char *pattern = pattern_j->valuestring;
    const char *path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Resolve step aliases (returns heap-allocated string, caller must free) */
    char *resolved = tool_resolve_alias(ctx, path);
    if (resolved) path = resolved;

    /* Resolve store/ paths (legacy) */
    char resolved_path[4096];
    if (strncmp(path, "store/", 6) == 0) {
        snprintf(resolved_path, sizeof(resolved_path), "%s/%s", ctx->session_dir, path);
        path = resolved_path;
    }

    /* use fork/execvp to avoid shell injection */
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return make_error("fork failed"); }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("grep", "grep", "-rn",
               "--include=*.c", "--include=*.h", "--include=*.py",
               "--include=*.js", "--include=*.json", "--include=*.yaml",
               "--include=*.yml", "--include=*.md", "--include=*.txt",
               "--include=*.sh", "--include=*.go", "--include=*.rs",
               "--include=*.toml", "--include=*.xml", "--include=*.html",
               "--include=*.java", "--include=*.rb", "--include=*.php",
               "--include=*.cfg", "--include=*.ini", "--include=*.conf",
               "-m", "50",
               pattern, path, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    str_t out = str_new(4096);
    char buf[4096];
    ssize_t n;

    /* Read with timeout + output cap */
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
    if (!pattern_j || !pattern_j->valuestring)
        return make_error("missing 'pattern' parameter");

    const char *pattern = pattern_j->valuestring;
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    const char *search_path = path_j && path_j->valuestring ? path_j->valuestring : ".";

    /* Parse the glob pattern into components */
    char root[1024], name[1024], exact_path[1024];
    int recursive, exact;
    parse_glob_pattern(pattern, root, sizeof(root), name, sizeof(name),
                       &recursive, &exact, exact_path, sizeof(exact_path));

    /* Build the find command based on parsed components */
    char cmd[4096];

    if (exact) {
        /* Exact file path — check if it exists */
        snprintf(cmd, sizeof(cmd),
            "test -f '%s/%s' && echo '%s/%s' || true",
            search_path, exact_path, search_path, exact_path);
    } else if (strcmp(root, ".") == 0 && recursive) {
        /* Bare glob pattern — search recursively from search_path */
        snprintf(cmd, sizeof(cmd),
            "find '%s' -type f -name '%s' "
            "! -path '*/.git/*' "
            "! -path '*/node_modules/*' "
            "! -path '*/__pycache__/*' "
            "! -name '*.o' "
            "2>/dev/null | sort | head -200",
            search_path, name);
    } else if (recursive) {
        /* Directory prefix with recursive glob */
        snprintf(cmd, sizeof(cmd),
            "find '%s/%s' -type f -name '%s' "
            "! -path '*/.git/*' "
            "! -path '*/node_modules/*' "
            "! -path '*/__pycache__/*' "
            "! -name '*.o' "
            "2>/dev/null | sort | head -200",
            search_path, root, name);
    } else {
        /* Directory prefix with non-recursive glob */
        snprintf(cmd, sizeof(cmd),
            "find '%s/%s' -maxdepth 1 -type f -name '%s' "
            "! -path '*/.git/*' "
            "! -path '*/node_modules/*' "
            "! -path '*/__pycache__/*' "
            "! -name '*.o' "
            "2>/dev/null | sort | head -200",
            search_path, root, name);
    }

    str_t out = str_new(4096);
    FILE *fp = popen(cmd, "r");
    if (!fp) return make_error("failed to execute find");

    char line[4096];
    while (fgets(line, sizeof(line), fp))
        str_append_cstr(&out, line);
    pclose(fp);

    int matches = count_lines(out.data);
    char *hash = store_save(ctx->store, out.len > 0 ? out.data : "(no matches)");
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "pattern", pattern);
    if (strcmp(search_path, ".") != 0)
        cJSON_AddStringToObject(meta, "path", search_path);
    cJSON_AddNumberToObject(meta, "matches", matches);
    cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "glob_search", params, alias,
                   out.len, matches, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    return make_result(1, meta, ref_copy);
}

/* ── scratchpad section operations (GDN-2 inspired) ─── */

void scratchpad_init(scratchpad_t *sp) {
    memset(sp, 0, sizeof(*sp));
}

void scratchpad_free(scratchpad_t *sp) {
    for (int i = 0; i < sp->count; i++) {
        free(sp->sections[i].name);
        free(sp->sections[i].content);
    }
    sp->count = 0;
}

int scratchpad_find(scratchpad_t *sp, const char *name) {
    for (int i = 0; i < sp->count; i++) {
        if (strcmp(sp->sections[i].name, name) == 0)
            return i;
    }
    return -1;
}

int scratchpad_write(scratchpad_t *sp, const char *name, const char *content, int priority) {
    if (priority < 1) priority = 1;
    if (priority > 9) priority = 9;

    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Overwrite existing section */
        free(sp->sections[idx].content);
        sp->sections[idx].content = strdup(content);
        sp->sections[idx].priority = priority;
        return 0;
    }
    if (sp->count >= SCRATCHPAD_MAX_SECTIONS)
        return -1;  /* full */

    sp->sections[sp->count].name = strdup(name);
    sp->sections[sp->count].content = strdup(content);
    sp->sections[sp->count].priority = priority;
    sp->count++;
    return 0;
}

int scratchpad_append(scratchpad_t *sp, const char *name, const char *content, int priority) {
    int idx = scratchpad_find(sp, name);
    if (idx >= 0) {
        /* Append to existing */
        size_t old_len = strlen(sp->sections[idx].content);
        size_t add_len = strlen(content);
        char *combined = malloc(old_len + add_len + 2);  /* +newline+nul */
        if (!combined) return -1;
        memcpy(combined, sp->sections[idx].content, old_len);
        combined[old_len] = '\n';
        memcpy(combined + old_len + 1, content, add_len);
        combined[old_len + 1 + add_len] = '\0';
        free(sp->sections[idx].content);
        sp->sections[idx].content = combined;
        return 0;
    }
    /* Create new section */
    return scratchpad_write(sp, name, content, priority);
}

int scratchpad_clear(scratchpad_t *sp, const char *name) {
    int idx = scratchpad_find(sp, name);
    if (idx < 0) return -1;

    free(sp->sections[idx].name);
    free(sp->sections[idx].content);

    /* Shift remaining sections down */
    for (int i = idx; i < sp->count - 1; i++)
        sp->sections[i] = sp->sections[i + 1];
    sp->count--;
    return 0;
}

/* Compare sections by priority for qsort (lower priority number = first) */
static int section_cmp(const void *a, const void *b) {
    const scratchpad_section_t *sa = a;
    const scratchpad_section_t *sb = b;
    return sa->priority - sb->priority;
}

char *scratchpad_serialize(scratchpad_t *sp) {
    if (sp->count == 0) return NULL;

    /* Sort by priority */
    scratchpad_section_t sorted[SCRATCHPAD_MAX_SECTIONS];
    memcpy(sorted, sp->sections, sp->count * sizeof(scratchpad_section_t));
    qsort(sorted, sp->count, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(2048);
    for (int i = 0; i < sp->count; i++) {
        str_appendf(&out, "## %s\n%s\n\n", sorted[i].name, sorted[i].content);
    }
    return str_steal(&out);
}

char *scratchpad_serialize_budget(scratchpad_t *sp, size_t max_chars) {
    if (sp->count == 0) return NULL;

    /* Sort by priority (ascending = highest priority first) */
    scratchpad_section_t sorted[SCRATCHPAD_MAX_SECTIONS];
    memcpy(sorted, sp->sections, sp->count * sizeof(scratchpad_section_t));
    qsort(sorted, sp->count, sizeof(scratchpad_section_t), section_cmp);

    str_t out = str_new(max_chars > 4096 ? 4096 : max_chars);
    for (int i = 0; i < sp->count; i++) {
        /* Calculate how much space this section needs */
        size_t header_len = strlen(sorted[i].name) + 6;  /* "## " + name + "\n" + trailing "\n\n" */
        size_t content_len = strlen(sorted[i].content);
        size_t section_total = header_len + content_len;
        size_t remaining = (max_chars > out.len) ? (max_chars - out.len) : 0;

        if (remaining < header_len + 20) {
            /* Not enough room even for a header + minimal content — drop this and all lower-priority */
            break;
        }

        str_appendf(&out, "## %s\n", sorted[i].name);

        if (section_total <= remaining) {
            /* Fits fully */
            str_append_cstr(&out, sorted[i].content);
        } else {
            /* Truncate content to fit budget */
            size_t avail = remaining - header_len - 12;  /* room for "\n[truncated]" */
            str_append(&out, sorted[i].content, avail);
            str_append_cstr(&out, "\n[truncated]");
        }
        str_append_cstr(&out, "\n\n");
    }

    if (out.len == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

int scratchpad_save(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/scratchpad.md", session_dir);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < sp->count; i++) {
        fprintf(f, "<!-- priority:%d -->\n## %s\n%s\n\n",
                sp->sections[i].priority, sp->sections[i].name,
                sp->sections[i].content);
    }
    fclose(f);
    return 0;
}

int scratchpad_load(scratchpad_t *sp, const char *session_dir) {
    if (!session_dir) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/scratchpad.md", session_dir);

    char *buf = slurp_file(path, NULL);
    if (!buf) return -1;
    if (buf[0] == '\0') { free(buf); return 0; }

    /* Parse sections from the file format:
     * <!-- priority:N -->
     * ## section_name
     * content...
     */
    scratchpad_free(sp);  /* clear any existing sections */

    char *pos = buf;
    while (pos && *pos) {
        int priority = 5;  /* default */

        /* Look for priority comment */
        if (strncmp(pos, "<!-- priority:", 14) == 0) {
            priority = atoi(pos + 14);
            if (priority < 1) priority = 1;
            if (priority > 9) priority = 9;
            pos = strchr(pos, '\n');
            if (pos) pos++;
        }

        /* Look for ## header */
        if (!pos || strncmp(pos, "## ", 3) != 0) {
            /* Skip to next line */
            pos = strchr(pos, '\n');
            if (pos) pos++;
            continue;
        }

        /* Extract section name */
        const char *name_start = pos + 3;
        const char *name_end = strchr(name_start, '\n');
        if (!name_end) break;

        char name[256];
        size_t nlen = (size_t)(name_end - name_start);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, name_start, nlen);
        name[nlen] = '\0';

        /* Extract content until next "<!-- priority:" or "## " or EOF */
        const char *content_start = name_end + 1;
        const char *content_end = NULL;

        /* Scan forward for next section marker */
        const char *scan = content_start;
        while (*scan) {
            if (strncmp(scan, "<!-- priority:", 14) == 0 ||
                (strncmp(scan, "## ", 3) == 0 && (scan == buf || *(scan-1) == '\n'))) {
                content_end = scan;
                break;
            }
            scan++;
        }
        if (!content_end) content_end = buf + strlen(buf);

        /* Trim trailing whitespace from content */
        while (content_end > content_start &&
               (*(content_end-1) == '\n' || *(content_end-1) == ' '))
            content_end--;

        size_t clen = (size_t)(content_end - content_start);
        char *content = malloc(clen + 1);
        if (content) {
            memcpy(content, content_start, clen);
            content[clen] = '\0';
            scratchpad_write(sp, name, content, priority);
            free(content);
        }

        pos = (char *)content_end;
        /* Skip whitespace between sections */
        while (*pos == '\n' || *pos == ' ') pos++;
    }

    free(buf);
    return 0;
}

/* ── notes (section-based scratchpad) ────────────────── */

static void scratchpad_sync_legacy(tool_ctx_t *ctx) {
    /* Update legacy scratchpad string from sections */
    if (ctx->scratchpad) { free(ctx->scratchpad); ctx->scratchpad = NULL; }
    ctx->scratchpad = scratchpad_serialize(&ctx->scratch);
    /* Persist to disk */
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

        scratchpad_sync_legacy(ctx);

        char *full = scratchpad_serialize(&ctx->scratch);
        char *hash = store_save(ctx->store, full ? full : "");
        char *alias = tool_register_alias(ctx, hash ? hash : "");

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);
        cJSON_AddStringToObject(meta, "ref", alias);

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

        scratchpad_sync_legacy(ctx);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "write");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);

        /* Store content for full audit trail */
        char *w_hash = store_save(ctx->store, content);
        char *w_alias = tool_register_alias(ctx, w_hash ? w_hash : "");
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "notes", params, w_alias,
                       strlen(content), 0, NULL, NULL);
        free(w_alias); free(w_hash);
        return make_result(1, meta, NULL);

    } else if (strcmp(op, "append") == 0) {
        if (!section) return make_error("'append' requires 'section' parameter");
        if (!content) return make_error("'append' requires 'content' parameter");

        int rc = scratchpad_append(&ctx->scratch, section, content, priority);
        if (rc != 0) return make_error("scratchpad full");

        scratchpad_sync_legacy(ctx);

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

        scratchpad_sync_legacy(ctx);

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

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "done", params, alias,
                   strlen(result), 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    return make_result(1, meta, ref_copy);
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
    scratchpad_sync_legacy(ctx);
    scratchpad_save(&ctx->scratch, ctx->session_dir);

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

typedef struct {
    embed_multi_vec_t *new_emb;
    const char *new_emb_fname;
    const char *dir;
    float consolidation_threshold;
    char best_key[256];
    char best_path[4096];
    float best_sim;
} consolidation_scan_t;

static int consolidation_cb(const char *dirpath, const char *filename,
                             const char *fullpath, void *user_data) {
    consolidation_scan_t *s = (consolidation_scan_t *)user_data;
    (void)dirpath;

    size_t len = strlen(filename);
    /* Derive key from filename: strip .emb suffix */
    char emb_base[256];
    snprintf(emb_base, sizeof(emb_base), "%.*s", (int)(len - 4), filename);

    /* Skip self — the entry we just stored. */
    if (strcmp(emb_base, s->new_emb_fname) == 0) return 0;

    embed_multi_vec_t other_emb = embed_multi_vec_load(fullpath);
    if (!other_emb.data) return 0;

    /* Dimension check: skip stale embeddings from a different model.
     * Mismatched dims give 0.0 from cosine_sim anyway, but deleting
     * the stale file lets memory_embed_all() regenerate it. */
    if (other_emb.dim != s->new_emb->dim) {
        unlink(fullpath);
        embed_multi_vec_free(&other_emb);
        return 0;
    }

    /* MaxSim across all chunk pairs */
    float sim = embed_cosine_sim_multi_multi(s->new_emb, &other_emb);
    embed_multi_vec_free(&other_emb);

    if (sim > s->best_sim && sim > s->consolidation_threshold) {
        s->best_sim = sim;
        snprintf(s->best_key, sizeof(s->best_key), "%s", emb_base);
        /* Derive JSON path from emb path */
        snprintf(s->best_path, sizeof(s->best_path), "%s/%s.json",
                 s->dir, emb_base);
    }
    return 0;
}

static void memory_try_consolidate(tool_ctx_t *ctx, const char *new_key,
                                    const char *new_value) {
    if (!ctx->llm || !ctx->memory) return;
    if (!ctx->memory->embed || !ctx->memory->embed->available) return;

    /* Load the multi-vec embedding for the new entry (just stored by
     * memory_embed_entry, which already produced chunked embeddings). */
    char new_emb_fname[512];
    snprintf(new_emb_fname, sizeof(new_emb_fname), "%s", new_key);
    for (char *p = new_emb_fname; *p; p++) {
        if (*p == ':' || *p == '/') *p = '_';
    }
    char new_emb_path[4096];
    snprintf(new_emb_path, sizeof(new_emb_path), "%s/%s.emb",
             ctx->memory->dir, new_emb_fname);
    embed_multi_vec_t new_emb = embed_multi_vec_load(new_emb_path);
    if (!new_emb.data) return;

    /* Scan all memory .emb files for high similarity */
    consolidation_scan_t scan = {
        .new_emb = &new_emb,
        .new_emb_fname = new_emb_fname,
        .dir = ctx->memory->dir,
        .consolidation_threshold =
            ctx->cfg ? ctx->cfg->consolidation_threshold : 0.82f,
        .best_key = {0},
        .best_path = {0},
        .best_sim = 0.0f,
    };
    /* Consolidation threshold: cosine similarity above which two memories
     * are considered near-duplicates and merged. 0.82 is conservative —
     * only genuinely redundant entries trigger consolidation.
     * Configurable via config.toml [limits] consolidation_threshold.
     *
     * Research basis: IR literature places "semantically equivalent"
     * text at cosine similarity 0.80-0.90 depending on embedding model.
     * See also: MemForest [arXiv:2605.23986] for temporal dedup. */

    for_each_dir_entry(ctx->memory->dir, ".emb", consolidation_cb, &scan);
    embed_multi_vec_free(&new_emb);

    if (scan.best_key[0] == '\0') return;  /* no similar memory found */

    /* Load the similar memory's value */
    size_t buf_len = 0;
    char *buf = slurp_file(scan.best_path, &buf_len);
    if (!buf || buf_len == 0 || buf_len > 65536) { free(buf); return; }

    cJSON *old_entry = cJSON_Parse(buf);
    free(buf);
    if (!old_entry) return;

    cJSON *old_key_j = cJSON_GetObjectItem(old_entry, "key");
    cJSON *old_val_j = cJSON_GetObjectItem(old_entry, "value");
    if (!old_key_j || !old_val_j ||
        !old_key_j->valuestring || !old_val_j->valuestring) {
        cJSON_Delete(old_entry);
        return;
    }

    const char *old_key = old_key_j->valuestring;
    const char *old_value = old_val_j->valuestring;

    /* Don't consolidate pinned memories */
    cJSON *pinned_j = cJSON_GetObjectItem(old_entry, "pinned");
    if (pinned_j && cJSON_IsTrue(pinned_j)) {
        cJSON_Delete(old_entry);
        return;
    }

    /* Build consolidation prompt.
     * FIX B10: Include key lengths in allocation — the format string
     * interpolates new_key and old_key too, which could be up to 256
     * chars each. The previous 1024-byte slack was insufficient. */
    size_t prompt_sz = strlen(new_value) + strlen(old_value) +
                     strlen(new_key) + strlen(old_key) + 1024;
    char *prompt = malloc(prompt_sz);
    if (!prompt) { cJSON_Delete(old_entry); return; }
    snprintf(prompt, prompt_sz,
        "Consolidate these two related memory entries into ONE concise entry.\n"
        "Preserve all unique information. Remove redundancy. Keep the same style.\n"
        "Output ONLY the consolidated text, no preamble.\n\n"
        "--- Entry 1 (key: %s) ---\n%s\n\n"
        "--- Entry 2 (key: %s) ---\n%s\n\n"
        "Consolidated entry:",
        new_key, new_value, old_key, old_value);

    /* Call LLM for consolidation via provider when available (FIX #3) */
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add(chat, "system",
        "You are a memory consolidation assistant. Merge the two entries "
        "into one clear, concise entry preserving all unique information.");
    llm_chat_add(chat, "user", prompt);
    free(prompt);

    llm_stats_t stats = {0};
    char *consolidated = NULL;

    /* Temporarily override provider config for consolidation (low temp, short output) */
    int saved_max_tokens = ctx->provider->cfg.max_tokens;
    float saved_temp = ctx->provider->cfg.temperature;
    int saved_thinking = ctx->provider->cfg.enable_thinking;
    int saved_budget = ctx->provider->cfg.thinking_budget;
    ctx->provider->cfg.max_tokens = 2048;
    ctx->provider->cfg.temperature = 0.1f;
    ctx->provider->cfg.enable_thinking = 0;
    ctx->provider->cfg.thinking_budget = 0;
    consolidated = provider_complete(ctx->provider, chat, &stats);
    ctx->provider->cfg.max_tokens = saved_max_tokens;
    ctx->provider->cfg.temperature = saved_temp;
    ctx->provider->cfg.enable_thinking = saved_thinking;
    ctx->provider->cfg.thinking_budget = saved_budget;
    llm_chat_free(chat);

    if (!consolidated || strlen(consolidated) < 20) {
        /* Consolidation failed or too short — skip */
        free(consolidated);
        cJSON_Delete(old_entry);
        return;
    }

    {
        /* FIX B1: Preserve journal_ref provenance from the new entry.
         * Previously passed NULL, breaking the provenance chain for
         * consolidated memories — the dreaming system couldn't trace
         * them back to their originating session journal. */
        char new_fname[512];
        snprintf(new_fname, sizeof(new_fname), "%s", new_key);
        for (char *p = new_fname; *p; p++)
            if (*p == ':' || *p == '/') *p = '_';
        char new_json_path[4096];
        snprintf(new_json_path, sizeof(new_json_path), "%s/%s.json",
                 ctx->memory->dir, new_fname);

        const char *new_jref = NULL;
        cJSON *new_entry_json = NULL;
        {
            size_t nbuf_len = 0;
            char *nbuf = slurp_file(new_json_path, &nbuf_len);
            if (nbuf && nbuf_len > 0 && nbuf_len < 65536) {
                new_entry_json = cJSON_Parse(nbuf);
            }
            free(nbuf);
            if (new_entry_json) {
                cJSON *jr = cJSON_GetObjectItem(new_entry_json, "journal_ref");
                if (jr && jr->valuestring) new_jref = jr->valuestring;
            }
        }

        /* Store consolidated version under the new key */
        memory_store(ctx->memory, new_key, consolidated,
                     0, new_jref, NULL, 0);

        cJSON_Delete(new_entry_json);
    }

    /* Delete the old entry if it has a different key.
     * FIX B3: Use memory_delete() instead of raw unlink() — this properly
     * removes .json + .emb files and git commits the deletion. */
    if (strcmp(old_key, new_key) != 0) {
        memory_delete(ctx->memory, old_key);
    }

    free(consolidated);
    cJSON_Delete(old_entry);
}

static tool_result_t tool_memory_store(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    cJSON *val_j = cJSON_GetObjectItem(params, "value");
    if (!key_j || !key_j->valuestring || !val_j || !val_j->valuestring)
        return make_error("missing 'key' or 'value' parameter");

    const char *key = key_j->valuestring;
    const char *value = val_j->valuestring;

    int pinned = 0;
    cJSON *pin_j = cJSON_GetObjectItem(params, "pinned");
    if (pin_j && cJSON_IsTrue(pin_j)) pinned = 1;

    /* Build journal provenance reference: "session_dir/journal.jsonl:R<loop>" */
    char jref[4096];
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
    if (refs_j && refs_j->valuestring) {
        refs_copy = strdup(refs_j->valuestring);
        char *saveptr = NULL;
        char *tok = strtok_r(refs_copy, ",", &saveptr);
        while (tok && n_refs < 32) {
            while (*tok == ' ') tok++;  /* trim leading space */
            refs_arr[n_refs++] = tok;
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    int rc = memory_store(ctx->memory, key, value, pinned,
                          jref[0] ? jref : NULL,
                          n_refs > 0 ? refs_arr : NULL, n_refs);
    free(refs_copy);

    if (rc != 0) return make_error("failed to store memory");

    /* GDN-2 P2: Try to consolidate with similar existing memories.
     * FIX B2: Guard against recursive consolidation — memory_try_consolidate
     * calls memory_store() which could trigger another consolidation cycle. */
    if (!pinned && !ctx->consolidating) {
        ctx->consolidating = 1;
        memory_try_consolidate(ctx, key, value);
        ctx->consolidating = 0;
    }

    /* Store for audit */
    char *hash = store_save(ctx->store, value);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "key", key);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

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
    if (!query_j || !query_j->valuestring)
        return make_error("missing 'query' parameter");

    const char *query = query_j->valuestring;
    memory_results_t results = memory_recall(ctx->memory, query, 5);

    /* Build result string + track recalled keys for validation scoring */
    str_t out = str_new(1024);
    for (int i = 0; i < results.count; i++) {
        memory_entry_t *e = &results.entries[i];
        str_appendf(&out, "--- %s ---\n%s\n\n", e->key, e->value);
        tool_track_recalled_key(ctx, e->key);
    }

    char *hash = NULL;
    char *alias = NULL;
    if (out.len > 0) {
        hash = store_save(ctx->store, out.data);
        alias = tool_register_alias(ctx, hash ? hash : "");
    }

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "matches", results.count);
    /* Content stored to .store/ — model reads via file_read(ref) */
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_recall",
                   params, alias, out.len, results.count, NULL, NULL);

    memory_results_free(&results);
    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    str_free(&out);
    return make_result(1, meta, ref_copy);
}



/* ── memory_pin ─────────────────────────────────────────── */

static tool_result_t tool_memory_pin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring)
        return make_error("missing 'key' parameter");

    int rc = memory_pin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "pinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_pin",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── memory_unpin ───────────────────────────────────────── */

static tool_result_t tool_memory_unpin(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring)
        return make_error("missing 'key' parameter");

    int rc = memory_unpin(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "unpinned");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_unpin",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── memory_delete ──────────────────────────────────────── */

static tool_result_t tool_memory_delete(tool_ctx_t *ctx, cJSON *params) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring)
        return make_error("missing 'key' parameter");

    int rc = memory_delete(ctx->memory, key_j->valuestring);
    if (rc != 0) return make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "deleted");
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "memory_delete",
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return make_result(1, meta, NULL);
}

/* ── web_fetch ──────────────────────────────────────────── */

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
    if (!url_j || !url_j->valuestring)
        return make_error("missing 'url' parameter");

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
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

    /* Store to shared store */
    char *hash = store_save(ctx->store, body.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    /* Build metadata */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "url", url);
    cJSON_AddNumberToObject(meta, "http_code", http_code);
    cJSON_AddNumberToObject(meta, "chars", (double)body.len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(body.data));
    if (content_type) cJSON_AddStringToObject(meta, "content_type", content_type);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    /* Content stored to .store/ — model reads via file_read(ref) */

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_fetch",
                   params, alias, body.len, count_lines(body.data), NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
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
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    str_t body = str_new(256);
    curl_easy_setopt(curl, CURLOPT_URL, base_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    str_free(&body);
    return (res == CURLE_OK && http_code >= 200 && http_code < 500);
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

/* Start a SearXNG container using podman/docker.
 * Returns 0 on success, -1 on failure. */
static int searxng_start_container(int port) {
    /* Check if container already exists (maybe stopped) */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "podman rm -f nash-searxng >/dev/null 2>&1; "
             "podman run -d --name nash-searxng "
             "-p %d:8080 "
             "-e SEARXNG_BASE_URL=http://localhost:%d/ "
             "docker.io/searxng/searxng:latest "
             ">/dev/null 2>&1",
             port, port);
    int rc = system(cmd);
    if (rc != 0) {
        /* Try docker as fallback */
        snprintf(cmd, sizeof(cmd),
                 "docker rm -f nash-searxng >/dev/null 2>&1; "
                 "docker run -d --name nash-searxng "
                 "-p %d:8080 "
                 "-e SEARXNG_BASE_URL=http://localhost:%d/ "
                 "docker.io/searxng/searxng:latest "
                 ">/dev/null 2>&1",
                 port, port);
        rc = system(cmd);
    }
    if (rc != 0) return -1;

    searxng_auto_started = 1;

    /* Wait for SearXNG to become ready (up to 30 seconds) */
    char health_url[256];
    snprintf(health_url, sizeof(health_url), "http://localhost:%d/", port);
    for (int i = 0; i < 30; i++) {
        sleep(1);
        if (searxng_is_running(health_url)) return 0;
    }
    return -1;  /* timed out */
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
    fprintf(stderr, "[nash] SearXNG not running at %s — starting container on port %d...\n",
            base, port);
    free(base);

    if (searxng_start_container(port) != 0) {
        fprintf(stderr, "[nash] Failed to start SearXNG container\n");
        return -1;
    }
    fprintf(stderr, "[nash] SearXNG container started successfully\n");
    return 0;
}

/* Perform a search using SearXNG JSON API.
 * Returns a formatted results string (caller frees), or NULL on failure.
 * *out_count receives the number of results. */
static char *searxng_search(const char *searxng_url, const char *query,
                            int *out_count) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *encoded_q = curl_easy_escape(curl, query, 0);
    char url[2048];
    snprintf(url, sizeof(url), "%s?q=%s&format=json&categories=general",
             searxng_url, encoded_q);
    curl_free(encoded_q);

    str_t body = str_new(32768);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
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
            str_appendf(&results, "%d. %s\n   %s\n   %s\n\n", count, title, item_url, content);
        } else if (title[0]) {
            str_appendf(&results, "%d. %s\n   %s\n\n", count, title, item_url);
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

/* DuckDuckGo lite search — original implementation as fallback.
 * Returns formatted results string (caller frees) or NULL.
 * *out_count receives number of results. */
static char *ddg_search(const char *query, int *out_count) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *encoded_q = curl_easy_escape(curl, query, 0);
    char url[2048];
    snprintf(url, sizeof(url), "https://lite.duckduckgo.com/lite/?q=%s", encoded_q);
    curl_free(encoded_q);

    str_t body = str_new(32768);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        str_free(&body);
        return NULL;
    }

    str_t results = str_new(4096);
    int result_count = 0;

    const char *p = body.data;
    while (p && result_count < 10) {
        const char *href = strstr(p, "href=\"http");
        if (!href) break;
        href += 6;
        const char *end = strchr(href, '"');
        if (!end || end - href > 500) { p = href; continue; }

        char link[512];
        size_t link_len = (size_t)(end - href);
        if (link_len >= sizeof(link)) link_len = sizeof(link) - 1;
        memcpy(link, href, link_len);
        link[link_len] = '\0';

        if (strstr(link, "duckduckgo.com") || strstr(link, "duck.co")) {
            p = end;
            continue;
        }

        const char *tag_end = strchr(end, '>');
        char title[256] = "";
        if (tag_end) {
            tag_end++;
            const char *title_end = strchr(tag_end, '<');
            if (title_end && title_end - tag_end > 0 && title_end - tag_end < 250) {
                size_t tlen = (size_t)(title_end - tag_end);
                memcpy(title, tag_end, tlen);
                title[tlen] = '\0';
            }
        }

        result_count++;
        if (title[0]) {
            str_appendf(&results, "%d. %s\n   %s\n\n", result_count, title, link);
        } else {
            str_appendf(&results, "%d. %s\n\n", result_count, link);
        }
        p = end;
    }

    str_free(&body);
    *out_count = result_count;

    if (result_count == 0) {
        str_free(&results);
        return NULL;
    }

    return str_steal(&results);
}

static tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring)
        return make_error("missing 'query' parameter");

    const char *query = query_j->valuestring;
    const char *engine = ctx->cfg->search_engine;
    char *results_text = NULL;
    int result_count = 0;

    if (engine && strcmp(engine, "searxng") == 0) {
        /* SearXNG mode — ensure server is running, then search */
        if (ensure_searxng(ctx->cfg->searxng_url) != 0) {
            return make_error("SearXNG not available and could not be auto-started. "
                              "Install podman/docker or configure a running SearXNG instance "
                              "in ~/.nash/config.toml [search] section.");
        }
        results_text = searxng_search(ctx->cfg->searxng_url, query, &result_count);
    } else {
        /* DuckDuckGo mode — try DDG first, fall back to SearXNG */
        results_text = ddg_search(query, &result_count);

        if (!results_text) {
            /* DDG failed — try SearXNG as fallback */
            if (ensure_searxng(ctx->cfg->searxng_url) == 0) {
                results_text = searxng_search(ctx->cfg->searxng_url, query, &result_count);
            }
        }
    }

    if (!results_text || result_count == 0) {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "no results found for query: %s", query);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                       params, NULL, 0, 0, errmsg, NULL);
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

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                   params, alias, strlen(results_text), result_count, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(results_text);
    return make_result(1, meta, ref_copy);
}

/* Tear down auto-started SearXNG container. Called on nash exit. */
void web_search_cleanup(void) {
    if (!searxng_auto_started) return;
    fprintf(stderr, "[nash] Stopping auto-started SearXNG container...\n");
    int rc = system("podman stop nash-searxng >/dev/null 2>&1 && "
                    "podman rm nash-searxng >/dev/null 2>&1");
    if (rc != 0) {
        /* Try docker as fallback */
        system("docker stop nash-searxng >/dev/null 2>&1 && "
               "docker rm nash-searxng >/dev/null 2>&1");
    }
    searxng_auto_started = 0;
}


/* Tool dispatch table — maps tool names to handler functions.
 * Adding a new tool requires: (1) add handler function above,
 * (2) add entry here, (3) add to TOOL_REGISTRY in tools_registry.h.
 * The dispatch table is separate from TOOL_REGISTRY because handler
 * functions are static to this file and can't be in a shared header. */
typedef tool_result_t (*tool_handler_fn)(tool_ctx_t *, cJSON *);

static const struct {
    const char *name;
    tool_handler_fn handler;
} TOOL_DISPATCH[] = {
    {"shell_exec",    tool_shell_exec},
    {"file_read",     tool_file_read},
    {"file_write",    tool_file_write},
    {"file_edit",     tool_file_edit},
    {"grep_search",   tool_grep_search},
    {"glob_search",   tool_glob_search},
    {"web_fetch",     tool_web_fetch},
    {"web_search",    tool_web_search},
    {"notes",         tool_notes},
    {"done",          tool_done},
    {"plan",          tool_plan},
    {"user_ask",      tool_plan},  /* stub — actual logic is in react.c (special-cased before tool_execute) */
    {"memory_store",  tool_memory_store},
    {"memory_recall", tool_memory_recall},
    {"memory_pin",    tool_memory_pin},
    {"memory_unpin",  tool_memory_unpin},
    {"memory_delete", tool_memory_delete},
    {NULL, NULL}  /* sentinel */
};

tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    /* Dispatch via table lookup */
    for (int i = 0; TOOL_DISPATCH[i].name; i++) {
        if (strcmp(action, TOOL_DISPATCH[i].name) == 0)
            return TOOL_DISPATCH[i].handler(ctx, params);
    }

    /* Unknown tool — build available tools list dynamically from the
     * dispatch table (no hardcoded list to keep in sync). react.c handles
     * this by injecting a corrective message and letting the model retry. */
    char msg[1024];
    int pos = snprintf(msg, sizeof(msg), "unknown tool: '%.100s'. Available: ", action);
    for (int i = 0; TOOL_DISPATCH[i].name && pos < (int)sizeof(msg) - 32; i++) {
        if (i > 0) pos += snprintf(msg + pos, sizeof(msg) - pos, ", ");
        pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", TOOL_DISPATCH[i].name);
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

const char *tools_system_prompt(void) {
    static char buf[4096];

    /* UTC timestamp */
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M UTC", utc);

    /* Current working directory */
    char cwdbuf[1024];
    if (!getcwd(cwdbuf, sizeof(cwdbuf)))
        snprintf(cwdbuf, sizeof(cwdbuf), "(unknown)");

    snprintf(buf, sizeof(buf),
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

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

    int lines = count_lines(content);
    char *hash = store_save(ctx->store, content);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddNumberToObject(meta, "lines", lines);
    cJSON_AddNumberToObject(meta, "chars", (double)len);
    cJSON_AddStringToObject(meta, "ref", alias);

    /* file_read MUST return content — that's its purpose */
    if (len <= 50000) {
        cJSON_AddStringToObject(meta, "content", content);
    } else {
        char *trunc = malloc(50001);
        if (trunc) {
            utf8_truncate(trunc, content, 50000);
            cJSON_AddStringToObject(meta, "content", trunc);
            cJSON_AddBoolToObject(meta, "truncated", 1);
            free(trunc);
        }
    }

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_read", params, alias,
                   len, lines, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(content);
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

    FILE *f = fopen(path, "w");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", path, strerror(errno));
        return make_error(msg);
    }

    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);

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

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "ok");
    cJSON_AddStringToObject(meta, "path", path);
    cJSON_AddStringToObject(meta, "pre_ref", pre_alias);
    cJSON_AddStringToObject(meta, "ref", post_alias);

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "file_edit", params, post_alias,
                   result_len, count_lines(result), NULL, NULL);

    free(content);
    free(result);
    free(pre_alias);
    free(pre_hash);
    free(post_hash);
    return make_result(1, meta, post_alias);
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

/* ── dispatcher ──────────────────────────────────────── */

/* ── memory_store ──────────────────────────────────────── */

/* ── LLM-based memory consolidation (GDN-2 P2) ──────── */
/* After storing a memory, check for semantically similar existing memories.
 * If found, concatenate both and call the LLM to produce a consolidated
 * version. This implements the "subtract-before-write" principle from GDN-2:
 * related content is merged rather than duplicated. */
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
    DIR *dir = opendir(ctx->memory->dir);
    if (!dir) { embed_multi_vec_free(&new_emb); return; }

    char best_key[256] = "";
    char best_path[4096] = "";
    float best_sim = 0.0f;
    /* Consolidation threshold: cosine similarity above which two memories
     * are considered near-duplicates and merged. 0.82 is conservative —
     * only genuinely redundant entries trigger consolidation.
     * Configurable via config.toml [limits] consolidation_threshold.
     *
     * Research basis: IR literature places "semantically equivalent"
     * text at cosine similarity 0.80-0.90 depending on embedding model.
     * See also: MemForest [arXiv:2605.23986] for temporal dedup. */
    const float consolidation_threshold =
        ctx->cfg ? ctx->cfg->consolidation_threshold : 0.82f;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 4 || strcmp(de->d_name + len - 4, ".emb") != 0) continue;

        /* Derive key from filename: strip .emb suffix */
        char emb_base[256];
        snprintf(emb_base, sizeof(emb_base), "%.*s", (int)(len - 4), de->d_name);

        /* Skip self — the entry we just stored. */
        if (strcmp(emb_base, new_emb_fname) == 0) continue;

        char emb_path[4096];
        snprintf(emb_path, sizeof(emb_path), "%s/%s", ctx->memory->dir, de->d_name);
        embed_multi_vec_t other_emb = embed_multi_vec_load(emb_path);
        if (!other_emb.data) continue;

        /* Dimension check: skip stale embeddings from a different model.
         * Mismatched dims give 0.0 from cosine_sim anyway, but deleting
         * the stale file lets memory_embed_all() regenerate it. */
        if (other_emb.dim != new_emb.dim) {
            unlink(emb_path);
            embed_multi_vec_free(&other_emb);
            continue;
        }

        /* MaxSim across all chunk pairs */
        float sim = embed_cosine_sim_multi_multi(&new_emb, &other_emb);
        embed_multi_vec_free(&other_emb);

        if (sim > best_sim && sim > consolidation_threshold) {
            best_sim = sim;
            snprintf(best_key, sizeof(best_key), "%s", emb_base);
            /* Derive JSON path from emb path */
            snprintf(best_path, sizeof(best_path), "%s/%s.json",
                     ctx->memory->dir, emb_base);
        }
    }
    closedir(dir);
    embed_multi_vec_free(&new_emb);

    if (best_key[0] == '\0') return;  /* no similar memory found */

    /* Load the similar memory's value */
    size_t buf_len = 0;
    char *buf = slurp_file(best_path, &buf_len);
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

    if (ctx->provider) {
        /* Use provider abstraction (works with Vertex, Anthropic, OpenAI, local) */
        consolidated = provider_complete(ctx->provider, chat, &stats);
    } else if (ctx->llm) {
        /* Fallback to direct llm_complete for local-only setups */
        llm_config_t consolidation_cfg = *ctx->llm;
        consolidation_cfg.max_tokens = 2048;
        consolidation_cfg.temperature = 0.1f;
        consolidation_cfg.enable_thinking = 0;
        consolidation_cfg.thinking_budget = 0;
        consolidated = llm_complete(&consolidation_cfg, chat, &stats);
    }
    llm_chat_free(chat);

    if (!consolidated || strlen(consolidated) < 20) {
        /* Consolidation failed or too short — skip */
        free(consolidated);
        cJSON_Delete(old_entry);
        return;
    }

    /* FIX B13: Merge tags from both entries before storing consolidated version.
     * Previously, consolidation called memory_store with NULL tags, losing
     * all tags from both the old and new entries. */
    {
        /* Collect tags from old entry */
        cJSON *old_tags = cJSON_GetObjectItem(old_entry, "tags");
        int n_old_tags = old_tags ? cJSON_GetArraySize(old_tags) : 0;

        /* Load new entry's tags from its stored JSON file */
        char new_fname[512];
        snprintf(new_fname, sizeof(new_fname), "%s", new_key);
        for (char *p = new_fname; *p; p++)
            if (*p == ':' || *p == '/') *p = '_';
        char new_json_path[4096];
        snprintf(new_json_path, sizeof(new_json_path), "%s/%s.json",
                 ctx->memory->dir, new_fname);

        cJSON *new_entry_json = NULL;
        cJSON *new_tags = NULL;
        int n_new_tags = 0;
        {
            size_t nbuf_len = 0;
            char *nbuf = slurp_file(new_json_path, &nbuf_len);
            if (nbuf && nbuf_len > 0 && nbuf_len < 65536) {
                new_entry_json = cJSON_Parse(nbuf);
            }
            free(nbuf);
            if (new_entry_json) {
                new_tags = cJSON_GetObjectItem(new_entry_json, "tags");
                n_new_tags = new_tags ? cJSON_GetArraySize(new_tags) : 0;
            }
        }

        /* Merge: collect unique tags from both entries */
        int max_merged = n_old_tags + n_new_tags;
        const char **merged_tags = NULL;
        int n_merged = 0;
        if (max_merged > 0) {
            merged_tags = malloc(sizeof(char *) * (size_t)max_merged);
            if (merged_tags) {
                /* Add new entry's tags first */
                for (int i = 0; i < n_new_tags; i++) {
                    cJSON *t = cJSON_GetArrayItem(new_tags, i);
                    if (t && t->valuestring)
                        merged_tags[n_merged++] = t->valuestring;
                }
                /* Add old entry's tags (skip duplicates) */
                for (int i = 0; i < n_old_tags; i++) {
                    cJSON *t = cJSON_GetArrayItem(old_tags, i);
                    if (!t || !t->valuestring) continue;
                    int dup = 0;
                    for (int j = 0; j < n_merged; j++) {
                        if (strcmp(merged_tags[j], t->valuestring) == 0) {
                            dup = 1; break;
                        }
                    }
                    if (!dup) merged_tags[n_merged++] = t->valuestring;
                }
            }
        }

        /* FIX B1: Preserve journal_ref provenance from the new entry.
         * Previously passed NULL, breaking the provenance chain for
         * consolidated memories — the dreaming system couldn't trace
         * them back to their originating session journal. */
        const char *new_jref = NULL;
        if (new_entry_json) {
            cJSON *jr = cJSON_GetObjectItem(new_entry_json, "journal_ref");
            if (jr && jr->valuestring) new_jref = jr->valuestring;
        }

        /* Store consolidated version under the new key with merged tags */
        memory_store(ctx->memory, new_key, consolidated,
                     merged_tags, n_merged, 0, new_jref, NULL, 0);

        free(merged_tags);
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

    /* Parse tags (comma-separated string) */
    const char *tags_arr[32];
    int n_tags = 0;
    cJSON *tags_j = cJSON_GetObjectItem(params, "tags");
    char *tags_copy = NULL;
    if (tags_j && tags_j->valuestring) {
        tags_copy = strdup(tags_j->valuestring);
        char *saveptr = NULL;
        char *tok = strtok_r(tags_copy, ",", &saveptr);
        while (tok && n_tags < 32) {
            while (*tok == ' ') tok++;  /* trim leading space */
            tags_arr[n_tags++] = tok;
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

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

    int rc = memory_store(ctx->memory, key, value, tags_arr, n_tags, pinned,
                          jref[0] ? jref : NULL,
                          n_refs > 0 ? refs_arr : NULL, n_refs);
    free(tags_copy);
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

static tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring)
        return make_error("missing 'query' parameter");

    const char *query = query_j->valuestring;

    /* Use DuckDuckGo lite HTML search */
    /* URL-encode the query */
    CURL *curl = curl_easy_init();
    if (!curl) return make_error("curl_easy_init failed");

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "search failed: %s", curl_easy_strerror(res));
        str_free(&body);
        return make_error(msg);
    }

    /* Parse DuckDuckGo lite HTML results — extract links and snippets */
    str_t results = str_new(4096);
    int result_count = 0;

    /* Simple HTML parsing: find result links in <a class="result-link"> or <a> tags with http */
    const char *p = body.data;
    while (p && result_count < 10) {
        /* Look for result links — DDG lite uses <a rel="nofollow" href="..."> */
        const char *href = strstr(p, "href=\"http");
        if (!href) break;
        href += 6;  /* skip href=" */
        const char *end = strchr(href, '"');
        if (!end || end - href > 500) { p = href; continue; }

        /* Extract URL */
        char link[512];
        size_t link_len = (size_t)(end - href);
        if (link_len >= sizeof(link)) link_len = sizeof(link) - 1;
        memcpy(link, href, link_len);
        link[link_len] = '\0';

        /* Skip DDG internal links */
        if (strstr(link, "duckduckgo.com") || strstr(link, "duck.co")) {
            p = end;
            continue;
        }

        /* Try to find a title — look for text between > and < after the <a> tag */
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

        if (title[0]) {
            str_appendf(&results, "%d. %s\n   %s\n\n", result_count + 1, title, link);
        } else {
            str_appendf(&results, "%d. %s\n\n", result_count + 1, link);
        }
        result_count++;
        p = end;
    }

    if (result_count == 0) {
        /* No results = explicit failure — don't store useless content,
         * record as error in journal so reflection can see it */
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "no results found for query: %s", query);

        journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                       params, NULL, 0, 0, errmsg, NULL);

        str_free(&results);
        str_free(&body);
        return make_error(errmsg);
    }

    /* Store results */
    char *hash = store_save(ctx->store, results.data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "query", query);
    cJSON_AddNumberToObject(meta, "results", result_count);
    cJSON_AddNumberToObject(meta, "chars", (double)results.len);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    /* Content stored to .store/ — model reads via file_read(ref) */

    journal_append(ctx->journal, ctx->react_loop, ctx->step, "web_search",
                   params, alias, results.len, result_count, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    str_free(&results);
    str_free(&body);
    return make_result(1, meta, ref_copy);
}


tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    if (strcmp(action, "shell_exec")  == 0) return tool_shell_exec(ctx, params);
    if (strcmp(action, "file_read")   == 0) return tool_file_read(ctx, params);
    if (strcmp(action, "file_write")  == 0) return tool_file_write(ctx, params);
    if (strcmp(action, "file_edit")   == 0) return tool_file_edit(ctx, params);
    if (strcmp(action, "grep_search") == 0) return tool_grep_search(ctx, params);
    if (strcmp(action, "web_fetch")   == 0) return tool_web_fetch(ctx, params);
    if (strcmp(action, "web_search")  == 0) return tool_web_search(ctx, params);
    if (strcmp(action, "notes")       == 0) return tool_notes(ctx, params);
    if (strcmp(action, "done")        == 0) return tool_done(ctx, params);
    if (strcmp(action, "memory_store") == 0) return tool_memory_store(ctx, params);
    if (strcmp(action, "memory_recall")== 0) return tool_memory_recall(ctx, params);
    if (strcmp(action, "memory_pin")  == 0) return tool_memory_pin(ctx, params);
    if (strcmp(action, "memory_unpin")== 0) return tool_memory_unpin(ctx, params);
    if (strcmp(action, "memory_delete")==0) return tool_memory_delete(ctx, params);

    /* Unknown tool — return error with available tool list so the model
     * can self-correct. react.c handles this by injecting a corrective
     * message and letting the model retry (no fuzzy matching hacks). */
    char msg[512];
    snprintf(msg, sizeof(msg),
        "unknown tool: '%.100s'. Available tools: shell_exec, file_read, "
        "file_write, file_edit, grep_search, web_fetch, web_search, "
        "notes, done, memory_store, memory_recall, memory_pin, "
        "memory_unpin, memory_delete", action);
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
        "- Record key findings in notes — they survive context eviction.\n"
        "- Call done with the final answer when finished.\n",
        timebuf, cwdbuf);

    return buf;
}

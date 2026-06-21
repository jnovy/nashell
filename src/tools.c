#include "tools_internal.h"
#include "memory.h"
#include "tui.h"
#include "scratchpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

/* ── helpers ─────────────────────────────────────────── */

/* Inject the current step's thought into a params cJSON before journal_append.
 * The thought is stored in ctx->thought by react.c before calling tool_execute.
 * Skip whitespace-only thoughts (e.g. "\n\n" emitted before tool calls). */
void tools_inject_thought(tool_ctx_t *ctx, cJSON *params) {
    if (!ctx->thought || !ctx->thought[0] || !params ||
        cJSON_GetObjectItem(params, "thought"))
        return;
    /* Skip whitespace-only thoughts */
    if (is_whitespace_only(ctx->thought)) return;
    cJSON_AddStringToObject(params, "thought", ctx->thought);
}

tool_result_t tools_make_result(int success, cJSON *meta, char *ref) {
    return (tool_result_t){ .meta = meta, .store_ref = ref, .success = success };
}

tool_result_t tools_make_error(const char *msg) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "error", msg);
    return tools_make_result(0, m, NULL);
}

/* Check tool filter whitelist/blacklist. Returns 1 if allowed, 0 if blocked. */
static int tool_filter_allows(const tool_filter_t *f, const char *name) {
    if (f->allowed) {
        int found = 0;
        for (int i = 0; i < f->n_allowed; i++)
            if (strcmp(name, f->allowed[i]) == 0) { found = 1; break; }
        if (!found) return 0;
    }
    if (f->blocked) {
        for (int i = 0; i < f->n_blocked; i++)
            if (strcmp(name, f->blocked[i]) == 0)
                return 0;
    }
    return 1;
}

/* Resolve a tool path: step alias → store path, store/ prefix → session-relative.
 * Writes resolved path into resolved_buf (size NASH_PATH_MAX).
 * Returns the path to use (may be the original, resolved alias, or resolved_buf).
 * *resolved_out is set to the alias resolution (caller must free if non-NULL). */
const char *tools_resolve_path(tool_ctx_t *ctx, const char *path,
                               char *resolved_buf, char **resolved_out) {
    *resolved_out = tool_resolve_alias(ctx, path);
    if (*resolved_out) path = *resolved_out;

    if (strncmp(path, "store/", 6) == 0 && ctx->session_dir) {
        snprintf(resolved_buf, NASH_PATH_MAX, "%s/%s", ctx->session_dir, path);
        return resolved_buf;
    }
    return path;
}

/* Unified memory key operation for pin/unpin/delete.
 * ws_fn/mem_fn are the workspace/memory layer functions to call. */
tool_result_t tools_memory_key_op(tool_ctx_t *ctx, cJSON *params,
                                  const char *tool_name, const char *err_prefix,
                                  const char *status_str, const char *harness_note,
                                  ws_key_fn ws_fn, mem_key_fn mem_fn) {
    cJSON *key_j = cJSON_GetObjectItem(params, "key");
    if (!key_j || !key_j->valuestring || !key_j->valuestring[0]) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s requires a non-empty 'key' string.", err_prefix);
        return tools_make_error(msg);
    }

    int rc = ctx->ws ? ws_fn(ctx->ws, key_j->valuestring)
                      : mem_fn(ctx->memory, key_j->valuestring);
    if (rc != 0) return tools_make_error("memory entry not found");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", status_str);
    cJSON_AddStringToObject(meta, "key", key_j->valuestring);
    if (harness_note)
        cJSON_AddStringToObject(meta, "harness_note", harness_note);

    {
        char *_p = cJSON_PrintUnformatted(params);
        char *_h = store_save(ctx->store, _p ? _p : "{}");
        char *_a = tool_register_alias(ctx, _h ? _h : "");
        tools_inject_thought(ctx, params);
        journal_append(ctx->journal, ctx->react_loop, ctx->step, tool_name,
                       params, _a, _p ? strlen(_p) : 0, 0, NULL, NULL);
        free(_a); free(_h); free(_p);
    }

    return tools_make_result(1, meta, NULL);
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

/* FIX #2: Reverse lookup — find alias for a given store hash.
 * Used to retrieve an already-registered alias without creating a new one.
 * Linear scan over all buckets (acceptable — alias maps are typically small). */
const char *alias_map_reverse_lookup(alias_map_t *map, const char *hash) {
    if (!map || !hash) return NULL;
    for (int b = 0; b < map->capacity; b++) {
        alias_node_t *node = map->buckets[b];
        while (node) {
            if (node->hash && strcmp(node->hash, hash) == 0)
                return node->alias;
            node = node->next;
        }
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
        /* FIX: Isolate child from parent's terminal session.
         * Without setsid(), long-running grandchildren (e.g. dnf spawned by
         * a shell pipeline) inherit our process group and controlling terminal.
         * If the parent (nash) exits or the grandchild becomes orphaned, it
         * remains in the foreground PGRP with access to our stdin, which can
         * steal keystrokes or block the terminal.
         * Also redirect stdin from /dev/null — child commands don't need it
         * and leaving it connected to the terminal lets orphaned grandchildren
         * interfere with the parent's TUI input. */
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
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
        return tools_make_error("shell_exec requires a non-empty 'command' string. "
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

    tools_inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "shell_exec", params, alias,
                   out.len, count_lines(out.data), exit_code == 0 ? NULL : "non-zero exit", NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    return tools_make_result(exit_code == 0, meta, ref_copy);
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

    tools_inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "done", params, alias,
                   strlen(result), 0, NULL, NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    free(hash);
    return tools_make_result(1, meta, ref_copy);
}


/* ── user_ask stub ────────────────────────────────────── */
/* user_ask is handled by react.c before reaching tool_execute().
 * This stub exists only so the dispatch table has an entry.
 * If reached, it means react.c's special-casing was bypassed. */
static tool_result_t tool_user_ask_stub(tool_ctx_t *ctx, cJSON *params) {
    (void)ctx; (void)params;
    return tools_make_error("user_ask must be handled by react loop, not tool dispatch");
}

/* ── plan ──────────────────────────────────────────────── */

static tool_result_t tool_plan(tool_ctx_t *ctx, cJSON *params) {
    const char *result = NULL;
    cJSON *r = cJSON_GetObjectItem(params, "result");
    if (r && r->valuestring) result = r->valuestring;
    if (!result || !result[0])
        return tools_make_error("missing 'result' parameter with the plan text");

    /* Write plan to scratchpad as a high-priority section.
     * The plan survives context eviction and is visible to the model
     * throughout the react loop via the scratchpad injection. */
    scratchpad_write(&ctx->scratch, "plan", result, 1);  /* priority 1 = high */
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

    tools_inject_thought(ctx, params);
    journal_append(ctx->journal, ctx->react_loop, ctx->step, "plan",
                   params, alias, strlen(result), steps, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    return tools_make_result(1, meta, ref_copy);
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

    /* FIX #6: Deduplicate deferred queue by key — keep only the LATEST value
     * for each key. When the same key is stored multiple times in one loop,
     * earlier values are stale and would cause incorrect merge decisions. */
    for (int i = 0; i < ctx->n_deferred_consol; i++) {
        if (!ctx->deferred_consol[i].key) continue;
        for (int j = i + 1; j < ctx->n_deferred_consol; j++) {
            if (ctx->deferred_consol[j].key &&
                strcmp(ctx->deferred_consol[i].key, ctx->deferred_consol[j].key) == 0) {
                /* Later entry has same key — discard earlier (stale) value */
                free(ctx->deferred_consol[i].key);
                free(ctx->deferred_consol[i].value);
                ctx->deferred_consol[i].key = NULL;
                ctx->deferred_consol[i].value = NULL;
                break;
            }
        }
    }

    /* FIX CRIT#1: Use atomic CAS to prevent TOCTOU race — two threads
     * could both see consolidating==0 and both proceed without CAS. */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&ctx->memory->consolidating, &expected, 1)) {
        /* Another thread is already consolidating — skip */
        tool_free_deferred_consolidations(ctx);
        return;
    }
    for (int i = 0; i < ctx->n_deferred_consol; i++) {
        if (ctx->deferred_consol[i].key && ctx->deferred_consol[i].value) {
            char *dk = tools_memory_try_consolidate(ctx, ctx->deferred_consol[i].key,
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
    atomic_store(&ctx->memory->consolidating, 0);

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

/* SearXNG cleanup moved to searxng.c */


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
    tool_memory_search,    /* memory_search */
    tool_memory_pin,       /* memory_pin    */
    tool_memory_unpin,     /* memory_unpin  */
    tool_done,             /* done          */
    tool_plan,             /* plan          */
    tool_notes,            /* notes         */
    tool_user_ask_stub,    /* user_ask      */
    tool_memory_delete,    /* memory_delete */
    tool_memory_list,      /* memory_list   */
    tool_image_analyze,    /* image_analyze */
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
    if (!tool_filter_allows(&ctx->tool_filter, action))
        return tools_make_error("tool not available in this context");

    /* Dispatch via unified registry lookup (Fix #11).
     * TOOL_REGISTRY[i].name provides the name, TOOL_HANDLERS[i] the handler.
     * Required-param validation is derived from each tool's params_json schema
     * ("required":[...] array) — no hardcoded table needed. */
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        if (strcmp(action, TOOL_REGISTRY[i].name) == 0) {
            /* Validate required params from the registry's JSON schema */
            if (TOOL_REGISTRY[i].params_json) {
                cJSON *schema = cJSON_Parse(TOOL_REGISTRY[i].params_json);
                if (schema) {
                    cJSON *req = cJSON_GetObjectItem(schema, "required");
                    if (req && cJSON_IsArray(req)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, req) {
                            if (!cJSON_IsString(item)) continue;
                            cJSON *val = cJSON_GetObjectItem(params,
                                                             item->valuestring);
                            if (!val || (cJSON_IsString(val) &&
                                    (!val->valuestring ||
                                     !val->valuestring[0]))) {
                                char err[256];
                                snprintf(err, sizeof(err),
                                    "Reminder: %s requires \"%s\" in params. "
                                    "Re-call with the required parameter.",
                                    action, item->valuestring);
                                cJSON_Delete(schema);
                                return tools_make_error(err);
                            }
                        }
                    }
                    cJSON_Delete(schema);
                }
            }
            return TOOL_HANDLERS[i](ctx, params);
        }
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
            /* Re-check tool filter for the recovered name */
            if (!tool_filter_allows(&ctx->tool_filter, best_name))
                return tools_make_error("tool not available in this context");
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
    return tools_make_error(msg);
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
        "- Call done with the final answer when finished.\n"
        "- The user only sees [done] text. Notes/scratchpad are invisible to them. "
        "Never reference notes content — include all data directly in done result.\n"
        "\n"
        "Clarification seeking:\n"
        "- Before selecting your first action, assess request_uncertainty on a 0-1 "
        "scale: 0 = fully specified task, 0.5 = missing parameters the user likely "
        "has a preference about, 1 = critically ambiguous.\n"
        "- If request_uncertainty >= 0.5, call user_ask BEFORE proceeding with any "
        "other tool. Asking early is far better than discovering ambiguity mid-task.\n"
        "- Do NOT guess when the user's intent is unclear — ask.\n",
        timebuf, cwdbuf);

    return buf;
}

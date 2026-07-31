#include "tools_internal.h"
#include "tool_plugin.h"
#include "subprocess.h"
#include "memory.h"
#include "tui.h"
#include "scratchpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

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

/* tools_make_result() and tools_make_error() are static inline in
 * tool_plugin.h -- available to all tool files and external plugins. */

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
        tool_journal(ctx, tool_name,
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

/* Scan session_dir for existing R<loop>S<N> symlinks and return the
 * highest N found, or -1 if none exist.  Used by react_run() to set
 * next_seq past any refs created by earlier queries in the same session,
 * preventing alias collisions and stale symlink shadowing. */
int alias_scan_max_seq(const char *session_dir, int react_loop) {
    if (!session_dir) return -1;
    DIR *d = opendir(session_dir);
    if (!d) return -1;

    char prefix[32];
    int prefix_len = snprintf(prefix, sizeof(prefix), "R%dS", react_loop);
    int max_seq = -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, (size_t)prefix_len) != 0)
            continue;
        /* Parse the sequence number after the prefix */
        const char *seq_str = ent->d_name + prefix_len;
        char *endp;
        long seq = strtol(seq_str, &endp, 10);
        if (endp != seq_str && *endp == '\0' && seq >= 0) {
            if ((int)seq > max_seq)
                max_seq = (int)seq;
        }
    }
    closedir(d);
    return max_seq;
}

char *tool_register_alias(tool_ctx_t *ctx, const char *hash) {
    if (!ctx || !ctx->aliases) {
        nash_log("[tools] CRITICAL: tool_register_alias called with NULL ctx/aliases");
        return strdup("R?S?");
    }

    char alias_buf[32];
    snprintf(alias_buf, sizeof(alias_buf), "R%dS%d", ctx->react_loop, ctx->aliases->next_seq);
    ctx->aliases->next_seq++;

    alias_map_insert(ctx->aliases, alias_buf, hash ? hash : "");

    /* Create symlink in session directory: R1S0 → <store_dir>/hash
     * Use absolute path to the store — relative "../../store" breaks for
     * nested workspaces (e.g. workspaces/rh/container-tools/sessions/SID). */
    if (ctx->session_dir && ctx->store && ctx->store->dir && hash && hash[0]) {
        char link_path[NASH_PATH_MAX];
        char target[NASH_PATH_MAX];
        snprintf(link_path, sizeof(link_path), "%s/%s", ctx->session_dir, alias_buf);
        snprintf(target, sizeof(target), "%s/%s", ctx->store->dir, hash);
        /* Force-overwrite: remove stale symlink from previous query in
         * same session before creating the new one.  Without this, a
         * second react_run() in the same session reuses R0S0, R0S1, ...
         * but the old symlinks survive (symlink() returns EEXIST) and
         * point to the *previous* query's store content. */
        unlink(link_path);           /* remove stale symlink if exists */
        symlink(target, link_path);  /* may still fail (e.g. dir gone) */
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
    char *dup = strdup(key);
    if (!dup) return;
    ctx->recalled_keys[ctx->n_recalled_keys++] = dup;
}

/* ── Fire ledger ────────────────────────────────────── */

#define FIRE_LEDGER_MAX 256

int tool_fire_ledger_contains(tool_ctx_t *ctx, const char *key) {
    if (!ctx || !key) return 0;
    for (int i = 0; i < ctx->n_fire_ledger; i++)
        if (strcmp(ctx->fire_ledger[i], key) == 0) return 1;
    return 0;
}

void tool_fire_ledger_add(tool_ctx_t *ctx, const char *key) {
    if (!ctx || !key) return;
    /* Deduplicate */
    if (tool_fire_ledger_contains(ctx, key)) return;
    /* Cap */
    if (ctx->n_fire_ledger >= FIRE_LEDGER_MAX) return;
    /* Grow if needed */
    if (ctx->n_fire_ledger >= ctx->fire_ledger_cap) {
        int new_cap = ctx->fire_ledger_cap ? ctx->fire_ledger_cap * 2 : 16;
        if (new_cap > FIRE_LEDGER_MAX) new_cap = FIRE_LEDGER_MAX;
        char **new_arr = realloc(ctx->fire_ledger,
                                  (size_t)new_cap * sizeof(char *));
        if (!new_arr) return;
        ctx->fire_ledger = new_arr;
        ctx->fire_ledger_cap = new_cap;
    }
    char *dup = strdup(key);
    if (!dup) return;
    ctx->fire_ledger[ctx->n_fire_ledger++] = dup;
}

void tool_fire_ledger_reset(tool_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->n_fire_ledger; i++)
        free(ctx->fire_ledger[i]);
    ctx->n_fire_ledger = 0;
    /* Keep allocated buffer for reuse */
}

void tool_fire_ledger_free(tool_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->n_fire_ledger; i++)
        free(ctx->fire_ledger[i]);
    free(ctx->fire_ledger);
    ctx->fire_ledger = NULL;
    ctx->n_fire_ledger = 0;
    ctx->fire_ledger_cap = 0;
}

/* ── shell_exec ──────────────────────────────────────── */

/* Resolve ref aliases (R0S1, R2S14, ...) in a shell command string to their
 * full store paths so that e.g. `head -n10 R0S3` works in the shell.
 * Returns a malloc'd string with substitutions, or NULL if nothing to resolve. */
static char *shell_resolve_aliases(tool_ctx_t *ctx, const char *cmd) {
    if (!ctx || !cmd) return NULL;

    str_t resolved = str_new(strlen(cmd) + 256);
    const char *p = cmd;
    int any = 0;

    while (*p) {
        /* Look for R followed by digit */
        if (*p == 'R' && p[1] >= '0' && p[1] <= '9') {
            /* Extract potential alias: R<digits>S<digits> */
            const char *start = p;
            p++; /* skip R */
            while (*p >= '0' && *p <= '9') p++;
            if (*p == 'S' && p[1] >= '0' && p[1] <= '9') {
                p++; /* skip S */
                while (*p >= '0' && *p <= '9') p++;
                /* Check word boundary: next char must not be alnum/underscore */
                if (!*p || !isalnum((unsigned char)*p)) {
                    /* Extract alias token */
                    size_t alen = (size_t)(p - start);
                    char alias[32];
                    if (alen < sizeof(alias)) {
                        memcpy(alias, start, alen);
                        alias[alen] = '\0';
                        char *path = tool_resolve_alias(ctx, alias);
                        if (path) {
                            str_append_cstr(&resolved, path);
                            free(path);
                            any = 1;
                            continue;
                        }
                    }
                }
                /* Not a valid alias — copy the token literally */
                str_append(&resolved, start, (size_t)(p - start));
            } else {
                /* No 'S' — copy literally */
                str_append(&resolved, start, (size_t)(p - start));
            }
        } else {
            str_append(&resolved, p, 1);
            p++;
        }
    }

    if (!any) { str_free(&resolved); return NULL; }
    /* Take ownership of the buffer */
    char *result = resolved.data;
    resolved.data = NULL;
    return result;
}

static tool_result_t tool_shell_exec(tool_ctx_t *ctx, cJSON *params) {
    cJSON *cmd_j = cJSON_GetObjectItem(params, "command");
    if (!cmd_j || !cmd_j->valuestring || !cmd_j->valuestring[0])
        return tools_make_error("shell_exec requires a non-empty 'command' string. "
                          "Provide the shell command to execute.");

    const char *command = cmd_j->valuestring;
    char *resolved_cmd = shell_resolve_aliases(ctx, command);
    if (resolved_cmd) command = resolved_cmd;

    str_t out = str_new(4096);
    char *argv[] = { "sh", "-c", (char *)command, NULL };
    int cfg_timeout = ctx->cfg ? ctx->cfg->shell_timeout : 30;
    int timeout = cfg_timeout;
    cJSON *timeout_j = cJSON_GetObjectItem(params, "timeout");
    if (timeout_j && cJSON_IsNumber(timeout_j)) {
        timeout = (int)timeout_j->valuedouble;
    }
    int max_out = ctx->cfg ? ctx->cfg->shell_max_output : 512000;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    subprocess_result_t r = subprocess_run(argv, NULL, timeout, max_out, 0,
                                           SUBPROCESS_PIPE_STDERR, &out);
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000L +
                      (t_end.tv_nsec - t_start.tv_nsec) / 1000000L;
    if (r.timed_out)
        str_appendf(&out, "\n[TIMEOUT: killed after %ds]\n", timeout);
    else if (r.output_capped)
        str_appendf(&out, "\n[OUTPUT CAPPED at %d bytes]\n", max_out);
    int exit_code = r.exit_code;

    /* Store to shared store */
    char *hash = store_save(ctx->store, out.data);

    /* Register alias */
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "exit_code", exit_code);
    cJSON_AddNumberToObject(meta, "chars", (double)out.len);
    cJSON_AddNumberToObject(meta, "lines", out.data ? count_lines(out.data) : 0);
    cJSON_AddStringToObject(meta, "ref", alias);
    cJSON_AddNumberToObject(meta, "elapsed_ms", (double)elapsed_ms);
    if (elapsed_ms > 10000) {
        char hint[256];
        snprintf(hint, sizeof(hint),
            "This command took %lds. Output is saved at \"%s\". "
            "Re-analyze that instead of re-running the command.",
            elapsed_ms / 1000, alias);
        cJSON_AddStringToObject(meta, "slow_hint", hint);
    }

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
    tool_journal(ctx, "shell_exec", params, alias,
                   out.len, count_lines(out.data), exit_code == 0 ? NULL : "non-zero exit", NULL);

    char *ref_copy = strdup(alias);
    free(alias);
    str_free(&out);
    free(hash);
    free(resolved_cmd);
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
    tool_journal(ctx, "done", params, alias,
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
    tool_journal(ctx, "plan",
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

    /* Set consolidating flag on all involved memory_t instances.
     * Use CAS on global; also set on workspace if active. */
    int expected = 0;
    if (ctx->memory &&
        !atomic_compare_exchange_strong(&ctx->memory->consolidating, &expected, 1)) {
        tool_free_deferred_consolidations(ctx);
        return;
    }
    memory_t *ws_mem = (ctx->ws && ctx->ws->workspace) ? ctx->ws->workspace : NULL;
    if (ws_mem) atomic_store(&ws_mem->consolidating, 1);

    /* Track deletions with their target memory_t */
    typedef struct { char *key; memory_t *target; } del_entry_t;
    del_entry_t *del_entries = NULL;

    for (int i = 0; i < ctx->n_deferred_consol; i++) {
        if (ctx->deferred_consol[i].key && ctx->deferred_consol[i].value) {
            /* FIX #4: Resolve is_workspace flag to live memory_t pointer.
             * Previously stored a raw memory_t* that could dangle after
             * session reset in daemon mode. */
            memory_t *tgt = ctx->deferred_consol[i].is_workspace
                            ? ws_mem : ctx->memory;
            if (!tgt) tgt = ctx->memory;
            char *dk = tools_memory_try_consolidate(ctx, ctx->deferred_consol[i].key,
                                                    ctx->deferred_consol[i].value, tgt);
            if (dk) {
                if (n_del >= del_cap) {
                    del_cap = del_cap ? del_cap * 2 : 16;
                    del_entries = realloc(del_entries, sizeof(del_entry_t) * (size_t)del_cap);
                }
                if (del_entries) {
                    del_entries[n_del].key = dk;
                    del_entries[n_del].target = tgt;
                    n_del++;
                } else {
                    free(dk);
                }
            }
        }
    }
    if (ctx->memory) atomic_store(&ctx->memory->consolidating, 0);
    if (ws_mem) atomic_store(&ws_mem->consolidating, 0);

    /* Batch delete, grouped by target memory_t */
    if (n_del > 0 && del_entries) {
        /* FIX #9: dynamically size batch arrays instead of fixed 64 */
        const char **gl_keys = malloc(sizeof(const char *) * (size_t)n_del);
        const char **ws_keys = malloc(sizeof(const char *) * (size_t)n_del);
        int n_gl = 0;
        int n_ws = 0;
        if (gl_keys && ws_keys) {
            for (int i = 0; i < n_del; i++) {
                if (del_entries[i].target == ws_mem && ws_mem)
                    ws_keys[n_ws++] = del_entries[i].key;
                else
                    gl_keys[n_gl++] = del_entries[i].key;
            }
            if (n_gl > 0 && ctx->memory)
                memory_delete_batch(ctx->memory, gl_keys, n_gl);
            if (n_ws > 0 && ws_mem)
                memory_delete_batch(ws_mem, ws_keys, n_ws);
        }
        free(gl_keys);
        free(ws_keys);
        for (int i = 0; i < n_del; i++) free(del_entries[i].key);
    }
    free(del_entries);

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


/* Handler function pointer type — used by dispatch_handler() below. */
typedef tool_result_t (*tool_handler_fn)(tool_ctx_t *, cJSON *);

/* ── Parameter definitions for core tools ─────────────────────────── */

static const tool_param_t shell_exec_params[] = {
    TOOL_PARAM("command", "string",  "Shell command",                    1),
    TOOL_PARAM("timeout", "integer", "Timeout in seconds (default: 30)", 0),
    TOOL_PARAM_END
};

static const tool_param_t done_params[] = {
    TOOL_PARAM("result", "string", "Complete answer with details", 1),
    TOOL_PARAM_END
};

static const tool_param_t plan_params[] = {
    TOOL_PARAM("result", "string", "Numbered plan: 1. step (tool)\n2. ...", 1),
    TOOL_PARAM_END
};

static const tool_param_t user_ask_params[] = {
    TOOL_PARAM("question", "string", "Question to ask the user", 1),
    TOOL_PARAM_END
};

/* ── Plugin descriptors for tools defined in this file ────────────── */

static const tool_plugin_t core_plugins[] = {
    TOOL_DEF("shell_exec",
             "Execute a shell command (git, make, docker, gh, npm, etc.). "
             "For file reading use file_read, for content search use grep_search, "
             "for file search use glob_search, for URL fetching use web_fetch. "
             "Output is stored at a ref (e.g. R0S3) that resolves to a file path. "
             "Re-analyze stored output (grep/head/tail on the ref) instead of "
             "re-running the command. Do not file_write to ref paths. "
             "For stored refs, shell_exec (grep/head/tail) avoids loading large "
             "outputs into context.",
             shell_exec_params, tool_shell_exec),

    TOOL_DEF("done",
             "Signal task completion. Include all concrete data (paths, numbers, URLs) in result. "
             "The user CANNOT see notes/scratchpad -- never say \"see above\" or reference data only in notes. "
             "Copy all relevant content (tables, lists, data) directly into the result text. "
             "Before calling done, verify every claim in your result is supported by evidence "
             "you actually observed (tool output, file content, command result) -- never state "
             "facts you did not verify or assume tool calls succeeded without reading the output.",
             done_params, tool_done),

    TOOL_DEF("plan",
             "Outline a numbered execution plan (3-8 steps) before starting work.",
             plan_params, tool_plan),

    TOOL_DEF("user_ask",
             "Ask the user a clarifying question. Use when you need information "
             "that cannot be determined from the codebase or context. The react loop "
             "pauses until the user responds. "
             "Prefer calling this EARLY (step 0-2) when the task is ambiguous, rather "
             "than guessing and discovering the wrong assumption later. "
             "Before selecting your first action, assess request_uncertainty on a 0-1 "
             "scale: 0 = fully specified task, 0.5 = missing parameters the user likely "
             "has a preference about, 1 = critically ambiguous. "
             "If request_uncertainty >= 0.5, call user_ask BEFORE proceeding with any "
             "other tool. Do NOT guess when the user's intent is unclear -- ask.",
             user_ask_params, tool_user_ask_stub),
};
TOOL_PLUGIN_REGISTER_ARRAY(core_plugins, 4)

/* Validate required params from the plugin's tool_param_t array, then call
 * the handler.  Returns 1 always (result written to *out). */
static int dispatch_handler(tool_ctx_t *ctx, const char *action, cJSON *params,
                            const tool_param_t *params_def,
                            tool_handler_fn handler,
                            tool_result_t *out) {
    /* Validate required params directly from the struct array */
    char missing[64];
    if (params_def &&
        !tool_params_check_required(params_def, params, missing,
                                    sizeof(missing))) {
        char err[256];
        snprintf(err, sizeof(err),
            "Reminder: %s requires \"%s\" in params. "
            "Re-call with the required parameter.",
            action, missing);
        *out = tools_make_error(err);
        char *ehash = store_save(ctx->store, err);
        char *ealias = ehash ? tool_register_alias(ctx, ehash) : NULL;
        tool_journal(ctx, action, params, ealias, 0, 0, err, NULL);
        free(ehash);
        free(ealias);
        return 1;  /* dispatched (with error) */
    }
    /* Call the handler */
    *out = handler(ctx, params);
    /* Fallback journal if handler didn't call tool_journal() */
    if (!ctx->journal_done) {
        const char *err = NULL;
        if (!out->success && out->meta) {
            cJSON *ej = cJSON_GetObjectItem(out->meta, "error");
            if (ej && ej->valuestring) err = ej->valuestring;
        }
        const char *ref = out->store_ref;
        char *fb_hash = NULL, *fb_alias = NULL;
        if (!ref && err) {
            fb_hash = store_save(ctx->store, err);
            fb_alias = fb_hash ? tool_register_alias(ctx, fb_hash)
                               : NULL;
            if (fb_alias) ref = fb_alias;
        }
        tool_journal(ctx, action, params, ref, 0, 0, err, NULL);
        free(fb_hash);
        free(fb_alias);
    }
    return 1;  /* dispatched */
}

tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params) {
    ctx->journal_done = 0;  /* reset — tool_journal() sets to 1 */

    /* Tool filter: check whitelist/blacklist before dispatch */
    if (!tool_filter_allows(&ctx->tool_filter, action)) {
        char fmsg[256];
        snprintf(fmsg, sizeof(fmsg),
                 "'%s' is not available in this context. "
                 "Use a different tool or approach.", action);
        tool_result_t r = tools_make_error(fmsg);
        /* Store error so failed steps get clickable links in reactRX.md */
        char *fhash = store_save(ctx->store, fmsg);
        char *falias = fhash ? tool_register_alias(ctx, fhash) : NULL;
        tool_journal(ctx, action, params, falias, 0, 0, fmsg, NULL);
        free(fhash);
        free(falias);
        return r;
    }

    /* Plugin registry dispatch — all tools self-register via constructors. */
    {
        const tool_plugin_t *plugin = tool_plugin_find(action);
        if (plugin && plugin->execute) {
            tool_result_t result;
            dispatch_handler(ctx, action, params, plugin->params,
                             (tool_handler_fn)plugin->execute, &result);
            return result;
        }
    }

    /* Concatenated tool name recovery: when the model emits a garbled name
     * like "file_readfile_read" or "shell_execmemory_search", try to find
     * a known tool name as a prefix.  Pick the longest matching prefix to
     * avoid false positives (e.g. "done" matching "donefile_read"). */
    {
        const char *best_name = NULL;
        const tool_param_t *best_params = NULL;
        tool_handler_fn best_handler = NULL;
        size_t best_len = 0;
        size_t action_len = strlen(action);

        for (int i = 0; i < tool_plugin_count(); i++) {
            const tool_plugin_t *p = tool_plugin_get(i);
            if (!p) continue;
            size_t nlen = strlen(p->name);
            if (nlen < action_len && nlen > best_len &&
                strncmp(action, p->name, nlen) == 0) {
                best_name = p->name;
                best_params = p->params;
                best_handler = (tool_handler_fn)p->execute;
                best_len = nlen;
            }
        }

        if (best_handler) {
            /* Re-check tool filter for the recovered name */
            if (!tool_filter_allows(&ctx->tool_filter, best_name)) {
                char fmsg[256];
                snprintf(fmsg, sizeof(fmsg),
                         "'%s' is not available in this context. "
                         "Use a different tool or approach.", best_name);
                tool_result_t r = tools_make_error(fmsg);
                char *fh2 = store_save(ctx->store, fmsg);
                char *fa2 = fh2 ? tool_register_alias(ctx, fh2) : NULL;
                tool_journal(ctx, best_name, params, fa2, 0, 0, fmsg, NULL);
                free(fh2);
                free(fa2);
                return r;
            }
            nash_log("[tool] recovered concatenated tool name: "
                    "'%s' -> '%s' (dropped suffix: '%s')",
                    action, best_name, action + best_len);
            tool_result_t result;
            dispatch_handler(ctx, best_name, params, best_params,
                             best_handler, &result);
            return result;
        }
    }

    /* Truly unknown tool - build available tools list. */
    char msg[1024];
    int pos = snprintf(msg, sizeof(msg), "unknown tool: '%.100s'. Available: ", action);
    int first = 1;
    for (int i = 0; i < tool_plugin_count() && pos < (int)sizeof(msg) - 32; i++) {
        const tool_plugin_t *p = tool_plugin_get(i);
        if (!p) continue;
        if (!first) pos += snprintf(msg + pos, sizeof(msg) - pos, ", ");
        pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", p->name);
        first = 0;
    }
    tool_result_t r = tools_make_error(msg);
    char *uh = store_save(ctx->store, msg);
    char *ua = uh ? tool_register_alias(ctx, uh) : NULL;
    tool_journal(ctx, action, params, ua, 0, 0, msg, NULL);
    free(uh);
    free(ua);
    return r;
}

void tool_result_free(tool_result_t *r) {
    if (r->meta) cJSON_Delete(r->meta);
    free(r->store_ref);
    r->meta = NULL;
    r->store_ref = NULL;
}

/* ── system prompt ───────────────────────────────────── */

char *tools_system_prompt(const char *session_dir, const char *workspace, int headless) {
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

    /* Session-scoped temporary directory: /tmp/.nash/[<workspace>/]<epoch> */
    char tmpdir[NASH_PATH_MAX];
    const char *epoch = "";
    if (session_dir) {
        const char *slash = strrchr(session_dir, '/');
        epoch = slash ? slash + 1 : session_dir;
    }
    if (workspace && workspace[0])
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/.nash/%s/%s", workspace, epoch);
    else
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/.nash/%s", epoch);

    str_t s = str_new(4096);

    /* Identity — headless agents drop "coding" to avoid clashing with
     * agent YAML identities like "CVE analyst" or "news agent". */
    if (headless)
        str_append_cstr(&s, "You are an autonomous agent. Solve the task step by step "
                            "using the available tools.\n");
    else
        str_append_cstr(&s, "You are an autonomous coding agent. Solve the user's task step by step "
                            "using the available tools.\n");

    str_appendf(&s, "\nNow is %s. CWD: %s\n", timebuf, cwdbuf);

    str_appendf(&s,
        "\nTemporary directory: %s\n"
        "Use for scratch files, build artifacts, and intermediate outputs. "
        "Pre-created; cleaned on reboot.\n", tmpdir);

    str_append_cstr(&s,
        "\nStore-and-reference pattern:\n"
        "- Most tool outputs are stored to disk. You see only metadata with a ref "
        "alias (R0S1, R0S2, etc.).\n"
        "- Ref aliases resolve to file paths. Use file_read for small outputs, or "
        "shell_exec (grep/head/tail on the ref) for large ones.\n"
        "- You MUST read the ref if you need to see what a command produced "
        "or what a file contains.\n");

    str_append_cstr(&s,
        "\nRules:\n"
        "- Never invoke tools speculatively. Every tool call must have a clear reason "
        "and you MUST read the result before proceeding.\n"
        "- Never guess tool results. Wait for actual output.\n");

    /* Result visibility — headless-specific context (interactive case
     * is already covered by the done tool's own description). */
    if (headless)
        str_append_cstr(&s,
            "- The result from done is written to result.md and delivered via mailbox.\n");

    str_append_cstr(&s,
        "\nMulti-part feature implementation:\n"
        "- When implementing a feature that touches multiple files, place "
        "TODO(feature-name) markers at every integration point before writing code. "
        "Remove markers only when integration is verified (compiles + tested).\n"
        "- This ensures partially-completed features are discoverable via "
        "grep -rn TODO src/ and the next session knows exactly where to resume.\n"
        "- Use graduated markers: TODO = planned work, FIXME = known bug, "
        "HACK = works but wrong approach.\n");

    str_append_cstr(&s,
        "\nPredict before acting:\n"
        "- Before each tool call, mentally predict what the tool will return.\n"
        "- If your prediction suggests the action won't achieve your goal, "
        "refine the action before executing.\n");

    return str_steal(&s);
}

/* tool_subtask.c — Sub-task spawning: isolated child react_run() calls.
 *
 * Implements the "LLM-as-Code" DAG pattern (arXiv:2606.15874):
 * the parent's context grows by exactly 2 messages per sub-task call
 * (assistant action + tool result), regardless of how many steps the
 * child took internally.  The child's entire chat is freed inside
 * react_run() before control returns.
 *
 * Pattern follows playbook.c:562-652 (proven child context setup). */

#include "tools_internal.h"
#include "react.h"
#include "scratchpad.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>

/* Global counter for unique subtask directory names */
static atomic_int subtask_counter = 0;

/* Maximum steps a subtask may run (prevents runaway children) */
#define SUBTASK_MAX_STEPS 30

/* Maximum nesting depth (prevents unbounded recursion) */
#define SUBTASK_MAX_DEPTH 3

tool_result_t tool_subtask(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring || !query_j->valuestring[0])
        return tools_make_error("subtask requires a 'query' parameter");

    const char *query = query_j->valuestring;

    /* Optional max_steps override (capped at SUBTASK_MAX_STEPS) */
    int max_steps = SUBTASK_MAX_STEPS;
    cJSON *ms_j = cJSON_GetObjectItem(params, "max_steps");
    if (ms_j && cJSON_IsNumber(ms_j)) {
        int requested = (int)ms_j->valuedouble;
        if (requested > 0 && requested < max_steps)
            max_steps = requested;
    }

    /* ── Depth guard ──────────────────────────────────────── */
    /* ctx->react_loop encodes the current loop ID.  To track depth,
     * we count how many subtask_N directories are nested in session_dir.
     * A simpler approach: pass depth through a naming convention. */
    int depth = 0;
    {
        const char *p = ctx->session_dir;
        while ((p = strstr(p, "/subtask_")) != NULL) {
            depth++;
            p += 9;
        }
    }
    if (depth >= SUBTASK_MAX_DEPTH) {
        char err[128];
        snprintf(err, sizeof(err),
                 "Sub-task nesting limit reached (depth %d >= %d). "
                 "Solve this directly instead of spawning another sub-task.",
                 depth, SUBTASK_MAX_DEPTH);
        return tools_make_error(err);
    }

    /* ── Create child session directory under parent's session ──── */
    int seq = atomic_fetch_add(&subtask_counter, 1);
    char child_dir[4096];
    snprintf(child_dir, sizeof(child_dir), "%s/subtask_%d", ctx->session_dir, seq);
    if (mkdir(child_dir, 0755) != 0 && errno != EEXIST) {
        char err[256];
        snprintf(err, sizeof(err), "Failed to create subtask directory: %s", strerror(errno));
        return tools_make_error(err);
    }

    /* ── Create child journal ─────────────────────────────── */
    journal_t *child_journal = journal_new(child_dir);
    if (!child_journal)
        return tools_make_error("Failed to create subtask journal");

    /* ── Create child tool context (follows playbook.c:572 pattern) ── */
    tool_ctx_t child_tools = {
        .store          = ctx->store,       /* shared: global content store */
        .journal        = child_journal,    /* own: isolated journal */
        .memory         = ctx->memory,      /* shared: global memory */
        .ws             = ctx->ws,          /* shared: workspace */
        .cfg            = ctx->cfg,         /* shared: configuration */
        .session_dir    = child_dir,        /* own: subtask directory */
        .session_lock_fd = -1,              /* no lock needed (under parent dir) */
        .provider       = ctx->provider,    /* shared: LLM provider */
        .react_loop     = 0,               /* fresh loop numbering */
        .aliases        = alias_map_new(),  /* own: fresh alias map */
        .last_notes_step = -1,             /* init: no notes yet */
    };

    /* Scratchpad: snapshot parent's scratchpad into child (read-only copy).
     * Child modifications do NOT propagate back to parent.
     * No scratchpad_copy() exists, so we serialize + parse. */
    scratchpad_init(&child_tools.scratch);
    char *parent_scratch = scratchpad_serialize(&ctx->scratch);
    if (parent_scratch) {
        scratchpad_parse(&child_tools.scratch, parent_scratch, "inherited", 5);
        free(parent_scratch);
    }

    /* Block user_ask in child — no TUI user to answer.
     * We do this by not initializing the user_ask mutex/cond in the
     * child react_ctx_t (leaving them zeroed), which means react_run
     * will work but user_ask cannot be used. The tool_filter blocks it. */
    const char *blocked_tools[] = { "user_ask", "subtask" };  /* prevent recursion via filter too */
    tool_filter_t child_filter = {
        .blocked   = blocked_tools,
        .n_blocked = (depth + 1 >= SUBTASK_MAX_DEPTH) ? 2 : 1, /* block subtask at max depth-1 */
    };
    /* If parent already has a filter, we only add our blocks.
     * For simplicity, just use our filter (subtask inherits all tools
     * except user_ask, and subtask at depth limit). */
    child_tools.tool_filter = child_filter;

    /* ── Create child react context ───────────────────────── */
    react_ctx_t child_react = {
        .provider    = ctx->provider,       /* shared */
        .tools       = &child_tools,        /* own */
        .max_steps   = max_steps,           /* capped */
        .verbose     = 0,                   /* quiet — parent handles UI */
        .flags       = REACT_FLAGS_BARE,    /* no reflection/scoring/memory injection */
        .parent_loop = ctx->react_loop,     /* DAG edge to parent */
    };

    /* Initialize user_ask mutex/cond (react_run may reference them) */
    pthread_mutex_init(&child_react.user_ask_mutex, NULL);
    pthread_cond_init(&child_react.user_ask_cond, NULL);
    pthread_mutex_init(&child_react.pause_mutex, NULL);
    pthread_cond_init(&child_react.pause_cond, NULL);

    /* ── Journal the subtask start ────────────────────────── */
    tools_inject_thought(ctx, params);

    /* ── Run the child react loop ─────────────────────────── */
    char *result = react_run(&child_react, query, NULL, NULL);

    /* ── Cleanup child resources ──────────────────────────── */
    pthread_mutex_destroy(&child_react.user_ask_mutex);
    pthread_cond_destroy(&child_react.user_ask_cond);
    pthread_mutex_destroy(&child_react.pause_mutex);
    pthread_cond_destroy(&child_react.pause_cond);
    free(child_react.user_ask_question);
    free(child_react.user_ask_answer);
    free(child_react.pause_query);

    scratchpad_free(&child_tools.scratch);
    alias_map_free(child_tools.aliases);
    tool_free_deferred_consolidations(&child_tools);
    journal_free(child_journal);
    /* child_dir is stack-allocated, no free needed */
    /* session_lock_fd is -1, no release needed */

    /* ── Build result for parent context ──────────────────── */
    if (!result) {
        return tools_make_error("Sub-task failed to produce a result");
    }

    /* Store result in content-addressed store */
    char *hash = store_save(ctx->store, result);
    char *alias = hash ? tool_register_alias(ctx, hash) : NULL;

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "subtask completed");
    cJSON_AddStringToObject(meta, "result", result);
    cJSON_AddNumberToObject(meta, "depth", depth + 1);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    tool_journal(ctx, "subtask", params, alias,
                 strlen(result), 0, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(result);
    return tools_make_result(1, meta, ref_copy);
}

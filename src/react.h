#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "react_event.h"
#include <stdatomic.h>
#include <pthread.h>

/* React loop subsystem flags — controls which subsystems fire per react_run().
 * Default: all enabled (1). Playbooks/dream can selectively disable. */
typedef struct {
    unsigned int inject_memory       : 1;  /* inject memory index + pinned + recall */
    unsigned int inject_prev_result  : 1;  /* inject result.md as [PREVIOUS RESULT] */
    unsigned int enable_reflection   : 1;  /* post-task reflection (memory_store lessons) */
    unsigned int enable_pruning      : 1;  /* post-reflection scratchpad pruning */
    unsigned int enable_compaction   : 1;  /* LLM-based context eviction/summarization */
    unsigned int enable_scoring      : 1;  /* validation scoring (recall_hits/misses) */
} react_flags_t;

/* Default: all subsystems enabled */
#define REACT_FLAGS_DEFAULT { 1, 1, 1, 1, 1, 1 }

/* Bare mode: all subsystems disabled (for dream/playbook passes) */
#define REACT_FLAGS_BARE    { 0, 0, 0, 0, 0, 0 }

/* Thread ownership contract for react_ctx_t:
 *
 *   INIT-ONLY (set before pthread_create, never modified during react_run):
 *     provider, tools, max_steps, verbose, flags, parent_loop
 *     Note: provider->cfg.enable_thinking and thinking_budget are copied
 *     from ctx->rt just before each provider_complete_stream() call.
 *     The authoritative values live in ctx->rt (INFER-ONLY); cfg is only
 *     written as a transfer mechanism immediately before the provider call.
 *     chars_per_token lives in ctx->rt (not provider->cfg).
 *
 *   MAIN→INFER (set by main thread, read by inference thread):
 *     pause_requested  — atomic_int, safe for cross-thread signaling
 *     user_ask_answer  — protected by user_ask_mutex
 *
 *   INFER→MAIN (set by inference thread, read by main thread):
 *     user_ask_pending  — atomic_int, safe for cross-thread polling
 *     user_ask_question — protected by user_ask_mutex
 *     paused            — set by inference thread inside react_run ONLY,
 *                         read by main thread after pthread_join (happens-before)
 *
 *   BETWEEN-RUNS (set by main thread between react_run calls, after join):
 *     last_query, last_result — safe by happens-before (pthread_join → next setup)
 */
/* Mutable per-loop runtime state.
 * These values change during react_run() and must NOT live in provider->cfg
 * (which is documented as INIT-ONLY). */
typedef struct {
    float chars_per_token;   /* EMA-calibrated chars/token ratio */
    int   enable_thinking;   /* thinking mode (0=off, 1=on) */
    int   thinking_budget;   /* thinking token budget */
    int   preamble_consumed; /* 1 after plan() — degrade preamble importance to LOW */
} react_runtime_t;

typedef struct {
    provider_t   *provider;  /* [INIT-ONLY] default/worker provider */
    provider_t   *planner_provider;    /* [INIT-ONLY] planning steps (step 0). NULL = use provider */
    provider_t   *reflection_provider; /* [INIT-ONLY] post-task reflection. NULL = use provider */
    tool_ctx_t   *tools;     /* [INIT-ONLY] tool context */
    int           max_steps; /* [INIT-ONLY] max react loop iterations */
    int           verbose;   /* [INIT-ONLY] verbosity level */
    react_flags_t flags;     /* [INIT-ONLY] controls which subsystems fire */
    const char   *custom_system_prompt; /* [INIT-ONLY] per-pass system prompt from YAML (NULL = default) */
    int           system_prompt_replace; /* [INIT-ONLY] 0 = append to base, 1 = replace base entirely */
    int           headless;  /* [INIT-ONLY] 1 = no UI (agent/headless mode) */
    react_runtime_t rt;      /* [INFER-ONLY] mutable per-loop runtime state */
    atomic_int    pause_requested;  /* [MAIN→INFER] set by TUI (Space) to pause */
    int           paused;           /* [INFER→MAIN] 1 when paused (read after join) */

    /* user_ask: model asks user a question during the react loop.
     * The inference thread sets question + pending, emits REACT_EVENT_USER_ASK,
     * then waits on user_ask_cond until the TUI thread sets the answer.
     * All fields protected by user_ask_mutex except user_ask_pending (atomic). */
    atomic_int    user_ask_pending;   /* [INFER→MAIN] 1 = waiting for answer */
    char         *user_ask_question;  /* [INFER, guarded by user_ask_mutex] */
    char         *user_ask_answer;    /* [MAIN, guarded by user_ask_mutex] */
    pthread_mutex_t user_ask_mutex;   /* protects user_ask handoff */
    pthread_cond_t  user_ask_cond;    /* signaled when answer is ready */
    int             user_ask_used;    /* [INFER] 1 if user_ask was called this loop */

    /* pause_wait: when the react loop is paused and waiting for user input.
     * The inference thread sets pause_waiting=1, emits REACT_EVENT_WARNING,
     * then waits on pause_cond until the TUI thread provides a redirect query.
     * This keeps the llm_chat_t alive so conversation context is preserved.
     * All fields protected by pause_mutex except pause_waiting (atomic). */
    atomic_int    pause_waiting;    /* [INFER→MAIN] 1 = paused, waiting for query */
    char         *pause_query;     /* [MAIN, guarded by pause_mutex] redirect query */
    pthread_mutex_t pause_mutex;   /* protects pause handoff */
    pthread_cond_t  pause_cond;    /* signaled when redirect query is ready */

    /* Cross-query context inheritance.
     * [BETWEEN-RUNS] set by main thread after pthread_join, before next react_run.
     * Safe by happens-before guarantee of pthread_join → pthread_create. */
    char         *last_query;    /* previous query text (NULL for first query) */
    char         *last_result;   /* previous result text (NULL for first query) */

    /* [INIT-ONLY] Tree-based branching: parent of the current react loop.
     * -1 = root (no parent), otherwise the react_loop ID of the parent.
     * Set by main thread before pthread_create. */
    int           parent_loop;

    /* [INIT-ONLY] Path of file user was viewing in TUI at query time.
     * NULL if not in TUI mode, viewing session.md, or no file loaded.
     * Ownership: strdup'd by main thread, freed after pthread_join. */
    char         *tui_viewing_file;
} react_ctx_t;

/* Run the react loop for a user query.
 * Each call starts with FRESH context: system prompt + journal manifest + scratchpad.
 * Cross-query continuity comes from the journal manifest and last_query/last_result.
 * on_event: callback for all react events (NULL = silent mode)
 * userdata: passed to on_event
 * Returns the final result string (caller frees). */
char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata);

/* Read the original user_query from a checkpoint without restoring full state.
 * Used by /continue to resume with the original query instead of "continue".
 * Returns heap-allocated string or NULL if no checkpoint exists. Caller frees. */
char *checkpoint_read_query(const char *session_dir);

#endif

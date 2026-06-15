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
    unsigned int inject_prev_result  : 1;  /* inject result.txt as [PREVIOUS RESULT] */
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
 *     Note: provider->cfg.enable_thinking and thinking_budget are set per-call
 *     by the inference thread before provider_complete_stream(), which is safe
 *     because no other thread reads them concurrently.
 *     FIX #4: chars_per_token is now in ctx->rt (not provider->cfg).
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
/* FIX #4: Mutable per-loop runtime state.
 * These values change during react_run() and must NOT live in provider->cfg
 * (which is documented as INIT-ONLY). */
typedef struct {
    float chars_per_token;   /* EMA-calibrated chars/token ratio */
    int   enable_thinking;   /* EDRM routing result (0/1) */
    int   thinking_budget;   /* thinking token budget */
} react_runtime_t;

typedef struct {
    provider_t   *provider;  /* [INIT-ONLY] provider abstraction */
    tool_ctx_t   *tools;     /* [INIT-ONLY] tool context */
    int           max_steps; /* [INIT-ONLY] max react loop iterations */
    int           verbose;   /* [INIT-ONLY] verbosity level */
    react_flags_t flags;     /* [INIT-ONLY] controls which subsystems fire */
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

    /* Cross-query context inheritance.
     * [BETWEEN-RUNS] set by main thread after pthread_join, before next react_run.
     * Safe by happens-before guarantee of pthread_join → pthread_create. */
    char         *last_query;    /* previous query text (NULL for first query) */
    char         *last_result;   /* previous result text (NULL for first query) */

    /* [INIT-ONLY] Tree-based branching: parent of the current react loop.
     * -1 = root (no parent), otherwise the react_loop ID of the parent.
     * Set by main thread before pthread_create. */
    int           parent_loop;
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

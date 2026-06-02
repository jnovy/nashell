#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "react_event.h"

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

typedef struct {
    provider_t   *provider;  /* provider abstraction (replaces llm_config_t) */
    llm_config_t *llm;       /* kept for backward compat (EDRM probe, etc.) */
    tool_ctx_t   *tools;
    int           max_steps;
    int           verbose;
    react_flags_t flags;          /* controls which subsystems fire */
    volatile int  pause_requested;  /* set by TUI (Space) to pause after current step */
    int           paused;           /* 1 when paused with checkpoint saved (toggle state) */

    /* user_ask: model asks user a question during the react loop.
     * The inference thread sets question + pending, emits REACT_EVENT_USER_ASK,
     * then polls user_ask_pending until the TUI thread sets the answer. */
    volatile int  user_ask_pending;   /* 1 = waiting for answer, 0 = idle */
    char         *user_ask_question;  /* question text (set by inference thread) */
    char         *user_ask_answer;    /* answer text (set by TUI thread, freed by inference) */

    /* Cross-query context inheritance (set by caller between react_run calls) */
    char         *last_query;    /* previous query text (NULL for first query) */
    char         *last_result;   /* previous result text (NULL for first query) */
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

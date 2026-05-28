#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "react_event.h"

typedef struct {
    provider_t   *provider;  /* provider abstraction (replaces llm_config_t) */
    llm_config_t *llm;       /* kept for backward compat (EDRM probe, etc.) */
    tool_ctx_t   *tools;
    int           max_steps;
    int           verbose;
    volatile int  pause_requested;  /* set by TUI (ESC key) to pause after current step */

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

#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "tools.h"
#include "react_event.h"

typedef struct {
    llm_config_t *llm;
    tool_ctx_t   *tools;
    int           max_steps;
    int           verbose;

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

#endif

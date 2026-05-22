#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "tools.h"
#include "react_event.h"

#define MAX_REACT_STEPS 50

typedef struct {
    llm_config_t *llm;
    tool_ctx_t   *tools;
    int           max_steps;
    int           verbose;       /* kept for backward compat, ignored if on_event set */
} react_ctx_t;

/* Run the react loop for a user query.
 * on_event: callback for all react events (NULL = silent mode)
 * userdata: passed to on_event
 * Returns the final result string (caller frees). */
char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata);

#endif

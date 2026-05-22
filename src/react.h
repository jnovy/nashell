#ifndef REACT_H
#define REACT_H

#include "llm.h"
#include "tools.h"

#define MAX_REACT_STEPS 50

typedef struct {
    llm_config_t *llm;
    tool_ctx_t   *tools;
    int           max_steps;
    int           verbose;
} react_ctx_t;

/* Run the react loop for a user query. Returns the final result string (caller frees). */
char *react_run(react_ctx_t *ctx, const char *user_query);

#endif

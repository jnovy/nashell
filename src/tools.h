#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"
#include "store.h"
#include "journal.h"

/* Tool result: metadata JSON + optional stored content ref */
typedef struct {
    cJSON  *meta;       /* metadata JSON returned to model context */
    char   *store_ref;  /* path in store/ where full output lives */
    int     success;    /* 1 = ok, 0 = error */
} tool_result_t;

/* Session context passed to all tools */
typedef struct {
    store_t   *store;
    journal_t *journal;
    char      *session_dir;   /* .session/<id>/ */
    char      *scratchpad;    /* current scratchpad content (owned) */
    int        step;          /* current step number */
} tool_ctx_t;

/* Execute a tool by name, returns result (caller frees) */
tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params);

/* Free a tool result */
void tool_result_free(tool_result_t *r);

/* System prompt with tool descriptions */
const char *tools_system_prompt(void);

#endif

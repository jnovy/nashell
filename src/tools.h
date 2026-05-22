#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"
#include "store.h"
#include "journal.h"
#include "memory.h"

/* Tool result: metadata JSON + optional stored content hash */
typedef struct {
    cJSON  *meta;       /* metadata JSON returned to model context */
    char   *store_ref;  /* hash in shared store (caller frees) */
    int     success;    /* 1 = ok, 0 = error */
} tool_result_t;

/* Step alias entry: maps R1S0, R1S1, ... to store hashes */
#define MAX_ALIASES 256
typedef struct {
    char alias[16];     /* "R1S0", "R1S1", "R2S1", ... */
    char hash[128];     /* content hash in shared store */
} alias_entry_t;

/* Session context passed to all tools */
typedef struct {
    store_t       *store;
    journal_t     *journal;
    memory_t      *memory;        /* long-term memory store (.memory/) */
    char          *session_dir;   /* .sessions/<id>/ */
    char          *scratchpad;    /* current scratchpad content (owned) */
    int            step;          /* current step number (within react loop) */
    int            react_loop;    /* react loop counter (0-based, increments per query) */
    /* Step alias tracking */
    alias_entry_t  aliases[MAX_ALIASES];
    int            alias_count;
} tool_ctx_t;

/* Register a store hash as a step alias, returns alias string like "R1S0" */
const char *tool_register_alias(tool_ctx_t *ctx, const char *hash);

/* Resolve a step alias (e.g. "R1S1") to the full store path. Returns NULL if not found.
 * Returned string must be freed by caller. */
const char *tool_resolve_alias(tool_ctx_t *ctx, const char *alias);

/* Execute a tool by name, returns result (caller frees) */
tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params);

/* Free a tool result */
void tool_result_free(tool_result_t *r);

/* System prompt with tool descriptions */
const char *tools_system_prompt(void);

#endif

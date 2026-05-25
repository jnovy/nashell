#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"
#include "store.h"
#include "journal.h"
#include "memory.h"
#include "config.h"

/* Tool result: metadata JSON + optional stored content hash */
typedef struct {
    cJSON  *meta;       /* metadata JSON returned to model context */
    char   *store_ref;  /* hash in shared store (caller frees) */
    int     success;    /* 1 = ok, 0 = error */
} tool_result_t;

/* Dynamic hash map for step aliases (R1S0 → store hash).
 * Grows automatically — no artificial limit. */
typedef struct alias_node {
    char        *alias;      /* "R1S0", "R1S1", etc. */
    char        *hash;       /* content hash in shared store */
    struct alias_node *next; /* chain for collisions */
} alias_node_t;

typedef struct {
    alias_node_t **buckets;
    int            capacity;
    int            count;
    int            next_seq;  /* next step sequence number for alias generation */
} alias_map_t;

/* Hash map lifecycle */
alias_map_t *alias_map_new(void);
void         alias_map_free(alias_map_t *map);
void         alias_map_clear(alias_map_t *map);  /* keep allocated buckets */
void        *alias_map_insert(alias_map_t *map, const char *alias, const char *hash);
const char  *alias_map_lookup(alias_map_t *map, const char *alias);

/* Session context passed to all tools */
typedef struct {
    store_t       *store;
    journal_t     *journal;
    memory_t      *memory;        /* long-term memory store (.memory/) */
    config_t      *cfg;           /* configuration (tool limits, etc.) */
    char          *session_dir;   /* .sessions/<id>/ */
    char          *scratchpad;    /* current scratchpad content (owned) */
    int            step;          /* current step number (within react loop) */
    int            react_loop;    /* react loop counter (0-based, increments per query) */
    /* Step alias tracking — dynamic hash map, no size limit */
    alias_map_t   *aliases;
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

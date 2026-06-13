#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"
#include "store.h"
#include "journal.h"
#include "memory.h"
#include "config.h"
#include "llm.h"
#include "provider.h"

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

#include "scratchpad.h"

/* Tool filter: whitelist or blacklist tool access per-pass.
 * If allowed is non-NULL, only those tools can be called (whitelist mode).
 * If blocked is non-NULL, those tools are denied (blacklist mode).
 * Both NULL = all tools available (default).
 * desc_names/desc_values: per-tool description overrides (Unified Spec). */
typedef struct tool_filter_t {
    const char **allowed;    /* NULL = all allowed; non-NULL = whitelist */
    int          n_allowed;
    const char **blocked;    /* NULL = none blocked; non-NULL = blacklist */
    int          n_blocked;
    /* Per-tool description overrides (parallel arrays, NULL = no overrides) */
    char       **desc_names;   /* tool names with overridden descriptions */
    char       **desc_values;  /* replacement description strings */
    int          n_descs;
} tool_filter_t;

/* Session context passed to all tools */
typedef struct {
    store_t       *store;
    journal_t     *journal;
    memory_t      *memory;        /* long-term memory store (.memory/) */
    config_t      *cfg;           /* configuration (tool limits, etc.) */
    char          *session_dir;   /* .sessions/<id>/ */
    scratchpad_t   scratch;       /* section-based scratchpad */
    provider_t    *provider;     /* provider abstraction (FIX #3: for consolidation) */
    int            step;          /* current step number (within react loop) */
    int            react_loop;    /* react loop counter (0-based, increments per query) */
    /* Step alias tracking — dynamic hash map, no size limit */
    alias_map_t   *aliases;
    /* Validation scoring: track which memory keys were recalled this task */
    char         **recalled_keys;
    int            n_recalled_keys;
    int            recalled_keys_cap;
    /* Current step's thought (set by react.c before tool_execute, cleared after) */
    const char    *thought;
    /* Per-pass tool access control (playbooks/dream) */
    tool_filter_t  tool_filter;
    /* Spec journal tracking: last spec hash for change detection */
    char          *last_spec_hash;
    /* FIX CRIT1: Deferred consolidation queue — populated during react loop,
     * flushed after task completion to avoid blocking LLM calls mid-task. */
    struct {
        char *key;
        char *value;
    }             *deferred_consol;
    int            n_deferred_consol;
    int            cap_deferred_consol;
} tool_ctx_t;

/* Track a recalled memory key for post-task validation scoring */
void tool_track_recalled_key(tool_ctx_t *ctx, const char *key);

/* Register a store hash as a step alias, returns alias string like "R1S0".
 * Caller must free the returned string. */
char *tool_register_alias(tool_ctx_t *ctx, const char *hash);

/* Resolve a step alias (e.g. "R1S1") to the full store path. Returns NULL if not found.
 * Returned string must be freed by caller. */
/* Resolve a step alias (R0S1, R1S2, ...) to a full store path.
 * Returns heap-allocated string (caller must free), or NULL if not an alias. */
char *tool_resolve_alias(tool_ctx_t *ctx, const char *alias);

/* Execute a tool by name, returns result (caller frees) */
tool_result_t tool_execute(tool_ctx_t *ctx, const char *action, cJSON *params);

/* Free a tool result */
void tool_result_free(tool_result_t *r);

/* System prompt with tool descriptions */
char *tools_system_prompt(void);  /* caller must free() */

/* FIX CRIT1: Process deferred memory consolidations after task completion.
 * Runs the LLM-based consolidation that was queued during memory_store calls,
 * outside the hot path of the react loop. */
void tool_flush_deferred_consolidations(tool_ctx_t *ctx);

/* FIX CRIT1: Free the deferred consolidation queue (call before tool_ctx cleanup) */
void tool_free_deferred_consolidations(tool_ctx_t *ctx);

/* Tear down auto-started SearXNG container (called on nash exit) */
void web_search_cleanup(void);

#endif

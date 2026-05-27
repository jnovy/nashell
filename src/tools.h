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

/* ── Section-based scratchpad (GDN-2 inspired) ──────────── */
/* Each section has independent name, content, and priority.
 * Operations: write, append, read, clear, list.
 * Priority determines compression/eviction order under context pressure. */

#define SCRATCHPAD_MAX_SECTIONS 32

typedef struct {
    char *name;       /* section name (e.g. "findings", "plan", "status") */
    char *content;    /* section content (owned) */
    int   priority;   /* 1 = highest priority, 9 = lowest. Default: 5 */
} scratchpad_section_t;

typedef struct {
    scratchpad_section_t sections[SCRATCHPAD_MAX_SECTIONS];
    int count;
} scratchpad_t;

/* Scratchpad lifecycle */
void scratchpad_init(scratchpad_t *sp);
void scratchpad_free(scratchpad_t *sp);

/* Find section by name. Returns index or -1. */
int scratchpad_find(scratchpad_t *sp, const char *name);

/* Write (create/overwrite) a section. Returns 0 on success. */
int scratchpad_write(scratchpad_t *sp, const char *name, const char *content, int priority);

/* Append to a section (creates if not exists). Returns 0 on success. */
int scratchpad_append(scratchpad_t *sp, const char *name, const char *content, int priority);

/* Clear (delete) a section. Returns 0 on success, -1 if not found. */
int scratchpad_clear(scratchpad_t *sp, const char *name);

/* Serialize all sections to a single string for context injection.
 * Format: "## section_name\ncontent\n\n## section2\ncontent2\n"
 * Sections are ordered by priority (1 first, 9 last).
 * Caller must free. Returns NULL if empty. */
char *scratchpad_serialize(scratchpad_t *sp);

/* Serialize with a max_chars budget. Low-priority sections are truncated/dropped first.
 * Caller must free. Returns NULL if empty. */
char *scratchpad_serialize_budget(scratchpad_t *sp, size_t max_chars);

/* Persist scratchpad to disk (session_dir/scratchpad.md). */
int scratchpad_save(scratchpad_t *sp, const char *session_dir);

/* Load scratchpad from disk. Returns 0 on success. */
int scratchpad_load(scratchpad_t *sp, const char *session_dir);

/* Session context passed to all tools */
typedef struct {
    store_t       *store;
    journal_t     *journal;
    memory_t      *memory;        /* long-term memory store (.memory/) */
    config_t      *cfg;           /* configuration (tool limits, etc.) */
    char          *session_dir;   /* .sessions/<id>/ */
    char          *scratchpad;    /* legacy: serialized scratchpad (owned, auto-generated) */
    scratchpad_t   scratch;       /* section-based scratchpad (GDN-2 inspired) */
    llm_config_t  *llm;          /* LLM config for inline consolidation (P2) */
    provider_t    *provider;     /* provider abstraction (FIX #3: for consolidation) */
    int            step;          /* current step number (within react loop) */
    int            react_loop;    /* react loop counter (0-based, increments per query) */
    /* Step alias tracking — dynamic hash map, no size limit */
    alias_map_t   *aliases;
    /* Validation scoring: track which memory keys were recalled this task */
    char         **recalled_keys;
    int            n_recalled_keys;
    int            recalled_keys_cap;
} tool_ctx_t;

/* Track a recalled memory key for post-task validation scoring */
void tool_track_recalled_key(tool_ctx_t *ctx, const char *key);

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

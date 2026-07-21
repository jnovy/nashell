#ifndef TOOLS_H
#define TOOLS_H

#include <stdint.h>
#include "cJSON.h"
#include "store.h"
#include "journal.h"
#include "memory.h"
#include "workspace.h"
#include "config.h"
#include "llm.h"
#include "provider.h"
#include "react_event.h"

/* Forward declaration for session index (v4 unified memory L3 tier) */
typedef struct session_index_t session_index_t;

/* Tool result: metadata JSON + optional stored content hash */
typedef struct {
    cJSON  *meta;       /* metadata JSON returned to model context */
    char   *store_ref;  /* hash in shared store (caller frees) */
    int     success;    /* 1 = ok, 0 = error */
    int     importance; /* 0=low, 1=normal, 2=high, 3=critical (Harness-1 §3.2) */
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
const char  *alias_map_reverse_lookup(alias_map_t *map, const char *hash);

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
    memory_t      *memory;        /* long-term memory store (.memory/) — points to active layer */
    workspace_t   *ws;            /* workspace: two-layer memory (global + workspace) */
    config_t      *cfg;           /* configuration (tool limits, etc.) */
    char          *session_dir;   /* .sessions/<id>/ */
    int            session_lock_fd; /* flock fd for exclusive session access (-1 = none) */
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
        memory_t *target;  /* which memory_t to consolidate against */
    }             *deferred_consol;
    int            n_deferred_consol;
    int            cap_deferred_consol;
    /* Harness-1 §3.3: Context-level deduplication — CRC32 hashes of recent
     * tool result content to detect near-duplicate injections.
     * FIX MED#7: Added dedup_lens as secondary collision guard — CRC32's
     * 32-bit hash space has high collision rates for structured JSON data.
     * Requiring both hash AND length match reduces false positives. */
    uint32_t       dedup_hashes[64]; /* rolling buffer of content hashes */
    uint32_t       dedup_lens[64];   /* content length for each hash (collision guard) */
    int            dedup_steps[64];  /* step number for each hash */
    int            dedup_count;      /* entries in dedup buffer */
    /* Harness-1 §4.2: Tool usage tracking for diversity nudging */
    int            tool_use_counts[32]; /* indexed by tool_registry order */
    int            n_tool_uses;     /* total tool invocations this loop */
    /* Incremental notes tracking: detect deferred synthesis anti-pattern.
     * When the model reads many files without saving findings, compaction
     * evicts the raw content and the model confabulates from degraded memory. */
    int            last_notes_step;          /* step when notes() last used (-1 = never) */
    int            file_reads_since_notes;   /* file_read calls since last notes() */
    int            pre_compact_warned;       /* 1 = pre-compaction warning already fired */
    /* v4 unified memory: session index for L3 search via memory_query */
    session_index_t *session_idx;
    /* Journal enforcement: set by tool_journal(), checked by tool_execute().
     * If a handler returns without setting this, tool_execute() adds a
     * fallback journal entry — so no tool call is ever invisible. */
    int            journal_done;
    double         start_ts;       /* tool start time (epoch), set by react.c before tool_execute */
    /* Event callback from parent react loop — threaded through so that
     * child react loops (subtask) can forward events to the TUI. */
    react_event_fn on_event;       /* parent's event callback (NULL = headless) */
    void          *on_event_data;  /* parent's event userdata */
} tool_ctx_t;

/* Track a recalled memory key for post-task validation scoring */
void tool_track_recalled_key(tool_ctx_t *ctx, const char *key);

/* Scan session_dir for existing R<loop>S<N> symlinks and return the
 * highest sequence number found, or -1 if none exist. */
int alias_scan_max_seq(const char *session_dir, int react_loop);

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

/* System prompt with tool descriptions.
 * session_dir: full path to session directory (epoch extracted via basename).
 * workspace:   workspace name (may contain '/', e.g. "rh/container-tools"), or NULL.
 * headless:    1 = agent/headless mode (suppresses user_ask rules, adjusts identity).
 * A session-scoped temp directory instruction is included in the prompt. */
char *tools_system_prompt(const char *session_dir, const char *workspace, int headless);  /* caller must free() */

/* FIX CRIT1: Process deferred memory consolidations after task completion.
 * Runs the LLM-based consolidation that was queued during memory_store calls,
 * outside the hot path of the react loop. */
void tool_flush_deferred_consolidations(tool_ctx_t *ctx);

/* FIX CRIT1: Free the deferred consolidation queue (call before tool_ctx cleanup) */
void tool_free_deferred_consolidations(tool_ctx_t *ctx);

/* Tear down auto-started SearXNG container — see searxng.h */

#endif

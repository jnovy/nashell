#ifndef TOOLS_H
#define TOOLS_H

#include <stdint.h>

/* Maximum number of tools tracked for usage diversity nudging.
 * Must be >= total built-in + plugin tools to avoid silently
 * ignoring tools beyond this index. */
#define MAX_TOOL_TRACKED 64
#include "cJSON.h"
#include "tool_plugin.h" /* tool_result_t, tool_plugin_t, tool_param_t */
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

/* Forward declaration for prediction tracking (decision observability) */
typedef struct predict_tracker_t predict_tracker_t;

/* Forward declaration for react context (circular: react.h includes tools.h) */
typedef struct react_ctx_t react_ctx_t;

/* Dynamic hash map for step aliases (R1S0 → store hash).
 * Grows automatically — no artificial limit. */
typedef struct alias_node {
  char *alias;             /* "R1S0", "R1S1", etc. */
  char *hash;              /* content hash in shared store */
  struct alias_node *next; /* chain for collisions */
} alias_node_t;

typedef struct {
  alias_node_t **buckets;
  int capacity;
  int count;
  int next_seq; /* next step sequence number for alias generation */
} alias_map_t;

/* Hash map lifecycle */
alias_map_t *alias_map_new(void);
void alias_map_free(alias_map_t *map);
void alias_map_clear(alias_map_t *map); /* keep allocated buckets */
void *alias_map_insert(alias_map_t *map, const char *alias, const char *hash);
const char *alias_map_lookup(alias_map_t *map, const char *alias);
const char *alias_map_reverse_lookup(alias_map_t *map, const char *hash);

#include "scratchpad.h"

/* Tool filter: whitelist or blacklist tool access per-pass.
 * If allowed is non-NULL, only those tools can be called (whitelist mode).
 * If blocked is non-NULL, those tools are denied (blacklist mode).
 * Both NULL = all tools available (default).
 * desc_names/desc_values: per-tool description overrides (Unified Spec). */
typedef struct tool_filter_t {
  const char **allowed; /* NULL = all allowed; non-NULL = whitelist */
  int n_allowed;
  const char **blocked; /* NULL = none blocked; non-NULL = blacklist */
  int n_blocked;
  int blocked_owned; /* 1 if blocked[] is heap-owned (deep-copied) */
  /* Per-tool description overrides (parallel arrays, NULL = no overrides) */
  char **desc_names;  /* tool names with overridden descriptions */
  char **desc_values; /* replacement description strings */
  int n_descs;
} tool_filter_t;

/* Session context passed to all tools */
typedef struct {
  store_t *store;
  journal_t *journal;
  memory_t *memory;     /* long-term memory store (.memory/) — points to active layer */
  workspace_t *ws;      /* workspace: two-layer memory (global + workspace) */
  config_t *cfg;        /* configuration (tool limits, etc.) */
  char *session_dir;    /* .sessions/<id>/ */
  int session_lock_fd;  /* flock fd for exclusive session access (-1 = none) */
  scratchpad_t scratch; /* section-based scratchpad */
  provider_t *provider;   /* provider abstraction (FIX #3: for consolidation) */
  react_ctx_t *react_ctx;  /* parent react context (for subtask pause routing) */
  int step;               /* current step number (within react loop) */
  int react_loop;       /* react loop counter (0-based, increments per query) */
  /* Step alias tracking — dynamic hash map, no size limit */
  alias_map_t *aliases;
  /* Validation scoring: track which memory keys were recalled this task */
  char **recalled_keys;
  int n_recalled_keys;
  int recalled_keys_cap;
  /* Fire ledger: tracks which memory keys have been injected in the
     * current context window. Prevents redundant injection within a window
     * but resets on compaction so facts re-arm. (arXiv 2607.20972) */
  char **fire_ledger;
  int n_fire_ledger;
  int fire_ledger_cap;
  /* Current step's thought (set by react.c before tool_execute, cleared after) */
  const char *thought;
  /* Per-pass tool access control (playbooks/dream) */
  tool_filter_t tool_filter;
  /* Spec journal tracking: last spec hash for change detection */
  char *last_spec_hash;
  /* FIX CRIT1: Deferred consolidation queue — populated during react loop,
     * flushed after task completion to avoid blocking LLM calls mid-task. */
  struct {
    char *key;
    char *value;
    int is_workspace; /* FIX #4: flag instead of raw memory_t* to avoid
                             * dangling pointer after session reset. Resolved
                             * to the live memory_t at flush time. */
  } *deferred_consol;
  int n_deferred_consol;
  int cap_deferred_consol;
  /* Harness-1 §3.3: Context-level deduplication — CRC32 hashes of recent
     * tool result content to detect near-duplicate injections.
     * FIX MED#7: Added dedup_lens as secondary collision guard — CRC32's
     * 32-bit hash space has high collision rates for structured JSON data.
     * Requiring both hash AND length match reduces false positives. */
  uint32_t dedup_hashes[64]; /* rolling buffer of content hashes */
  uint32_t dedup_lens[64];   /* content length for each hash (collision guard) */
  int dedup_steps[64];       /* step number for each hash */
  int dedup_count;           /* entries in dedup buffer */
  /* Harness-1 §4.2: Tool usage tracking for diversity nudging */
  int tool_use_counts[MAX_TOOL_TRACKED]; /* indexed by tool_registry order */
  int n_tool_uses;                       /* total tool invocations this loop */
  /* Incremental notes tracking: detect deferred synthesis anti-pattern.
     * When the model reads many files without saving findings, compaction
     * evicts the raw content and the model confabulates from degraded memory. */
  int last_notes_step;        /* step when notes() last used (-1 = never) */
  int file_reads_since_notes; /* file_read calls since last notes() */
  int pre_compact_warned;     /* 1 = pre-compaction warning already fired */
  /* v4 unified memory: session index for L3 search via memory_query */
  session_index_t *session_idx;
  /* Journal enforcement: set by tool_journal(), checked by tool_execute().
     * If a handler returns without setting this, tool_execute() adds a
     * fallback journal entry — so no tool call is ever invisible. */
  int journal_done;
  double start_ts; /* tool start time (epoch), set by react.c before tool_execute */
  /* Event callback from parent react loop — threaded through so that
     * child react loops (subtask) can forward events to the TUI. */
  react_event_fn on_event; /* parent's event callback (NULL = headless) */
  void *on_event_data;     /* parent's event userdata */
  /* Lightweight INFORM: track files modified this session so the model
   * knows what it changed without re-reading. Injected into system prompt
   * tail each step; zero context growth (regenerated, not appended). */
#define INFORM_MAX_FILES 32
  struct {
    char *path;      /* heap-allocated, freed at context teardown */
    int last_step;   /* step number of most recent modification */
    int count;       /* total modifications to this file */
  } modified_files[INFORM_MAX_FILES];
  int n_modified_files;
  int last_plan_check_step; /* step when last plan(check) ran; for per-step evidence */
  /* In-memory staleness tracking for plan steps (replaces plan.json mutation).
   * Bit N is set when plan step N+1 has stale evidence (file modified after
   * verification).  Supports up to 64 steps. */
  uint64_t stale_steps;
  /* Edit transaction: tracks save-points for rollback.
   * Auto-opens on first file_edit/file_write, cleared on rollback or
   * at react loop end. Only the FIRST pre-edit hash per path is kept
   * so rollback restores to the true original state. */
#define TXN_MAX_EDITS 64
  struct {
    char *path;      /* file path (heap-allocated) */
    char *pre_hash;  /* content-addressed store hash before edit (NULL = new file) */
    int step;        /* step number when first edit occurred */
    int is_new_file; /* 1 = file_write created this (rollback = delete) */
  } txn_edits[TXN_MAX_EDITS];
  int txn_n_edits;   /* number of entries in txn_edits[] */

  /* Decision observability: prediction tracking for harness evolution.
   * NULL when prediction_tracking is disabled. */
  predict_tracker_t *predict;
} tool_ctx_t;

/* Track a recalled memory key for post-task validation scoring */
void tool_track_recalled_key(tool_ctx_t *ctx, const char *key);

/* Lightweight INFORM: record a file modification for session state tracking.
 * Call after successful file_edit or file_write. */
void tool_track_modified_file(tool_ctx_t *ctx, const char *path, int step);

/* Evidence staleness: check if a modified file invalidates any plan step
 * evidence. Call after tool_track_modified_file() in file_edit/file_write. */
void plan_check_evidence_staleness(tool_ctx_t *ctx, const char *path);

/* Replay journal.jsonl entries for tool="plan" to reconstruct plan state.
 * Returns a cJSON object with "steps" (array) and "active_step" (number),
 * or NULL if no plan exists.  Caller owns the returned object. */
cJSON *plan_replay_journal_dir(const char *session_dir);

/* Derive which parent plan step each subtask was spawned under, by
 * replaying the parent journal (replaces the old parent_link.json).
 * Returns a cJSON object mapping child dir basename (e.g. "subtask_0")
 * to the parent step number, or NULL if the session has no journal.
 * Caller owns the returned object. */
cJSON *plan_subtask_links(const char *session_dir);

/* Look up the parent step a subtask dir is linked to (0 = unlinked). */
int plan_link_for(const cJSON *links, const char *child_name);

/* Collect subtask_N dir names from session_dir, sorted by numeric suffix.
 * Returns an array of xstrdup'd names; *out_n is set to the count.
 * Caller frees each name and the array via plan_subtask_names_free(). */
char **plan_subtask_names(const char *session_dir, int *out_n);

/* Free a plan_subtask_names() result. */
void plan_subtask_names_free(char **names, int n);

/* Render one subtask's plan steps as N.M sub-items under parent_idx.
 * sub_start is the 1-based number of the first sub-item.
 * Returns the number of sub-items rendered (0 if no plan). */
int plan_render_subtask_items(str_t *s, const char *session_dir,
                              const char *child_name,
                              int parent_idx, int sub_start);

/* Append subtask sub-plan lines linked to parent_idx, as indented N.M
 * items in numeric subtask order. */
void plan_append_subtask_steps(str_t *s, const char *session_dir,
                               int parent_idx, const cJSON *links);

/* Append subtask sub-plans NOT linked to any parent step (spawned before
 * a plan existed), as "Subtask N:" blocks in numeric subtask order. */
void plan_append_unlinked_subtasks(str_t *s, const char *session_dir,
                                   const cJSON *links);

/* Edit transaction: record a save-point before modifying a file.
 * Only the FIRST pre-edit hash per path is kept (dedup).
 * is_new_file: 1 = file did not exist before (rollback = delete). */
void tool_txn_record(tool_ctx_t *ctx, const char *path,
                     const char *pre_hash, int is_new_file);

/* Free all txn_edits entries (called at react loop end or after rollback). */
void tool_txn_clear(tool_ctx_t *ctx);

/* Format the INFORM block for injection into the system prompt tail.
 * Returns a heap-allocated string, or NULL if no files were modified.
 * Caller must free. */
char *tool_format_inform_block(tool_ctx_t *ctx);

/* Fire ledger: dedup memory injection within a context window.
 * Resets on compaction so that memories re-arm for the new window. */
int tool_fire_ledger_contains(tool_ctx_t *ctx, const char *key);
void tool_fire_ledger_add(tool_ctx_t *ctx, const char *key);
void tool_fire_ledger_reset(tool_ctx_t *ctx);
void tool_fire_ledger_free(tool_ctx_t *ctx);

/* Scan session_dir for existing R<loop>S<N> symlinks and return the
 * highest sequence number found, or -1 if none exist. */
int alias_scan_max_seq(const char *session_dir, int react_loop);

/* Register a store hash as a step alias, returns alias string like "R1S0".
 * Caller must free the returned string. */
char *tool_register_alias(tool_ctx_t *ctx, const char *hash);

/* Format a ref alias for LLM-facing metadata: "$NASH_SESSION_DIR/R0S5".
 * Writes into buf (must be >= 64 bytes). Returns buf. */
const char *tool_ref_path(const char *alias, char *buf, size_t bufsz);

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
char *tools_system_prompt(const char *session_dir, const char *workspace, int headless); /* caller must free() */

/* FIX CRIT1: Process deferred memory consolidations after task completion.
 * Runs the LLM-based consolidation that was queued during memory_store calls,
 * outside the hot path of the react loop. */
void tool_flush_deferred_consolidations(tool_ctx_t *ctx);

/* FIX CRIT1: Free the deferred consolidation queue (call before tool_ctx cleanup) */
void tool_free_deferred_consolidations(tool_ctx_t *ctx);

/* Tear down auto-started SearXNG container — see searxng.h */

#endif

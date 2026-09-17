/* react_internal.h — Shared between react_*.c files.
 * NOT part of the public API — only react*.c files should include this. */
#ifndef REACT_INTERNAL_H
#define REACT_INTERNAL_H

#include "react.h"
#include "nash_limits.h"
#include "config.h"
#include "memory.h"
#include "workspace.h"
#include "journal.h"
#include "store.h"
#include "nash_log.h"
#include "cJSON.h"
#include "predict.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>

/* ── Eviction Scoring Constants ────────────────────── */
/* Shared between progressive (react_eviction.c) and emergency (react_error.c)
 * scorers.  Change here to keep both in sync. */
#define REACT_SCORE_IMP_WEIGHT 100     /* points per importance tier */
#define REACT_SCORE_REC_WEIGHT 10      /* points per recoverability tier */
#define REACT_SCORE_POS_RANGE 19       /* position normalization range */
#define REACT_SCORE_SIZE_MAX 90        /* max size_bonus (< one imp tier) */
#define REACT_SCORE_SIZE_THRESH 200    /* min msg len for size bonus */
#define REACT_SCORE_SIZE_DIV 500       /* size bonus divisor */
#define REACT_SCORE_SEMANTIC_WEIGHT 40 /* max semantic relevance bonus (< half imp tier) */

/* Truncation limit for message content before embedding (chars).
 * The "topic" is usually in the first 500 chars; embedding the full
 * content of large tool results would waste compute for marginal gain. */
#define REACT_EMBED_TRUNC_CHARS 500

/* ── Eviction Policy ───────────────────────────────── */
/* Computed once at eviction entry from config.  Replaces 20+ scattered
 * #defines with a single struct whose fields are either direct from config
 * or derived via simple formulas.  Five config.toml knobs (under [limits])
 * drive all values:
 *   context_eviction_pct  (trigger threshold, default 70)
 *   eviction_floor_pct    (min retention, default 20)
 *   scratchpad_budget_pct (SP as % of context, default 15)
 *   breadcrumb_budget_pct (combined breadcrumb %, default 5)
 *   compress_min_length   (min msg size for BM25, default 800) */
typedef struct {
  /* Direct from config */
  int trigger_pct;      /* context_eviction_pct (70) */
  int floor_pct;        /* eviction_floor_pct (20) */
  int sp_budget_pct;    /* scratchpad_budget_pct (15) */
  int breadcrumb_pct;   /* breadcrumb_budget_pct (5) */
  int compress_min_len; /* compress_min_length (800) */

  /* Derived (computed once) */
  int target_pct;           /* trigger - trigger/5  = 56 */
  int emergency_target_pct; /* trigger + 10         = 80 */
  int hysteresis_gap;       /* trigger/5, min 5     = 14 */
  int warn_gap;             /* hysteresis/2, min 3  = 7  */

  int sp_max_remaining_pct; /* sp_budget * 8/3      ~40 */
  long sp_min_chars;        /* 2048 (absolute floor) */
  long sp_shrink_min;       /* sp_min / 4           = 512 */
  long sp_fallback;         /* 8192 (when context unknown) */

  int bc_index_pct;        /* breadcrumb * 2/5     = 2 */
  int bc_summary_pct;      /* breadcrumb * 3/5     = 3 */
  int summary_per_msg_min; /* 200 (absolute floor) */
  int summary_per_msg_max; /* per_msg_min * 5      = 1000 */

  int compress_min_chars; /* compress_min_len / 2 = 400 */
  int compress_min_units; /* 4 (always) */

  long floor_min_chars; /* 7000 (absolute floor — ~2000 tokens at 3.5 cpt) */
} eviction_policy_t;

/* Compute policy from config.  Call once at start of react_maybe_evict(). */
eviction_policy_t react_eviction_policy(const config_t *cfg);

/* Maximum total recovery attempts across all error types before giving up.
 * Prevents unbounded retries from alternating error types.
 * Wired up via cycle_window_t.total_recoveries in react.c. */
#define REACT_MAX_TOTAL_RECOVERY 12

/* Sliding-window cycle detection (arXiv 2608.00101)
 * Tracks the last CYCLE_WINDOW_SIZE action signatures to detect
 * repeating patterns beyond simple last-vs-current comparison.
 * Detects: A->A (existing), A->B->A, A->B->C->A, and longer cycles. */
#define CYCLE_WINDOW_SIZE 12  /* last N signatures to retain */
#define CYCLE_MAX_PERIOD 4    /* max cycle length to detect (A->B->C->D->A) */

typedef struct {
  /* Circular buffer of recent action signatures */
  char *sigs[CYCLE_WINDOW_SIZE];     /* heap-allocated sig strings */
  char *results[CYCLE_WINDOW_SIZE];  /* cached result JSON per sig */
  char *refs[CYCLE_WINDOW_SIZE];     /* cached store alias per sig */
  int steps[CYCLE_WINDOW_SIZE];      /* step number for each entry */
  int head;                          /* next write position */
  int count;                         /* entries in buffer (<=CYCLE_WINDOW_SIZE) */

  /* Cycle detection state */
  int cycle_len;          /* detected cycle period (0 = no cycle) */
  int cycle_occurrences;  /* how many times the cycle has repeated */
  int cycle_first_step;   /* step where cycle was first detected */

  /* Compute amplification tracking */
  long tokens_baseline;     /* tokens on first-occurrence steps (no cycling) */
  long tokens_total;        /* total tokens including retried steps */
  int cycling_steps;        /* count of steps classified as cycling */

  /* Total recovery counter (wires up REACT_MAX_TOTAL_RECOVERY) */
  int total_recoveries;     /* sum across all recovery types */
} cycle_window_t;

/* Initialize a cycle window (zero-fill). */
static inline void cycle_window_init(cycle_window_t *cw) {
  memset(cw, 0, sizeof(*cw));
}

/* Free all heap-allocated strings in a cycle window. */
void cycle_window_free(cycle_window_t *cw);

/* Reset cycle window (e.g. after eviction invalidates cached results).
 * Preserves amplification counters across resets. */
void cycle_window_reset(cycle_window_t *cw);

/* Functions in react_cycling.c */
int cycle_window_push(cycle_window_t *cw, const char *sig,
                      const char *result_json, const char *ref, int step);
int cycle_window_find(const cycle_window_t *cw, const char *sig,
                      int skip_slot);
int cycle_window_detect(cycle_window_t *cw);
float cycle_window_amplification(const cycle_window_t *cw);
char *cycle_window_describe(const cycle_window_t *cw);

/* Maximum keep_tail — prevents unbounded tail growth from
 * interleaved user_ask responses shrinking the evictable range. */
#define REACT_KEEP_TAIL_MAX 8

/* ── Eviction Tuning Constants (stable algorithm internals) ──── */
/* Thought/content truncation limit for BM25 query augmentation (chars). */
#define REACT_THOUGHT_TRUNC_LEN 200

/* Compaction hint text injected as MEMORY_HINT after eviction.
 * Extracted to a constant to eliminate 3 copies and the magic-130 estimate.
 * Note: the pre-compaction warning in react.c fires BEFORE eviction to give
 * the model a chance to save findings while file contents are still in
 * context.  This post-compaction hint focuses on recovery (memory_search)
 * and confirms that compaction occurred.  Saving is still mentioned as a
 * last resort for any analysis the model holds but hasn't yet persisted. */
#define EVICT_COMPACT_HINT \
  "[Context compacted. Some file contents have been evicted. " \
  "If you have unsaved analysis, save it to notes() immediately " \
  "\xe2\x80\x94 do NOT try to recall evicted file details from memory. " \
  "Use memory_search to recover lost context.]"

/* Pre-compaction warning text injected BEFORE eviction fires, while file
 * contents are still in context.  Uses warn_gap (derived from trigger_pct)
 * to detect the warning zone — no hardcoded thresholds. */
#define EVICT_PRE_COMPACT_WARN \
  "[URGENT: Context nearing compaction threshold. Save your unsaved " \
  "analysis to notes() NOW with exact file:line references. After " \
  "compaction, evicted file contents CANNOT be recovered from " \
  "memory \xe2\x80\x94 you will confabulate if you try to recall them " \
  "later. Use notes(op=\"append\", section=\"findings\", " \
  "content=\"...\").]"

/* Scratchpad message prefix — shared by react_inject_scratchpad_msg
 * and react_format_scratchpad_msg. */
#define REACT_SP_PREFIX "[SCRATCHPAD]\n"
#define REACT_SP_PREFIX_LEN 13 /* strlen("[SCRATCHPAD]\n") */

/* ── Partner Index ──────────────────────── */

/* Pre-computed partner map for pair-safe eviction.
 * Built once before eviction passes, eliminates repeated O(n) scanning
 * and JSON parsing during eviction. */
typedef struct {
  int *partner; /* partner[i] = partner index for msg i, or -1 */
  int n_msgs;   /* number of messages (for bounds checking) */
} evict_partner_map_t;

/* Extended scoring context for semantic-aware eviction.
 * Wraps the partner map (needed for size accounting) plus pre-computed
 * cosine similarities between each message and the current task.
 * When similarities is NULL, the scorer falls back to the base formula
 * (zero overhead — graceful degradation when embeddings unavailable). */
typedef struct {
  evict_partner_map_t *pmap; /* partner map (never NULL) */
  float *similarities;       /* pre-computed cosine(task, msg[i]) per msg
                                        * indexed by absolute msg index; NULL if
                                        * embeddings unavailable */
  int n_msgs;                /* length of similarities array */
} evict_score_ctx_t;

/* ── Generic Mark-Sweep ────────────────────────────── */

/* Scoring callback for evict_mark_candidates().
 * Called once per evictable message (importance < HIGH).
 * Lower score = evicted first.
 *   chat       — the conversation
 *   mi         — absolute message index
 *   ri         — relative index (mi - evict_start)
 *   n_evictable — total messages in eviction range
 *   userdata   — caller-supplied context (NULL if unused)
 * Must return an integer score. */
typedef int (*evict_score_fn)(const llm_chat_t *chat, int mi, int ri,
                              int n_evictable, void *userdata);

/* ── Helpers shared across react submodules ─────────── */

/* Get chars-per-token ratio from provider config, defaulting to 3.5. */
float react_get_chars_per_token(const react_ctx_t *ctx);

/* Check if this context should abort: own pause OR parent's pause.
 * Subtask children have pause_owner pointing to the parent react_ctx_t.
 * When the user pauses, the parent's pause_requested is set; the child
 * detects it here and enters react_wait_for_redirect on the parent's
 * pause infrastructure, injecting the redirect into its own chat. */
static inline int react_should_abort(const react_ctx_t *ctx) {
  if (atomic_load(&((react_ctx_t *)ctx)->pause_requested)) return 1;
  if (ctx->pause_owner &&
      atomic_load(&((react_ctx_t *)ctx->pause_owner)->pause_requested))
    return 1;
  return 0;
}

/* Compute dynamic keep_head: count of CRITICAL messages at head.
 * Replaces hardcoded REACT_EVICT_KEEP_HEAD=3 that assumed fixed
 * [system, memory_index, pinned] structure. Adapts to actual
 * injection configuration (inject_memory=0 → only 1 head msg). */
int react_compute_keep_head(const llm_chat_t *chat);

/* Compute dynamic keep_tail: count of messages in the last N complete
 * tool-call exchanges at tail. Replaces hardcoded REACT_EVICT_KEEP_TAIL=4
 * that assumed exactly 2 exchange pairs. Adapts to actual tail structure
 * (user_ask, error recovery, multi-tool). Returns at least 2. */
int react_compute_keep_tail(const llm_chat_t *chat);

/* Calculate usage percentage of context budget. Centralizes the repeated
 * pattern: (context_budget > 0) ? (int)(100L * chars / budget) : 0 */
static inline int react_usage_pct(long total_chars, long context_budget) {
  return (context_budget > 0)
           ? (int)(100L * total_chars / context_budget)
           : 0;
}

/* Calculate total chars across all messages in a chat.
 * O(1) — returns cached total_chars maintained incrementally
 * by llm_chat_add, remove, insert, and replace_content. */
static inline long react_calc_total_chars(const llm_chat_t *chat) {
  return chat->total_chars;
}

/* Convenience wrapper — combines calc_total_chars + usage_pct. */
static inline int react_chat_usage_pct(const llm_chat_t *chat, long budget) {
  return react_usage_pct(react_calc_total_chars(chat), budget);
}

/* Compute eviction target_pct from config.
 * Shared between react_maybe_evict and react_emergency_evict_and_reinject
 * to eliminate duplicated hysteresis gap calculation.
 * Now implemented via react_eviction_policy() derivation chain. */
int react_eviction_target_pct(const config_t *cfg);

/* Compute total chars in head (messages before evict_start). */
long react_head_chars(const llm_chat_t *chat, int evict_start);

/* Compute total chars in tail (messages at or after evict_end). */
long react_tail_chars(const llm_chat_t *chat, int evict_end);

/* Shared floor calculation for progressive and emergency eviction.
 * Returns minimum chars that must be retained in the evictable region.
 * If known_head_chars >= 0, uses that value directly to avoid recomputing.
 * Subtracts tail_chars from base - the floor is based on the evictable
 * region capacity, not the entire non-head budget.  Pass -1 for
 * known_tail_chars to auto-compute (requires evict_end).
 * Reads floor_pct and floor_min_chars from eviction_policy_t. */
long react_calc_floor_chars_pol(const llm_chat_t *chat,
                                int evict_start, int evict_end,
                                long context_budget,
                                long known_head_chars,
                                long known_tail_chars,
                                const eviction_policy_t *pol);
/* Compute context_budget in chars from provider config. */
static inline long react_context_budget(const react_ctx_t *ctx) {
  double cpt = (double)react_get_chars_per_token(ctx);
  return (ctx->provider && ctx->provider->cfg.context_size > 0)
           ? (long)(ctx->provider->cfg.context_size * cpt)
           : 0;
}

/* Compute scratchpad budget using the dual-cap policy.
 * Returns min(abs_cap, rel_cap) with a floor of min_budget.
 * Shared by react_reinject_scratchpad(), react_build_context(),
 * and evict_finalize().
 * Reads sp_budget_pct / sp_max_remaining_pct / sp_fallback from policy. */
size_t react_scratchpad_budget_pol(long context_budget,
                                   long current_chars,
                                   size_t min_budget,
                                   const eviction_policy_t *pol);

/* Convenience wrapper using default policy from config. */
size_t react_scratchpad_budget(long context_budget,
                               long current_chars,
                               size_t min_budget);

/* Compute a budget cap = max(budget * pct / 100, min_val).
 * Shared between breadcrumb index and summary cap computations. */
static inline long react_budget_cap(long budget, int pct, long min_val) {
  long cap = budget * pct / 100;
  return cap < min_val ? min_val : cap;
}

/* Format and inject a "[SCRATCHPAD]\n..." message at position pos.
 * Returns the injected message length (0 if nothing injected). */
long react_inject_scratchpad_msg(llm_chat_t *chat, int pos,
                                 const char *sp_content);

/* Format a "[SCRATCHPAD]\n..." string without inserting into chat.
 * Returns malloc'd formatted string, or NULL.  Caller must free().
 * Used by react_error.c tier-2 recovery (replace in-place). */
char *react_format_scratchpad_msg(const char *content);

/* Pair-safe boundary adjustment for eviction ranges.
 * Adjusts evict_start/evict_end so that no tool_call/tool_result pair is
 * split across the boundary.  Modifies *evict_start and *evict_end in place. */
void evict_adjust_boundaries(const llm_chat_t *chat,
                             int *evict_start, int *evict_end);

/* Mark-sweep helper — removes marked messages in reverse order
 * and recovers tool threading.  Returns the number of messages removed.
 * session_dir: if non-NULL, archives evicted message content to
 * session_dir/evicted.jsonl before freeing (full-content resurrection). */
int evict_sweep_marked(llm_chat_t *chat, int evict_start,
                       const int *evict_mark, int n_evictable,
                       const char *session_dir);

/* Generic mark-candidates — scores all evictable messages using
 * a caller-supplied scoring function, sorts by score ascending (lowest =
 * evicted first), and marks candidates for removal while respecting:
 *   - Compaction floor (minimum retained content)
 *   - Partner pairing (tool_call + tool_result evicted together)
 *   - HIGH/CRITICAL importance protection
 *   - Target remaining budget stop condition
 *
 * Parameters:
 *   evict_mark     — pre-zeroed calloc'd array of n_evictable ints
 *   remaining_nonhead — sum of tail + evictable chars (updated internally)
 *   tail_chars        — chars in protected tail (subtracted in floor check)
 *   target_remaining  — stop marking when remaining_nonhead <= this value
 *   score_fn/score_ud — scoring callback + userdata
 *
 * Returns: number of messages marked for eviction. */
int evict_mark_candidates(const llm_chat_t *chat,
                          int evict_start, int evict_end,
                          const evict_partner_map_t *pmap,
                          long floor_chars,
                          long remaining_nonhead,
                          long tail_chars,
                          long target_remaining,
                          evict_score_fn score_fn, void *score_ud,
                          int *evict_mark);

/* Build enriched BM25 query from user_query + recent thoughts + scratchpad.
 * Returns malloc'd string — caller must free. Shared between eviction and
 * context construction to avoid duplicate implementations. */
char *react_build_bm25_query(const llm_chat_t *chat, const char *user_query,
                             scratchpad_t *scratch);

/* Find the partner of a tool_call or tool_result message by scanning.
 * For tool_calls_json messages: scans forward for matching tool_call_id.
 * For tool_call_id messages: scans backward for matching tool_calls_json.
 * Returns partner index within [range_start, range_end), or -1 if none.
 * Matches by scanning (not adjacency) to handle interleaved messages,
 * and validates importance < HIGH before returning. */
int react_find_tool_partner(const llm_chat_t *chat, int msg_idx,
                            int range_start, int range_end);

/* Recover tool_call threading state (last_tool_call_id / last_tool_calls_json)
 * from surviving messages after eviction. Shared between progressive eviction
 * (pass3) and emergency eviction. */
void react_recover_tool_threading(llm_chat_t *chat);

/* Build the full system prompt string. Returns malloc'd string — caller frees.
 * Used for both chat injection and journal logging.
 * Respects ctx->headless (suppresses user_ask rules, adjusts identity) and
 * ctx->custom_system_prompt (append or replace per system_prompt_replace). */
char *react_build_system_prompt(const react_ctx_t *ctx);

/* Add system prompt to chat. */
void react_add_system_prompt(llm_chat_t *chat, const react_ctx_t *ctx);

/* Emit a react event (NULL-safe). */
void react_emit(react_event_fn fn, void *ud, react_event_t *ev);

/* Streaming token callback context */
typedef struct {
  react_event_fn on_event;
  void *userdata;
  int step;
  int react_loop;
} react_stream_ctx_t;

/* Streaming token callback — bridges llm_token_fn to react_event_fn */
void react_stream_token_cb(const char *token, void *userdata);

/* Progress callback — bridges provider_progress_fn to react_event_fn */
void react_progress_cb(int processed, int total, void *userdata);

/* Recursively unwrap nested JSON in the "thought" field. */
void react_sanitize_thought(cJSON *action);

/* Extract key display parameter for a tool action */
const char *react_get_action_desc(cJSON *action, const char *action_name,
                                  const char *thought);

/* Extract usable text from LLM output (JSON tool-call or markdown). */
char *react_extract_llm_text_output(const char *raw);

/* Log memory context injection to journal for debugging. */
void react_log_memory_context(tool_ctx_t *tools, int react_loop, int step,
                              const char *mem_index,
                              const char *pinned,
                              memory_results_t *all_memories,
                              const char *query);

/* Inject recall context: temporal calendar, episodic recall, type-specific
 * memory recall (skills/lessons/strategies/antipatterns), and associative
 * graph walk.  Shared between react_build_context() and
 * react_checkpoint_restore() for structurally identical context.
 * mem_summary/pinned are borrowed for logging only (not freed). */
void react_inject_recall_context(llm_chat_t *chat, react_ctx_t *ctx,
                                 const char *user_query,
                                 const char *mem_summary,
                                 const char *pinned);

/* ── Checkpoint ──────────────────────────────────────── */

/* Restore conversation from checkpoint. Returns resume step, or -1 if none. */
int react_checkpoint_restore(react_ctx_t *ctx, llm_chat_t *chat,
                             const char *user_query,
                             react_event_fn on_event, void *userdata);

/* Save checkpoint after each tool execution (atomic write). */
void react_checkpoint_save(react_ctx_t *ctx, int step, const char *user_query,
                           const char *last_tc_id);

/* Remove checkpoint on completion. */
void react_checkpoint_remove(react_ctx_t *ctx);

/* ── Context Construction ────────────────────────────── */

/* Build initial context: system prompt, memory, scratchpad, previous result, query.
 * Called once at the start of react_run() when not restoring from checkpoint. */
void react_build_context(react_ctx_t *ctx, llm_chat_t *chat,
                         const char *user_query,
                         react_event_fn on_event, void *userdata);

/* Inject memory index + pinned knowledge into chat.
 * Used by react_build_context() and react_checkpoint_restore().
 * Returns mem_summary and pinned via output params for logging (caller frees).
 * Pass NULL for output params if not needed. */
void react_inject_memory_and_pinned(llm_chat_t *chat, tool_ctx_t *tools,
                                    char **out_mem_summary, char **out_pinned);

/* Scan CWD for workspace policy files (CLAUDE.md, AGENT.md, SKILL.md,
 * .cursorrules, etc.) and inject their content as a single
 * LLM_MSG_WORKSPACE_POLICY message.  No-op if no files found. */
void react_inject_workspace_policy(llm_chat_t *chat);

/* ── Error Recovery ──────────────────────────────────── */

/* Emergency eviction — proportionally removes oldest evictable messages
 * to reach target_pct of context budget. context_budget is in chars (0 = unknown,
 * falls back to target_pct of current usage). Returns count evicted.
 * target_pct: 0 = use pol.emergency_target_pct default (80%).
 * Accepts target_pct so callers can match the configured eviction_pct
 * (preventing immediate re-trigger).  Targets budget, not current usage.
 * Pair-safe — removes tool_call/tool_result pairs together. */
int react_emergency_evict(llm_chat_t *chat, long context_budget, int target_pct,
                          const config_t *cfg, const char *session_dir);

/* Shared emergency breadcrumb + scratchpad injection.
 * Injects breadcrumb summary + MEMORY_HINT + scratchpad (budget-guarded).
 * Used by evict_finalize strategy-2 and react_emergency_evict_and_reinject.
 * skip_sp: when true, suppress scratchpad re-injection (used when Strategy 1
 * already stripped SP and re-injecting would defeat the strip). */
void react_inject_emergency_breadcrumbs(react_ctx_t *ctx, llm_chat_t *chat,
                                        int n_evicted, long context_budget,
                                        int target_pct, int skip_sp);

/* Emergency evict + scratchpad re-injection helper.
 * Combines react_emergency_evict + react_reinject_scratchpad into one call.
 * Returns number of messages evicted (0 if none). */
int react_emergency_evict_and_reinject(react_ctx_t *ctx, llm_chat_t *chat);

/* Handle NULL response from LLM (HTTP 400/500/auth errors).
 * Returns: 0 = continue (retry), 1 = break (give up).
 * Modifies chat in-place for recovery. */
int react_handle_null_response(react_ctx_t *ctx, llm_chat_t *chat,
                               int *consecutive_null, int *total_400,
                               llm_stats_t *stats, int step,
                               react_event_fn on_event, void *userdata);

/* ── Context Eviction ────────────────────────────────── */

/* Check context usage and evict old messages if over threshold.
 * Implements mark-then-sweep eviction  with unified finalization
 * , partner index , and per-pass targets . */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata);

/* Re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive and emergency eviction. */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos);

/* Build a partner index mapping each tool_call message to its
 * result and vice versa. Built once before eviction, used by all phases.
 * Returns a heap-allocated map. Caller must call evict_free_partner_map(). */
evict_partner_map_t evict_build_partner_map(const llm_chat_t *chat,
                                            int range_start, int range_end);
void evict_free_partner_map(evict_partner_map_t *map);

/* Unified post-eviction finalization.  Re-injects scratchpad,
 * breadcrumbs, and compaction hint in a single pass.  Verifies budget.
 * breadcrumb_str ownership is CONSUMED (freed) by this function.
 * Caller must not use breadcrumb_str after calling evict_finalize().
 * n_evicted tracks actual eviction count (not inferred from breadcrumb_str).
 * Returns the number of messages emergency-evicted by Strategy 2
 * (0 if Strategy 2 didn't fire). */
int evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str /* consumed */, int n_evicted,
                   int step,
                   react_event_fn on_event, void *userdata);

/* ── Step 3: Progressive scoring callback (exposed for testing) ────── */

int evict_score_progressive(const llm_chat_t *chat, int mi, int ri,
                            int n_evictable, void *userdata);

/* Semantic-aware progressive scoring callback.
 * Adds a semantic relevance bonus (0..REACT_SCORE_SEMANTIC_WEIGHT) from
 * pre-computed cosine similarities stored in evict_score_ctx_t.
 * Falls back to base formula when similarities array is NULL. */
int evict_score_progressive_semantic(const llm_chat_t *chat, int mi, int ri,
                                     int n_evictable, void *userdata);

/* ── Step 2.5: Tool Lifecycle — Stale/Superseded Read Detection ────── */

/* Pichay [arXiv:2603.09023]: Replace stale/superseded file_read results
 * with compact paging handles. Runs before the mark phase. */
void evict_lifecycle_stale_reads(llm_chat_t *chat,
                                 int evict_start, int evict_end);

/* ── Step 4.5: Type-Aware Pre-Compression ────── */

/* CWL [arXiv:2606.11213] + Complexity Trap [arXiv:2508.21433]: Structure-aware
 * compression for specific tool output types (shell_exec, glob_search,
 * grep_search). Runs after sweep, before BM25 compression.
 * Returns number of messages compressed. */
int evict_type_compress(llm_chat_t *chat, int keep_head, int keep_tail,
                        int compress_min_len);

/* ── Post-Loop (Reflection, Promotion, Pruning) ────── */

/* Run post-loop phases: validation scoring, reflection, promotion, pruning. */
void react_post_loop(react_ctx_t *ctx, const char *user_query,
                     const char *final_result, int task_succeeded,
                     react_event_fn on_event, void *userdata);

/* ── Plan-Then-Shed: Preamble Degradation ────── */

/* After plan() executes, preamble injections have informed the plan and are
 * now dead weight. Walk chat messages and downgrade preamble types from
 * NORMAL to LOW so they shed first on the next eviction pass.
 * Pinned knowledge stays HIGH - it's pinned for a reason. */
void react_degrade_preamble(llm_chat_t *chat);

#endif /* REACT_INTERNAL_H */

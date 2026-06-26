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
#include "tools_registry.h"
#include "cJSON.h"
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
 * scorers.  Change here to keep both in sync — see Review Issue #1. */
#define REACT_SCORE_IMP_WEIGHT      100  /* points per importance tier */
#define REACT_SCORE_REC_WEIGHT      10   /* points per recoverability tier */
#define REACT_SCORE_POS_RANGE       19   /* position normalization range */
#define REACT_SCORE_SIZE_MAX        90   /* max size_bonus (< one imp tier) */
#define REACT_SCORE_SIZE_THRESH     200  /* min msg len for size bonus */
#define REACT_SCORE_SIZE_DIV        500  /* size bonus divisor */

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
    int trigger_pct;          /* context_eviction_pct (70) */
    int floor_pct;            /* eviction_floor_pct (20) */
    int sp_budget_pct;        /* scratchpad_budget_pct (15) */
    int breadcrumb_pct;       /* breadcrumb_budget_pct (5) */
    int compress_min_len;     /* compress_min_length (800) */

    /* Derived (computed once) */
    int target_pct;           /* trigger - trigger/5  = 56 */
    int emergency_target_pct; /* trigger + 10         = 80 */
    int hysteresis_gap;       /* trigger/5, min 5     = 14 */
    int warn_gap;             /* hysteresis/2, min 3  = 7  */

    int sp_max_remaining_pct; /* sp_budget * 8/3      ≈ 40 */
    long sp_min_chars;        /* 2048 (absolute floor) */
    long sp_shrink_min;       /* sp_min / 4           = 512 */
    long sp_fallback;         /* 8192 (when context unknown) */

    int bc_index_pct;         /* breadcrumb * 2/5     = 2 */
    int bc_summary_pct;       /* breadcrumb * 3/5     = 3 */
    int summary_per_msg_min;  /* 200 (absolute floor) */
    int summary_per_msg_max;  /* per_msg_min * 5      = 1000 */

    int compress_min_chars;   /* compress_min_len / 2 = 400 */
    int compress_min_units;   /* 4 (always) */

    long floor_min_chars;     /* 7000 (absolute floor — ~2000 tokens at 3.5 cpt) */
} eviction_policy_t;

/* Compute policy from config.  Call once at start of react_maybe_evict(). */
static inline eviction_policy_t react_eviction_policy(const config_t *cfg) {
    eviction_policy_t p = {0};
    p.trigger_pct       = cfg ? cfg->context_eviction_pct : 70;
    p.floor_pct         = cfg ? cfg->eviction_floor_pct : 20;
    p.sp_budget_pct     = cfg ? cfg->scratchpad_budget_pct : 15;
    p.breadcrumb_pct    = cfg ? cfg->breadcrumb_budget_pct : 5;
    p.compress_min_len  = cfg ? cfg->compress_min_length : 800;

    /* Hysteresis */
    p.hysteresis_gap    = p.trigger_pct / 5;
    if (p.hysteresis_gap < 5) p.hysteresis_gap = 5;
    p.target_pct        = p.trigger_pct - p.hysteresis_gap;
    p.warn_gap          = p.hysteresis_gap / 2;
    if (p.warn_gap < 3) p.warn_gap = 3;
    p.emergency_target_pct = p.trigger_pct + 10;
    if (p.emergency_target_pct > 95) p.emergency_target_pct = 95;

    /* Scratchpad budget chain */
    p.sp_max_remaining_pct = p.sp_budget_pct * 8 / 3;  /* ≈ 2.67× */
    p.sp_min_chars      = 2048;
    p.sp_shrink_min     = p.sp_min_chars / 4;           /* 512 */
    p.sp_fallback       = 8192;

    /* Breadcrumb budget chain */
    p.bc_index_pct      = p.breadcrumb_pct * 2 / 5;
    p.bc_summary_pct    = p.breadcrumb_pct - p.bc_index_pct;
    p.summary_per_msg_min = 200;
    p.summary_per_msg_max = p.summary_per_msg_min * 5;  /* 1000 */

    /* Compression chain */
    p.compress_min_chars = p.compress_min_len / 2;      /* 400 */
    p.compress_min_units = 4;

    p.floor_min_chars    = 7000;  /* ~2000 tokens at 3.5 cpt — anti-pattern minimum */
    return p;
}

/* Maximum total recovery attempts across all error types before giving up.
 * Prevents unbounded retries from alternating error types (D3 fix). */
#define REACT_MAX_TOTAL_RECOVERY    12

/* L1 FIX: Maximum keep_tail to prevent unbounded tail growth from
 * interleaved user_ask responses shrinking the evictable range. */
#define REACT_KEEP_TAIL_MAX          8

/* ── Eviction Tuning Constants (stable algorithm internals) ──── */
/* Thought/content truncation limit for BM25 query augmentation (chars). */
#define REACT_THOUGHT_TRUNC_LEN     200

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

/* FIX #11: Scratchpad message prefix — eliminates duplicate string literals
 * in react_inject_scratchpad_msg and react_format_scratchpad_msg. */
#define REACT_SP_PREFIX     "[SCRATCHPAD]\n"
#define REACT_SP_PREFIX_LEN 13  /* strlen("[SCRATCHPAD]\n") */

/* ── Proposal E: Partner Index ──────────────────────── */

/* Pre-computed partner map for pair-safe eviction.
 * Built once before eviction passes, eliminates repeated O(n) scanning
 * and JSON parsing during eviction. */
typedef struct {
    int *partner;     /* partner[i] = partner index for msg i, or -1 */
    int  n_msgs;      /* number of messages (for bounds checking) */
} evict_partner_map_t;

/* ── Generic Mark-Sweep (Review B4) ────────────────── */

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
        ? (int)(100L * total_chars / context_budget) : 0;
}

/* Calculate total chars across all messages in a chat.
 * Review B1/C4: Now O(1) — returns cached total_chars maintained incrementally
 * by llm_chat_add, remove, insert, and replace_content. Previously O(n) with
 * 17 call sites causing thousands of redundant strlen() calls. */
static inline long react_calc_total_chars(const llm_chat_t *chat) {
    return chat->total_chars;
}

/* FIX #12: Convenience wrapper — combines calc_total_chars + usage_pct.
 * This 2-function composition appeared 8+ times across eviction files. */
static inline int react_chat_usage_pct(const llm_chat_t *chat, long budget) {
    return react_usage_pct(react_calc_total_chars(chat), budget);
}

/* Compute eviction target_pct from config.
 * Shared between react_maybe_evict and react_emergency_evict_and_reinject
 * to eliminate duplicated hysteresis gap calculation.
 * Now implemented via react_eviction_policy() derivation chain. */
static inline int react_eviction_target_pct(const config_t *cfg) {
    eviction_policy_t pol = react_eviction_policy(cfg);
    return pol.target_pct;
}

/* FIX #11: Compute total chars in head (messages before evict_start).
 * Eliminates 3 copies of the same loop. */
static inline long react_head_chars(const llm_chat_t *chat, int evict_start) {
    long hc = 0;
    for (int i = 0; i < evict_start && i < chat->n_msgs; i++)
        hc += (long)chat->msgs[i].content_len;
    return hc;
}

/* FIX #11b: Compute total chars in tail (messages at or after evict_end). */
static inline long react_tail_chars(const llm_chat_t *chat, int evict_end) {
    long tc = 0;
    for (int i = evict_end; i < chat->n_msgs; i++)
        tc += (long)chat->msgs[i].content_len;
    return tc;
}

/* FIX FLAW 4: Shared floor calculation used by both progressive eviction (pass3)
 * and emergency eviction. Eliminates duplication and ensures consistency.
 * Returns minimum chars that must be retained in the evictable region.
 * If known_head_chars >= 0, uses that value directly to avoid recomputing.
 * FIX #5: Subtracts tail_chars from base — the floor should be based on the
 * evictable region capacity, not the entire non-head budget. Pass -1 for
 * known_tail_chars to auto-compute (requires evict_end).
 * Now reads floor_pct and floor_min_chars from eviction_policy_t. */
static inline long react_calc_floor_chars_pol(const llm_chat_t *chat,
                                              int evict_start, int evict_end,
                                              long context_budget,
                                              long known_head_chars,
                                              long known_tail_chars,
                                              const eviction_policy_t *pol) {
    long head_chars = (known_head_chars >= 0)
        ? known_head_chars
        : react_head_chars(chat, evict_start);
    long tail_chars = (known_tail_chars >= 0)
        ? known_tail_chars
        : react_tail_chars(chat, evict_end);
    long base = (context_budget > 0)
        ? context_budget - head_chars - tail_chars
        : react_calc_total_chars(chat) - head_chars - tail_chars;
    long floor = base * pol->floor_pct / 100;
    return floor < pol->floor_min_chars ? pol->floor_min_chars : floor;
}
/* BUG 5 FIX: Removed dead react_calc_floor_chars() convenience wrapper that
 * called react_eviction_policy(NULL), using hardcoded defaults instead of
 * user config. All callers use react_calc_floor_chars_pol() directly. */

/* Compute context_budget in chars from provider config. */
static inline long react_context_budget(const react_ctx_t *ctx) {
    double cpt = (double)react_get_chars_per_token(ctx);
    return (ctx->provider && ctx->provider->cfg.context_size > 0)
        ? (long)(ctx->provider->cfg.context_size * cpt) : 0;
}

/* D1+S5 FIX: Compute scratchpad budget using the dual-cap policy.
 * Returns min(abs_cap, rel_cap) with a floor of min_budget.
 * Shared between react_reinject_scratchpad(), react_build_context(),
 * and evict_finalize() to eliminate 3 copies of the same logic.
 * Now reads sp_budget_pct / sp_max_remaining_pct / sp_fallback from policy. */
static inline size_t react_scratchpad_budget_pol(long context_budget,
                                                 long current_chars,
                                                 size_t min_budget,
                                                 const eviction_policy_t *pol) {
    if (context_budget <= 0)
        return (size_t)pol->sp_fallback;
    size_t abs_cap = (size_t)(context_budget
                              * pol->sp_budget_pct / 100);
    long remaining = context_budget - current_chars;
    if (remaining < 0) remaining = 0;
    size_t rel_cap = (size_t)(remaining
                              * pol->sp_max_remaining_pct / 100);
    size_t budget = abs_cap < rel_cap ? abs_cap : rel_cap;
    return budget < min_budget ? min_budget : budget;
}
/* Convenience wrapper using default policy from config. */
static inline size_t react_scratchpad_budget(long context_budget,
                                              long current_chars,
                                              size_t min_budget) {
    eviction_policy_t pol = react_eviction_policy(NULL);
    return react_scratchpad_budget_pol(context_budget, current_chars,
                                       min_budget, &pol);
}

/* SIMP1 FIX: Compute a budget cap = max(budget * pct / 100, min_val).
 * Shared between breadcrumb index and summary cap computations. */
static inline long react_budget_cap(long budget, int pct, long min_val) {
    long cap = budget * pct / 100;
    return cap < min_val ? min_val : cap;
}

/* D1 FIX: Format and inject a "[SCRATCHPAD]\n..." message at position pos.
 * Eliminates 4 copies of the alloc + snprintf("[SCRATCHPAD]\n%s") + insert
 * pattern across react_eviction.c, react_context.c, and react_error.c.
 * FIX #11: Uses REACT_SP_PREFIX constant instead of duplicate string literal.
 * Returns the injected message length (0 if nothing injected). */
static inline long react_inject_scratchpad_msg(llm_chat_t *chat, int pos,
                                                const char *sp_content) {
    if (!sp_content || !sp_content[0]) return 0;
    size_t slen = strlen(sp_content);
    size_t total = REACT_SP_PREFIX_LEN + slen + 1;
    char *sp_msg = malloc(total);
    if (!sp_msg) return 0;
    snprintf(sp_msg, total, "%s%s", REACT_SP_PREFIX, sp_content);
    llm_chat_insert_typed(chat, pos, "user", sp_msg, LLM_MSG_SCRATCHPAD);
    long injected = (long)(total - 1);
    free(sp_msg);
    return injected;
}

/* B5 FIX: Format a "[SCRATCHPAD]\n..." string without inserting.
 * Returns malloc'd formatted string, or NULL. Caller must free().
 * Used by react_error.c tier-2 recovery (replace in-place, can't use
 * react_inject_scratchpad_msg which does insert).
 * FIX #11: Uses REACT_SP_PREFIX constant. */
static inline char *react_format_scratchpad_msg(const char *content) {
    if (!content || !content[0]) return NULL;
    size_t clen = strlen(content);
    size_t total = REACT_SP_PREFIX_LEN + clen + 1;
    char *msg = malloc(total);
    if (!msg) return NULL;
    snprintf(msg, total, "%s%s", REACT_SP_PREFIX, content);
    return msg;
}

/* F2/DD1 FIX: Pair-safe boundary adjustment for eviction ranges.
 * Adjusts evict_start/evict_end so that no tool_call/tool_result pair is
 * split across the boundary. Shared between progressive and emergency eviction.
 * Modifies *evict_start and *evict_end in place. */
static inline void evict_adjust_boundaries(const llm_chat_t *chat,
                                            int *evict_start, int *evict_end) {
    /* Adjust evict_end: pull back if a tool_result sits just outside
     * the range (its tool_call partner would be inside) or if a tool_call
     * sits at the boundary edge (its result would be outside). */
    while (*evict_end > *evict_start + 1) {
        if (*evict_end < chat->n_msgs &&
            chat->msgs[*evict_end].tool_call_id) {
            (*evict_end)--;
            continue;
        }
        if (*evict_end - 1 >= *evict_start &&
            chat->msgs[*evict_end - 1].tool_calls_json) {
            (*evict_end)--;
            continue;
        }
        break;
    }
    /* Adjust evict_start: advance past orphaned tool_results whose
     * tool_call partner is in the protected keep_head zone. */
    while (*evict_start < *evict_end &&
           chat->msgs[*evict_start].tool_call_id &&
           (*evict_start == 0 ||
            !chat->msgs[*evict_start - 1].tool_calls_json))
        (*evict_start)++;
}

/* D3 FIX: Shared mark-sweep helper — removes marked messages in reverse order
 * and recovers tool threading. Used by both progressive and emergency eviction.
 * Returns the number of messages actually removed. */
int evict_sweep_marked(llm_chat_t *chat, int evict_start,
                       const int *evict_mark, int n_evictable);

/* Review B4: Generic mark-candidates — scores all evictable messages using
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
 * FIX: Matches by scanning (not adjacency) to handle interleaved messages,
 * and validates importance < HIGH before returning. */
int react_find_tool_partner(const llm_chat_t *chat, int msg_idx,
                            int range_start, int range_end);

/* Recover tool_call threading state (last_tool_call_id / last_tool_calls_json)
 * from surviving messages after eviction. Shared between progressive eviction
 * (pass3) and emergency eviction for consistency (FIX B4). */
void react_recover_tool_threading(llm_chat_t *chat);

/* Safe JSON string accessor */
const char *react_json_get_str(cJSON *obj, const char *key);

/* Build the full system prompt string. Returns malloc'd string — caller frees.
 * Used for both chat injection and journal logging (Fix #12). */
char *react_build_system_prompt(const config_t *cfg, const char *session_dir);

/* Add system prompt to chat, appending model-specific rules if configured. */
void react_add_system_prompt(llm_chat_t *chat, const config_t *cfg,
                             const char *session_dir);

/* Emit a react event (NULL-safe). */
void react_emit(react_event_fn fn, void *ud, react_event_t *ev);

/* Streaming token callback context */
typedef struct {
    react_event_fn on_event;
    void          *userdata;
    int            step;
    int            react_loop;
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

/* FIX #9: Inject memory index + pinned knowledge into chat.
 * Used by react_build_context() and react_checkpoint_restore().
 * Returns mem_summary and pinned via output params for logging (caller frees).
 * Pass NULL for output params if not needed. */
void react_inject_memory_and_pinned(llm_chat_t *chat, tool_ctx_t *tools,
                                     char **out_mem_summary, char **out_pinned);

/* ── Error Recovery ──────────────────────────────────── */

/* Emergency eviction — proportionally removes oldest evictable messages
 * to reach target_pct of context budget. context_budget is in chars (0 = unknown,
 * falls back to target_pct of current usage). Returns count evicted.
 * target_pct: 0 = use pol.emergency_target_pct default (80%).
 * Flaw 2 FIX: Accepts target so callers can pass a value consistent with
 * the configured eviction_pct, preventing immediate re-trigger.
 * FIX #3: Takes budget param so it targets budget, not current usage.
 * FIX #7: Pair-safe — removes tool_call/tool_result pairs together. */
int react_emergency_evict(llm_chat_t *chat, long context_budget, int target_pct,
                          const config_t *cfg);

/* Shared emergency breadcrumb + scratchpad injection.
 * Injects breadcrumb summary + MEMORY_HINT + scratchpad (budget-guarded).
 * Used by evict_finalize strategy-2 and react_emergency_evict_and_reinject.
 * skip_sp: when true, suppress scratchpad re-injection (used when Strategy 1
 * already stripped SP and re-injecting would defeat the strip). */
void react_inject_emergency_breadcrumbs(react_ctx_t *ctx, llm_chat_t *chat,
                                         int n_evicted, long context_budget,
                                         int target_pct, int skip_sp);

/* D3 FIX: Emergency evict + scratchpad re-injection helper.
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
 * Implements mark-then-sweep eviction (Proposal B) with unified finalization
 * (Proposal A), partner index (Proposal E), and per-pass targets (Proposal D). */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata);

/* Re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive and emergency eviction (BUG A/C FIX). */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos);

/* Proposal E: Build a partner index mapping each tool_call message to its
 * result and vice versa. Built once before eviction, used by all phases.
 * Returns a heap-allocated map. Caller must call evict_free_partner_map(). */
evict_partner_map_t evict_build_partner_map(const llm_chat_t *chat,
                                             int range_start, int range_end);
void evict_free_partner_map(evict_partner_map_t *map);

/* Proposal A: Unified post-eviction finalization. Re-injects scratchpad,
 * breadcrumbs, and compaction hint in a single pass. Verifies budget.
 * FIX #5: breadcrumb_str ownership is CONSUMED (freed) by this function.
 * Caller must not use breadcrumb_str after calling evict_finalize().
 * FIX #1: n_evicted tracks actual eviction count — eviction status is no
 * longer inferred from breadcrumb_str being non-NULL (which fails when all
 * evicted messages are system-role or empty-content).
 * BUG2+3 FIX: Returns the number of messages emergency-evicted by Strategy 2
 * (0 if Strategy 2 didn't fire). Callers use this for journal + event tracking. */
int evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str /* consumed */, int n_evicted,
                   int step,
                   react_event_fn on_event, void *userdata);

/* ── Step 3: Progressive scoring callback (exposed for testing) ────── */

int evict_score_progressive(const llm_chat_t *chat, int mi, int ri,
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
 * Pinned knowledge stays HIGH — it's pinned for a reason. */
static inline void react_degrade_preamble(llm_chat_t *chat) {
    for (int i = 0; i < chat->n_msgs; i++) {
        switch (chat->msgs[i].msg_type) {
            case LLM_MSG_MEMORY_INDEX:
            case LLM_MSG_TEMPORAL:
            case LLM_MSG_EPISODIC:
            case LLM_MSG_SKILLS:
            case LLM_MSG_LESSONS:
            case LLM_MSG_STRATEGIES:
            case LLM_MSG_ANTIPATTERNS:
                chat->msgs[i].importance = LLM_MSG_IMPORTANCE_LOW;
                break;
            default:
                break;
        }
    }
}

#endif /* REACT_INTERNAL_H */

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

/* ── Eviction Constants ─────────────────────────────── */
/* Scratchpad budget as percentage of total context size. */
#define REACT_SCRATCHPAD_BUDGET_PCT  15
/* Maximum percentage of post-eviction content that scratchpad may occupy.
 * Prevents scratchpad from drowning out conversation after heavy eviction. */
#define REACT_SCRATCHPAD_MAX_OF_REMAINING_PCT  40
/* Compaction floor: minimum retained context as fraction of non-head budget.
 * Prevents over-eviction death spiral (context-eviction-cliff feedback loop). */
#define REACT_EVICT_FLOOR_PCT       20
#define REACT_EVICT_FLOOR_MIN_CHARS 4000
/* Emergency eviction target as percentage of context budget. */
#define REACT_EMERGENCY_TARGET_PCT  80
/* Minimum hysteresis gap in percentage points between trigger and target. */
#define REACT_HYSTERESIS_MIN_GAP    5
/* Hysteresis gap divisor: gap = eviction_pct / REACT_HYSTERESIS_DIVISOR. */
#define REACT_HYSTERESIS_DIVISOR    5
/* Maximum total recovery attempts across all error types before giving up.
 * Prevents unbounded retries from alternating error types (D3 fix). */
#define REACT_MAX_TOTAL_RECOVERY    12

/* ── Eviction Tuning Constants (formerly inline magic numbers) ──── */
/* Thought/content truncation limit for BM25 query augmentation (chars). */
#define REACT_THOUGHT_TRUNC_LEN     200
/* FIX #7: REACT_SP_BM25_BUDGET moved to react.c (only user).
 * REACT_BREADCRUMB_CAP_MIN, REACT_REINJECT_PAD, REACT_EFF_TARGET_MIN_PCT
 * moved to react_eviction.c (only user). */
/* Minimum scratchpad budget (chars). */
#define REACT_SP_MIN                2048
/* Fallback scratchpad budget when context_size unknown (chars). */
#define REACT_SP_FALLBACK           8192
/* FIX #7: Eviction-only constants (REACT_SCORE_*, REACT_BREADCRUMB_*,
 * REACT_SUMMARY_*, REACT_SP_SHRINK_MIN, REACT_COMPRESS_*) moved to
 * react_eviction.c — the only file that uses them. */

/* Compaction hint text injected as MEMORY_HINT after eviction.
 * Extracted to a constant to eliminate 3 copies and the magic-130 estimate. */
#define EVICT_COMPACT_HINT \
    "[Context compacted. Use memory_recall to recover lost " \
    "context \xe2\x80\x94 it searches both stored knowledge and past " \
    "session history.]"

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

/* FIX #11: Compute total chars in head (messages before evict_start).
 * Eliminates 3 copies of the same loop. */
static inline long react_head_chars(const llm_chat_t *chat, int evict_start) {
    long hc = 0;
    for (int i = 0; i < evict_start && i < chat->n_msgs; i++)
        hc += (long)chat->msgs[i].content_len;
    return hc;
}

/* FIX FLAW 4: Shared floor calculation used by both progressive eviction (pass3)
 * and emergency eviction. Eliminates duplication and ensures consistency.
 * Returns minimum chars that must be retained in the evictable region.
 * If known_head_chars >= 0, uses that value directly to avoid recomputing. */
static inline long react_calc_floor_chars(const llm_chat_t *chat,
                                          int evict_start,
                                          long context_budget,
                                          long known_head_chars) {
    /* FIX #11: Use react_head_chars helper instead of inline loop */
    long head_chars = (known_head_chars >= 0)
        ? known_head_chars
        : react_head_chars(chat, evict_start);
    long base = (context_budget > 0)
        ? context_budget - head_chars
        : react_calc_total_chars(chat) - head_chars;
    long floor = base * REACT_EVICT_FLOOR_PCT / 100;
    return floor < REACT_EVICT_FLOOR_MIN_CHARS ? REACT_EVICT_FLOOR_MIN_CHARS : floor;
}

/* Compute context_budget in chars from provider config. */
static inline long react_context_budget(const react_ctx_t *ctx) {
    double cpt = (double)react_get_chars_per_token(ctx);
    return (ctx->provider && ctx->provider->cfg.context_size > 0)
        ? (long)(ctx->provider->cfg.context_size * cpt) : 0;
}

/* D1+S5 FIX: Compute scratchpad budget using the dual-cap policy.
 * Returns min(abs_cap, rel_cap) with a floor of min_budget.
 * Shared between react_reinject_scratchpad(), react_build_context(),
 * and evict_finalize() to eliminate 3 copies of the same logic. */
static inline size_t react_scratchpad_budget(long context_budget,
                                              long current_chars,
                                              size_t min_budget) {
    if (context_budget <= 0)
        return REACT_SP_FALLBACK;
    size_t abs_cap = (size_t)(context_budget
                              * REACT_SCRATCHPAD_BUDGET_PCT / 100);
    long remaining = context_budget - current_chars;
    if (remaining < 0) remaining = 0;
    size_t rel_cap = (size_t)(remaining
                              * REACT_SCRATCHPAD_MAX_OF_REMAINING_PCT / 100);
    size_t budget = abs_cap < rel_cap ? abs_cap : rel_cap;
    return budget < min_budget ? min_budget : budget;
}

/* D1 FIX: Format and inject a "[SCRATCHPAD]\n..." message at position pos.
 * Eliminates 4 copies of the alloc + snprintf("[SCRATCHPAD]\n%s") + insert
 * pattern across react_eviction.c, react_context.c, and react_error.c.
 * Returns the injected message length (0 if nothing injected). */
static inline long react_inject_scratchpad_msg(llm_chat_t *chat, int pos,
                                                const char *sp_content) {
    if (!sp_content || !sp_content[0]) return 0;
    size_t slen = strlen(sp_content);
    char *sp_msg = malloc(slen + 32);
    if (!sp_msg) return 0;
    snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", sp_content);
    llm_chat_insert_typed(chat, pos, "user", sp_msg, LLM_MSG_SCRATCHPAD);
    long injected = (long)strlen(sp_msg);
    free(sp_msg);
    return injected;
}

/* B5 FIX: Format a "[SCRATCHPAD]\n..." string without inserting.
 * Returns malloc'd formatted string, or NULL. Caller must free().
 * Used by react_error.c tier-2 recovery (replace in-place, can't use
 * react_inject_scratchpad_msg which does insert). */
static inline char *react_format_scratchpad_msg(const char *content) {
    if (!content || !content[0]) return NULL;
    size_t clen = strlen(content);
    char *msg = malloc(clen + 32);
    if (!msg) return NULL;
    snprintf(msg, clen + 32, "[SCRATCHPAD]\n%s", content);
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
 *   target_remaining  — stop marking when remaining_nonhead <= this value
 *   score_fn/score_ud — scoring callback + userdata
 *
 * Returns: number of messages marked for eviction. */
int evict_mark_candidates(const llm_chat_t *chat,
                          int evict_start, int evict_end,
                          const evict_partner_map_t *pmap,
                          long floor_chars,
                          long remaining_nonhead,
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
char *react_build_system_prompt(const config_t *cfg);

/* Add system prompt to chat, appending model-specific rules if configured. */
void react_add_system_prompt(llm_chat_t *chat, const config_t *cfg);

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

/* ── Error Recovery ──────────────────────────────────── */

/* Emergency eviction — proportionally removes oldest evictable messages
 * to reach target_pct of context budget. context_budget is in chars (0 = unknown,
 * falls back to target_pct of current usage). Returns count evicted.
 * target_pct: 0 = use REACT_EMERGENCY_TARGET_PCT default (80%).
 * Flaw 2 FIX: Accepts target so callers can pass a value consistent with
 * the configured eviction_pct, preventing immediate re-trigger.
 * FIX #3: Takes budget param so it targets budget, not current usage.
 * FIX #7: Pair-safe — removes tool_call/tool_result pairs together. */
int react_emergency_evict(llm_chat_t *chat, long context_budget, int target_pct);

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
 * Caller must not use breadcrumb_str after calling evict_finalize(). */
void evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str /* consumed */,
                   int step,
                   react_event_fn on_event, void *userdata);

/* ── Post-Loop (Reflection, Promotion, Pruning) ────── */

/* Run post-loop phases: validation scoring, reflection, promotion, pruning. */
void react_post_loop(react_ctx_t *ctx, const char *user_query,
                     const char *final_result, int task_succeeded,
                     react_event_fn on_event, void *userdata);

#endif /* REACT_INTERNAL_H */

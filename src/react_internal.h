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
/* Scratchpad preview budget for BM25 query augmentation (chars). */
#define REACT_SP_BM25_BUDGET        500
/* Default and minimum breadcrumb capacity (chars). */
#define REACT_BREADCRUMB_CAP_MIN    1024
/* FIX FLAW 7: Breadcrumb budget as a direct percentage of context budget.
 * Replaces confusing formula (SCRATCHPAD_BUDGET_PCT / CAP_DIV = 15/300 = 5%). */
#define REACT_BREADCRUMB_BUDGET_PCT 5
/* Padding added to re-injection estimate (chars). */
#define REACT_REINJECT_PAD          200
/* Minimum effective target percentage (prevents target going to 0). */
#define REACT_EFF_TARGET_MIN_PCT    10
/* Minimum scratchpad budget (chars). */
#define REACT_SP_MIN                2048
/* Fallback scratchpad budget when context_size unknown (chars). */
#define REACT_SP_FALLBACK           8192
/* Compress-scaled parameters: minimum chunks and chars. */
#define REACT_COMPRESS_MIN_UNITS    4
#define REACT_COMPRESS_MIN_CHARS    400
/* Score formula coefficients for Pass 3 eviction scoring. */
#define REACT_SCORE_IMP_WEIGHT      100  /* points per importance tier */
#define REACT_SCORE_REC_WEIGHT      10   /* points per recoverability tier */
#define REACT_SCORE_POS_RANGE       19   /* position normalization range */
#define REACT_SCORE_SIZE_MAX        90   /* max size_bonus (< one imp tier) */
#define REACT_SCORE_SIZE_THRESH     200  /* min msg len for size bonus */
#define REACT_SCORE_SIZE_DIV        500  /* size bonus divisor */
/* Breadcrumb brief preview truncation (chars). */
#define REACT_BREADCRUMB_BRIEF_LEN  80
/* Minimum tool content length to include in eviction summary. */
#define REACT_SUMMARY_TOOL_MIN_LEN  50
/* Per-message summary min/max chars. */
#define REACT_SUMMARY_PER_MSG_MIN   200
#define REACT_SUMMARY_PER_MSG_MAX   1000
/* Minimum scratchpad chars for proportional shrink (below this, strip entirely). */
#define REACT_SP_SHRINK_MIN         512
/* FIX FLAW 5: Separate budgets for breadcrumb index and eviction summary.
 * Previously both shared REACT_BREADCRUMB_BUDGET_PCT, causing unpredictable
 * allocation depending on which messages had store aliases. */
#define REACT_BREADCRUMB_INDEX_PCT  2  /* % of context budget for store-alias index */
#define REACT_BREADCRUMB_SUMMARY_PCT 3 /* % of context budget for eviction summary */
/* FIX FLAW 8: Fixed minimum compress threshold instead of average-based.
 * Messages below this size yield negligible savings from BM25 compression. */
#define REACT_COMPRESS_THRESH_FIXED 800

/* ── Proposal E: Partner Index ──────────────────────── */

/* Pre-computed partner map for pair-safe eviction.
 * Built once before eviction passes, eliminates repeated O(n) scanning
 * and JSON parsing during eviction. */
typedef struct {
    int *partner;     /* partner[i] = partner index for msg i, or -1 */
    int  n_msgs;      /* number of messages (for bounds checking) */
} evict_partner_map_t;

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

/* Calculate total chars across all messages in a chat. */
static inline long react_calc_total_chars(const llm_chat_t *chat) {
    long total = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            total += (long)strlen(chat->msgs[i].content);
    return total;
}

/* FIX FLAW 4: Shared floor calculation used by both progressive eviction (pass3)
 * and emergency eviction. Eliminates duplication and ensures consistency.
 * Returns minimum chars that must be retained in the evictable region.
 * If known_head_chars >= 0, uses that value directly to avoid recomputing. */
static inline long react_calc_floor_chars(const llm_chat_t *chat,
                                          int evict_start,
                                          long context_budget,
                                          long known_head_chars) {
    long head_chars = known_head_chars;
    if (head_chars < 0) {
        head_chars = 0;
        for (int i = 0; i < evict_start && i < chat->n_msgs; i++)
            if (chat->msgs[i].content)
                head_chars += (long)strlen(chat->msgs[i].content);
    }
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

/* D3 FIX: Shared mark-sweep helper — removes marked messages in reverse order
 * and recovers tool threading. Used by both progressive and emergency eviction.
 * Returns the number of messages actually removed. */
int evict_sweep_marked(llm_chat_t *chat, int evict_start,
                       const int *evict_mark, int n_evictable);

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
 * breadcrumbs, and compaction hint in a single pass. Verifies budget. */
void evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str, int step,
                   react_event_fn on_event, void *userdata);

/* ── Post-Loop (Reflection, Promotion, Pruning) ────── */

/* Run post-loop phases: validation scoring, reflection, promotion, pruning. */
void react_post_loop(react_ctx_t *ctx, const char *user_query,
                     const char *final_result, int task_succeeded,
                     react_event_fn on_event, void *userdata);

#endif /* REACT_INTERNAL_H */

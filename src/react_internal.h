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
/* Number of messages at HEAD of conversation to always keep during eviction.
 * Protects: [0] system prompt, [1] memory index, [2] pinned knowledge.
 * These are CRITICAL messages that anchor the agent's identity and memory. */
#define REACT_EVICT_KEEP_HEAD  3
/* Number of messages at TAIL of conversation to always keep during eviction.
 * Protects the most recent 2 exchange pairs (assistant+tool_result × 2).
 * Ensures the agent always sees its latest actions and their results. */
#define REACT_EVICT_KEEP_TAIL  4
/* Scratchpad budget as percentage of total context size. */
#define REACT_SCRATCHPAD_BUDGET_PCT  15

/* ── Helpers shared across react submodules ─────────── */

/* Get chars-per-token ratio from provider config, defaulting to 3.5. */
float react_get_chars_per_token(const react_ctx_t *ctx);

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
 * to reach ~80% of context budget. context_budget is in chars (0 = unknown,
 * falls back to 80% of current usage). Returns count evicted.
 * FIX #3: Takes budget param so it targets budget, not current usage.
 * FIX #7: Pair-safe — removes tool_call/tool_result pairs together. */
int react_emergency_evict(llm_chat_t *chat, long context_budget);

/* Handle NULL response from LLM (HTTP 400/500/auth errors).
 * Returns: 0 = continue (retry), 1 = break (give up).
 * Modifies chat in-place for recovery. */
int react_handle_null_response(react_ctx_t *ctx, llm_chat_t *chat,
                               int *consecutive_null, int *total_400,
                               llm_stats_t *stats, int step,
                               react_event_fn on_event, void *userdata);

/* ── Context Eviction ────────────────────────────────── */

/* Check context usage and evict old messages if over threshold.
 * Includes importance-aware multi-pass eviction, pair-safe boundaries,
 * scratchpad re-injection, and breadcrumb generation (LCM-Lite). */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata);

/* Re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive and emergency eviction (BUG A/C FIX). */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos);

/* ── Post-Loop (Reflection, Promotion, Pruning) ────── */

/* Run post-loop phases: validation scoring, reflection, promotion, pruning. */
void react_post_loop(react_ctx_t *ctx, const char *user_query,
                     const char *final_result, int task_succeeded,
                     react_event_fn on_event, void *userdata);

#endif /* REACT_INTERNAL_H */

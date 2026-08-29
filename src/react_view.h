/* react_view.h - Working-view construction for the react loop.
 *
 * Implements the half-life working-view rule from Lewis (arXiv:2608.26218):
 * separates the complete execution record from the model's working view.
 * Older tool results are progressively shortened by age tier while keeping
 * the full record intact for eviction, dedup, and journal logging.
 *
 * The view is an ephemeral shallow-copy chat where only shortened messages
 * have new content buffers. The caller must free with view_free(). */

#ifndef REACT_VIEW_H
#define REACT_VIEW_H

#include "llm.h"

/* Policy controlling how the working view is constructed. */
typedef struct {
  int enabled;         /* master switch (0 = pass-through, no view) */
  int keep_full;       /* newest N tool results stay at full length (default: 4) */
  int base_cap;        /* max chars for first tier of shortened results (default: 4000) */
  int min_cap;         /* floor cap - never shorten below this (default: 200) */
  int activation_pct;  /* only activate when prompt > this % of budget (default: 50) */
  long context_budget; /* total context budget in chars (from provider) */
} view_policy_t;

/* Statistics from the last view_build call. */
typedef struct {
  long record_chars;    /* total chars in the full record */
  long view_chars;      /* total chars in the constructed view */
  int msgs_shortened;   /* number of messages that were shortened */
  int activated;        /* 1 if shortening was active, 0 if below threshold */
} view_stats_t;

/* Build a working view from the full chat record.
 *
 * Returns a new llm_chat_t with:
 *   - All messages from record (shallow copy of pointers)
 *   - Old tool results shortened per the half-life policy
 *   - Only shortened messages have newly allocated content buffers
 *
 * Returns NULL if policy->enabled is false or no shortening is needed.
 * When NULL is returned, the caller should pass the original chat.
 *
 * If stats is non-NULL, it is filled with compression metrics.
 * Caller must free the returned chat with view_free(). */
llm_chat_t *view_build(const llm_chat_t *record, const view_policy_t *policy,
                        view_stats_t *stats);

/* Free a view returned by view_build.
 * Only frees content buffers for messages that were shortened (allocated
 * by view_build). Does NOT free content owned by the original record.
 * Safe to call with NULL. */
void view_free(llm_chat_t *view);

/* Initialize a view_policy_t with sensible defaults. */
static inline view_policy_t view_policy_defaults(void) {
  return (view_policy_t){
    .enabled = 1,
    .keep_full = 4,
    .base_cap = 4000,
    .min_cap = 200,
    .activation_pct = 50,
    .context_budget = 0
  };
}

#endif /* REACT_VIEW_H */

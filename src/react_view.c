/* react_view.c - Working-view construction (Lewis half-life rule).
 *
 * Builds an ephemeral shallow-copy chat from the full execution record,
 * progressively shortening older tool results by age tier. The full
 * record is never modified - only the view contains shortened content.
 *
 * Half-life schedule (Lewis, arXiv:2608.26218 Section 2):
 *   - Newest keep_full tool results stay at full length
 *   - For older results: tier = floor(log2(age / keep_full))
 *   - cap = base_cap >> tier (halves per tier)
 *   - cap = max(cap, min_cap) (floor to prevent zero-length)
 *   - Shortened: first cap/2 chars + omission marker + last cap/2 chars
 *
 * Activation: only shortens when total record chars exceed
 * activation_pct/100 of context_budget. Below that threshold,
 * returns NULL (caller uses original chat).
 *
 * Memory layout: view->msgs is a combined allocation containing
 * n llm_msg_t structs followed by n char* original-content pointers.
 * view_free uses the original pointers to identify which content
 * buffers were allocated by view_build (shortened != original). */

#include "react_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Access the original-content pointer array stored after msgs[]. */
static inline char **view_orig_content(const llm_chat_t *view) {
  return (char **)((char *)view->msgs + sizeof(llm_msg_t) * (size_t)view->n_msgs);
}

/* ---- Internal: shorten a single content string ---- */

static char *shorten_content(const char *content, size_t content_len,
                             int cap, const char *store_alias) {
  if ((int)content_len <= cap)
    return NULL; /* no shortening needed */

  int half = cap / 2;
  if (half < 50) half = 50;

  char marker[256];
  int omitted = (int)content_len - 2 * half;
  if (omitted < 0) omitted = 0;

  if (store_alias && store_alias[0]) {
    snprintf(marker, sizeof(marker),
             "\n[... %d chars omitted - full content in $NASH_SESSION_DIR/%s ...]\n",
             omitted, store_alias);
  } else {
    snprintf(marker, sizeof(marker),
             "\n[... %d chars omitted ...]\n", omitted);
  }

  size_t marker_len = strlen(marker);
  size_t new_len = (size_t)half + marker_len + (size_t)half;
  char *shortened = malloc(new_len + 1);
  if (!shortened)
    return NULL;

  memcpy(shortened, content, (size_t)half);
  memcpy(shortened + half, marker, marker_len);
  memcpy(shortened + half + marker_len, content + content_len - half, (size_t)half);
  shortened[new_len] = '\0';

  return shortened;
}

/* ---- Public API ---- */

llm_chat_t *view_build(const llm_chat_t *record, const view_policy_t *policy,
                        view_stats_t *stats) {
  if (stats)
    memset(stats, 0, sizeof(*stats));

  if (!record || !policy || !policy->enabled || record->n_msgs == 0) {
    if (stats) {
      stats->record_chars = record ? record->total_chars : 0;
      stats->view_chars = stats->record_chars;
    }
    return NULL;
  }

  long record_chars = record->total_chars;
  if (stats)
    stats->record_chars = record_chars;

  /* Check activation threshold */
  if (policy->context_budget > 0) {
    int usage_pct = (int)(100L * record_chars / policy->context_budget);
    if (usage_pct < policy->activation_pct) {
      if (stats) {
        stats->view_chars = record_chars;
        stats->activated = 0;
      }
      return NULL;
    }
  }

  /* Count tool-result messages from tail, assigning age indices.
   * age 0 = newest tool result, age 1 = second newest, etc. */
  int n = record->n_msgs;
  int *tool_age = calloc((size_t)n, sizeof(int));
  if (!tool_age) return NULL;

  for (int i = 0; i < n; i++)
    tool_age[i] = -1;

  int tool_count = 0;
  for (int i = n - 1; i >= 0; i--) {
    if (record->msgs[i].msg_type == LLM_MSG_TOOL_RESULT) {
      tool_age[i] = tool_count++;
    }
  }

  int keep_full = policy->keep_full > 0 ? policy->keep_full : 4;
  if (tool_count <= keep_full) {
    free(tool_age);
    if (stats) {
      stats->view_chars = record_chars;
      stats->activated = 0;
    }
    return NULL;
  }

  /* Allocate the view chat struct */
  llm_chat_t *view = calloc(1, sizeof(llm_chat_t));
  if (!view) {
    free(tool_age);
    return NULL;
  }

  /* Combined allocation: n llm_msg_t structs + n char* original pointers.
   * This lets view_free identify which content buffers to free. */
  size_t msgs_bytes = sizeof(llm_msg_t) * (size_t)n;
  size_t orig_bytes = sizeof(char *) * (size_t)n;
  view->msgs = malloc(msgs_bytes + orig_bytes);
  if (!view->msgs) {
    free(view);
    free(tool_age);
    return NULL;
  }
  view->n_msgs = n;
  view->cap_msgs = n;

  char **orig = view_orig_content(view);

  int msgs_shortened = 0;
  long view_chars = 0;
  int base_cap = policy->base_cap > 0 ? policy->base_cap : 4000;
  int min_cap = policy->min_cap > 0 ? policy->min_cap : 200;

  for (int i = 0; i < n; i++) {
    /* Shallow copy the entire message struct */
    view->msgs[i] = record->msgs[i];
    orig[i] = record->msgs[i].content;

    int age = tool_age[i];
    if (age >= keep_full && record->msgs[i].content_len > 0) {
      /* tier = floor(log2(age / keep_full)) */
      int tier = 0;
      int age_over = age / keep_full;
      if (age_over > 1)
        tier = (int)floor(log2((double)age_over));
      int cap = base_cap >> tier;
      if (cap < min_cap) cap = min_cap;

      char *shortened = shorten_content(
        record->msgs[i].content,
        record->msgs[i].content_len,
        cap,
        record->msgs[i].store_alias
      );

      if (shortened) {
        view->msgs[i].content = shortened;
        view->msgs[i].content_len = strlen(shortened);
        msgs_shortened++;
      }
    }

    view_chars += (long)view->msgs[i].content_len;
  }

  view->total_chars = view_chars;
  view->last_tool_call_id = record->last_tool_call_id;
  view->last_tool_calls_json = record->last_tool_calls_json;
  view->multi_tool_count = record->multi_tool_count;

  free(tool_age);

  if (stats) {
    stats->view_chars = view_chars;
    stats->msgs_shortened = msgs_shortened;
    stats->activated = 1;
  }

  if (msgs_shortened == 0) {
    free(view->msgs);
    free(view);
    if (stats)
      stats->activated = 0;
    return NULL;
  }

  return view;
}

void view_free(llm_chat_t *view) {
  if (!view) return;

  /* Free only content buffers allocated by view_build.
   * The original content pointers are stored after the msgs array.
   * If view->msgs[i].content != orig[i], we allocated it. */
  char **orig = view_orig_content(view);
  for (int i = 0; i < view->n_msgs; i++) {
    if (view->msgs[i].content != orig[i])
      free(view->msgs[i].content);
  }

  free(view->msgs); /* frees the combined msgs + orig block */
  free(view);
}

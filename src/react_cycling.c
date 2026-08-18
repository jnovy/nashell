/* react_cycling.c - Sliding-window retry loop detection.
 * arXiv 2608.00101: 9% of agent turns trigger retry loops
 * causing up to 4x compute amplification.
 *
 * Detects cycles of period 1..CYCLE_MAX_PERIOD in a sliding window
 * of the last CYCLE_WINDOW_SIZE action signatures. */

#include "react_internal.h"

/* Push a new signature into the window. Returns the slot index used. */
int cycle_window_push(cycle_window_t *cw, const char *sig,
                      const char *result_json, const char *ref,
                      int step) {
  int slot = cw->head;
  free(cw->sigs[slot]);
  free(cw->results[slot]);
  free(cw->refs[slot]);
  cw->sigs[slot] = sig ? xstrdup(sig) : NULL;
  cw->results[slot] = result_json ? xstrdup(result_json) : NULL;
  cw->refs[slot] = ref ? xstrdup(ref) : NULL;
  cw->steps[slot] = step;
  cw->head = (cw->head + 1) % CYCLE_WINDOW_SIZE;
  if (cw->count < CYCLE_WINDOW_SIZE) cw->count++;
  return slot;
}

/* Look up the most recent entry matching `sig` in the window.
 * Returns the slot index, or -1 if not found.
 * Skips the entry at `skip_slot` (the one we just pushed). */
int cycle_window_find(const cycle_window_t *cw, const char *sig,
                      int skip_slot) {
  if (!sig) return -1;
  for (int i = 0; i < cw->count; i++) {
    int idx = (cw->head - 1 - i + CYCLE_WINDOW_SIZE) % CYCLE_WINDOW_SIZE;
    if (idx == skip_slot) continue;
    if (cw->sigs[idx] && strcmp(cw->sigs[idx], sig) == 0)
      return idx;
  }
  return -1;
}

/* Detect cycles of period 1..CYCLE_MAX_PERIOD in the window.
 *
 * A cycle of period P means the last P signatures match the P
 * signatures before them. e.g. for P=2: [..,A,B,A,B] is a cycle.
 *
 * Returns the cycle period (1..CYCLE_MAX_PERIOD) or 0 if no cycle.
 * On detection, sets cw->cycle_len and cw->cycle_occurrences. */
int cycle_window_detect(cycle_window_t *cw) {
  if (cw->count < 2) return 0;

  for (int period = 1; period <= CYCLE_MAX_PERIOD &&
                       period * 2 <= cw->count; period++) {
    /* Check if the last `period` sigs match the `period` sigs before them */
    int match = 1;
    for (int j = 0; j < period && match; j++) {
      int cur = (cw->head - 1 - j + CYCLE_WINDOW_SIZE) % CYCLE_WINDOW_SIZE;
      int prev = (cw->head - 1 - j - period + CYCLE_WINDOW_SIZE)
                 % CYCLE_WINDOW_SIZE;
      if (!cw->sigs[cur] || !cw->sigs[prev] ||
          strcmp(cw->sigs[cur], cw->sigs[prev]) != 0)
        match = 0;
    }
    if (!match) continue;

    /* Count how many times this cycle repeats in the window */
    int reps = 2; /* we matched at least 2 repetitions */
    for (int r = 2; (r + 1) * period <= cw->count; r++) {
      int still_match = 1;
      for (int j = 0; j < period && still_match; j++) {
        int cur = (cw->head - 1 - j + CYCLE_WINDOW_SIZE) % CYCLE_WINDOW_SIZE;
        int back = (cw->head - 1 - j - r * period + CYCLE_WINDOW_SIZE)
                   % CYCLE_WINDOW_SIZE;
        if (!cw->sigs[cur] || !cw->sigs[back] ||
            strcmp(cw->sigs[cur], cw->sigs[back]) != 0)
          still_match = 0;
      }
      if (still_match)
        reps++;
      else
        break;
    }

    cw->cycle_len = period;
    cw->cycle_occurrences = reps;
    return period;
  }

  cw->cycle_len = 0;
  cw->cycle_occurrences = 0;
  return 0;
}

/* Compute amplification ratio: total_tokens / baseline_tokens.
 * Returns 1.0 if no baseline or no cycling occurred. */
float cycle_window_amplification(const cycle_window_t *cw) {
  if (cw->tokens_baseline <= 0) return 1.0f;
  return (float)cw->tokens_total / (float)cw->tokens_baseline;
}

/* Format a human-readable cycle description for injection into context.
 * Returns heap-allocated string; caller must free.
 * Returns NULL if no cycle detected.
 * Example: "Cycle detected (period=2, 3 repetitions): steps 5,6,7,8,9,10" */
char *cycle_window_describe(const cycle_window_t *cw) {
  if (cw->cycle_len == 0) return NULL;
  char buf[1024];
  int pos = 0;
  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                  "Cycle detected (period=%d, %d repetitions): steps ",
                  cw->cycle_len, cw->cycle_occurrences);
  /* List the step numbers involved (oldest first) */
  int n_steps = cw->cycle_len * cw->cycle_occurrences;
  if (n_steps > cw->count) n_steps = cw->count;
  for (int i = n_steps - 1; i >= 0; i--) {
    int idx = (cw->head - 1 - i + CYCLE_WINDOW_SIZE) % CYCLE_WINDOW_SIZE;
    if (i < n_steps - 1)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d",
                    cw->steps[idx]);
  }
  return xstrdup(buf);
}

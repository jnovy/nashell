/* scan_cycles.c - Scan real session journals for cycle patterns.
 * Reads journal.jsonl files, builds action signatures from tool+params,
 * and runs cycle_window_detect() to find historical cycles.
 *
 * Build: cc -O2 -I src -o tools/scan_cycles tools/scan_cycles.c \
 *        src/react_cycling.c src/str.c src/cJSON.c -lm
 * Usage: ./tools/scan_cycles ~/.nash/sessions/SESSION/journal.jsonl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "str.h"

/* We only need the cycle_window_t struct and functions.
 * Pull in the minimal defines directly rather than react_internal.h
 * which drags in too many dependencies. */
#define CYCLE_WINDOW_SIZE 12
#define CYCLE_MAX_PERIOD 4

typedef struct {
  char *sigs[CYCLE_WINDOW_SIZE];
  char *results[CYCLE_WINDOW_SIZE];
  char *refs[CYCLE_WINDOW_SIZE];
  int steps[CYCLE_WINDOW_SIZE];
  int head;
  int count;
  int cycle_len;
  int cycle_occurrences;
  int cycle_first_step;
  long tokens_baseline;
  long tokens_total;
  int cycling_steps;
  int total_recoveries;
} cycle_window_t;

static inline void cycle_window_init(cycle_window_t *cw) {
  memset(cw, 0, sizeof(*cw));
}
static inline void cycle_window_free(cycle_window_t *cw) {
  for (int i = 0; i < CYCLE_WINDOW_SIZE; i++) {
    free(cw->sigs[i]);
    free(cw->results[i]);
    free(cw->refs[i]);
  }
  memset(cw, 0, sizeof(*cw));
}

/* Forward declarations - implemented in react_cycling.c but we
 * redefine the functions here to avoid full react_internal.h */
int cycle_window_push(cycle_window_t *cw, const char *sig,
                      const char *result_json, const char *ref, int step);
int cycle_window_find(const cycle_window_t *cw, const char *sig,
                      int skip_slot);
int cycle_window_detect(cycle_window_t *cw);
char *cycle_window_describe(const cycle_window_t *cw);

/* Build a simplified signature from a journal entry.
 * Format: "tool:path:pattern:command" (whatever params are present) */
static char *build_sig(cJSON *entry) {
  cJSON *tool = cJSON_GetObjectItem(entry, "tool");
  cJSON *params = cJSON_GetObjectItem(entry, "params");
  if (!tool || !tool->valuestring) return NULL;

  const char *tname = tool->valuestring;

  /* Skip non-tool entries */
  if (strcmp(tname, "log") == 0) return NULL;
  if (strcmp(tname, "cycling_cached") == 0) return NULL;
  if (strcmp(tname, "cycling_refused") == 0) return NULL;
  if (strcmp(tname, "cycling_escalated") == 0) return NULL;
  if (strcmp(tname, "memory_refresh") == 0) return NULL;
  if (strcmp(tname, "device_control") == 0) return NULL;

  char sig[2048];
  int pos = 0;
  pos += snprintf(sig + pos, sizeof(sig) - (size_t)pos, "%s:", tname);

  if (params) {
    /* Extract key identifying params */
    const char *fields[] = {"path", "command", "pattern", "query",
                            "url", "key", "op", "section", NULL};
    for (int i = 0; fields[i]; i++) {
      cJSON *f = cJSON_GetObjectItem(params, fields[i]);
      if (f && f->valuestring) {
        /* Truncate long values */
        pos += snprintf(sig + pos, sizeof(sig) - (size_t)pos,
                        "%.*s:", 100, f->valuestring);
      }
    }
    /* start_line / end_line */
    cJSON *sl = cJSON_GetObjectItem(params, "start_line");
    cJSON *el = cJSON_GetObjectItem(params, "end_line");
    if (sl && cJSON_IsNumber(sl))
      pos += snprintf(sig + pos, sizeof(sig) - (size_t)pos,
                      "s%d:", (int)sl->valuedouble);
    if (el && cJSON_IsNumber(el))
      pos += snprintf(sig + pos, sizeof(sig) - (size_t)pos,
                      "e%d:", (int)el->valuedouble);
  }

  return strdup(sig);
}

static void scan_journal(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Cannot open: %s\n", path);
    return;
  }

  /* Extract session ID from path */
  const char *session_id = path;
  const char *p = strrchr(path, '/');
  if (p) {
    /* Go back one more '/' to get the session dir name */
    const char *q = p;
    while (q > path && *(q-1) != '/') q--;
    session_id = q;
  }

  cycle_window_t cw;
  cycle_window_init(&cw);

  int total_tools = 0;
  int total_cycles = 0;
  int max_period = 0;
  int max_reps = 0;

  char line[65536];
  while (fgets(line, sizeof(line), f)) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) continue;

    cJSON *step_j = cJSON_GetObjectItem(entry, "step");
    int step = step_j && cJSON_IsNumber(step_j) ? (int)step_j->valuedouble : 0;

    char *sig = build_sig(entry);
    if (sig) {
      total_tools++;
      int slot = cycle_window_push(&cw, sig, NULL, NULL, step);
      int is_repeat = (cycle_window_find(&cw, sig, slot) >= 0);
      if (is_repeat) {
        int period = cycle_window_detect(&cw);
        if (period > 0) {
          total_cycles++;
          if (period > max_period) max_period = period;
          if (cw.cycle_occurrences > max_reps) max_reps = cw.cycle_occurrences;

          /* Report significant cycles (period >= 2 or many repetitions) */
          if (period >= 2 || cw.cycle_occurrences >= 3) {
            char *desc = cycle_window_describe(&cw);
            printf("  CYCLE: period=%d reps=%d at step %d sig=%.60s%s\n",
                   period, cw.cycle_occurrences, step,
                   sig, strlen(sig) > 60 ? "..." : "");
            if (desc) {
              printf("         %s\n", desc);
              free(desc);
            }
          }
        }
      }
      free(sig);
    }
    cJSON_Delete(entry);
  }
  fclose(f);

  /* Summary for this session */
  if (total_cycles > 0) {
    /* Trim path for display */
    char short_path[256];
    snprintf(short_path, sizeof(short_path), "%s", path);
    /* Find session dir component */
    const char *sessions = strstr(path, "sessions/");
    if (sessions) {
      const char *sd = sessions + 9; /* skip "sessions/" */
      const char *slash = strchr(sd, '/');
      if (slash) {
        char sid[64];
        int slen = (int)(slash - sd);
        if (slen > 63) slen = 63;
        snprintf(sid, sizeof(sid), "%.*s", slen, sd);
        printf("SESSION %s: %d tools, %d cycle detections, "
               "max_period=%d, max_reps=%d\n",
               sid, total_tools, total_cycles, max_period, max_reps);
      }
    }
  }

  cycle_window_free(&cw);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <journal.jsonl> [journal.jsonl ...]\n", argv[0]);
    fprintf(stderr, "  e.g.: %s ~/.nash/sessions/*/journal.jsonl\n", argv[0]);
    return 1;
  }

  int sessions_with_cycles = 0;
  int total_sessions = 0;

  for (int i = 1; i < argc; i++) {
    total_sessions++;
    /* Capture stdout to detect if cycles were printed */
    scan_journal(argv[i]);
  }

  printf("\n=== Scanned %d session journals ===\n", total_sessions);
  return 0;
}

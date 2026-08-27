/* goal.c - Structured goal state management (Recuris WM Phase 1).
 *
 * Implements hierarchical goal tracking with enforced state machine,
 * JSON persistence, and scratchpad projection.
 */

#include "goal.h"
#include "str.h"
#include "nash_limits.h"
#include "nash_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Status string conversion ---- */

static const char *status_names[] = {
  "pending", "active", "blocked", "done", "failed"
};

const char *goal_status_str(goal_status_t s) {
  if (s >= 0 && s <= GOAL_FAILED) return status_names[s];
  return "unknown";
}

goal_status_t goal_status_from_str(const char *s) {
  if (!s) return GOAL_PENDING;
  for (int i = 0; i <= GOAL_FAILED; i++) {
    if (strcmp(s, status_names[i]) == 0) return (goal_status_t)i;
  }
  return GOAL_PENDING;
}

/* ---- Lifecycle ---- */

void goal_state_init(goal_state_t *gs) {
  memset(gs, 0, sizeof(*gs));
  gs->cap = 16;
  gs->goals = calloc((size_t)gs->cap, sizeof(goal_t));
  gs->next_id = 1;
}

static void goal_free_fields(goal_t *g) {
  free(g->content);
  free(g->evidence);
  free(g->blocker);
  g->content = NULL;
  g->evidence = NULL;
  g->blocker = NULL;
}

void goal_state_free(goal_state_t *gs) {
  if (!gs) return;
  for (int i = 0; i < gs->count; i++)
    goal_free_fields(&gs->goals[i]);
  free(gs->goals);
  gs->goals = NULL;
  gs->count = 0;
  gs->cap = 0;
}

/* ---- Find ---- */

goal_t *goal_find(goal_state_t *gs, int id) {
  for (int i = 0; i < gs->count; i++) {
    if (gs->goals[i].id == id) return &gs->goals[i];
  }
  return NULL;
}

/* ---- Add ---- */

int goal_add(goal_state_t *gs, const char *content, int parent_id,
             int step, double ts) {
  if (!content || !content[0]) return -1;

  /* Validate parent exists if specified */
  if (parent_id > 0 && !goal_find(gs, parent_id)) return -1;

  /* Grow array if needed */
  if (gs->count >= gs->cap) {
    int new_cap = gs->cap * 2;
    goal_t *new_goals = realloc(gs->goals, (size_t)new_cap * sizeof(goal_t));
    if (!new_goals) return -1;
    gs->goals = new_goals;
    gs->cap = new_cap;
  }

  goal_t *g = &gs->goals[gs->count];
  memset(g, 0, sizeof(*g));
  g->id = gs->next_id++;
  g->parent_id = parent_id;
  g->content = strdup(content);
  g->status = GOAL_PENDING;
  g->created_ts = ts;
  g->step_created = step;
  g->step_resolved = -1;
  gs->count++;

  return g->id;
}

/* ---- State transitions ---- */

int goal_activate(goal_t *g, int step, double ts) {
  if (!g) return -1;
  /* Allow: PENDING -> ACTIVE, BLOCKED -> ACTIVE (unblock alias) */
  if (g->status != GOAL_PENDING && g->status != GOAL_BLOCKED) return -1;
  g->status = GOAL_ACTIVE;
  (void)step; (void)ts;
  return 0;
}

int goal_block(goal_t *g, const char *blocker, int step, double ts) {
  if (!g || !blocker || !blocker[0]) return -1;
  if (g->status != GOAL_ACTIVE) return -1;
  g->status = GOAL_BLOCKED;
  free(g->blocker);
  g->blocker = strdup(blocker);
  (void)step; (void)ts;
  return 0;
}

int goal_unblock(goal_t *g, int step, double ts) {
  if (!g) return -1;
  if (g->status != GOAL_BLOCKED) return -1;
  g->status = GOAL_ACTIVE;
  free(g->blocker);
  g->blocker = NULL;
  (void)step; (void)ts;
  return 0;
}

int goal_done(goal_t *g, const char *evidence, int step, double ts) {
  if (!g || !evidence || !evidence[0]) return -1;
  /* Allow done from ACTIVE or PENDING (for simple tasks) */
  if (g->status != GOAL_ACTIVE && g->status != GOAL_PENDING) return -1;
  g->status = GOAL_DONE;
  free(g->evidence);
  g->evidence = strdup(evidence);
  g->resolved_ts = ts;
  g->step_resolved = step;
  return 0;
}

int goal_fail(goal_t *g, int step, double ts) {
  if (!g) return -1;
  /* Can fail from any non-terminal state */
  if (g->status == GOAL_DONE || g->status == GOAL_FAILED) return -1;
  g->status = GOAL_FAILED;
  g->resolved_ts = ts;
  g->step_resolved = step;
  return 0;
}

/* ---- JSON serialization ---- */

cJSON *goal_state_to_json(const goal_state_t *gs) {
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();

  for (int i = 0; i < gs->count; i++) {
    const goal_t *g = &gs->goals[i];
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", g->id);
    cJSON_AddNumberToObject(item, "parent_id", g->parent_id);
    cJSON_AddStringToObject(item, "content", g->content ? g->content : "");
    cJSON_AddStringToObject(item, "status", goal_status_str(g->status));
    if (g->evidence)
      cJSON_AddStringToObject(item, "evidence", g->evidence);
    if (g->blocker)
      cJSON_AddStringToObject(item, "blocker", g->blocker);
    cJSON_AddNumberToObject(item, "created_ts", g->created_ts);
    cJSON_AddNumberToObject(item, "resolved_ts", g->resolved_ts);
    cJSON_AddNumberToObject(item, "step_created", g->step_created);
    cJSON_AddNumberToObject(item, "step_resolved", g->step_resolved);
    cJSON_AddItemToArray(arr, item);
  }

  cJSON_AddItemToObject(root, "goals", arr);
  cJSON_AddNumberToObject(root, "next_id", gs->next_id);
  return root;
}

int goal_state_from_json(goal_state_t *gs, const cJSON *root) {
  if (!root) return -1;

  const cJSON *arr = cJSON_GetObjectItem(root, "goals");
  if (!arr || !cJSON_IsArray(arr)) return -1;

  const cJSON *next_id_j = cJSON_GetObjectItem(root, "next_id");
  if (next_id_j && cJSON_IsNumber(next_id_j))
    gs->next_id = (int)next_id_j->valuedouble;

  const cJSON *item;
  cJSON_ArrayForEach(item, arr) {
    const cJSON *id_j = cJSON_GetObjectItem(item, "id");
    const cJSON *pid_j = cJSON_GetObjectItem(item, "parent_id");
    const cJSON *content_j = cJSON_GetObjectItem(item, "content");
    const cJSON *status_j = cJSON_GetObjectItem(item, "status");

    if (!id_j || !content_j) continue;

    /* Grow if needed */
    if (gs->count >= gs->cap) {
      int new_cap = gs->cap * 2;
      goal_t *ng = realloc(gs->goals, (size_t)new_cap * sizeof(goal_t));
      if (!ng) return -1;
      gs->goals = ng;
      gs->cap = new_cap;
    }

    goal_t *g = &gs->goals[gs->count];
    memset(g, 0, sizeof(*g));
    g->id = (int)id_j->valuedouble;
    g->parent_id = pid_j ? (int)pid_j->valuedouble : 0;
    g->content = strdup(cJSON_IsString(content_j) ? content_j->valuestring : "");
    g->status = status_j && cJSON_IsString(status_j)
                  ? goal_status_from_str(status_j->valuestring)
                  : GOAL_PENDING;

    const cJSON *ev_j = cJSON_GetObjectItem(item, "evidence");
    if (ev_j && cJSON_IsString(ev_j))
      g->evidence = strdup(ev_j->valuestring);

    const cJSON *bl_j = cJSON_GetObjectItem(item, "blocker");
    if (bl_j && cJSON_IsString(bl_j))
      g->blocker = strdup(bl_j->valuestring);

    const cJSON *cts_j = cJSON_GetObjectItem(item, "created_ts");
    g->created_ts = cts_j ? cts_j->valuedouble : 0;

    const cJSON *rts_j = cJSON_GetObjectItem(item, "resolved_ts");
    g->resolved_ts = rts_j ? rts_j->valuedouble : 0;

    const cJSON *sc_j = cJSON_GetObjectItem(item, "step_created");
    g->step_created = sc_j ? (int)sc_j->valuedouble : 0;

    const cJSON *sr_j = cJSON_GetObjectItem(item, "step_resolved");
    g->step_resolved = sr_j ? (int)sr_j->valuedouble : -1;

    gs->count++;

    /* Track highest ID for next_id */
    if (g->id >= gs->next_id)
      gs->next_id = g->id + 1;
  }

  return 0;
}

/* ---- File persistence ---- */

/* Helper: read a JSON file. Returns cJSON* or NULL. */
static cJSON *read_json_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)len, f);
  buf[rd] = '\0';
  fclose(f);
  cJSON *root = cJSON_Parse(buf);
  free(buf);
  return root;
}

/* Helper: write cJSON to a file. */
static int write_json_file(const char *path, const cJSON *root) {
  char *str = cJSON_PrintUnformatted(root);
  if (!str) return -1;
  FILE *f = fopen(path, "w");
  if (!f) { free(str); return -1; }
  fputs(str, f);
  fputc('\n', f);
  fclose(f);
  free(str);
  return 0;
}

int goal_state_load(goal_state_t *gs, const char *session_dir) {
  char path[NASH_PATH_MAX];
  snprintf(path, sizeof(path), "%s/goals.json", session_dir);
  cJSON *root = read_json_file(path);
  if (!root) return -1;
  int rc = goal_state_from_json(gs, root);
  cJSON_Delete(root);
  return rc;
}

int goal_state_save(const goal_state_t *gs, const char *session_dir) {
  char path[NASH_PATH_MAX];
  snprintf(path, sizeof(path), "%s/goals.json", session_dir);
  cJSON *root = goal_state_to_json(gs);
  int rc = write_json_file(path, root);
  cJSON_Delete(root);
  return rc;
}

/* ---- Formatting ---- */

/* Status markers for display */
static const char *status_marker(goal_status_t s) {
  switch (s) {
    case GOAL_PENDING: return "[ ]";
    case GOAL_ACTIVE:  return "[>]";
    case GOAL_BLOCKED: return "[!]";
    case GOAL_DONE:    return "[x]";
    case GOAL_FAILED:  return "[-]";
    default:           return "[?]";
  }
}

/* Recursive helper to format a goal and its children */
static void format_goal_tree(str_t *s, const goal_state_t *gs,
                             int parent_id, int depth) {
  for (int i = 0; i < gs->count; i++) {
    const goal_t *g = &gs->goals[i];
    if (g->parent_id != parent_id) continue;

    /* Indent sub-goals */
    for (int d = 0; d < depth; d++)
      str_append_cstr(s, "  ");

    str_appendf(s, "%s G%d: %s", status_marker(g->status), g->id,
                g->content ? g->content : "");

    if (g->evidence)
      str_appendf(s, " (%s)", g->evidence);
    if (g->blocker)
      str_appendf(s, " [blocked: %s]", g->blocker);

    str_append_cstr(s, "\n");

    /* Recurse into children */
    format_goal_tree(s, gs, g->id, depth + 1);
  }
}

char *goal_format_text(const goal_state_t *gs) {
  if (!gs || gs->count == 0) return NULL;

  str_t s = str_new(512);

  /* Count by status */
  int pending = 0, active = 0, blocked = 0, done = 0, failed = 0;
  for (int i = 0; i < gs->count; i++) {
    switch (gs->goals[i].status) {
      case GOAL_PENDING: pending++; break;
      case GOAL_ACTIVE:  active++;  break;
      case GOAL_BLOCKED: blocked++; break;
      case GOAL_DONE:    done++;    break;
      case GOAL_FAILED:  failed++;  break;
    }
  }

  format_goal_tree(&s, gs, 0, 0);

  str_appendf(&s, "Goals: %d total, %d done, %d active, %d blocked, "
              "%d pending, %d failed",
              gs->count, done, active, blocked, pending, failed);

  char *result = strdup(str_cstr(&s));
  str_free(&s);
  return result;
}

/* ---- Counting ---- */

int goal_count_unresolved(const goal_state_t *gs, int root_only) {
  int count = 0;
  for (int i = 0; i < gs->count; i++) {
    const goal_t *g = &gs->goals[i];
    if (root_only && g->parent_id != 0) continue;
    if (g->status != GOAL_DONE && g->status != GOAL_FAILED)
      count++;
  }
  return count;
}

char *goal_unresolved_warning(const goal_state_t *gs) {
  int unresolved = goal_count_unresolved(gs, 0);
  if (unresolved == 0) return NULL;

  str_t s = str_new(256);
  str_appendf(&s, "WARNING: %d goal(s) unresolved:", unresolved);

  for (int i = 0; i < gs->count; i++) {
    const goal_t *g = &gs->goals[i];
    if (g->status == GOAL_DONE || g->status == GOAL_FAILED) continue;
    str_appendf(&s, " G%d(%s): %s;", g->id, goal_status_str(g->status),
                g->content ? g->content : "?");
  }

  char *result = strdup(str_cstr(&s));
  str_free(&s);
  return result;
}

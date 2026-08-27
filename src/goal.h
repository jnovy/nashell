/* goal.h - Structured goal state for Working Memory (Recuris-informed).
 *
 * Provides a hierarchical goal tracker with enforced state machine:
 *   PENDING -> ACTIVE -> DONE (requires evidence ref)
 *                     -> FAILED
 *   ACTIVE  -> BLOCKED (requires blocker text)
 *   BLOCKED -> ACTIVE  (unblock)
 *
 * Goals project to the scratchpad at priority 1 and survive context
 * eviction, addressing the "length costs completion, not comprehension"
 * finding from Recuris (arXiv 2608.24876, Sec 5.4).
 */
#ifndef GOAL_H
#define GOAL_H

#include <stddef.h>
#include <time.h>
#include "cJSON.h"

/* Goal status enum - defines the state machine */
typedef enum {
  GOAL_PENDING = 0,  /* created but not yet started */
  GOAL_ACTIVE,       /* currently being worked on */
  GOAL_BLOCKED,      /* waiting on something (blocker text required) */
  GOAL_DONE,         /* completed (evidence ref required) */
  GOAL_FAILED        /* abandoned or impossible */
} goal_status_t;

/* Single goal entry */
typedef struct {
  int id;              /* unique goal ID (1-based, auto-assigned) */
  int parent_id;       /* parent goal ID (0 = root/top-level) */
  char *content;       /* what needs to happen (owned) */
  goal_status_t status;
  char *evidence;      /* ref alias proving completion (owned, NULL if not done) */
  char *blocker;       /* what is blocking progress (owned, NULL if not blocked) */
  double created_ts;   /* epoch when goal was created */
  double resolved_ts;  /* epoch when goal was resolved (done/failed), 0 if open */
  int step_created;    /* react step when created */
  int step_resolved;   /* react step when resolved, -1 if open */
} goal_t;

/* Goal state container */
typedef struct {
  goal_t *goals;  /* dynamically allocated array */
  int count;
  int cap;
  int next_id;    /* next auto-assigned goal ID */
} goal_state_t;

/* Status string conversion */
const char *goal_status_str(goal_status_t s);
goal_status_t goal_status_from_str(const char *s);

/* Lifecycle */
void goal_state_init(goal_state_t *gs);
void goal_state_free(goal_state_t *gs);

/* Persistence: load/save from session_dir/goals.json */
int goal_state_load(goal_state_t *gs, const char *session_dir);
int goal_state_save(const goal_state_t *gs, const char *session_dir);

/* Find a goal by ID. Returns pointer into gs->goals or NULL. */
goal_t *goal_find(goal_state_t *gs, int id);

/* Add a new goal. Returns the assigned goal ID (>0), or -1 on error. */
int goal_add(goal_state_t *gs, const char *content, int parent_id,
             int step, double ts);

/* State transitions (return 0 on success, -1 on invalid transition).
 * goal_done() requires a non-NULL evidence string.
 * goal_block() requires a non-NULL blocker string. */
int goal_activate(goal_t *g, int step, double ts);
int goal_block(goal_t *g, const char *blocker, int step, double ts);
int goal_unblock(goal_t *g, int step, double ts);
int goal_done(goal_t *g, const char *evidence, int step, double ts);
int goal_fail(goal_t *g, int step, double ts);

/* Format goals as text for scratchpad projection.
 * Hierarchical tree with status markers. Caller must free. */
char *goal_format_text(const goal_state_t *gs);

/* Count unresolved goals (not DONE and not FAILED).
 * If root_only is true, counts only top-level goals (parent_id == 0). */
int goal_count_unresolved(const goal_state_t *gs, int root_only);

/* Build a warning string for unresolved goals (for done() tool).
 * Returns heap-allocated string or NULL if all goals resolved.
 * Caller must free. */
char *goal_unresolved_warning(const goal_state_t *gs);

/* Serialize goal state to cJSON (for persistence). Caller owns result. */
cJSON *goal_state_to_json(const goal_state_t *gs);

/* Deserialize goal state from cJSON. Returns 0 on success. */
int goal_state_from_json(goal_state_t *gs, const cJSON *root);

#endif /* GOAL_H */

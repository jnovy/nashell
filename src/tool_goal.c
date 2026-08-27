/* tool_goal.c - Structured goal tracking tool (Recuris WM Phase 1).
 *
 * Provides goal() tool with ops: add, activate, block, unblock, done, fail,
 * status. Enforces state machine transitions and evidence validation.
 *
 * Projects goal state to scratchpad section "goals" at priority 1.
 */

#include "tools_internal.h"
#include "tool_plugin.h"
#include "scratchpad.h"
#include "goal.h"

#include <string.h>
#include <time.h>

/* ---- Helpers ---- */

static double now_ts(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Load goal state from session, initializing if no file exists. */
static void load_goals(tool_ctx_t *ctx, goal_state_t *gs) {
  goal_state_init(gs);
  goal_state_load(gs, ctx->session_dir); /* ok if file missing */
}

/* Save goal state and project to scratchpad. */
static void save_and_project(tool_ctx_t *ctx, goal_state_t *gs) {
  goal_state_save(gs, ctx->session_dir);
  char *text = goal_format_text(gs);
  if (text) {
    scratchpad_write(&ctx->scratch, "goals", text, 1);
    scratchpad_save(&ctx->scratch, ctx->session_dir);
    free(text);
  }
}

/* Build a status summary cJSON object. */
static void add_status_summary(cJSON *meta, const goal_state_t *gs) {
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
  cJSON_AddNumberToObject(meta, "total", gs->count);
  cJSON_AddNumberToObject(meta, "done", done);
  cJSON_AddNumberToObject(meta, "active", active);
  cJSON_AddNumberToObject(meta, "blocked", blocked);
  cJSON_AddNumberToObject(meta, "pending", pending);
  cJSON_AddNumberToObject(meta, "failed", failed);
  cJSON_AddNumberToObject(meta, "unresolved", pending + active + blocked);
}

/* ---- Tool handler ---- */

static tool_result_t tool_goal(tool_ctx_t *ctx, cJSON *params) {
  TOOL_REQ_STR(params, "op", op);

  goal_state_t gs;
  load_goals(ctx, &gs);

  tool_result_t result;

  /* ---- op: add ---- */
  if (strcmp(op, "add") == 0) {
    TOOL_OPT_STR(params, "content", content);
    if (!content || !content[0]) {
      goal_state_free(&gs);
      return tools_make_error("'content' is required for op=add");
    }

    int parent_id = 0;
    cJSON *parent_j = cJSON_GetObjectItem(params, "parent");
    if (parent_j && cJSON_IsNumber(parent_j))
      parent_id = (int)parent_j->valuedouble;

    int gid = goal_add(&gs, content, parent_id, ctx->step, now_ts());
    if (gid < 0) {
      goal_state_free(&gs);
      return tools_make_error(parent_id > 0
        ? "Failed to add goal - parent goal not found"
        : "Failed to add goal - content must be non-empty");
    }

    save_and_project(ctx, &gs);

    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal added"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: activate ---- */
  if (strcmp(op, "activate") == 0) {
    cJSON *id_j = cJSON_GetObjectItem(params, "id");
    if (!id_j || !cJSON_IsNumber(id_j)) {
      goal_state_free(&gs);
      return tools_make_error("'id' (integer) is required for op=activate");
    }
    int gid = (int)id_j->valuedouble;
    goal_t *g = goal_find(&gs, gid);
    if (!g) {
      goal_state_free(&gs);
      return tools_make_error("Goal not found");
    }
    if (goal_activate(g, ctx->step, now_ts()) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Cannot activate G%d - current status is '%s' "
               "(must be pending or blocked)", gid, goal_status_str(g->status));
      goal_state_free(&gs);
      return tools_make_error(msg);
    }

    save_and_project(ctx, &gs);

    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal activated"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: block ---- */
  if (strcmp(op, "block") == 0) {
    cJSON *id_j = cJSON_GetObjectItem(params, "id");
    if (!id_j || !cJSON_IsNumber(id_j)) {
      goal_state_free(&gs);
      return tools_make_error("'id' (integer) is required for op=block");
    }
    TOOL_OPT_STR(params, "blocker", blocker);
    if (!blocker || !blocker[0]) {
      goal_state_free(&gs);
      return tools_make_error("'blocker' text is required for op=block");
    }
    int gid = (int)id_j->valuedouble;
    goal_t *g = goal_find(&gs, gid);
    if (!g) {
      goal_state_free(&gs);
      return tools_make_error("Goal not found");
    }
    if (goal_block(g, blocker, ctx->step, now_ts()) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Cannot block G%d - current status is '%s' (must be active)",
               gid, goal_status_str(g->status));
      goal_state_free(&gs);
      return tools_make_error(msg);
    }

    save_and_project(ctx, &gs);

    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal blocked"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: unblock ---- */
  if (strcmp(op, "unblock") == 0) {
    cJSON *id_j = cJSON_GetObjectItem(params, "id");
    if (!id_j || !cJSON_IsNumber(id_j)) {
      goal_state_free(&gs);
      return tools_make_error("'id' (integer) is required for op=unblock");
    }
    int gid = (int)id_j->valuedouble;
    goal_t *g = goal_find(&gs, gid);
    if (!g) {
      goal_state_free(&gs);
      return tools_make_error("Goal not found");
    }
    if (goal_unblock(g, ctx->step, now_ts()) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Cannot unblock G%d - current status is '%s' (must be blocked)",
               gid, goal_status_str(g->status));
      goal_state_free(&gs);
      return tools_make_error(msg);
    }

    save_and_project(ctx, &gs);

    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal unblocked"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: done ---- */
  if (strcmp(op, "done") == 0) {
    cJSON *id_j = cJSON_GetObjectItem(params, "id");
    if (!id_j || !cJSON_IsNumber(id_j)) {
      goal_state_free(&gs);
      return tools_make_error("'id' (integer) is required for op=done");
    }
    TOOL_OPT_STR(params, "evidence", evidence);
    if (!evidence || !evidence[0]) {
      goal_state_free(&gs);
      return tools_make_error("'evidence' ref is required for op=done "
                              "(provide a ref alias like R0S5)");
    }
    /* Validate evidence ref exists */
    const char *resolved = alias_map_lookup(ctx->aliases, evidence);
    if (!resolved) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "Evidence ref '%s' not found - must be a valid step ref alias",
               evidence);
      goal_state_free(&gs);
      return tools_make_error(msg);
    }
    int gid = (int)id_j->valuedouble;
    goal_t *g = goal_find(&gs, gid);
    if (!g) {
      goal_state_free(&gs);
      return tools_make_error("Goal not found");
    }
    if (goal_done(g, evidence, ctx->step, now_ts()) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Cannot complete G%d - current status is '%s' "
               "(must be active or pending)", gid, goal_status_str(g->status));
      goal_state_free(&gs);
      return tools_make_error(msg);
    }

    save_and_project(ctx, &gs);

    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal completed"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    cJSON_AddStringToObject(result.meta, "evidence", evidence);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: fail ---- */
  if (strcmp(op, "fail") == 0) {
    cJSON *id_j = cJSON_GetObjectItem(params, "id");
    if (!id_j || !cJSON_IsNumber(id_j)) {
      goal_state_free(&gs);
      return tools_make_error("'id' (integer) is required for op=fail");
    }
    int gid = (int)id_j->valuedouble;
    goal_t *g = goal_find(&gs, gid);
    if (!g) {
      goal_state_free(&gs);
      return tools_make_error("Goal not found");
    }
    if (goal_fail(g, ctx->step, now_ts()) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Cannot fail G%d - already resolved (status: '%s')",
               gid, goal_status_str(g->status));
      goal_state_free(&gs);
      return tools_make_error(msg);
    }

    save_and_project(ctx, &gs);

    TOOL_OPT_STR(params, "reason", reason);
    result = tool_result_ok();
    cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("goal failed"));
    cJSON_AddNumberToObject(result.meta, "id", gid);
    if (reason)
      cJSON_AddStringToObject(result.meta, "reason", reason);
    add_status_summary(result.meta, &gs);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  /* ---- op: status ---- */
  if (strcmp(op, "status") == 0) {
    result = tool_result_ok();
    if (gs.count == 0) {
      cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString("no goals defined"));
    } else {
      char *text = goal_format_text(&gs);
      cJSON_ReplaceItemInObject(result.meta, "status", cJSON_CreateString(text ? text : ""));
      free(text);
      add_status_summary(result.meta, &gs);
    }

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "goal", params, NULL, 0, 0, NULL, NULL);
    goal_state_free(&gs);
    return result;
  }

  goal_state_free(&gs);
  return tools_make_error("Unknown op - must be one of: "
                          "add, activate, block, unblock, done, fail, status");
}

/* ---- Registration ---- */

static const char *goal_op_enum[] = {
  "add", "activate", "block", "unblock", "done", "fail", "status", NULL
};

static const tool_param_t goal_params[] = {
  TOOL_PARAM_ENUM("op", "string",
    "Operation: add (create goal), activate (start working), "
    "block (mark blocked with reason), unblock, "
    "done (complete with evidence ref), fail, status (show all goals)",
    1, goal_op_enum),
  TOOL_PARAM("id", "integer",
    "Goal ID (required for activate/block/unblock/done/fail)", 0),
  TOOL_PARAM("content", "string",
    "Goal description (required for add)", 0),
  TOOL_PARAM("parent", "integer",
    "Parent goal ID for sub-goals (0 or omit for top-level)", 0),
  TOOL_PARAM("evidence", "string",
    "Ref alias (e.g. R0S5) proving goal completion (required for done)", 0),
  TOOL_PARAM("blocker", "string",
    "What is blocking progress (required for block)", 0),
  TOOL_PARAM("reason", "string",
    "Why the goal failed (optional for fail)", 0),
  TOOL_PARAM_END
};

static const tool_plugin_t goal_plugin = TOOL_DEF(
  "goal",
  "Structured goal tracking with enforced state machine. Goals have status: "
  "pending -> active -> done (requires evidence ref) or failed. "
  "Active goals can be blocked (requires blocker text) and unblocked. "
  "Goals project to scratchpad and survive context eviction. "
  "Use for multi-step tasks to track what needs doing, what is in progress, "
  "and what is complete. The done() tool warns about unresolved goals.",
  goal_params, tool_goal);

TOOL_PLUGIN_REGISTER(goal_plugin)

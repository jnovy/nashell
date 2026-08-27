/* test_goal.c - Tests for structured goal tracking (Recuris WM Phase 1).
 *
 * Tests goal state machine, persistence, scratchpad projection,
 * tool operations, and done() integration.
 */

#include "test_common.h"
#include "tools.h"
#include "goal.h"
#include "scratchpad.h"
#include "store.h"
#include "journal.h"
#include "str.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* Global required by tools.c (main.c not linked in tests) */
int g_path_given = 0;

static char *g_tmpdir;
static tool_ctx_t g_ctx;

static void setup_ctx(void) {
  g_tmpdir = make_test_dir();
  char store_path[512];
  snprintf(store_path, sizeof(store_path), "%s/store", g_tmpdir);
  mkdir(store_path, 0755);
  memset(&g_ctx, 0, sizeof(g_ctx));
  g_ctx.store = store_new(store_path);
  g_ctx.journal = journal_new(g_tmpdir);
  g_ctx.aliases = alias_map_new();
  g_ctx.session_dir = strdup(g_tmpdir);
  g_ctx.react_loop = 0;
  g_ctx.step = 0;
  scratchpad_init(&g_ctx.scratch);
}

static void teardown_ctx(void) {
  scratchpad_free(&g_ctx.scratch);
  alias_map_free(g_ctx.aliases);
  journal_free(g_ctx.journal);
  store_free(g_ctx.store);
  free(g_ctx.session_dir);
  rm_rf(g_tmpdir);
  free(g_tmpdir);
}

/* Helper: create a ref alias for evidence validation */
static char *create_ref_alias(void) {
  char *hash = store_save(g_ctx.store, "test evidence content");
  char *alias = tool_register_alias(&g_ctx, hash ? hash : "");
  g_ctx.step++;
  free(hash);
  return alias;
}

/* ---- Core API tests (goal.c) ---- */

static void test_goal_state_lifecycle(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  ASSERT(gs.count == 0);
  ASSERT(gs.cap > 0);
  ASSERT(gs.next_id == 1);

  goal_state_free(&gs);
}

static void test_goal_add_basic(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  int id1 = goal_add(&gs, "First goal", 0, 1, 1000.0);
  ASSERT(id1 == 1);
  ASSERT(gs.count == 1);

  int id2 = goal_add(&gs, "Second goal", 0, 2, 1001.0);
  ASSERT(id2 == 2);
  ASSERT(gs.count == 2);

  goal_t *g = goal_find(&gs, id1);
  ASSERT_NOT_NULL(g);
  ASSERT_STR_EQ(g->content, "First goal");
  ASSERT(g->status == GOAL_PENDING);
  ASSERT(g->parent_id == 0);
  ASSERT(g->step_created == 1);
  ASSERT(g->step_resolved == -1);

  goal_state_free(&gs);
}

static void test_goal_add_with_parent(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  int parent = goal_add(&gs, "Parent goal", 0, 1, 1000.0);
  int child = goal_add(&gs, "Child goal", parent, 2, 1001.0);
  ASSERT(child == 2);

  goal_t *g = goal_find(&gs, child);
  ASSERT_NOT_NULL(g);
  ASSERT(g->parent_id == parent);

  /* Invalid parent should fail */
  int bad = goal_add(&gs, "Orphan", 999, 3, 1002.0);
  ASSERT(bad == -1);

  goal_state_free(&gs);
}

static void test_goal_add_empty_content_rejected(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  ASSERT(goal_add(&gs, "", 0, 1, 1000.0) == -1);
  ASSERT(goal_add(&gs, NULL, 0, 1, 1000.0) == -1);
  ASSERT(gs.count == 0);

  goal_state_free(&gs);
}

static void test_goal_find(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  goal_add(&gs, "goal one", 0, 1, 1000.0);
  goal_add(&gs, "goal two", 0, 2, 1001.0);

  ASSERT_NOT_NULL(goal_find(&gs, 1));
  ASSERT_NOT_NULL(goal_find(&gs, 2));
  ASSERT_NULL(goal_find(&gs, 3));
  ASSERT_NULL(goal_find(&gs, 0));

  goal_state_free(&gs);
}

/* ---- State machine transition tests ---- */

static void test_transition_pending_to_active(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);

  ASSERT(g->status == GOAL_PENDING);
  ASSERT(goal_activate(g, 2, 1001.0) == 0);
  ASSERT(g->status == GOAL_ACTIVE);

  goal_state_free(&gs);
}

static void test_transition_active_to_done(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  ASSERT(goal_done(g, "R0S1", 3, 1002.0) == 0);
  ASSERT(g->status == GOAL_DONE);
  ASSERT_STR_EQ(g->evidence, "R0S1");
  ASSERT(g->step_resolved == 3);

  goal_state_free(&gs);
}

static void test_transition_pending_to_done(void) {
  /* Simple tasks can go directly from PENDING to DONE */
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "simple task", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);

  ASSERT(goal_done(g, "R0S1", 2, 1001.0) == 0);
  ASSERT(g->status == GOAL_DONE);

  goal_state_free(&gs);
}

static void test_transition_active_to_blocked(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  ASSERT(goal_block(g, "waiting for dependency", 3, 1002.0) == 0);
  ASSERT(g->status == GOAL_BLOCKED);
  ASSERT_STR_EQ(g->blocker, "waiting for dependency");

  goal_state_free(&gs);
}

static void test_transition_blocked_to_active(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);
  goal_block(g, "blocker", 3, 1002.0);

  ASSERT(goal_unblock(g, 4, 1003.0) == 0);
  ASSERT(g->status == GOAL_ACTIVE);
  ASSERT_NULL(g->blocker);

  goal_state_free(&gs);
}

static void test_transition_to_failed(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  ASSERT(goal_fail(g, 3, 1002.0) == 0);
  ASSERT(g->status == GOAL_FAILED);
  ASSERT(g->step_resolved == 3);

  goal_state_free(&gs);
}

static void test_fail_from_any_non_terminal(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  /* Fail from PENDING */
  goal_add(&gs, "pending goal", 0, 1, 1000.0);
  goal_t *g1 = goal_find(&gs, 1);
  ASSERT(goal_fail(g1, 2, 1001.0) == 0);

  /* Fail from BLOCKED */
  goal_add(&gs, "blocked goal", 0, 2, 1002.0);
  goal_t *g2 = goal_find(&gs, 2);
  goal_activate(g2, 3, 1003.0);
  goal_block(g2, "reason", 4, 1004.0);
  ASSERT(goal_fail(g2, 5, 1005.0) == 0);

  goal_state_free(&gs);
}

/* ---- Invalid transition tests ---- */

static void test_invalid_transitions(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);
  goal_done(g, "R0S1", 3, 1002.0);

  /* Cannot transition from DONE */
  ASSERT(goal_activate(g, 4, 1003.0) == -1);
  ASSERT(goal_block(g, "blocker", 4, 1003.0) == -1);
  ASSERT(goal_fail(g, 4, 1003.0) == -1);
  ASSERT(goal_done(g, "R0S2", 4, 1003.0) == -1);

  goal_state_free(&gs);
}

static void test_cannot_block_pending(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);

  /* PENDING cannot be blocked (must be ACTIVE first) */
  ASSERT(goal_block(g, "blocker", 2, 1001.0) == -1);
  ASSERT(g->status == GOAL_PENDING);

  goal_state_free(&gs);
}

static void test_cannot_unblock_non_blocked(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  /* ACTIVE cannot be unblocked */
  ASSERT(goal_unblock(g, 3, 1002.0) == -1);
  ASSERT(g->status == GOAL_ACTIVE);

  goal_state_free(&gs);
}

static void test_done_requires_evidence(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  ASSERT(goal_done(g, NULL, 3, 1002.0) == -1);
  ASSERT(goal_done(g, "", 3, 1002.0) == -1);
  ASSERT(g->status == GOAL_ACTIVE);

  goal_state_free(&gs);
}

static void test_block_requires_blocker(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "test", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);

  ASSERT(goal_block(g, NULL, 3, 1002.0) == -1);
  ASSERT(goal_block(g, "", 3, 1002.0) == -1);
  ASSERT(g->status == GOAL_ACTIVE);

  goal_state_free(&gs);
}

/* ---- Persistence tests ---- */

static void test_save_load_roundtrip(void) {
  char *tmpdir = make_test_dir();

  /* Create and save */
  goal_state_t gs1;
  goal_state_init(&gs1);
  goal_add(&gs1, "goal one", 0, 1, 1000.0);
  goal_add(&gs1, "goal two", 0, 2, 1001.0);
  goal_t *g = goal_find(&gs1, 1);
  goal_activate(g, 3, 1002.0);
  goal_done(g, "R0S1", 4, 1003.0);

  g = goal_find(&gs1, 2);
  goal_activate(g, 5, 1004.0);
  goal_block(g, "waiting", 6, 1005.0);

  ASSERT(goal_state_save(&gs1, tmpdir) == 0);

  /* Load into fresh state */
  goal_state_t gs2;
  goal_state_init(&gs2);
  ASSERT(goal_state_load(&gs2, tmpdir) == 0);

  ASSERT(gs2.count == 2);

  goal_t *g1 = goal_find(&gs2, 1);
  ASSERT_NOT_NULL(g1);
  ASSERT_STR_EQ(g1->content, "goal one");
  ASSERT(g1->status == GOAL_DONE);
  ASSERT_STR_EQ(g1->evidence, "R0S1");
  ASSERT(g1->step_resolved == 4);

  goal_t *g2 = goal_find(&gs2, 2);
  ASSERT_NOT_NULL(g2);
  ASSERT_STR_EQ(g2->content, "goal two");
  ASSERT(g2->status == GOAL_BLOCKED);
  ASSERT_STR_EQ(g2->blocker, "waiting");

  /* next_id should be preserved */
  ASSERT(gs2.next_id == 3);

  goal_state_free(&gs1);
  goal_state_free(&gs2);
  rm_rf(tmpdir);
  free(tmpdir);
}

static void test_load_missing_file(void) {
  char *tmpdir = make_test_dir();

  goal_state_t gs;
  goal_state_init(&gs);
  ASSERT(goal_state_load(&gs, tmpdir) == -1);
  ASSERT(gs.count == 0);

  goal_state_free(&gs);
  rm_rf(tmpdir);
  free(tmpdir);
}

/* ---- Formatting tests ---- */

static void test_format_text_empty(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  char *text = goal_format_text(&gs);
  ASSERT_NULL(text);

  goal_state_free(&gs);
}

static void test_format_text_basic(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "implement feature", 0, 1, 1000.0);
  goal_add(&gs, "write tests", 0, 2, 1001.0);

  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 3, 1002.0);

  char *text = goal_format_text(&gs);
  ASSERT_NOT_NULL(text);
  ASSERT_STR_CONTAINS(text, "[>] G1: implement feature");
  ASSERT_STR_CONTAINS(text, "[ ] G2: write tests");
  ASSERT_STR_CONTAINS(text, "Goals:");
  free(text);

  goal_state_free(&gs);
}

static void test_format_text_hierarchy(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  int p = goal_add(&gs, "parent", 0, 1, 1000.0);
  goal_add(&gs, "child", p, 2, 1001.0);

  char *text = goal_format_text(&gs);
  ASSERT_NOT_NULL(text);
  /* Child should be indented */
  ASSERT_STR_CONTAINS(text, "  [ ] G2: child");
  free(text);

  goal_state_free(&gs);
}

static void test_format_text_with_evidence(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "done goal", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);
  goal_done(g, "R0S5", 3, 1002.0);

  char *text = goal_format_text(&gs);
  ASSERT_NOT_NULL(text);
  ASSERT_STR_CONTAINS(text, "[x] G1: done goal (R0S5)");
  free(text);

  goal_state_free(&gs);
}

static void test_format_text_with_blocker(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "blocked goal", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);
  goal_block(g, "need API key", 3, 1002.0);

  char *text = goal_format_text(&gs);
  ASSERT_NOT_NULL(text);
  ASSERT_STR_CONTAINS(text, "[!] G1: blocked goal [blocked: need API key]");
  free(text);

  goal_state_free(&gs);
}

/* ---- Counting/warning tests ---- */

static void test_count_unresolved(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "pending", 0, 1, 1000.0);
  goal_add(&gs, "active", 0, 2, 1001.0);
  int id3 = goal_add(&gs, "done", 0, 3, 1002.0);

  goal_t *g2 = goal_find(&gs, 2);
  goal_activate(g2, 4, 1003.0);

  goal_t *g3 = goal_find(&gs, id3);
  goal_activate(g3, 5, 1004.0);
  goal_done(g3, "R0S1", 6, 1005.0);

  ASSERT(goal_count_unresolved(&gs, 0) == 2);
  ASSERT(goal_count_unresolved(&gs, 1) == 2); /* all are root */

  goal_state_free(&gs);
}

static void test_count_unresolved_with_children(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  int parent = goal_add(&gs, "parent", 0, 1, 1000.0);
  goal_add(&gs, "child", parent, 2, 1001.0);

  /* Both unresolved, but only 1 root */
  ASSERT(goal_count_unresolved(&gs, 0) == 2);
  ASSERT(goal_count_unresolved(&gs, 1) == 1);

  goal_state_free(&gs);
}

static void test_unresolved_warning(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "unfinished task", 0, 1, 1000.0);

  char *warn = goal_unresolved_warning(&gs);
  ASSERT_NOT_NULL(warn);
  ASSERT_STR_CONTAINS(warn, "WARNING:");
  ASSERT_STR_CONTAINS(warn, "unfinished task");
  free(warn);

  goal_state_free(&gs);
}

static void test_no_warning_when_all_resolved(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  goal_add(&gs, "done task", 0, 1, 1000.0);
  goal_t *g = goal_find(&gs, 1);
  goal_activate(g, 2, 1001.0);
  goal_done(g, "R0S1", 3, 1002.0);

  char *warn = goal_unresolved_warning(&gs);
  ASSERT_NULL(warn);

  goal_state_free(&gs);
}

/* ---- Status string conversion ---- */

static void test_status_str_roundtrip(void) {
  ASSERT_STR_EQ(goal_status_str(GOAL_PENDING), "pending");
  ASSERT_STR_EQ(goal_status_str(GOAL_ACTIVE), "active");
  ASSERT_STR_EQ(goal_status_str(GOAL_BLOCKED), "blocked");
  ASSERT_STR_EQ(goal_status_str(GOAL_DONE), "done");
  ASSERT_STR_EQ(goal_status_str(GOAL_FAILED), "failed");

  ASSERT(goal_status_from_str("pending") == GOAL_PENDING);
  ASSERT(goal_status_from_str("active") == GOAL_ACTIVE);
  ASSERT(goal_status_from_str("blocked") == GOAL_BLOCKED);
  ASSERT(goal_status_from_str("done") == GOAL_DONE);
  ASSERT(goal_status_from_str("failed") == GOAL_FAILED);

  /* Unknown defaults to PENDING */
  ASSERT(goal_status_from_str("bogus") == GOAL_PENDING);
  ASSERT(goal_status_from_str(NULL) == GOAL_PENDING);
}

/* ---- JSON serialization tests ---- */

static void test_json_roundtrip(void) {
  goal_state_t gs1;
  goal_state_init(&gs1);
  goal_add(&gs1, "goal A", 0, 1, 1000.0);
  goal_add(&gs1, "goal B", 0, 2, 1001.0);
  goal_t *g = goal_find(&gs1, 1);
  goal_activate(g, 3, 1002.0);
  goal_done(g, "R0S1", 4, 1003.0);

  cJSON *json = goal_state_to_json(&gs1);
  ASSERT_NOT_NULL(json);

  goal_state_t gs2;
  goal_state_init(&gs2);
  ASSERT(goal_state_from_json(&gs2, json) == 0);
  ASSERT(gs2.count == 2);
  ASSERT(gs2.next_id == gs1.next_id);

  goal_t *g1 = goal_find(&gs2, 1);
  ASSERT_NOT_NULL(g1);
  ASSERT_STR_EQ(g1->content, "goal A");
  ASSERT(g1->status == GOAL_DONE);
  ASSERT_STR_EQ(g1->evidence, "R0S1");

  goal_t *g2 = goal_find(&gs2, 2);
  ASSERT_NOT_NULL(g2);
  ASSERT_STR_EQ(g2->content, "goal B");
  ASSERT(g2->status == GOAL_PENDING);

  cJSON_Delete(json);
  goal_state_free(&gs1);
  goal_state_free(&gs2);
}

static void test_json_from_null(void) {
  goal_state_t gs;
  goal_state_init(&gs);
  ASSERT(goal_state_from_json(&gs, NULL) == -1);
  goal_state_free(&gs);
}

/* ---- Array growth test ---- */

static void test_array_growth(void) {
  goal_state_t gs;
  goal_state_init(&gs);

  /* Add more goals than initial capacity (16) */
  for (int i = 0; i < 20; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "goal %d", i + 1);
    int id = goal_add(&gs, buf, 0, i + 1, 1000.0 + i);
    ASSERT(id == i + 1);
  }
  ASSERT(gs.count == 20);
  ASSERT(gs.cap >= 20);

  goal_t *g20 = goal_find(&gs, 20);
  ASSERT_NOT_NULL(g20);
  ASSERT_STR_EQ(g20->content, "goal 20");

  goal_state_free(&gs);
}

/* ---- Tool integration tests (via tool_execute) ---- */

static void test_tool_add(void) {
  setup_ctx();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "add");
  cJSON_AddStringToObject(params, "content", "implement feature X");

  tool_result_t res = tool_execute(&g_ctx, "goal", params);
  ASSERT(res.success == 1);

  const char *status = json_str(res.meta, "status");
  ASSERT_STR_EQ(status, "goal added");

  cJSON *id_j = cJSON_GetObjectItem(res.meta, "id");
  ASSERT_NOT_NULL(id_j);
  ASSERT(id_j->valuedouble == 1.0);

  tool_result_free(&res);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_tool_add_missing_content(void) {
  setup_ctx();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "add");

  tool_result_t res = tool_execute(&g_ctx, "goal", params);
  ASSERT(res.success == 0);

  tool_result_free(&res);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_tool_activate(void) {
  setup_ctx();

  /* Add a goal first */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "test goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  ASSERT(r1.success == 1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Activate it */
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "activate");
  cJSON_AddNumberToObject(p2, "id", 1);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  ASSERT(r2.success == 1);
  ASSERT_STR_EQ(json_str(r2.meta, "status"), "goal activated");
  tool_result_free(&r2);
  cJSON_Delete(p2);

  teardown_ctx();
}

static void test_tool_block_unblock(void) {
  setup_ctx();

  /* Add + activate */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "test goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "activate");
  cJSON_AddNumberToObject(p2, "id", 1);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  /* Block */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "op", "block");
  cJSON_AddNumberToObject(p3, "id", 1);
  cJSON_AddStringToObject(p3, "blocker", "need API access");
  tool_result_t r3 = tool_execute(&g_ctx, "goal", p3);
  ASSERT(r3.success == 1);
  ASSERT_STR_EQ(json_str(r3.meta, "status"), "goal blocked");
  tool_result_free(&r3);
  cJSON_Delete(p3);

  /* Unblock */
  cJSON *p4 = cJSON_CreateObject();
  cJSON_AddStringToObject(p4, "op", "unblock");
  cJSON_AddNumberToObject(p4, "id", 1);
  tool_result_t r4 = tool_execute(&g_ctx, "goal", p4);
  ASSERT(r4.success == 1);
  ASSERT_STR_EQ(json_str(r4.meta, "status"), "goal unblocked");
  tool_result_free(&r4);
  cJSON_Delete(p4);

  teardown_ctx();
}

static void test_tool_done_with_evidence(void) {
  setup_ctx();

  /* Add + activate */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "test goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "activate");
  cJSON_AddNumberToObject(p2, "id", 1);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  /* Create a real ref alias for evidence */
  char *alias = create_ref_alias();

  /* Complete with evidence */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "op", "done");
  cJSON_AddNumberToObject(p3, "id", 1);
  cJSON_AddStringToObject(p3, "evidence", alias);
  tool_result_t r3 = tool_execute(&g_ctx, "goal", p3);
  ASSERT(r3.success == 1);
  ASSERT_STR_EQ(json_str(r3.meta, "status"), "goal completed");
  ASSERT_STR_EQ(json_str(r3.meta, "evidence"), alias);
  tool_result_free(&r3);
  cJSON_Delete(p3);
  free(alias);

  teardown_ctx();
}

static void test_tool_done_invalid_evidence(void) {
  setup_ctx();

  /* Add + activate */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "test goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "activate");
  cJSON_AddNumberToObject(p2, "id", 1);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  /* Try to complete with fake evidence */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "op", "done");
  cJSON_AddNumberToObject(p3, "id", 1);
  cJSON_AddStringToObject(p3, "evidence", "R99S99");
  tool_result_t r3 = tool_execute(&g_ctx, "goal", p3);
  ASSERT(r3.success == 0); /* Should fail - invalid ref */
  tool_result_free(&r3);
  cJSON_Delete(p3);

  teardown_ctx();
}

static void test_tool_fail(void) {
  setup_ctx();

  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "doomed goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "fail");
  cJSON_AddNumberToObject(p2, "id", 1);
  cJSON_AddStringToObject(p2, "reason", "impossible");
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  ASSERT(r2.success == 1);
  ASSERT_STR_EQ(json_str(r2.meta, "status"), "goal failed");
  tool_result_free(&r2);
  cJSON_Delete(p2);

  teardown_ctx();
}

static void test_tool_status(void) {
  setup_ctx();

  /* Status with no goals */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "status");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  ASSERT(r1.success == 1);
  ASSERT_STR_EQ(json_str(r1.meta, "status"), "no goals defined");
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Add a goal */
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "add");
  cJSON_AddStringToObject(p2, "content", "my goal");
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  /* Status with goals */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "op", "status");
  tool_result_t r3 = tool_execute(&g_ctx, "goal", p3);
  ASSERT(r3.success == 1);
  ASSERT_STR_CONTAINS(json_str(r3.meta, "status"), "G1: my goal");
  tool_result_free(&r3);
  cJSON_Delete(p3);

  teardown_ctx();
}

static void test_tool_invalid_transition(void) {
  setup_ctx();

  /* Add goal (PENDING) */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "test");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Try to block PENDING goal (invalid - must be ACTIVE) */
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "block");
  cJSON_AddNumberToObject(p2, "id", 1);
  cJSON_AddStringToObject(p2, "blocker", "reason");
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  ASSERT(r2.success == 0);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  teardown_ctx();
}

static void test_tool_unknown_op(void) {
  setup_ctx();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "bogus");
  tool_result_t res = tool_execute(&g_ctx, "goal", params);
  ASSERT(res.success == 0);
  tool_result_free(&res);
  cJSON_Delete(params);

  teardown_ctx();
}

static void test_tool_nonexistent_goal(void) {
  setup_ctx();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "activate");
  cJSON_AddNumberToObject(params, "id", 999);
  tool_result_t res = tool_execute(&g_ctx, "goal", params);
  ASSERT(res.success == 0);
  tool_result_free(&res);
  cJSON_Delete(params);

  teardown_ctx();
}

static void test_tool_sub_goals(void) {
  setup_ctx();

  /* Add parent */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "parent goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  ASSERT(r1.success == 1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Add child */
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "add");
  cJSON_AddStringToObject(p2, "content", "child goal");
  cJSON_AddNumberToObject(p2, "parent", 1);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  ASSERT(r2.success == 1);
  cJSON *id_j = cJSON_GetObjectItem(r2.meta, "id");
  ASSERT(id_j->valuedouble == 2.0);
  tool_result_free(&r2);
  cJSON_Delete(p2);

  /* Verify hierarchy in status */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "op", "status");
  tool_result_t r3 = tool_execute(&g_ctx, "goal", p3);
  ASSERT(r3.success == 1);
  ASSERT_STR_CONTAINS(json_str(r3.meta, "status"), "parent goal");
  ASSERT_STR_CONTAINS(json_str(r3.meta, "status"), "child goal");
  tool_result_free(&r3);
  cJSON_Delete(p3);

  teardown_ctx();
}

/* ---- Scratchpad projection test ---- */

static void test_scratchpad_projection(void) {
  setup_ctx();

  /* Add a goal via tool */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "projected goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  ASSERT(r1.success == 1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Check scratchpad has "goals" section */
  int idx = scratchpad_find(&g_ctx.scratch, "goals");
  ASSERT(idx >= 0);
  ASSERT_STR_CONTAINS(g_ctx.scratch.sections[idx].content, "projected goal");
  ASSERT(g_ctx.scratch.sections[idx].priority == 1);

  teardown_ctx();
}

/* ---- Persistence across tool calls ---- */

static void test_tool_persistence(void) {
  setup_ctx();

  /* Add a goal */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "persistent goal");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Load directly from file to verify persistence */
  goal_state_t gs;
  goal_state_init(&gs);
  ASSERT(goal_state_load(&gs, g_ctx.session_dir) == 0);
  ASSERT(gs.count == 1);
  ASSERT_STR_EQ(gs.goals[0].content, "persistent goal");
  goal_state_free(&gs);

  teardown_ctx();
}

/* ---- done() integration test ---- */

static void test_done_warns_unresolved_goals(void) {
  setup_ctx();

  /* Add unresolved goal */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "unfinished work");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  /* Call done() */
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "result", "task complete");
  tool_result_t r2 = tool_execute(&g_ctx, "done", p2);
  ASSERT(r2.success == 1);

  /* Should have goal_warning in meta */
  const char *gw = json_str(r2.meta, "goal_warning");
  ASSERT_NOT_NULL(gw);
  ASSERT_STR_CONTAINS(gw, "unfinished work");
  tool_result_free(&r2);
  cJSON_Delete(p2);

  teardown_ctx();
}

static void test_done_no_warning_when_resolved(void) {
  setup_ctx();

  /* Add and complete a goal */
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "add");
  cJSON_AddStringToObject(p1, "content", "finished work");
  tool_result_t r1 = tool_execute(&g_ctx, "goal", p1);
  tool_result_free(&r1);
  cJSON_Delete(p1);

  char *alias = create_ref_alias();

  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "done");
  cJSON_AddNumberToObject(p2, "id", 1);
  cJSON_AddStringToObject(p2, "evidence", alias);
  tool_result_t r2 = tool_execute(&g_ctx, "goal", p2);
  tool_result_free(&r2);
  cJSON_Delete(p2);
  free(alias);

  /* Call done() */
  cJSON *p3 = cJSON_CreateObject();
  cJSON_AddStringToObject(p3, "result", "all done");
  tool_result_t r3 = tool_execute(&g_ctx, "done", p3);
  ASSERT(r3.success == 1);

  /* Should NOT have goal_warning */
  ASSERT_NULL(cJSON_GetObjectItem(r3.meta, "goal_warning"));
  tool_result_free(&r3);
  cJSON_Delete(p3);

  teardown_ctx();
}

/* ---- main ---- */

int main(void) {
  int fail = 0;

  /* Core API tests */
  RUN_TEST(test_goal_state_lifecycle);
  RUN_TEST(test_goal_add_basic);
  RUN_TEST(test_goal_add_with_parent);
  RUN_TEST(test_goal_add_empty_content_rejected);
  RUN_TEST(test_goal_find);

  /* State machine transitions */
  RUN_TEST(test_transition_pending_to_active);
  RUN_TEST(test_transition_active_to_done);
  RUN_TEST(test_transition_pending_to_done);
  RUN_TEST(test_transition_active_to_blocked);
  RUN_TEST(test_transition_blocked_to_active);
  RUN_TEST(test_transition_to_failed);
  RUN_TEST(test_fail_from_any_non_terminal);

  /* Invalid transitions */
  RUN_TEST(test_invalid_transitions);
  RUN_TEST(test_cannot_block_pending);
  RUN_TEST(test_cannot_unblock_non_blocked);
  RUN_TEST(test_done_requires_evidence);
  RUN_TEST(test_block_requires_blocker);

  /* Persistence */
  RUN_TEST(test_save_load_roundtrip);
  RUN_TEST(test_load_missing_file);

  /* Formatting */
  RUN_TEST(test_format_text_empty);
  RUN_TEST(test_format_text_basic);
  RUN_TEST(test_format_text_hierarchy);
  RUN_TEST(test_format_text_with_evidence);
  RUN_TEST(test_format_text_with_blocker);

  /* Counting/warnings */
  RUN_TEST(test_count_unresolved);
  RUN_TEST(test_count_unresolved_with_children);
  RUN_TEST(test_unresolved_warning);
  RUN_TEST(test_no_warning_when_all_resolved);

  /* Status string conversion */
  RUN_TEST(test_status_str_roundtrip);

  /* JSON serialization */
  RUN_TEST(test_json_roundtrip);
  RUN_TEST(test_json_from_null);

  /* Array growth */
  RUN_TEST(test_array_growth);

  /* Tool integration */
  RUN_TEST(test_tool_add);
  RUN_TEST(test_tool_add_missing_content);
  RUN_TEST(test_tool_activate);
  RUN_TEST(test_tool_block_unblock);
  RUN_TEST(test_tool_done_with_evidence);
  RUN_TEST(test_tool_done_invalid_evidence);
  RUN_TEST(test_tool_fail);
  RUN_TEST(test_tool_status);
  RUN_TEST(test_tool_invalid_transition);
  RUN_TEST(test_tool_unknown_op);
  RUN_TEST(test_tool_nonexistent_goal);
  RUN_TEST(test_tool_sub_goals);

  /* Scratchpad projection */
  RUN_TEST(test_scratchpad_projection);

  /* Persistence across tool calls */
  RUN_TEST(test_tool_persistence);

  /* done() integration */
  RUN_TEST(test_done_warns_unresolved_goals);
  RUN_TEST(test_done_no_warning_when_resolved);

  TEST_SUMMARY();
  return fail ? 1 : 0;
}

/* test_plan_tracking.c - Tests for plan tool completion tracking.
 *
 * Verifies:
 *   1. Plan creation from numbered steps
 *   2. Step check with evidence validation
 *   3. Step check requires evidence
 *   4. Step check requires valid ref
 *   5. Step uncheck
 *   6. Plan status reporting
 *   7. Done warns on incomplete plan
 *   8. Done no warning when plan complete
 *   9. Backward compatibility (result-only API)
 */

#include "test_common.h"
#include "tool_plugin.h"
#include "tools.h"
#include "store.h"
#include "journal.h"
#include "cJSON.h"
#include "scratchpad.h"

/* Provide globals defined in main.c (not linked into tests) */
int g_path_given = 0;

/* ---- Helpers ---- */

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

/* Helper: create a two-step plan */
static void create_two_step_plan(void) {
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result",
                          "1. step one\n2. step two");
  tool_result_t r = tool_execute(&g_ctx, "plan", params);
  tool_result_free(&r);
  cJSON_Delete(params);
  g_ctx.step++;
}

/* Helper: create a valid ref alias by storing content */
static char *create_ref_alias(void) {
  char *hash = store_save(g_ctx.store, "test content for evidence");
  char *alias = tool_register_alias(&g_ctx, hash ? hash : "");
  free(hash);
  g_ctx.step++;
  return alias; /* caller must free */
}

/* ---- Tests ---- */

static void test_plan_create(void) {
  setup_ctx();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result",
                          "1. step one\n2. step two\n3. step three");

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify status in meta */
  cJSON *status = cJSON_GetObjectItem(result.meta, "status");
  ASSERT_NOT_NULL(status);

  /* Verify step count */
  cJSON *steps_j = cJSON_GetObjectItem(result.meta, "steps");
  ASSERT_NOT_NULL(steps_j);
  ASSERT_EQ((int)steps_j->valuedouble, 3);

  /* Verify scratchpad plan section has content */
  int idx = scratchpad_find(&g_ctx.scratch, "plan");
  ASSERT(idx >= 0);
  ASSERT_NOT_NULL(g_ctx.scratch.sections[idx].content);
  ASSERT_STR_CONTAINS(g_ctx.scratch.sections[idx].content, "step one");
  ASSERT_STR_CONTAINS(g_ctx.scratch.sections[idx].content, "step two");
  ASSERT_STR_CONTAINS(g_ctx.scratch.sections[idx].content, "step three");

  tool_result_free(&result);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_plan_check(void) {
  setup_ctx();
  create_two_step_plan();

  /* Create a valid ref alias for evidence */
  char *alias = create_ref_alias();
  ASSERT_NOT_NULL(alias);

  /* Check step 1 with evidence */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "check");
  cJSON_AddNumberToObject(params, "step", 1);
  cJSON_AddStringToObject(params, "evidence", alias);

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify step is marked done */
  cJSON *done_j = cJSON_GetObjectItem(result.meta, "done");
  ASSERT_NOT_NULL(done_j);
  ASSERT_EQ((int)done_j->valuedouble, 1);

  /* Verify status indicates check */
  cJSON *status = cJSON_GetObjectItem(result.meta, "status");
  ASSERT_NOT_NULL(status);
  ASSERT_STR_CONTAINS(status->valuestring, "checked");

  tool_result_free(&result);
  cJSON_Delete(params);
  free(alias);
  teardown_ctx();
}

static void test_plan_check_requires_evidence(void) {
  setup_ctx();
  create_two_step_plan();

  /* Try to check step 1 without evidence */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "check");
  cJSON_AddNumberToObject(params, "step", 1);
  /* No evidence param */

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(!result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify error message mentions evidence */
  cJSON *err = cJSON_GetObjectItem(result.meta, "error");
  ASSERT_NOT_NULL(err);
  ASSERT_STR_CONTAINS(err->valuestring, "evidence");

  tool_result_free(&result);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_plan_check_requires_valid_ref(void) {
  setup_ctx();
  create_two_step_plan();

  /* Try to check with a bogus ref */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "check");
  cJSON_AddNumberToObject(params, "step", 1);
  cJSON_AddStringToObject(params, "evidence", "R99S99");

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(!result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify error message mentions the bad ref */
  cJSON *err = cJSON_GetObjectItem(result.meta, "error");
  ASSERT_NOT_NULL(err);
  ASSERT_STR_CONTAINS(err->valuestring, "R99S99");

  tool_result_free(&result);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_plan_uncheck(void) {
  setup_ctx();
  create_two_step_plan();

  /* Check step 1 first */
  char *alias = create_ref_alias();
  cJSON *check_p = cJSON_CreateObject();
  cJSON_AddStringToObject(check_p, "op", "check");
  cJSON_AddNumberToObject(check_p, "step", 1);
  cJSON_AddStringToObject(check_p, "evidence", alias);
  tool_result_t r1 = tool_execute(&g_ctx, "plan", check_p);
  ASSERT(r1.success);
  tool_result_free(&r1);
  cJSON_Delete(check_p);
  g_ctx.step++;

  /* Now uncheck step 1 */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "uncheck");
  cJSON_AddNumberToObject(params, "step", 1);

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify done count is back to 0 */
  cJSON *done_j = cJSON_GetObjectItem(result.meta, "done");
  ASSERT_NOT_NULL(done_j);
  ASSERT_EQ((int)done_j->valuedouble, 0);

  /* Verify status says unchecked */
  cJSON *status = cJSON_GetObjectItem(result.meta, "status");
  ASSERT_NOT_NULL(status);
  ASSERT_STR_CONTAINS(status->valuestring, "unchecked");

  tool_result_free(&result);
  cJSON_Delete(params);
  free(alias);
  teardown_ctx();
}

static void test_plan_status(void) {
  setup_ctx();
  create_two_step_plan();

  /* Check step 1 */
  char *alias = create_ref_alias();
  cJSON *check_p = cJSON_CreateObject();
  cJSON_AddStringToObject(check_p, "op", "check");
  cJSON_AddNumberToObject(check_p, "step", 1);
  cJSON_AddStringToObject(check_p, "evidence", alias);
  tool_result_t r1 = tool_execute(&g_ctx, "plan", check_p);
  ASSERT(r1.success);
  tool_result_free(&r1);
  cJSON_Delete(check_p);
  g_ctx.step++;

  /* Get status */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "status");

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify counts */
  cJSON *total_j = cJSON_GetObjectItem(result.meta, "total");
  cJSON *done_j = cJSON_GetObjectItem(result.meta, "done");
  cJSON *remaining_j = cJSON_GetObjectItem(result.meta, "remaining");
  ASSERT_NOT_NULL(total_j);
  ASSERT_NOT_NULL(done_j);
  ASSERT_NOT_NULL(remaining_j);
  ASSERT_EQ((int)total_j->valuedouble, 2);
  ASSERT_EQ((int)done_j->valuedouble, 1);
  ASSERT_EQ((int)remaining_j->valuedouble, 1);

  /* Verify status string */
  cJSON *status = cJSON_GetObjectItem(result.meta, "status");
  ASSERT_NOT_NULL(status);
  ASSERT_STR_CONTAINS(status->valuestring, "1/2");

  tool_result_free(&result);
  cJSON_Delete(params);
  free(alias);
  teardown_ctx();
}

static void test_plan_done_warning(void) {
  setup_ctx();
  create_two_step_plan();

  /* Don't check any steps — call done directly */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result", "task is done");

  tool_result_t result = tool_execute(&g_ctx, "done", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify warning about incomplete plan */
  cJSON *warning = cJSON_GetObjectItem(result.meta, "warning");
  ASSERT_NOT_NULL(warning);
  ASSERT_STR_CONTAINS(warning->valuestring, "WARNING");
  ASSERT_STR_CONTAINS(warning->valuestring, "incomplete");

  tool_result_free(&result);
  cJSON_Delete(params);
  teardown_ctx();
}

static void test_plan_done_no_warning(void) {
  setup_ctx();
  create_two_step_plan();

  /* Check both steps */
  char *alias1 = create_ref_alias();
  cJSON *p1 = cJSON_CreateObject();
  cJSON_AddStringToObject(p1, "op", "check");
  cJSON_AddNumberToObject(p1, "step", 1);
  cJSON_AddStringToObject(p1, "evidence", alias1);
  tool_result_t r1 = tool_execute(&g_ctx, "plan", p1);
  ASSERT(r1.success);
  tool_result_free(&r1);
  cJSON_Delete(p1);
  g_ctx.step++;

  char *alias2 = create_ref_alias();
  cJSON *p2 = cJSON_CreateObject();
  cJSON_AddStringToObject(p2, "op", "check");
  cJSON_AddNumberToObject(p2, "step", 2);
  cJSON_AddStringToObject(p2, "evidence", alias2);
  tool_result_t r2 = tool_execute(&g_ctx, "plan", p2);
  ASSERT(r2.success);
  tool_result_free(&r2);
  cJSON_Delete(p2);
  g_ctx.step++;

  /* Now call done — should have no warning */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result", "all done");

  tool_result_t result = tool_execute(&g_ctx, "done", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify no warning */
  cJSON *warning = cJSON_GetObjectItem(result.meta, "warning");
  ASSERT_NULL(warning);

  tool_result_free(&result);
  cJSON_Delete(params);
  free(alias1);
  free(alias2);
  teardown_ctx();
}

static void test_plan_backward_compat(void) {
  setup_ctx();

  /* Old API: just result param, no op */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result",
                          "1. first step\n2. second step");

  tool_result_t result = tool_execute(&g_ctx, "plan", params);
  ASSERT(result.success);
  ASSERT_NOT_NULL(result.meta);

  /* Verify steps were parsed */
  cJSON *steps_j = cJSON_GetObjectItem(result.meta, "steps");
  ASSERT_NOT_NULL(steps_j);
  ASSERT_EQ((int)steps_j->valuedouble, 2);

  /* Verify scratchpad has plan section */
  int idx = scratchpad_find(&g_ctx.scratch, "plan");
  ASSERT(idx >= 0);
  ASSERT_STR_CONTAINS(g_ctx.scratch.sections[idx].content, "first step");

  tool_result_free(&result);
  cJSON_Delete(params);
  teardown_ctx();
}

/* Helper: record a subtask spawn in the parent journal.  The parent is
 * blocked while the child runs, so the plan active_step at this journal
 * point equals the spawn-time active step. */
static void journal_subtask_spawn(const char *child_dir) {
  cJSON *sp = cJSON_CreateObject();
  cJSON_AddStringToObject(sp, "child_dir", child_dir);
  journal_append(g_ctx.journal, 0, g_ctx.step, "subtask", sp,
                 NULL, 0, 0, NULL, NULL, 0);
  cJSON_Delete(sp);
  g_ctx.step++;
}

/* Helper: create a child plan in session_dir/subtask_N/journal.jsonl */
static void create_child_plan(const char *child_dir, const char *result) {
  char dir[512];
  snprintf(dir, sizeof(dir), "%s/%s", g_tmpdir, child_dir);
  mkdir(dir, 0755);
  journal_t *cj = journal_new(dir);
  cJSON *cp = cJSON_CreateObject();
  cJSON_AddStringToObject(cp, "result", result);
  journal_append(cj, 0, 0, "plan", cp, NULL, 0, 0, NULL, NULL, 0);
  cJSON_Delete(cp);
  journal_free(cj);
}

/* Helper: re-project the scratchpad by checking step 1 with valid evidence */
static void reproject_via_check(void) {
  char *alias = create_ref_alias();
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "op", "check");
  cJSON_AddNumberToObject(params, "step", 1);
  cJSON_AddStringToObject(params, "evidence", alias);
  tool_result_t r = tool_execute(&g_ctx, "plan", params);
  ASSERT(r.success);
  tool_result_free(&r);
  cJSON_Delete(params);
  free(alias);
}

static void test_plan_subtask_links(void) {
  setup_ctx();

  /* Create a 3-step plan (active_step = 1) */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result",
                          "1. step one\n2. step two\n3. step three");
  tool_result_t r = tool_execute(&g_ctx, "plan", params);
  ASSERT(r.success);
  tool_result_free(&r);
  cJSON_Delete(params);
  g_ctx.step++;

  /* Spawn subtask_0 while step 1 is active */
  journal_subtask_spawn("subtask_0");

  /* Check step 1 (active_step becomes 2) */
  reproject_via_check();

  /* Spawn subtask_1 while step 2 is active */
  journal_subtask_spawn("subtask_1");

  /* Derive links from the journal */
  cJSON *links = plan_subtask_links(g_tmpdir);
  ASSERT_NOT_NULL(links);
  cJSON *l0 = cJSON_GetObjectItem(links, "subtask_0");
  ASSERT_NOT_NULL(l0);
  ASSERT_EQ((int)l0->valuedouble, 1);
  cJSON *l1 = cJSON_GetObjectItem(links, "subtask_1");
  ASSERT_NOT_NULL(l1);
  ASSERT_EQ((int)l1->valuedouble, 2);
  cJSON_Delete(links);

  teardown_ctx();
}

static void test_plan_subtask_interleaved(void) {
  setup_ctx();

  /* Create a 3-step plan (active_step = 1) */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result",
                          "1. step one\n2. step two\n3. step three");
  tool_result_t r = tool_execute(&g_ctx, "plan", params);
  ASSERT(r.success);
  tool_result_free(&r);
  cJSON_Delete(params);
  g_ctx.step++;

  /* Spawn subtask_0 while step 1 is active */
  journal_subtask_spawn("subtask_0");

  /* Create the child's plan in subtask_0/ */
  create_child_plan("subtask_0", "1. child one\n2. child two");

  /* Re-project via check step 1 */
  reproject_via_check();

  /* Verify interleaved N.M rendering in the scratchpad */
  int idx = scratchpad_find(&g_ctx.scratch, "plan");
  ASSERT(idx >= 0);
  const char *content = g_ctx.scratch.sections[idx].content;
  ASSERT_STR_CONTAINS(content, "1.1. child one");
  ASSERT_STR_CONTAINS(content, "1.2. child two");
  /* A linked subtask must NOT also appear as an unlinked "Subtask 0:" block */
  ASSERT(!strstr(content, "Subtask 0:"));

  teardown_ctx();
}

static void test_plan_subtask_unlinked(void) {
  setup_ctx();

  /* Create a child plan in subtask_0/ with no parent subtask entry, so
   * the subtask is unlinked (spawned before any plan / no journal link). */
  create_child_plan("subtask_0", "1. child one\n2. child two");

  /* Create a 2-step parent plan (no subtask entry in the parent journal) */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "result", "1. step one\n2. step two");
  tool_result_t r = tool_execute(&g_ctx, "plan", params);
  ASSERT(r.success);
  tool_result_free(&r);
  cJSON_Delete(params);
  g_ctx.step++;

  /* Verify the unlinked subtask is rendered as a "Subtask 0:" block */
  int idx = scratchpad_find(&g_ctx.scratch, "plan");
  ASSERT(idx >= 0);
  const char *content = g_ctx.scratch.sections[idx].content;
  ASSERT_STR_CONTAINS(content, "Subtask 0:");
  ASSERT_STR_CONTAINS(content, "1. child one");
  /* An unlinked subtask must NOT be interleaved as N.M */
  ASSERT(!strstr(content, "1.1."));

  teardown_ctx();
}

int main(void) {
  printf("test_plan_tracking\n");

  RUN_TEST(test_plan_create);
  RUN_TEST(test_plan_check);
  RUN_TEST(test_plan_check_requires_evidence);
  RUN_TEST(test_plan_check_requires_valid_ref);
  RUN_TEST(test_plan_uncheck);
  RUN_TEST(test_plan_status);
  RUN_TEST(test_plan_done_warning);
  RUN_TEST(test_plan_done_no_warning);
  RUN_TEST(test_plan_backward_compat);
  RUN_TEST(test_plan_subtask_links);
  RUN_TEST(test_plan_subtask_interleaved);
  RUN_TEST(test_plan_subtask_unlinked);

  TEST_SUMMARY();
}

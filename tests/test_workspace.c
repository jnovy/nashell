#include "test_common.h"
#include "../src/workspace.h"
#include "../src/memory.h"

/* ---- helpers ---- */

/* Create a workspace with separate ws and global dirs under a temp root. */
static workspace_t *make_test_workspace(const char *root) {
  /* workspace_new expects a nash_dir and creates subdirs.
   * We pass root as nash_dir and "testws" as workspace name. */
  workspace_t *ws = workspace_new(root, "testws", 0, 0.8);
  return ws;
}

/* ---- Phase 1 tests: is_global tagging ---- */

static void test_recall_tags_global(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);
  ASSERT_NOT_NULL(ws->workspace);
  ASSERT_NOT_NULL(ws->global);

  /* Store a skill in workspace layer */
  int rc = memory_store(ws->workspace, "skill:local-thing",
                        "A workspace-local skill", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  /* Store a skill in global layer */
  rc = memory_store(ws->global, "skill:global-thing",
                    "A global skill", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  /* Recall - both should come back */
  memory_results_t results = workspace_recall(ws, "skill thing", 10);
  ASSERT_GT(results.count, 0);

  /* Check is_global flags */
  int found_local = 0, found_global = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "skill:local-thing") == 0) {
      ASSERT_EQ(results.entries[i].is_global, 0);
      found_local = 1;
    }
    if (strcmp(results.entries[i].key, "skill:global-thing") == 0) {
      ASSERT_EQ(results.entries[i].is_global, 1);
      found_global = 1;
    }
  }
  ASSERT(found_local);
  ASSERT(found_global);

  memory_results_free(&results);
  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

static void test_recall_global_only_tags(void) {
  /* When no workspace layer is active, all results should be is_global=1 */
  char *dir = make_test_dir();
  workspace_t *ws = workspace_new(dir, NULL, 0, 0.8);
  ASSERT_NOT_NULL(ws);
  ASSERT_NULL(ws->workspace);  /* no workspace layer */
  ASSERT_NOT_NULL(ws->global);

  int rc = memory_store(ws->global, "skill:global-only",
                        "A global skill", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  memory_results_t results = workspace_recall(ws, "global skill", 10);
  ASSERT_GT(results.count, 0);

  for (int i = 0; i < results.count; i++) {
    ASSERT_EQ(results.entries[i].is_global, 1);
  }

  memory_results_free(&results);
  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

static void test_recall_dedup_preserves_winner_flag(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);

  /* Store same key in both layers - workspace should win (higher relevance) */
  int rc = memory_store(ws->workspace, "skill:shared",
                        "workspace version of shared skill", 0, NULL,
                        NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);
  rc = memory_store(ws->global, "skill:shared",
                    "global version of shared skill", 0, NULL,
                    NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  memory_results_t results = workspace_recall(ws, "shared skill", 10);

  /* Should be deduplicated - only one entry for "skill:shared" */
  int shared_count = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "skill:shared") == 0) {
      shared_count++;
      /* Workspace version has no global_weight discount so it wins.
       * Winner's is_global flag should be preserved (0 = workspace). */
      ASSERT_EQ(results.entries[i].is_global, 0);
    }
  }
  ASSERT_EQ(shared_count, 1);

  memory_results_free(&results);
  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

/* ---- Phase 2 tests: workspace_promote / workspace_demote ---- */

static void test_promote_moves_entry(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);

  int rc = memory_store(ws->workspace, "skill:promote-me",
                        "A skill to promote", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  /* Verify it exists in workspace */
  memory_t *found = workspace_find_memory(ws, "skill:promote-me");
  ASSERT(found == ws->workspace);

  /* Promote */
  rc = workspace_promote(ws, "skill:promote-me");
  ASSERT_EQ(rc, 0);

  /* Should now be in global, not workspace */
  found = workspace_find_memory(ws, "skill:promote-me");
  ASSERT(found == ws->global);

  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

static void test_promote_nonexistent_fails(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);

  int rc = workspace_promote(ws, "skill:does-not-exist");
  ASSERT_EQ(rc, -1);

  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

static void test_promote_already_global_fails(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);

  /* Store only in global */
  int rc = memory_store(ws->global, "skill:already-global",
                        "Already global", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  /* Promote should fail - not in workspace layer */
  rc = workspace_promote(ws, "skill:already-global");
  ASSERT_EQ(rc, -1);

  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

static void test_demote_moves_entry(void) {
  char *dir = make_test_dir();
  workspace_t *ws = make_test_workspace(dir);
  ASSERT_NOT_NULL(ws);

  int rc = memory_store(ws->global, "skill:demote-me",
                        "A skill to demote", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  /* Verify it exists in global */
  memory_t *found = workspace_find_memory(ws, "skill:demote-me");
  ASSERT(found == ws->global);

  /* Demote */
  rc = workspace_demote(ws, "skill:demote-me");
  ASSERT_EQ(rc, 0);

  /* Should now be in workspace, not global */
  found = workspace_find_memory(ws, "skill:demote-me");
  ASSERT(found == ws->workspace);

  workspace_free(ws);
  rm_rf(dir);
  free(dir);
}

/* ---- main ---- */

int main(void) {
  printf("\ntest_workspace\n");

  printf("\n  --- is_global Tagging ---\n");
  RUN_TEST(test_recall_tags_global);
  RUN_TEST(test_recall_global_only_tags);
  RUN_TEST(test_recall_dedup_preserves_winner_flag);

  printf("\n  --- Promote/Demote ---\n");
  RUN_TEST(test_promote_moves_entry);
  RUN_TEST(test_promote_nonexistent_fails);
  RUN_TEST(test_promote_already_global_fails);
  RUN_TEST(test_demote_moves_entry);

  TEST_SUMMARY();
}

#include "test_common.h"
#include "../src/store.h"
#include "../src/tools.h"
#include <errno.h>

/* Minimal slurp for test verification */
static char *test_slurp(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)sz + 1);
  size_t n = fread(buf, 1, (size_t)sz, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

static void test_write(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  ASSERT_NOT_NULL(f);
  fputs(content, f);
  fclose(f);
}

/* ── test_store_load ── */
static void test_store_load(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  ASSERT_NOT_NULL(s);

  const char *content = "hello store_load test";
  char *hash = store_save(s, content);
  ASSERT_NOT_NULL(hash);

  char *loaded = store_load(s, hash);
  ASSERT_NOT_NULL(loaded);
  ASSERT_STR_EQ(loaded, content);

  free(loaded);
  free(hash);

  /* Non-existent hash returns NULL */
  char *missing = store_load(s, "0000000000000000000000000000000000000000000000000000000000000000");
  ASSERT_NULL(missing);

  /* NULL safety */
  ASSERT_NULL(store_load(NULL, "abc"));
  ASSERT_NULL(store_load(s, NULL));

  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_txn_record_dedup ── */
static void test_txn_record_dedup(void) {
  tool_ctx_t ctx = {0};
  ctx.step = 1;

  /* First record for a path should succeed */
  tool_txn_record(&ctx, "/tmp/test_a.txt", "hash_aaa", 0);
  ASSERT_EQ(ctx.txn_n_edits, 1);
  ASSERT_STR_EQ(ctx.txn_edits[0].path, "/tmp/test_a.txt");
  ASSERT_STR_EQ(ctx.txn_edits[0].pre_hash, "hash_aaa");
  ASSERT_EQ(ctx.txn_edits[0].is_new_file, 0);

  /* Second record for same path should be deduplicated */
  tool_txn_record(&ctx, "/tmp/test_a.txt", "hash_bbb", 0);
  ASSERT_EQ(ctx.txn_n_edits, 1); /* still 1 */
  ASSERT_STR_EQ(ctx.txn_edits[0].pre_hash, "hash_aaa"); /* original hash kept */

  /* Different path should be added */
  tool_txn_record(&ctx, "/tmp/test_b.txt", NULL, 1);
  ASSERT_EQ(ctx.txn_n_edits, 2);
  ASSERT_NULL(ctx.txn_edits[1].pre_hash);
  ASSERT_EQ(ctx.txn_edits[1].is_new_file, 1);

  tool_txn_clear(&ctx);
  ASSERT_EQ(ctx.txn_n_edits, 0);
}

/* ── test_txn_basic_rollback ── */
static void test_txn_basic_rollback(void) {
  /* Setup: create store and a file with known content */
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  tool_ctx_t ctx = {0};
  ctx.store = s;
  ctx.step = 1;

  char filepath[256];
  snprintf(filepath, sizeof(filepath), "%s/test_file.txt", dir);
  test_write(filepath, "original content");

  /* Record the pre-edit state */
  char *pre_hash = store_save(s, "original content");
  tool_txn_record(&ctx, filepath, pre_hash, 0);
  free(pre_hash);

  /* Simulate editing the file */
  test_write(filepath, "modified content");

  /* Verify file is modified */
  char *check1 = test_slurp(filepath);
  ASSERT_STR_EQ(check1, "modified content");
  free(check1);

  /* Now rollback: manually restore from txn_edits */
  ASSERT_EQ(ctx.txn_n_edits, 1);
  char *content = store_load(s, ctx.txn_edits[0].pre_hash);
  ASSERT_NOT_NULL(content);
  test_write(filepath, content);
  free(content);

  /* Verify file is restored */
  char *check2 = test_slurp(filepath);
  ASSERT_STR_EQ(check2, "original content");
  free(check2);

  tool_txn_clear(&ctx);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_txn_new_file_rollback ── */
static void test_txn_new_file_rollback(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  tool_ctx_t ctx = {0};
  ctx.store = s;
  ctx.step = 1;

  char filepath[256];
  snprintf(filepath, sizeof(filepath), "%s/new_file.txt", dir);

  /* Record as new file and create it */
  tool_txn_record(&ctx, filepath, NULL, 1);
  test_write(filepath, "new file content");

  /* Verify file exists */
  struct stat st;
  ASSERT_EQ(stat(filepath, &st), 0);

  /* Rollback: delete the new file */
  ASSERT_EQ(ctx.txn_edits[0].is_new_file, 1);
  ASSERT_EQ(unlink(filepath), 0);

  /* Verify file is gone */
  ASSERT_EQ(stat(filepath, &st), -1);

  tool_txn_clear(&ctx);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_txn_multi_file_rollback ── */
static void test_txn_multi_file_rollback(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  tool_ctx_t ctx = {0};
  ctx.store = s;
  ctx.step = 1;

  char pathA[256], pathB[256], pathC[256];
  snprintf(pathA, sizeof(pathA), "%s/a.txt", dir);
  snprintf(pathB, sizeof(pathB), "%s/b.txt", dir);
  snprintf(pathC, sizeof(pathC), "%s/c.txt", dir);

  /* Create original files */
  test_write(pathA, "AAA");
  test_write(pathB, "BBB");
  test_write(pathC, "CCC");

  /* Record pre-edit state for all three */
  char *hA = store_save(s, "AAA");
  char *hB = store_save(s, "BBB");
  char *hC = store_save(s, "CCC");
  tool_txn_record(&ctx, pathA, hA, 0);
  tool_txn_record(&ctx, pathB, hB, 0);
  tool_txn_record(&ctx, pathC, hC, 0);
  free(hA); free(hB); free(hC);

  /* Modify all three */
  test_write(pathA, "aaa_modified");
  test_write(pathB, "bbb_modified");
  test_write(pathC, "ccc_modified");

  ASSERT_EQ(ctx.txn_n_edits, 3);

  /* Rollback all in reverse order */
  for (int i = ctx.txn_n_edits - 1; i >= 0; i--) {
    char *content = store_load(s, ctx.txn_edits[i].pre_hash);
    ASSERT_NOT_NULL(content);
    test_write(ctx.txn_edits[i].path, content);
    free(content);
  }

  /* Verify all restored */
  char *cA = test_slurp(pathA);
  char *cB = test_slurp(pathB);
  char *cC = test_slurp(pathC);
  ASSERT_STR_EQ(cA, "AAA");
  ASSERT_STR_EQ(cB, "BBB");
  ASSERT_STR_EQ(cC, "CCC");
  free(cA); free(cB); free(cC);

  tool_txn_clear(&ctx);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_txn_double_edit_same_file ── */
static void test_txn_double_edit_same_file(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  tool_ctx_t ctx = {0};
  ctx.store = s;
  ctx.step = 1;

  char filepath[256];
  snprintf(filepath, sizeof(filepath), "%s/double.txt", dir);
  test_write(filepath, "version_1");

  /* First edit */
  char *h1 = store_save(s, "version_1");
  tool_txn_record(&ctx, filepath, h1, 0);
  free(h1);
  test_write(filepath, "version_2");

  /* Second edit - same path, should be deduped */
  char *h2 = store_save(s, "version_2");
  tool_txn_record(&ctx, filepath, h2, 0);
  free(h2);
  test_write(filepath, "version_3");

  /* Should only have one txn entry with original hash */
  ASSERT_EQ(ctx.txn_n_edits, 1);

  /* Rollback should restore to version_1 (not version_2) */
  char *content = store_load(s, ctx.txn_edits[0].pre_hash);
  ASSERT_NOT_NULL(content);
  ASSERT_STR_EQ(content, "version_1");
  test_write(filepath, content);
  free(content);

  char *check = test_slurp(filepath);
  ASSERT_STR_EQ(check, "version_1");
  free(check);

  tool_txn_clear(&ctx);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_txn_empty_rollback ── */
static void test_txn_empty_rollback(void) {
  tool_ctx_t ctx = {0};
  /* No edits recorded */
  ASSERT_EQ(ctx.txn_n_edits, 0);
  /* tool_txn_clear on empty should not crash */
  tool_txn_clear(&ctx);
  ASSERT_EQ(ctx.txn_n_edits, 0);
}

/* ── test_txn_clear_frees ── */
static void test_txn_clear_frees(void) {
  tool_ctx_t ctx = {0};
  ctx.step = 1;

  tool_txn_record(&ctx, "/tmp/x.txt", "hash_x", 0);
  tool_txn_record(&ctx, "/tmp/y.txt", NULL, 1);
  ASSERT_EQ(ctx.txn_n_edits, 2);

  tool_txn_clear(&ctx);
  ASSERT_EQ(ctx.txn_n_edits, 0);

  /* Can re-use after clear */
  tool_txn_record(&ctx, "/tmp/z.txt", "hash_z", 0);
  ASSERT_EQ(ctx.txn_n_edits, 1);
  ASSERT_STR_EQ(ctx.txn_edits[0].path, "/tmp/z.txt");

  tool_txn_clear(&ctx);
}

int main(void) {
  printf("test_rollback:\n");
  RUN_TEST(test_store_load);
  RUN_TEST(test_txn_record_dedup);
  RUN_TEST(test_txn_basic_rollback);
  RUN_TEST(test_txn_new_file_rollback);
  RUN_TEST(test_txn_multi_file_rollback);
  RUN_TEST(test_txn_double_edit_same_file);
  RUN_TEST(test_txn_empty_rollback);
  RUN_TEST(test_txn_clear_frees);
  TEST_SUMMARY();
}

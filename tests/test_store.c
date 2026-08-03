#include "test_common.h"
#include "../src/store.h"
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */

/* ── test_sha256_hex ── */
static void test_sha256_hex(void) {
  /* Known SHA256 of "hello world" */
  char *hex = sha256_hex("hello world", 11);
  ASSERT_NOT_NULL(hex);
  ASSERT_STR_EQ(hex, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
  free(hex);
}

/* ── test_save_and_resolve ── */
static void test_save_and_resolve(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  ASSERT_NOT_NULL(s);

  char *hash = store_save(s, "test content");
  ASSERT_NOT_NULL(hash);
  ASSERT_GT((int)strlen(hash), 0);

  /* Resolve to path */
  char *path = store_resolve(s, hash);
  ASSERT_NOT_NULL(path);

  /* Verify file exists */
  struct stat st;
  ASSERT_EQ(stat(path, &st), 0);
  ASSERT_GT((int)st.st_size, 0);

  free(hash);
  free(path);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_dedup ── */
static void test_dedup(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);

  char *hash1 = store_save(s, "same content");
  char *hash2 = store_save(s, "same content");

  /* Same content = same hash */
  ASSERT_STR_EQ(hash1, hash2);

  free(hash1);
  free(hash2);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_different_content ── */
static void test_different_content(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);

  char *hash1 = store_save(s, "content A");
  char *hash2 = store_save(s, "content B");

  /* Different content = different hash */
  ASSERT(strcmp(hash1, hash2) != 0);

  free(hash1);
  free(hash2);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_empty_content ── */
static void test_empty_content(void) {
  char *dir = make_test_dir();
  store_t *s = store_new(dir);

  char *hash = store_save(s, "");
  ASSERT_NOT_NULL(hash); /* empty string still has a hash */

  free(hash);
  store_free(s);
  rm_rf(dir);
  free(dir);
}

/* ── test_null_safety ── */
static void test_null_safety(void) {
  char *hash = store_save(NULL, "test");
  ASSERT_NULL(hash);

  char *dir = make_test_dir();
  store_t *s = store_new(dir);
  hash = store_save(s, NULL);
  ASSERT_NULL(hash);

  store_free(s);
  rm_rf(dir);
  free(dir);
}

int main(void) {
  printf("test_store:\n");
  RUN_TEST(test_sha256_hex);
  RUN_TEST(test_save_and_resolve);
  RUN_TEST(test_dedup);
  RUN_TEST(test_different_content);
  RUN_TEST(test_empty_content);
  RUN_TEST(test_null_safety);
  TEST_SUMMARY();
}

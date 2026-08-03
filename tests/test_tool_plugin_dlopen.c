/* test_tool_plugin_dlopen.c - Tests for external plugin loading via dlopen.
 *
 * Verifies:
 *   1. Loading a single-tool .so plugin (tool_plugin_load)
 *   2. ABI version mismatch rejection
 *   3. Loading a multi-tool .so plugin (shared dlhandle tracking)
 *   4. Loading all .so files from a directory (tool_plugin_load_dir)
 *   5. Unloading a single plugin (tool_plugin_unload)
 *   6. Unloading one tool from a multi-tool .so (dlclose deferred)
 *   7. Static plugin cannot be unloaded
 *   8. Plugin cleanup (tool_plugin_cleanup)
 *   9. Sort preserves dlhandle association
 *
 * Requires: tests/sample_plugin.so, tests/sample_plugin_bad_abi.so,
 *           tests/sample_plugin_multi.so (built by Makefile)
 */

#include "test_common.h"
#include "tool_plugin.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Provide globals that linked modules reference */
int g_path_given = 0;

/* Helper: get path relative to test binary location.
 * Tests are run from the project root, so "tests/X.so" works. */
static const char *SAMPLE_SO = "tests/sample_plugin.so";
static const char *BAD_ABI_SO = "tests/sample_plugin_bad_abi.so";
static const char *MULTI_SO = "tests/sample_plugin_multi.so";
static const char *PLUGIN_DIR = "tests/plugin_dir";

/* ---- Helper to set up a temp plugin directory ---- */
static void setup_plugin_dir(void) {
  mkdir(PLUGIN_DIR, 0755);
  /* Symlink sample_plugin.so and multi .so into the dir */
  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "ln -sf $(pwd)/tests/sample_plugin.so %s/sample_plugin.so && "
           "ln -sf $(pwd)/tests/sample_plugin_multi.so %s/sample_plugin_multi.so",
           PLUGIN_DIR, PLUGIN_DIR);
  system(cmd);
}

static void teardown_plugin_dir(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", PLUGIN_DIR);
  system(cmd);
}

/* ---- Tests ---- */

static void test_load_single(void) {
  tool_plugin_clear();

  /* Verify .so exists */
  struct stat st;
  ASSERT_EQ(stat(SAMPLE_SO, &st), 0);

  int rc = tool_plugin_load(SAMPLE_SO);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 1);

  const tool_plugin_t *p = tool_plugin_find("sample_hello");
  ASSERT_NOT_NULL(p);
  ASSERT_STR_EQ(p->name, "sample_hello");
  ASSERT_STR_EQ(p->version, "1.0.0");
  ASSERT_EQ(p->abi_version, TOOL_PLUGIN_ABI_VERSION);
  ASSERT_NOT_NULL(p->execute);

  tool_plugin_cleanup();
  tool_plugin_clear();
}

static void test_load_bad_abi(void) {
  tool_plugin_clear();

  struct stat st;
  ASSERT_EQ(stat(BAD_ABI_SO, &st), 0);

  /* dlopen succeeds but constructor's register call fails due to ABI check,
     * then tool_plugin_load sees no new plugins and returns -1. */
  int rc = tool_plugin_load(BAD_ABI_SO);
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(tool_plugin_count(), 0);

  tool_plugin_clear();
}

static void test_load_multi(void) {
  tool_plugin_clear();

  struct stat st;
  ASSERT_EQ(stat(MULTI_SO, &st), 0);

  int rc = tool_plugin_load(MULTI_SO);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 2);

  const tool_plugin_t *a = tool_plugin_find("multi_alpha");
  const tool_plugin_t *b = tool_plugin_find("multi_beta");
  ASSERT_NOT_NULL(a);
  ASSERT_NOT_NULL(b);
  ASSERT_STR_EQ(a->group, "multi_sample");
  ASSERT_STR_EQ(b->group, "multi_sample");

  tool_plugin_cleanup();
  tool_plugin_clear();
}

static void test_load_dir(void) {
  tool_plugin_clear();
  setup_plugin_dir();

  int loaded = tool_plugin_load_dir(PLUGIN_DIR);
  ASSERT(loaded >= 2);
  /* sample_plugin.so registers 1 + sample_plugin_multi.so registers 2 = 3 */
  ASSERT(tool_plugin_count() >= 3);

  /* Verify specific tools exist */
  ASSERT_NOT_NULL(tool_plugin_find("sample_hello"));
  ASSERT_NOT_NULL(tool_plugin_find("multi_alpha"));
  ASSERT_NOT_NULL(tool_plugin_find("multi_beta"));

  tool_plugin_cleanup();
  tool_plugin_clear();
  teardown_plugin_dir();
}

static void test_load_nonexistent_dir(void) {
  tool_plugin_clear();

  int rc = tool_plugin_load_dir("/tmp/nash_nonexistent_plugin_dir_xyz");
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(tool_plugin_count(), 0);

  tool_plugin_clear();
}

static void test_load_nonexistent_so(void) {
  tool_plugin_clear();

  int rc = tool_plugin_load("/tmp/nash_nonexistent_plugin.so");
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(tool_plugin_count(), 0);

  tool_plugin_clear();
}

static void test_unload_single(void) {
  tool_plugin_clear();

  tool_plugin_load(SAMPLE_SO);
  ASSERT_EQ(tool_plugin_count(), 1);

  int rc = tool_plugin_unload("sample_hello");
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 0);
  ASSERT_NULL(tool_plugin_find("sample_hello"));

  tool_plugin_clear();
}

static void test_unload_one_from_multi(void) {
  tool_plugin_clear();

  tool_plugin_load(MULTI_SO);
  ASSERT_EQ(tool_plugin_count(), 2);

  /* Unload one - dlhandle should NOT be closed (other tool still uses it) */
  int rc = tool_plugin_unload("multi_alpha");
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 1);
  ASSERT_NULL(tool_plugin_find("multi_alpha"));
  ASSERT_NOT_NULL(tool_plugin_find("multi_beta"));

  /* Unload the last one - NOW dlhandle should be closed */
  rc = tool_plugin_unload("multi_beta");
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 0);

  tool_plugin_clear();
}

static void test_unload_static_rejected(void) {
  tool_plugin_clear();

  /* Register a static plugin (no dlhandle) */
  static const tool_plugin_t static_p = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "static_test",
    .version = "1.0.0",
    .description = "static tool",
    .params = NULL,
    .execute = NULL,
    .caps = 0,
    .group = NULL,
  };
  tool_plugin_register(&static_p);
  ASSERT_EQ(tool_plugin_count(), 1);

  /* Try to unload - should fail */
  int rc = tool_plugin_unload("static_test");
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(tool_plugin_count(), 1);

  tool_plugin_clear();
}

static void test_sort_preserves_dlhandle(void) {
  tool_plugin_clear();

  /* Register a static plugin first (sorts before 'sample') */
  static const tool_plugin_t aaa_p = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "aaa_first",
    .version = "1.0.0",
    .description = "sorts first",
    .params = NULL,
    .execute = NULL,
    .caps = 0,
    .group = NULL,
  };
  tool_plugin_register(&aaa_p);

  /* Load dynamic plugin (name "sample_hello" sorts after "aaa_first") */
  tool_plugin_load(SAMPLE_SO);
  ASSERT_EQ(tool_plugin_count(), 2);

  /* Sort */
  tool_plugin_sort();

  /* After sort, aaa_first should be at index 0, sample_hello at index 1 */
  const tool_plugin_t *p0 = tool_plugin_get(0);
  const tool_plugin_t *p1 = tool_plugin_get(1);
  ASSERT_STR_EQ(p0->name, "aaa_first");
  ASSERT_STR_EQ(p1->name, "sample_hello");

  /* Unload should still work (dlhandle association preserved through sort) */
  int rc = tool_plugin_unload("sample_hello");
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(tool_plugin_count(), 1);

  /* Static unload should still fail */
  rc = tool_plugin_unload("aaa_first");
  ASSERT_EQ(rc, -1);

  tool_plugin_clear();
}

static void test_cleanup(void) {
  tool_plugin_clear();

  /* Load two .so files */
  tool_plugin_load(SAMPLE_SO);
  tool_plugin_load(MULTI_SO);
  ASSERT_EQ(tool_plugin_count(), 3);

  /* Cleanup closes all handles but keeps registry entries */
  tool_plugin_cleanup();

  /* Plugins are still in the registry (stale pointers, but count unchanged) */
  ASSERT_EQ(tool_plugin_count(), 3);

  /* Clear to reset for next test */
  tool_plugin_clear();
}

static void test_null_args(void) {
  ASSERT_EQ(tool_plugin_load(NULL), -1);
  ASSERT_EQ(tool_plugin_load_dir(NULL), -1);
  ASSERT_EQ(tool_plugin_unload(NULL), -1);
  ASSERT_EQ(tool_plugin_unload("nonexistent"), -1);
}

/* ---- Main ---- */

int main(void) {
  printf("test_tool_plugin_dlopen\n");

  RUN_TEST(test_load_single);
  RUN_TEST(test_load_bad_abi);
  RUN_TEST(test_load_multi);
  RUN_TEST(test_load_dir);
  RUN_TEST(test_load_nonexistent_dir);
  RUN_TEST(test_load_nonexistent_so);
  RUN_TEST(test_unload_single);
  RUN_TEST(test_unload_one_from_multi);
  RUN_TEST(test_unload_static_rejected);
  RUN_TEST(test_sort_preserves_dlhandle);
  RUN_TEST(test_cleanup);
  RUN_TEST(test_null_args);

  TEST_SUMMARY();
}

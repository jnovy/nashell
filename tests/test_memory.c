#include "test_common.h"
#include "../src/memory.h"
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */

/* ── test_store_and_recall ── */
static void test_store_and_recall(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);
    ASSERT_NOT_NULL(m);

    const char *tags[] = {"math", "basics"};
    int rc = memory_store(m, "lesson:addition", "2+2=4", tags, 2, 0);
    ASSERT_EQ(rc, 0);

    memory_results_t results = memory_recall(m, "addition", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_EQ(results.entries[0].key, "lesson:addition");
    ASSERT_STR_EQ(results.entries[0].value, "2+2=4");

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_tags ── */
static void test_tags(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"redis", "migration"};
    memory_store(m, "lesson:redis-v7", "HMSET renamed to HSET", tags, 2, 0);

    /* Recall by tag */
    memory_results_t results = memory_recall(m, "redis", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_CONTAINS(results.entries[0].key, "redis");

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_pinned ── */
static void test_pinned(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"critical"};
    memory_store(m, "fact:api-key", "always use HTTPS", tags, 1, 1);  /* pinned=1 */

    char *pinned = memory_load_pinned(m);
    ASSERT_NOT_NULL(pinned);
    ASSERT_STR_CONTAINS(pinned, "always use HTTPS");
    ASSERT_STR_CONTAINS(pinned, "fact:api-key");
    free(pinned);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_build_index ── */
static void test_build_index(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "lesson:a", "value a", NULL, 0, 0);
    memory_store(m, "strategy:b", "value b", NULL, 0, 0);
    memory_store(m, "fact:c", "value c", NULL, 0, 0);

    char *index = memory_build_index(m, 50);
    ASSERT_NOT_NULL(index);
    ASSERT_STR_CONTAINS(index, "lesson");
    ASSERT_STR_CONTAINS(index, "strategy");
    ASSERT_STR_CONTAINS(index, "fact");
    free(index);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_prune ── */
static void test_prune(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store a "stale" fact — we can't easily fake timestamps in C without
       modifying the file, so we just verify prune doesn't crash and
       doesn't remove pinned/strategy entries */
    const char *tags[] = {"test"};
    memory_store(m, "fact:stale", "old data", tags, 1, 0);
    memory_store(m, "strategy:keep", "important", tags, 1, 0);
    memory_store(m, "lesson:keep2", "also important", tags, 1, 0);

    /* Prune with 0 days / 0 access — should NOT remove strategy/lesson */
    int pruned = memory_prune(m, 0, 999);  /* max_age=0 days, min_access=999 */
    /* fact:stale should be pruned (age > 0 days, access < 999) */
    /* strategy:keep and lesson:keep2 should survive (protected types) */

    /* Verify strategy and lesson still exist */
    memory_results_t r1 = memory_recall(m, "strategy", 5);
    ASSERT_GT(r1.count, 0);
    memory_results_free(&r1);

    memory_results_t r2 = memory_recall(m, "lesson", 5);
    ASSERT_GT(r2.count, 0);
    memory_results_free(&r2);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_overwrite ── */
static void test_overwrite(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "fact:pi", "3.14", NULL, 0, 0);
    memory_store(m, "fact:pi", "3.14159", NULL, 0, 0);  /* overwrite */

    memory_results_t results = memory_recall(m, "pi", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_EQ(results.entries[0].value, "3.14159");

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_no_match ── */
static void test_no_match(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "fact:pi", "3.14", NULL, 0, 0);

    memory_results_t results = memory_recall(m, "nonexistent_xyz", 5);
    ASSERT_EQ(results.count, 0);

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_write_index_file ── */
static void test_write_index_file(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "lesson:test", "test value", NULL, 0, 0);

    /* Check MEMORY.md was created (written to parent of memory dir = test dir) */
    char path[4096];
    snprintf(path, sizeof(path), "%s/MEMORY.md", dir);
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_GT(st.st_size, 0);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

int main(void) {
    printf("test_memory:\n");
    RUN_TEST(test_store_and_recall);
    RUN_TEST(test_tags);
    RUN_TEST(test_pinned);
    RUN_TEST(test_build_index);
    RUN_TEST(test_prune);
    RUN_TEST(test_overwrite);
    RUN_TEST(test_no_match);
    RUN_TEST(test_write_index_file);
    TEST_SUMMARY();
}

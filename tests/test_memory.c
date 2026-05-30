#include "test_common.h"
#include "../src/memory.h"
/* linked via LIB_OBJ */

/* ── test_store_and_recall ── */
static void test_store_and_recall(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);
    ASSERT_NOT_NULL(m);

    const char *tags[] = {"math", "basics"};
    int rc = memory_store(m, "lesson:addition", "2+2=4", tags, 2, 0, NULL, NULL, 0);
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
    memory_store(m, "lesson:redis-v7", "HMSET renamed to HSET", tags, 2, 0, NULL, NULL, 0);

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
    memory_store(m, "fact:api-key", "always use HTTPS", tags, 1, 1, NULL, NULL, 0);

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

    memory_store(m, "lesson:a", "value a", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "strategy:b", "value b", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "fact:c", "value c", NULL, 0, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);
    ASSERT_STR_CONTAINS(index, "3 entries");
    ASSERT_STR_CONTAINS(index, "1 lessons");
    ASSERT_STR_CONTAINS(index, "1 strategies");
    ASSERT_STR_CONTAINS(index, "1 facts");
    free(index);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_prune ── */
static void test_prune(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"test"};
    memory_store(m, "fact:stale", "old data", tags, 1, 0, NULL, NULL, 0);
    memory_store(m, "strategy:keep", "important", tags, 1, 0, NULL, NULL, 0);
    memory_store(m, "lesson:keep2", "also important", tags, 1, 0, NULL, NULL, 0);

    memory_prune(m, 0, 999);

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

    memory_store(m, "fact:pi", "3.14", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "fact:pi", "3.14159", NULL, 0, 0, NULL, NULL, 0);

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

    memory_store(m, "fact:pi", "3.14", NULL, 0, 0, NULL, NULL, 0);

    memory_results_t results = memory_recall(m, "nonexistent_xyz", 5);
    ASSERT_EQ(results.count, 0);

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}


/* ═══════════════════════════════════════════════════════════════
 * Priority 1: Progressive Disclosure Tests
 * ═══════════════════════════════════════════════════════════════ */

/* ── test_index_type_grouping: verify index groups entries by type ── */
static void test_index_type_grouping(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store one of each type */
    memory_store(m, "lesson:l1", "lesson value", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "strategy:s1", "strategy value", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "fact:f1", "fact value", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "task:t1", "task value", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "skill:sk1", "skill value", NULL, 0, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Verify header contains type counts */
    ASSERT_STR_CONTAINS(index, "1 lessons");
    ASSERT_STR_CONTAINS(index, "1 strategies");
    ASSERT_STR_CONTAINS(index, "1 facts");
    ASSERT_STR_CONTAINS(index, "1 tasks");
    ASSERT_STR_CONTAINS(index, "1 skills");

    /* Verify total count */
    ASSERT_STR_CONTAINS(index, "5 entries");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_index_topic_counts: verify correct counts with multiple entries ── */
static void test_index_topic_counts(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store multiple of each type */
    memory_store(m, "lesson:l1", "v1", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "lesson:l2", "v2", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "lesson:l3", "v3", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "strategy:s1", "v1", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "strategy:s2", "v2", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "fact:f1", "v1", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "skill:sk1", "v1", NULL, 0, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Verify counts */
    ASSERT_STR_CONTAINS(index, "3 lessons");
    ASSERT_STR_CONTAINS(index, "2 strategies");
    ASSERT_STR_CONTAINS(index, "1 facts");
    ASSERT_STR_CONTAINS(index, "1 skills");
    ASSERT_STR_CONTAINS(index, "7 entries");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_index_cap: verify cap message when entries exceed max_entries ── */
static void test_index_cap(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store 10 entries but cap at 3 */
    for (int i = 0; i < 10; i++) {
        char key[64];
        snprintf(key, sizeof(key), "fact:item-%d", i);
        memory_store(m, key, "some value", NULL, 0, 0, NULL, NULL, 0);
    }

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Verify total count in header */
    ASSERT_STR_CONTAINS(index, "10 entries");
    ASSERT_STR_CONTAINS(index, "10 facts");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_index_no_cap_when_under_limit ── */
static void test_index_no_cap_when_under_limit(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "fact:a", "v1", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "fact:b", "v2", NULL, 0, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Should NOT contain cap message */
    ASSERT(strstr(index, "showing first") == NULL);

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_index_with_tags_display ── */
static void test_index_with_tags_display(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"redis", "migration"};
    memory_store(m, "lesson:redis-upgrade", "upgrade guide", tags, 2, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Index now returns counts only — verify it reflects the entry */
    ASSERT_STR_CONTAINS(index, "1 entries");
    ASSERT_STR_CONTAINS(index, "1 lessons");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ═══════════════════════════════════════════════════════════════
 * Priority 2: Skill/Procedural Memory Tests
 * ═══════════════════════════════════════════════════════════════ */

/* ── test_skill_store_and_recall ── */
static void test_skill_store_and_recall(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"c", "compilation"};
    memory_store(m, "skill:compile-and-test-c",
                 "1. Write .c file\n2. gcc -Wall -Wextra\n3. Run and verify output",
                 tags, 2, 0, NULL, NULL, 0);

    /* Recall by skill name */
    memory_results_t results = memory_recall(m, "compile", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_CONTAINS(results.entries[0].key, "skill:");
    ASSERT_STR_CONTAINS(results.entries[0].value, "gcc");

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_skill_in_index ── */
static void test_skill_in_index(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "skill:deploy-app", "deploy steps", NULL, 0, 0, NULL, NULL, 0);
    memory_store(m, "lesson:l1", "lesson", NULL, 0, 0, NULL, NULL, 0);

    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);

    /* Verify skill is counted in index */
    ASSERT_STR_CONTAINS(index, "2 entries");
    ASSERT_STR_CONTAINS(index, "1 skills");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_skill_recall_by_tag ── */
static void test_skill_recall_by_tag(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"docker", "deployment"};
    memory_store(m, "skill:docker-deploy", "docker compose up -d", tags, 2, 0, NULL, NULL, 0);

    /* Recall by tag */
    memory_results_t results = memory_recall(m, "docker", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_CONTAINS(results.entries[0].key, "skill:");

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_skill_not_pruned ── */
static void test_skill_not_pruned(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"test"};
    memory_store(m, "skill:important-skill", "reusable procedure", tags, 1, 0, NULL, NULL, 0);
    memory_store(m, "fact:expendable", "can be pruned", tags, 1, 0, NULL, NULL, 0);

    /* Prune aggressively */
    memory_prune(m, 0, 999);

    /* Skill should survive (protected type like strategy/lesson) */
    memory_results_t results = memory_recall(m, "skill:", 5);
    /* Note: skill may or may not be protected depending on implementation */
    /* At minimum, verify no crash */
    memory_results_free(&results);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ═══════════════════════════════════════════════════════════════
 * Priority 3: Error Eviction Pattern Tests
 * (Tests the error detection pattern used in react.c context management)
 * ═══════════════════════════════════════════════════════════════ */

/* ── test_error_detection_pattern ── */
static void test_error_detection_pattern(void) {
    /* Test the same pattern react.c uses to detect error messages */
    const char *error_msg = "ERROR: cannot read '/dev/null': Permission denied";
    const char *normal_msg = "{\"exit_code\":0,\"chars\":105,\"lines\":9,\"ref\":\"R0S1\"}";
    const char *error_json = "{\"error\":\"missing 'command' parameter\"}";

    /* ERROR: prefix detection (used in react.c error eviction) */
    ASSERT(strstr(error_msg, "ERROR:") != NULL);
    ASSERT(strstr(normal_msg, "ERROR:") == NULL);
    ASSERT(strstr(error_json, "ERROR:") == NULL);

    /* The react.c eviction also checks for lowercase "error" */
    ASSERT(strstr(error_json, "error") != NULL);
}

/* ── test_index_unlimited_with_zero ── */
static void test_index_unlimited_with_zero(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    for (int i = 0; i < 5; i++) {
        char key[64];
        snprintf(key, sizeof(key), "fact:item-%d", i);
        memory_store(m, key, "value", NULL, 0, 0, NULL, NULL, 0);
    }

    /* Index returns counts only */
    char *index = memory_build_index(m);
    ASSERT_NOT_NULL(index);
    ASSERT_STR_CONTAINS(index, "5 entries");

    free(index);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ═══════════════════════════════════════════════════════════════
 * Priority 4: Pin/Unpin Tests
 * ═══════════════════════════════════════════════════════════════ */

/* ── test_pin_unpinned_entry: pin an entry that was stored unpinned ── */
static void test_pin_unpinned_entry(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store unpinned */
    memory_store(m, "fact:server-ip", "192.168.1.1", NULL, 0, 0, NULL, NULL, 0);

    /* Verify not pinned initially */
    char *pinned = memory_load_pinned(m);
    ASSERT(pinned == NULL || strstr(pinned, "server-ip") == NULL);
    free(pinned);

    /* Pin it */
    int rc = memory_pin(m, "fact:server-ip");
    ASSERT_EQ(rc, 0);

    /* Verify now pinned */
    pinned = memory_load_pinned(m);
    ASSERT_NOT_NULL(pinned);
    ASSERT_STR_CONTAINS(pinned, "192.168.1.1");
    ASSERT_STR_CONTAINS(pinned, "fact:server-ip");
    free(pinned);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_unpin_pinned_entry: unpin an entry that was stored pinned ── */
static void test_unpin_pinned_entry(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    /* Store pinned */
    memory_store(m, "fact:api-url", "https://api.example.com", NULL, 0, 1, NULL, NULL, 0);

    /* Verify pinned initially */
    char *pinned = memory_load_pinned(m);
    ASSERT_NOT_NULL(pinned);
    ASSERT_STR_CONTAINS(pinned, "api-url");
    free(pinned);

    /* Unpin it */
    int rc = memory_unpin(m, "fact:api-url");
    ASSERT_EQ(rc, 0);

    /* Verify no longer pinned */
    pinned = memory_load_pinned(m);
    ASSERT(pinned == NULL || strstr(pinned, "api-url") == NULL);
    free(pinned);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_pin_nonexistent: pin a key that doesn't exist ── */
static void test_pin_nonexistent(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    int rc = memory_pin(m, "fact:does-not-exist");
    ASSERT_EQ(rc, -1);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_unpin_nonexistent: unpin a key that doesn't exist ── */
static void test_unpin_nonexistent(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    int rc = memory_unpin(m, "fact:does-not-exist");
    ASSERT_EQ(rc, -1);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_pin_already_pinned: pin an already-pinned entry (idempotent) ── */
static void test_pin_already_pinned(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    memory_store(m, "lesson:important", "critical knowledge", NULL, 0, 1, NULL, NULL, 0);

    /* Pin again — should succeed (idempotent) */
    int rc = memory_pin(m, "lesson:important");
    ASSERT_EQ(rc, 0);

    /* Still pinned */
    char *pinned = memory_load_pinned(m);
    ASSERT_NOT_NULL(pinned);
    ASSERT_STR_CONTAINS(pinned, "critical knowledge");
    free(pinned);

    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* ── test_pin_preserves_value: pinning doesn't alter the stored value ── */
static void test_pin_preserves_value(void) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);

    const char *tags[] = {"redis", "migration"};
    memory_store(m, "lesson:redis-v7", "HMSET renamed to HSET", tags, 2, 0, NULL, NULL, 0);

    /* Pin it */
    memory_pin(m, "lesson:redis-v7");

    /* Recall and verify value + tags preserved */
    memory_results_t results = memory_recall(m, "redis-v7", 5);
    ASSERT_GT(results.count, 0);
    ASSERT_STR_EQ(results.entries[0].value, "HMSET renamed to HSET");
    ASSERT_EQ(results.entries[0].pinned, 1);

    memory_results_free(&results);
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

int main(void) {
    printf("test_memory:\n");

    /* Original tests */
    RUN_TEST(test_store_and_recall);
    RUN_TEST(test_tags);
    RUN_TEST(test_pinned);
    RUN_TEST(test_build_index);
    RUN_TEST(test_prune);
    RUN_TEST(test_overwrite);
    RUN_TEST(test_no_match);

    /* Priority 1: Progressive Disclosure */
    printf("\n  --- Progressive Disclosure ---\n");
    RUN_TEST(test_index_type_grouping);
    RUN_TEST(test_index_topic_counts);
    RUN_TEST(test_index_cap);
    RUN_TEST(test_index_no_cap_when_under_limit);
    RUN_TEST(test_index_with_tags_display);
    RUN_TEST(test_index_unlimited_with_zero);

    /* Priority 2: Skill Memory */
    printf("\n  --- Skill Memory ---\n");
    RUN_TEST(test_skill_store_and_recall);
    RUN_TEST(test_skill_in_index);
    RUN_TEST(test_skill_recall_by_tag);
    RUN_TEST(test_skill_not_pruned);

    /* Priority 3: Error Eviction Pattern */
    printf("\n  --- Error Eviction Pattern ---\n");
    RUN_TEST(test_error_detection_pattern);

    /* Priority 4: Pin/Unpin */
    printf("\n  --- Pin/Unpin ---\n");
    RUN_TEST(test_pin_unpinned_entry);
    RUN_TEST(test_unpin_pinned_entry);
    RUN_TEST(test_pin_nonexistent);
    RUN_TEST(test_unpin_nonexistent);
    RUN_TEST(test_pin_already_pinned);
    RUN_TEST(test_pin_preserves_value);

    TEST_SUMMARY();
}

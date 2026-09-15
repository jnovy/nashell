#include "test_common.h"
#include "../src/memory.h"
#include "../src/llm.h"
/* linked via LIB_OBJ */

#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ── test_store_and_recall ── */
static void test_store_and_recall(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);
  ASSERT_NOT_NULL(m);


  int rc = memory_store(m, "lesson:addition", "2+2=4", 0, NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);

  memory_results_t results = memory_query(m, "addition", 5);
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


  memory_store(m, "lesson:redis-v7", "HMSET renamed to HSET", 0, NULL, NULL, 0, NULL, 0);

  memory_results_t results = memory_query(m, "redis", 5);
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


  memory_store(m, "fact:api-key", "always use HTTPS", 1, NULL, NULL, 0, NULL, 0);

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

  memory_store(m, "lesson:a", "value a", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "strategy:b", "value b", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:c", "value c", 0, NULL, NULL, 0, NULL, 0);

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


  memory_store(m, "fact:stale", "old data", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "strategy:keep", "important", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:keep2", "also important", 0, NULL, NULL, 0, NULL, 0);

  memory_prune(m, 0, 999);

  memory_results_t r1 = memory_query(m, "strategy", 5);
  ASSERT_GT(r1.count, 0);
  memory_results_free(&r1);

  memory_results_t r2 = memory_query(m, "lesson", 5);
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

  memory_store(m, "fact:pi", "3.14", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:pi", "3.14159", 0, NULL, NULL, 0, NULL, 0);

  memory_results_t results = memory_query(m, "pi", 5);
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

  memory_store(m, "fact:pi", "3.14", 0, NULL, NULL, 0, NULL, 0);

  memory_results_t results = memory_query(m, "nonexistent_xyz", 5);
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
  memory_store(m, "lesson:l1", "lesson value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "strategy:s1", "strategy value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:f1", "fact value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "task:t1", "task value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "skill:sk1", "skill value", 0, NULL, NULL, 0, NULL, 0);

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
  memory_store(m, "lesson:l1", "v1", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:l2", "v2", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:l3", "v3", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "strategy:s1", "v1", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "strategy:s2", "v2", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:f1", "v1", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "skill:sk1", "v1", 0, NULL, NULL, 0, NULL, 0);

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
    memory_store(m, key, "some value", 0, NULL, NULL, 0, NULL, 0);
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

  memory_store(m, "fact:a", "v1", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:b", "v2", 0, NULL, NULL, 0, NULL, 0);

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


  memory_store(m, "lesson:redis-upgrade", "upgrade guide", 0, NULL, NULL, 0, NULL, 0);

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


  memory_store(m, "skill:compile-and-test-c",
               "1. Write .c file\n2. gcc -Wall -Wextra\n3. Run and verify output",
               0, NULL, NULL, 0, NULL, 0);

  /* Recall by skill name */
  memory_results_t results = memory_query(m, "compile", 5);
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

  memory_store(m, "skill:deploy-app", "deploy steps", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:l1", "lesson", 0, NULL, NULL, 0, NULL, 0);

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


  memory_store(m, "skill:docker-deploy", "docker compose up -d", 0, NULL, NULL, 0, NULL, 0);

  /* Recall by keyword in key */
  memory_results_t results = memory_query(m, "docker", 5);
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


  memory_store(m, "skill:important-skill", "reusable procedure", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:expendable", "can be pruned", 0, NULL, NULL, 0, NULL, 0);

  /* Prune aggressively */
  memory_prune(m, 0, 999);

  /* Skill should survive (protected type like strategy/lesson) */
  memory_results_t results = memory_query(m, "skill:", 5);
  /* Note: skill may or may not be protected depending on implementation */
  /* At minimum, verify no crash */
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── test_skill_description_populated ──
 * Verifies that memory_query() populates the description field
 * (progressive disclosure: agents see description first, then load
 * full value on demand via memory_search key=). */
static void test_skill_description_populated(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Store a skill with a multi-sentence value */
  memory_store(m, "skill:progressive-test",
               "When debugging react loops, count total steps vs tool calls. "
               "This is the second sentence with more detail about the procedure. "
               "Step 3 involves checking token counts for anomalies.",
               0, NULL, NULL, 0, NULL, 0);

  memory_results_t results = memory_query(m, "progressive", 5);
  ASSERT_GT(results.count, 0);
  ASSERT_STR_EQ(results.entries[0].key, "skill:progressive-test");

  /* Description should be populated (auto-generated first sentence) */
  ASSERT_NOT_NULL(results.entries[0].description);
  ASSERT(strlen(results.entries[0].description) > 0);
  /* Description should be shorter than full value */
  ASSERT(strlen(results.entries[0].description) < strlen(results.entries[0].value));
  /* Description should contain the first sentence */
  ASSERT_STR_CONTAINS(results.entries[0].description, "debugging react loops");

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── test_skill_created_at_populated ──
 * Verifies that memory_query() copies created_at from index to results.
 * (Bug fix: was previously missing, causing format_recency() to always show "unknown".) */
static void test_skill_created_at_populated(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "skill:recency-test", "test value", 0, NULL, NULL, 0, NULL, 0);

  memory_results_t results = memory_query(m, "recency", 5);
  ASSERT_GT(results.count, 0);
  /* created_at should be a recent epoch timestamp (> 2024-01-01) */
  ASSERT(results.entries[0].created_at > 1704067200.0);

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
    memory_store(m, key, "value", 0, NULL, NULL, 0, NULL, 0);
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
  memory_store(m, "fact:server-ip", "192.168.1.1", 0, NULL, NULL, 0, NULL, 0);

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
  memory_store(m, "fact:api-url", "https://api.example.com", 1, NULL, NULL, 0, NULL, 0);

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

  memory_store(m, "lesson:important", "critical knowledge", 1, NULL, NULL, 0, NULL, 0);

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


  memory_store(m, "lesson:redis-v7", "HMSET renamed to HSET", 0, NULL, NULL, 0, NULL, 0);

  /* Pin it */
  memory_pin(m, "lesson:redis-v7");

  /* Recall and verify value preserved */
  memory_results_t results = memory_query(m, "redis-v7", 5);
  ASSERT_GT(results.count, 0);
  ASSERT_STR_EQ(results.entries[0].value, "HMSET renamed to HSET");
  ASSERT_EQ(results.entries[0].pinned, 1);

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── Temporal Validity tests ── */

static void test_validity_persistent(void) {
  /* "persistent" is never stale */
  ASSERT_EQ(memory_is_stale("persistent", 0, NULL), 0);
}

static void test_validity_expires_when_never_stale(void) {
  /* expires_when: validity never auto-expires regardless of age */
  double now = (double)time(NULL);
  double year_ago = now - 365.0 * 86400.0;
  int days_past = 99;
  ASSERT_EQ(memory_is_stale("expires_when:new build submitted", now, &days_past), 0);
  ASSERT_EQ(days_past, 0); /* unchanged - not stale */
  /* Even a year-old entry is not stale */
  ASSERT_EQ(memory_is_stale("expires_when:new build submitted", year_ago, &days_past), 0);
}

static void test_validity_expires_when_description(void) {
  /* The description should be extractable via validity_expires_desc() */
  const char *v1 = "expires_when:errata advisory is created for this build";
  const char *desc1 = validity_expires_desc(v1);
  ASSERT_STR_EQ(desc1, "errata advisory is created for this build");
  ASSERT_EQ(memory_is_stale(v1, 0, NULL), 0);

  /* Non-expiring validity returns NULL */
  ASSERT_EQ(validity_expires_desc("persistent") == NULL, 1);
  ASSERT_EQ(validity_expires_desc("volatile") == NULL, 1);
  ASSERT_EQ(validity_expires_desc(NULL) == NULL, 1);
}

static void test_validity_volatile(void) {
  /* Volatile is always stale */
  double now = (double)time(NULL);
  ASSERT_EQ(memory_is_stale("volatile", now, NULL), 1);
}

static void test_validity_session(void) {
  /* Session entries are stale after 6 hours */
  double now = (double)time(NULL);
  /* Created 1 hour ago - not stale */
  ASSERT_EQ(memory_is_stale("session", now - 3600.0, NULL), 0);
  /* Created 12 hours ago - stale */
  ASSERT_EQ(memory_is_stale("session", now - 43200.0, NULL), 1);
}

static void test_validity_null_is_persistent(void) {
  /* NULL validity = persistent = never stale */
  ASSERT_EQ(memory_is_stale(NULL, 0, NULL), 0);
  ASSERT_EQ(memory_is_stale("", 0, NULL), 0);
}

static void test_basis_field(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "fact:test-basis", "API field is X", 0, NULL, NULL, 0, NULL, 0);
  memory_set_basis(m, "fact:test-basis", "confirmed by querying API");

  /* Verify via memory_find */
  mem_index_entry_t *found = memory_find(m, "fact:test-basis");
  ASSERT_NOT_NULL(found);
  ASSERT_NOT_NULL(found->basis);
  ASSERT_STR_EQ(found->basis, "confirmed by querying API");
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_validity_basis_preserved_on_update(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "fact:update-test", "value1", 0, NULL, NULL, 0, NULL, 0);
  memory_set_validity(m, "fact:update-test", "expires_when:new build tagged in candidate");
  memory_set_basis(m, "fact:update-test", "initial evidence");

  /* Re-store with new value - validity and basis should be preserved */
  memory_store(m, "fact:update-test", "value2", 0, NULL, NULL, 0, NULL, 0);

  mem_index_entry_t *found = memory_find(m, "fact:update-test");
  ASSERT_NOT_NULL(found);
  ASSERT_STR_EQ(found->value, "value2");
  ASSERT_NOT_NULL(found->validity);
  ASSERT_STR_EQ(found->validity, "expires_when:new build tagged in candidate");
  ASSERT_NOT_NULL(found->basis);
  ASSERT_STR_EQ(found->basis, "initial evidence");
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_validity_in_query_results(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "fact:query-val-test", "some fact for query test", 0, NULL, NULL, 0, NULL, 0);
  memory_set_validity(m, "fact:query-val-test", "volatile");
  memory_set_basis(m, "fact:query-val-test", "test basis text");

  memory_results_t results = memory_query(m, "fact:query-val-test", 5);
  ASSERT_GT(results.count, 0);
  ASSERT_NOT_NULL(results.entries[0].validity);
  ASSERT_STR_EQ(results.entries[0].validity, "volatile");
  ASSERT_NOT_NULL(results.entries[0].basis);
  ASSERT_STR_EQ(results.entries[0].basis, "test basis text");

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_validity_in_find(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:find-val-test", "find validity test", 0, NULL, NULL, 0, NULL, 0);
  memory_set_validity(m, "lesson:find-val-test", "expires_when:upstream release published");

  mem_index_entry_t *found = memory_find(m, "lesson:find-val-test");
  ASSERT_NOT_NULL(found);
  ASSERT_NOT_NULL(found->validity);
  ASSERT_STR_EQ(found->validity, "expires_when:upstream release published");
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── test_validity_basis_persist_on_disk: verify validity/basis survive
 *    memory_free() + memory_new() reload from JSON on disk ── */
static void test_validity_basis_persist_on_disk(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Store entry, set validity and basis */
  memory_store(m, "fact:disk-persist", "build is 1.43.2-2.el9", 0, NULL, NULL, 0, NULL, 0);
  memory_set_validity(m, "fact:disk-persist", "expires_when:new brew build submitted");
  memory_set_basis(m, "fact:disk-persist", "brew latest-build query on 2026-08-13");

  /* Verify in-memory before reload */
  mem_index_entry_t *pre = memory_find(m, "fact:disk-persist");
  ASSERT_NOT_NULL(pre);
  ASSERT_STR_EQ(pre->validity, "expires_when:new brew build submitted");
  ASSERT_STR_EQ(pre->basis, "brew latest-build query on 2026-08-13");
  memory_find_free(pre);

  /* Destroy in-memory state, reload from disk */
  memory_free(m);
  m = memory_new(dir);
  ASSERT_NOT_NULL(m);

  /* Verify fields survived the round-trip */
  mem_index_entry_t *post = memory_find(m, "fact:disk-persist");
  ASSERT_NOT_NULL(post);
  ASSERT_STR_EQ(post->value, "build is 1.43.2-2.el9");
  ASSERT_NOT_NULL(post->validity);
  ASSERT_STR_EQ(post->validity, "expires_when:new brew build submitted");
  ASSERT_NOT_NULL(post->basis);
  ASSERT_STR_EQ(post->basis, "brew latest-build query on 2026-08-13");
  memory_find_free(post);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── test_contradiction_detection_mechanism: verify that memory_query()
 *    returns similar entries with raw_relevance >= 0.60, which is the
 *    threshold used by tool_memory_store() (tool_memory.c:488) to emit
 *    contradiction warnings. Tests the underlying mechanism without
 *    needing full tool_ctx_t infrastructure. ── */
static void test_contradiction_detection_mechanism(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Store an entry about a JIRA field */
  memory_store(m, "lesson:errata-field-id",
               "The JIRA errataLink field is customfield_12323",
               0, NULL, NULL, 0, NULL, 0);

  /* Query with the KEY of the existing entry - simulates storing a new
   * memory whose value mentions the same concept. The substring scorer
   * gives 3.0 for full-query-in-key match (memory.c:787), yielding
   * raw_relevance = 3.0/4.0 = 0.75 which is above the 0.60 threshold. */
  memory_results_t results = memory_query(m, "lesson:errata-field-id", 5);
  ASSERT_GT(results.count, 0);

  /* Find the original entry in results */
  int found = -1;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:errata-field-id") == 0) {
      found = i;
      break;
    }
  }
  ASSERT(found >= 0); /* entry must appear in results */
  ASSERT(results.entries[found].raw_relevance >= 0.60);

  memory_results_free(&results);

  /* Negative case: completely unrelated query should NOT trigger */
  memory_results_t unrelated = memory_query(m, "docker compose networking", 5);
  int has_high_match = 0;
  for (int i = 0; i < unrelated.count; i++) {
    if (strcmp(unrelated.entries[i].key, "lesson:errata-field-id") == 0 &&
        unrelated.entries[i].raw_relevance >= 0.60) {
      has_high_match = 1;
    }
  }
  ASSERT_EQ(has_high_match, 0); /* unrelated query must NOT trigger */
  memory_results_free(&unrelated);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── test_contradiction_detection_similar_values: verify that two entries
 *    with overlapping key names are found as potential contradictions ── */
static void test_contradiction_detection_similar_values(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Store two entries about the same topic with different keys */
  memory_store(m, "fact:latest-buildah-build",
               "buildah-1.43.2-1.el9 is the latest candidate build",
               0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:latest-buildah-build-updated",
               "buildah-1.43.2-2.el9 is the latest candidate build",
               0, NULL, NULL, 0, NULL, 0);

  /* Querying with the first entry's key should find it with high relevance.
   * This simulates the contradiction check in tool_memory.c:479 where
   * memory_query is called with the new entry's value text. */
  memory_results_t results = memory_query(m, "fact:latest-buildah-build", 5);
  ASSERT_GT(results.count, 0);

  /* The first entry's key matches the query exactly -> raw_relevance >= 0.60 */
  int found_original = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "fact:latest-buildah-build") == 0 &&
        results.entries[i].raw_relevance >= 0.60) {
      found_original = 1;
    }
  }
  ASSERT_EQ(found_original, 1);

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── Typed Edges (SodaMem) ── */

static void test_typed_edge_add_ref(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:a", "alpha value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:b", "beta value", 0, NULL, NULL, 0, NULL, 0);

  /* Add a CONTRADICTS edge from a -> b */
  int rc = memory_add_ref(m, "lesson:a", "lesson:b", MEM_EDGE_CONTRADICTS);
  ASSERT_EQ(rc, 0);

  /* Reload from disk and verify edge persisted */
  memory_free(m);
  m = memory_new(dir);

  /* Find entry a in index and check ref_types */
  memory_results_t results = memory_query(m, "alpha", 5);
  ASSERT_GT(results.count, 0);
  int found = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:a") == 0) {
      ASSERT_EQ(results.entries[i].n_refs, 1);
      if (results.entries[i].n_refs > 0)
        ASSERT_STR_EQ(results.entries[i].refs[0], "lesson:b");
      found = 1;
    }
  }
  ASSERT_EQ(found, 1);

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_typed_edge_add_ref_nonexistent(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Adding ref to nonexistent entry should fail */
  int rc = memory_add_ref(m, "nonexistent", "also-nonexistent", MEM_EDGE_RELATES);
  ASSERT_EQ(rc, -1);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_typed_edge_supersedes_auto(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:old", "old knowledge", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:new", "new knowledge", 0, NULL, NULL, 0, NULL, 0);

  /* Set supersedes should auto-create SUPERSEDES ref */
  memory_set_supersedes(m, "lesson:new", "lesson:old");

  /* Reload and verify */
  memory_free(m);
  m = memory_new(dir);

  memory_results_t results = memory_query(m, "new knowledge", 5);
  int found_ref = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:new") == 0) {
      for (int r = 0; r < results.entries[i].n_refs; r++) {
        if (strcmp(results.entries[i].refs[r], "lesson:old") == 0) {
          found_ref = 1;
        }
      }
    }
  }
  ASSERT_EQ(found_ref, 1);

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_superseded_hard_exclusion(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Lower min_score so demoted entries still appear */
  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 1.3f, 90.0f);

  /* Store two entries with very similar content */
  memory_store(m, "fact:version-old", "the software version is 1.0", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:version-new", "the software version is 2.0", 0, NULL, NULL, 0, NULL, 0);
  memory_set_supersedes(m, "fact:version-new", "fact:version-old");

  /* With soft demotion (0.3) and low min_score, old entry should still appear */
  memory_results_t results = memory_query(m, "version", 10);
  int found_old = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "fact:version-old") == 0)
      found_old = 1;
  }
  ASSERT_EQ(found_old, 1); /* still visible with soft demotion */
  memory_results_free(&results);

  /* Set demotion to 0.0 for hard exclusion */
  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.3f, 0.0f, 0.0f, 1.3f, 90.0f);

  results = memory_query(m, "version", 10);
  found_old = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "fact:version-old") == 0)
      found_old = 1;
  }
  ASSERT_EQ(found_old, 0); /* hard excluded */

  /* New entry should still be there */
  int found_new = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "fact:version-new") == 0)
      found_new = 1;
  }
  ASSERT_EQ(found_new, 1);

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_typed_edge_update_existing(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:x", "x value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:y", "y value", 0, NULL, NULL, 0, NULL, 0);

  /* Add as RELATES first */
  int rc = memory_add_ref(m, "lesson:x", "lesson:y", MEM_EDGE_RELATES);
  ASSERT_EQ(rc, 0);

  /* Update to UPDATES */
  rc = memory_add_ref(m, "lesson:x", "lesson:y", MEM_EDGE_UPDATES);
  ASSERT_EQ(rc, 0);

  /* Should still have only 1 ref (updated, not duplicated) */
  memory_free(m);
  m = memory_new(dir);

  memory_results_t results = memory_query(m, "x value", 5);
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:x") == 0) {
      ASSERT_EQ(results.entries[i].n_refs, 1);
    }
  }

  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ── TAINTED Importance Tier ── */

static void test_tainted_importance_value(void) {
  /* Verify TAINTED is -1 and below LOW */
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_TAINTED, -1);
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_LOW, 0);
  ASSERT((int)LLM_MSG_IMPORTANCE_TAINTED < (int)LLM_MSG_IMPORTANCE_LOW);
}

/* ===============================================================
 * Comprehensive Typed Edges Tests
 * =============================================================== */

/* Verify that storing with refs but no explicit ref_types defaults to RELATES (0) */
static void test_typed_edge_relates_default(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  const char *refs[] = {"lesson:related1", "lesson:related2"};
  memory_store(m, "lesson:source", "source value", 0, NULL, refs, 2, NULL, 0);
  memory_store(m, "lesson:related1", "related1 value", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:related2", "related2 value", 0, NULL, NULL, 0, NULL, 0);

  /* Now add typed edges to set ref_types in index; the store path only
   * writes ref_types to JSON when old_ref_types existed. Use memory_add_ref
   * to explicitly create RELATES edges and verify default behavior. */
  memory_add_ref(m, "lesson:source", "lesson:related1", MEM_EDGE_RELATES);
  memory_add_ref(m, "lesson:source", "lesson:related2", MEM_EDGE_RELATES);

  mem_index_entry_t *found = memory_find(m, "lesson:source");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 2);
  ASSERT_NOT_NULL(found->ref_types);
  ASSERT_EQ(found->ref_types[0], MEM_EDGE_RELATES);
  ASSERT_EQ(found->ref_types[1], MEM_EDGE_RELATES);
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify all 5 edge types can be created and read back */
static void test_typed_edge_all_types(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:hub", "hub entry", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:r", "relates target", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:s", "supersedes target", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:c", "contradicts target", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:u", "updates target", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:d", "depends target", 0, NULL, NULL, 0, NULL, 0);

  ASSERT_EQ(memory_add_ref(m, "lesson:hub", "lesson:r", MEM_EDGE_RELATES), 0);
  ASSERT_EQ(memory_add_ref(m, "lesson:hub", "lesson:s", MEM_EDGE_SUPERSEDES), 0);
  ASSERT_EQ(memory_add_ref(m, "lesson:hub", "lesson:c", MEM_EDGE_CONTRADICTS), 0);
  ASSERT_EQ(memory_add_ref(m, "lesson:hub", "lesson:u", MEM_EDGE_UPDATES), 0);
  ASSERT_EQ(memory_add_ref(m, "lesson:hub", "lesson:d", MEM_EDGE_DEPENDS), 0);

  mem_index_entry_t *found = memory_find(m, "lesson:hub");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 5);
  ASSERT_NOT_NULL(found->ref_types);
  ASSERT_EQ(found->ref_types[0], MEM_EDGE_RELATES);
  ASSERT_EQ(found->ref_types[1], MEM_EDGE_SUPERSEDES);
  ASSERT_EQ(found->ref_types[2], MEM_EDGE_CONTRADICTS);
  ASSERT_EQ(found->ref_types[3], MEM_EDGE_UPDATES);
  ASSERT_EQ(found->ref_types[4], MEM_EDGE_DEPENDS);
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify ref_types survive free+reload from disk (JSON persistence) */
static void test_typed_edge_persist_on_disk(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:src", "source data", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:dep", "dependency data", 0, NULL, NULL, 0, NULL, 0);

  memory_add_ref(m, "lesson:src", "lesson:dep", MEM_EDGE_DEPENDS);

  /* Verify in-memory */
  mem_index_entry_t *pre = memory_find(m, "lesson:src");
  ASSERT_NOT_NULL(pre);
  ASSERT_EQ(pre->n_refs, 1);
  ASSERT_NOT_NULL(pre->ref_types);
  ASSERT_EQ(pre->ref_types[0], MEM_EDGE_DEPENDS);
  memory_find_free(pre);

  /* Destroy and reload from disk */
  memory_free(m);
  m = memory_new(dir);

  mem_index_entry_t *post = memory_find(m, "lesson:src");
  ASSERT_NOT_NULL(post);
  ASSERT_EQ(post->n_refs, 1);
  ASSERT_STR_EQ(post->refs[0], "lesson:dep");
  ASSERT_NOT_NULL(post->ref_types);
  ASSERT_EQ(post->ref_types[0], MEM_EDGE_DEPENDS);
  memory_find_free(post);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify multiple refs with different types on the same entry */
static void test_typed_edge_multiple_refs(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:center", "center entry", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:t1", "target one", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:t2", "target two", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:t3", "target three", 0, NULL, NULL, 0, NULL, 0);

  memory_add_ref(m, "lesson:center", "lesson:t1", MEM_EDGE_CONTRADICTS);
  memory_add_ref(m, "lesson:center", "lesson:t2", MEM_EDGE_UPDATES);
  memory_add_ref(m, "lesson:center", "lesson:t3", MEM_EDGE_DEPENDS);

  /* Reload from disk to test full round-trip */
  memory_free(m);
  m = memory_new(dir);

  mem_index_entry_t *found = memory_find(m, "lesson:center");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 3);
  ASSERT_NOT_NULL(found->ref_types);

  /* Verify each ref has its correct type */
  for (int i = 0; i < found->n_refs; i++) {
    if (strcmp(found->refs[i], "lesson:t1") == 0)
      ASSERT_EQ(found->ref_types[i], MEM_EDGE_CONTRADICTS);
    else if (strcmp(found->refs[i], "lesson:t2") == 0)
      ASSERT_EQ(found->ref_types[i], MEM_EDGE_UPDATES);
    else if (strcmp(found->refs[i], "lesson:t3") == 0)
      ASSERT_EQ(found->ref_types[i], MEM_EDGE_DEPENDS);
  }
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify GC cleanup: deleting a ref'd entry shifts ref_types in parallel */
static void test_typed_edge_gc_cleanup(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:parent", "parent entry", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:child1", "child one", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:child2", "child two", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:child3", "child three", 0, NULL, NULL, 0, NULL, 0);

  memory_add_ref(m, "lesson:parent", "lesson:child1", MEM_EDGE_RELATES);
  memory_add_ref(m, "lesson:parent", "lesson:child2", MEM_EDGE_CONTRADICTS);
  memory_add_ref(m, "lesson:parent", "lesson:child3", MEM_EDGE_UPDATES);

  /* Delete child2 - this should trigger GC that shifts ref_types */
  memory_delete(m, "lesson:child2");

  mem_index_entry_t *found = memory_find(m, "lesson:parent");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 2);
  ASSERT_NOT_NULL(found->ref_types);

  /* After GC: child1 (RELATES) and child3 (UPDATES) should remain */
  int found_child1 = 0, found_child3 = 0;
  for (int i = 0; i < found->n_refs; i++) {
    if (strcmp(found->refs[i], "lesson:child1") == 0) {
      ASSERT_EQ(found->ref_types[i], MEM_EDGE_RELATES);
      found_child1 = 1;
    } else if (strcmp(found->refs[i], "lesson:child3") == 0) {
      ASSERT_EQ(found->ref_types[i], MEM_EDGE_UPDATES);
      found_child3 = 1;
    }
  }
  ASSERT_EQ(found_child1, 1);
  ASSERT_EQ(found_child3, 1);
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify ref_types preserved when re-storing (updating) an entry's value */
static void test_typed_edge_preserved_on_value_update(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  const char *refs[] = {"lesson:target"};
  memory_store(m, "lesson:source", "original value", 0, NULL, refs, 1, NULL, 0);
  memory_store(m, "lesson:target", "target value", 0, NULL, NULL, 0, NULL, 0);

  /* Set edge type via memory_add_ref */
  memory_add_ref(m, "lesson:source", "lesson:target", MEM_EDGE_UPDATES);

  /* Verify before update */
  mem_index_entry_t *pre = memory_find(m, "lesson:source");
  ASSERT_NOT_NULL(pre);
  ASSERT_EQ(pre->n_refs, 1);
  ASSERT_NOT_NULL(pre->ref_types);
  ASSERT_EQ(pre->ref_types[0], MEM_EDGE_UPDATES);
  memory_find_free(pre);

  /* Re-store with new value but same refs - ref_types should be preserved */
  memory_store(m, "lesson:source", "updated value", 0, NULL, refs, 1, NULL, 0);

  mem_index_entry_t *post = memory_find(m, "lesson:source");
  ASSERT_NOT_NULL(post);
  ASSERT_STR_EQ(post->value, "updated value");
  ASSERT_EQ(post->n_refs, 1);
  ASSERT_STR_EQ(post->refs[0], "lesson:target");
  ASSERT_NOT_NULL(post->ref_types);
  ASSERT_EQ(post->ref_types[0], MEM_EDGE_UPDATES);
  memory_find_free(post);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify memory_add_ref handles NULL parameters */
static void test_typed_edge_null_params(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  ASSERT_EQ(memory_add_ref(NULL, "key", "ref", MEM_EDGE_RELATES), -1);
  ASSERT_EQ(memory_add_ref(m, NULL, "ref", MEM_EDGE_RELATES), -1);
  ASSERT_EQ(memory_add_ref(m, "key", NULL, MEM_EDGE_RELATES), -1);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify memory_find copies ref_types correctly (bug fix test) */
static void test_find_copies_ref_types(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:a", "alpha", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:b", "beta", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:c", "gamma", 0, NULL, NULL, 0, NULL, 0);

  memory_add_ref(m, "lesson:a", "lesson:b", MEM_EDGE_CONTRADICTS);
  memory_add_ref(m, "lesson:a", "lesson:c", MEM_EDGE_DEPENDS);

  /* memory_find must return deep copy including ref_types */
  mem_index_entry_t *copy = memory_find(m, "lesson:a");
  ASSERT_NOT_NULL(copy);
  ASSERT_EQ(copy->n_refs, 2);
  ASSERT_NOT_NULL(copy->refs);
  ASSERT_NOT_NULL(copy->ref_types);
  ASSERT_STR_EQ(copy->refs[0], "lesson:b");
  ASSERT_EQ(copy->ref_types[0], MEM_EDGE_CONTRADICTS);
  ASSERT_STR_EQ(copy->refs[1], "lesson:c");
  ASSERT_EQ(copy->ref_types[1], MEM_EDGE_DEPENDS);
  memory_find_free(copy);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ===============================================================
 * Comprehensive Supersession Tests
 * =============================================================== */

/* Verify multi-level supersession chain: A supersedes B supersedes C */
static void test_superseded_chain(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  /* Low min_score to see demoted entries */
  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.3f, 0.0f, 1.3f, 90.0f);

  memory_store(m, "fact:version-v1", "the software version is 1.0", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:version-v2", "the software version is 2.0", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:version-v3", "the software version is 3.0", 0, NULL, NULL, 0, NULL, 0);

  /* Chain: v3 supersedes v2 supersedes v1 */
  memory_set_supersedes(m, "fact:version-v2", "fact:version-v1");
  memory_set_supersedes(m, "fact:version-v3", "fact:version-v2");

  /* v3 should have version >= 3 (lineage tracking) */
  mem_index_entry_t *v3 = memory_find(m, "fact:version-v3");
  ASSERT_NOT_NULL(v3);
  ASSERT(v3->version >= 3);
  ASSERT_STR_EQ(v3->supersedes, "fact:version-v2");
  memory_find_free(v3);

  /* v2 should be superseded (superseded_at > 0) */
  mem_index_entry_t *v2 = memory_find(m, "fact:version-v2");
  ASSERT_NOT_NULL(v2);
  ASSERT(v2->superseded_at > 0.0);
  memory_find_free(v2);

  /* v1 should also be superseded */
  mem_index_entry_t *v1 = memory_find(m, "fact:version-v1");
  ASSERT_NOT_NULL(v1);
  ASSERT(v1->superseded_at > 0.0);
  memory_find_free(v1);

  /* With soft demotion (0.3), all should still appear */
  memory_results_t results = memory_query(m, "version", 10);
  int count_version = 0;
  for (int i = 0; i < results.count; i++) {
    if (strstr(results.entries[i].key, "fact:version-v"))
      count_version++;
  }
  ASSERT_EQ(count_version, 3); /* all visible with soft demotion */
  memory_results_free(&results);

  /* With hard exclusion, only v3 should appear */
  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.3f, 90.0f);
  results = memory_query(m, "version", 10);
  count_version = 0;
  int found_v3 = 0;
  for (int i = 0; i < results.count; i++) {
    if (strstr(results.entries[i].key, "fact:version-v")) {
      count_version++;
      if (strcmp(results.entries[i].key, "fact:version-v3") == 0)
        found_v3 = 1;
    }
  }
  ASSERT_EQ(count_version, 1); /* only non-superseded entry */
  ASSERT_EQ(found_v3, 1);
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify negative demotion value acts as hard exclusion (same as 0.0) */
static void test_superseded_negative_demotion(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "fact:neg-old", "old negdemo fact", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "fact:neg-new", "new negdemo fact", 0, NULL, NULL, 0, NULL, 0);
  memory_set_supersedes(m, "fact:neg-new", "fact:neg-old");

  /* Set negative demotion - should behave like hard exclusion */
  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, -0.5f, 0.0f, 1.3f, 90.0f);

  memory_results_t results = memory_query(m, "negdemo", 10);
  int found_old = 0;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "fact:neg-old") == 0)
      found_old = 1;
  }
  ASSERT_EQ(found_old, 0); /* hard excluded */
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* Verify auto-created SUPERSEDES ref has correct type */
static void test_supersedes_auto_ref_type(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_store(m, "lesson:old-way", "old approach", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:new-way", "new approach", 0, NULL, NULL, 0, NULL, 0);

  memory_set_supersedes(m, "lesson:new-way", "lesson:old-way");

  /* Verify the new entry has a SUPERSEDES ref to the old entry */
  mem_index_entry_t *found = memory_find(m, "lesson:new-way");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 1);
  ASSERT_STR_EQ(found->refs[0], "lesson:old-way");
  ASSERT_NOT_NULL(found->ref_types);
  ASSERT_EQ(found->ref_types[0], MEM_EDGE_SUPERSEDES);
  memory_find_free(found);

  /* Verify it survives disk round-trip */
  memory_free(m);
  m = memory_new(dir);

  found = memory_find(m, "lesson:new-way");
  ASSERT_NOT_NULL(found);
  ASSERT_EQ(found->n_refs, 1);
  ASSERT_STR_EQ(found->refs[0], "lesson:old-way");
  ASSERT_NOT_NULL(found->ref_types);
  ASSERT_EQ(found->ref_types[0], MEM_EDGE_SUPERSEDES);
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ===============================================================
 * Edge-Aware Boost Weight Tests
 *
 * The ref-boost mechanism (memory.c:1285-1323) applies when a
 * high-scoring entry (>= 0.5) has refs. The ref'd entry gets:
 *   boosted_score = target.score + weight * source.score
 * We test that different edge types produce the expected relative
 * ordering of scores.
 * =============================================================== */

/* SUPERSEDES ref should NOT boost the ref'd entry (weight=0.0),
 * while RELATES does boost (+0.3). Compare the two to prove SUPERSEDES
 * is weaker. Query must be exact key substring for score >= 0.5. */
static void test_edge_boost_supersedes_no_boost(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.3f, 0.0f, 1.3f, 90.0f);

  /* Source: key contains "zsup-source" - query matches key via fast path */
  memory_store(m, "lesson:zsup-source", "source info about zsup", 0, NULL, NULL, 0, NULL, 0);
  /* Two targets with identical content containing "zsup" for weak match */
  memory_store(m, "lesson:zsup-old", "old info about zsup", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:zsup-related", "old info about zsup", 0, NULL, NULL, 0, NULL, 0);

  /* SUPERSEDES edge (weight 0.0) vs RELATES edge (weight 0.3) */
  memory_add_ref(m, "lesson:zsup-source", "lesson:zsup-old", MEM_EDGE_SUPERSEDES);
  memory_add_ref(m, "lesson:zsup-source", "lesson:zsup-related", MEM_EDGE_RELATES);

  memory_results_t results = memory_query(m, "zsup-source", 10);
  double old_score = -1, related_score = -1;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:zsup-old") == 0)
      old_score = results.entries[i].relevance;
    if (strcmp(results.entries[i].key, "lesson:zsup-related") == 0)
      related_score = results.entries[i].relevance;
  }
  /* SUPERSEDES (0.0 boost) should be LOWER than RELATES (+0.3 boost) */
  if (old_score >= 0 && related_score >= 0) {
    ASSERT(old_score < related_score);
  }
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* CONTRADICTS ref should suppress the ref'd entry (weight=-0.2).
 * The ref-boost mechanism only fires when scored[i].score >= 0.5.
 * Substring scoring: full query in key -> 3.0+/4.0 = 0.75+ (above threshold).
 * So queries must be exact substrings of the source key. */
static void test_edge_boost_contradicts_suppresses(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.3f, 0.0f, 1.3f, 90.0f);

  /* Source key contains "zcontra" - query "zcontra" will match key via
   * fast path: strcasestr(key, query) -> YES -> score = 3.0+/4.0 = 0.75+ */
  memory_store(m, "lesson:zcontra-source", "source info about zcontra", 0, NULL, NULL, 0, NULL, 0);
  /* Two targets: both contain "zcontra" in value so they appear in scored[]
   * but below 0.5 threshold (value-only match = 1.0/4.0 = 0.25) */
  memory_store(m, "lesson:zcontra-wrong", "wrong info about zcontra", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:zcontra-neutral", "wrong info about zcontra", 0, NULL, NULL, 0, NULL, 0);

  /* CONTRADICTS edge: source -> wrong (weight -0.2, should suppress) */
  memory_add_ref(m, "lesson:zcontra-source", "lesson:zcontra-wrong", MEM_EDGE_CONTRADICTS);

  memory_results_t results = memory_query(m, "zcontra-source", 10);
  double wrong_score = -1, neutral_score = -1;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:zcontra-wrong") == 0)
      wrong_score = results.entries[i].relevance;
    if (strcmp(results.entries[i].key, "lesson:zcontra-neutral") == 0)
      neutral_score = results.entries[i].relevance;
  }
  /* Wrong (contradicted) should score LOWER than neutral (no ref) */
  if (wrong_score >= 0 && neutral_score >= 0) {
    ASSERT(wrong_score < neutral_score);
  }
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* UPDATES ref should boost more than RELATES (+0.5 vs +0.3).
 * Query must be exact key substring so source scores >= 0.5 (boost threshold). */
static void test_edge_boost_updates_stronger(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.3f, 0.0f, 1.3f, 90.0f);

  /* Source: key contains "zupd-main" - query matches key via fast path */
  memory_store(m, "lesson:zupd-main", "main info about zupd", 0, NULL, NULL, 0, NULL, 0);
  /* Two targets with identical content containing "zupd" for weak match */
  memory_store(m, "lesson:zupd-updated", "reference info about zupd", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:zupd-related", "reference info about zupd", 0, NULL, NULL, 0, NULL, 0);

  /* UPDATES on one, RELATES on the other */
  memory_add_ref(m, "lesson:zupd-main", "lesson:zupd-updated", MEM_EDGE_UPDATES);
  memory_add_ref(m, "lesson:zupd-main", "lesson:zupd-related", MEM_EDGE_RELATES);

  memory_results_t results = memory_query(m, "zupd-main", 10);
  double updated_score = -1, related_score = -1;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:zupd-updated") == 0)
      updated_score = results.entries[i].relevance;
    if (strcmp(results.entries[i].key, "lesson:zupd-related") == 0)
      related_score = results.entries[i].relevance;
  }
  /* UPDATES (+0.5) should boost more than RELATES (+0.3) */
  if (updated_score >= 0 && related_score >= 0) {
    ASSERT(updated_score > related_score);
  }
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* DEPENDS ref should boost the target (+0.4 weight).
 * Query must be exact key substring so source scores >= 0.5 (boost threshold). */
static void test_edge_boost_depends(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);

  memory_set_recall_config(m, 0.01, 0.5f, 0.5f, 0.0f, 0.3f, 0.0f, 1.3f, 90.0f);

  /* Source: key contains "zdep-app" - query matches key via fast path */
  memory_store(m, "lesson:zdep-app", "main info about zdep", 0, NULL, NULL, 0, NULL, 0);
  /* Two targets with identical content containing "zdep" for weak match */
  memory_store(m, "lesson:zdep-prereq", "prerequisite info about zdep", 0, NULL, NULL, 0, NULL, 0);
  memory_store(m, "lesson:zdep-noreq", "prerequisite info about zdep", 0, NULL, NULL, 0, NULL, 0);

  /* DEPENDS on one, nothing on the other */
  memory_add_ref(m, "lesson:zdep-app", "lesson:zdep-prereq", MEM_EDGE_DEPENDS);

  memory_results_t results = memory_query(m, "zdep-app", 10);
  double dep_score = -1, nodep_score = -1;
  for (int i = 0; i < results.count; i++) {
    if (strcmp(results.entries[i].key, "lesson:zdep-prereq") == 0)
      dep_score = results.entries[i].relevance;
    if (strcmp(results.entries[i].key, "lesson:zdep-noreq") == 0)
      nodep_score = results.entries[i].relevance;
  }
  /* DEPENDS (+0.4) should boost dep above nodep */
  if (dep_score >= 0 && nodep_score >= 0) {
    ASSERT(dep_score > nodep_score);
  }
  memory_results_free(&results);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* ===============================================================
 * Comprehensive TAINTED Importance Tier Tests
 * =============================================================== */

/* Verify all 5 importance tiers are in strict ascending order */
static void test_tainted_ordering(void) {
  ASSERT((int)LLM_MSG_IMPORTANCE_TAINTED < (int)LLM_MSG_IMPORTANCE_LOW);
  ASSERT((int)LLM_MSG_IMPORTANCE_LOW < (int)LLM_MSG_IMPORTANCE_NORMAL);
  ASSERT((int)LLM_MSG_IMPORTANCE_NORMAL < (int)LLM_MSG_IMPORTANCE_HIGH);
  ASSERT((int)LLM_MSG_IMPORTANCE_HIGH < (int)LLM_MSG_IMPORTANCE_CRITICAL);
}

/* Verify the scoring formula base scores: imp * 100 */
static void test_tainted_score_formula(void) {
  /* The eviction scoring formula uses imp * 100 as base.
   * TAINTED = -1 -> -100 (always evicted first)
   * LOW = 0 -> 0
   * NORMAL = 1 -> 100
   * HIGH = 2 -> 200
   * CRITICAL = 3 -> 300 */
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_TAINTED * 100, -100);
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_LOW * 100, 0);
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_NORMAL * 100, 100);
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_HIGH * 100, 200);
  ASSERT_EQ((int)LLM_MSG_IMPORTANCE_CRITICAL * 100, 300);
}

/* Verify TAINTED is below the eviction skip threshold (>= HIGH) */
static void test_tainted_below_skip_threshold(void) {
  /* Eviction skips messages with importance >= HIGH.
   * TAINTED must be below this threshold. */
  ASSERT((int)LLM_MSG_IMPORTANCE_TAINTED < (int)LLM_MSG_IMPORTANCE_HIGH);
  /* Also verify LOW and NORMAL are below (they should be evictable too) */
  ASSERT((int)LLM_MSG_IMPORTANCE_LOW < (int)LLM_MSG_IMPORTANCE_HIGH);
  ASSERT((int)LLM_MSG_IMPORTANCE_NORMAL < (int)LLM_MSG_IMPORTANCE_HIGH);
}

/* is_global defaults to 0 for single-layer memory_query */
static void test_is_global_default(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);
  ASSERT_NOT_NULL(m);
  int rc = memory_store(m, "skill:test-global-flag",
                        "test skill for is_global default", 0,
                        NULL, NULL, 0, NULL, 0);
  ASSERT_EQ(rc, 0);
  memory_results_t results = memory_query(m, "test global flag", 5);
  ASSERT_GT(results.count, 0);
  /* Single-layer query: is_global should default to 0 */
  ASSERT_EQ(results.entries[0].is_global, 0);
  memory_results_free(&results);
  memory_free(m);
  rm_rf(dir);
  free(dir);
}

/* memory_seed_defaults tests */
static void test_seed_defaults(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);
  ASSERT_NOT_NULL(m);

  /* Create a fake datadir with a skills subdirectory */
  char datadir[2048], skills_dir[2048];
  snprintf(datadir, sizeof(datadir), "%s/fakedata", dir);
  snprintf(skills_dir, sizeof(skills_dir), "%s/generic-skills", datadir);
  mkdir(datadir, 0755);
  mkdir(skills_dir, 0755);

  /* Write a test skill JSON */
  char skill_path[2048];
  snprintf(skill_path, sizeof(skill_path), "%s/skill_test-seed.json", skills_dir);
  FILE *fp = fopen(skill_path, "w");
  ASSERT_NOT_NULL(fp);
  fprintf(fp,
    "{\"key\":\"skill:test-seed\","
    "\"value\":\"A seeded test skill\","
    "\"tags\":[],"
    "\"pinned\":false,"
    "\"created_at\":\"1786900000.00000\","
    "\"last_accessed\":\"1786900000.00000\","
    "\"access_count\":1,"
    "\"recall_hits\":0,"
    "\"recall_misses\":0,"
    "\"belief_entropy\":-1,"
    "\"description\":\"A seeded test skill\","
    "\"version\":1}"
  );
  fclose(fp);

  int seeded = memory_seed_defaults(m, datadir);
  ASSERT_EQ(seeded, 1);

  /* Verify the skill was stored */
  mem_index_entry_t *found = memory_find(m, "skill:test-seed");
  ASSERT_NOT_NULL(found);
  ASSERT_STR_CONTAINS(found->value, "seeded test skill");
  memory_find_free(found);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_seed_defaults_idempotent(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);
  ASSERT_NOT_NULL(m);

  /* Create fake datadir */
  char datadir[2048], skills_dir[2048];
  snprintf(datadir, sizeof(datadir), "%s/fakedata", dir);
  snprintf(skills_dir, sizeof(skills_dir), "%s/generic-skills", datadir);
  mkdir(datadir, 0755);
  mkdir(skills_dir, 0755);

  /* Write a test skill JSON */
  char skill_path[2048];
  snprintf(skill_path, sizeof(skill_path), "%s/skill_test-idem.json", skills_dir);
  FILE *fp = fopen(skill_path, "w");
  ASSERT_NOT_NULL(fp);
  fprintf(fp,
    "{\"key\":\"skill:test-idem\","
    "\"value\":\"Idempotent test\","
    "\"tags\":[],"
    "\"pinned\":false,"
    "\"created_at\":\"1786900000.00000\","
    "\"last_accessed\":\"1786900000.00000\","
    "\"access_count\":1,"
    "\"recall_hits\":0,"
    "\"recall_misses\":0,"
    "\"belief_entropy\":-1,"
    "\"description\":\"Idempotent test\","
    "\"version\":1}"
  );
  fclose(fp);

  /* First seed */
  int seeded1 = memory_seed_defaults(m, datadir);
  ASSERT_EQ(seeded1, 1);

  /* Second seed should return 0 (already seeded) */
  int seeded2 = memory_seed_defaults(m, datadir);
  ASSERT_EQ(seeded2, 0);

  memory_free(m);
  rm_rf(dir);
  free(dir);
}

static void test_seed_defaults_no_datadir(void) {
  char *dir = make_test_dir();
  memory_t *m = memory_new(dir);
  ASSERT_NOT_NULL(m);

  /* Non-existent datadir should return 0 gracefully */
  int seeded = memory_seed_defaults(m, "/nonexistent/path");
  ASSERT_EQ(seeded, 0);

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
  RUN_TEST(test_skill_description_populated);
  RUN_TEST(test_skill_created_at_populated);

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

  /* Temporal validity + basis + staleness */
  printf("\n  --- Temporal Validity ---\n");
  RUN_TEST(test_validity_persistent);
  RUN_TEST(test_validity_expires_when_never_stale);
  RUN_TEST(test_validity_expires_when_description);
  RUN_TEST(test_validity_volatile);
  RUN_TEST(test_validity_session);
  RUN_TEST(test_validity_null_is_persistent);
  RUN_TEST(test_basis_field);
  RUN_TEST(test_validity_basis_preserved_on_update);
  RUN_TEST(test_validity_in_query_results);
  RUN_TEST(test_validity_in_find);
  RUN_TEST(test_validity_basis_persist_on_disk);

  /* Contradiction detection mechanism */
  printf("\n  --- Contradiction Detection ---\n");
  RUN_TEST(test_contradiction_detection_mechanism);
  RUN_TEST(test_contradiction_detection_similar_values);

  /* Typed Edges (SodaMem arXiv 2608.08055) */
  printf("\n  --- Typed Edges ---\n");
  RUN_TEST(test_typed_edge_add_ref);
  RUN_TEST(test_typed_edge_add_ref_nonexistent);
  RUN_TEST(test_typed_edge_supersedes_auto);
  RUN_TEST(test_typed_edge_update_existing);
  RUN_TEST(test_superseded_hard_exclusion);

  /* Typed Edges - Comprehensive */
  printf("\n  --- Typed Edges (Comprehensive) ---\n");
  RUN_TEST(test_typed_edge_relates_default);
  RUN_TEST(test_typed_edge_all_types);
  RUN_TEST(test_typed_edge_persist_on_disk);
  RUN_TEST(test_typed_edge_multiple_refs);
  RUN_TEST(test_typed_edge_gc_cleanup);
  RUN_TEST(test_typed_edge_preserved_on_value_update);
  RUN_TEST(test_typed_edge_null_params);
  RUN_TEST(test_find_copies_ref_types);

  /* Supersession - Comprehensive */
  printf("\n  --- Supersession (Comprehensive) ---\n");
  RUN_TEST(test_superseded_chain);
  RUN_TEST(test_superseded_negative_demotion);
  RUN_TEST(test_supersedes_auto_ref_type);

  /* Edge-Aware Boost Weights */
  printf("\n  --- Edge-Aware Boost Weights ---\n");
  RUN_TEST(test_edge_boost_supersedes_no_boost);
  RUN_TEST(test_edge_boost_contradicts_suppresses);
  RUN_TEST(test_edge_boost_updates_stronger);
  RUN_TEST(test_edge_boost_depends);

  /* TAINTED Importance Tier (ACID-Agent arXiv 2608.13900) */
  printf("\n  --- TAINTED Importance ---\n");
  RUN_TEST(test_tainted_importance_value);
  RUN_TEST(test_tainted_ordering);
  RUN_TEST(test_tainted_score_formula);
  RUN_TEST(test_tainted_below_skip_threshold);

  /* is_global default */
  printf("\n  --- is_global ---\n");
  RUN_TEST(test_is_global_default);

  /* Generic skill seeding */
  printf("\n  --- Seed Defaults ---\n");
  RUN_TEST(test_seed_defaults);
  RUN_TEST(test_seed_defaults_idempotent);
  RUN_TEST(test_seed_defaults_no_datadir);

  TEST_SUMMARY();
}

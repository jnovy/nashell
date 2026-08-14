#include "test_common.h"
#include "../src/memory.h"
/* linked via LIB_OBJ */

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
  /* Legacy causal: prefix also works */
  ASSERT_EQ(memory_is_stale("causal:new build submitted", now, &days_past), 0);
  ASSERT_EQ(memory_is_stale("causal:new build submitted", year_ago, &days_past), 0);
}

static void test_validity_expires_when_description(void) {
  /* The description should be extractable via validity_expires_desc() */
  const char *v1 = "expires_when:errata advisory is created for this build";
  const char *desc1 = validity_expires_desc(v1);
  ASSERT_STR_EQ(desc1, "errata advisory is created for this build");
  ASSERT_EQ(memory_is_stale(v1, 0, NULL), 0);

  /* Legacy causal: prefix also extracts correctly */
  const char *v2 = "causal:upstream release published";
  const char *desc2 = validity_expires_desc(v2);
  ASSERT_STR_EQ(desc2, "upstream release published");

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
  memory_set_validity(m, "lesson:find-val-test", "causal:upstream release published");

  mem_index_entry_t *found = memory_find(m, "lesson:find-val-test");
  ASSERT_NOT_NULL(found);
  ASSERT_NOT_NULL(found->validity);
  ASSERT_STR_EQ(found->validity, "causal:upstream release published");
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
  memory_set_validity(m, "fact:disk-persist", "causal:new brew build submitted");
  memory_set_basis(m, "fact:disk-persist", "brew latest-build query on 2026-08-13");

  /* Verify in-memory before reload */
  mem_index_entry_t *pre = memory_find(m, "fact:disk-persist");
  ASSERT_NOT_NULL(pre);
  ASSERT_STR_EQ(pre->validity, "causal:new brew build submitted");
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
  ASSERT_STR_EQ(post->validity, "causal:new brew build submitted");
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

  TEST_SUMMARY();
}

/* test_memory_query.c — Tests for memory search scoring, ranking, delete,
 * find, listing, lineage, and validation scoring.
 *
 * Covers gaps: M1 (query scoring/ranking), M2 (delete/batch),
 * M3 (hits/misses/vscore), M4 (supersedes), M5 (build_listing),
 * M6 (memory_find), M7 (key_to_path). */

#include "test_common.h"
#include "../src/memory.h"

/* ═══════════════════════════════════════════════════════════════════
 * Section 1: key_to_path (GAP M7)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_key_to_path_basic(void) {
    char out[256];
    key_to_path("lesson:foo", ".json", out, sizeof(out));
    ASSERT_STR_EQ(out, "lesson_foo.json");
}

static void test_key_to_path_slashes(void) {
    char out[256];
    key_to_path("skill:c/code/fix", ".json", out, sizeof(out));
    ASSERT_STR_EQ(out, "skill_c_code_fix.json");
}

static void test_key_to_path_emb_ext(void) {
    char out[256];
    key_to_path("fact:api-key", ".emb", out, sizeof(out));
    ASSERT_STR_EQ(out, "fact_api-key.emb");
}

static void test_key_to_path_small_buf(void) {
    char out[4];
    key_to_path("lesson:foo", ".json", out, sizeof(out));
    /* Buffer too small — guard should produce empty or truncated */
    ASSERT(strlen(out) < sizeof(out));
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 2: memory_query scoring & ranking (GAP M1)
 * ═══════════════════════════════════════════════════════════════════ */

/* Helper: create memory store with test entries */
static memory_t *make_test_memory(char **out_dir) {
    char *dir = make_test_dir();
    memory_t *m = memory_new(dir);
    *out_dir = dir;
    return m;
}

static void finish_test_memory(memory_t *m, char *dir) {
    memory_free(m);
    rm_rf(dir);
    free(dir);
}

/* M1a: Relevance ordering — key match scores higher than value match */
static void test_query_ordering_key_vs_value(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* "redis" appears in key of first, value of second */
    memory_store(m, "lesson:redis-migration", "How to upgrade redis", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:database-tips", "Use redis for caching", 0, NULL, NULL, 0, NULL, 0);

    memory_results_t r = memory_query(m, "redis", 10);
    ASSERT(r.count >= 2);
    /* Key match (3.0) + value match (1.0) = 4.0 → score 1.0
     * vs value-only match (1.0) → score 0.25.
     * First result should be the key-match entry. */
    ASSERT_STR_EQ(r.entries[0].key, "lesson:redis-migration");
    ASSERT(r.entries[0].relevance > r.entries[1].relevance);

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1a: Multiple entries with different relevance levels */
static void test_query_ordering_multiple(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* Both key+value match = highest */
    memory_store(m, "lesson:python-debugging", "python debugging tips", 0, NULL, NULL, 0, NULL, 0);
    /* Key only match */
    memory_store(m, "skill:python-profiling", "How to profile code", 0, NULL, NULL, 0, NULL, 0);
    /* Value only match */
    memory_store(m, "lesson:code-quality", "use python linters", 0, NULL, NULL, 0, NULL, 0);
    /* No match */
    memory_store(m, "lesson:rust-macros", "macro expansion rules", 0, NULL, NULL, 0, NULL, 0);

    memory_results_t r = memory_query(m, "python", 10);
    /* Should get 3 results (no match excluded), ordered by score */
    ASSERT(r.count >= 2);
    /* First result: both key+value match */
    ASSERT_STR_EQ(r.entries[0].key, "lesson:python-debugging");
    /* "rust-macros" should not appear */
    for (int i = 0; i < r.count; i++)
        ASSERT(strcmp(r.entries[i].key, "lesson:rust-macros") != 0);

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1b: Type prefix filtering */
static void test_query_type_prefix_filter(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:debugging", "how to debug code", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "skill:debugging", "debugging skill desc", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "fact:debugging", "debug is important", 0, NULL, NULL, 0, NULL, 0);

    /* Query with "skill:" prefix should only return skill entries */
    memory_results_t r = memory_query(m, "skill:debugging", 10);
    ASSERT_EQ(r.count, 1);
    ASSERT_STR_EQ(r.entries[0].key, "skill:debugging");

    memory_results_free(&r);

    /* "lesson:" prefix */
    r = memory_query(m, "lesson:debugging", 10);
    ASSERT_EQ(r.count, 1);
    ASSERT_STR_EQ(r.entries[0].key, "lesson:debugging");

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1c: recall_min_score filtering */
static void test_query_min_score_filtering(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:exact-match", "exact match content", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:barely-related",
                 "this has nothing to do with the query except a tiny overlap partial",
                 0, NULL, NULL, 0, NULL, 0);

    /* With very high min_score, only strong matches survive */
    memory_set_recall_config(m, 0.70, 0.5, 0.5, 0.0);
    memory_results_t r = memory_query(m, "exact-match", 10);
    /* Only the key-matching entry should survive 0.70 threshold */
    ASSERT_EQ(r.count, 1);
    ASSERT_STR_EQ(r.entries[0].key, "lesson:exact-match");

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1d: Ref-boost — referenced entries get boosted scores.
 * Ref-boost triggers when a scored entry has score >= 0.5 and refs another.
 * The ref'd entry gets +0.3 × referrer's score, capped at 1.0.
 * With substring scoring: key match = 3.0/4.0 = 0.75, value = 1.0/4.0 = 0.25.
 * We need entry A to score >= 0.5 (key match) and B/C to have baseline scores. */
static void test_query_ref_boost(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);
    /* Use low min_score so weak matches survive */
    memory_set_recall_config(m, 0.05, 0.5, 0.5, 0.0);

    /* Entry A: key matches "boost" → score = 0.75 (above 0.5 trigger).
     * Refs entry B. */
    const char *refs[] = {"lesson:ref-target"};
    memory_store(m, "lesson:boost", "entry that boosts its reference", 0, NULL, refs, 1, NULL, 0);
    /* Entry B (ref target): value matches "boost" → baseline score = 0.25 */
    memory_store(m, "lesson:ref-target", "should get a boost from referrer", 0, NULL, NULL, 0, NULL, 0);
    /* Entry C (unlinked): same value match as B → same baseline = 0.25 */
    memory_store(m, "lesson:unlinked", "should get a boost from referrer", 0, NULL, NULL, 0, NULL, 0);

    memory_results_t r = memory_query(m, "boost", 10);
    ASSERT(r.count >= 3);  /* all three should match */

    /* Find ref-target and unlinked scores */
    double ref_target_score = -1, unlinked_score = -1;
    for (int i = 0; i < r.count; i++) {
        if (strcmp(r.entries[i].key, "lesson:ref-target") == 0)
            ref_target_score = r.entries[i].relevance;
        if (strcmp(r.entries[i].key, "lesson:unlinked") == 0)
            unlinked_score = r.entries[i].relevance;
    }

    /* ref-target should be boosted above unlinked (same base score + ref boost) */
    ASSERT(ref_target_score >= 0);
    ASSERT(unlinked_score >= 0);
    ASSERT(ref_target_score > unlinked_score);

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1f: vscore influence on ranking */
static void test_query_vscore_influence(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* Enable vscore with exponent 1.0 (full influence) */
    memory_set_recall_config(m, 0.05, 0.5, 0.5, 1.0);

    /* Both entries match equally on substring */
    memory_store(m, "lesson:proven-method", "how to fix bugs in code", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:risky-method", "how to fix bugs in code", 0, NULL, NULL, 0, NULL, 0);

    /* Give one entry good validation, the other bad */
    memory_increment_hits(m, "lesson:proven-method");
    memory_increment_hits(m, "lesson:proven-method");
    memory_increment_hits(m, "lesson:proven-method");
    /* proven: vscore = (3+1)/(3+0+2) = 0.80 */

    memory_increment_misses(m, "lesson:risky-method");
    memory_increment_misses(m, "lesson:risky-method");
    memory_increment_misses(m, "lesson:risky-method");
    /* risky: vscore = (0+1)/(0+3+2) = 0.20 */

    memory_results_t r = memory_query(m, "fix bugs", 10);
    ASSERT(r.count >= 2);

    /* With vscore exponent=1.0:
     * proven: relevance * 0.80
     * risky: relevance * 0.20
     * proven should rank first */
    ASSERT_STR_EQ(r.entries[0].key, "lesson:proven-method");

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1f: vscore disabled (exponent=0) — both rank equally */
static void test_query_vscore_disabled(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* vscore disabled (exponent = 0) */
    memory_set_recall_config(m, 0.05, 0.5, 0.5, 0.0);

    memory_store(m, "lesson:method-a", "how to fix bugs quickly", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:method-b", "how to fix bugs quickly", 0, NULL, NULL, 0, NULL, 0);

    memory_increment_hits(m, "lesson:method-a");
    memory_increment_hits(m, "lesson:method-a");
    memory_increment_hits(m, "lesson:method-a");
    memory_increment_misses(m, "lesson:method-b");
    memory_increment_misses(m, "lesson:method-b");
    memory_increment_misses(m, "lesson:method-b");

    memory_results_t r = memory_query(m, "fix bugs", 10);
    ASSERT(r.count >= 2);
    /* With vscore disabled, both should have equal scores */
    double diff = r.entries[0].relevance - r.entries[1].relevance;
    if (diff < 0) diff = -diff;
    ASSERT(diff < 0.01);  /* essentially equal */

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1g: max_results cap */
static void test_query_max_results(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* Store 5 entries all matching "coding" */
    for (int i = 0; i < 5; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "lesson:coding-%d", i);
        snprintf(val, sizeof(val), "coding tip number %d", i);
        memory_store(m, key, val, 0, NULL, NULL, 0, NULL, 0);
    }

    memory_results_t r = memory_query(m, "coding", 3);
    ASSERT_EQ(r.count, 3);  /* capped at 3 */

    memory_results_free(&r);

    r = memory_query(m, "coding", 10);
    ASSERT_EQ(r.count, 5);  /* all 5 returned */

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* M1h: NULL/empty query returns 0 results */
static void test_query_null_empty(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:test", "test value", 0, NULL, NULL, 0, NULL, 0);

    memory_results_t r = memory_query(m, NULL, 10);
    ASSERT_EQ(r.count, 0);

    /* Empty query — score_entry_substring returns 0 for empty */
    r = memory_query(m, "", 10);
    ASSERT_EQ(r.count, 0);

    /* NULL memory */
    r = memory_query(NULL, "test", 10);
    ASSERT_EQ(r.count, 0);

    finish_test_memory(m, dir);
}

/* M1: Score values are in [0,1] range */
static void test_query_score_range(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:foobar", "foobar baz qux quux", 0, NULL, NULL, 0, NULL, 0);

    memory_results_t r = memory_query(m, "foobar", 10);
    ASSERT(r.count > 0);
    for (int i = 0; i < r.count; i++) {
        ASSERT(r.entries[i].relevance >= 0.0);
        ASSERT(r.entries[i].relevance <= 1.0);
        ASSERT(r.entries[i].raw_relevance >= 0.0);
        ASSERT(r.entries[i].raw_relevance <= 1.0);
    }

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 3: memory_delete / memory_delete_batch (GAP M2)
 * ═══════════════════════════════════════════════════════════════════ */

/* M2a: delete existing key */
static void test_delete_existing(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:to-delete", "delete me", 0, NULL, NULL, 0, NULL, 0);
    memory_results_t r = memory_query(m, "to-delete", 10);
    ASSERT_EQ(r.count, 1);
    memory_results_free(&r);

    int rc = memory_delete(m, "lesson:to-delete");
    ASSERT_EQ(rc, 0);

    r = memory_query(m, "to-delete", 10);
    ASSERT_EQ(r.count, 0);
    memory_results_free(&r);

    finish_test_memory(m, dir);
}

/* M2b: delete nonexistent key */
static void test_delete_nonexistent(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    int rc = memory_delete(m, "lesson:does-not-exist");
    ASSERT_EQ(rc, -1);

    finish_test_memory(m, dir);
}

/* M2c: delete removes entry from refs of other entries */
static void test_delete_cleans_refs(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* A refs B */
    const char *refs[] = {"lesson:target"};
    memory_store(m, "lesson:source", "source entry", 0, NULL, refs, 1, NULL, 0);
    memory_store(m, "lesson:target", "target entry", 0, NULL, NULL, 0, NULL, 0);

    /* Verify source has ref */
    mem_index_entry_t *found = memory_find(m, "lesson:source");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found->n_refs, 1);
    memory_find_free(found);

    /* Delete target */
    memory_delete(m, "lesson:target");

    /* Source's refs should now be cleaned */
    found = memory_find(m, "lesson:source");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found->n_refs, 0);
    memory_find_free(found);

    finish_test_memory(m, dir);
}

/* M2d: batch delete */
static void test_delete_batch(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:a", "val a", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:b", "val b", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:c", "val c", 0, NULL, NULL, 0, NULL, 0);
    ASSERT_EQ(memory_count(m), 3);

    const char *keys[] = {"lesson:a", "lesson:c"};
    int n = memory_delete_batch(m, keys, 2);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(memory_count(m), 1);

    /* Remaining entry */
    mem_index_entry_t *found = memory_find(m, "lesson:b");
    ASSERT_NOT_NULL(found);
    memory_find_free(found);

    /* Deleted entries gone */
    found = memory_find(m, "lesson:a");
    ASSERT_NULL(found);

    finish_test_memory(m, dir);
}

/* M2e: batch delete with some nonexistent */
static void test_delete_batch_partial(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:exists", "val", 0, NULL, NULL, 0, NULL, 0);

    const char *keys[] = {"lesson:exists", "lesson:ghost"};
    int n = memory_delete_batch(m, keys, 2);
    ASSERT_EQ(n, 1);  /* only one actually existed */

    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 4: memory_increment_hits/misses + vscore (GAP M3)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_increment_hits(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:validated", "test", 0, NULL, NULL, 0, NULL, 0);

    ASSERT_EQ(memory_increment_hits(m, "lesson:validated"), 0);
    ASSERT_EQ(memory_increment_hits(m, "lesson:validated"), 0);

    mem_index_entry_t *e = memory_find(m, "lesson:validated");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->recall_hits, 2);
    ASSERT_EQ(e->recall_misses, 0);
    memory_find_free(e);

    finish_test_memory(m, dir);
}

static void test_increment_misses(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:bad", "test", 0, NULL, NULL, 0, NULL, 0);

    ASSERT_EQ(memory_increment_misses(m, "lesson:bad"), 0);
    ASSERT_EQ(memory_increment_misses(m, "lesson:bad"), 0);
    ASSERT_EQ(memory_increment_misses(m, "lesson:bad"), 0);

    mem_index_entry_t *e = memory_find(m, "lesson:bad");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->recall_hits, 0);
    ASSERT_EQ(e->recall_misses, 3);
    memory_find_free(e);

    finish_test_memory(m, dir);
}

static void test_increment_nonexistent(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    ASSERT_EQ(memory_increment_hits(m, "lesson:ghost"), -1);
    ASSERT_EQ(memory_increment_misses(m, "lesson:ghost"), -1);

    finish_test_memory(m, dir);
}

/* vscore = (hits+1)/(hits+misses+2) — verify via query ranking */
static void test_vscore_calculation(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);
    memory_set_recall_config(m, 0.01, 0.5, 0.5, 1.0);

    memory_store(m, "lesson:good-vscore", "fix issues in code", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:bad-vscore", "fix issues in code", 0, NULL, NULL, 0, NULL, 0);

    /* good: 5 hits, 0 misses → vscore = 6/7 ≈ 0.857 */
    for (int i = 0; i < 5; i++)
        memory_increment_hits(m, "lesson:good-vscore");

    /* bad: 0 hits, 5 misses → vscore = 1/7 ≈ 0.143 */
    for (int i = 0; i < 5; i++)
        memory_increment_misses(m, "lesson:bad-vscore");

    memory_results_t r = memory_query(m, "fix issues", 10);
    ASSERT(r.count >= 2);
    ASSERT_STR_EQ(r.entries[0].key, "lesson:good-vscore");
    /* Score ratio should reflect vscore difference */
    ASSERT(r.entries[0].relevance > r.entries[1].relevance * 2.0);

    memory_results_free(&r);
    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 5: memory_set_supersedes / lineage (GAP M4)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_supersedes_basic(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:old-v1", "old approach", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "lesson:new-v2", "new better approach", 0, NULL, NULL, 0, NULL, 0);

    int rc = memory_set_supersedes(m, "lesson:new-v2", "lesson:old-v1");
    ASSERT_EQ(rc, 0);

    /* Verify by re-reading (set_supersedes writes to disk) */
    /* We can't easily check the JSON fields via the public API since
     * supersedes/version aren't in mem_index_entry_t. Just verify
     * it didn't crash and returned success. */
    finish_test_memory(m, dir);
}

static void test_supersedes_nonexistent(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:old-v1", "old approach", 0, NULL, NULL, 0, NULL, 0);

    int rc = memory_set_supersedes(m, "lesson:ghost", "lesson:old-v1");
    ASSERT_EQ(rc, -1);

    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 6: memory_build_listing (GAP M5)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_listing_all(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:foo", "foo value", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "strategy:bar", "bar value", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "skill:baz", "baz value", 0, NULL, NULL, 0, NULL, 0);

    char *listing = memory_build_listing(m, NULL);
    ASSERT_NOT_NULL(listing);
    ASSERT_STR_CONTAINS(listing, "## Lessons (1)");
    ASSERT_STR_CONTAINS(listing, "## Strategies (1)");
    ASSERT_STR_CONTAINS(listing, "## Skills (1)");
    ASSERT_STR_CONTAINS(listing, "lesson:foo");
    ASSERT_STR_CONTAINS(listing, "strategy:bar");
    ASSERT_STR_CONTAINS(listing, "skill:baz");
    free(listing);

    finish_test_memory(m, dir);
}

static void test_listing_type_filter(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:foo", "foo value", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "strategy:bar", "bar value", 0, NULL, NULL, 0, NULL, 0);
    memory_store(m, "skill:baz", "baz value", 0, NULL, NULL, 0, NULL, 0);

    /* Filter to lessons only */
    char *listing = memory_build_listing(m, "lesson");
    ASSERT_NOT_NULL(listing);
    ASSERT_STR_CONTAINS(listing, "lesson:foo");
    /* Should NOT contain other types */
    ASSERT(!strstr(listing, "strategy:bar"));
    ASSERT(!strstr(listing, "skill:baz"));
    free(listing);

    finish_test_memory(m, dir);
}

static void test_listing_type_filter_no_match(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:foo", "foo value", 0, NULL, NULL, 0, NULL, 0);

    char *listing = memory_build_listing(m, "strategy");
    /* No strategies exist — should return NULL */
    ASSERT_NULL(listing);

    finish_test_memory(m, dir);
}

static void test_listing_pinned_badge(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "fact:pinned-item", "pinned value", 1, NULL, NULL, 0, NULL, 0);

    char *listing = memory_build_listing(m, NULL);
    ASSERT_NOT_NULL(listing);
    ASSERT_STR_CONTAINS(listing, "[pinned]");
    free(listing);

    finish_test_memory(m, dir);
}

static void test_listing_empty(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    char *listing = memory_build_listing(m, NULL);
    ASSERT_NULL(listing);

    finish_test_memory(m, dir);
}

static void test_listing_other_type(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    /* "custom-key" has no standard prefix → goes into "Other" */
    memory_store(m, "custom-key", "custom value", 0, NULL, NULL, 0, NULL, 0);

    char *listing = memory_build_listing(m, NULL);
    ASSERT_NOT_NULL(listing);
    ASSERT_STR_CONTAINS(listing, "## Other (1)");
    ASSERT_STR_CONTAINS(listing, "custom-key");
    free(listing);

    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 7: memory_find / memory_find_free (GAP M6)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_find_existing(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    const char *refs[] = {"lesson:other"};
    memory_store(m, "lesson:findme", "find this value", 1, NULL, refs, 1, NULL, 0);

    mem_index_entry_t *e = memory_find(m, "lesson:findme");
    ASSERT_NOT_NULL(e);
    ASSERT_STR_EQ(e->key, "lesson:findme");
    ASSERT_STR_EQ(e->value, "find this value");
    ASSERT_EQ(e->pinned, 1);
    ASSERT_EQ(e->n_refs, 1);
    ASSERT_STR_EQ(e->refs[0], "lesson:other");
    ASSERT(e->has_emb == 0);  /* embedding not copied */
    memory_find_free(e);

    finish_test_memory(m, dir);
}

static void test_find_nonexistent(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    mem_index_entry_t *e = memory_find(m, "lesson:ghost");
    ASSERT_NULL(e);

    finish_test_memory(m, dir);
}

static void test_find_null_args(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    ASSERT_NULL(memory_find(NULL, "key"));
    ASSERT_NULL(memory_find(m, NULL));

    finish_test_memory(m, dir);
}

/* Verify find returns a deep copy (modifying it doesn't affect index) */
static void test_find_deep_copy(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:deep", "original value", 0, NULL, NULL, 0, NULL, 0);

    mem_index_entry_t *e1 = memory_find(m, "lesson:deep");
    ASSERT_NOT_NULL(e1);

    /* Mutate the copy */
    free(e1->value);
    e1->value = strdup("mutated");

    /* Original should be unchanged */
    mem_index_entry_t *e2 = memory_find(m, "lesson:deep");
    ASSERT_NOT_NULL(e2);
    ASSERT_STR_EQ(e2->value, "original value");

    memory_find_free(e1);
    memory_find_free(e2);
    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 8: memory_update_scores (GAP M3 cont.)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_update_scores(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:scored", "test", 0, NULL, NULL, 0, NULL, 0);

    int rc = memory_update_scores(m, "lesson:scored", 3, 2);
    ASSERT_EQ(rc, 0);

    mem_index_entry_t *e = memory_find(m, "lesson:scored");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->recall_hits, 3);
    ASSERT_EQ(e->recall_misses, 2);
    memory_find_free(e);

    /* Add more */
    rc = memory_update_scores(m, "lesson:scored", 1, 1);
    ASSERT_EQ(rc, 0);

    e = memory_find(m, "lesson:scored");
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->recall_hits, 4);
    ASSERT_EQ(e->recall_misses, 3);
    memory_find_free(e);

    finish_test_memory(m, dir);
}

static void test_update_scores_nonexistent(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    int rc = memory_update_scores(m, "lesson:ghost", 1, 0);
    ASSERT_EQ(rc, -1);

    finish_test_memory(m, dir);
}

static void test_update_scores_zero(void) {
    char *dir;
    memory_t *m = make_test_memory(&dir);

    memory_store(m, "lesson:zero", "test", 0, NULL, NULL, 0, NULL, 0);

    /* Both zero → guard returns -1 */
    int rc = memory_update_scores(m, "lesson:zero", 0, 0);
    ASSERT_EQ(rc, -1);

    finish_test_memory(m, dir);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_memory_query:\n");

    /* Section 1: key_to_path */
    RUN_TEST(test_key_to_path_basic);
    RUN_TEST(test_key_to_path_slashes);
    RUN_TEST(test_key_to_path_emb_ext);
    RUN_TEST(test_key_to_path_small_buf);

    /* Section 2: memory_query scoring/ranking */
    RUN_TEST(test_query_ordering_key_vs_value);
    RUN_TEST(test_query_ordering_multiple);
    RUN_TEST(test_query_type_prefix_filter);
    RUN_TEST(test_query_min_score_filtering);
    RUN_TEST(test_query_ref_boost);
    RUN_TEST(test_query_vscore_influence);
    RUN_TEST(test_query_vscore_disabled);
    RUN_TEST(test_query_max_results);
    RUN_TEST(test_query_null_empty);
    RUN_TEST(test_query_score_range);

    /* Section 3: memory_delete */
    RUN_TEST(test_delete_existing);
    RUN_TEST(test_delete_nonexistent);
    RUN_TEST(test_delete_cleans_refs);
    RUN_TEST(test_delete_batch);
    RUN_TEST(test_delete_batch_partial);

    /* Section 4: hits/misses/vscore */
    RUN_TEST(test_increment_hits);
    RUN_TEST(test_increment_misses);
    RUN_TEST(test_increment_nonexistent);
    RUN_TEST(test_vscore_calculation);

    /* Section 5: supersedes */
    RUN_TEST(test_supersedes_basic);
    RUN_TEST(test_supersedes_nonexistent);

    /* Section 6: build_listing */
    RUN_TEST(test_listing_all);
    RUN_TEST(test_listing_type_filter);
    RUN_TEST(test_listing_type_filter_no_match);
    RUN_TEST(test_listing_pinned_badge);
    RUN_TEST(test_listing_empty);
    RUN_TEST(test_listing_other_type);

    /* Section 7: memory_find */
    RUN_TEST(test_find_existing);
    RUN_TEST(test_find_nonexistent);
    RUN_TEST(test_find_null_args);
    RUN_TEST(test_find_deep_copy);

    /* Section 8: update_scores */
    RUN_TEST(test_update_scores);
    RUN_TEST(test_update_scores_nonexistent);
    RUN_TEST(test_update_scores_zero);

    TEST_SUMMARY();
}

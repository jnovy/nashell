/* test_cycling.c - Unit tests for sliding-window cycle detection.
 * Tests the cycle_window_t data structure and detection algorithm
 * without requiring LLM or react loop infrastructure. */

#include "test_common.h"
#include "react_internal.h"
#include <math.h>

/* ---- Test cases ---- */

static void test_no_cycle_empty(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  ASSERT_EQ(cw.cycle_len, 0);
  ASSERT_EQ(cw.cycle_occurrences, 0);
  cycle_window_free(&cw);
}

static void test_no_cycle_unique(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "C", NULL, NULL, 3);
  cycle_window_push(&cw, "D", NULL, NULL, 4);
  cycle_window_push(&cw, "E", NULL, NULL, 5);
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  cycle_window_free(&cw);
}

static void test_period1_simple(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "A", NULL, NULL, 2);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 1);
  ASSERT_EQ(cw.cycle_len, 1);
  ASSERT_EQ(cw.cycle_occurrences, 2);
  cycle_window_free(&cw);
}

static void test_period1_triple(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "A", NULL, NULL, 2);
  cycle_window_push(&cw, "A", NULL, NULL, 3);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 1);
  ASSERT_EQ(cw.cycle_occurrences, 3);
  cycle_window_free(&cw);
}

static void test_period2_abab(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "X", NULL, NULL, 1); /* non-cycle prefix */
  cycle_window_push(&cw, "A", NULL, NULL, 2);
  cycle_window_push(&cw, "B", NULL, NULL, 3);
  cycle_window_push(&cw, "A", NULL, NULL, 4);
  cycle_window_push(&cw, "B", NULL, NULL, 5);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 2);
  ASSERT_EQ(cw.cycle_occurrences, 2);
  cycle_window_free(&cw);
}

static void test_period2_ababab(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "A", NULL, NULL, 3);
  cycle_window_push(&cw, "B", NULL, NULL, 4);
  cycle_window_push(&cw, "A", NULL, NULL, 5);
  cycle_window_push(&cw, "B", NULL, NULL, 6);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 2);
  ASSERT_EQ(cw.cycle_occurrences, 3);
  cycle_window_free(&cw);
}

static void test_period3_abcabc(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "C", NULL, NULL, 3);
  cycle_window_push(&cw, "A", NULL, NULL, 4);
  cycle_window_push(&cw, "B", NULL, NULL, 5);
  cycle_window_push(&cw, "C", NULL, NULL, 6);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 3);
  ASSERT_EQ(cw.cycle_occurrences, 2);
  cycle_window_free(&cw);
}

static void test_period4_max(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "C", NULL, NULL, 3);
  cycle_window_push(&cw, "D", NULL, NULL, 4);
  cycle_window_push(&cw, "A", NULL, NULL, 5);
  cycle_window_push(&cw, "B", NULL, NULL, 6);
  cycle_window_push(&cw, "C", NULL, NULL, 7);
  cycle_window_push(&cw, "D", NULL, NULL, 8);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 4);
  ASSERT_EQ(cw.cycle_occurrences, 2);
  cycle_window_free(&cw);
}

static void test_no_cycle_period5(void) {
  /* Period 5 exceeds CYCLE_MAX_PERIOD=4, should not be detected */
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "C", NULL, NULL, 3);
  cycle_window_push(&cw, "D", NULL, NULL, 4);
  cycle_window_push(&cw, "E", NULL, NULL, 5);
  cycle_window_push(&cw, "A", NULL, NULL, 6);
  cycle_window_push(&cw, "B", NULL, NULL, 7);
  cycle_window_push(&cw, "C", NULL, NULL, 8);
  cycle_window_push(&cw, "D", NULL, NULL, 9);
  cycle_window_push(&cw, "E", NULL, NULL, 10);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 0);
  cycle_window_free(&cw);
}

static void test_cycle_broken(void) {
  /* A,B,A,B,C - cycle broken by C at the end */
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "A", NULL, NULL, 3);
  cycle_window_push(&cw, "B", NULL, NULL, 4);
  cycle_window_push(&cw, "C", NULL, NULL, 5);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 0);
  cycle_window_free(&cw);
}

static void test_find_in_window(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  int s0 = cycle_window_push(&cw, "A", "r1", "ref1", 1);
  cycle_window_push(&cw, "B", "r2", "ref2", 2);
  cycle_window_push(&cw, "C", "r3", "ref3", 3);
  int s3 = cycle_window_push(&cw, "A", "r4", "ref4", 4);
  /* Find A, skipping the one we just pushed (s3) */
  int found = cycle_window_find(&cw, "A", s3);
  ASSERT_EQ(found, s0);
  /* Verify stored result from the found slot */
  ASSERT_STR_EQ(cw.results[found], "r1");
  ASSERT_STR_EQ(cw.refs[found], "ref1");
  cycle_window_free(&cw);
}

static void test_find_not_present(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_push(&cw, "C", NULL, NULL, 3);
  int found = cycle_window_find(&cw, "D", -1);
  ASSERT_EQ(found, -1);
  cycle_window_free(&cw);
}

static void test_find_null_sig(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  int found = cycle_window_find(&cw, NULL, -1);
  ASSERT_EQ(found, -1);
  cycle_window_free(&cw);
}

static void test_window_overflow(void) {
  /* Push more than CYCLE_WINDOW_SIZE entries; oldest evicted */
  cycle_window_t cw;
  cycle_window_init(&cw);
  for (int i = 0; i < CYCLE_WINDOW_SIZE + 5; i++) {
    char sig[16];
    snprintf(sig, sizeof(sig), "U%d", i);
    cycle_window_push(&cw, sig, NULL, NULL, i + 1);
  }
  ASSERT_EQ(cw.count, CYCLE_WINDOW_SIZE);
  /* The first 5 entries should be gone */
  ASSERT_EQ(cycle_window_find(&cw, "U0", -1), -1);
  ASSERT_EQ(cycle_window_find(&cw, "U4", -1), -1);
  /* But recent ones should exist */
  ASSERT(cycle_window_find(&cw, "U5", -1) >= 0);
  ASSERT(cycle_window_find(&cw, "U16", -1) >= 0);
  /* No cycle in unique sigs */
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  cycle_window_free(&cw);
}

static void test_overflow_with_cycle(void) {
  /* Push enough unique sigs to overflow, then a cycle at the end */
  cycle_window_t cw;
  cycle_window_init(&cw);
  for (int i = 0; i < CYCLE_WINDOW_SIZE - 4; i++) {
    char sig[16];
    snprintf(sig, sizeof(sig), "U%d", i);
    cycle_window_push(&cw, sig, NULL, NULL, i + 1);
  }
  /* Now add A,B,A,B at the end */
  int base = CYCLE_WINDOW_SIZE - 4;
  cycle_window_push(&cw, "A", NULL, NULL, base + 1);
  cycle_window_push(&cw, "B", NULL, NULL, base + 2);
  cycle_window_push(&cw, "A", NULL, NULL, base + 3);
  cycle_window_push(&cw, "B", NULL, NULL, base + 4);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 2);
  ASSERT_EQ(cw.cycle_occurrences, 2);
  cycle_window_free(&cw);
}

static void test_reset_preserves_counters(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cw.tokens_baseline = 5000;
  cw.tokens_total = 12000;
  cw.cycling_steps = 7;
  cw.total_recoveries = 3;
  cycle_window_push(&cw, "A", "result", "ref", 1);
  cycle_window_reset(&cw);
  /* Counters preserved */
  ASSERT_EQ(cw.tokens_baseline, 5000);
  ASSERT_EQ(cw.tokens_total, 12000);
  ASSERT_EQ(cw.cycling_steps, 7);
  ASSERT_EQ(cw.total_recoveries, 3);
  /* Buffer cleared */
  ASSERT_EQ(cw.count, 0);
  ASSERT_EQ(cw.head, 0);
  ASSERT_EQ(cycle_window_find(&cw, "A", -1), -1);
  cycle_window_free(&cw);
}

static void test_amplification_ratio(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cw.tokens_baseline = 1000;
  cw.tokens_total = 3500;
  float ratio = cycle_window_amplification(&cw);
  ASSERT(fabsf(ratio - 3.5f) < 0.01f);
  cycle_window_free(&cw);
}

static void test_amplification_no_baseline(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cw.tokens_baseline = 0;
  cw.tokens_total = 5000;
  float ratio = cycle_window_amplification(&cw);
  ASSERT(fabsf(ratio - 1.0f) < 0.01f);
  cycle_window_free(&cw);
}

static void test_describe_period2(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 5);
  cycle_window_push(&cw, "B", NULL, NULL, 6);
  cycle_window_push(&cw, "A", NULL, NULL, 7);
  cycle_window_push(&cw, "B", NULL, NULL, 8);
  cycle_window_detect(&cw);
  char *desc = cycle_window_describe(&cw);
  ASSERT_NOT_NULL(desc);
  ASSERT_STR_CONTAINS(desc, "period=2");
  ASSERT_STR_CONTAINS(desc, "2 repetitions");
  ASSERT_STR_CONTAINS(desc, "5");
  ASSERT_STR_CONTAINS(desc, "8");
  free(desc);
  cycle_window_free(&cw);
}

static void test_describe_no_cycle(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "B", NULL, NULL, 2);
  cycle_window_detect(&cw);
  char *desc = cycle_window_describe(&cw);
  ASSERT_NULL(desc);
  cycle_window_free(&cw);
}

static void test_eviction_reset(void) {
  /* Simulate: build up a cycle, reset (as eviction would), verify clean */
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", "r1", NULL, 1);
  cycle_window_push(&cw, "A", "r2", NULL, 2);
  ASSERT_EQ(cycle_window_detect(&cw), 1);
  cycle_window_reset(&cw);
  /* No stale matches */
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  ASSERT_EQ(cycle_window_find(&cw, "A", -1), -1);
  /* Push new sigs - should work cleanly */
  cycle_window_push(&cw, "X", NULL, NULL, 10);
  cycle_window_push(&cw, "Y", NULL, NULL, 11);
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  ASSERT(cycle_window_find(&cw, "X", -1) >= 0);
  cycle_window_free(&cw);
}

static void test_single_entry(void) {
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  ASSERT_EQ(cycle_window_detect(&cw), 0);
  ASSERT_EQ(cw.count, 1);
  cycle_window_free(&cw);
}

static void test_period1_prefers_over_period2(void) {
  /* A,A,A,A could be period-1 (4 reps) or period-2 (2 reps).
   * Algorithm should prefer the shorter period. */
  cycle_window_t cw;
  cycle_window_init(&cw);
  cycle_window_push(&cw, "A", NULL, NULL, 1);
  cycle_window_push(&cw, "A", NULL, NULL, 2);
  cycle_window_push(&cw, "A", NULL, NULL, 3);
  cycle_window_push(&cw, "A", NULL, NULL, 4);
  int p = cycle_window_detect(&cw);
  ASSERT_EQ(p, 1);
  ASSERT_EQ(cw.cycle_occurrences, 4);
  cycle_window_free(&cw);
}

int main(void) {
  printf("test_cycling:\n");
  RUN_TEST(test_no_cycle_empty);
  RUN_TEST(test_no_cycle_unique);
  RUN_TEST(test_period1_simple);
  RUN_TEST(test_period1_triple);
  RUN_TEST(test_period2_abab);
  RUN_TEST(test_period2_ababab);
  RUN_TEST(test_period3_abcabc);
  RUN_TEST(test_period4_max);
  RUN_TEST(test_no_cycle_period5);
  RUN_TEST(test_cycle_broken);
  RUN_TEST(test_find_in_window);
  RUN_TEST(test_find_not_present);
  RUN_TEST(test_find_null_sig);
  RUN_TEST(test_window_overflow);
  RUN_TEST(test_overflow_with_cycle);
  RUN_TEST(test_reset_preserves_counters);
  RUN_TEST(test_amplification_ratio);
  RUN_TEST(test_amplification_no_baseline);
  RUN_TEST(test_describe_period2);
  RUN_TEST(test_describe_no_cycle);
  RUN_TEST(test_eviction_reset);
  RUN_TEST(test_single_entry);
  RUN_TEST(test_period1_prefers_over_period2);
  TEST_SUMMARY();
}

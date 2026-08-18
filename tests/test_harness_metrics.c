/* test_harness_metrics.c - Unit tests for cross-session metrics
 * (harness_metrics.h/harness_metrics.c).
 * Covers: T2.1-T2.5 from the implementation plan. */

#include "test_common.h"
#include "../src/harness_metrics.h"
#include "../src/predict.h"
#include <math.h>

/* T2.1 - Metrics save/load roundtrip */
static void test_save_load_roundtrip(void) {
  char *dir = make_test_dir();

  /* Create metrics with known values */
  harness_metrics_t *m = harness_metrics_load(NULL);
  ASSERT_NOT_NULL(m);
  m->sessions_counted = 5;
  m->types[PREDICT_EVICTION].confirmed = 100;
  m->types[PREDICT_EVICTION].refuted = 10;
  m->types[PREDICT_EVICTION].expired = 5;
  m->types[PREDICT_NUDGE].confirmed = 20;
  m->types[PREDICT_NUDGE].refuted = 30;
  m->component_scores[PREDICT_EVICTION] = 0.91;
  m->prev_accuracy[PREDICT_EVICTION] = 0.88;
  m->delta[PREDICT_EVICTION] = 0.03;

  /* Save */
  int rc = harness_metrics_save(m, dir);
  ASSERT_EQ(rc, 0);
  harness_metrics_free(m);

  /* Load and verify */
  harness_metrics_t *loaded = harness_metrics_load(dir);
  ASSERT_NOT_NULL(loaded);
  ASSERT_EQ(loaded->sessions_counted, 5);
  ASSERT_EQ(loaded->types[PREDICT_EVICTION].confirmed, 100);
  ASSERT_EQ(loaded->types[PREDICT_EVICTION].refuted, 10);
  ASSERT_EQ(loaded->types[PREDICT_EVICTION].expired, 5);
  ASSERT_EQ(loaded->types[PREDICT_NUDGE].confirmed, 20);
  ASSERT_EQ(loaded->types[PREDICT_NUDGE].refuted, 30);
  ASSERT(fabs(loaded->component_scores[PREDICT_EVICTION] - 0.91) < 0.01);
  ASSERT(fabs(loaded->prev_accuracy[PREDICT_EVICTION] - 0.88) < 0.01);
  ASSERT(fabs(loaded->delta[PREDICT_EVICTION] - 0.03) < 0.01);

  harness_metrics_free(loaded);
  rm_rf(dir);
  free(dir);
}

/* T2.1 continued - Missing file returns defaults */
static void test_load_missing_file(void) {
  harness_metrics_t *m = harness_metrics_load("/nonexistent/path");
  ASSERT_NOT_NULL(m);
  ASSERT_EQ(m->sessions_counted, 0);
  ASSERT_EQ(m->version, 1);
  for (int t = 0; t < PREDICT_COUNT; t++) {
    ASSERT_EQ(m->types[t].confirmed, 0);
    ASSERT_EQ(m->types[t].refuted, 0);
    ASSERT_EQ(m->types[t].expired, 0);
  }
  harness_metrics_free(m);
}

/* T2.1 continued - NULL nash_dir returns defaults */
static void test_load_null_dir(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);
  ASSERT_NOT_NULL(m);
  ASSERT_EQ(m->sessions_counted, 0);
  harness_metrics_free(m);
}

/* T2.2 - Metrics update from tracker */
static void test_update_from_tracker(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);
  predict_tracker_t *pt = predict_tracker_new();

  /* Create tracker with known outcomes */
  predict_record(pt, PREDICT_EVICTION, 1, "msg[1]", "claim", 0.7);
  predict_record(pt, PREDICT_EVICTION, 2, "msg[2]", "claim", 0.8);
  predict_record(pt, PREDICT_EVICTION, 3, "msg[3]", "claim", 0.6);
  predict_verify(pt, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt, 1, PREDICT_CONFIRMED, 11);
  predict_verify(pt, 2, PREDICT_REFUTED, 12);

  harness_metrics_update(m, pt);

  ASSERT_EQ(m->sessions_counted, 1);
  ASSERT_EQ(m->types[PREDICT_EVICTION].confirmed, 2);
  ASSERT_EQ(m->types[PREDICT_EVICTION].refuted, 1);
  /* accuracy = 2/3 = 0.667 */
  double acc = harness_metrics_accuracy(m, PREDICT_EVICTION);
  ASSERT(fabs(acc - 0.6667) < 0.01);
  /* First session: component score initialized directly */
  ASSERT(fabs(m->component_scores[PREDICT_EVICTION] - 0.6667) < 0.01);

  predict_tracker_free(pt);
  harness_metrics_free(m);
}

/* T2.2 continued - Update twice with EMA blending */
static void test_update_ema_blending(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* Session 1: eviction accuracy = 1.0 (2 confirmed, 0 refuted) */
  predict_tracker_t *pt1 = predict_tracker_new();
  predict_record(pt1, PREDICT_EVICTION, 1, "a", "c", 0.7);
  predict_record(pt1, PREDICT_EVICTION, 2, "b", "c", 0.7);
  predict_verify(pt1, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt1, 1, PREDICT_CONFIRMED, 11);
  harness_metrics_update(m, pt1);
  ASSERT_EQ(m->sessions_counted, 1);
  ASSERT(fabs(m->component_scores[PREDICT_EVICTION] - 1.0) < 0.01);
  predict_tracker_free(pt1);

  /* Session 2: eviction accuracy = 0.5 (1 confirmed, 1 refuted) */
  predict_tracker_t *pt2 = predict_tracker_new();
  predict_record(pt2, PREDICT_EVICTION, 1, "c", "c", 0.7);
  predict_record(pt2, PREDICT_EVICTION, 2, "d", "c", 0.7);
  predict_verify(pt2, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt2, 1, PREDICT_REFUTED, 11);
  harness_metrics_update(m, pt2);
  ASSERT_EQ(m->sessions_counted, 2);
  /* EMA: 0.1 * 0.5 + 0.9 * 1.0 = 0.95 */
  ASSERT(fabs(m->component_scores[PREDICT_EVICTION] - 0.95) < 0.01);
  predict_tracker_free(pt2);

  harness_metrics_free(m);
}

/* T2.3 - Trend tracking */
static void test_trend_tracking(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* Session 1: accuracy = 0.5 */
  predict_tracker_t *pt1 = predict_tracker_new();
  predict_record(pt1, PREDICT_EVICTION, 1, "a", "c", 0.7);
  predict_record(pt1, PREDICT_EVICTION, 2, "b", "c", 0.7);
  predict_verify(pt1, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt1, 1, PREDICT_REFUTED, 11);
  harness_metrics_update(m, pt1);
  predict_tracker_free(pt1);

  /* Session 2: accuracy = 1.0 (improving) */
  predict_tracker_t *pt2 = predict_tracker_new();
  predict_record(pt2, PREDICT_EVICTION, 1, "c", "c", 0.7);
  predict_record(pt2, PREDICT_EVICTION, 2, "d", "c", 0.7);
  predict_verify(pt2, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt2, 1, PREDICT_CONFIRMED, 11);
  harness_metrics_update(m, pt2);
  predict_tracker_free(pt2);

  /* Session 3: accuracy = 0.75 (slight decrease) */
  predict_tracker_t *pt3 = predict_tracker_new();
  for (int i = 0; i < 4; i++)
    predict_record(pt3, PREDICT_EVICTION, i, "e", "c", 0.7);
  predict_verify(pt3, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt3, 1, PREDICT_CONFIRMED, 11);
  predict_verify(pt3, 2, PREDICT_CONFIRMED, 12);
  predict_verify(pt3, 3, PREDICT_REFUTED, 13);
  harness_metrics_update(m, pt3);
  predict_tracker_free(pt3);

  /* Trend should be negative (0.75 - 1.0 = -0.25) */
  ASSERT(m->delta[PREDICT_EVICTION] < 0.0);
  ASSERT(fabs(m->delta[PREDICT_EVICTION] - (-0.25)) < 0.01);

  harness_metrics_free(m);
}

/* T2.4 - Recommendations */
static void test_recommendations_low_accuracy(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* Set eviction accuracy below threshold (0.6) with enough samples */
  m->types[PREDICT_EVICTION].confirmed = 4;
  m->types[PREDICT_EVICTION].refuted = 8;
  /* 12 samples, accuracy = 4/12 = 0.333 */

  harness_recommendation_t recs[PREDICT_COUNT];
  int n = harness_metrics_recommend(m, recs, PREDICT_COUNT);
  ASSERT_GT(n, 0);
  ASSERT_EQ(recs[0].type, PREDICT_EVICTION);
  ASSERT(fabs(recs[0].current_accuracy - 0.333) < 0.01);
  ASSERT_STR_EQ(recs[0].recommendation, "decrease_threshold");

  harness_metrics_free(m);
}

/* T2.4 continued - High accuracy = no recommendation or increase */
static void test_recommendations_high_accuracy(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* All types at 0.95+ with positive trend */
  for (int t = 0; t < PREDICT_COUNT; t++) {
    m->types[t].confirmed = 19;
    m->types[t].refuted = 1;
    m->delta[t] = 0.01;
  }

  harness_recommendation_t recs[PREDICT_COUNT];
  int n = harness_metrics_recommend(m, recs, PREDICT_COUNT);
  /* Should get "increase_threshold" recommendations */
  for (int i = 0; i < n; i++)
    ASSERT_STR_EQ(recs[i].recommendation, "increase_threshold");

  harness_metrics_free(m);
}

/* T2.4 continued - Not enough data = no recommendation */
static void test_recommendations_insufficient_data(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* Only 5 samples (below threshold of 10) */
  m->types[PREDICT_EVICTION].confirmed = 2;
  m->types[PREDICT_EVICTION].refuted = 3;

  harness_recommendation_t recs[PREDICT_COUNT];
  int n = harness_metrics_recommend(m, recs, PREDICT_COUNT);
  /* No recommendations for types with < 10 verifiable predictions */
  for (int i = 0; i < n; i++)
    ASSERT(recs[i].type != PREDICT_EVICTION);

  harness_metrics_free(m);
}

/* T2.5 - Corrupt file handling */
static void test_corrupt_file(void) {
  char *dir = make_test_dir();

  /* Write corrupt JSON */
  char path[4096];
  snprintf(path, sizeof(path), "%s/harness_metrics.json", dir);
  FILE *f = fopen(path, "w");
  fprintf(f, "{this is not valid JSON!!!");
  fclose(f);

  /* Load should return defaults, not crash */
  harness_metrics_t *m = harness_metrics_load(dir);
  ASSERT_NOT_NULL(m);
  ASSERT_EQ(m->sessions_counted, 0);
  ASSERT_EQ(m->version, 1);

  harness_metrics_free(m);
  rm_rf(dir);
  free(dir);
}

/* T2.5 continued - Accuracy query */
static void test_accuracy_query(void) {
  harness_metrics_t *m = harness_metrics_load(NULL);

  /* No data = -1.0 */
  double acc = harness_metrics_accuracy(m, PREDICT_EVICTION);
  ASSERT(acc < 0.0);

  m->types[PREDICT_EVICTION].confirmed = 8;
  m->types[PREDICT_EVICTION].refuted = 2;
  acc = harness_metrics_accuracy(m, PREDICT_EVICTION);
  ASSERT(fabs(acc - 0.8) < 0.01);

  /* Invalid type */
  acc = harness_metrics_accuracy(m, PREDICT_COUNT);
  ASSERT(acc < 0.0);

  harness_metrics_free(m);
}

/* Test NULL safety */
static void test_null_safety(void) {
  harness_metrics_update(NULL, NULL);
  ASSERT_EQ(harness_metrics_save(NULL, NULL), -1);
  harness_metrics_free(NULL);
  ASSERT(harness_metrics_accuracy(NULL, PREDICT_EVICTION) < 0.0);
  ASSERT_EQ(harness_metrics_recommend(NULL, NULL, 0), 0);
  ASSERT(1); /* survived */
}

int main(void) {
  printf("test_harness_metrics\n");
  RUN_TEST(test_save_load_roundtrip);
  RUN_TEST(test_load_missing_file);
  RUN_TEST(test_load_null_dir);
  RUN_TEST(test_update_from_tracker);
  RUN_TEST(test_update_ema_blending);
  RUN_TEST(test_trend_tracking);
  RUN_TEST(test_recommendations_low_accuracy);
  RUN_TEST(test_recommendations_high_accuracy);
  RUN_TEST(test_recommendations_insufficient_data);
  RUN_TEST(test_corrupt_file);
  RUN_TEST(test_accuracy_query);
  RUN_TEST(test_null_safety);
  TEST_SUMMARY();
}

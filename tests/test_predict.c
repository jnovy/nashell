/* test_predict.c - Unit tests for prediction tracking (predict.h/predict.c).
 * Covers: T1.1-T1.6 from the implementation plan. */

#include "test_common.h"
#include "../src/predict.h"

/* T1.1 - Tracker lifecycle */
static void test_tracker_new(void) {
  predict_tracker_t *pt = predict_tracker_new();
  ASSERT_NOT_NULL(pt);
  ASSERT_EQ(pt->head, 0);
  ASSERT_EQ(pt->count, 0);
  ASSERT_EQ(pt->next_id, 0);
  ASSERT_EQ(pt->n_evicted, 0);
  ASSERT_EQ(pt->n_injected, 0);
  for (int t = 0; t < PREDICT_COUNT; t++) {
    ASSERT_EQ(pt->totals[t], 0);
    ASSERT_EQ(pt->confirmed[t], 0);
    ASSERT_EQ(pt->refuted[t], 0);
    ASSERT_EQ(pt->expired[t], 0);
  }
  predict_tracker_free(pt);
}

/* T1.2 - Recording */
static void test_record_basic(void) {
  predict_tracker_t *pt = predict_tracker_new();

  int id0 = predict_record(pt, PREDICT_EVICTION, 1, "msg[5]",
                           "evicting msg 5", 0.8);
  ASSERT_EQ(id0, 0);
  ASSERT_EQ(pt->count, 1);
  ASSERT_EQ(pt->totals[PREDICT_EVICTION], 1);
  ASSERT_EQ(pt->next_id, 1);

  /* Verify stored fields */
  prediction_t *p = &pt->ring[0];
  ASSERT_EQ(p->type, PREDICT_EVICTION);
  ASSERT_EQ(p->step, 1);
  ASSERT_STR_EQ(p->subject, "msg[5]");
  ASSERT_STR_EQ(p->claim, "evicting msg 5");
  ASSERT(p->confidence > 0.79 && p->confidence < 0.81);
  ASSERT_EQ(p->outcome, PREDICT_PENDING);
  ASSERT_EQ(p->verified_step, -1);
  ASSERT_EQ(p->decision_id, 0);

  int id1 = predict_record(pt, PREDICT_NUDGE, 2, "nudge", "test nudge", 0.5);
  ASSERT_EQ(id1, 1);
  ASSERT_EQ(pt->count, 2);
  ASSERT_EQ(pt->totals[PREDICT_NUDGE], 1);
  ASSERT_EQ(pt->next_id, 2);

  predict_tracker_free(pt);
}

/* T1.2 continued - Ring buffer overflow */
static void test_record_overflow(void) {
  predict_tracker_t *pt = predict_tracker_new();

  /* Fill ring to capacity */
  for (int i = 0; i < PREDICT_RING_SIZE; i++) {
    int id = predict_record(pt, PREDICT_EVICTION, i, "test", "claim", 0.5);
    ASSERT_EQ(id, i);
  }
  ASSERT_EQ(pt->count, PREDICT_RING_SIZE);
  ASSERT_EQ(pt->totals[PREDICT_EVICTION], PREDICT_RING_SIZE);

  /* Overflow: oldest entry should be auto-expired */
  int overflow_id = predict_record(pt, PREDICT_NUDGE, 999, "overflow",
                                   "overflow claim", 0.9);
  ASSERT_EQ(overflow_id, PREDICT_RING_SIZE);
  ASSERT_EQ(pt->count, PREDICT_RING_SIZE); /* still at max */
  ASSERT_EQ(pt->expired[PREDICT_EVICTION], 1); /* oldest was auto-expired */
  ASSERT_EQ(pt->totals[PREDICT_NUDGE], 1);
  ASSERT_EQ(pt->totals[PREDICT_EVICTION], PREDICT_RING_SIZE);

  /* Aggregate counters survive overflow */
  ASSERT_GT(pt->totals[PREDICT_EVICTION] + pt->totals[PREDICT_NUDGE],
            PREDICT_RING_SIZE);

  predict_tracker_free(pt);
}

/* T1.3 - Verification by ID */
static void test_verify_by_id(void) {
  predict_tracker_t *pt = predict_tracker_new();

  int id0 = predict_record(pt, PREDICT_EVICTION, 1, "msg[3]", "claim", 0.7);
  int id1 = predict_record(pt, PREDICT_NUDGE, 2, "nudge", "claim2", 0.5);

  /* Verify first as CONFIRMED */
  predict_verify(pt, id0, PREDICT_CONFIRMED, 10);
  ASSERT_EQ(pt->confirmed[PREDICT_EVICTION], 1);
  ASSERT_EQ(pt->ring[0].outcome, PREDICT_CONFIRMED);
  ASSERT_EQ(pt->ring[0].verified_step, 10);

  /* Verify second as REFUTED */
  predict_verify(pt, id1, PREDICT_REFUTED, 15);
  ASSERT_EQ(pt->refuted[PREDICT_NUDGE], 1);
  ASSERT_EQ(pt->ring[1].outcome, PREDICT_REFUTED);
  ASSERT_EQ(pt->ring[1].verified_step, 15);

  /* Re-verification should be no-op */
  predict_verify(pt, id0, PREDICT_REFUTED, 20);
  ASSERT_EQ(pt->confirmed[PREDICT_EVICTION], 1); /* unchanged */
  ASSERT_EQ(pt->refuted[PREDICT_EVICTION], 0);   /* not incremented */

  /* Non-existent ID is no-op */
  predict_verify(pt, 999, PREDICT_CONFIRMED, 30);
  /* No crash, no counter change */

  predict_tracker_free(pt);
}

/* T1.4 - Verification by subject */
static void test_verify_by_subject(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_EVICTION, 1, "msg[5]", "claim A", 0.7);
  predict_record(pt, PREDICT_EVICTION, 2, "msg[10]", "claim B", 0.8);

  /* Verify only the matching subject */
  predict_verify_by_subject(pt, PREDICT_EVICTION, "msg[10]",
                            PREDICT_CONFIRMED, 20);
  ASSERT_EQ(pt->confirmed[PREDICT_EVICTION], 1);
  ASSERT_EQ(pt->ring[0].outcome, PREDICT_PENDING);  /* msg[5] unchanged */
  ASSERT_EQ(pt->ring[1].outcome, PREDICT_CONFIRMED); /* msg[10] matched */

  /* Non-matching subject is no-op */
  predict_verify_by_subject(pt, PREDICT_EVICTION, "msg[99]",
                            PREDICT_REFUTED, 25);
  ASSERT_EQ(pt->refuted[PREDICT_EVICTION], 0); /* no change */

  /* NULL subject is no-op */
  predict_verify_by_subject(pt, PREDICT_EVICTION, NULL,
                            PREDICT_REFUTED, 30);

  predict_tracker_free(pt);
}

/* T1.5 - Finalization */
static void test_finalize(void) {
  predict_tracker_t *pt = predict_tracker_new();

  /* 5 predictions: 3 verified, 2 pending */
  predict_record(pt, PREDICT_EVICTION, 1, "msg[1]", "claim1", 0.7);
  predict_record(pt, PREDICT_NUDGE, 2, "nudge1", "claim2", 0.5);
  predict_record(pt, PREDICT_EVICTION, 3, "msg[2]", "claim3", 0.8);
  predict_record(pt, PREDICT_INJECTION, 4, "key1", "claim4", 0.6);
  predict_record(pt, PREDICT_DEDUP, 5, "step_5", "claim5", 0.95);

  /* Verify 3 of them */
  predict_verify(pt, 0, PREDICT_CONFIRMED, 10);
  predict_verify(pt, 1, PREDICT_REFUTED, 11);
  predict_verify(pt, 2, PREDICT_CONFIRMED, 12);

  /* Finalize: pending INJECTION -> EXPIRED (default), DEDUP -> CONFIRMED (default) */
  predict_finalize(pt);

  ASSERT_EQ(pt->confirmed[PREDICT_EVICTION], 2);
  ASSERT_EQ(pt->refuted[PREDICT_NUDGE], 1);
  /* INJECTION default is EXPIRED */
  ASSERT_EQ(pt->expired[PREDICT_INJECTION], 1);
  /* DEDUP default is CONFIRMED */
  ASSERT_EQ(pt->confirmed[PREDICT_DEDUP], 1);

  /* No pending predictions remain */
  for (int i = 0; i < pt->count; i++) {
    int idx = (pt->head - pt->count + i + PREDICT_RING_SIZE) % PREDICT_RING_SIZE;
    ASSERT(pt->ring[idx].outcome != PREDICT_PENDING);
  }

  predict_tracker_free(pt);
}

/* T1.6 - Ring buffer edge cases: pending entries auto-expired on overwrite */
static void test_ring_auto_expire(void) {
  predict_tracker_t *pt = predict_tracker_new();

  /* Fill ring completely with pending predictions */
  for (int i = 0; i < PREDICT_RING_SIZE; i++)
    predict_record(pt, PREDICT_EVICTION, i, "test", "claim", 0.5);

  /* All should be pending, none expired yet */
  ASSERT_EQ(pt->expired[PREDICT_EVICTION], 0);

  /* Overflow by 3: those 3 oldest entries should be auto-expired */
  for (int i = 0; i < 3; i++)
    predict_record(pt, PREDICT_NUDGE, PREDICT_RING_SIZE + i, "new", "claim", 0.5);

  ASSERT_EQ(pt->expired[PREDICT_EVICTION], 3);
  /* Totals should track lifetime, not just ring contents */
  ASSERT_EQ(pt->totals[PREDICT_EVICTION], PREDICT_RING_SIZE);
  ASSERT_EQ(pt->totals[PREDICT_NUDGE], 3);

  predict_tracker_free(pt);
}

/* Test type/outcome names */
static void test_type_names(void) {
  ASSERT_STR_EQ(predict_type_name(PREDICT_EVICTION), "eviction");
  ASSERT_STR_EQ(predict_type_name(PREDICT_NUDGE), "nudge");
  ASSERT_STR_EQ(predict_type_name(PREDICT_COUNT), "unknown");

  ASSERT_STR_EQ(predict_outcome_name(PREDICT_PENDING), "pending");
  ASSERT_STR_EQ(predict_outcome_name(PREDICT_CONFIRMED), "confirmed");
  ASSERT_STR_EQ(predict_outcome_name(PREDICT_REFUTED), "refuted");
  ASSERT_STR_EQ(predict_outcome_name(PREDICT_EXPIRED), "expired");
}

/* Test evicted CRC storage */
static void test_store_evicted_crc(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_store_evicted_crc(pt, 0xDEADBEEF, 1024, 5);
  ASSERT_EQ(pt->n_evicted, 1);
  ASSERT_EQ(pt->evicted_crcs[0], 0xDEADBEEF);
  ASSERT_EQ(pt->evicted_lens[0], (uint32_t)1024);
  ASSERT_EQ(pt->evicted_steps[0], 5);

  /* Fill to capacity */
  for (int i = 1; i < PREDICT_MAX_EVICTED; i++)
    predict_store_evicted_crc(pt, (uint32_t)i, (uint32_t)(i * 10), i);
  ASSERT_EQ(pt->n_evicted, PREDICT_MAX_EVICTED);

  /* Overflow is safely ignored */
  predict_store_evicted_crc(pt, 0xFFFF, 9999, 999);
  ASSERT_EQ(pt->n_evicted, PREDICT_MAX_EVICTED); /* no change */

  predict_tracker_free(pt);
}

/* Test injected key storage */
static void test_store_injected_key(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_store_injected_key(pt, "lesson:foo");
  ASSERT_EQ(pt->n_injected, 1);
  ASSERT_STR_EQ(pt->injected_keys[0], "lesson:foo");

  /* NULL key is safely ignored */
  predict_store_injected_key(pt, NULL);
  ASSERT_EQ(pt->n_injected, 1);

  predict_tracker_free(pt);
}

/* Test check_triggers: NUDGE confirmation */
static void test_triggers_nudge_confirmed(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_NUDGE, 10, "notes_nudge", "nudge claim", 0.5);

  /* notes() called within 3 steps -> CONFIRMED */
  predict_check_triggers(pt, 12, "notes", NULL, NULL, 0, 0);
  ASSERT_EQ(pt->confirmed[PREDICT_NUDGE], 1);
  ASSERT_EQ(pt->ring[0].outcome, PREDICT_CONFIRMED);
  ASSERT_EQ(pt->ring[0].verified_step, 12);

  predict_tracker_free(pt);
}

/* Test check_triggers: NUDGE refutation (expired after 3 steps) */
static void test_triggers_nudge_refuted(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_NUDGE, 10, "notes_nudge", "nudge claim", 0.5);

  /* 4 steps later without notes() -> REFUTED */
  predict_check_triggers(pt, 14, "file_read", "/foo", "content", 7, 0);
  ASSERT_EQ(pt->refuted[PREDICT_NUDGE], 1);
  ASSERT_EQ(pt->ring[0].outcome, PREDICT_REFUTED);

  predict_tracker_free(pt);
}

/* Test check_triggers: CYCLING confirmation (productive step) */
static void test_triggers_cycling_confirmed(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_CYCLING, 5, "cycle_break", "breaking cycle", 0.6);

  /* Productive step (file_edit) within 3 steps -> CONFIRMED */
  predict_check_triggers(pt, 7, "file_edit", "/foo.c", NULL, 0, 0);
  ASSERT_EQ(pt->confirmed[PREDICT_CYCLING], 1);

  predict_tracker_free(pt);
}

/* Test check_triggers: CYCLING refutation (no productive step) */
static void test_triggers_cycling_refuted(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_CYCLING, 5, "cycle_break", "breaking cycle", 0.6);

  /* Non-productive steps then expire after 3 */
  predict_check_triggers(pt, 6, "file_read", "/foo", "x", 1, 0);
  ASSERT_EQ(pt->ring[0].outcome, PREDICT_PENDING);
  predict_check_triggers(pt, 9, "file_read", "/bar", "y", 1, 0);
  ASSERT_EQ(pt->refuted[PREDICT_CYCLING], 1);

  predict_tracker_free(pt);
}

/* Test check_triggers: ERROR_RECALL */
static void test_triggers_error_recall(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_ERROR_RECALL, 10, "error_recall",
                 "injected memory for error", 0.5);

  /* Error recurs within 5 steps -> REFUTED */
  predict_check_triggers(pt, 13, "shell_exec", NULL, NULL, 0, 1);
  ASSERT_EQ(pt->refuted[PREDICT_ERROR_RECALL], 1);

  predict_tracker_free(pt);
}

/* Test check_triggers: ERROR_RECALL confirmed (no recurrence) */
static void test_triggers_error_recall_confirmed(void) {
  predict_tracker_t *pt = predict_tracker_new();

  predict_record(pt, PREDICT_ERROR_RECALL, 10, "error_recall",
                 "injected memory for error", 0.5);

  /* 6 steps without error -> CONFIRMED */
  predict_check_triggers(pt, 16, "file_read", "/foo", "x", 1, 0);
  ASSERT_EQ(pt->confirmed[PREDICT_ERROR_RECALL], 1);

  predict_tracker_free(pt);
}

/* Test NULL safety */
static void test_null_safety(void) {
  /* All functions should handle NULL gracefully */
  ASSERT_EQ(predict_record(NULL, PREDICT_EVICTION, 1, "x", "y", 0.5), -1);
  predict_verify(NULL, 0, PREDICT_CONFIRMED, 1);
  predict_verify_by_subject(NULL, PREDICT_EVICTION, "x", PREDICT_CONFIRMED, 1);
  predict_finalize(NULL);
  predict_journal_flush(NULL, NULL);
  predict_check_triggers(NULL, 1, "foo", NULL, NULL, 0, 0);
  predict_store_evicted_crc(NULL, 0, 0, 0);
  predict_store_injected_key(NULL, "x");
  predict_tracker_free(NULL);
  /* If we get here without crashing, we pass */
  ASSERT(1);
}

int main(void) {
  printf("test_predict\n");
  RUN_TEST(test_tracker_new);
  RUN_TEST(test_record_basic);
  RUN_TEST(test_record_overflow);
  RUN_TEST(test_verify_by_id);
  RUN_TEST(test_verify_by_subject);
  RUN_TEST(test_finalize);
  RUN_TEST(test_ring_auto_expire);
  RUN_TEST(test_type_names);
  RUN_TEST(test_store_evicted_crc);
  RUN_TEST(test_store_injected_key);
  RUN_TEST(test_triggers_nudge_confirmed);
  RUN_TEST(test_triggers_nudge_refuted);
  RUN_TEST(test_triggers_cycling_confirmed);
  RUN_TEST(test_triggers_cycling_refuted);
  RUN_TEST(test_triggers_error_recall);
  RUN_TEST(test_triggers_error_recall_confirmed);
  RUN_TEST(test_null_safety);
  TEST_SUMMARY();
}

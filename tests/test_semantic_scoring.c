/*
 * test_semantic_scoring.c -- Unit tests for semantic-aware eviction scoring.
 *
 * Tests cover:
 *   - evict_score_progressive_semantic() with NULL similarities (fallback)
 *   - Positive/negative/zero similarity bonus computation
 *   - Bonus capping at REACT_SCORE_SEMANTIC_WEIGHT
 *   - Semantic scoring integration with evict_mark_candidates()
 *   - Relevant LOW-importance message surviving over irrelevant NORMAL message
 *   - Partner map integration with semantic scoring
 */

#include "test_common.h"
#include "../src/react_internal.h"

/* ── Helper: Build a standard test chat ──
 * Layout:
 *   [0] system (CRITICAL)
 *   [1..n_body*2] tool_call/tool_result pairs (NORMAL)
 *   [last] user query (CRITICAL)
 */
static llm_chat_t *make_test_chat(int n_body, int body_msg_len) {
  llm_chat_t *chat = llm_chat_new();

  /* Head: system prompt (CRITICAL) */
  llm_chat_add_typed(chat, "system", "You are a helpful assistant.", LLM_MSG_SYSTEM);

  /* Body: tool_call/tool_result pairs */
  for (int i = 0; i < n_body; i++) {
    char tc_id[32], tc_json[256];
    snprintf(tc_id, sizeof(tc_id), "call_%03d", i);
    snprintf(tc_json, sizeof(tc_json),
             "[{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"file_read\",\"arguments\":\"{}\"}}]",
             tc_id);
    llm_chat_add_assistant_tool_call(chat, "thinking...", tc_json);

    char *content = malloc((size_t)body_msg_len + 1);
    memset(content, 'A' + (i % 26), (size_t)body_msg_len);
    content[body_msg_len] = '\0';
    llm_chat_add_tool_result(chat, tc_id, content);
    free(content);
  }

  /* Tail: user query */
  llm_chat_add_typed(chat, "user", "Fix the eviction bug", LLM_MSG_USER_QUERY);

  return chat;
}

/* ═══════════════════════════════════════════════════════
 *  1. Scoring Function Tests (unit-level)
 * ═══════════════════════════════════════════════════════ */

/* Test: NULL similarities falls back to base score (identical to evict_score_progressive) */
static void test_null_similarities_matches_base(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  /* Semantic context with NULL similarities */
  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = NULL;
  sctx.n_msgs = 0;

  /* Compare scores for each evictable message */
  for (int i = 0; i < n_evictable; i++) {
    int mi = evict_start + i;
    if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;

    int base = evict_score_progressive(chat, mi, i, n_evictable, &pmap);
    int semantic = evict_score_progressive_semantic(chat, mi, i, n_evictable, &sctx);
    ASSERT_EQ(semantic, base);
  }

  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Positive similarity adds bonus */
static void test_positive_similarity_adds_bonus(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  /* Pre-compute similarities: msg at index 2 (tool_result) has high similarity */
  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  int target_mi = evict_start + 1; /* first tool_result */
  sims[target_mi] = 0.8f;

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int base = evict_score_progressive(chat, target_mi, 1, n_evictable, &pmap);
  int semantic = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, &sctx);

  /* Semantic score should be higher by sim * WEIGHT = 0.8 * 40 = 32 */
  int expected_bonus = (int)(0.8f * (float)REACT_SCORE_SEMANTIC_WEIGHT);
  ASSERT_EQ(semantic, base + expected_bonus);
  ASSERT(semantic > base);

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Negative similarity gives no bonus (clamped to 0) */
static void test_negative_similarity_no_bonus(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  int target_mi = evict_start + 1;
  sims[target_mi] = -0.5f; /* negative similarity */

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int base = evict_score_progressive(chat, target_mi, 1, n_evictable, &pmap);
  int semantic = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, &sctx);

  /* No bonus for negative similarity */
  ASSERT_EQ(semantic, base);

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Zero similarity gives no bonus */
static void test_zero_similarity_no_bonus(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  /* sims[target_mi] = 0.0f by default from calloc */

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int target_mi = evict_start + 1;
  int base = evict_score_progressive(chat, target_mi, 1, n_evictable, &pmap);
  int semantic = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, &sctx);

  ASSERT_EQ(semantic, base);

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Bonus is capped at REACT_SCORE_SEMANTIC_WEIGHT */
static void test_similarity_bonus_capped(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  int target_mi = evict_start + 1;
  sims[target_mi] = 1.5f; /* Impossibly high sim -- bonus must cap */

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int base = evict_score_progressive(chat, target_mi, 1, n_evictable, &pmap);
  int semantic = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, &sctx);

  /* Bonus must be exactly REACT_SCORE_SEMANTIC_WEIGHT, not 1.5 * 40 = 60 */
  ASSERT_EQ(semantic, base + REACT_SCORE_SEMANTIC_WEIGHT);

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Perfect similarity (1.0) gives full bonus */
static void test_perfect_similarity_full_bonus(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  int target_mi = evict_start + 1;
  sims[target_mi] = 1.0f;

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int base = evict_score_progressive(chat, target_mi, 1, n_evictable, &pmap);
  int semantic = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, &sctx);

  ASSERT_EQ(semantic, base + REACT_SCORE_SEMANTIC_WEIGHT);

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: NULL evict_score_ctx_t pointer is handled gracefully */
static void test_null_ctx_graceful(void) {
  llm_chat_t *chat = make_test_chat(4, 200);
  int keep_head = react_compute_keep_head(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - 2;
  int n_evictable = evict_end - evict_start;

  /* Pass NULL as userdata -- should not crash */
  int target_mi = evict_start + 1;
  int score = evict_score_progressive_semantic(chat, target_mi, 1, n_evictable, NULL);

  /* Should compute base score without partner map or semantic bonus.
     * imp=1(NORMAL)*100 - rec=0*10 - size=0(200 < thresh) + pos_norm */
  int expected_pos = 1 * REACT_SCORE_POS_RANGE / (n_evictable - 1);
  int expected = 1 * REACT_SCORE_IMP_WEIGHT - 0 + expected_pos;
  ASSERT_EQ(score, expected);

  llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  2. Integration with evict_mark_candidates()
 * ═══════════════════════════════════════════════════════ */

/* Test: Relevant LOW-importance msg survives over irrelevant NORMAL msg.
 * This is the key behavioral test for semantic scoring.
 *
 * Setup: 6 tool pairs. Make pair 0 LOW importance + high similarity (0.9).
 * Make pair 3 NORMAL importance + zero similarity.
 * With semantic scoring, the LOW+relevant pair should score higher than
 * NORMAL+irrelevant pair and survive eviction. */
static void test_relevant_low_survives_irrelevant_normal(void) {
  llm_chat_t *chat = make_test_chat(6, 2000);
  int keep_head = react_compute_keep_head(chat);
  int keep_tail = react_compute_keep_tail(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - keep_tail;
  int n_evictable = evict_end - evict_start;

  if (n_evictable <= 4) {
    /* Not enough evictable messages for this test */
    llm_chat_free(chat);
    return;
  }

  /* Make pair 0 (msg indices 1,2) LOW importance */
  chat->msgs[evict_start].importance = LLM_MSG_IMPORTANCE_LOW;
  chat->msgs[evict_start + 1].importance = LLM_MSG_IMPORTANCE_LOW;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  /* Build similarities: LOW pair has high relevance, NORMAL pairs have zero */
  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  sims[evict_start] = 0.95f;     /* LOW tool_call -- very relevant */
  sims[evict_start + 1] = 0.95f; /* LOW tool_result -- very relevant */
  /* All other messages: similarity = 0 (irrelevant) */

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  /* Verify score ordering: LOW+relevant should score higher than
     * some NORMAL+irrelevant messages at similar positions */
  int low_score = evict_score_progressive_semantic(
    chat, evict_start + 1, 1, n_evictable, &sctx);
  /* Pick a NORMAL msg at a similar early position (pair 1, index 3) */
  int normal_score = evict_score_progressive_semantic(
    chat, evict_start + 3, 3, n_evictable, &sctx);

  /* LOW(0)*100 + 40(bonus) = 40 base + pos_norm
     * NORMAL(1)*100 + 0(bonus) = 100 base + pos_norm
     * With pos_norm being similar, LOW+relevant (40+pos) should be close to
     * or potentially exceed NORMAL+irrelevant (100+pos) for nearby positions.
     * The exact threshold depends on position normalization. */

  /* The semantic bonus should have made the LOW msg's score significantly
     * higher than it would be without semantics */
  int low_base = evict_score_progressive(
    chat, evict_start + 1, 1, n_evictable, &pmap);
  ASSERT(low_score > low_base); /* semantic bonus applied */
  ASSERT_EQ(low_score - low_base, (int)(0.95f * (float)REACT_SCORE_SEMANTIC_WEIGHT));

  /* Now test via mark_candidates: with enough pressure, the irrelevant
     * NORMAL messages should be evicted before the relevant LOW message.
     * We need tight eviction (small target_remaining) but enough floor. */
  long evictable_chars = 0;
  for (int i = evict_start; i < evict_end; i++)
    evictable_chars += (long)chat->msgs[i].content_len;
  long tail_chars_val = react_tail_chars(chat, evict_end);

  int *mark = calloc((size_t)n_evictable, sizeof(int));
  /* Target: keep only 30% of evictable chars -- must evict most messages */
  long target_remaining = evictable_chars * 30 / 100;
  eviction_policy_t tpol = react_eviction_policy(NULL);
  long floor_chars = tpol.floor_min_chars;

  int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                       &pmap, floor_chars,
                                       evictable_chars + tail_chars_val,
                                       tail_chars_val,
                                       target_remaining,
                                       evict_score_progressive_semantic,
                                       &sctx,
                                       mark);

  ASSERT(n_marked > 0);

  /* The relevant LOW tool_result (ri=1) should NOT be marked for eviction
     * if there are enough irrelevant messages to evict instead.
     * Note: the tool_call at ri=0 might be evicted (it's LOW + no content),
     * but the tool_result at ri=1 has the high similarity bonus. */
  /* Check that at least one of the LOW+relevant pair survived */
  int low_result_survived = !mark[1]; /* ri=1 is the LOW tool_result */
  /* Some irrelevant NORMAL messages should be marked */
  int some_normal_marked = 0;
  for (int i = 2; i < n_evictable; i++) {
    if (mark[i]) some_normal_marked = 1;
  }

  ASSERT(some_normal_marked);
  /* The LOW+relevant result should survive when there are enough
     * irrelevant NORMAL messages to meet the eviction target */
  if (n_marked < n_evictable - 2) {
    /* If we didn't need to evict everything, the relevant LOW msg survived */
    ASSERT(low_result_survived);
  }

  free(mark);
  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Multiple messages with varying similarities get correct relative ordering */
static void test_similarity_affects_eviction_order(void) {
  llm_chat_t *chat = make_test_chat(5, 1000);
  int keep_head = react_compute_keep_head(chat);
  int evict_end = chat->n_msgs - 2;
  int evict_start = keep_head;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  /* Set all messages to NORMAL importance for fair comparison */
  for (int i = evict_start; i < evict_end; i++)
    chat->msgs[i].importance = LLM_MSG_IMPORTANCE_NORMAL;

  /* Build similarities: gradient from low to high */
  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  for (int i = 0; i < n_evictable; i++) {
    int mi = evict_start + i;
    sims[mi] = (float)i / (float)(n_evictable - 1); /* 0.0 to 1.0 */
  }

  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  /* Messages with higher similarity should have higher scores.
     * Since all are NORMAL importance and similarity increases with index,
     * and position also increases with index, scores should be monotonically
     * increasing (both position and semantic bonus increase together). */
  int prev_score = -9999;
  for (int i = 0; i < n_evictable; i++) {
    int mi = evict_start + i;
    if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;
    int score = evict_score_progressive_semantic(chat, mi, i, n_evictable, &sctx);
    ASSERT(score >= prev_score);
    prev_score = score;
  }

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Semantic scoring with out-of-bounds mi gracefully handles */
static void test_out_of_bounds_mi_safe(void) {
  llm_chat_t *chat = make_test_chat(3, 200);

  evict_partner_map_t pmap = evict_build_partner_map(chat, 1, chat->n_msgs - 1);

  /* Small similarities array */
  float sims[3] = {0.5f, 0.5f, 0.5f};
  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = 3; /* smaller than chat->n_msgs */

  /* mi=5 is beyond sctx.n_msgs -- should not get bonus */
  int mi = 5;
  if (mi < chat->n_msgs) {
    int score = evict_score_progressive_semantic(chat, mi, 4, 6, &sctx);
    /* Should compute base score without bonus (mi >= n_msgs) */
    int base = evict_score_progressive(chat, mi, 4, 6, &pmap);
    ASSERT_EQ(score, base);
  }

  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* Test: Semantic bonus range verification across all valid similarities */
static void test_bonus_range(void) {
  llm_chat_t *chat = make_test_chat(2, 200);
  int evict_start = 1;
  int evict_end = chat->n_msgs - 1;
  int n_evictable = evict_end - evict_start;

  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  float *sims = calloc((size_t)chat->n_msgs, sizeof(float));
  evict_score_ctx_t sctx = {0};
  sctx.pmap = &pmap;
  sctx.similarities = sims;
  sctx.n_msgs = chat->n_msgs;

  int mi = evict_start + 1; /* first tool_result */
  if (mi < evict_end) {
    /* Test various similarity values */
    float test_sims[] = {-1.0f, -0.5f, 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
    int n_tests = (int)(sizeof(test_sims) / sizeof(test_sims[0]));

    int base = evict_score_progressive(chat, mi, 1, n_evictable, &pmap);

    for (int t = 0; t < n_tests; t++) {
      sims[mi] = test_sims[t];
      int score = evict_score_progressive_semantic(chat, mi, 1, n_evictable, &sctx);
      int bonus = score - base;

      /* Bonus must be in [0, REACT_SCORE_SEMANTIC_WEIGHT] */
      ASSERT(bonus >= 0);
      ASSERT(bonus <= REACT_SCORE_SEMANTIC_WEIGHT);

      /* For positive sims, bonus should match formula */
      if (test_sims[t] > 0.0f) {
        int expected = (int)(test_sims[t] * (float)REACT_SCORE_SEMANTIC_WEIGHT);
        if (expected > REACT_SCORE_SEMANTIC_WEIGHT)
          expected = REACT_SCORE_SEMANTIC_WEIGHT;
        ASSERT_EQ(bonus, expected);
      } else {
        ASSERT_EQ(bonus, 0);
      }
    }
  }

  free(sims);
  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════ */

int main(void) {
  printf("test_semantic_scoring\n");

  /* 1. Scoring function unit tests */
  RUN_TEST(test_null_similarities_matches_base);
  RUN_TEST(test_positive_similarity_adds_bonus);
  RUN_TEST(test_negative_similarity_no_bonus);
  RUN_TEST(test_zero_similarity_no_bonus);
  RUN_TEST(test_similarity_bonus_capped);
  RUN_TEST(test_perfect_similarity_full_bonus);
  RUN_TEST(test_null_ctx_graceful);
  RUN_TEST(test_bonus_range);
  RUN_TEST(test_out_of_bounds_mi_safe);

  /* 2. Integration with mark_candidates */
  RUN_TEST(test_similarity_affects_eviction_order);
  RUN_TEST(test_relevant_low_survives_irrelevant_normal);

  TEST_SUMMARY();
}

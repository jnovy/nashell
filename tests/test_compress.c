/* test_compress.c — Tests for BM25 compression, CRC32 dedup, and
 * the production eviction scoring formula.
 *
 * Covers gaps: C1 (evict_score_progressive), C2 (compress_to_relevant),
 * C3 (compress_crc32). */

#include "test_common.h"
#include "../src/compress.h"
#include "../src/react_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Section 1: compress_crc32 (GAP C3)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_crc32_basic(void) {
  uint32_t h = compress_crc32("hello", 5);
  ASSERT(h != 0);
  ASSERT(h != 0xFFFFFFFF);
}

static void test_crc32_deterministic(void) {
  uint32_t h1 = compress_crc32("test data", 9);
  uint32_t h2 = compress_crc32("test data", 9);
  ASSERT_EQ(h1, h2);
}

static void test_crc32_different_inputs(void) {
  uint32_t h1 = compress_crc32("hello world", 11);
  uint32_t h2 = compress_crc32("hello worlD", 11);
  ASSERT(h1 != h2);
}

static void test_crc32_known_value(void) {
  /* CRC32 of empty string should be 0x00000000 */
  uint32_t h = compress_crc32("", 0);
  ASSERT_EQ(h, 0x00000000);
}

static void test_crc32_single_byte(void) {
  uint32_t h1 = compress_crc32("a", 1);
  uint32_t h2 = compress_crc32("b", 1);
  ASSERT(h1 != 0);
  ASSERT(h2 != 0);
  ASSERT(h1 != h2);
}

static void test_crc32_partial_length(void) {
  /* CRC of first 4 bytes should differ from full 11 bytes */
  uint32_t h_partial = compress_crc32("hello world", 4);
  uint32_t h_full = compress_crc32("hello world", 11);
  ASSERT(h_partial != h_full);
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 2: compress_to_relevant (GAP C2)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_compress_null_input(void) {
  char *result = compress_to_relevant(NULL, "query", 10, 1000);
  ASSERT_NULL(result);
}

static void test_compress_empty_input(void) {
  char *result = compress_to_relevant("", "query", 10, 1000);
  ASSERT_NULL(result);
}

static void test_compress_short_passthrough(void) {
  /* Text shorter than max_chars → returned as-is */
  const char *text = "short text here";
  char *result = compress_to_relevant(text, "query", 10, 1000);
  ASSERT_NOT_NULL(result);
  ASSERT_STR_EQ(result, text);
  free(result);
}

static void test_compress_exact_fit(void) {
  /* Text exactly at max_chars boundary → returned as-is */
  const char *text = "exactly this";
  int len = (int)strlen(text);
  char *result = compress_to_relevant(text, "query", 100, len);
  ASSERT_NOT_NULL(result);
  ASSERT_STR_EQ(result, text);
  free(result);
}

static void test_compress_reduces_size(void) {
  /* Build a long text with many lines */
  char text[4000];
  int pos = 0;
  for (int i = 0; i < 50; i++) {
    pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                    "Line %d: this is filler content for testing compression.\n", i);
  }
  /* Request compression to 500 chars, 10 units */
  char *result = compress_to_relevant(text, "testing", 10, 500);
  ASSERT_NOT_NULL(result);
  ASSERT((int)strlen(result) <= 500);
  ASSERT(strlen(result) < strlen(text)); /* actually compressed */
  free(result);
}

static void test_compress_retains_relevant(void) {
  /* Build text where one line is clearly relevant to query */
  char text[2000];
  int pos = 0;
  for (int i = 0; i < 20; i++) {
    if (i == 10) {
      pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                      "The critical database migration step involves running ALTER TABLE.\n");
    } else {
      pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                      "Line %d: generic filler content that is not related to anything.\n", i);
    }
  }
  char *result = compress_to_relevant(text, "database migration ALTER TABLE", 5, 500);
  ASSERT_NOT_NULL(result);
  /* The relevant line should be retained */
  ASSERT_STR_CONTAINS(result, "database migration");
  free(result);
}

static void test_compress_tag_appended(void) {
  /* When compression truncates, [...compressed] tag should appear */
  char text[4000];
  int pos = 0;
  for (int i = 0; i < 50; i++) {
    pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                    "Line %d: filler content for compression test number %d.\n", i, i);
  }
  char *result = compress_to_relevant(text, "filler", 5, 500);
  ASSERT_NOT_NULL(result);
  ASSERT_STR_CONTAINS(result, "[...compressed]");
  free(result);
}

static void test_compress_max_units_limit(void) {
  /* With max_units=3, should keep at most 3 chunks out of 30.
     * Lines must be > COMPRESS_MERGE_LEN (40 chars) to avoid being
     * merged into a single chunk by split_chunks().
     * max_chars must be smaller than total text so the short-circuit
     * (tlen <= max_chars → strdup) doesn't fire. */
  char text[6000];
  int pos = 0;
  for (int i = 0; i < 30; i++) {
    pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                    "Line %02d: this is a sufficiently long line of content for the unit limit compression test scenario.\n", i);
  }
  int tlen = (int)strlen(text);
  /* max_chars = tlen/2 to force compression path */
  char *result = compress_to_relevant(text, "content", 3, tlen / 2);
  ASSERT_NOT_NULL(result);
  /* Result should be shorter than original */
  ASSERT((int)strlen(result) <= tlen / 2);
  /* Should have compression tag since chunks were dropped */
  ASSERT_STR_CONTAINS(result, "[...compressed]");
  free(result);
}

static void test_compress_null_query(void) {
  /* NULL query should not crash — falls back to position-based scoring */
  char text[2000];
  int pos = 0;
  for (int i = 0; i < 30; i++) {
    pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                    "Line %d: content for null query test.\n", i);
  }
  char *result = compress_to_relevant(text, NULL, 5, 500);
  ASSERT_NOT_NULL(result);
  ASSERT(strlen(result) <= 500);
  free(result);
}

static void test_compress_min_args(void) {
  /* max_units=0 and max_chars=0 are clamped to 1 */
  char text[2000];
  int pos = 0;
  for (int i = 0; i < 30; i++) {
    pos += snprintf(text + pos, sizeof(text) - (size_t)pos,
                    "Line %d: content for min args test.\n", i);
  }
  char *result = compress_to_relevant(text, "content", 0, 0);
  /* Should not crash; result might be very small or NULL fallback */
  if (result) free(result);
  passes++; /* if we got here, no crash */
}

/* ═══════════════════════════════════════════════════════════════════
 * Section 3: evict_score_progressive (GAP C1)
 * ═══════════════════════════════════════════════════════════════════ */

/* Helper: create a simple chat for scoring tests */
static llm_chat_t *make_score_chat(void) {
  llm_chat_t *chat = llm_chat_new();
  /* System message (index 0) */
  llm_chat_add_typed(chat, "system", "You are a helper", LLM_MSG_SYSTEM);
  return chat;
}

static void add_tool_msg(llm_chat_t *chat, const char *content,
                         llm_msg_importance_t imp, llm_recoverability_t rec) {
  llm_chat_add_typed(chat, "tool", content, LLM_MSG_TOOL_RESULT);
  int i = chat->n_msgs - 1;
  chat->msgs[i].importance = imp;
  chat->msgs[i].recoverability = rec;
}

/* C1a: LOW importance scores lower than NORMAL */
static void test_score_importance_ordering(void) {
  llm_chat_t *chat = make_score_chat();

  /* Add LOW message at index 1 */
  add_tool_msg(chat, "low content", LLM_MSG_IMPORTANCE_LOW, LLM_RECOVER_NONE);
  /* Add NORMAL message at index 2 */
  add_tool_msg(chat, "normal content", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  /* Score with mi=1, ri=0 and mi=2, ri=1, n_evictable=2 */
  int score_low = evict_score_progressive(chat, 1, 0, 2, NULL);
  int score_normal = evict_score_progressive(chat, 2, 1, 2, NULL);

  /* LOW * 100 = 0; NORMAL * 100 = 100 */
  /* Position: ri=0 → 0; ri=1 → 19 */
  /* LOW score = 0 + 0 = 0; NORMAL score = 100 + 19 = 119 */
  ASSERT(score_low < score_normal);

  llm_chat_free(chat);
}

/* C1b: Higher recoverability → lower score (evicted sooner) */
static void test_score_recoverability_ordering(void) {
  llm_chat_t *chat = make_score_chat();

  /* NORMAL importance, different recoverabilities */
  add_tool_msg(chat, "not recoverable", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);
  add_tool_msg(chat, "file recoverable", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_FILE);

  int score_none = evict_score_progressive(chat, 1, 0, 2, NULL);
  int score_file = evict_score_progressive(chat, 2, 1, 2, NULL);

  /* RECOVER_NONE → -0*10 = 0 penalty
     * RECOVER_FILE(3) → -3*10 = -30 penalty
     * So score_file should be lower (despite position bonus) */
  /* score_none = 100 - 0 + 0 = 100
     * score_file = 100 - 30 + 19 = 89 */
  ASSERT(score_file < score_none);

  llm_chat_free(chat);
}

/* C1c: Large messages get size bonus (evicted sooner) */
static void test_score_size_bonus(void) {
  llm_chat_t *chat = make_score_chat();

  /* Small message */
  add_tool_msg(chat, "small", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  /* Large message (>200 chars threshold) */
  char big[1200];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  add_tool_msg(chat, big, LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  /* Same position (ri=0) for fair comparison */
  int score_small = evict_score_progressive(chat, 1, 0, 2, NULL);
  int score_big = evict_score_progressive(chat, 2, 0, 2, NULL);

  /* Big message gets size_bonus = 1200/1000 = 1 (rec=0 path) */
  /* Small message: no size bonus (len=5, < 200 threshold) */
  ASSERT(score_big < score_small);

  llm_chat_free(chat);
}

/* C1d: Size bonus capped at 90 */
static void test_score_size_bonus_cap(void) {
  llm_chat_t *chat = make_score_chat();

  /* Very large recoverable message */
  char huge[100001];
  memset(huge, 'y', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  add_tool_msg(chat, huge, LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_FILE);

  int score = evict_score_progressive(chat, 1, 0, 1, NULL);
  /* imp=1*100=100, rec=3*10=30, size_bonus=capped at 90, pos=0
     * score = 100 - 30 - 90 + 0 = -20 */
  ASSERT_EQ(score, -20);

  llm_chat_free(chat);
}

/* C1e: Position normalization */
static void test_score_position_normalization(void) {
  llm_chat_t *chat = make_score_chat();

  /* Add 5 identical messages */
  for (int i = 0; i < 5; i++)
    add_tool_msg(chat, "content", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  /* First position (oldest) should score lowest, last (newest) highest */
  int score_first = evict_score_progressive(chat, 1, 0, 5, NULL);
  int score_last = evict_score_progressive(chat, 5, 4, 5, NULL);

  /* pos_first = 0 * 19 / 4 = 0; pos_last = 4 * 19 / 4 = 19 */
  ASSERT(score_first < score_last);
  ASSERT_EQ(score_last - score_first, 19); /* exactly REACT_SCORE_POS_RANGE */

  llm_chat_free(chat);
}

/* C1f: Single evictable message — pos_norm should be 0 */
static void test_score_single_message(void) {
  llm_chat_t *chat = make_score_chat();

  add_tool_msg(chat, "only one", LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  int score = evict_score_progressive(chat, 1, 0, 1, NULL);
  /* imp=100, rec=0, size=0 (len<200), pos=0 (n_evictable<=1) */
  ASSERT_EQ(score, 100);

  llm_chat_free(chat);
}

/* C1g: Partner size inclusion */
static void test_score_partner_size(void) {
  llm_chat_t *chat = make_score_chat();

  /* Add a tool_call message (has tool_calls_json) */
  llm_chat_add_assistant_tool_call(chat, "thinking",
                                   "[{\"id\":\"tc1\",\"type\":\"function\",\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"foo.c\\\"}\"}}]");
  int tc_idx = chat->n_msgs - 1;
  chat->msgs[tc_idx].importance = LLM_MSG_IMPORTANCE_NORMAL;

  /* Add a large tool result (partner) */
  char big_result[5000];
  memset(big_result, 'z', sizeof(big_result) - 1);
  big_result[sizeof(big_result) - 1] = '\0';
  llm_chat_add_tool_result(chat, "tc1", big_result);
  int tr_idx = chat->n_msgs - 1;
  chat->msgs[tr_idx].importance = LLM_MSG_IMPORTANCE_NORMAL;

  /* Build partner map */
  evict_partner_map_t pmap = evict_build_partner_map(chat, tc_idx, tr_idx + 1);

  /* Score the tool_call message WITH partner map */
  int score_with_partner = evict_score_progressive(chat, tc_idx, 0, 2, &pmap);
  /* Score WITHOUT partner map */
  int score_without_partner = evict_score_progressive(chat, tc_idx, 0, 2, NULL);

  /* With partner, the tool_call gets the big result's size added,
     * so its score should be lower (more likely to evict) */
  ASSERT(score_with_partner < score_without_partner);

  evict_free_partner_map(&pmap);
  llm_chat_free(chat);
}

/* C1h: Recoverable large message gets amplified size bonus */
static void test_score_recoverable_size_amplification(void) {
  llm_chat_t *chat = make_score_chat();

  /* Non-recoverable 1000-char message */
  char msg[1001];
  memset(msg, 'a', 1000);
  msg[1000] = '\0';
  add_tool_msg(chat, msg, LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_NONE);

  /* Same size but FILE recoverable (rec=3) */
  add_tool_msg(chat, msg, LLM_MSG_IMPORTANCE_NORMAL, LLM_RECOVER_FILE);

  int score_none = evict_score_progressive(chat, 1, 0, 2, NULL);
  int score_file = evict_score_progressive(chat, 2, 0, 2, NULL);

  /* rec=0: size_bonus = 1000/1000 = 1
     * rec=3: size_bonus = (1000/500)*3 = 6; also -rec*10 = -30
     * So FILE recoverable gets much lower score */
  ASSERT(score_file < score_none);

  llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
  printf("test_compress:\n");

  /* Section 1: CRC32 */
  RUN_TEST(test_crc32_basic);
  RUN_TEST(test_crc32_deterministic);
  RUN_TEST(test_crc32_different_inputs);
  RUN_TEST(test_crc32_known_value);
  RUN_TEST(test_crc32_single_byte);
  RUN_TEST(test_crc32_partial_length);

  /* Section 2: compress_to_relevant */
  RUN_TEST(test_compress_null_input);
  RUN_TEST(test_compress_empty_input);
  RUN_TEST(test_compress_short_passthrough);
  RUN_TEST(test_compress_exact_fit);
  RUN_TEST(test_compress_reduces_size);
  RUN_TEST(test_compress_retains_relevant);
  RUN_TEST(test_compress_tag_appended);
  RUN_TEST(test_compress_max_units_limit);
  RUN_TEST(test_compress_null_query);
  RUN_TEST(test_compress_min_args);

  /* Section 3: evict_score_progressive */
  RUN_TEST(test_score_importance_ordering);
  RUN_TEST(test_score_recoverability_ordering);
  RUN_TEST(test_score_position_normalization);
  RUN_TEST(test_score_size_bonus);
  RUN_TEST(test_score_size_bonus_cap);
  RUN_TEST(test_score_single_message);
  RUN_TEST(test_score_partner_size);
  RUN_TEST(test_score_recoverable_size_amplification);

  TEST_SUMMARY();
}

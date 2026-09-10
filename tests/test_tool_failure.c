/* test_tool_failure.c - Tests for tool failure status classification
 * and harness-level failure tracking (Paper 6 - Silent Failures).
 *
 * Verifies:
 *   1. tool_status_t enum values
 *   2. tools_make_result_status helper
 *   3. tools_make_error_status helper
 *   4. tool_failure_record ring buffer
 *   5. tool_failure_pattern threshold detection
 *   6. Ring buffer wraparound behavior
 *   7. Mixed tool failure tracking
 *   8. NULL/edge-case handling
 */

#include "test_common.h"
#include "tool_plugin.h"
#include "tools.h"
#include "cJSON.h"

/* Provide globals defined in main.c (not linked into tests) */
int g_path_given = 0;

/* ── test_status_enum_values ── */
static void test_status_enum_values(void) {
  /* Verify enum ordering and that SUCCESS is 0 (backward compat) */
  ASSERT_EQ(TOOL_STATUS_SUCCESS, 0);
  ASSERT_EQ(TOOL_STATUS_ERROR, 1);
  ASSERT_EQ(TOOL_STATUS_TIMEOUT, 2);
  ASSERT_EQ(TOOL_STATUS_AUTH_FAILURE, 3);
  ASSERT_EQ(TOOL_STATUS_RATE_LIMITED, 4);
  ASSERT_EQ(TOOL_STATUS_PARTIAL, 5);
  ASSERT_EQ(TOOL_STATUS_NOT_FOUND, 6);
  ASSERT_EQ(TOOL_STATUS_SERVER_ERROR, 7);
}

/* ── test_make_result_status ── */
static void test_make_result_status(void) {
  cJSON *meta = cJSON_CreateObject();
  cJSON_AddStringToObject(meta, "key", "value");

  tool_result_t r = tools_make_result_status(1, meta, NULL, TOOL_STATUS_TIMEOUT);
  ASSERT_EQ(r.success, 1);
  ASSERT_EQ(r.status, TOOL_STATUS_TIMEOUT);
  ASSERT_NOT_NULL(r.meta);
  ASSERT_NULL(r.store_ref);

  /* Verify meta is passed through */
  cJSON *k = cJSON_GetObjectItem(r.meta, "key");
  ASSERT_NOT_NULL(k);
  ASSERT_STR_EQ(k->valuestring, "value");

  cJSON_Delete(r.meta);
}

/* ── test_make_result_status_default ── */
static void test_make_result_status_default(void) {
  /* Regular tools_make_result should have status=0 (SUCCESS) */
  cJSON *meta = cJSON_CreateObject();
  tool_result_t r = tools_make_result(1, meta, NULL);
  ASSERT_EQ(r.status, TOOL_STATUS_SUCCESS);
  cJSON_Delete(r.meta);
}

/* ── test_make_error_status ── */
static void test_make_error_status(void) {
  tool_result_t r = tools_make_error_status("auth failed", TOOL_STATUS_AUTH_FAILURE);
  ASSERT_EQ(r.success, 0);
  ASSERT_EQ(r.status, TOOL_STATUS_AUTH_FAILURE);
  ASSERT_NOT_NULL(r.meta);

  /* Verify error message in meta */
  cJSON *err = cJSON_GetObjectItem(r.meta, "error");
  ASSERT_NOT_NULL(err);
  ASSERT_STR_EQ(err->valuestring, "auth failed");

  cJSON_Delete(r.meta);
}

/* ── test_make_error_default ── */
static void test_make_error_default(void) {
  /* Regular tools_make_error should have status=0 (SUCCESS default) */
  tool_result_t r = tools_make_error("some error");
  ASSERT_EQ(r.status, TOOL_STATUS_SUCCESS);
  ASSERT_EQ(r.success, 0);
  cJSON_Delete(r.meta);
}

/* ── test_failure_record_basic ── */
static void test_failure_record_basic(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_TIMEOUT);
  ASSERT_EQ(ctx.failure_count, 1);
  ASSERT_STR_EQ(ctx.failure_history[0].tool_name, "web_fetch");
  ASSERT_EQ(ctx.failure_history[0].status, TOOL_STATUS_TIMEOUT);

  tool_failure_record(&ctx, "web_search", TOOL_STATUS_RATE_LIMITED);
  ASSERT_EQ(ctx.failure_count, 2);
  ASSERT_STR_EQ(ctx.failure_history[1].tool_name, "web_search");
  ASSERT_EQ(ctx.failure_history[1].status, TOOL_STATUS_RATE_LIMITED);
}

/* ── test_failure_record_null_safety ── */
static void test_failure_record_null_safety(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* These should not crash */
  tool_failure_record(NULL, "web_fetch", TOOL_STATUS_TIMEOUT);
  tool_failure_record(&ctx, NULL, TOOL_STATUS_TIMEOUT);
  ASSERT_EQ(ctx.failure_count, 0);
}

/* ── test_failure_pattern_below_threshold ── */
static void test_failure_pattern_below_threshold(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* 0 failures - should return NULL */
  char *msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NULL(msg);

  /* 1 failure - below threshold of 3 */
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_TIMEOUT);
  msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NULL(msg);

  /* 2 failures - still below threshold */
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_ERROR);
  msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NULL(msg);
}

/* ── test_failure_pattern_at_threshold ── */
static void test_failure_pattern_at_threshold(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Record 3 failures for same tool */
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_TIMEOUT);
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_ERROR);
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_SERVER_ERROR);

  char *msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NOT_NULL(msg);
  ASSERT_STR_CONTAINS(msg, "web_fetch");
  ASSERT_STR_CONTAINS(msg, "3 of last 3");
  ASSERT_STR_CONTAINS(msg, "TOOL PATTERN");
  free(msg);
}

/* ── test_failure_pattern_mixed_tools ── */
static void test_failure_pattern_mixed_tools(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* 2 web_fetch + 2 shell_exec - neither reaches threshold */
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_TIMEOUT);
  tool_failure_record(&ctx, "shell_exec", TOOL_STATUS_TIMEOUT);
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_ERROR);
  tool_failure_record(&ctx, "shell_exec", TOOL_STATUS_ERROR);

  char *msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NULL(msg);

  msg = tool_failure_pattern(&ctx, "shell_exec");
  ASSERT_NULL(msg);

  /* Add one more web_fetch - now it hits threshold */
  tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_SERVER_ERROR);
  msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NOT_NULL(msg);
  ASSERT_STR_CONTAINS(msg, "web_fetch");
  ASSERT_STR_CONTAINS(msg, "3 of last 5");
  free(msg);

  /* shell_exec is still at 2 */
  msg = tool_failure_pattern(&ctx, "shell_exec");
  ASSERT_NULL(msg);
}

/* ── test_failure_pattern_null_safety ── */
static void test_failure_pattern_null_safety(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  ASSERT_NULL(tool_failure_pattern(NULL, "web_fetch"));
  ASSERT_NULL(tool_failure_pattern(&ctx, NULL));
  ASSERT_NULL(tool_failure_pattern(NULL, NULL));
}

/* ── test_failure_ring_buffer_wraparound ── */
static void test_failure_ring_buffer_wraparound(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Fill beyond TOOL_FAILURE_WINDOW (16) with shell_exec failures */
  for (int i = 0; i < 20; i++) {
    tool_failure_record(&ctx, "shell_exec", TOOL_STATUS_ERROR);
  }

  ASSERT_EQ(ctx.failure_count, 20);

  /* Pattern should still detect failures despite wraparound */
  char *msg = tool_failure_pattern(&ctx, "shell_exec");
  ASSERT_NOT_NULL(msg);
  /* Window is capped at TOOL_FAILURE_WINDOW=16 */
  ASSERT_STR_CONTAINS(msg, "16 of last 16");
  free(msg);
}

/* ── test_failure_ring_buffer_overwrites_old ── */
static void test_failure_ring_buffer_overwrites_old(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Record 3 web_fetch failures */
  for (int i = 0; i < 3; i++) {
    tool_failure_record(&ctx, "web_fetch", TOOL_STATUS_TIMEOUT);
  }

  /* Verify pattern fires */
  char *msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NOT_NULL(msg);
  free(msg);

  /* Now flood with 16 shell_exec failures - pushes web_fetch out of window */
  for (int i = 0; i < 16; i++) {
    tool_failure_record(&ctx, "shell_exec", TOOL_STATUS_ERROR);
  }

  /* web_fetch should no longer be in the window */
  msg = tool_failure_pattern(&ctx, "web_fetch");
  ASSERT_NULL(msg);

  /* shell_exec should now trigger pattern */
  msg = tool_failure_pattern(&ctx, "shell_exec");
  ASSERT_NOT_NULL(msg);
  free(msg);
}

/* ── test_all_status_types_recorded ── */
static void test_all_status_types_recorded(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Record one of each status type */
  tool_failure_record(&ctx, "t1", TOOL_STATUS_ERROR);
  tool_failure_record(&ctx, "t2", TOOL_STATUS_TIMEOUT);
  tool_failure_record(&ctx, "t3", TOOL_STATUS_AUTH_FAILURE);
  tool_failure_record(&ctx, "t4", TOOL_STATUS_RATE_LIMITED);
  tool_failure_record(&ctx, "t5", TOOL_STATUS_PARTIAL);
  tool_failure_record(&ctx, "t6", TOOL_STATUS_NOT_FOUND);
  tool_failure_record(&ctx, "t7", TOOL_STATUS_SERVER_ERROR);

  ASSERT_EQ(ctx.failure_count, 7);

  /* Verify each status is stored correctly */
  ASSERT_EQ(ctx.failure_history[0].status, TOOL_STATUS_ERROR);
  ASSERT_EQ(ctx.failure_history[1].status, TOOL_STATUS_TIMEOUT);
  ASSERT_EQ(ctx.failure_history[2].status, TOOL_STATUS_AUTH_FAILURE);
  ASSERT_EQ(ctx.failure_history[3].status, TOOL_STATUS_RATE_LIMITED);
  ASSERT_EQ(ctx.failure_history[4].status, TOOL_STATUS_PARTIAL);
  ASSERT_EQ(ctx.failure_history[5].status, TOOL_STATUS_NOT_FOUND);
  ASSERT_EQ(ctx.failure_history[6].status, TOOL_STATUS_SERVER_ERROR);
}

/* ── test_tool_name_truncation ── */
static void test_tool_name_truncation(void) {
  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Tool name longer than 32 chars should be truncated, not overflow */
  const char *long_name = "this_is_a_very_long_tool_name_that_exceeds_buffer";
  tool_failure_record(&ctx, long_name, TOOL_STATUS_ERROR);
  ASSERT_EQ(ctx.failure_count, 1);
  /* Should be truncated to 31 chars + null */
  ASSERT_EQ((int)strlen(ctx.failure_history[0].tool_name), 31);
}

int main(void) {
  printf("test_tool_failure:\n");
  RUN_TEST(test_status_enum_values);
  RUN_TEST(test_make_result_status);
  RUN_TEST(test_make_result_status_default);
  RUN_TEST(test_make_error_status);
  RUN_TEST(test_make_error_default);
  RUN_TEST(test_failure_record_basic);
  RUN_TEST(test_failure_record_null_safety);
  RUN_TEST(test_failure_pattern_below_threshold);
  RUN_TEST(test_failure_pattern_at_threshold);
  RUN_TEST(test_failure_pattern_mixed_tools);
  RUN_TEST(test_failure_pattern_null_safety);
  RUN_TEST(test_failure_ring_buffer_wraparound);
  RUN_TEST(test_failure_ring_buffer_overwrites_old);
  RUN_TEST(test_all_status_types_recorded);
  RUN_TEST(test_tool_name_truncation);
  TEST_SUMMARY();
}

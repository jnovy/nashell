/*
 * test_compaction.c — Unit tests for context compaction (eviction) subsystem.
 *
 * Tests cover:
 *   - Inline helpers: react_usage_pct, react_head_chars, react_tail_chars,
 *     react_calc_floor_chars_pol, react_scratchpad_budget, react_format_scratchpad_msg,
 *     evict_adjust_boundaries
 *   - Partner map: build, pair matching, HIGH-importance exclusion
 *   - Mark-sweep: evict_sweep_marked, evict_mark_candidates
 *   - Emergency eviction: react_emergency_evict with target_pct
 *   - Scenario tests for FIX #1, #5, #4
 */

#include "test_common.h"
#include "../src/react_internal.h"

/* ── Helper: Build a chat with a standard head/body/tail layout ──
 * Layout:
 *   [0] system (CRITICAL)
 *   [1..n_body*2] tool_call/tool_result pairs (NORMAL)
 *   [last] user query (CRITICAL)
 *
 * react_compute_keep_head returns 1 (only SYSTEM is CRITICAL).
 * react_compute_keep_tail returns >=2 (last 2 tool pairs).
 * The evictable region is the body pairs not in head or tail.
 */
static llm_chat_t *make_test_chat(int n_body, int body_msg_len) {
    llm_chat_t *chat = llm_chat_new();

    /* Head: system prompt only (CRITICAL) */
    llm_chat_add_typed(chat, "system", "You are a helpful assistant.", LLM_MSG_SYSTEM);

    /* Body: tool_call/tool_result pairs so keep_tail can find pair boundaries */
    for (int i = 0; i < n_body; i++) {
        char tc_id[32], tc_json[256];
        snprintf(tc_id, sizeof(tc_id), "call_%03d", i);
        snprintf(tc_json, sizeof(tc_json),
            "[{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"grep_search\",\"arguments\":\"{}\"}}]",
            tc_id);
        llm_chat_add_assistant_tool_call(chat, "thinking...", tc_json);

        char *content = malloc((size_t)body_msg_len + 1);
        memset(content, 'A' + (i % 26), (size_t)body_msg_len);
        content[body_msg_len] = '\0';
        llm_chat_add_tool_result(chat, tc_id, content);
        /* Set recoverability for some messages */
        if (i % 3 == 0)
            chat->msgs[chat->n_msgs - 1].recoverability = LLM_RECOVER_STORE;
        free(content);
    }

    /* Tail: user query */
    llm_chat_add_typed(chat, "user", "What is the meaning of life?", LLM_MSG_USER_QUERY);

    return chat;
}

/* Helper: Build a chat with tool_call/tool_result pairs */
static llm_chat_t *make_tool_chat(void) {
    llm_chat_t *chat = llm_chat_new();

    /* Head */
    llm_chat_add_typed(chat, "system", "System prompt.", LLM_MSG_SYSTEM);

    /* Tool pair 1: grep_search */
    llm_chat_add_assistant_tool_call(chat,
        "I'll search for the function.",
        "[{\"id\":\"call_001\",\"type\":\"function\",\"function\":{\"name\":\"grep_search\",\"arguments\":\"{\\\"pattern\\\":\\\"foo\\\"}\"}}]");
    llm_chat_add_tool_result(chat, "call_001",
        "Found 5 matches in src/bar.c");

    /* Tool pair 2: file_read */
    llm_chat_add_assistant_tool_call(chat,
        "Let me read that file.",
        "[{\"id\":\"call_002\",\"type\":\"function\",\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"src/bar.c\\\"}\"}}]");
    llm_chat_add_tool_result(chat, "call_002",
        "int bar(void) { return 42; }");

    /* Tail */
    llm_chat_add_typed(chat, "user", "Fix the bug", LLM_MSG_USER_QUERY);

    return chat;
}

/* ═══════════════════════════════════════════════════════
 *  1. Inline Helper Tests
 * ═══════════════════════════════════════════════════════ */

static void test_usage_pct(void) {
    ASSERT_EQ(react_usage_pct(5000, 10000), 50);
    ASSERT_EQ(react_usage_pct(10000, 10000), 100);
    ASSERT_EQ(react_usage_pct(0, 10000), 0);
    ASSERT_EQ(react_usage_pct(7500, 10000), 75);
    /* Zero budget → 0% */
    ASSERT_EQ(react_usage_pct(5000, 0), 0);
}

static void test_head_chars(void) {
    llm_chat_t *chat = make_test_chat(5, 100);
    int keep_head = react_compute_keep_head(chat);
    /* Only SYSTEM is CRITICAL → keep_head = 1 */
    ASSERT_EQ(keep_head, 1);

    long hc = react_head_chars(chat, keep_head);
    /* Head = system prompt only */
    long expected = (long)chat->msgs[0].content_len;
    ASSERT_EQ((int)hc, (int)expected);

    llm_chat_free(chat);
}

static void test_tail_chars(void) {
    llm_chat_t *chat = make_test_chat(5, 100);
    int keep_tail = react_compute_keep_tail(chat);
    ASSERT(keep_tail >= 2);

    int evict_end = chat->n_msgs - keep_tail;
    long tc = react_tail_chars(chat, evict_end);
    /* Tail includes at least the user query */
    ASSERT(tc > 0);

    llm_chat_free(chat);
}

/* FIX #5: Floor calculation must subtract tail_chars */
static void test_calc_floor_chars_with_tail(void) {
    llm_chat_t *chat = make_test_chat(10, 1000);
    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);
    int evict_end = chat->n_msgs - keep_tail;

    long head_chars = react_head_chars(chat, keep_head);
    long tail_chars_val = react_tail_chars(chat, evict_end);
    long context_budget = react_calc_total_chars(chat) * 2; /* 50% usage */

    /* With tail_chars */
    eviction_policy_t pol = react_eviction_policy(NULL);
    long floor_with_tail = react_calc_floor_chars_pol(chat, keep_head, evict_end,
                                                      context_budget,
                                                      head_chars, tail_chars_val, &pol);
    /* Without tail_chars (old behavior, passing 0) */
    long floor_no_tail = react_calc_floor_chars_pol(chat, keep_head, evict_end,
                                                     context_budget,
                                                     head_chars, 0, &pol);

    /* Floor with tail should be <= floor without tail because the
     * evictable region is smaller when tail is excluded */
    ASSERT(floor_with_tail <= floor_no_tail);

    /* Both should be at least the default floor_min_chars */
    eviction_policy_t dpol = react_eviction_policy(NULL);
    ASSERT(floor_with_tail >= dpol.floor_min_chars);

    llm_chat_free(chat);
}

static void test_calc_floor_chars_minimum(void) {
    /* With a tiny budget, floor should clamp to minimum */
    llm_chat_t *chat = make_test_chat(2, 50);
    eviction_policy_t pol3 = react_eviction_policy(NULL);
    long floor = react_calc_floor_chars_pol(chat, 2, chat->n_msgs,
                                             1000, 200, 0, &pol3);
    { eviction_policy_t dpol2 = react_eviction_policy(NULL);
    ASSERT_EQ((int)floor, (int)dpol2.floor_min_chars); }
    llm_chat_free(chat);
}

static void test_scratchpad_budget(void) {
    /* Normal case: budget is min(abs_cap, rel_cap) */
    size_t budget = react_scratchpad_budget(100000, 50000, 2048);
    /* abs_cap = 100000 * 15 / 100 = 15000 */
    /* remaining = 50000, rel_cap = 50000 * 40 / 100 = 20000 */
    /* min(15000, 20000) = 15000 */
    ASSERT_EQ((int)budget, 15000);

    /* High usage: remaining is small */
    budget = react_scratchpad_budget(100000, 95000, 2048);
    /* abs_cap = 15000, remaining = 5000, rel_cap = 2000 */
    /* min(15000, 2000) = 2000, but min_budget = 2048, so 2048 */
    ASSERT_EQ((int)budget, 2048);

    /* No budget → fallback */
    budget = react_scratchpad_budget(0, 5000, 2048);
    { eviction_policy_t spol = react_eviction_policy(NULL);
    ASSERT_EQ((int)budget, (int)spol.sp_fallback); }
}

static void test_format_scratchpad_msg(void) {
    char *msg = react_format_scratchpad_msg("hello world");
    ASSERT_NOT_NULL(msg);
    ASSERT_STR_EQ(msg, "[SCRATCHPAD]\nhello world");
    /* Verify prefix is exactly REACT_SP_PREFIX */
    ASSERT(strncmp(msg, REACT_SP_PREFIX, REACT_SP_PREFIX_LEN) == 0);
    free(msg);

    /* NULL/empty input → NULL */
    ASSERT_NULL(react_format_scratchpad_msg(NULL));
    ASSERT_NULL(react_format_scratchpad_msg(""));
}

static void test_chat_usage_pct(void) {
    llm_chat_t *chat = make_test_chat(5, 100);
    long total = react_calc_total_chars(chat);
    long budget = total * 2; /* 50% usage */
    ASSERT_EQ(react_chat_usage_pct(chat, budget), 50);
    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  2. Boundary Adjustment Tests
 * ═══════════════════════════════════════════════════════ */

static void test_adjust_boundaries_no_tools(void) {
    llm_chat_t *chat = make_test_chat(5, 100);
    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;
    int orig_start = evict_start, orig_end = evict_end;

    evict_adjust_boundaries(chat, &evict_start, &evict_end);

    /* No tool messages → boundaries unchanged */
    ASSERT_EQ(evict_start, orig_start);
    ASSERT_EQ(evict_end, orig_end);

    llm_chat_free(chat);
}

static void test_adjust_boundaries_with_tools(void) {
    llm_chat_t *chat = make_tool_chat();
    int evict_start = 1; /* after system */
    int evict_end = chat->n_msgs - 1; /* before user query */

    evict_adjust_boundaries(chat, &evict_start, &evict_end);

    /* Boundaries should not split tool_call/tool_result pairs */
    /* evict_start should not point to an orphaned tool_result */
    if (evict_start < chat->n_msgs && chat->msgs[evict_start].tool_call_id) {
        /* If it's a tool_result, its partner should also be in range */
        ASSERT(evict_start > 0);
    }
    /* evict_end should not leave a tool_call at the boundary without its result */
    if (evict_end > 0 && evict_end <= chat->n_msgs) {
        ASSERT(!chat->msgs[evict_end - 1].tool_calls_json ||
               evict_end < chat->n_msgs);
    }

    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  3. Partner Map Tests
 * ═══════════════════════════════════════════════════════ */

static void test_partner_map_basic(void) {
    llm_chat_t *chat = make_tool_chat();
    /* Range: all messages */
    evict_partner_map_t pmap = evict_build_partner_map(chat, 0, chat->n_msgs);

    ASSERT_NOT_NULL(pmap.partner);
    ASSERT_EQ(pmap.n_msgs, chat->n_msgs);

    /* Tool call at idx 1 should partner with tool result at idx 2 */
    ASSERT_EQ(pmap.partner[1], 2);
    ASSERT_EQ(pmap.partner[2], 1);

    /* Tool call at idx 3 should partner with tool result at idx 4 */
    ASSERT_EQ(pmap.partner[3], 4);
    ASSERT_EQ(pmap.partner[4], 3);

    /* System prompt (0) and user query (5) have no partner */
    ASSERT_EQ(pmap.partner[0], -1);
    ASSERT_EQ(pmap.partner[5], -1);

    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

static void test_partner_map_high_importance(void) {
    llm_chat_t *chat = make_tool_chat();

    /* Mark tool_result at idx 2 as HIGH importance */
    chat->msgs[2].importance = LLM_MSG_IMPORTANCE_HIGH;

    evict_partner_map_t pmap = evict_build_partner_map(chat, 0, chat->n_msgs);

    /* HIGH importance partner should NOT be matched by react_find_tool_partner
     * (it returns -1 for importance >= HIGH) */
    ASSERT_EQ(pmap.partner[1], -1);
    ASSERT_EQ(pmap.partner[2], -1);

    /* Second pair should still work */
    ASSERT_EQ(pmap.partner[3], 4);
    ASSERT_EQ(pmap.partner[4], 3);

    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

static void test_partner_map_partial_range(void) {
    llm_chat_t *chat = make_tool_chat();
    /* Build map only for messages [2, 5) — should miss pair 1 (idx 1-2) */
    evict_partner_map_t pmap = evict_build_partner_map(chat, 2, 5);

    /* Partner map still has entries for all messages but only
     * tool_calls_json messages in [2,5) are scanned. idx 3 is a tool_call. */
    ASSERT_EQ(pmap.partner[3], 4);
    ASSERT_EQ(pmap.partner[4], 3);
    /* idx 1 (tool_call) is outside range, should not be paired */
    ASSERT_EQ(pmap.partner[1], -1);

    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  4. Sweep Tests
 * ═══════════════════════════════════════════════════════ */

static void test_sweep_basic(void) {
    llm_chat_t *chat = make_test_chat(6, 100);
    int n_before = chat->n_msgs;
    long chars_before = react_calc_total_chars(chat);

    int keep_head = 2;
    int n_evictable = n_before - keep_head - 1; /* exclude tail */

    /* Mark messages 0 and 2 (relative to evict_start) for eviction */
    int *mark = calloc((size_t)n_evictable, sizeof(int));
    mark[0] = 1; /* absolute idx: keep_head + 0 = 2 */
    mark[2] = 1; /* absolute idx: keep_head + 2 = 4 */

    long removed_chars = (long)chat->msgs[2].content_len +
                         (long)chat->msgs[4].content_len;

    int removed = evict_sweep_marked(chat, keep_head, mark, n_evictable);
    ASSERT_EQ(removed, 2);
    ASSERT_EQ(chat->n_msgs, n_before - 2);

    /* total_chars should be updated */
    ASSERT_EQ((int)react_calc_total_chars(chat), (int)(chars_before - removed_chars));

    free(mark);
    llm_chat_free(chat);
}

static void test_sweep_none_marked(void) {
    llm_chat_t *chat = make_test_chat(4, 100);
    int n_before = chat->n_msgs;
    long chars_before = react_calc_total_chars(chat);

    int n_evictable = 4;
    int *mark = calloc((size_t)n_evictable, sizeof(int));

    int removed = evict_sweep_marked(chat, 2, mark, n_evictable);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(chat->n_msgs, n_before);
    ASSERT_EQ((int)react_calc_total_chars(chat), (int)chars_before);

    free(mark);
    llm_chat_free(chat);
}

static void test_sweep_all_marked(void) {
    llm_chat_t *chat = make_test_chat(4, 100);
    int keep_head = 2;
    int keep_tail = 1;
    int n_evictable = chat->n_msgs - keep_head - keep_tail;
    int n_before = chat->n_msgs;

    int *mark = calloc((size_t)n_evictable, sizeof(int));
    for (int i = 0; i < n_evictable; i++)
        mark[i] = 1;

    int removed = evict_sweep_marked(chat, keep_head, mark, n_evictable);
    ASSERT_EQ(removed, n_evictable);
    ASSERT_EQ(chat->n_msgs, n_before - n_evictable);

    /* Head and tail should survive */
    ASSERT_EQ(chat->n_msgs, keep_head + keep_tail);

    free(mark);
    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  5. Mark-Candidates Tests
 * ═══════════════════════════════════════════════════════ */

/* Simple scoring: just use position (older = lower score = evicted first) */
static int score_by_position(const llm_chat_t *chat, int mi, int ri,
                             int n_evictable, void *userdata) {
    (void)chat; (void)mi; (void)n_evictable; (void)userdata;
    return ri; /* older (lower ri) = lower score = evicted first */
}

static void test_mark_candidates_basic(void) {
    /* Use 12 pairs of 2000-char results → evictable >> floor (4000) */
    llm_chat_t *chat = make_test_chat(12, 2000);
    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;
    int n_evictable = evict_end - evict_start;

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    long evictable_chars = 0;
    for (int i = evict_start; i < evict_end; i++)
        evictable_chars += (long)chat->msgs[i].content_len;

    eviction_policy_t tpol = react_eviction_policy(NULL);
    long floor_chars = tpol.floor_min_chars;
    long target_remaining = evictable_chars / 2; /* try to evict half */

    int *mark = calloc((size_t)n_evictable, sizeof(int));
    int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                          &pmap, floor_chars,
                                          evictable_chars, 0 /* tail_chars */,
                                          target_remaining,
                                          score_by_position, NULL,
                                          mark);

    /* Should have marked some messages (evictable >> floor) */
    ASSERT(n_marked > 0);
    /* Should not have marked more than we need */
    ASSERT(n_marked <= n_evictable);

    /* Verify marks cluster at low indices (oldest evicted first) */
    int first_unmarked = -1;
    for (int i = 0; i < n_evictable; i++) {
        if (!mark[i]) { first_unmarked = i; break; }
    }
    /* All marks should be at indices below first_unmarked
     * (position scoring = strictly ordered, pairs may add one extra) */
    if (first_unmarked > 0) {
        for (int i = 0; i < first_unmarked; i++)
            ASSERT_EQ(mark[i], 1);
    }

    free(mark);
    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

static void test_mark_candidates_respects_floor(void) {
    /* Create chat where floor prevents evicting everything */
    llm_chat_t *chat = make_test_chat(4, 2000);
    int evict_start = 2;
    int evict_end = chat->n_msgs - 1;
    int n_evictable = evict_end - evict_start;

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    long evictable_chars = 0;
    for (int i = evict_start; i < evict_end; i++)
        evictable_chars += (long)chat->msgs[i].content_len;

    /* Set floor very high — almost all content must be retained */
    long floor_chars = evictable_chars - 100;
    long target_remaining = 0; /* want to evict everything */

    int *mark = calloc((size_t)n_evictable, sizeof(int));
    int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                          &pmap, floor_chars,
                                          evictable_chars, 0 /* tail_chars */,
                                          target_remaining,
                                          score_by_position, NULL,
                                          mark);

    /* FLAW 5 FIX: Floor prevents evicting large messages, but small messages
     * (the ~10-char assistant tool_call stubs) can be evicted individually
     * without their 2000-char partners.  Previously both were skipped. */
    ASSERT(n_marked > 0);  /* small messages fit within the 100-char budget */
    /* But the 2000-char tool_results should NOT be marked (too large) */
    for (int i = 0; i < n_evictable; i++) {
        if (mark[i]) {
            int mi = evict_start + i;
            ASSERT(chat->msgs[mi].content_len < 100);  /* only small msgs */
        }
    }

    free(mark);
    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

static void test_mark_candidates_protects_high_importance(void) {
    llm_chat_t *chat = make_test_chat(4, 500);
    int evict_start = 2;
    int evict_end = chat->n_msgs - 1;
    int n_evictable = evict_end - evict_start;

    /* Mark first body message as HIGH importance */
    chat->msgs[2].importance = LLM_MSG_IMPORTANCE_HIGH;

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    long evictable_chars = 0;
    for (int i = evict_start; i < evict_end; i++)
        evictable_chars += (long)chat->msgs[i].content_len;

    int *mark = calloc((size_t)n_evictable, sizeof(int));
    int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                          &pmap, react_eviction_policy(NULL).floor_min_chars,
                                          evictable_chars, 0 /* tail_chars */,
                                          0,
                                          score_by_position, NULL,
                                          mark);

    /* The HIGH importance message (relative idx 0) should NOT be marked */
    ASSERT_EQ(mark[0], 0);

    /* But other messages should be marked if floor allows */
    if (n_marked > 0) {
        int any_non_high_marked = 0;
        for (int i = 1; i < n_evictable; i++) {
            if (mark[i]) any_non_high_marked = 1;
        }
        ASSERT(any_non_high_marked);
    }

    free(mark);
    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

static void test_mark_candidates_pair_eviction(void) {
    llm_chat_t *chat = make_tool_chat();
    int evict_start = 1; /* after system */
    int evict_end = chat->n_msgs - 1; /* before user query */
    int n_evictable = evict_end - evict_start;

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    long evictable_chars = 0;
    for (int i = evict_start; i < evict_end; i++)
        evictable_chars += (long)chat->msgs[i].content_len;

    int *mark = calloc((size_t)n_evictable, sizeof(int));
    int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                          &pmap, 0, /* no floor */
                                          evictable_chars, 0 /* tail_chars */,
                                          0,
                                          score_by_position, NULL,
                                          mark);

    /* If a tool_call is marked, its partner tool_result should also be marked */
    for (int i = 0; i < n_evictable; i++) {
        if (!mark[i]) continue;
        int mi = evict_start + i;
        if (mi < pmap.n_msgs && pmap.partner[mi] >= 0) {
            int partner_ri = pmap.partner[mi] - evict_start;
            if (partner_ri >= 0 && partner_ri < n_evictable) {
                ASSERT_EQ(mark[partner_ri], 1);
            }
        }
    }

    free(mark);
    evict_free_partner_map(&pmap);
    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  6. Emergency Eviction Tests
 * ═══════════════════════════════════════════════════════ */

static void test_emergency_evict_basic(void) {
    /* Build a large chat that clearly exceeds a small budget */
    llm_chat_t *chat = make_test_chat(20, 1000);
    long total = react_calc_total_chars(chat);
    /* Set budget so we're at ~200% capacity → emergency eviction should fire */
    long context_budget = total / 2;
    int n_before = chat->n_msgs;

    int removed = react_emergency_evict(chat, context_budget, 60, NULL);
    ASSERT(removed > 0);
    ASSERT(chat->n_msgs < n_before);

    /* After eviction, should be closer to 60% of budget */
    int usage_after = react_chat_usage_pct(chat, context_budget);
    /* May not hit exactly 60% due to floor, but should be reduced significantly */
    ASSERT(usage_after < 200);

    llm_chat_free(chat);
}

static void test_emergency_evict_respects_target(void) {
    llm_chat_t *chat = make_test_chat(15, 800);
    long total = react_calc_total_chars(chat);
    long context_budget = total; /* exactly at 100% */

    /* Evict to 50% */
    int removed = react_emergency_evict(chat, context_budget, 50, NULL);
    ASSERT(removed > 0);

    /* Usage should be around or below 50% (floor may prevent exact) */
    int usage = react_chat_usage_pct(chat, context_budget);
    /* Should be significantly reduced from 100% */
    ASSERT(usage < 80);

    llm_chat_free(chat);
}

/* FIX #4: target_pct=0 should use REACT_EMERGENCY_TARGET_PCT (80%) */
static void test_emergency_evict_default_target(void) {
    llm_chat_t *chat = make_test_chat(20, 1000);
    long total = react_calc_total_chars(chat);
    long context_budget = total / 2; /* 200% usage */

    int removed = react_emergency_evict(chat, context_budget, 0, NULL);
    ASSERT(removed > 0);

    /* Should target 80% (REACT_EMERGENCY_TARGET_PCT) */
    int usage = react_chat_usage_pct(chat, context_budget);
    /* Floor may prevent reaching exactly 80%, but should be well below 200% */
    ASSERT(usage < 150);

    llm_chat_free(chat);
}

static void test_emergency_evict_already_below_target(void) {
    llm_chat_t *chat = make_test_chat(3, 100);
    long total = react_calc_total_chars(chat);
    long context_budget = total * 3; /* 33% usage — well below any target */

    int removed = react_emergency_evict(chat, context_budget, 80, NULL);
    ASSERT_EQ(removed, 0); /* Nothing to evict */

    llm_chat_free(chat);
}

static void test_emergency_evict_preserves_critical(void) {
    llm_chat_t *chat = make_test_chat(10, 500);
    long total = react_calc_total_chars(chat);
    long context_budget = total / 2; /* 200% usage */

    /* Record CRITICAL messages */
    int critical_count = 0;
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_CRITICAL)
            critical_count++;
    }

    react_emergency_evict(chat, context_budget, 60, NULL);

    /* Count CRITICAL messages after eviction — should be same */
    int critical_after = 0;
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_CRITICAL)
            critical_after++;
    }
    ASSERT_EQ(critical_after, critical_count);

    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  7. Scenario Tests (FIX validation)
 * ═══════════════════════════════════════════════════════ */

/* FIX #5 scenario: Floor with large tail should be lower than without */
static void test_fix5_floor_with_large_tail(void) {
    llm_chat_t *chat = llm_chat_new();

    /* Head: 1 message */
    llm_chat_add_typed(chat, "system", "System.", LLM_MSG_SYSTEM);

    /* Body: 5 normal messages, 1000 chars each */
    for (int i = 0; i < 5; i++) {
        char body[1001];
        memset(body, 'X', 1000);
        body[1000] = '\0';
        llm_chat_add_typed(chat, "assistant", body, LLM_MSG_GENERIC);
    }

    /* Large tail: 3 messages, 2000 chars each */
    for (int i = 0; i < 3; i++) {
        char tail[2001];
        memset(tail, 'T', 2000);
        tail[2000] = '\0';
        llm_chat_add_typed(chat, "user", tail, LLM_MSG_USER_QUERY);
    }

    int evict_start = 1;
    int evict_end = chat->n_msgs - 3;
    long head_chars = react_head_chars(chat, evict_start);
    long tail_chars_val = react_tail_chars(chat, evict_end);
    long budget = 50000;

    eviction_policy_t pol5 = react_eviction_policy(NULL);
    /* Floor WITH tail subtracted */
    long floor_correct = react_calc_floor_chars_pol(chat, evict_start, evict_end,
                                                     budget, head_chars, tail_chars_val, &pol5);

    /* Floor WITHOUT tail (old bug) */
    long floor_buggy = react_calc_floor_chars_pol(chat, evict_start, evict_end,
                                                   budget, head_chars, 0, &pol5);

    /* The correct floor should be smaller (base is smaller when tail excluded) */
    ASSERT(floor_correct < floor_buggy);

    /* Verify: base_correct = budget - head - tail, base_buggy = budget - head */
    /* floor = base * 20 / 100 */
    long base_correct = budget - head_chars - tail_chars_val;
    long base_buggy = budget - head_chars;
    { eviction_policy_t fpol = react_eviction_policy(NULL);
    ASSERT_EQ((int)floor_correct, (int)(base_correct * fpol.floor_pct / 100));
    ASSERT_EQ((int)floor_buggy, (int)(base_buggy * fpol.floor_pct / 100)); }

    llm_chat_free(chat);
}

/* FIX #11: Scratchpad message prefix constant */
static void test_fix11_scratchpad_prefix(void) {
    /* Verify REACT_SP_PREFIX_LEN matches actual string length */
    ASSERT_EQ(REACT_SP_PREFIX_LEN, (int)strlen(REACT_SP_PREFIX));
    ASSERT_STR_EQ(REACT_SP_PREFIX, "[SCRATCHPAD]\n");
}

/* Test inject_scratchpad_msg */
static void test_inject_scratchpad_msg(void) {
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add_typed(chat, "system", "System prompt", LLM_MSG_SYSTEM);
    llm_chat_add_typed(chat, "user", "Hello", LLM_MSG_USER_QUERY);

    /* Inject at position 1 (between system and user) */
    long injected = react_inject_scratchpad_msg(chat, 1, "## Notes\nImportant thing");
    ASSERT(injected > 0);
    ASSERT_EQ(chat->n_msgs, 3);
    ASSERT_STR_CONTAINS(chat->msgs[1].content, "[SCRATCHPAD]");
    ASSERT_STR_CONTAINS(chat->msgs[1].content, "Important thing");
    ASSERT_EQ((int)chat->msgs[1].msg_type, (int)LLM_MSG_SCRATCHPAD);

    /* Empty/NULL scratchpad → no injection */
    long nope = react_inject_scratchpad_msg(chat, 1, NULL);
    ASSERT_EQ((int)nope, 0);
    ASSERT_EQ(chat->n_msgs, 3); /* unchanged */

    nope = react_inject_scratchpad_msg(chat, 1, "");
    ASSERT_EQ((int)nope, 0);
    ASSERT_EQ(chat->n_msgs, 3);

    llm_chat_free(chat);
}

/* Test total_chars consistency through operations */
static void test_total_chars_consistency(void) {
    llm_chat_t *chat = llm_chat_new();

    llm_chat_add_typed(chat, "system", "Hello", LLM_MSG_SYSTEM);
    ASSERT_EQ((int)chat->total_chars, 5);

    llm_chat_add_typed(chat, "user", "World!", LLM_MSG_USER_QUERY);
    ASSERT_EQ((int)chat->total_chars, 11);

    /* Remove by type */
    llm_chat_remove_by_type(chat, LLM_MSG_USER_QUERY);
    ASSERT_EQ((int)chat->total_chars, 5);
    ASSERT_EQ(chat->n_msgs, 1);

    /* Insert typed */
    llm_chat_insert_typed(chat, 1, "assistant", "Response here", LLM_MSG_GENERIC);
    ASSERT_EQ((int)chat->total_chars, 18); /* 5 + 13 */

    /* Replace content */
    char *newc = strdup("Short");
    llm_chat_replace_content(chat, 1, newc);
    ASSERT_EQ((int)chat->total_chars, 10); /* 5 + 5 */

    llm_chat_free(chat);
}

/* Test tool pair sweep preserves unpaired messages */
static void test_sweep_preserves_unpaired_tools(void) {
    llm_chat_t *chat = make_tool_chat();
    int n_before = chat->n_msgs;

    /* Mark only the first tool_call (idx 1, relative 0) but NOT its result */
    int evict_start = 1;
    int n_evictable = n_before - evict_start - 1;
    int *mark = calloc((size_t)n_evictable, sizeof(int));
    mark[0] = 1; /* only tool_call at idx 1 */

    int removed = evict_sweep_marked(chat, evict_start, mark, n_evictable);
    ASSERT_EQ(removed, 1);
    ASSERT_EQ(chat->n_msgs, n_before - 1);

    free(mark);
    llm_chat_free(chat);
}

/* Comprehensive scenario: evict from a realistic conversation */
static void test_realistic_eviction_scenario(void) {
    llm_chat_t *chat = llm_chat_new();

    /* System prompt */
    llm_chat_add_typed(chat, "system",
        "You are an autonomous coding agent. Solve tasks step by step.",
        LLM_MSG_SYSTEM);

    /* Memory index */
    llm_chat_add_typed(chat, "user",
        "[MEMORY INDEX] Memory: 500 entries", LLM_MSG_MEMORY_INDEX);

    /* Scratchpad */
    llm_chat_add_typed(chat, "user",
        "[SCRATCHPAD]\n## plan\n1. Read files\n2. Fix bug", LLM_MSG_SCRATCHPAD);

    /* Several tool call/result pairs with large results */
    for (int step = 0; step < 15; step++) {
        char tc_json[256], tc_id[32];
        snprintf(tc_id, sizeof(tc_id), "call_%03d", step);
        char thought[128];
        snprintf(thought, sizeof(thought), "Step %d: investigating", step);
        snprintf(tc_json, sizeof(tc_json),
            "[{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"grep_search\",\"arguments\":\"{}\"}}]",
            tc_id);
        llm_chat_add_assistant_tool_call(chat, thought, tc_json);

        /* Large tool results (~2000 chars each) */
        char result[2001];
        memset(result, 'R', 2000);
        result[2000] = '\0';
        llm_chat_add_tool_result(chat, tc_id, result);
    }

    /* User query at the end */
    llm_chat_add_typed(chat, "user", "Fix the buffer overflow bug", LLM_MSG_USER_QUERY);

    int n_before = chat->n_msgs;
    long total_before = react_calc_total_chars(chat);

    /* Set budget to trigger eviction (50% of current usage) */
    long budget = total_before / 2;

    /* Emergency evict to 60% of budget */
    int removed = react_emergency_evict(chat, budget, 60, NULL);
    ASSERT(removed > 0);
    ASSERT(chat->n_msgs < n_before);

    /* Verify structural integrity */
    /* System prompt should survive (CRITICAL) */
    ASSERT_EQ((int)chat->msgs[0].msg_type, (int)LLM_MSG_SYSTEM);
    /* User query should survive (CRITICAL) */
    int found_query = 0;
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].msg_type == LLM_MSG_USER_QUERY) found_query = 1;
    }
    ASSERT(found_query);

    /* total_chars should be consistent with actual content */
    long manual_sum = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        manual_sum += (long)chat->msgs[i].content_len;
    ASSERT_EQ((int)react_calc_total_chars(chat), (int)manual_sum);

    llm_chat_free(chat);
}

/* ═══════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════ */

int main(void) {
    printf("test_compaction:\n");

    /* 1. Inline helpers */
    RUN_TEST(test_usage_pct);
    RUN_TEST(test_head_chars);
    RUN_TEST(test_tail_chars);
    RUN_TEST(test_calc_floor_chars_with_tail);
    RUN_TEST(test_calc_floor_chars_minimum);
    RUN_TEST(test_scratchpad_budget);
    RUN_TEST(test_format_scratchpad_msg);
    RUN_TEST(test_chat_usage_pct);

    /* 2. Boundary adjustment */
    RUN_TEST(test_adjust_boundaries_no_tools);
    RUN_TEST(test_adjust_boundaries_with_tools);

    /* 3. Partner map */
    RUN_TEST(test_partner_map_basic);
    RUN_TEST(test_partner_map_high_importance);
    RUN_TEST(test_partner_map_partial_range);

    /* 4. Sweep */
    RUN_TEST(test_sweep_basic);
    RUN_TEST(test_sweep_none_marked);
    RUN_TEST(test_sweep_all_marked);

    /* 5. Mark-candidates */
    RUN_TEST(test_mark_candidates_basic);
    RUN_TEST(test_mark_candidates_respects_floor);
    RUN_TEST(test_mark_candidates_protects_high_importance);
    RUN_TEST(test_mark_candidates_pair_eviction);

    /* 6. Emergency eviction */
    RUN_TEST(test_emergency_evict_basic);
    RUN_TEST(test_emergency_evict_respects_target);
    RUN_TEST(test_emergency_evict_default_target);
    RUN_TEST(test_emergency_evict_already_below_target);
    RUN_TEST(test_emergency_evict_preserves_critical);

    /* 7. Scenario tests */
    RUN_TEST(test_fix5_floor_with_large_tail);
    RUN_TEST(test_fix11_scratchpad_prefix);
    RUN_TEST(test_inject_scratchpad_msg);
    RUN_TEST(test_total_chars_consistency);
    RUN_TEST(test_sweep_preserves_unpaired_tools);
    RUN_TEST(test_realistic_eviction_scenario);

    TEST_SUMMARY();
}

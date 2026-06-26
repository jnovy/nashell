/*
 * test_lifecycle.c — Unit tests for tool lifecycle tracking & type-aware
 * eviction (commit 6c63e39).
 *
 * Tests cover:
 *   - evict_lifecycle_stale_reads(): stale read detection and marker replacement
 *   - evict_type_compress(): type-aware pre-compression for shell_exec,
 *     glob_search, grep_search
 *
 * Papers:
 *   - Pichay [arXiv:2603.09023]: demand paging / stale read detection
 *   - CWL [arXiv:2606.11213]: graduated type-aware compression
 *   - Complexity Trap [arXiv:2508.21433]: simple masking matches LLM summarization
 */

#include "test_common.h"
#include "../src/react_internal.h"

/* ══════════════════════════════════════════════════════════
 *  Helpers: Build synthetic chats with lifecycle metadata
 * ══════════════════════════════════════════════════════════ */

static void add_tool_pair(llm_chat_t *chat, int idx,
                          const char *tool_name, const char *tool_path,
                          const char *content) {
    char tc_id[32], tc_json[512];
    snprintf(tc_id, sizeof(tc_id), "call_%03d", idx);
    snprintf(tc_json, sizeof(tc_json),
        "[{\"id\":\"%s\",\"type\":\"function\",\"function\""
        ":{\"name\":\"%s\",\"arguments\":\"{}\"}}]",
        tc_id, tool_name);

    llm_chat_add_assistant_tool_call(chat, "thinking...", tc_json);
    llm_chat_add_tool_result(chat, tc_id, content);

    int ri = chat->n_msgs - 1;
    chat->msgs[ri].tool_name = strdup(tool_name);
    if (tool_path)
        chat->msgs[ri].tool_path = strdup(tool_path);
}

static llm_chat_t *make_lifecycle_chat(void) {
    llm_chat_t *chat = llm_chat_new();
    llm_chat_add_typed(chat, "system", "You are a helpful assistant.", LLM_MSG_SYSTEM);
    return chat;
}

static void finish_chat(llm_chat_t *chat) {
    llm_chat_add_typed(chat, "user", "Fix the bug", LLM_MSG_USER_QUERY);
}

static char *make_content(int n_chars) {
    char *buf = malloc((size_t)n_chars + 1);
    for (int i = 0; i < n_chars; i++)
        buf[i] = (i % 80 == 79) ? '\n' : 'X';
    buf[n_chars] = '\0';
    return buf;
}

static char *make_lines(int n_lines) {
    int line_len = 25;
    int total = n_lines * line_len;
    char *buf = malloc((size_t)total + 1);
    int pos = 0;
    for (int i = 0; i < n_lines; i++) {
        int wrote = snprintf(buf + pos, (size_t)(total - pos + 1),
                             "/path/to/file_%04d.c\n", i);
        pos += wrote;
    }
    buf[pos] = '\0';
    return buf;
}

/* ══════════════════════════════════════════════════════════
 *  1. evict_lifecycle_stale_reads() Tests
 * ══════════════════════════════════════════════════════════ */

/* file_read then file_edit on same path → content replaced */
static void test_stale_read_edited(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/foo.c",
                  "int foo(void) { return 42; }");
    add_tool_pair(chat, 1, "file_edit", "src/foo.c",
                  "Edit applied successfully.");
    finish_chat(chat);

    long chars_before = react_calc_total_chars(chat);
    int read_idx = 2;  /* 0=system, 1=tc, 2=file_read result */

    ASSERT_STR_CONTAINS(chat->msgs[read_idx].content, "int foo");

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    ASSERT_STR_CONTAINS(chat->msgs[read_idx].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[read_idx].content, "src/foo.c");
    ASSERT_STR_CONTAINS(chat->msgs[read_idx].content, "edited");
    ASSERT_STR_CONTAINS(chat->msgs[read_idx].content, "Re-read if needed.");
    ASSERT_EQ((int)chat->msgs[read_idx].importance, (int)LLM_MSG_IMPORTANCE_LOW);
    ASSERT_EQ((int)chat->msgs[read_idx].recoverability, (int)LLM_RECOVER_FILE);
    ASSERT(react_calc_total_chars(chat) < chars_before);
    ASSERT_EQ((int)chat->msgs[read_idx].content_len,
              (int)strlen(chat->msgs[read_idx].content));

    llm_chat_free(chat);
}

/* file_read then another file_read on same path → superseded */
static void test_stale_read_superseded(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/bar.c",
                  "int bar(void) { return 1; }");
    add_tool_pair(chat, 1, "file_read", "src/bar.c",
                  "int bar(void) { return 2; }");
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    /* First read should be marked as superseded (re-read) */
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "re-read");
    /* Second read should be unchanged */
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "int bar");

    llm_chat_free(chat);
}

/* file_read then file_edit on DIFFERENT path → no change */
static void test_stale_read_different_path(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/foo.c",
                  "int foo(void) { return 42; }");
    add_tool_pair(chat, 1, "file_edit", "src/other.c",
                  "Edit applied.");
    finish_chat(chat);

    size_t original_len = chat->msgs[2].content_len;
    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    ASSERT_EQ((int)chat->msgs[2].content_len, (int)original_len);
    ASSERT((int)chat->msgs[2].importance != (int)LLM_MSG_IMPORTANCE_LOW);

    llm_chat_free(chat);
}

/* file_read with HIGH importance → skipped */
static void test_stale_read_high_importance_protected(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/critical.c",
                  "Critical code here.");
    chat->msgs[2].importance = LLM_MSG_IMPORTANCE_HIGH;
    add_tool_pair(chat, 1, "file_edit", "src/critical.c",
                  "Edit applied.");
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    ASSERT_STR_CONTAINS(chat->msgs[2].content, "Critical code");
    ASSERT_EQ((int)chat->msgs[2].importance, (int)LLM_MSG_IMPORTANCE_HIGH);

    llm_chat_free(chat);
}

/* No file_reads in the range → no changes */
static void test_stale_read_no_reads(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "grep_search", "src/foo.c", "Found 5 matches.");
    add_tool_pair(chat, 1, "shell_exec", NULL, "Build succeeded.");
    finish_chat(chat);

    long chars_before = react_calc_total_chars(chat);
    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);
    ASSERT_EQ((int)chars_before, (int)react_calc_total_chars(chat));

    llm_chat_free(chat);
}

/* Multiple stale reads — all should be replaced */
static void test_stale_read_multiple(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/foo.c", "contents of foo");
    add_tool_pair(chat, 1, "file_read", "src/bar.c", "contents of bar");
    add_tool_pair(chat, 2, "file_edit", "src/foo.c", "foo edited.");
    add_tool_pair(chat, 3, "file_edit", "src/bar.c", "bar edited.");
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    ASSERT_STR_CONTAINS(chat->msgs[2].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "src/foo.c");
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "src/bar.c");

    llm_chat_free(chat);
}

/* Edit takes priority over re-read */
static void test_stale_read_edit_over_reread(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "file_read", "src/foo.c", "original content");
    add_tool_pair(chat, 1, "file_read", "src/foo.c", "re-read content");
    add_tool_pair(chat, 2, "file_edit", "src/foo.c", "foo edited.");
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    /* First read: edit found after re-read → should say "edited" */
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "edited");
    /* Second read: also stale because of the edit */
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "[Stale:");
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "edited");

    llm_chat_free(chat);
}

/* Empty eviction range → no crash */
static void test_stale_read_empty_range(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, 1);
    evict_lifecycle_stale_reads(chat, 3, 1);
    ASSERT(1);

    llm_chat_free(chat);
}

/* ══════════════════════════════════════════════════════════
 *  2. evict_type_compress() Tests
 * ══════════════════════════════════════════════════════════ */

/* shell_exec output > 2000 chars → head+tail truncated */
static void test_type_compress_shell_exec(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *big_output = make_content(5000);
    add_tool_pair(chat, 0, "shell_exec", NULL, big_output);
    free(big_output);
    finish_chat(chat);

    int ri = 2;
    size_t len_before = chat->msgs[ri].content_len;
    ASSERT(len_before > 2000);

    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 1);

    ASSERT(chat->msgs[ri].content_len < len_before);
    ASSERT(chat->msgs[ri].content_len < 1500);
    ASSERT_STR_CONTAINS(chat->msgs[ri].content, "truncated");
    ASSERT_EQ((int)chat->msgs[ri].content_len,
              (int)strlen(chat->msgs[ri].content));

    llm_chat_free(chat);
}

/* shell_exec output < 2000 chars → unchanged */
static void test_type_compress_shell_exec_small(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "shell_exec", NULL,
                  "Build succeeded. 0 errors, 0 warnings.");
    finish_chat(chat);

    size_t len_before = chat->msgs[2].content_len;
    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 0);
    ASSERT_EQ((int)chat->msgs[2].content_len, (int)len_before);

    llm_chat_free(chat);
}

/* glob_search with > 50 lines → truncated */
static void test_type_compress_glob_search(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *many_files = make_lines(100);
    add_tool_pair(chat, 0, "glob_search", NULL, many_files);
    free(many_files);
    finish_chat(chat);

    size_t len_before = chat->msgs[2].content_len;

    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 1);

    ASSERT(chat->msgs[2].content_len < len_before);
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "omitted");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "100 total");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "file_0000");
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "file_0099");
    ASSERT_EQ((int)chat->msgs[2].content_len,
              (int)strlen(chat->msgs[2].content));

    llm_chat_free(chat);
}

/* glob_search with ≤ 50 lines → unchanged */
static void test_type_compress_glob_search_small(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *few_files = make_lines(30);
    add_tool_pair(chat, 0, "glob_search", NULL, few_files);
    free(few_files);
    finish_chat(chat);

    size_t len_before = chat->msgs[2].content_len;
    int compressed = evict_type_compress(chat, 1, 1, 100);
    ASSERT_EQ(compressed, 0);
    ASSERT_EQ((int)chat->msgs[2].content_len, (int)len_before);

    llm_chat_free(chat);
}

/* grep_search output > 2000 chars → head+tail truncated */
static void test_type_compress_grep_search(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *big_grep = make_content(4000);
    add_tool_pair(chat, 0, "grep_search", "src/", big_grep);
    free(big_grep);
    finish_chat(chat);

    size_t len_before = chat->msgs[2].content_len;
    ASSERT(len_before > 2000);

    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 1);

    ASSERT(chat->msgs[2].content_len < len_before);
    ASSERT(chat->msgs[2].content_len < 1500);
    ASSERT_STR_CONTAINS(chat->msgs[2].content, "omitted");
    ASSERT_EQ((int)chat->msgs[2].content_len,
              (int)strlen(chat->msgs[2].content));

    llm_chat_free(chat);
}

/* HIGH importance messages → skipped */
static void test_type_compress_high_importance(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *big_output = make_content(5000);
    add_tool_pair(chat, 0, "shell_exec", NULL, big_output);
    free(big_output);
    chat->msgs[2].importance = LLM_MSG_IMPORTANCE_HIGH;
    finish_chat(chat);

    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 0);

    llm_chat_free(chat);
}

/* Messages below compress_min_len → skipped */
static void test_type_compress_below_min_len(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *medium = make_content(2500);
    add_tool_pair(chat, 0, "shell_exec", NULL, medium);
    free(medium);
    finish_chat(chat);

    int compressed = evict_type_compress(chat, 1, 1, 3000);
    ASSERT_EQ(compressed, 0);

    llm_chat_free(chat);
}

/* Messages in head/tail region → skipped */
static void test_type_compress_respects_boundaries(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    char *big1 = make_content(5000);
    char *big2 = make_content(5000);
    add_tool_pair(chat, 0, "shell_exec", NULL, big1);
    add_tool_pair(chat, 1, "shell_exec", NULL, big2);
    free(big1);
    free(big2);
    finish_chat(chat);

    /* keep_head=3 covers first pair, keep_tail=3 covers second + user */
    int compressed = evict_type_compress(chat, 3, 3, 800);
    ASSERT_EQ(compressed, 0);

    llm_chat_free(chat);
}

/* Multiple tool types → all compressed */
static void test_type_compress_mixed(void) {
    llm_chat_t *chat = make_lifecycle_chat();

    char *big_shell = make_content(5000);
    add_tool_pair(chat, 0, "shell_exec", NULL, big_shell);
    free(big_shell);

    char *many_files = make_lines(100);
    add_tool_pair(chat, 1, "glob_search", NULL, many_files);
    free(many_files);

    char *big_grep = make_content(4000);
    add_tool_pair(chat, 2, "grep_search", "src/", big_grep);
    free(big_grep);

    finish_chat(chat);

    int compressed = evict_type_compress(chat, 1, 1, 800);
    ASSERT_EQ(compressed, 3);

    ASSERT_STR_CONTAINS(chat->msgs[2].content, "truncated");
    ASSERT_STR_CONTAINS(chat->msgs[4].content, "omitted");
    ASSERT_STR_CONTAINS(chat->msgs[6].content, "omitted");

    llm_chat_free(chat);
}

/* ══════════════════════════════════════════════════════════
 *  3. Integration Tests
 * ══════════════════════════════════════════════════════════ */

/* total_chars consistency after lifecycle + type-compress */
static void test_total_chars_consistency(void) {
    llm_chat_t *chat = make_lifecycle_chat();

    char *big_content = make_content(3000);
    add_tool_pair(chat, 0, "file_read", "src/foo.c", big_content);
    free(big_content);
    add_tool_pair(chat, 1, "file_edit", "src/foo.c", "Edit applied.");
    char *big_shell = make_content(5000);
    add_tool_pair(chat, 2, "shell_exec", NULL, big_shell);
    free(big_shell);
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);
    evict_type_compress(chat, 1, 1, 800);

    /* Verify total_chars matches manual sum */
    long manual_sum = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        manual_sum += (long)chat->msgs[i].content_len;
    ASSERT_EQ((int)react_calc_total_chars(chat), (int)manual_sum);

    /* Verify each message's content_len matches strlen */
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].content)
            ASSERT_EQ((int)chat->msgs[i].content_len,
                      (int)strlen(chat->msgs[i].content));
    }

    llm_chat_free(chat);
}

/* Stale reads get LOW importance → evicted before normal msgs */
static void test_stale_reads_get_low_importance(void) {
    llm_chat_t *chat = make_lifecycle_chat();
    add_tool_pair(chat, 0, "grep_search", "src/", "Found 5 matches");
    add_tool_pair(chat, 1, "file_read", "src/foo.c",
                  "int foo(void) { return 42; }");
    add_tool_pair(chat, 2, "file_edit", "src/foo.c", "Edit applied.");
    finish_chat(chat);

    evict_lifecycle_stale_reads(chat, 1, chat->n_msgs - 1);

    /* grep_search result at idx 2 should still be NORMAL */
    ASSERT_EQ((int)chat->msgs[2].importance, (int)LLM_MSG_IMPORTANCE_NORMAL);
    /* file_read result at idx 4 should now be LOW (stale) */
    ASSERT_EQ((int)chat->msgs[4].importance, (int)LLM_MSG_IMPORTANCE_LOW);
    ASSERT_EQ((int)chat->msgs[4].recoverability, (int)LLM_RECOVER_FILE);

    llm_chat_free(chat);
}

/* ══════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_lifecycle:\n");

    /* 1. Stale read detection */
    RUN_TEST(test_stale_read_edited);
    RUN_TEST(test_stale_read_superseded);
    RUN_TEST(test_stale_read_different_path);
    RUN_TEST(test_stale_read_high_importance_protected);
    RUN_TEST(test_stale_read_no_reads);
    RUN_TEST(test_stale_read_multiple);
    RUN_TEST(test_stale_read_edit_over_reread);
    RUN_TEST(test_stale_read_empty_range);

    /* 2. Type-aware compression */
    RUN_TEST(test_type_compress_shell_exec);
    RUN_TEST(test_type_compress_shell_exec_small);
    RUN_TEST(test_type_compress_glob_search);
    RUN_TEST(test_type_compress_glob_search_small);
    RUN_TEST(test_type_compress_grep_search);
    RUN_TEST(test_type_compress_high_importance);
    RUN_TEST(test_type_compress_below_min_len);
    RUN_TEST(test_type_compress_respects_boundaries);
    RUN_TEST(test_type_compress_mixed);

    /* 3. Integration */
    RUN_TEST(test_total_chars_consistency);
    RUN_TEST(test_stale_reads_get_low_importance);

    TEST_SUMMARY();
}

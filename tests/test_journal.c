#include "test_common.h"
#include "../src/journal.h"
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */

/* ── test_append_and_manifest ── */
static void test_append_and_manifest(void) {
    char *dir = make_test_dir();
    journal_t *j = journal_new(dir);
    ASSERT_NOT_NULL(j);

    /* Append a few entries */
    cJSON *params1 = cJSON_CreateObject();
    cJSON_AddStringToObject(params1, "command", "ls -la");
    journal_append(j, 0, 1, "shell_exec", params1, "R0S1", 100, 5, NULL);
    cJSON_Delete(params1);

    cJSON *params2 = cJSON_CreateObject();
    cJSON_AddStringToObject(params2, "path", "test.c");
    journal_append(j, 0, 2, "file_read", params2, "R0S2", 500, 20, NULL);
    cJSON_Delete(params2);

    /* Verify journal file exists */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", dir);
    struct stat st;
    ASSERT_EQ(stat(jpath, &st), 0);
    ASSERT_GT((int)st.st_size, 0);

    /* Verify manifest */
    char *manifest = journal_manifest(j, 50);
    ASSERT_NOT_NULL(manifest);
    ASSERT(strstr(manifest, "shell_exec") != NULL);
    ASSERT(strstr(manifest, "file_read") != NULL);
    ASSERT(strstr(manifest, "R0S1") != NULL);
    free(manifest);

    journal_free(j);
    rm_rf(dir);
    free(dir);
}

/* ── test_react_loop_grouping ── */
static void test_react_loop_grouping(void) {
    char *dir = make_test_dir();
    journal_t *j = journal_new(dir);

    /* React loop 0: query + shell_exec + done */
    cJSON *q0 = cJSON_CreateObject();
    cJSON_AddStringToObject(q0, "text", "list files");
    journal_append(j, 0, 0, "query", q0, NULL, 10, 0, NULL);
    cJSON_Delete(q0);

    cJSON *s0 = cJSON_CreateObject();
    cJSON_AddStringToObject(s0, "command", "ls");
    journal_append(j, 0, 1, "shell_exec", s0, "R0S1", 50, 3, NULL);
    cJSON_Delete(s0);

    /* React loop 1: different query */
    cJSON *q1 = cJSON_CreateObject();
    cJSON_AddStringToObject(q1, "text", "show kernel");
    journal_append(j, 1, 0, "query", q1, NULL, 11, 0, NULL);
    cJSON_Delete(q1);

    cJSON *s1 = cJSON_CreateObject();
    cJSON_AddStringToObject(s1, "command", "uname -r");
    journal_append(j, 1, 1, "shell_exec", s1, "R1S1", 24, 1, NULL);
    cJSON_Delete(s1);

    /* Manifest should group by react loop */
    char *manifest = journal_manifest(j, 50);
    ASSERT_NOT_NULL(manifest);
    ASSERT(strstr(manifest, "list files") != NULL);
    ASSERT(strstr(manifest, "show kernel") != NULL);
    ASSERT(strstr(manifest, "R0S1") != NULL);
    ASSERT(strstr(manifest, "R1S1") != NULL);
    free(manifest);

    journal_free(j);
    rm_rf(dir);
    free(dir);
}

/* ── test_empty_manifest ── */
static void test_empty_manifest(void) {
    char *dir = make_test_dir();
    journal_t *j = journal_new(dir);

    char *manifest = journal_manifest(j, 50);
    ASSERT_NOT_NULL(manifest);
    ASSERT(strstr(manifest, "empty") != NULL);
    free(manifest);

    journal_free(j);
    rm_rf(dir);
    free(dir);
}

/* ── test_error_entries ── */
static void test_error_entries(void) {
    char *dir = make_test_dir();
    journal_t *j = journal_new(dir);

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "command", "false");
    journal_append(j, 0, 1, "shell_exec", p, "R0S1", 0, 0, "non-zero exit");
    cJSON_Delete(p);

    char *manifest = journal_manifest(j, 50);
    ASSERT_NOT_NULL(manifest);
    /* Error entries should appear in manifest */
    ASSERT(strstr(manifest, "shell_exec") != NULL);
    free(manifest);

    journal_free(j);
    rm_rf(dir);
    free(dir);
}

int main(void) {
    printf("test_journal:\n");
    RUN_TEST(test_append_and_manifest);
    RUN_TEST(test_react_loop_grouping);
    RUN_TEST(test_empty_manifest);
    RUN_TEST(test_error_entries);
    TEST_SUMMARY();
}

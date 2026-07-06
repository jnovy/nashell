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
    journal_append(j, 0, 1, "shell_exec", params1, "R0S1", 100, 5, NULL, NULL, 0.0);
    cJSON_Delete(params1);

    cJSON *params2 = cJSON_CreateObject();
    cJSON_AddStringToObject(params2, "path", "test.c");
    journal_append(j, 0, 2, "file_read", params2, "R0S2", 500, 20, NULL, NULL, 0.0);
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
    journal_append(j, 0, 0, "query", q0, NULL, 10, 0, NULL, NULL, 0.0);
    cJSON_Delete(q0);

    cJSON *s0 = cJSON_CreateObject();
    cJSON_AddStringToObject(s0, "command", "ls");
    journal_append(j, 0, 1, "shell_exec", s0, "R0S1", 50, 3, NULL, NULL, 0.0);
    cJSON_Delete(s0);

    /* React loop 1: different query */
    cJSON *q1 = cJSON_CreateObject();
    cJSON_AddStringToObject(q1, "text", "show kernel");
    journal_append(j, 1, 0, "query", q1, NULL, 11, 0, NULL, NULL, 0.0);
    cJSON_Delete(q1);

    cJSON *s1 = cJSON_CreateObject();
    cJSON_AddStringToObject(s1, "command", "uname -r");
    journal_append(j, 1, 1, "shell_exec", s1, "R1S1", 24, 1, NULL, NULL, 0.0);
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
    journal_append(j, 0, 1, "shell_exec", p, "R0S1", 0, 0, "non-zero exit", NULL, 0.0);
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

/* —— test_failed_field —— */
static void test_failed_field(void) {
    char *dir = make_test_dir();
    journal_t *j = journal_new(dir);

    /* Success entry */
    cJSON *p1 = cJSON_CreateObject();
    cJSON_AddStringToObject(p1, "command", "ls");
    journal_append(j, 0, 1, "shell_exec", p1, "R0S1", 100, 5, NULL, NULL, 0.0);
    cJSON_Delete(p1);

    /* Failed entry */
    cJSON *p2 = cJSON_CreateObject();
    cJSON_AddStringToObject(p2, "query", "nonexistent");
    journal_append(j, 0, 2, "web_search", p2, NULL, 0, 0, "no results found", NULL, 0.0);
    cJSON_Delete(p2);

    /* Read journal.jsonl and verify failed field */
    char path[4096];
    snprintf(path, sizeof(path), "%s/journal.jsonl", dir);
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);

    char line[65536];
    int line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        ASSERT_NOT_NULL(entry);
        cJSON *failed = cJSON_GetObjectItem(entry, "failed");
        ASSERT_NOT_NULL(failed);  /* failed field must always be present */

        if (line_num == 0) {
            /* Success: failed=false */
            ASSERT(cJSON_IsFalse(failed));
        } else if (line_num == 1) {
            /* Failure: failed=true */
            ASSERT(cJSON_IsTrue(failed));
            cJSON *err = cJSON_GetObjectItem(entry, "error");
            ASSERT_NOT_NULL(err);
            ASSERT_STR_CONTAINS(err->valuestring, "no results");
        }
        cJSON_Delete(entry);
        line_num++;
    }
    fclose(f);
    ASSERT_EQ(line_num, 2);

    /* Verify manifest shows ✓/✗ markers */
    char *manifest = journal_manifest(j, 50);
    ASSERT_NOT_NULL(manifest);
    ASSERT(strstr(manifest, "+") != NULL);  /* ✓ UTF-8 */
    ASSERT(strstr(manifest, "x") != NULL);  /* ✗ UTF-8 */
    /* Error text NOT inlined in manifest — model looks it up via ref */
    ASSERT(strstr(manifest, "ERROR") == NULL);
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
    RUN_TEST(test_failed_field);
    TEST_SUMMARY();
}

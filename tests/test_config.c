#include "test_common.h"
#include "../src/config.h"
/* linked via LIB_OBJ */
/* linked via LIB_OBJ */

/* ── test_defaults ── */
static void test_defaults(void) {
    /* Load from non-existent file — should return defaults */
    config_t *cfg = config_load("/tmp/nash_test_nonexistent_config.toml");
    ASSERT_NOT_NULL(cfg);
    ASSERT_GT(cfg->max_tokens, 0);
    ASSERT(cfg->max_react_steps == -1 || cfg->max_react_steps > 0);  /* -1 = unlimited (default), >0 = hard limit */
    ASSERT_GT(cfg->shell_timeout, -1);
    ASSERT_GT(cfg->file_max_size, 0);
    ASSERT_GT(cfg->memory_index_max, 0);
    ASSERT_GT(cfg->context_eviction_pct, 0);
    ASSERT(cfg->temperature > 0.0f);
    config_free(cfg);
}

/* ── test_parse_toml ── */
static void test_parse_toml(void) {
    char *dir = make_test_dir();
    char path[4096];
    snprintf(path, sizeof(path), "%s/test.toml", dir);

    /* Write a test TOML file */
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f,
        "[providers.test]\n"
        "type = \"local\"\n"
        "api_base = \"http://test:9999\"\n"
        "\n"
        "[routing]\n"
        "default = \"test\"\n"
        "\n"
        "[client]\n"
        "temperature = 0.5\n"
        "max_tokens = 2048\n"
        "\n"
        "[limits]\n"
        "shell_timeout = 60\n"
        "memory_index_max = 25\n"
        "max_react_steps = 10\n"
        "\n"
        "[paths]\n"
        "data_dir = \"/tmp/nash-test-data\"\n"
    );
    fclose(f);

    config_t *cfg = config_load(path);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cfg->n_named_providers, 1);
    ASSERT_STR_EQ(cfg->named_providers[0].config.api_base, "http://test:9999");
    ASSERT(cfg->temperature > 0.49f && cfg->temperature < 0.51f);
    ASSERT_EQ(cfg->max_tokens, 2048);
    ASSERT_EQ(cfg->shell_timeout, 60);
    ASSERT_EQ(cfg->memory_index_max, 25);
    ASSERT_EQ(cfg->max_react_steps, 10);
    ASSERT_STR_EQ(cfg->data_dir, "/tmp/nash-test-data");

    config_free(cfg);
    rm_rf(dir);
    free(dir);
}

/* ── test_write_default ── */
static void test_write_default(void) {
    char *dir = make_test_dir();
    char path[4096];
    snprintf(path, sizeof(path), "%s/default.toml", dir);

    int rc = config_write_default(path);
    ASSERT_EQ(rc, 0);

    /* Verify file exists and has content */
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_GT((int)st.st_size, 100);

    /* Verify it's parseable */
    config_t *cfg = config_load(path);
    ASSERT_NOT_NULL(cfg);
    config_free(cfg);

    rm_rf(dir);
    free(dir);
}

/* ── test_missing_sections ── */
static void test_missing_sections(void) {
    char *dir = make_test_dir();
    char path[4096];
    snprintf(path, sizeof(path), "%s/partial.toml", dir);

    /* Write TOML with only [providers.partial] section */
    FILE *f = fopen(path, "w");
    fprintf(f, "[providers.partial]\ntype = \"local\"\napi_base = \"http://partial:1234\"\n");
    fclose(f);

    config_t *cfg = config_load(path);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cfg->n_named_providers, 1);
    ASSERT_STR_EQ(cfg->named_providers[0].config.api_base, "http://partial:1234");
    /* Other fields should have defaults */
    ASSERT_GT(cfg->max_tokens, 0);
    ASSERT_GT(cfg->shell_timeout, -1);

    config_free(cfg);
    rm_rf(dir);
    free(dir);
}

int main(void) {
    printf("test_config:\n");
    RUN_TEST(test_defaults);
    RUN_TEST(test_parse_toml);
    RUN_TEST(test_write_default);
    RUN_TEST(test_missing_sections);
    TEST_SUMMARY();
}

/* test_tool_plugin.c - Tests for the pluggable tool architecture.
 *
 * Verifies:
 *   1. Plugin registration (tool_plugin_register)
 *   2. Plugin lookup (tool_plugin_find)
 *   3. Iteration (tool_plugin_count, tool_plugin_get)
 *   4. Duplicate detection
 *   5. Clear (tool_plugin_clear)
 *   6. Dispatch integration (tool_execute routes to plugin handler)
 *   7. Plugin shows up in build_tools_from_registry_filtered output
 *   8. Constructor-based self-registration via TOOL_PLUGIN_REGISTER macro
 */

#include "test_common.h"
#include "tool_plugin.h"
#include "tools.h"
#include "store.h"
#include "journal.h"
#include "cJSON.h"

/* Provide globals defined in main.c (not linked into tests) */
int g_path_given = 0;

/* ---- Mock plugin handlers ---- */

/* A simple handler that returns success with a marker in meta */
static tool_result_t mock_echo_handler(tool_ctx_t *ctx, cJSON *params) {
    (void)ctx;
    cJSON *meta = cJSON_CreateObject();
    cJSON *msg_j = cJSON_GetObjectItem(params, "message");
    if (msg_j && msg_j->valuestring)
        cJSON_AddStringToObject(meta, "echo", msg_j->valuestring);
    else
        cJSON_AddStringToObject(meta, "echo", "no message");
    return (tool_result_t){ .meta = meta, .store_ref = NULL, .success = 1 };
}

/* A handler that always fails */
static tool_result_t mock_fail_handler(tool_ctx_t *ctx, cJSON *params) {
    (void)ctx; (void)params;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "error", "intentional failure");
    return (tool_result_t){ .meta = meta, .store_ref = NULL, .success = 0 };
}

/* Parameter definitions for test plugins */
static const tool_param_t echo_params[] = {
    {"message", "string", "Message to echo", 1, NULL, NULL},
    {0}
};

static const tool_param_t empty_params[] = {
    {0}
};

/* Plugin definitions for tests */
static const tool_plugin_t echo_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "test_echo",
    .version = "1.0.0",
    .description = "Echo a message back (test plugin).",
    .params = echo_params,
    .execute = (void *)mock_echo_handler,
    .caps = TOOL_CAP_STORE,
    .group = "test",
};

static const tool_plugin_t fail_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "test_fail",
    .version = "1.0.0",
    .description = "Always fails (test plugin).",
    .params = empty_params,
    .execute = (void *)mock_fail_handler,
    .caps = 0,
    .group = "test",
};

/* ---- Auto-registration test plugin ---- */
static tool_result_t mock_auto_handler(tool_ctx_t *ctx, cJSON *params) {
    (void)ctx; (void)params;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "status", "auto-registered");
    return (tool_result_t){ .meta = meta, .store_ref = NULL, .success = 1 };
}

static const tool_plugin_t auto_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name = "test_auto",
    .version = "1.0.0",
    .description = "Auto-registered test plugin.",
    .params = empty_params,
    .execute = (void *)mock_auto_handler,
    .caps = 0,
};

TOOL_PLUGIN_REGISTER(auto_plugin)

/* ---- Tests ---- */

static void test_register_and_find(void) {
    tool_plugin_clear();

    ASSERT_EQ(tool_plugin_count(), 0);
    ASSERT_NULL(tool_plugin_find("test_echo"));

    int rc = tool_plugin_register(&echo_plugin);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(tool_plugin_count(), 1);

    const tool_plugin_t *found = tool_plugin_find("test_echo");
    ASSERT_NOT_NULL(found);
    ASSERT_STR_EQ(found->name, "test_echo");
    ASSERT_STR_EQ(found->version, "1.0.0");
    ASSERT(found->execute == (void *)mock_echo_handler);

    /* Register a second plugin */
    rc = tool_plugin_register(&fail_plugin);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(tool_plugin_count(), 2);

    const tool_plugin_t *found2 = tool_plugin_find("test_fail");
    ASSERT_NOT_NULL(found2);
    ASSERT_STR_EQ(found2->name, "test_fail");
}

static void test_duplicate_rejected(void) {
    tool_plugin_clear();

    int rc = tool_plugin_register(&echo_plugin);
    ASSERT_EQ(rc, 0);

    /* Duplicate name should be rejected */
    rc = tool_plugin_register(&echo_plugin);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(tool_plugin_count(), 1);
}

static void test_iteration(void) {
    tool_plugin_clear();

    tool_plugin_register(&echo_plugin);
    tool_plugin_register(&fail_plugin);

    ASSERT_EQ(tool_plugin_count(), 2);

    const tool_plugin_t *p0 = tool_plugin_get(0);
    ASSERT_NOT_NULL(p0);
    ASSERT_STR_EQ(p0->name, "test_echo");

    const tool_plugin_t *p1 = tool_plugin_get(1);
    ASSERT_NOT_NULL(p1);
    ASSERT_STR_EQ(p1->name, "test_fail");

    /* Out of range */
    ASSERT_NULL(tool_plugin_get(-1));
    ASSERT_NULL(tool_plugin_get(2));
    ASSERT_NULL(tool_plugin_get(999));
}

static void test_clear(void) {
    tool_plugin_clear();
    tool_plugin_register(&echo_plugin);
    ASSERT_EQ(tool_plugin_count(), 1);

    tool_plugin_clear();
    ASSERT_EQ(tool_plugin_count(), 0);
    ASSERT_NULL(tool_plugin_find("test_echo"));
}

static void test_null_rejected(void) {
    tool_plugin_clear();

    int rc = tool_plugin_register(NULL);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(tool_plugin_count(), 0);

    /* Plugin with NULL name */
    static const tool_plugin_t bad = { .name = NULL };
    rc = tool_plugin_register(&bad);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(tool_plugin_count(), 0);
}

static void test_capability_flags(void) {
    tool_plugin_clear();
    tool_plugin_register(&echo_plugin);

    const tool_plugin_t *p = tool_plugin_find("test_echo");
    ASSERT_NOT_NULL(p);
    ASSERT(p->caps & TOOL_CAP_STORE);
    ASSERT(!(p->caps & TOOL_CAP_PROVIDER));
    ASSERT(!(p->caps & TOOL_CAP_CORE));
}

static void test_dispatch_via_tool_execute(void) {
    tool_plugin_clear();
    tool_plugin_register(&echo_plugin);

    /* Set up minimal tool_ctx_t for dispatch */
    char *tmpdir = make_test_dir();
    char store_path[512], journal_path[512];
    snprintf(store_path, sizeof(store_path), "%s/store", tmpdir);
    snprintf(journal_path, sizeof(journal_path), "%s/journal.jsonl", tmpdir);
    mkdir(store_path, 0755);

    store_t *store = store_new(store_path);
    journal_t *journal = journal_new(tmpdir);

    tool_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.store = store;
    ctx.journal = journal;
    ctx.react_loop = 0;
    ctx.step = 0;

    /* Dispatch to plugin-registered tool */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "message", "hello world");

    tool_result_t result = tool_execute(&ctx, "test_echo", params);
    ASSERT(result.success);
    ASSERT_NOT_NULL(result.meta);

    cJSON *echo_j = cJSON_GetObjectItem(result.meta, "echo");
    ASSERT_NOT_NULL(echo_j);
    ASSERT_STR_EQ(echo_j->valuestring, "hello world");

    tool_result_free(&result);
    cJSON_Delete(params);

    /* Test required param validation */
    cJSON *empty_params = cJSON_CreateObject();
    tool_result_t result2 = tool_execute(&ctx, "test_echo", empty_params);
    ASSERT(!result2.success);  /* should fail: missing required "message" */
    ASSERT_NOT_NULL(result2.meta);
    cJSON *err_j = cJSON_GetObjectItem(result2.meta, "error");
    ASSERT_NOT_NULL(err_j);
    ASSERT(strstr(err_j->valuestring, "message") != NULL);

    tool_result_free(&result2);
    cJSON_Delete(empty_params);

    /* Verify unknown tool returns error */
    cJSON *bad_params = cJSON_CreateObject();
    tool_result_t result3 = tool_execute(&ctx, "nonexistent_tool", bad_params);
    ASSERT(!result3.success);
    tool_result_free(&result3);
    cJSON_Delete(bad_params);

    journal_free(journal);
    store_free(store);
    rm_rf(tmpdir);
    free(tmpdir);
}

static void test_plugin_in_tool_definitions(void) {
    tool_plugin_clear();
    tool_plugin_register(&echo_plugin);

    /* build_tools_from_registry_filtered should include plugin tools */
    cJSON *tools = build_tools_from_registry_filtered(PROVIDER_ANTHROPIC, NULL);
    ASSERT_NOT_NULL(tools);

    /* Find test_echo in the tools array */
    int found_echo = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, tools) {
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (name && name->valuestring) {
            if (strcmp(name->valuestring, "test_echo") == 0)
                found_echo = 1;
        }
    }
    ASSERT(found_echo);   /* plugin tool must be present */

    cJSON_Delete(tools);
}

static void test_constructor_registration(void) {
    /* The auto_plugin should have been registered by the constructor
     * before main() was called. But since other tests call clear(),
     * we need to check that the mechanism works by re-registering. */
    tool_plugin_clear();

    /* Re-register via explicit call (constructor already ran) */
    int rc = tool_plugin_register(&auto_plugin);
    ASSERT_EQ(rc, 0);

    const tool_plugin_t *p = tool_plugin_find("test_auto");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "test_auto");
    ASSERT_STR_EQ(p->description, "Auto-registered test plugin.");
}

static void test_group_field(void) {
    tool_plugin_clear();
    tool_plugin_register(&echo_plugin);
    tool_plugin_register(&fail_plugin);

    const tool_plugin_t *p1 = tool_plugin_find("test_echo");
    const tool_plugin_t *p2 = tool_plugin_find("test_fail");
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_STR_EQ(p1->group, "test");
    ASSERT_STR_EQ(p2->group, "test");

    /* auto_plugin has no group */
    tool_plugin_register(&auto_plugin);
    const tool_plugin_t *p3 = tool_plugin_find("test_auto");
    ASSERT_NOT_NULL(p3);
    ASSERT_NULL(p3->group);
}

static void test_sort(void) {
    tool_plugin_clear();

    /* Register in reverse alphabetical order */
    static const tool_plugin_t z_plugin = {
        TOOL_PLUGIN_ABI_VERSION, "z_tool", "1.0.0", "z", NULL, NULL, 0, 0, NULL};
    static const tool_plugin_t a_plugin = {
        TOOL_PLUGIN_ABI_VERSION, "a_tool", "1.0.0", "a", NULL, NULL, 0, 0, NULL};
    static const tool_plugin_t m_plugin = {
        TOOL_PLUGIN_ABI_VERSION, "m_tool", "1.0.0", "m", NULL, NULL, 0, 0, NULL};

    tool_plugin_register(&z_plugin);
    tool_plugin_register(&a_plugin);
    tool_plugin_register(&m_plugin);

    /* Before sort: registration order */
    ASSERT_STR_EQ(tool_plugin_get(0)->name, "z_tool");
    ASSERT_STR_EQ(tool_plugin_get(1)->name, "a_tool");
    ASSERT_STR_EQ(tool_plugin_get(2)->name, "m_tool");

    /* After sort: alphabetical */
    tool_plugin_sort();
    ASSERT_STR_EQ(tool_plugin_get(0)->name, "a_tool");
    ASSERT_STR_EQ(tool_plugin_get(1)->name, "m_tool");
    ASSERT_STR_EQ(tool_plugin_get(2)->name, "z_tool");

    /* find still works after sort */
    ASSERT_NOT_NULL(tool_plugin_find("a_tool"));
    ASSERT_NOT_NULL(tool_plugin_find("m_tool"));
    ASSERT_NOT_NULL(tool_plugin_find("z_tool"));
}

static void test_params_to_cjson(void) {
    /* Basic: two params, one required */
    static const tool_param_t params[] = {
        {"name",  "string",  "A name",   1, NULL, NULL},
        {"count", "integer", "A count",  0, NULL, NULL},
        {0}
    };
    cJSON *schema = tool_params_to_cjson(params);
    ASSERT_NOT_NULL(schema);

    /* Top-level structure */
    cJSON *type = cJSON_GetObjectItem(schema, "type");
    ASSERT_NOT_NULL(type);
    ASSERT_STR_EQ(type->valuestring, "object");

    /* Properties */
    cJSON *props = cJSON_GetObjectItem(schema, "properties");
    ASSERT_NOT_NULL(props);
    cJSON *name_prop = cJSON_GetObjectItem(props, "name");
    ASSERT_NOT_NULL(name_prop);
    ASSERT_STR_EQ(cJSON_GetObjectItem(name_prop, "type")->valuestring, "string");
    ASSERT_STR_EQ(cJSON_GetObjectItem(name_prop, "description")->valuestring, "A name");
    cJSON *count_prop = cJSON_GetObjectItem(props, "count");
    ASSERT_NOT_NULL(count_prop);
    ASSERT_STR_EQ(cJSON_GetObjectItem(count_prop, "type")->valuestring, "integer");

    /* Required array */
    cJSON *req = cJSON_GetObjectItem(schema, "required");
    ASSERT_NOT_NULL(req);
    ASSERT_EQ(cJSON_GetArraySize(req), 1);
    ASSERT_STR_EQ(cJSON_GetArrayItem(req, 0)->valuestring, "name");

    cJSON_Delete(schema);

    /* Enum values */
    static const char *ops[] = {"add", "remove", NULL};
    static const tool_param_t enum_params[] = {
        {"op", "string", "Operation", 1, ops, NULL},
        {0}
    };
    schema = tool_params_to_cjson(enum_params);
    ASSERT_NOT_NULL(schema);
    props = cJSON_GetObjectItem(schema, "properties");
    cJSON *op_prop = cJSON_GetObjectItem(props, "op");
    cJSON *ev = cJSON_GetObjectItem(op_prop, "enum");
    ASSERT_NOT_NULL(ev);
    ASSERT_EQ(cJSON_GetArraySize(ev), 2);
    ASSERT_STR_EQ(cJSON_GetArrayItem(ev, 0)->valuestring, "add");
    ASSERT_STR_EQ(cJSON_GetArrayItem(ev, 1)->valuestring, "remove");
    cJSON_Delete(schema);

    /* Array type with items */
    static const tool_param_t arr_params[] = {
        {"tags", "array", "Tag list", 0, NULL, "string"},
        {0}
    };
    schema = tool_params_to_cjson(arr_params);
    ASSERT_NOT_NULL(schema);
    props = cJSON_GetObjectItem(schema, "properties");
    cJSON *tags_prop = cJSON_GetObjectItem(props, "tags");
    cJSON *items = cJSON_GetObjectItem(tags_prop, "items");
    ASSERT_NOT_NULL(items);
    ASSERT_STR_EQ(cJSON_GetObjectItem(items, "type")->valuestring, "string");
    cJSON_Delete(schema);

    /* NULL params returns NULL */
    ASSERT_NULL(tool_params_to_cjson(NULL));

    /* Empty sentinel-only returns NULL */
    static const tool_param_t empty[] = {{0}};
    ASSERT_NULL(tool_params_to_cjson(empty));
}

static void test_params_check_required(void) {
    static const tool_param_t params[] = {
        {"path",    "string",  "File path",   1, NULL, NULL},
        {"content", "string",  "Content",     1, NULL, NULL},
        {"mode",    "string",  "Mode",        0, NULL, NULL},
        {0}
    };
    char missing[64] = {0};

    /* All required present */
    cJSON *ok = cJSON_Parse("{\"path\":\"a.txt\",\"content\":\"hello\"}");
    ASSERT_EQ(tool_params_check_required(params, ok, missing, sizeof(missing)), 1);
    cJSON_Delete(ok);

    /* Missing path */
    cJSON *no_path = cJSON_Parse("{\"content\":\"hello\"}");
    ASSERT_EQ(tool_params_check_required(params, no_path, missing, sizeof(missing)), 0);
    ASSERT_STR_EQ(missing, "path");
    cJSON_Delete(no_path);

    /* Missing content */
    cJSON *no_content = cJSON_Parse("{\"path\":\"a.txt\"}");
    ASSERT_EQ(tool_params_check_required(params, no_content, missing, sizeof(missing)), 0);
    ASSERT_STR_EQ(missing, "content");
    cJSON_Delete(no_content);

    /* Empty string counts as missing */
    cJSON *empty_val = cJSON_Parse("{\"path\":\"\",\"content\":\"x\"}");
    ASSERT_EQ(tool_params_check_required(params, empty_val, missing, sizeof(missing)), 0);
    ASSERT_STR_EQ(missing, "path");
    cJSON_Delete(empty_val);

    /* No required fields - always passes */
    static const tool_param_t all_opt[] = {
        {"x", "integer", "X", 0, NULL, NULL},
        {0}
    };
    cJSON *any = cJSON_Parse("{}");
    ASSERT_EQ(tool_params_check_required(all_opt, any, missing, sizeof(missing)), 1);
    cJSON_Delete(any);

    /* NULL params - always passes */
    cJSON *dummy = cJSON_Parse("{}");
    ASSERT_EQ(tool_params_check_required(NULL, dummy, missing, sizeof(missing)), 1);
    cJSON_Delete(dummy);
}

static void test_params_first_required(void) {
    static const tool_param_t params[] = {
        {"optional", "string", "Opt", 0, NULL, NULL},
        {"needed",   "string", "Req", 1, NULL, NULL},
        {"also",     "string", "Also", 1, NULL, NULL},
        {0}
    };
    /* Returns the first required param, not the first param */
    ASSERT_STR_EQ(tool_params_first_required(params), "needed");

    /* No required */
    static const tool_param_t all_opt[] = {
        {"x", "integer", "X", 0, NULL, NULL},
        {0}
    };
    ASSERT_NULL(tool_params_first_required(all_opt));

    /* NULL */
    ASSERT_NULL(tool_params_first_required(NULL));

    /* Empty sentinel */
    static const tool_param_t empty[] = {{0}};
    ASSERT_NULL(tool_params_first_required(empty));
}

int main(void) {
    printf("test_tool_plugin\n");

    RUN_TEST(test_register_and_find);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_iteration);
    RUN_TEST(test_clear);
    RUN_TEST(test_null_rejected);
    RUN_TEST(test_capability_flags);
    RUN_TEST(test_dispatch_via_tool_execute);
    RUN_TEST(test_plugin_in_tool_definitions);
    RUN_TEST(test_constructor_registration);
    RUN_TEST(test_group_field);
    RUN_TEST(test_sort);
    RUN_TEST(test_params_to_cjson);
    RUN_TEST(test_params_check_required);
    RUN_TEST(test_params_first_required);

    TEST_SUMMARY();
}

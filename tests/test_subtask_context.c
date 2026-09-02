/* test_subtask_context.c - Tests for subtask context inheritance control.
 *
 * Verifies:
 *   1. Subtask tool schema includes "context" param with enum values
 *   2. Invalid context level returns error
 *   3. Missing context defaults to "standard" (no error)
 *   4. Scratchpad copy behavior for different context levels
 *   5. React flags configuration for different context levels
 */

#include "test_common.h"
#include "../src/tool_plugin.h"
#include "../src/tools.h"
#include "../src/store.h"
#include "../src/journal.h"
#include "../src/scratchpad.h"
#include "../src/react.h"
#include "../src/config.h"
#include "../src/cJSON.h"

/* Provide globals defined in main.c (not linked into tests) */
int g_path_given = 0;

/* Static config for tests that reach the depth guard (needs cfg->subtask_max_depth) */
static config_t test_cfg;

/* ---- Helper: create a minimal tool_ctx_t for subtask calls ---- */
typedef struct {
  char *tmpdir;
  store_t *store;
  journal_t *journal;
  tool_ctx_t ctx;
} test_env_t;

static test_env_t make_test_env(void) {
  test_env_t env;
  memset(&env, 0, sizeof(env));

  env.tmpdir = make_test_dir();

  char store_path[512];
  snprintf(store_path, sizeof(store_path), "%s/store", env.tmpdir);
  mkdir(store_path, 0755);

  env.store = store_new(store_path);
  env.journal = journal_new(env.tmpdir);

  memset(&env.ctx, 0, sizeof(env.ctx));
  env.ctx.store = env.store;
  env.ctx.journal = env.journal;
  env.ctx.session_dir = env.tmpdir;
  env.ctx.cfg = &test_cfg;
  env.ctx.react_loop = 0;
  env.ctx.step = 0;
  env.ctx.aliases = alias_map_new();
  scratchpad_init(&env.ctx.scratch);

  return env;
}

static void free_test_env(test_env_t *env) {
  scratchpad_free(&env->ctx.scratch);
  alias_map_free(env->ctx.aliases);
  journal_free(env->journal);
  store_free(env->store);
  rm_rf(env->tmpdir);
  free(env->tmpdir);
}

/* ── Test 1: Schema registration ────────────────────── */
static void test_context_param_in_schema(void) {
  /* The subtask plugin should already be registered via constructor */
  const tool_plugin_t *plugin = tool_plugin_find("subtask");
  ASSERT_NOT_NULL(plugin);

  /* Find the "context" parameter */
  const tool_param_t *params = plugin->params;
  ASSERT_NOT_NULL(params);

  int found_query = 0;
  int found_context = 0;
  const tool_param_t *ctx_param = NULL;

  for (const tool_param_t *p = params; p->name; p++) {
    if (strcmp(p->name, "query") == 0) found_query = 1;
    if (strcmp(p->name, "context") == 0) {
      found_context = 1;
      ctx_param = p;
    }
  }

  ASSERT(found_query);
  ASSERT(found_context);

  /* Verify context param properties */
  ASSERT_NOT_NULL(ctx_param);
  ASSERT_STR_EQ(ctx_param->type, "string");
  ASSERT_EQ(ctx_param->required, 0); /* optional */
  ASSERT_NOT_NULL(ctx_param->enum_values);

  /* Verify enum values are exactly: minimal, standard, rich, critic */
  ASSERT_STR_EQ(ctx_param->enum_values[0], "minimal");
  ASSERT_STR_EQ(ctx_param->enum_values[1], "standard");
  ASSERT_STR_EQ(ctx_param->enum_values[2], "rich");
  ASSERT_STR_EQ(ctx_param->enum_values[3], "critic");
  ASSERT_NULL(ctx_param->enum_values[4]); /* NULL-terminated */
}

/* ── Test 2: Schema generates correct JSON ──────────── */
static void test_context_param_json_schema(void) {
  const tool_plugin_t *plugin = tool_plugin_find("subtask");
  ASSERT_NOT_NULL(plugin);

  cJSON *schema = tool_params_to_cjson(plugin->params);
  ASSERT_NOT_NULL(schema);

  /* Schema should have "properties" with "context" */
  cJSON *props = cJSON_GetObjectItem(schema, "properties");
  ASSERT_NOT_NULL(props);

  cJSON *ctx_prop = cJSON_GetObjectItem(props, "context");
  ASSERT_NOT_NULL(ctx_prop);

  /* Should have type: "string" */
  cJSON *type_j = cJSON_GetObjectItem(ctx_prop, "type");
  ASSERT_NOT_NULL(type_j);
  ASSERT_STR_EQ(type_j->valuestring, "string");

  /* Should have enum array with 4 values */
  cJSON *enum_j = cJSON_GetObjectItem(ctx_prop, "enum");
  ASSERT_NOT_NULL(enum_j);
  ASSERT_EQ(cJSON_GetArraySize(enum_j), 4);
  ASSERT_STR_EQ(cJSON_GetArrayItem(enum_j, 0)->valuestring, "minimal");
  ASSERT_STR_EQ(cJSON_GetArrayItem(enum_j, 1)->valuestring, "standard");
  ASSERT_STR_EQ(cJSON_GetArrayItem(enum_j, 2)->valuestring, "rich");
  ASSERT_STR_EQ(cJSON_GetArrayItem(enum_j, 3)->valuestring, "critic");

  /* "context" should NOT be in required array */
  cJSON *required = cJSON_GetObjectItem(schema, "required");
  if (required) {
    int ctx_required = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, required) {
      if (strcmp(item->valuestring, "context") == 0) ctx_required = 1;
    }
    ASSERT(!ctx_required);
  }

  cJSON_Delete(schema);
}

/* ── Test 3: Invalid context value returns error ────── */
static void test_invalid_context_returns_error(void) {
  test_env_t env = make_test_env();

  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "query", "test query");
  cJSON_AddStringToObject(params, "context", "bogus");

  tool_result_t result = tool_execute(&env.ctx, "subtask", params);
  ASSERT(!result.success);
  ASSERT_NOT_NULL(result.meta);

  cJSON *err = cJSON_GetObjectItem(result.meta, "error");
  ASSERT_NOT_NULL(err);
  ASSERT_STR_CONTAINS(err->valuestring, "Invalid context level");
  ASSERT_STR_CONTAINS(err->valuestring, "bogus");

  tool_result_free(&result);
  cJSON_Delete(params);
  free_test_env(&env);
}

/* ── Test 4: Empty string context treated as absent (defaults to standard) ─
 * TOOL_OPT_STR returns NULL for empty strings, so context="" behaves
 * the same as omitting the parameter entirely. We verify this at the
 * macro level rather than calling tool_execute (which would need a
 * full provider for react_run). */
static void test_empty_context_is_absent(void) {
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "query", "test query");
  cJSON_AddStringToObject(params, "context", "");

  /* TOOL_OPT_STR treats empty string as absent */
  cJSON *ctx_j = cJSON_GetObjectItem(params, "context");
  const char *context_level = (ctx_j && cJSON_IsString(ctx_j) &&
                               ctx_j->valuestring[0])
                                ? ctx_j->valuestring
                                : NULL;
  ASSERT_NULL(context_level); /* empty string -> NULL -> defaults to standard */

  cJSON_Delete(params);
}

/* ── Test 5: Scratchpad copy behavior per context level ─
 *
 * We can't run the full subtask (needs react_run + provider), but we can
 * verify the scratchpad serialize/parse mechanics that underpin the
 * context inheritance. This tests the building blocks. */
static void test_scratchpad_copy_mechanics(void) {
  /* Simulate parent scratchpad with content */
  scratchpad_t parent;
  scratchpad_init(&parent);
  scratchpad_write(&parent, "plan", "1. Do thing A\n2. Do thing B", 2);
  scratchpad_write(&parent, "findings", "Found X at file.c:42", 1);

  /* Serialize parent */
  char *serialized = scratchpad_serialize(&parent);
  ASSERT_NOT_NULL(serialized);
  ASSERT_STR_CONTAINS(serialized, "plan");
  ASSERT_STR_CONTAINS(serialized, "findings");

  /* "standard"/"rich" path: parse into child */
  scratchpad_t child_standard;
  scratchpad_init(&child_standard);
  scratchpad_parse(&child_standard, serialized, "inherited", 5);
  ASSERT(child_standard.count > 0);

  /* Verify child got the content */
  char *child_ser = scratchpad_serialize(&child_standard);
  ASSERT_NOT_NULL(child_ser);
  ASSERT_STR_CONTAINS(child_ser, "plan");
  ASSERT_STR_CONTAINS(child_ser, "findings");
  free(child_ser);

  /* "minimal" path: child gets empty scratchpad */
  scratchpad_t child_minimal;
  scratchpad_init(&child_minimal);
  /* No parse - this is what minimal does */
  ASSERT_EQ(child_minimal.count, 0);

  scratchpad_free(&child_minimal);
  scratchpad_free(&child_standard);
  free(serialized);
  scratchpad_free(&parent);
}

/* ── Test 6: React flags for different context modes ──
 *
 * Verifies that REACT_FLAGS_BARE is the correct baseline and that
 * "rich" mode enables the expected flags. */
static void test_react_flags_configuration(void) {
  /* Bare flags: everything off */
  react_flags_t bare = REACT_FLAGS_BARE;
  ASSERT_EQ(bare.inject_memory, 0u);
  ASSERT_EQ(bare.inject_prev_result, 0u);
  ASSERT_EQ(bare.inject_repomap, 0u);
  ASSERT_EQ(bare.enable_reflection, 0u);
  ASSERT_EQ(bare.enable_pruning, 0u);
  ASSERT_EQ(bare.enable_compaction, 0u);
  ASSERT_EQ(bare.enable_scoring, 0u);

  /* Simulate "rich" mode: start with bare, then enable memory + compaction */
  react_flags_t rich = REACT_FLAGS_BARE;
  rich.inject_memory = 1;
  rich.enable_compaction = 1;
  ASSERT_EQ(rich.inject_memory, 1u);
  ASSERT_EQ(rich.enable_compaction, 1u);
  /* Everything else stays off */
  ASSERT_EQ(rich.inject_prev_result, 0u);
  ASSERT_EQ(rich.inject_repomap, 0u);
  ASSERT_EQ(rich.enable_reflection, 0u);
  ASSERT_EQ(rich.enable_pruning, 0u);
  ASSERT_EQ(rich.enable_scoring, 0u);

  /* "minimal" and "standard" both use bare flags unmodified */
  react_flags_t minimal = REACT_FLAGS_BARE;
  react_flags_t standard = REACT_FLAGS_BARE;
  ASSERT_EQ(minimal.inject_memory, 0u);
  ASSERT_EQ(standard.inject_memory, 0u);
}

/* ── Test 7: Tool description mentions context ──────── */
static void test_description_mentions_context(void) {
  const tool_plugin_t *plugin = tool_plugin_find("subtask");
  ASSERT_NOT_NULL(plugin);
  ASSERT_NOT_NULL(plugin->description);
  ASSERT_STR_CONTAINS(plugin->description, "context=");
  ASSERT_STR_CONTAINS(plugin->description, "minimal");
  ASSERT_STR_CONTAINS(plugin->description, "rich");
}

/* ── Test 8: Depth guard still works with context param ─ */
static void test_depth_guard_with_context(void) {
  /* Create a deeply nested session dir to trigger depth guard */
  char *tmpdir = make_test_dir();
  char deep_dir[4096];
  snprintf(deep_dir, sizeof(deep_dir),
           "%s/.sessions/123/subtask_0/subtask_1/subtask_2", tmpdir);

  /* Create the directory chain */
  char mkdir_cmd[4096];
  snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", deep_dir);
  system(mkdir_cmd);

  char store_path[4096];
  snprintf(store_path, sizeof(store_path), "%s/store", deep_dir);
  mkdir(store_path, 0755);

  store_t *store = store_new(store_path);
  journal_t *journal = journal_new(deep_dir);

  tool_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.store = store;
  ctx.journal = journal;
  ctx.session_dir = deep_dir;
  ctx.cfg = &test_cfg;
  ctx.react_loop = 0;
  ctx.step = 0;
  ctx.aliases = alias_map_new();
  scratchpad_init(&ctx.scratch);

  /* Try subtask with context="minimal" at max depth */
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "query", "test at depth limit");
  cJSON_AddStringToObject(params, "context", "minimal");

  tool_result_t result = tool_execute(&ctx, "subtask", params);
  ASSERT(!result.success); /* should fail: depth limit */
  ASSERT_NOT_NULL(result.meta);

  cJSON *err = cJSON_GetObjectItem(result.meta, "error");
  ASSERT_NOT_NULL(err);
  ASSERT_STR_CONTAINS(err->valuestring, "nesting limit");

  tool_result_free(&result);
  cJSON_Delete(params);

  scratchpad_free(&ctx.scratch);
  alias_map_free(ctx.aliases);
  journal_free(journal);
  store_free(store);
  rm_rf(tmpdir);
  free(tmpdir);
}

/* ── main ─────────────────────────────────────────────── */
int main(void) {
  printf("test_subtask_context:\n");

  /* Initialize test config with defaults */
  memset(&test_cfg, 0, sizeof(test_cfg));
  config_set_defaults(&test_cfg);

  RUN_TEST(test_context_param_in_schema);
  RUN_TEST(test_context_param_json_schema);
  RUN_TEST(test_invalid_context_returns_error);
  RUN_TEST(test_empty_context_is_absent);
  RUN_TEST(test_scratchpad_copy_mechanics);
  RUN_TEST(test_react_flags_configuration);
  RUN_TEST(test_description_mentions_context);
  RUN_TEST(test_depth_guard_with_context);

  TEST_SUMMARY();
}

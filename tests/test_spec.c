#include "test_common.h"
#include "../src/config.h"
#include <math.h>

/* ── Helpers ── */

static char *write_toml(const char *dir, const char *name, const char *content) {
  char *path = malloc(4096);
  snprintf(path, 4096, "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f) {
    perror("fopen");
    exit(1);
  }
  fprintf(f, "%s", content);
  fclose(f);
  return path;
}

/* Float comparison with tolerance */
#define ASSERT_FLOAT_EQ(a, b, eps) \
  do { \
    float _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) { \
      fprintf(stderr, "  FAIL: %s:%d: %.4f != %.4f (eps=%.4f)\n", \
              __FILE__, __LINE__, (double)_a, (double)_b, (double)(eps)); \
      failures++; \
    } else { \
      passes++; \
    } \
  } while (0)

#define ASSERT_DBL_EQ(a, b, eps) \
  do { \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) { \
      fprintf(stderr, "  FAIL: %s:%d: %.6f != %.6f (eps=%.6f)\n", \
              __FILE__, __LINE__, _a, _b, (double)(eps)); \
      failures++; \
    } else { \
      passes++; \
    } \
  } while (0)

/* ════════════════════════════════════════════════════════════════════════
 * test_profile_load_extended
 * Loads a model profile TOML with all new unified spec fields,
 * verifies every field is parsed correctly.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_profile_load_extended(void) {
  char *dir = make_test_dir();

  /* Create a models subdirectory */
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  /* Write a comprehensive profile */
  write_toml(models_dir, "test-local.toml",
             "match = \"test-local\"\n"
             "chars_per_token = 4.2\n"
             "native_context = 65536\n"
             "system_prompt_extra = \"Be concise.\"\n"
             "\n"
             "[thinking]\n"
             "mode = \"on\"\n"
             "budget = 4096\n"
             "\n"
             "[client]\n"
             "temperature = 0.4\n"
             "max_tokens = 8192\n"
             "\n"
             "[react]\n"
             "max_react_steps = 25\n"
             "max_reflection_steps = 2\n"
             "tool_retry_limit = 2\n"
             "cycling_detection = true\n"
             "inject_memory = true\n"
             "inject_prev_result = false\n"
             "enable_reflection = true\n"
             "enable_pruning = false\n"
             "enable_compaction = true\n"
             "enable_scoring = false\n"
             "\n"
             "[memory]\n"
             "recall_min_score = 0.30\n"
             "recall_blend_semantic = 0.8\n"
             "recall_blend_substring = 0.2\n"
             "vscore_exponent = 0.5\n"
             "memory_index_max = 30\n"
             "max_skills_per_query = 1\n"
             "max_lessons_per_query = 1\n"
             "max_strategies_per_query = 1\n"
             "max_antipatterns_per_query = 1\n"
             "context_eviction_pct = 60\n"
             "\n"
             "[tools]\n"
             "block = [\"web_search\", \"web_fetch\"]\n"
             "\n"
             "[tools.file_edit]\n"
             "description = \"Custom file_edit desc\"\n"
             "\n"
             "[tools.shell_exec]\n"
             "description = \"Custom shell_exec desc\"\n");

  /* Load profiles into a config */
  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(cfg->n_model_profiles, 1);

  /* Find the profile */
  const model_profile_t *p = config_match_model(cfg, "test-local-7b");
  ASSERT_NOT_NULL(p);

  /* Basic fields */
  ASSERT_STR_EQ(p->match, "test-local");
  ASSERT_FLOAT_EQ(p->chars_per_token, 4.2f, 0.01f);
  ASSERT_EQ(p->native_context, 65536);
  ASSERT_NOT_NULL(p->system_prompt_extra);
  ASSERT_STR_CONTAINS(p->system_prompt_extra, "Be concise");

  /* Thinking */
  ASSERT_EQ(p->thinking.mode, THINKING_ON);
  ASSERT_EQ(p->thinking.budget, 4096);

  /* Client */
  ASSERT_FLOAT_EQ(p->temperature, 0.4f, 0.01f);
  ASSERT_EQ(p->max_tokens, 8192);

  /* React limits */
  ASSERT_EQ(p->max_react_steps, 25);
  ASSERT_EQ(p->max_reflection_steps, 2);
  ASSERT_EQ(p->tool_retry_limit, 2);
  ASSERT_EQ(p->cycling_detection, 1);

  /* React flags */
  ASSERT_EQ(p->inject_memory, 1);
  ASSERT_EQ(p->inject_prev_result, 0);
  ASSERT_EQ(p->enable_reflection, 1);
  ASSERT_EQ(p->enable_pruning, 0);
  ASSERT_EQ(p->enable_compaction, 1);
  ASSERT_EQ(p->enable_scoring, 0);

  /* Memory */
  ASSERT_DBL_EQ(p->recall_min_score, 0.30, 0.01);
  ASSERT_FLOAT_EQ(p->recall_blend_semantic, 0.8f, 0.01f);
  ASSERT_FLOAT_EQ(p->recall_blend_substring, 0.2f, 0.01f);
  ASSERT_FLOAT_EQ(p->vscore_exponent, 0.5f, 0.01f);
  ASSERT_EQ(p->memory_index_max, 30);
  ASSERT_EQ(p->max_skills_per_query, 1);
  ASSERT_EQ(p->max_lessons_per_query, 1);
  ASSERT_EQ(p->max_strategies_per_query, 1);
  ASSERT_EQ(p->max_antipatterns_per_query, 1);
  ASSERT_EQ(p->context_eviction_pct, 60);

  /* Tools block list */
  ASSERT_EQ(p->n_tools_block, 2);
  ASSERT_NOT_NULL(p->tools_block);
  ASSERT_STR_EQ(p->tools_block[0], "web_search");
  ASSERT_STR_EQ(p->tools_block[1], "web_fetch");
  ASSERT_NULL(p->tools_allow);
  ASSERT_EQ(p->n_tools_allow, 0);

  /* Tool description overrides */
  ASSERT_EQ(p->n_tool_descs, 2);
  ASSERT_NOT_NULL(p->tool_desc_names);
  ASSERT_NOT_NULL(p->tool_desc_values);
  /* Order may vary, so check both exist */
  int found_file_edit = 0, found_shell_exec = 0;
  for (int i = 0; i < p->n_tool_descs; i++) {
    if (strcmp(p->tool_desc_names[i], "file_edit") == 0) {
      ASSERT_STR_EQ(p->tool_desc_values[i], "Custom file_edit desc");
      found_file_edit = 1;
    }
    if (strcmp(p->tool_desc_names[i], "shell_exec") == 0) {
      ASSERT_STR_EQ(p->tool_desc_values[i], "Custom shell_exec desc");
      found_shell_exec = 1;
    }
  }
  ASSERT(found_file_edit);
  ASSERT(found_shell_exec);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_apply_profile
 * Applies a loaded profile to a config and verifies the overlay logic.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_apply_profile(void) {
  char *dir = make_test_dir();
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  write_toml(models_dir, "local.toml",
             "match = \"localmodel\"\n"
             "chars_per_token = 4.0\n"
             "system_prompt_extra = \"Model rules here.\"\n"
             "\n"
             "[thinking]\n"
             "mode = \"on\"\n"
             "budget = 8192\n"
             "\n"
             "[client]\n"
             "temperature = 0.5\n"
             "max_tokens = 12288\n"
             "\n"
             "[react]\n"
             "max_react_steps = 30\n"
             "tool_retry_limit = 2\n"
             "cycling_detection = true\n"
             "enable_scoring = false\n"
             "enable_pruning = false\n"
             "\n"
             "[memory]\n"
             "recall_min_score = 0.28\n"
             "memory_index_max = 40\n");

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  /* Record original defaults */
  float orig_temp = cfg->temperature;
  int orig_max_tokens = cfg->max_tokens;
  int orig_max_steps = cfg->max_react_steps;
  int orig_retry = cfg->tool_retry_limit;
  double orig_recall_min = cfg->recall_min_score;
  int orig_index_max = cfg->memory_index_max;

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);

  const model_profile_t *p = config_match_model(cfg, "localmodel-9b");
  ASSERT_NOT_NULL(p);

  /* Apply profile */
  config_apply_profile(cfg, p);

  /* Verify overridden values */
  ASSERT_FLOAT_EQ(cfg->temperature, 0.5f, 0.01f);
  ASSERT_EQ(cfg->max_tokens, 12288);
  ASSERT_EQ(cfg->max_react_steps, 30);
  ASSERT_EQ(cfg->tool_retry_limit, 2);
  ASSERT_DBL_EQ(cfg->recall_min_score, 0.28, 0.01);
  ASSERT_EQ(cfg->memory_index_max, 40);
  ASSERT_EQ(cfg->cycling_detection, 1);
  ASSERT_STR_CONTAINS(cfg->system_prompt_extra, "Model rules here");
  ASSERT_EQ(cfg->thinking.mode, THINKING_ON);
  ASSERT_EQ(cfg->thinking.budget, 8192);

  /* Verify react flags stored on cfg */
  ASSERT_EQ(cfg->profile_enable_scoring, 0);
  ASSERT_EQ(cfg->profile_enable_pruning, 0);
  /* Unset flags should remain -1 (sentinel) */
  ASSERT_EQ(cfg->profile_inject_memory, -1);
  ASSERT_EQ(cfg->profile_inject_prev_result, -1);
  ASSERT_EQ(cfg->profile_enable_reflection, -1);
  ASSERT_EQ(cfg->profile_enable_compaction, -1);

  /* Verify values actually changed from defaults */
  ASSERT(cfg->temperature != orig_temp || orig_temp == 0.5f);
  (void)orig_max_tokens;
  (void)orig_max_steps;
  (void)orig_retry;
  (void)orig_recall_min;
  (void)orig_index_max;

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_apply_profile_sentinels
 * Verifies that sentinel values (inherit-from-below) don't override.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_apply_profile_sentinels(void) {
  char *dir = make_test_dir();
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  /* Profile with minimal overrides — most fields at sentinel */
  write_toml(models_dir, "cloud.toml",
             "match = \"cloud-model\"\n"
             "chars_per_token = 3.5\n"
             "\n"
             "[thinking]\n"
             "mode = \"yes\"\n"
             "budget = -1\n");

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  /* Record defaults */
  float orig_temp = cfg->temperature;
  int orig_max_tokens = cfg->max_tokens;
  int orig_max_steps = cfg->max_react_steps;
  int orig_retry = cfg->tool_retry_limit;
  double orig_recall_min = cfg->recall_min_score;
  int orig_index_max = cfg->memory_index_max;

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);

  const model_profile_t *p = config_match_model(cfg, "cloud-model-v2");
  ASSERT_NOT_NULL(p);

  config_apply_profile(cfg, p);

  /* Verify: non-overridden fields should keep their defaults */
  ASSERT_FLOAT_EQ(cfg->temperature, orig_temp, 0.01f);
  ASSERT_EQ(cfg->max_tokens, orig_max_tokens);
  ASSERT_EQ(cfg->max_react_steps, orig_max_steps);
  ASSERT_EQ(cfg->tool_retry_limit, orig_retry);
  ASSERT_DBL_EQ(cfg->recall_min_score, orig_recall_min, 0.001);
  ASSERT_EQ(cfg->memory_index_max, orig_index_max);

  /* All react flags should be -1 (not set by profile) */
  ASSERT_EQ(cfg->profile_inject_memory, -1);
  ASSERT_EQ(cfg->profile_inject_prev_result, -1);
  ASSERT_EQ(cfg->profile_enable_reflection, -1);
  ASSERT_EQ(cfg->profile_enable_pruning, -1);
  ASSERT_EQ(cfg->profile_enable_compaction, -1);
  ASSERT_EQ(cfg->profile_enable_scoring, -1);

  /* No tool filter */
  ASSERT_NULL(cfg->profile_tools_allow);
  ASSERT_NULL(cfg->profile_tools_block);
  ASSERT_EQ(cfg->n_profile_tools_allow, 0);
  ASSERT_EQ(cfg->n_profile_tools_block, 0);

  /* But chars_per_token and thinking SHOULD be applied */
  ASSERT_EQ(cfg->thinking.mode, THINKING_ON);
  ASSERT_EQ(cfg->thinking.budget, -1);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_match_model_longest
 * Tests that config_match_model uses longest-match priority.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_match_model_longest(void) {
  char *dir = make_test_dir();
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  /* Two profiles: "qwen" (short) and "qwen3-30b" (long) */
  write_toml(models_dir, "qwen.toml",
             "match = \"qwen\"\n"
             "chars_per_token = 4.0\n");

  write_toml(models_dir, "qwen3-30b.toml",
             "match = \"qwen3-30b\"\n"
             "chars_per_token = 3.8\n");

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(cfg->n_model_profiles, 2);

  /* "qwen3-30b-a3b" should match "qwen3-30b" (longer), not "qwen" */
  const model_profile_t *p = config_match_model(cfg, "qwen3-30b-a3b");
  ASSERT_NOT_NULL(p);
  ASSERT_STR_EQ(p->match, "qwen3-30b");
  ASSERT_FLOAT_EQ(p->chars_per_token, 3.8f, 0.01f);

  /* "qwen3-7b" should match "qwen" (only match) */
  p = config_match_model(cfg, "qwen3-7b");
  ASSERT_NOT_NULL(p);
  ASSERT_STR_EQ(p->match, "qwen");

  /* No match */
  p = config_match_model(cfg, "llama-3.1-8b");
  ASSERT_NULL(p);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_spec_dump
 * Verifies config_dump_spec produces valid output with expected sections.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_spec_dump(void) {
  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  /* Set some identifiable values */
  free(cfg->provider.type);
  cfg->provider.type = strdup("local");
  free(cfg->provider.model_id);
  cfg->provider.model_id = strdup("test-model-7b");
  cfg->temperature = 0.42f;
  cfg->max_tokens = 9999;
  cfg->max_react_steps = 15;

  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/spec.toml", dir);

  FILE *out = fopen(spec_path, "w");
  ASSERT_NOT_NULL(out);
  config_dump_spec(cfg, out, "test-profile.toml");
  fclose(out);

  /* Read back and verify content */
  FILE *in = fopen(spec_path, "r");
  ASSERT_NOT_NULL(in);
  char buf[32768];
  size_t n = fread(buf, 1, sizeof(buf) - 1, in);
  buf[n] = '\0';
  fclose(in);

  /* Check sections exist */
  ASSERT_STR_CONTAINS(buf, "[provider]");
  ASSERT_STR_CONTAINS(buf, "[client]");
  ASSERT_STR_CONTAINS(buf, "[thinking]");
  ASSERT_STR_CONTAINS(buf, "[react]");
  ASSERT_STR_CONTAINS(buf, "[memory]");
  ASSERT_STR_CONTAINS(buf, "[tools]");
  ASSERT_STR_CONTAINS(buf, "[embedding]");
  ASSERT_STR_CONTAINS(buf, "[limits]");
  ASSERT_STR_CONTAINS(buf, "[memory_belief_entropy]");

  /* Check specific values */
  ASSERT_STR_CONTAINS(buf, "test-model-7b");
  ASSERT_STR_CONTAINS(buf, "type = \"local\"");
  ASSERT_STR_CONTAINS(buf, "max_tokens = 9999");
  ASSERT_STR_CONTAINS(buf, "max_react_steps = 15");

  /* Check header */
  ASSERT_STR_CONTAINS(buf, "Nash Spec (fully resolved)");
  ASSERT_STR_CONTAINS(buf, "test-profile.toml");

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_spec_roundtrip
 * Dumps spec, loads it back via config_load_spec_overlay, verifies values.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_spec_roundtrip(void) {
  config_t *cfg1 = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg1);

  /* Set distinctive values */
  cfg1->temperature = 0.42f;
  cfg1->max_tokens = 7777;
  cfg1->max_react_steps = 20;
  cfg1->tool_retry_limit = 5;
  cfg1->recall_min_score = 0.35;
  cfg1->memory_index_max = 42;
  cfg1->cycling_detection = 1;
  cfg1->thinking.mode = THINKING_ON;
  cfg1->thinking.budget = 4096;
  cfg1->shell_timeout = 120;
  cfg1->web_timeout = 15;

  /* Dump to file */
  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/roundtrip.toml", dir);

  FILE *out = fopen(spec_path, "w");
  ASSERT_NOT_NULL(out);
  config_dump_spec(cfg1, out, NULL);
  fclose(out);

  /* Load a fresh config and overlay the spec */
  config_t *cfg2 = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg2);

  int rc = config_load_spec_overlay(cfg2, spec_path);
  ASSERT_EQ(rc, 0);

  /* Verify values round-tripped */
  ASSERT_FLOAT_EQ(cfg2->temperature, 0.42f, 0.05f);
  ASSERT_EQ(cfg2->max_tokens, 7777);
  ASSERT_EQ(cfg2->max_react_steps, 20);
  ASSERT_EQ(cfg2->tool_retry_limit, 5);
  ASSERT_DBL_EQ(cfg2->recall_min_score, 0.35, 0.01);
  ASSERT_EQ(cfg2->memory_index_max, 42);
  ASSERT_EQ(cfg2->cycling_detection, 1);
  ASSERT_EQ(cfg2->thinking.mode, THINKING_ON);
  ASSERT_EQ(cfg2->thinking.budget, 4096);
  ASSERT_EQ(cfg2->shell_timeout, 120);
  ASSERT_EQ(cfg2->web_timeout, 15);

  config_free(cfg1);
  config_free(cfg2);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_load_spec_overlay_tools
 * Tests loading a spec overlay with tools allow/block and description
 * overrides.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_load_spec_overlay_tools(void) {
  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/tools_spec.toml", dir);

  /* Write a spec with tool overrides */
  FILE *f = fopen(spec_path, "w");
  ASSERT_NOT_NULL(f);
  fprintf(f,
          "[tools]\n"
          "block = [\"web_search\", \"web_fetch\"]\n"
          "allow = [\"file_read\", \"file_write\", \"shell_exec\"]\n"
          "\n"
          "[tools.shell_exec]\n"
          "description = \"Run a simple command\"\n");
  fclose(f);

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_spec_overlay(cfg, spec_path);
  ASSERT_EQ(rc, 0);

  /* Check block list */
  ASSERT_EQ(cfg->n_profile_tools_block, 2);
  ASSERT_NOT_NULL(cfg->profile_tools_block);
  ASSERT_STR_EQ(cfg->profile_tools_block[0], "web_search");
  ASSERT_STR_EQ(cfg->profile_tools_block[1], "web_fetch");

  /* Check allow list */
  ASSERT_EQ(cfg->n_profile_tools_allow, 3);
  ASSERT_NOT_NULL(cfg->profile_tools_allow);
  ASSERT_STR_EQ(cfg->profile_tools_allow[0], "file_read");
  ASSERT_STR_EQ(cfg->profile_tools_allow[1], "file_write");
  ASSERT_STR_EQ(cfg->profile_tools_allow[2], "shell_exec");

  /* Check description override */
  ASSERT_EQ(cfg->n_profile_tool_descs, 1);
  ASSERT_NOT_NULL(cfg->profile_tool_desc_names);
  ASSERT_STR_EQ(cfg->profile_tool_desc_names[0], "shell_exec");
  ASSERT_STR_EQ(cfg->profile_tool_desc_values[0], "Run a simple command");

  /* Clean up the spec-allocated arrays (since config_free won't know about them) */
  for (int i = 0; i < cfg->n_profile_tools_allow; i++)
    free(cfg->profile_tools_allow[i]);
  free(cfg->profile_tools_allow);
  cfg->profile_tools_allow = NULL;
  for (int i = 0; i < cfg->n_profile_tools_block; i++)
    free(cfg->profile_tools_block[i]);
  free(cfg->profile_tools_block);
  cfg->profile_tools_block = NULL;
  for (int i = 0; i < cfg->n_profile_tool_descs; i++) {
    free(cfg->profile_tool_desc_names[i]);
    free(cfg->profile_tool_desc_values[i]);
  }
  free(cfg->profile_tool_desc_names);
  free(cfg->profile_tool_desc_values);
  cfg->profile_tool_desc_names = NULL;
  cfg->profile_tool_desc_values = NULL;
  cfg->n_profile_tool_descs = 0;

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_load_spec_overlay_react_flags
 * Tests that react boolean flags round-trip through spec overlay.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_load_spec_overlay_react_flags(void) {
  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/flags_spec.toml", dir);

  FILE *f = fopen(spec_path, "w");
  ASSERT_NOT_NULL(f);
  fprintf(f,
          "[react]\n"
          "inject_memory = false\n"
          "inject_prev_result = true\n"
          "enable_reflection = false\n"
          "enable_pruning = true\n"
          "enable_compaction = false\n"
          "enable_scoring = true\n"
          "max_react_steps = 12\n"
          "tool_retry_limit = 1\n"
          "cycling_detection = true\n");
  fclose(f);

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_spec_overlay(cfg, spec_path);
  ASSERT_EQ(rc, 0);

  /* Check react flags */
  ASSERT_EQ(cfg->profile_inject_memory, 0);
  ASSERT_EQ(cfg->profile_inject_prev_result, 1);
  ASSERT_EQ(cfg->profile_enable_reflection, 0);
  ASSERT_EQ(cfg->profile_enable_pruning, 1);
  ASSERT_EQ(cfg->profile_enable_compaction, 0);
  ASSERT_EQ(cfg->profile_enable_scoring, 1);

  /* Check react limits */
  ASSERT_EQ(cfg->max_react_steps, 12);
  ASSERT_EQ(cfg->tool_retry_limit, 1);
  ASSERT_EQ(cfg->cycling_detection, 1);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_load_spec_overlay_partial
 * Tests that loading a partial spec only changes specified fields.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_load_spec_overlay_partial(void) {
  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/partial.toml", dir);

  /* Only change temperature */
  FILE *f = fopen(spec_path, "w");
  ASSERT_NOT_NULL(f);
  fprintf(f, "[client]\ntemperature = 0.99\n");
  fclose(f);

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  /* Record all defaults */
  int orig_max_tokens = cfg->max_tokens;
  int orig_max_steps = cfg->max_react_steps;
  int orig_retry = cfg->tool_retry_limit;
  double orig_recall = cfg->recall_min_score;
  int orig_index = cfg->memory_index_max;
  int orig_shell = cfg->shell_timeout;

  int rc = config_load_spec_overlay(cfg, spec_path);
  ASSERT_EQ(rc, 0);

  /* Only temperature should change */
  ASSERT_FLOAT_EQ(cfg->temperature, 0.99f, 0.01f);

  /* Everything else unchanged */
  ASSERT_EQ(cfg->max_tokens, orig_max_tokens);
  ASSERT_EQ(cfg->max_react_steps, orig_max_steps);
  ASSERT_EQ(cfg->tool_retry_limit, orig_retry);
  ASSERT_DBL_EQ(cfg->recall_min_score, orig_recall, 0.001);
  ASSERT_EQ(cfg->memory_index_max, orig_index);
  ASSERT_EQ(cfg->shell_timeout, orig_shell);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_load_spec_overlay_invalid
 * Tests error handling for bad spec files.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_load_spec_overlay_invalid(void) {
  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  /* Non-existent file */
  int rc = config_load_spec_overlay(cfg, "/tmp/nash_test_absolutely_nonexistent_spec.toml");
  ASSERT_EQ(rc, -1);

  /* Invalid TOML */
  char *dir = make_test_dir();
  char bad_path[4096];
  snprintf(bad_path, sizeof(bad_path), "%s/bad.toml", dir);
  FILE *f = fopen(bad_path, "w");
  fprintf(f, "this is not valid toml {{{\n");
  fclose(f);

  rc = config_load_spec_overlay(cfg, bad_path);
  ASSERT_EQ(rc, -1);

  /* NULL args */
  rc = config_load_spec_overlay(NULL, bad_path);
  ASSERT_EQ(rc, -1);
  rc = config_load_spec_overlay(cfg, NULL);
  ASSERT_EQ(rc, -1);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_profile_vscore_sentinel
 * Verifies the special -2.0 sentinel for vscore_exponent (since 0.0
 * and -1.0 are valid values).
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_profile_vscore_sentinel(void) {
  char *dir = make_test_dir();
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  /* Profile without vscore_exponent → should inherit */
  write_toml(models_dir, "a.toml",
             "match = \"noval\"\n");

  /* Profile with vscore_exponent = 0.0 → valid, should apply */
  write_toml(models_dir, "b.toml",
             "match = \"zeroval\"\n"
             "[memory]\n"
             "vscore_exponent = 0.0\n");

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);
  cfg->vscore_exponent = 0.3f; /* default */

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);

  /* Profile "noval" has no vscore_exponent → sentinel -2.0 → no override */
  const model_profile_t *p1 = config_match_model(cfg, "noval-model");
  ASSERT_NOT_NULL(p1);
  ASSERT_FLOAT_EQ(p1->vscore_exponent, -2.0f, 0.01f); /* sentinel */
  config_apply_profile(cfg, p1);
  ASSERT_FLOAT_EQ(cfg->vscore_exponent, 0.3f, 0.01f); /* unchanged */

  /* Profile "zeroval" sets vscore_exponent = 0.0 → should apply */
  const model_profile_t *p2 = config_match_model(cfg, "zeroval-model");
  ASSERT_NOT_NULL(p2);
  ASSERT_FLOAT_EQ(p2->vscore_exponent, 0.0f, 0.01f);
  config_apply_profile(cfg, p2);
  ASSERT_FLOAT_EQ(cfg->vscore_exponent, 0.0f, 0.01f); /* applied! */

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_profile_tools_allow
 * Verifies tools allow (whitelist) parsing.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_profile_tools_allow(void) {
  char *dir = make_test_dir();
  char models_dir[4096];
  snprintf(models_dir, sizeof(models_dir), "%s/models", dir);
  mkdir(models_dir, 0755);

  write_toml(models_dir, "restricted.toml",
             "match = \"restricted\"\n"
             "[tools]\n"
             "allow = [\"file_read\", \"file_write\", \"done\"]\n");

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_model_profiles(cfg, models_dir);
  ASSERT_EQ(rc, 0);

  const model_profile_t *p = config_match_model(cfg, "restricted-model");
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(p->n_tools_allow, 3);
  ASSERT_STR_EQ(p->tools_allow[0], "file_read");
  ASSERT_STR_EQ(p->tools_allow[1], "file_write");
  ASSERT_STR_EQ(p->tools_allow[2], "done");

  /* Apply and verify on cfg */
  config_apply_profile(cfg, p);
  ASSERT_EQ(cfg->n_profile_tools_allow, 3);
  ASSERT_STR_EQ(cfg->profile_tools_allow[0], "file_read");

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

/* ════════════════════════════════════════════════════════════════════════
 * test_load_spec_overlay_belief_entropy
 * Tests [memory_belief_entropy] section in spec overlay.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_load_spec_overlay_belief_entropy(void) {
  char *dir = make_test_dir();
  char spec_path[4096];
  snprintf(spec_path, sizeof(spec_path), "%s/be.toml", dir);

  FILE *f = fopen(spec_path, "w");
  ASSERT_NOT_NULL(f);
  fprintf(f,
          "[memory_belief_entropy]\n"
          "enabled = true\n"
          "alpha = 2.5\n"
          "probe_tokens = 50\n"
          "probe_n_probs = 20\n"
          "warn_threshold = 0.25\n");
  fclose(f);

  config_t *cfg = config_load("/tmp/nash_test_nonexistent.toml");
  ASSERT_NOT_NULL(cfg);

  int rc = config_load_spec_overlay(cfg, spec_path);
  ASSERT_EQ(rc, 0);

  ASSERT_EQ(cfg->belief_entropy.enabled, 1);
  ASSERT_DBL_EQ(cfg->belief_entropy.alpha, 2.5, 0.01);
  ASSERT_EQ(cfg->belief_entropy.probe_tokens, 50);
  ASSERT_EQ(cfg->belief_entropy.probe_n_probs, 20);
  ASSERT_FLOAT_EQ(cfg->belief_entropy.warn_threshold, 0.25f, 0.01f);

  config_free(cfg);
  rm_rf(dir);
  free(dir);
}

int main(void) {
  printf("test_spec:\n");
  RUN_TEST(test_profile_load_extended);
  RUN_TEST(test_apply_profile);
  RUN_TEST(test_apply_profile_sentinels);
  RUN_TEST(test_match_model_longest);
  RUN_TEST(test_spec_dump);
  RUN_TEST(test_spec_roundtrip);
  RUN_TEST(test_load_spec_overlay_tools);
  RUN_TEST(test_load_spec_overlay_react_flags);
  RUN_TEST(test_load_spec_overlay_partial);
  RUN_TEST(test_load_spec_overlay_invalid);
  RUN_TEST(test_profile_vscore_sentinel);
  RUN_TEST(test_profile_tools_allow);
  RUN_TEST(test_load_spec_overlay_belief_entropy);
  TEST_SUMMARY();
}

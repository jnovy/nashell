#include "config.h"
#include "toml.h"
#include "tools_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

/* Helper: read TOML string, return strdup or NULL */
static char *toml_str(toml_table_t *tbl, const char *key) {
    toml_datum_t d = toml_string_in(tbl, key);
    return d.ok ? d.u.s : NULL;  /* d.u.s is already malloc'd by tomlc99 */
}

static int toml_int(toml_table_t *tbl, const char *key, int def) {
    toml_datum_t d = toml_int_in(tbl, key);
    return d.ok ? (int)d.u.i : def;
}

static double toml_dbl(toml_table_t *tbl, const char *key, double def) {
    toml_datum_t d = toml_double_in(tbl, key);
    return d.ok ? d.u.d : def;
}

static int toml_bl(toml_table_t *tbl, const char *key, int def) {
    toml_datum_t d = toml_bool_in(tbl, key);
    return d.ok ? d.u.b : def;
}

void config_set_defaults(config_t *cfg) {
    if (!cfg->api_base)      cfg->api_base = strdup("http://localhost:8080");
    if (cfg->temperature < 0) cfg->temperature = 0.7f;
    if (cfg->max_tokens == 0)  cfg->max_tokens = 16384;
    /* Replace -1 (sentinel for "not set in config") with actual defaults.
     * -1 comes from TOML parsing when the field is absent.
     * 0 comes from calloc when no config file exists at all.
     * User-specified 0 is valid (e.g., shell_timeout=0 means no limit),
     * so we use -1 as sentinel in TOML parsing and check <= 0 here
     * only for fields where 0 is NOT a valid user value.
     * For timeout/size fields where 0 = "no limit", we check == -1 only. */
    if (cfg->shell_timeout == -1)       cfg->shell_timeout = 300;
    if (cfg->shell_max_output <= 0)     cfg->shell_max_output = 512000;
    if (cfg->file_max_size <= 0)        cfg->file_max_size = 52428800;
    if (cfg->grep_timeout == -1)        cfg->grep_timeout = 60;
    if (cfg->grep_max_matches <= 0)     cfg->grep_max_matches = 50;
    if (cfg->web_timeout == -1)         cfg->web_timeout = 30;
    if (cfg->web_max_size <= 0)         cfg->web_max_size = 512000;
    if (cfg->llm_max_response <= 0)     cfg->llm_max_response = 10485760;
    if (cfg->llm_repeat_threshold <= 0) cfg->llm_repeat_threshold = 100;
    if (cfg->llm_timeout <= 0)          cfg->llm_timeout = 600;
    if (cfg->memory_index_max <= 0)     cfg->memory_index_max = 50;
    if (cfg->max_skills_per_query <= 0) cfg->max_skills_per_query = 2;
    if (cfg->max_lessons_per_query <= 0) cfg->max_lessons_per_query = 2;
    if (cfg->max_strategies_per_query <= 0) cfg->max_strategies_per_query = 1;
    if (cfg->max_antipatterns_per_query <= 0) cfg->max_antipatterns_per_query = 1;
    if (cfg->context_eviction_pct <= 0) cfg->context_eviction_pct = 70;
    if (cfg->max_reflection_steps <= 0) cfg->max_reflection_steps = 4;
    if (cfg->file_read_max_inline <= 0) cfg->file_read_max_inline = 50000;
    if (cfg->file_read_context_pct <= 0) cfg->file_read_context_pct = 10;
    if (cfg->prune_min_score <= 0)      cfg->prune_min_score = 0.35;
    if (cfg->prune_min_evidence <= 0)   cfg->prune_min_evidence = 3;
    if (cfg->consolidation_threshold <= 0) cfg->consolidation_threshold = 0.82f;
    if (cfg->dedup_threshold <= 0)          cfg->dedup_threshold = 0.90f;
    /* P0: recall_min_score — memories below this composite score are not
     * injected. Formula: composite = relevance × pow(vscore, exponent).
     *
     * Empirically calibrated with vscore_exponent=0.3 against 10 queries:
     *   Score distribution: P50=0.19, P75=0.22, P90=0.27, P95=0.30
     *   t=0.20: avg 21.7/query — too noisy (47, 48, 50 for broad queries)
     *   t=0.25: avg 6.8/query — good signal/noise, per-type limits cap it
     *   t=0.28: avg 4.1/query — good but starts losing some relevant hits
     *   t=0.30: avg 2.7/query — too aggressive (0 for "debug a segfault")
     *
     * With vscore_exponent=0.3, new memories (vscore=0.5) get ×0.81,
     * so a good semantic match (rel=0.35) → composite=0.28 — passes 0.25.
     * This was impossible with exponent=1.0 (same match → composite=0.175). */
    if (cfg->recall_min_score <= 0)     cfg->recall_min_score = 0.25;
    if (cfg->error_recall_min_length <= 0)    cfg->error_recall_min_length = 10;
    if (cfg->error_recall_candidates <= 0)    cfg->error_recall_candidates = 3;
    if (cfg->error_recall_max_inject <= 0)    cfg->error_recall_max_inject = 1;
    if (cfg->error_recall_min_relevance <= 0) cfg->error_recall_min_relevance = 0.25;
    if (cfg->dream_reminder_threshold <= 0)   cfg->dream_reminder_threshold = 50;
    /* reflection_gate: default to user_ask (0) — only reflect when the model
     * needed to ask the user, indicating genuine learning opportunity.
     * -1 = not set (sentinel), 0 = user_ask, 1 = always, 2 = never */
    if (cfg->reflection_gate < 0)             cfg->reflection_gate = 0;

    /* P3: Self-Harness tunable surfaces — see config.h for descriptions */
    if (cfg->recall_blend_semantic <= 0)  cfg->recall_blend_semantic = 0.7f;
    if (cfg->recall_blend_substring <= 0) cfg->recall_blend_substring = 0.3f;
    /* vscore_exponent: 0 is a valid value (disables vscore), so use sentinel -1.
     * Default 0.3 — empirically calibrated to reduce cold-start penalty:
     *   86% of memories have vscore=0.5 (zero evidence). With exponent=1.0,
     *   their composite scores are halved; with 0.3, they get ×0.81 — a
     *   19% penalty instead of 50%. Preserves downward signal for memories
     *   with actual misses (vscore=0.33 → ×0.72). */
    if (cfg->vscore_exponent < 0)         cfg->vscore_exponent = 0.3f;
    if (cfg->tool_retry_limit <= 0)       cfg->tool_retry_limit = 3;
    /* checkpoint_frequency: 0 = every step (default), so no sentinel needed */
    /* max_react_steps: -1 sentinel from TOML parsing means "not set".
     * 0 = unlimited (valid user value), so only replace negative sentinels. */
    if (cfg->max_react_steps < 0)         cfg->max_react_steps = 0;


    /* [memory_belief_entropy] defaults — sentinel-guarded like other sections.
     * calloc gives 0/NULL; TOML parsing sets actual values.  Only apply
     * defaults when the field is still at its calloc zero/NULL sentinel.
     * Bool fields (enabled, eviction_gate): 0 = disabled is the correct
     * default AND the calloc value, so no action needed — TOML can set to 1
     * and it won't be overwritten. */
    if (cfg->belief_entropy.alpha == 0)
        cfg->belief_entropy.alpha = 1.0;
    if (!cfg->belief_entropy.anchor_question)
        cfg->belief_entropy.anchor_question = strdup(
            "Based on current memory, what is our task progress and what information is still needed?");
    if (cfg->belief_entropy.probe_tokens == 0)
        cfg->belief_entropy.probe_tokens = 30;
    if (cfg->belief_entropy.probe_n_probs == 0)
        cfg->belief_entropy.probe_n_probs = 10;
    if (cfg->belief_entropy.probe_temperature == 0)
        cfg->belief_entropy.probe_temperature = 0.6f;
    if (cfg->belief_entropy.best_of_n_summaries == 0)
        cfg->belief_entropy.best_of_n_summaries = 1;
    if (cfg->belief_entropy.warn_threshold == 0)
        cfg->belief_entropy.warn_threshold = 0.15f;

    cfg->stream = 1;     /* always on for now */

    /* [thinking] defaults — EDRM is the default mode.
     * Float fields use NAN as sentinel (0.0 is valid for all of them).
     * budget uses INT_MIN as sentinel (0=no-thinking and -1=unrestricted are both valid). */
    if (cfg->thinking.mode == THINKING_OFF && cfg->thinking.probe_tokens == 0
        && isnan(cfg->thinking.tau_vnr) && isnan(cfg->thinking.tau_h)) {
        /* Nothing was set by TOML parsing — default to EDRM */
        cfg->thinking.mode = THINKING_EDRM;
    }
    if (cfg->thinking.probe_tokens == 0)      cfg->thinking.probe_tokens = 30;
    if (cfg->thinking.probe_n_probs == 0)     cfg->thinking.probe_n_probs = 10;
    if (isnan(cfg->thinking.probe_temperature)) cfg->thinking.probe_temperature = 0.6f;
    if (isnan(cfg->thinking.tau_rho))          cfg->thinking.tau_rho = -0.1f;
    if (isnan(cfg->thinking.tau_vnr))          cfg->thinking.tau_vnr = 1.5f;
    if (isnan(cfg->thinking.tau_h))            cfg->thinking.tau_h = 4.0f;
    if (cfg->thinking.budget == INT_MIN)       cfg->thinking.budget = -1; /* -1 = unrestricted */
    if (!cfg->search_engine) cfg->search_engine = strdup("searxng");
    if (!cfg->searxng_url)   cfg->searxng_url = strdup("http://localhost:8888/search");

    /* [workspace] defaults */
    if (cfg->workspace_global_weight <= 0)
        cfg->workspace_global_weight = 0.8;
    /* workspace_global_recall: 0 = not set (calloc), default to 1 (enabled) */
    if (cfg->workspace_global_recall == 0)
        cfg->workspace_global_recall = 1;

    /* [provider] env var fallbacks for Vertex AI / Anthropic.
     * When provider type is "vertex" or "anthropic" and a config field is
     * unset, fall back to well-known environment variables (same ones used
     * by Claude Code / Anthropic SDK). Config always takes priority. */
    if (cfg->provider.type &&
        (strcmp(cfg->provider.type, "vertex") == 0 ||
         strcmp(cfg->provider.type, "anthropic") == 0)) {
        const char *env;
        if (!cfg->provider.region) {
            env = getenv("CLOUD_ML_REGION");
            if (env) cfg->provider.region = strdup(env);
        }
        if (!cfg->provider.project_id) {
            env = getenv("ANTHROPIC_VERTEX_PROJECT_ID");
            if (env) cfg->provider.project_id = strdup(env);
        }
        if (!cfg->provider.model_id) {
            env = getenv("ANTHROPIC_MODEL");
            if (env) cfg->provider.model_id = strdup(env);
        }
    }

    /* [provider] env var fallbacks for OpenAI.
     * OPENAI_API_KEY → api_key_env default, OPENAI_MODEL → model_id. */
    if (cfg->provider.type &&
        strcmp(cfg->provider.type, "openai") == 0) {
        if (!cfg->provider.api_key_env) {
            cfg->provider.api_key_env = strdup("OPENAI_API_KEY");
        }
        if (!cfg->provider.model_id) {
            const char *env = getenv("OPENAI_MODEL");
            if (env) cfg->provider.model_id = strdup(env);
        }
    }
}

config_t *config_load(const char *path) {
    config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;
    cfg->temperature = -1.0f;      /* sentinel: 0.0 is valid (deterministic sampling) */
    cfg->vscore_exponent = -1.0f;   /* sentinel: 0 is valid (disables vscore) */
    /* Unified Spec: initialize profile react_flags sentinels to -1 (inherit) */
    cfg->profile_inject_memory = -1;
    cfg->profile_inject_prev_result = -1;
    cfg->profile_enable_reflection = -1;
    cfg->profile_enable_pruning = -1;
    cfg->profile_enable_compaction = -1;
    cfg->profile_enable_scoring = -1;
    /* Thinking config sentinels: 0.0 is a valid value for these float
     * fields (e.g. probe_temperature=0 means greedy, tau_rho=0 is a valid
     * threshold). Use NAN so config_set_defaults can distinguish "not set"
     * from "explicitly set to 0". budget uses INT_MIN since 0 means
     * "no thinking tokens" (valid) and -1 means "unrestricted" (also valid). */
    cfg->thinking.probe_temperature = NAN;
    cfg->thinking.tau_rho = NAN;
    cfg->thinking.tau_vnr = NAN;
    cfg->thinking.tau_h = NAN;
    cfg->thinking.budget = INT_MIN;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No config file — use defaults */
        config_set_defaults(cfg);
        return cfg;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    if (!root) {
        fprintf(stderr, "[config] parse error: %s\n", errbuf);
        config_set_defaults(cfg);
        return cfg;
    }

    /* [server] */
    toml_table_t *server = toml_table_in(root, "server");
    if (server) {
        cfg->api_base = toml_str(server, "api_base");
    }

    /* [provider] — multi-provider configuration (mirrors nashell config.yaml) */
    toml_table_t *provider = toml_table_in(root, "provider");
    if (provider) {
        cfg->provider.type           = toml_str(provider, "type");
        cfg->provider.model_id       = toml_str(provider, "model_id");
        cfg->provider.api_key_env    = toml_str(provider, "api_key_env");
        cfg->provider.project_id     = toml_str(provider, "project_id");
        cfg->provider.region         = toml_str(provider, "region");
        cfg->provider.context_size   = toml_int(provider, "context_size", 0);
        cfg->provider.chars_per_token = (float)toml_dbl(provider, "chars_per_token", 0);
        cfg->provider.caching        = toml_bl(provider, "caching", 0);
    }

    /* [embedding] — semantic memory matching via vector embeddings */
    toml_table_t *embedding = toml_table_in(root, "embedding");
    if (embedding) {
        cfg->embedding.type       = toml_str(embedding, "type");
        cfg->embedding.model      = toml_str(embedding, "model");
        cfg->embedding.api_base   = toml_str(embedding, "api_base");
        cfg->embedding.model_path = toml_str(embedding, "model_path");
        cfg->embedding.dimension  = toml_int(embedding, "dimension", 0);
        cfg->embedding.max_input_chars = toml_int(embedding, "max_input_chars", 0);
    }

    /* [client] */
    toml_table_t *client = toml_table_in(root, "client");
    if (client) {
        { double d = toml_dbl(client, "temperature", -1);
          if (d >= 0) cfg->temperature = (float)d; }
        cfg->max_tokens  = toml_int(client, "max_tokens", 0);
        cfg->stream      = toml_bl(client, "stream", 1);

        /* Backward compat: old "thinking = true/false" in [client] */
        toml_datum_t old_think = toml_bool_in(client, "thinking");
        if (old_think.ok)
            cfg->thinking.mode = old_think.u.b ? THINKING_ON : THINKING_OFF;
    }

    /* [limits] */
    toml_table_t *limits = toml_table_in(root, "limits");
    if (limits) {
        /* Use -1 as sentinel for "not set in config file" so that
         * user-specified 0 (e.g., shell_timeout=0 for no limit) is preserved.
         * config_set_defaults() replaces -1 with the actual default value. */
        cfg->shell_timeout      = toml_int(limits, "shell_timeout", -1);
        cfg->shell_max_output   = toml_int(limits, "shell_max_output", -1);
        cfg->file_max_size      = toml_int(limits, "file_max_size", -1);
        cfg->grep_timeout       = toml_int(limits, "grep_timeout", -1);
        cfg->grep_max_matches   = toml_int(limits, "grep_max_matches", -1);
        cfg->web_timeout        = toml_int(limits, "web_timeout", -1);
        cfg->web_max_size       = toml_int(limits, "web_max_size", -1);
        cfg->llm_max_response   = toml_int(limits, "llm_max_response", -1);
        cfg->llm_repeat_threshold = toml_int(limits, "llm_repeat_threshold", -1);
        cfg->llm_timeout        = toml_int(limits, "llm_timeout", -1);
        cfg->cycling_detection  = toml_bl(limits, "cycling_detection", 1);
        cfg->scratchpad_max     = toml_int(limits, "scratchpad_max", -1);
        cfg->max_react_steps    = toml_int(limits, "max_react_steps", -1);
        cfg->memory_index_max   = toml_int(limits, "memory_index_max", -1);
        cfg->max_skills_per_query = toml_int(limits, "max_skills_per_query", -1);
        cfg->max_lessons_per_query = toml_int(limits, "max_lessons_per_query", -1);
        cfg->max_strategies_per_query = toml_int(limits, "max_strategies_per_query", -1);
        cfg->max_antipatterns_per_query = toml_int(limits, "max_antipatterns_per_query", -1);
        cfg->context_eviction_pct = toml_int(limits, "context_eviction_pct", -1);
        cfg->max_reflection_steps = toml_int(limits, "max_reflection_steps", -1);
        { char *rg = toml_str(limits, "reflection_gate");
          if (rg) {
              if (strcmp(rg, "always") == 0) cfg->reflection_gate = 1;
              else if (strcmp(rg, "never") == 0) cfg->reflection_gate = 2;
              else cfg->reflection_gate = 0;  /* "user_ask" or unrecognized */
              free(rg);
          }
        }
        cfg->file_read_max_inline = toml_int(limits, "file_read_max_inline", -1);
        cfg->file_read_context_pct = toml_int(limits, "file_read_context_pct", -1);
        cfg->prune_min_score    = toml_dbl(limits, "prune_min_score", 0);
        cfg->prune_min_evidence = toml_int(limits, "prune_min_evidence", -1);
        cfg->consolidation_threshold = (float)toml_dbl(limits, "consolidation_threshold", 0);
        cfg->dedup_threshold = (float)toml_dbl(limits, "dedup_threshold", 0);
        cfg->recall_min_score   = toml_dbl(limits, "recall_min_score", 0);
        cfg->error_recall_min_length    = toml_int(limits, "error_recall_min_length", -1);
        cfg->error_recall_candidates    = toml_int(limits, "error_recall_candidates", -1);
        cfg->error_recall_max_inject    = toml_int(limits, "error_recall_max_inject", -1);
        cfg->error_recall_min_relevance = toml_dbl(limits, "error_recall_min_relevance", 0);
        cfg->dream_reminder_threshold  = toml_int(limits, "dream_reminder_threshold", -1);
        /* Backward compat: old "auto_dream_writes" in [limits] */
        if (cfg->dream_reminder_threshold <= 0)
            cfg->dream_reminder_threshold = toml_int(limits, "auto_dream_writes", -1);

        /* P3: Self-Harness tunable surfaces */
        cfg->recall_blend_semantic  = (float)toml_dbl(limits, "recall_blend_semantic", 0);
        cfg->recall_blend_substring = (float)toml_dbl(limits, "recall_blend_substring", 0);
        { double v = toml_dbl(limits, "vscore_exponent", -1);
          if (v >= 0) cfg->vscore_exponent = (float)v; }
        cfg->tool_retry_limit       = toml_int(limits, "tool_retry_limit", -1);
        cfg->checkpoint_frequency   = toml_int(limits, "checkpoint_frequency", 0);
    }

    /* [paths] */
    toml_table_t *paths = toml_table_in(root, "paths");
    if (paths) {
        cfg->data_dir = toml_str(paths, "data_dir");
    }

    /* [search] */
    toml_table_t *search = toml_table_in(root, "search");
    if (search) {
        cfg->search_engine = toml_str(search, "engine");
        cfg->searxng_url   = toml_str(search, "searxng_url");
    }

    /* [workspace] — memory segregation */
    toml_table_t *ws_tbl = toml_table_in(root, "workspace");
    if (ws_tbl) {
        cfg->workspace = toml_str(ws_tbl, "active");
        cfg->workspace_global_recall = toml_int(ws_tbl, "global_recall", 0);
        cfg->workspace_global_weight = toml_dbl(ws_tbl, "global_recall_weight", 0);
        cfg->workspace_isolated = toml_int(ws_tbl, "isolated", 0);
    }

    /* [telegram] — native Telegram Bot bridge */
    toml_table_t *telegram = toml_table_in(root, "telegram");
    if (telegram) {
        cfg->telegram_bot_token = toml_str(telegram, "bot_token");
        toml_datum_t d = toml_int_in(telegram, "chat_id");
        if (d.ok) cfg->telegram_chat_id = (long long)d.u.i;
    }

    /* [thinking] — overrides old [client].thinking if both present */
    toml_table_t *thinking = toml_table_in(root, "thinking");
    if (thinking) {
        cfg->thinking_explicit = 1;  /* model profiles won't override */
        char *mode_str = toml_str(thinking, "mode");
        if (mode_str) {
            if (strcmp(mode_str, "yes") == 0 || strcmp(mode_str, "on") == 0)
                cfg->thinking.mode = THINKING_ON;
            else if (strcmp(mode_str, "edrm") == 0)
                cfg->thinking.mode = THINKING_EDRM;
            else  /* "no", "off", or anything else */
                cfg->thinking.mode = THINKING_OFF;
            free(mode_str);
        }
        cfg->thinking.probe_tokens     = toml_int(thinking, "probe_tokens", 0);
        cfg->thinking.probe_n_probs    = toml_int(thinking, "probe_n_probs", 0);
        cfg->thinking.probe_temperature = (float)toml_dbl(thinking, "probe_temperature", NAN);
        cfg->thinking.tau_rho          = (float)toml_dbl(thinking, "tau_rho", NAN);
        cfg->thinking.tau_vnr          = (float)toml_dbl(thinking, "tau_vnr", NAN);
        cfg->thinking.tau_h            = (float)toml_dbl(thinking, "tau_h", NAN);
        cfg->thinking.budget           = toml_int(thinking, "budget", INT_MIN);
    }

    /* [memory_belief_entropy] — Belief Entropy quality signal */
    toml_table_t *be = toml_table_in(root, "memory_belief_entropy");
    if (be) {
        cfg->belief_entropy.enabled         = toml_bl(be, "enabled", 0);
        cfg->belief_entropy.alpha           = toml_dbl(be, "alpha", 0);
        cfg->belief_entropy.anchor_question = toml_str(be, "anchor_question");
        cfg->belief_entropy.probe_tokens    = toml_int(be, "probe_tokens", 0);
        cfg->belief_entropy.probe_n_probs   = toml_int(be, "probe_n_probs", 0);
        cfg->belief_entropy.probe_temperature = (float)toml_dbl(be, "probe_temperature", 0);
        cfg->belief_entropy.eviction_gate   = toml_bl(be, "eviction_gate", 0);
        cfg->belief_entropy.best_of_n_summaries = toml_int(be, "best_of_n_summaries", 0);
        cfg->belief_entropy.warn_threshold  = (float)toml_dbl(be, "warn_threshold", 0);
    }

    toml_free(root);
    config_set_defaults(cfg);
    return cfg;
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    free(cfg->api_base);
    free(cfg->provider.type);
    free(cfg->provider.model_id);
    free(cfg->provider.api_key_env);
    free(cfg->provider.project_id);
    free(cfg->provider.region);
    free(cfg->embedding.type);
    free(cfg->embedding.model);
    free(cfg->embedding.api_base);
    free(cfg->embedding.model_path);
    free(cfg->data_dir);
    free(cfg->workspace);
    free(cfg->search_engine);
    free(cfg->searxng_url);
    free(cfg->telegram_bot_token);
    free(cfg->belief_entropy.anchor_question);
    /* Free auto-generated max_tools allow list (owned by cfg, not profile) */
    if (cfg->profile_tools_allow_owned && cfg->profile_tools_allow) {
        for (int i = 0; i < cfg->n_profile_tools_allow; i++)
            free(cfg->profile_tools_allow[i]);
        free(cfg->profile_tools_allow);
        cfg->profile_tools_allow = NULL;
    }
    config_free_model_profiles(cfg);
    free(cfg);
}

/* ── Model profiles ── */

/* Helper: case-insensitive strstr */
static const char *strcasestr_local(const char *haystack, const char *needle) {
    if (!needle[0]) return haystack;
    for (const char *p = haystack; *p; p++) {
        const char *h = p, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return p;
    }
    return NULL;
}

/* Parse a [thinking] subtable from a TOML table */
static void parse_thinking_from_toml(toml_table_t *tbl, thinking_config_t *tc) {
    char *mode_str = toml_str(tbl, "mode");
    if (mode_str) {
        if (strcmp(mode_str, "yes") == 0 || strcmp(mode_str, "on") == 0)
            tc->mode = THINKING_ON;
        else if (strcmp(mode_str, "edrm") == 0)
            tc->mode = THINKING_EDRM;
        else
            tc->mode = THINKING_OFF;
        free(mode_str);
    }
    int b = toml_int(tbl, "budget", INT_MIN);
    if (b != INT_MIN) tc->budget = b;
    int pt = toml_int(tbl, "probe_tokens", 0);
    if (pt > 0) tc->probe_tokens = pt;
}

int config_load_model_profiles(config_t *cfg, const char *models_dir) {
    if (!cfg || !models_dir) return -1;

    DIR *d = opendir(models_dir);
    if (!d) return 0;  /* optional feature — no dir is fine */

    struct dirent *ent;
    int cap = 8;
    cfg->model_profiles = calloc(cap, sizeof(model_profile_t));
    cfg->n_model_profiles = 0;

    while ((ent = readdir(d)) != NULL) {
        /* Filter for *.toml files */
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 6 || strcmp(name + nlen - 5, ".toml") != 0)
            continue;

        /* Build full path */
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", models_dir, name);

        FILE *f = fopen(filepath, "r");
        if (!f) continue;

        char errbuf[256];
        toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
        fclose(f);
        if (!root) {
            fprintf(stderr, "[model-profile] parse error in %s: %s\n", name, errbuf);
            continue;
        }

        /* match field is required */
        char *match = toml_str(root, "match");
        if (!match || !match[0]) {
            fprintf(stderr, "[model-profile] %s: missing 'match' field, skipping\n", name);
            free(match);
            toml_free(root);
            continue;
        }

        /* Grow array if needed */
        if (cfg->n_model_profiles >= cap) {
            cap *= 2;
            cfg->model_profiles = realloc(cfg->model_profiles,
                                          cap * sizeof(model_profile_t));
        }

        model_profile_t *p = &cfg->model_profiles[cfg->n_model_profiles];
        memset(p, 0, sizeof(*p));

        p->match = match;
        p->match_len = (int)strlen(match);
        p->source_file = strdup(name);

        /* Optional fields */
        p->chars_per_token = (float)toml_dbl(root, "chars_per_token", 0);
        p->native_context  = toml_int(root, "native_context", 0);
        p->system_prompt_extra = toml_str(root, "system_prompt_extra");

        /* [thinking] subtable */
        p->thinking.mode = THINKING_UNSET;  /* sentinel: defer */
        p->thinking.budget = INT_MIN;       /* sentinel: inherit */
        toml_table_t *think_tbl = toml_table_in(root, "thinking");
        if (think_tbl) {
            parse_thinking_from_toml(think_tbl, &p->thinking);
        }

        /* ── Unified Spec: extended profile fields ── */

        /* Initialize sentinel values for boolean overrides */
        p->inject_memory = -1;
        p->inject_prev_result = -1;
        p->enable_reflection = -1;
        p->enable_pruning = -1;
        p->enable_compaction = -1;
        p->enable_scoring = -1;
        p->cycling_detection = -1;
        p->temperature = -1.0f;      /* -1 = inherit (0.0 is valid: deterministic) */
        p->vscore_exponent = -2.0f;  /* -2 = inherit (0 and -1 are valid) */

        /* [client] subtable */
        toml_table_t *client_tbl = toml_table_in(root, "client");
        if (client_tbl) {
            { double d = toml_dbl(client_tbl, "temperature", -1);
              if (d >= 0) p->temperature = (float)d; }
            p->max_tokens = toml_int(client_tbl, "max_tokens", 0);
        }

        /* [react] subtable */
        toml_table_t *react_tbl = toml_table_in(root, "react");
        if (react_tbl) {
            p->max_react_steps = toml_int(react_tbl, "max_react_steps", 0);
            p->max_reflection_steps = toml_int(react_tbl, "max_reflection_steps", 0);
            p->tool_retry_limit = toml_int(react_tbl, "tool_retry_limit", 0);
            { toml_datum_t d = toml_bool_in(react_tbl, "cycling_detection");
              if (d.ok) p->cycling_detection = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "inject_memory");
              if (d.ok) p->inject_memory = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "inject_prev_result");
              if (d.ok) p->inject_prev_result = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "enable_reflection");
              if (d.ok) p->enable_reflection = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "enable_pruning");
              if (d.ok) p->enable_pruning = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "enable_compaction");
              if (d.ok) p->enable_compaction = d.u.b ? 1 : 0; }
            { toml_datum_t d = toml_bool_in(react_tbl, "enable_scoring");
              if (d.ok) p->enable_scoring = d.u.b ? 1 : 0; }
        }

        /* [memory] subtable */
        toml_table_t *mem_tbl = toml_table_in(root, "memory");
        if (mem_tbl) {
            p->recall_min_score = toml_dbl(mem_tbl, "recall_min_score", 0);
            p->recall_blend_semantic = (float)toml_dbl(mem_tbl, "recall_blend_semantic", 0);
            p->recall_blend_substring = (float)toml_dbl(mem_tbl, "recall_blend_substring", 0);
            { double v = toml_dbl(mem_tbl, "vscore_exponent", -2);
              if (v > -2) p->vscore_exponent = (float)v; }
            p->memory_index_max = toml_int(mem_tbl, "memory_index_max", 0);
            p->max_skills_per_query = toml_int(mem_tbl, "max_skills_per_query", 0);
            p->max_lessons_per_query = toml_int(mem_tbl, "max_lessons_per_query", 0);
            p->max_strategies_per_query = toml_int(mem_tbl, "max_strategies_per_query", 0);
            p->max_antipatterns_per_query = toml_int(mem_tbl, "max_antipatterns_per_query", 0);
            p->context_eviction_pct = toml_int(mem_tbl, "context_eviction_pct", 0);
        }

        /* [tools] subtable — allow/block arrays + max_tools */
        toml_table_t *tools_tbl = toml_table_in(root, "tools");
        if (tools_tbl) {
            p->max_tools = toml_int(tools_tbl, "max_tools", 0);
            toml_array_t *allow_arr = toml_array_in(tools_tbl, "allow");
            if (allow_arr) {
                int n = toml_array_nelem(allow_arr);
                if (n > 0) {
                    p->tools_allow = calloc(n, sizeof(char *));
                    p->n_tools_allow = n;
                    for (int j = 0; j < n; j++) {
                        toml_datum_t d = toml_string_at(allow_arr, j);
                        p->tools_allow[j] = d.ok ? d.u.s : strdup("");
                    }
                }
            }
            toml_array_t *block_arr = toml_array_in(tools_tbl, "block");
            if (block_arr) {
                int n = toml_array_nelem(block_arr);
                if (n > 0) {
                    p->tools_block = calloc(n, sizeof(char *));
                    p->n_tools_block = n;
                    for (int j = 0; j < n; j++) {
                        toml_datum_t d = toml_string_at(block_arr, j);
                        p->tools_block[j] = d.ok ? d.u.s : strdup("");
                    }
                }
            }
        }

        /* [tools.<name>] subtables — per-tool description overrides.
         * We iterate over all keys in [tools] and check which are sub-tables.
         * Each sub-table must have a "description" key. */
        if (tools_tbl) {
            int ntabs = toml_table_ntab(tools_tbl);
            if (ntabs > 0) {
                p->tool_desc_names = calloc(ntabs, sizeof(char *));
                p->tool_desc_values = calloc(ntabs, sizeof(char *));
                int nd = 0;
                /* toml_key_in indexes over ALL keys (kval+arr+tab).
                 * Total = nkval + narr + ntab. We iterate all and
                 * filter for sub-tables via toml_table_in(). */
                int nkeys = toml_table_nkval(tools_tbl)
                          + toml_table_narr(tools_tbl)
                          + toml_table_ntab(tools_tbl);
                for (int j = 0; j < nkeys; j++) {
                    const char *subkey = toml_key_in(tools_tbl, j);
                    if (!subkey) continue;
                    toml_table_t *sub = toml_table_in(tools_tbl, subkey);
                    if (!sub) continue;
                    char *desc = toml_str(sub, "description");
                    if (desc) {
                        p->tool_desc_names[nd] = strdup(subkey);
                        p->tool_desc_values[nd] = desc;
                        nd++;
                    }
                }
                p->n_tool_descs = nd;
                if (nd == 0) {
                    free(p->tool_desc_names); p->tool_desc_names = NULL;
                    free(p->tool_desc_values); p->tool_desc_values = NULL;
                }
            }
        }

        cfg->n_model_profiles++;
        toml_free(root);
    }
    closedir(d);

    if (cfg->n_model_profiles > 0) {
        fprintf(stderr, "[model-profile] loaded %d profile(s) from %s\n",
                cfg->n_model_profiles, models_dir);
    }
    return 0;
}

const model_profile_t *config_match_model(const config_t *cfg, const char *model_name) {
    if (!cfg || !model_name || cfg->n_model_profiles == 0) return NULL;

    const model_profile_t *best = NULL;
    int best_len = 0;

    for (int i = 0; i < cfg->n_model_profiles; i++) {
        const model_profile_t *p = &cfg->model_profiles[i];
        if (strcasestr_local(model_name, p->match) && p->match_len > best_len) {
            best = p;
            best_len = p->match_len;
        }
    }
    return best;
}

void config_free_model_profiles(config_t *cfg) {
    if (!cfg || !cfg->model_profiles) return;
    for (int i = 0; i < cfg->n_model_profiles; i++) {
        model_profile_t *p = &cfg->model_profiles[i];
        free(p->match);
        free(p->source_file);
        free(p->system_prompt_extra);
        /* Unified Spec: free extended fields */
        for (int j = 0; j < p->n_tools_allow; j++) free(p->tools_allow[j]);
        free(p->tools_allow);
        for (int j = 0; j < p->n_tools_block; j++) free(p->tools_block[j]);
        free(p->tools_block);
        for (int j = 0; j < p->n_tool_descs; j++) {
            free(p->tool_desc_names[j]);
            free(p->tool_desc_values[j]);
        }
        free(p->tool_desc_names);
        free(p->tool_desc_values);
    }
    free(cfg->model_profiles);
    cfg->model_profiles = NULL;
    cfg->n_model_profiles = 0;
}

/* ── Unified Spec: apply model profile as overlay ── */

void config_apply_profile(config_t *cfg, const model_profile_t *p) {
    if (!cfg || !p) return;

    /* chars_per_token: profile overrides default, but explicit [provider] wins */
    if (p->chars_per_token > 0 && cfg->provider.chars_per_token <= 0)
        cfg->provider.chars_per_token = p->chars_per_token;

    /* thinking: profile sets defaults only if config didn't explicitly set them */
    if (!cfg->thinking_explicit) {
        if (p->thinking.mode != THINKING_UNSET)
            cfg->thinking.mode = p->thinking.mode;
    }
    if (p->thinking.budget != INT_MIN)
        cfg->thinking.budget = p->thinking.budget;

    /* system_prompt_extra: store for react.c to use */
    if (p->system_prompt_extra)
        cfg->system_prompt_extra = p->system_prompt_extra;

    /* [client] overrides */
    if (p->temperature >= 0)    cfg->temperature = p->temperature;
    if (p->max_tokens > 0)      cfg->max_tokens = p->max_tokens;

    /* [react] limits overrides */
    if (p->max_react_steps > 0)      cfg->max_react_steps = p->max_react_steps;
    if (p->max_reflection_steps > 0) cfg->max_reflection_steps = p->max_reflection_steps;
    if (p->tool_retry_limit > 0)     cfg->tool_retry_limit = p->tool_retry_limit;
    if (p->cycling_detection >= 0)   cfg->cycling_detection = p->cycling_detection;

    /* [memory] overrides */
    if (p->recall_min_score > 0)       cfg->recall_min_score = p->recall_min_score;
    if (p->recall_blend_semantic > 0)  cfg->recall_blend_semantic = p->recall_blend_semantic;
    if (p->recall_blend_substring > 0) cfg->recall_blend_substring = p->recall_blend_substring;
    if (p->vscore_exponent > -2.0f)    cfg->vscore_exponent = p->vscore_exponent;
    if (p->memory_index_max > 0)       cfg->memory_index_max = p->memory_index_max;
    if (p->max_skills_per_query > 0)   cfg->max_skills_per_query = p->max_skills_per_query;
    if (p->max_lessons_per_query > 0)  cfg->max_lessons_per_query = p->max_lessons_per_query;
    if (p->max_strategies_per_query > 0) cfg->max_strategies_per_query = p->max_strategies_per_query;
    if (p->max_antipatterns_per_query > 0) cfg->max_antipatterns_per_query = p->max_antipatterns_per_query;
    if (p->context_eviction_pct > 0)   cfg->context_eviction_pct = p->context_eviction_pct;

    /* [react] flags → store on cfg for main.c to apply to react_flags_t */
    cfg->profile_inject_memory = p->inject_memory;
    cfg->profile_inject_prev_result = p->inject_prev_result;
    cfg->profile_enable_reflection = p->enable_reflection;
    cfg->profile_enable_pruning = p->enable_pruning;
    cfg->profile_enable_compaction = p->enable_compaction;
    cfg->profile_enable_scoring = p->enable_scoring;

    /* [tools] filter → store on cfg for main.c/react.c to use.
     * These point into the profile's arrays (no copy needed — profile
     * lives as long as cfg). */
    cfg->profile_tools_allow = p->tools_allow;
    cfg->n_profile_tools_allow = p->n_tools_allow;
    cfg->profile_tools_block = p->tools_block;
    cfg->n_profile_tools_block = p->n_tools_block;

    /* max_tools: auto-populate allow list with essential tools when
     * max_tools is set but no explicit allow list is provided.
     * Rationale: 19 tools → ~3700 tokens of FC declarations;
     * 8 tools → ~800 tokens — crucial for small models where
     * grammar overhead is significant. */
    if (p->max_tools > 0 && p->n_tools_allow == 0) {
        /* SMALL_MODEL_TOOLS: the 8 essential tools for small models.
         * Ordered by importance. Includes done (required), notes (scratchpad),
         * and the 6 most commonly used workspace tools. */
        static const char *SMALL_MODEL_TOOLS[] = {
            "shell_exec", "file_read", "file_write", "file_edit",
            "grep_search", "glob_search", "done", "notes"
        };
        static const int N_SMALL_MODEL_TOOLS = 8;

        /* Use the preset if max_tools >= 8, otherwise take the first max_tools */
        int n = p->max_tools < N_SMALL_MODEL_TOOLS ? p->max_tools : N_SMALL_MODEL_TOOLS;
        /* Allocate and copy — these are stack strings, so we need strdup.
         * Allocated on cfg lifetime (freed in config_free). */
        cfg->profile_tools_allow = calloc(n, sizeof(char *));
        if (cfg->profile_tools_allow) {
            cfg->n_profile_tools_allow = n;
            cfg->profile_tools_allow_owned = 1; /* needs separate free */
            for (int i = 0; i < n; i++)
                cfg->profile_tools_allow[i] = strdup(SMALL_MODEL_TOOLS[i]);
        }
    }

    /* [tools.<name>] description overrides */
    cfg->profile_tool_desc_names = p->tool_desc_names;
    cfg->profile_tool_desc_values = p->tool_desc_values;
    cfg->n_profile_tool_descs = p->n_tool_descs;
}

/* ── Unified Spec: dump resolved spec as TOML ── */

void config_dump_spec(const config_t *cfg, FILE *out, const char *profile_file) {
    if (!cfg || !out) return;

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    fprintf(out, "# Nash Spec (fully resolved)\n");
    if (cfg->provider.model_id)
        fprintf(out, "# Model: %s via %s\n",
                cfg->provider.model_id,
                cfg->provider.type ? cfg->provider.type : "local");
    if (profile_file)
        fprintf(out, "# Profile: %s\n", profile_file);
    fprintf(out, "# Generated: %s\n\n", ts);

    fprintf(out, "[provider]\n");
    fprintf(out, "type = \"%s\"\n", cfg->provider.type ? cfg->provider.type : "local");
    if (cfg->provider.model_id)
        fprintf(out, "model_id = \"%s\"\n", cfg->provider.model_id);
    fprintf(out, "context_size = %d\n", cfg->provider.context_size);
    fprintf(out, "chars_per_token = %.1f\n", cfg->provider.chars_per_token > 0
            ? cfg->provider.chars_per_token : 3.5f);
    if (cfg->provider.caching)
        fprintf(out, "caching = true\n");
    fprintf(out, "\n");

    fprintf(out, "[client]\n");
    fprintf(out, "temperature = %.1f\n", cfg->temperature);
    fprintf(out, "max_tokens = %d\n", cfg->max_tokens);
    fprintf(out, "stream = %s\n\n", cfg->stream ? "true" : "false");

    fprintf(out, "[thinking]\n");
    const char *mode_str = cfg->thinking.mode == THINKING_ON ? "yes"
                         : cfg->thinking.mode == THINKING_EDRM ? "edrm" : "no";
    fprintf(out, "mode = \"%s\"\n", mode_str);
    fprintf(out, "budget = %d\n\n", cfg->thinking.budget);

    fprintf(out, "[react]\n");
    fprintf(out, "max_react_steps = %d\n", cfg->max_react_steps);
    fprintf(out, "max_reflection_steps = %d\n", cfg->max_reflection_steps);
    fprintf(out, "reflection_gate = \"%s\"\n",
            cfg->reflection_gate == 2 ? "never" :
            cfg->reflection_gate == 1 ? "always" : "user_ask");
    fprintf(out, "tool_retry_limit = %d\n", cfg->tool_retry_limit);
    fprintf(out, "cycling_detection = %s\n", cfg->cycling_detection ? "true" : "false");
    fprintf(out, "inject_memory = %s\n",
            cfg->profile_inject_memory == 0 ? "false" : "true");
    fprintf(out, "inject_prev_result = %s\n",
            cfg->profile_inject_prev_result == 0 ? "false" : "true");
    fprintf(out, "enable_reflection = %s\n",
            cfg->profile_enable_reflection == 0 ? "false" : "true");
    fprintf(out, "enable_pruning = %s\n",
            cfg->profile_enable_pruning == 0 ? "false" : "true");
    fprintf(out, "enable_compaction = %s\n",
            cfg->profile_enable_compaction == 0 ? "false" : "true");
    fprintf(out, "enable_scoring = %s\n\n",
            cfg->profile_enable_scoring == 0 ? "false" : "true");

    fprintf(out, "[memory]\n");
    fprintf(out, "recall_min_score = %.2f\n", cfg->recall_min_score);
    fprintf(out, "recall_blend_semantic = %.1f\n", cfg->recall_blend_semantic);
    fprintf(out, "recall_blend_substring = %.1f\n", cfg->recall_blend_substring);
    fprintf(out, "vscore_exponent = %.1f\n", cfg->vscore_exponent);
    fprintf(out, "memory_index_max = %d\n", cfg->memory_index_max);
    fprintf(out, "max_skills_per_query = %d\n", cfg->max_skills_per_query);
    fprintf(out, "max_lessons_per_query = %d\n", cfg->max_lessons_per_query);
    fprintf(out, "max_strategies_per_query = %d\n", cfg->max_strategies_per_query);
    fprintf(out, "max_antipatterns_per_query = %d\n", cfg->max_antipatterns_per_query);
    fprintf(out, "context_eviction_pct = %d\n\n", cfg->context_eviction_pct);

    fprintf(out, "[tools]\n");
    if (cfg->n_profile_tools_allow > 0) {
        fprintf(out, "allow = [");
        for (int i = 0; i < cfg->n_profile_tools_allow; i++)
            fprintf(out, "%s\"%s\"", i ? ", " : "",
                    cfg->profile_tools_allow[i]);
        fprintf(out, "]\n");
    }
    if (cfg->n_profile_tools_block > 0) {
        fprintf(out, "block = [");
        for (int i = 0; i < cfg->n_profile_tools_block; i++)
            fprintf(out, "%s\"%s\"", i ? ", " : "",
                    cfg->profile_tools_block[i]);
        fprintf(out, "]\n");
    }
    /* List active tools from the registry */
    fprintf(out, "active = [");
    {
        int first = 1;
        for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
            const char *tname = TOOL_REGISTRY[i].name;
            if (!tname) continue;
            /* Check if blocked */
            int blocked = 0;
            for (int j = 0; j < cfg->n_profile_tools_block; j++) {
                if (strcmp(tname, cfg->profile_tools_block[j]) == 0) {
                    blocked = 1;
                    break;
                }
            }
            if (blocked) continue;
            /* If allow list exists, check if tool is in it */
            if (cfg->n_profile_tools_allow > 0) {
                int allowed = 0;
                for (int j = 0; j < cfg->n_profile_tools_allow; j++) {
                    if (strcmp(tname, cfg->profile_tools_allow[j]) == 0) {
                        allowed = 1;
                        break;
                    }
                }
                if (!allowed) continue;
            }
            fprintf(out, "%s\"%s\"", first ? "" : ", ", tname);
            first = 0;
        }
    }
    fprintf(out, "]\n");
    fprintf(out, "\n");

    /* Tool description overrides */
    for (int i = 0; i < cfg->n_profile_tool_descs; i++) {
        fprintf(out, "[tools.%s]\n", cfg->profile_tool_desc_names[i]);
        /* Use triple-quoted string for multi-line descriptions */
        fprintf(out, "description = \"\"\"\n%s\"\"\"\n\n",
                cfg->profile_tool_desc_values[i]);
    }

    fprintf(out, "[embedding]\n");
    fprintf(out, "type = \"%s\"\n",
            cfg->embedding.type ? cfg->embedding.type : "none");
    if (cfg->embedding.model_path)
        fprintf(out, "model_path = \"%s\"\n", cfg->embedding.model_path);
    if (cfg->embedding.model)
        fprintf(out, "model = \"%s\"\n", cfg->embedding.model);
    fprintf(out, "\n");

    fprintf(out, "[limits]\n");
    fprintf(out, "shell_timeout = %d\n", cfg->shell_timeout);
    fprintf(out, "shell_max_output = %d\n", cfg->shell_max_output);
    fprintf(out, "file_max_size = %d\n", cfg->file_max_size);
    fprintf(out, "grep_timeout = %d\n", cfg->grep_timeout);
    fprintf(out, "grep_max_matches = %d\n", cfg->grep_max_matches);
    fprintf(out, "web_timeout = %d\n", cfg->web_timeout);
    fprintf(out, "web_max_size = %d\n", cfg->web_max_size);
    fprintf(out, "llm_timeout = %d\n", cfg->llm_timeout);
    fprintf(out, "llm_max_response = %d\n", cfg->llm_max_response);
    fprintf(out, "llm_repeat_threshold = %d\n", cfg->llm_repeat_threshold);
    fprintf(out, "file_read_max_inline = %d\n", cfg->file_read_max_inline);
    fprintf(out, "file_read_context_pct = %d\n", cfg->file_read_context_pct);
    fprintf(out, "scratchpad_max = %d\n", cfg->scratchpad_max);
    fprintf(out, "checkpoint_frequency = %d\n", cfg->checkpoint_frequency);
    fprintf(out, "prune_min_score = %.2f\n", cfg->prune_min_score);
    fprintf(out, "prune_min_evidence = %d\n", cfg->prune_min_evidence);
    fprintf(out, "consolidation_threshold = %.2f\n", cfg->consolidation_threshold);
    fprintf(out, "dedup_threshold = %.2f\n", cfg->dedup_threshold);
    fprintf(out, "dream_reminder_threshold = %d\n", cfg->dream_reminder_threshold);
    fprintf(out, "error_recall_min_length = %d\n", cfg->error_recall_min_length);
    fprintf(out, "error_recall_candidates = %d\n", cfg->error_recall_candidates);
    fprintf(out, "error_recall_max_inject = %d\n", cfg->error_recall_max_inject);
    fprintf(out, "error_recall_min_relevance = %.2f\n", cfg->error_recall_min_relevance);
    fprintf(out, "\n");

    fprintf(out, "[memory_belief_entropy]\n");
    fprintf(out, "enabled = %s\n", cfg->belief_entropy.enabled ? "true" : "false");
    fprintf(out, "alpha = %.1f\n", cfg->belief_entropy.alpha);
    fprintf(out, "probe_tokens = %d\n", cfg->belief_entropy.probe_tokens);
    fprintf(out, "probe_n_probs = %d\n", cfg->belief_entropy.probe_n_probs);
    fprintf(out, "probe_temperature = %.1f\n", cfg->belief_entropy.probe_temperature);
    fprintf(out, "eviction_gate = %s\n", cfg->belief_entropy.eviction_gate ? "true" : "false");
    fprintf(out, "best_of_n_summaries = %d\n", cfg->belief_entropy.best_of_n_summaries);
    fprintf(out, "warn_threshold = %.2f\n", cfg->belief_entropy.warn_threshold);
    fprintf(out, "\n");

    if (cfg->system_prompt_extra) {
        fprintf(out, "system_prompt_extra = \"\"\"\n%s\"\"\"\n", cfg->system_prompt_extra);
    }
}

/* ── Unified Spec: dump spec to heap string ── */

char *config_dump_spec_to_string(const config_t *cfg, const char *profile_file) {
    if (!cfg) return NULL;
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem) return NULL;
    config_dump_spec(cfg, mem, profile_file);
    fclose(mem);
    return buf;  /* caller frees */
}

/* ── Unified Spec: load spec as overlay ── */

int config_load_spec_overlay(config_t *cfg, const char *path) {
    if (!cfg || !path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[spec] cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    if (!root) {
        fprintf(stderr, "[spec] parse error in %s: %s\n", path, errbuf);
        return -1;
    }

    /* Validate top-level section names — warn on unknown sections.
     * Typos like [memeory] or [recat] silently have no effect. */
    {
        static const char *valid_sections[] = {
            "provider", "client", "thinking", "react", "memory",
            "tools", "limits", "memory_belief_entropy", NULL
        };
        int n_total = toml_table_nkval(root) + toml_table_narr(root)
                    + toml_table_ntab(root);
        for (int i = 0; i < n_total; i++) {
            const char *key = toml_key_in(root, i);
            if (!key) continue;
            int found = 0;
            for (const char **v = valid_sections; *v; v++) {
                if (strcmp(key, *v) == 0) { found = 1; break; }
            }
            if (!found) {
                fprintf(stderr, "[spec] warning: unknown section [%s] in %s "
                        "(typo?)\n", key, path);
            }
        }
    }

    /* [provider] overlay */
    toml_table_t *provider = toml_table_in(root, "provider");
    if (provider) {
        char *s;
        if ((s = toml_str(provider, "type"))) {
            free(cfg->provider.type); cfg->provider.type = s;
        }
        if ((s = toml_str(provider, "model_id"))) {
            free(cfg->provider.model_id); cfg->provider.model_id = s;
        }
        int v = toml_int(provider, "context_size", 0);
        if (v > 0) cfg->provider.context_size = v;
        double d = toml_dbl(provider, "chars_per_token", 0);
        if (d > 0) cfg->provider.chars_per_token = (float)d;
        { toml_datum_t td = toml_bool_in(provider, "caching");
          if (td.ok) cfg->provider.caching = td.u.b; }
    }

    /* [client] overlay */
    toml_table_t *client = toml_table_in(root, "client");
    if (client) {
        double d = toml_dbl(client, "temperature", -1);
        if (d >= 0) cfg->temperature = (float)d;
        int v = toml_int(client, "max_tokens", 0);
        if (v > 0) cfg->max_tokens = v;
        { toml_datum_t td = toml_bool_in(client, "stream");
          if (td.ok) cfg->stream = td.u.b; }
    }

    /* [thinking] overlay */
    toml_table_t *thinking = toml_table_in(root, "thinking");
    if (thinking) {
        char *mode_str = toml_str(thinking, "mode");
        if (mode_str) {
            if (strcmp(mode_str, "yes") == 0 || strcmp(mode_str, "on") == 0)
                cfg->thinking.mode = THINKING_ON;
            else if (strcmp(mode_str, "edrm") == 0)
                cfg->thinking.mode = THINKING_EDRM;
            else
                cfg->thinking.mode = THINKING_OFF;
            free(mode_str);
        }
        int b = toml_int(thinking, "budget", INT_MIN);
        if (b != INT_MIN) cfg->thinking.budget = b;
    }

    /* [react] overlay — includes flags + limits that live in [react] in spec format */
    toml_table_t *react = toml_table_in(root, "react");
    if (react) {
        int v;
        v = toml_int(react, "max_react_steps", 0);
        if (v > 0) cfg->max_react_steps = v;
        v = toml_int(react, "max_reflection_steps", 0);
        if (v > 0) cfg->max_reflection_steps = v;
        { char *rg = toml_str(react, "reflection_gate");
          if (rg) {
              if (strcmp(rg, "always") == 0) cfg->reflection_gate = 1;
              else if (strcmp(rg, "never") == 0) cfg->reflection_gate = 2;
              else cfg->reflection_gate = 0;
              free(rg);
          }
        }
        v = toml_int(react, "tool_retry_limit", 0);
        if (v > 0) cfg->tool_retry_limit = v;
        { toml_datum_t td = toml_bool_in(react, "cycling_detection");
          if (td.ok) cfg->cycling_detection = td.u.b; }

        /* React boolean flags → stored on cfg->profile_* */
        { toml_datum_t td = toml_bool_in(react, "inject_memory");
          if (td.ok) cfg->profile_inject_memory = td.u.b ? 1 : 0; }
        { toml_datum_t td = toml_bool_in(react, "inject_prev_result");
          if (td.ok) cfg->profile_inject_prev_result = td.u.b ? 1 : 0; }
        { toml_datum_t td = toml_bool_in(react, "enable_reflection");
          if (td.ok) cfg->profile_enable_reflection = td.u.b ? 1 : 0; }
        { toml_datum_t td = toml_bool_in(react, "enable_pruning");
          if (td.ok) cfg->profile_enable_pruning = td.u.b ? 1 : 0; }
        { toml_datum_t td = toml_bool_in(react, "enable_compaction");
          if (td.ok) cfg->profile_enable_compaction = td.u.b ? 1 : 0; }
        { toml_datum_t td = toml_bool_in(react, "enable_scoring");
          if (td.ok) cfg->profile_enable_scoring = td.u.b ? 1 : 0; }
    }

    /* [memory] overlay */
    toml_table_t *mem = toml_table_in(root, "memory");
    if (mem) {
        double d;
        int v;
        d = toml_dbl(mem, "recall_min_score", 0);
        if (d > 0) cfg->recall_min_score = d;
        d = toml_dbl(mem, "recall_blend_semantic", 0);
        if (d > 0) cfg->recall_blend_semantic = (float)d;
        d = toml_dbl(mem, "recall_blend_substring", 0);
        if (d > 0) cfg->recall_blend_substring = (float)d;
        { double vs = toml_dbl(mem, "vscore_exponent", -1);
          if (vs >= 0) cfg->vscore_exponent = (float)vs; }
        v = toml_int(mem, "memory_index_max", 0);
        if (v > 0) cfg->memory_index_max = v;
        v = toml_int(mem, "max_skills_per_query", 0);
        if (v > 0) cfg->max_skills_per_query = v;
        v = toml_int(mem, "max_lessons_per_query", 0);
        if (v > 0) cfg->max_lessons_per_query = v;
        v = toml_int(mem, "max_strategies_per_query", 0);
        if (v > 0) cfg->max_strategies_per_query = v;
        v = toml_int(mem, "max_antipatterns_per_query", 0);
        if (v > 0) cfg->max_antipatterns_per_query = v;
        v = toml_int(mem, "context_eviction_pct", 0);
        if (v > 0) cfg->context_eviction_pct = v;
    }

    /* [tools] overlay — allow/block lists */
    toml_table_t *tools = toml_table_in(root, "tools");
    if (tools) {
        toml_array_t *allow_arr = toml_array_in(tools, "allow");
        if (allow_arr) {
            int n = toml_array_nelem(allow_arr);
            if (n > 0) {
                /* Free any existing allow list */
                for (int i = 0; i < cfg->n_profile_tools_allow; i++)
                    free(cfg->profile_tools_allow[i]);
                free(cfg->profile_tools_allow);
                cfg->profile_tools_allow = calloc(n, sizeof(char *));
                cfg->n_profile_tools_allow = n;
                for (int j = 0; j < n; j++) {
                    toml_datum_t d = toml_string_at(allow_arr, j);
                    cfg->profile_tools_allow[j] = d.ok ? d.u.s : strdup("");
                }
            }
        }
        toml_array_t *block_arr = toml_array_in(tools, "block");
        if (block_arr) {
            int n = toml_array_nelem(block_arr);
            if (n > 0) {
                for (int i = 0; i < cfg->n_profile_tools_block; i++)
                    free(cfg->profile_tools_block[i]);
                free(cfg->profile_tools_block);
                cfg->profile_tools_block = calloc(n, sizeof(char *));
                cfg->n_profile_tools_block = n;
                for (int j = 0; j < n; j++) {
                    toml_datum_t d = toml_string_at(block_arr, j);
                    cfg->profile_tools_block[j] = d.ok ? d.u.s : strdup("");
                }
            }
        }

        /* [tools.<name>] description overrides */
        int ntabs = toml_table_ntab(tools);
        if (ntabs > 0) {
            /* Free existing desc overrides */
            for (int i = 0; i < cfg->n_profile_tool_descs; i++) {
                free(cfg->profile_tool_desc_names[i]);
                free(cfg->profile_tool_desc_values[i]);
            }
            free(cfg->profile_tool_desc_names);
            free(cfg->profile_tool_desc_values);
            cfg->profile_tool_desc_names = calloc(ntabs, sizeof(char *));
            cfg->profile_tool_desc_values = calloc(ntabs, sizeof(char *));
            int nd = 0;
            int nkeys = toml_table_nkval(tools)
                      + toml_table_narr(tools)
                      + toml_table_ntab(tools);
            for (int j = 0; j < nkeys; j++) {
                const char *subkey = toml_key_in(tools, j);
                if (!subkey) continue;
                toml_table_t *sub = toml_table_in(tools, subkey);
                if (!sub) continue;
                char *desc = toml_str(sub, "description");
                if (desc) {
                    cfg->profile_tool_desc_names[nd] = strdup(subkey);
                    cfg->profile_tool_desc_values[nd] = desc;
                    nd++;
                }
            }
            cfg->n_profile_tool_descs = nd;
            if (nd == 0) {
                free(cfg->profile_tool_desc_names);
                cfg->profile_tool_desc_names = NULL;
                free(cfg->profile_tool_desc_values);
                cfg->profile_tool_desc_values = NULL;
            }
        }
    }

    /* [limits] overlay */
    toml_table_t *limits = toml_table_in(root, "limits");
    if (limits) {
        int v;
        v = toml_int(limits, "shell_timeout", -1);
        if (v >= 0) cfg->shell_timeout = v;
        v = toml_int(limits, "shell_max_output", 0);
        if (v > 0) cfg->shell_max_output = v;
        v = toml_int(limits, "file_max_size", 0);
        if (v > 0) cfg->file_max_size = v;
        v = toml_int(limits, "grep_timeout", -1);
        if (v >= 0) cfg->grep_timeout = v;
        v = toml_int(limits, "grep_max_matches", 0);
        if (v > 0) cfg->grep_max_matches = v;
        v = toml_int(limits, "web_timeout", -1);
        if (v >= 0) cfg->web_timeout = v;
        v = toml_int(limits, "web_max_size", 0);
        if (v > 0) cfg->web_max_size = v;
        v = toml_int(limits, "llm_timeout", 0);
        if (v > 0) cfg->llm_timeout = v;
        v = toml_int(limits, "llm_max_response", 0);
        if (v > 0) cfg->llm_max_response = v;
        v = toml_int(limits, "llm_repeat_threshold", 0);
        if (v > 0) cfg->llm_repeat_threshold = v;
        v = toml_int(limits, "file_read_max_inline", 0);
        if (v > 0) cfg->file_read_max_inline = v;
        v = toml_int(limits, "file_read_context_pct", 0);
        if (v > 0) cfg->file_read_context_pct = v;
        v = toml_int(limits, "scratchpad_max", -1);
        if (v >= 0) cfg->scratchpad_max = v;
        v = toml_int(limits, "checkpoint_frequency", -1);
        if (v >= 0) cfg->checkpoint_frequency = v;
        { double d = toml_dbl(limits, "prune_min_score", 0);
          if (d > 0) cfg->prune_min_score = d; }
        v = toml_int(limits, "prune_min_evidence", 0);
        if (v > 0) cfg->prune_min_evidence = v;
        { double d = toml_dbl(limits, "consolidation_threshold", 0);
          if (d > 0) cfg->consolidation_threshold = (float)d; }
        { double d = toml_dbl(limits, "dedup_threshold", 0);
          if (d > 0) cfg->dedup_threshold = (float)d; }
        v = toml_int(limits, "dream_reminder_threshold", 0);
        if (v > 0) cfg->dream_reminder_threshold = v;
        v = toml_int(limits, "error_recall_min_length", 0);
        if (v > 0) cfg->error_recall_min_length = v;
        v = toml_int(limits, "error_recall_candidates", 0);
        if (v > 0) cfg->error_recall_candidates = v;
        v = toml_int(limits, "error_recall_max_inject", 0);
        if (v > 0) cfg->error_recall_max_inject = v;
        { double d = toml_dbl(limits, "error_recall_min_relevance", 0);
          if (d > 0) cfg->error_recall_min_relevance = d; }
    }

    /* [memory_belief_entropy] overlay */
    toml_table_t *be = toml_table_in(root, "memory_belief_entropy");
    if (be) {
        { toml_datum_t td = toml_bool_in(be, "enabled");
          if (td.ok) cfg->belief_entropy.enabled = td.u.b; }
        { double d = toml_dbl(be, "alpha", 0);
          if (d > 0) cfg->belief_entropy.alpha = d; }
        int v;
        v = toml_int(be, "probe_tokens", 0);
        if (v > 0) cfg->belief_entropy.probe_tokens = v;
        v = toml_int(be, "probe_n_probs", 0);
        if (v > 0) cfg->belief_entropy.probe_n_probs = v;
        { double d = toml_dbl(be, "probe_temperature", 0);
          if (d > 0) cfg->belief_entropy.probe_temperature = (float)d; }
        { toml_datum_t td = toml_bool_in(be, "eviction_gate");
          if (td.ok) cfg->belief_entropy.eviction_gate = td.u.b; }
        v = toml_int(be, "best_of_n_summaries", 0);
        if (v > 0) cfg->belief_entropy.best_of_n_summaries = v;
        { double d = toml_dbl(be, "warn_threshold", 0);
          if (d > 0) cfg->belief_entropy.warn_threshold = (float)d; }
    }

    toml_free(root);
    fprintf(stderr, "[spec] loaded overlay from %s\n", path);
    return 0;
}

int config_write_default(const char *path) {
    /* Don't overwrite existing config */
    struct stat st;
    if (stat(path, &st) == 0) return 0;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fputs(
        "# Nash configuration file\n"
        "# Copy to ~/.nash/config.toml and edit as needed\n"
        "\n"
        "[server]\n"
        "api_base = \"http://localhost:8080\"\n"
        "\n"
        "# Provider configuration — choose one:\n"
        "#   local    = llama.cpp or any OpenAI-compatible server (default)\n"
        "#   openai   = OpenAI API (GPT-4o, GPT-5, etc.)\n"
        "#   anthropic = Anthropic API (Claude)\n"
        "#   vertex   = Anthropic via Google Vertex AI\n"
        "# If [provider] is absent, defaults to local using [server].api_base\n"
        "[provider]\n"
        "type = \"local\"\n"
        "# model_id = \"claude-opus-4-6\"       # model identifier for API\n"
        "# api_key_env = \"OPENAI_API_KEY\"     # env var with API key\n"
        "# project_id = \"my-gcp-project\"      # Vertex AI project\n"
        "# region = \"global\"                # Vertex AI region\n"
        "# context_size = 200000              # context window (0 = auto-detect)\n"
        "# chars_per_token = 3.5              # chars per token ratio\n"
        "# caching = false                    # prompt caching (Anthropic)\n"
        "\n"
        "[client]\n"
        "temperature = 0.7\n"
        "max_tokens = 16384\n"
        "stream = true\n"
        "\n"
        "# Thinking mode: \"yes\" = always, \"no\" = never, \"edrm\" = adaptive (default)\n"
        "# EDRM routing based on: \"When Do LLMs Reason? A Dynamical Systems View\n"
        "# via Entropy Phase Transitions\" [arXiv:2605.22873]\n"
        "[thinking]\n"
        "mode = \"edrm\"\n"
        "# probe_tokens = 30          # tokens to generate in entropy probe\n"
        "# probe_n_probs = 10         # top-N logprobs to request\n"
        "# probe_temperature = 0.6    # probe sampling temperature\n"
        "# tau_rho = -0.1             # Spearman correlation threshold\n"
        "# tau_vnr = 1.5              # von Neumann ratio threshold\n"
        "# tau_h = 4.0                # mean entropy threshold\n"
        "# budget = -1                # thinking token budget: -1=unrestricted, 0=none, N>0=max tokens\n"
        "\n"
        "# Semantic embedding for memory matching (GDN-2 inspired)\n"
        "# Enables cosine-similarity scoring instead of substring matching.\n"
        "# Graceful fallback: if backend unavailable, uses substring matching.\n"
        "[embedding]\n"
        "type = \"onnx\"                            # \"onnx\" (local), \"ollama\", \"openai\", \"none\"\n"
        "model_path = \"~/models/all-MiniLM-L6-v2\" # ONNX model directory\n"
        "# model = \"nomic-embed-text\"             # embedding model name (ollama/openai)\n"
        "# api_base = \"http://localhost:11434\"    # API base (ollama/openai)\n"
        "# dimension = 0                          # 0 = auto-detect\n"
        "# max_input_chars = 0                    # 0 = auto from model (e.g. nomic-embed→32K, MiniLM→1K)\n"
        "\n"
        "[limits]\n"
        "# Shell command execution\n"
        "shell_timeout = 300          # max seconds for shell_exec (0 = no limit)\n"
        "shell_max_output = 512000    # max bytes of shell output (500KB)\n"
        "\n"
        "# File operations\n"
        "file_max_size = 52428800     # max file size for file_read (50MB)\n"
        "\n"
        "# Grep search\n"
        "grep_timeout = 60            # max seconds for grep_search\n"
        "grep_max_matches = 50        # max grep results\n"
        "\n"
        "# Web operations\n"
        "web_timeout = 30             # max seconds for web_fetch/web_search\n"
        "web_max_size = 512000        # max bytes for web response (500KB)\n"
        "\n"
        "# LLM streaming safety\n"
        "llm_max_response = 10485760  # max bytes from LLM response (10MB)\n"
        "llm_repeat_threshold = 100   # stop after N consecutive identical tokens\n"
        "llm_timeout = 600            # max seconds per LLM API call (0 = no limit)\n"
        "cycling_detection = true     # detect and refuse repeated identical tool calls\n"
        "\n"
        "# Context management\n"
        "scratchpad_max = 0           # max scratchpad chars (0 = auto: 5%% of context)\n"
        "max_react_steps = 0          # max steps per react loop (0 = unlimited)\n"
        "context_eviction_pct = 70    # context usage %% that triggers message eviction\n"
        "file_read_max_inline = 50000 # max chars returned inline by file_read (50KB)\n"
        "file_read_context_pct = 10   # max %% of context window for file_read inline (0 = use file_read_max_inline)\n"
        "\n"
        "# Memory\n"
        "memory_index_max = 50        # max entries shown in memory index injection\n"
        "max_skills_per_query = 2     # max skill memories loaded per query\n"
        "max_lessons_per_query = 2    # max lesson memories loaded per query\n"
        "max_strategies_per_query = 1 # max strategy memories loaded per query\n"
        "max_antipatterns_per_query = 1 # max anti-pattern memories loaded per query\n"
        "max_reflection_steps = 4     # max LLM steps for post-task reflection\n"
        "reflection_gate = \"user_ask\"  # when to reflect: \"user_ask\" (only after asking user),\n"
        "                              #   \"always\" (after every task), \"never\" (disable)\n"
        "prune_min_score = 0.35       # Bayesian validation score below which memories are prunable\n"
        "prune_min_evidence = 3       # minimum recall count before pruning is considered\n"
        "consolidation_threshold = 0.82 # cosine similarity threshold for near-duplicate consolidation\n"
        "dedup_threshold = 0.90       # cosine similarity threshold for reflection deduplication\n"
        "recall_min_score = 0.25      # P0: min composite score for memory injection (empirically calibrated)\n"
        "\n"
        "# Error-triggered reactive retrieval — when a tool fails, query memory\n"
        "# with the error text to surface relevant lessons/skills.\n"
        "error_recall_min_length = 10  # min error text chars to trigger recall\n"
        "error_recall_candidates = 3   # candidate memories to retrieve\n"
        "error_recall_max_inject = 1   # max memories to inject into context\n"
        "error_recall_min_relevance = 0.25 # min relevance score for injection [0.0-1.0]\n"
        "\n"
        "dream_reminder_threshold = 50 # new entries since last /dream to show status bar reminder (0 = disabled)\n"
        "\n"
        "# Self-Harness tunable surfaces (P3)\n"
        "# These parameters can be automatically tuned by the self-harness loop\n"
        "# and validated via: nash --regression --validate-harness compare\n"
        "recall_blend_semantic = 0.7  # weight for semantic similarity in memory recall (0.0-1.0)\n"
        "recall_blend_substring = 0.3 # weight for substring matching in memory recall (0.0-1.0)\n"
        "vscore_exponent = 0.3        # power-law exponent for validation score (0.0=disabled, 1.0=full)\n"
        "tool_retry_limit = 3         # max consecutive errors on same tool before forced strategy switch\n"
        "checkpoint_frequency = 0     # save checkpoint every N steps (0 = every step)\n"
        "\n"
        "# Belief Entropy — forward-looking memory quality signal.\n"
        "# Based on MMPO [arXiv:2605.30159]: measures how clearly the current\n"
        "# memory induces a confident belief about task state.\n"
        "# Lower entropy = clearer memory, higher entropy = ambiguous/incomplete.\n"
        "[memory_belief_entropy]\n"
        "enabled = false              # enable Belief Entropy monitoring\n"
        "alpha = 1.0                  # weight vs outcome reward (Eq. 6)\n"
        "anchor_question = \\\"Based on current memory, what is our task progress and what information is still needed?\\\"\n"
        "probe_tokens = 30            # tokens to generate in entropy probe\n"
        "probe_n_probs = 10           # top-N logprobs to request\n"
        "probe_temperature = 0.6      # probe sampling temperature\n"
        "eviction_gate = false        # gate context eviction on entropy\n"
        "best_of_n_summaries = 1      # candidates for compression (1 = no selection)\n"
        "warn_threshold = 0.15        # H_BE increase that triggers warning\n"
        "\n"
        "[paths]\n"
        "data_dir = \"\"                # data directory (empty = ~/.nash/)\n"
        "\n"
        "[search]\n"
        "engine = \"searxng\"            # SearXNG (auto-started via podman/docker)\n"
        "searxng_url = \"http://localhost:8888/search\"\n",
        f);

    fclose(f);
    return 0;
}

#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>

/* Nash configuration — loaded from ~/.nash/config.toml */

/* Thinking mode: off=0, on=1, edrm=2
 * EDRM (Entropy Dynamics-based Reasoning Manifold) routing based on:
 *   "When Do LLMs Reason? A Dynamical Systems View via Entropy Phase
 *    Transitions" [arXiv:2605.22873, May 2026]
 * Probes early decoding entropy to decide if CoT reasoning is beneficial. */
#define THINKING_UNSET -1  /* calloc zeros mode to 0; we detect unset via this sentinel */
#define THINKING_OFF   0
#define THINKING_ON    1
#define THINKING_EDRM  2

typedef struct {
    int    mode;              /* THINKING_OFF / THINKING_ON / THINKING_EDRM */
    int    probe_tokens;      /* EDRM: tokens to generate in probe (default 30) */
    int    probe_n_probs;     /* EDRM: top-N logprobs to request (default 10) */
    float  probe_temperature; /* EDRM: probe sampling temperature (default 0.6) */
    float  tau_rho;           /* EDRM: Spearman correlation threshold (default -0.1) */
    float  tau_vnr;           /* EDRM: von Neumann ratio threshold (default 1.5) */
    float  tau_h;             /* EDRM: mean entropy threshold (default 4.0) */
    int    budget;            /* thinking token budget: -1=unrestricted (default),
                               * 0=no thinking, N>0=max thinking tokens.
                               * Maps to llama.cpp --reasoning-budget.
                               * Qwen3 natively supports thinking budgets via
                               * early truncation of the <think> block. */
} thinking_config_t;

/* Provider configuration — mirrors nashell's config.yaml model entries */
typedef struct {
    char  *type;             /* "local", "openai", "anthropic", "vertex" */
    char  *model_id;         /* model identifier for API calls */
    char  *api_key_env;      /* env var name for API key */
    char  *project_id;       /* Vertex AI project ID */
    char  *region;           /* Vertex AI region (e.g. "us-east5") */
    int    context_size;     /* context window size (0 = auto-detect) */
    float  chars_per_token;  /* chars per token ratio (default 3.5) */
    int    caching;          /* enable prompt caching (Anthropic) */
} provider_config_toml_t;

/* Belief Entropy configuration — forward-looking memory quality signal.
 * Based on MMPO [arXiv:2605.30159]: ℋ_BE(m_t) = H(y | m_t, q) measures
 * how clearly the current memory induces a confident belief about task state.
 * Lower entropy = clearer memory, higher entropy = ambiguous/incomplete. */
typedef struct {
    int    enabled;            /* enable Belief Entropy monitoring */
    double alpha;              /* weight vs outcome reward (Eq. 6, default 1.0) */
    char  *anchor_question;    /* probe question for entropy measurement */
    int    probe_tokens;       /* tokens to generate in entropy probe (default 30) */
    int    probe_n_probs;      /* top-N logprobs to request (default 10) */
    float  probe_temperature;  /* probe sampling temperature (default 0.6) */
    int    eviction_gate;      /* gate context eviction on entropy (default 0) */
    int    best_of_n_summaries;/* candidates for compression (default 1 = no selection) */
    float  warn_threshold;     /* H_BE increase that triggers warning (default 0.15) */
} belief_entropy_config_t;

/* Embedding configuration — semantic memory matching via vector embeddings.
 * Inspired by GDN-2's "short convolution on gates": instead of independent
 * substring scoring per memory, use dense semantic vectors for context-aware
 * similarity matching. */
typedef struct {
    char  *type;             /* "onnx", "ollama", "openai", "none" (default: "none") */
    char  *model;            /* embedding model name (e.g. "nomic-embed-text") */
    char  *api_base;         /* API base URL (e.g. "http://localhost:11434") */
    char  *model_path;       /* ONNX: directory with onnx/model.onnx + vocab.txt */
    int    dimension;        /* expected embedding dimension (0 = auto-detect) */
    int    max_input_chars;  /* max chars for text preparation (0 = auto from model) */
} embedding_config_t;

/* Per-model harness profile — loaded from ~/.nash/models/ (*.toml).
 * Enables model-specific adaptation without changing config.toml.
 * Inspired by Self-Harness [arXiv:2606.09498]: different models
 * need different harness rules for optimal performance. */
typedef struct {
    char  *match;               /* case-insensitive substring against model ID (required) */
    int    match_len;           /* cached strlen(match) for longest-match priority */
    char  *source_file;         /* which .toml file this came from (diagnostics) */

    /* Fields the server CAN'T tell us: */
    float  chars_per_token;     /* 0.0 = defer to provider/default (3.5) */
    thinking_config_t thinking; /* mode, budget; mode=THINKING_UNSET means defer */
    char  *system_prompt_extra; /* per-model harness rules, appended to system prompt */

    /* Informational only: */
    int    native_context;      /* model's training context size — for warnings */

    /* ── Unified Spec: per-model overrides (OpenJarvis-inspired) ──
     * All fields use sentinel values to mean "inherit from config.toml".
     * This enables model profiles to carry the full "tuned configuration"
     * so switching models auto-adjusts everything.
     *
     * SENTINEL REFERENCE (per-type):
     *   int fields:       0 = inherit (for counts/limits where 0 is invalid)
     *   bool-like ints:  -1 = inherit (because 0 = off is a valid value)
     *   float temperature: -1.0 = inherit (0.0 is valid: deterministic sampling)
     *   float top_p:       -1.0 = inherit (0.0 is valid edge case, 1.0 = disabled)
     *   int   top_k:         -1 = inherit (0 = disabled is valid)
     *   float vscore_exp:  -2.0 = inherit (0.0 = disabled and -1.0 are valid)
     *   double/float:      0.0 = inherit (for fields where 0.0 is invalid)
     *   pointers:         NULL = inherit
     *
     * WARNING: When adding new fields, verify the sentinel is OUTSIDE
     * the field's valid range. If 0 or -1 are valid values, choose a
     * domain-specific out-of-band sentinel (see vscore_exponent = -2.0).
     * For fields with unrestricted domains, consider a separate is_set flag. */

    /* [client] overrides */
    float  temperature;         /* -1.0 = inherit (0.0 is valid: deterministic sampling) */
    float  top_p;               /* -1.0 = inherit (0.0-1.0, 1.0=disabled) */
    int    top_k;               /* -1 = inherit (0=disabled) */
    int    max_tokens;          /* 0 = inherit */

    /* [react] subsystem overrides (same sentinel pattern as playbooks) */
    int    inject_memory;       /* -1 = inherit, 0 = off, 1 = on */
    int    inject_prev_result;  /* -1 = inherit */
    int    enable_reflection;   /* -1 = inherit */
    int    enable_pruning;      /* -1 = inherit */
    int    enable_compaction;   /* -1 = inherit */
    int    enable_scoring;      /* -1 = inherit */

    /* [limits] overrides */
    int    max_react_steps;     /* 0 = inherit */
    int    context_eviction_pct;/* 0 = inherit */
    int    eviction_floor_pct;  /* 0 = inherit */
    int    scratchpad_budget_pct;/* 0 = inherit */
    int    breadcrumb_budget_pct;/* 0 = inherit */
    int    compress_min_length; /* 0 = inherit */
    double recall_min_score;    /* 0.0 = inherit */
    float  recall_blend_semantic;  /* 0.0 = inherit */
    float  recall_blend_substring; /* 0.0 = inherit */
    float  vscore_exponent;     /* -2.0 = inherit (since -1.0 and 0.0 are valid values) */
    int    tool_retry_limit;    /* 0 = inherit */
    int    cycling_detection;   /* -1 = inherit */
    int    max_reflection_steps;/* 0 = inherit */
    int    memory_index_max;    /* 0 = inherit */
    int    max_skills_per_query;    /* 0 = inherit */
    int    max_lessons_per_query;   /* 0 = inherit */
    int    max_strategies_per_query;/* 0 = inherit */
    int    max_antipatterns_per_query; /* 0 = inherit */

    /* [tools] filter (restrict tools for weaker models) */
    int    max_tools;           /* 0 = inherit. When set and no explicit allow list,
                                 * auto-populates with SMALL_MODEL_TOOLS preset.
                                 * Reduces schema overhead for small models. */
    char **tools_allow;         /* NULL = inherit (all) */
    int    n_tools_allow;
    char **tools_block;         /* NULL = inherit (none) */
    int    n_tools_block;

    /* [tools.<name>] description overrides — per-tool description rewrites.
     * Parallel arrays: tool_desc_names[i] → tool_desc_values[i] */
    char **tool_desc_names;     /* tool names to override */
    char **tool_desc_values;    /* replacement descriptions */
    int    n_tool_descs;
} model_profile_t;

typedef struct {
    /* [server] — kept for backward compatibility.
     * When api_base is explicitly set in config.toml, it takes priority
     * over [provider] and forces local inference. */
    char  *api_base;
    int    api_base_explicit;  /* 1 = user set [server].api_base in config */

    /* [provider] — new multi-provider config */
    provider_config_toml_t provider;

    /* [embedding] — semantic memory matching */
    embedding_config_t embedding;

    /* [memory_belief_entropy] — Belief Entropy quality signal */
    belief_entropy_config_t belief_entropy;

    /* [client] */
    float  temperature;
    float  top_p;               /* nucleus sampling threshold (0.0-1.0, 1.0=disabled) */
    int    top_k;               /* top-k sampling (0=disabled) */
    int    max_tokens;
    int    stream;

    /* [thinking] */
    thinking_config_t thinking;

    /* [limits] */
    int    shell_timeout;        /* seconds, 0 = no limit */
    int    shell_max_output;     /* bytes */
    int    file_max_size;        /* bytes */
    int    grep_timeout;         /* seconds */
    int    grep_max_matches;     /* count */
    int    web_timeout;          /* seconds */
    int    web_max_size;         /* bytes */
    int    llm_max_response;     /* bytes */
    int    llm_repeat_threshold; /* consecutive identical tokens */
    int    llm_timeout;          /* seconds, per-call wall-clock timeout for LLM API */
    int    cycling_detection;    /* 0 = disabled (default), 1 = enabled */
    int    scratchpad_max;       /* chars, 0 = auto */
    int    max_react_steps;      /* steps per react loop */
    int    memory_index_max;     /* max entries in memory index injection */
    int    max_skills_per_query;     /* max skills loaded per query */
    int    max_lessons_per_query;    /* max lessons loaded per query */
    int    max_strategies_per_query; /* max strategies loaded per query */
    int    max_antipatterns_per_query; /* max anti-patterns loaded per query */
    int    context_eviction_pct; /* context usage % that triggers eviction */
    int    eviction_floor_pct;   /* min retained context as % of non-head budget (default 20) */
    int    scratchpad_budget_pct;/* scratchpad as % of context budget (default 15) */
    int    breadcrumb_budget_pct;/* combined breadcrumb budget as % of context (default 5) */
    int    compress_min_length;  /* min message size (chars) for BM25 compression (default 800) */
    int    max_reflection_steps; /* max steps in post-task reflection */
    int    reflection_gate;      /* 0 = user_ask (only reflect after user_ask),
                                  * 1 = always (reflect after every task),
                                  * 2 = never (disable reflection) */
    int    file_read_max_inline; /* max chars for file_read content inline */
    int    file_read_context_pct; /* max % of context window for file_read inline (0=use file_read_max_inline) */

    /* Self-Harness tunable surfaces (P3) — exposed for automated harness evolution.
     * These parameters can be tuned by the self-harness loop and validated via
     * the regression gate (--validate-harness compare). */
    float  recall_blend_semantic;  /* weight for semantic similarity in memory recall blend (default 0.5) */
    float  recall_blend_substring; /* weight for substring matching in memory recall blend (default 0.5) */
    float  vscore_exponent;        /* power-law exponent for Bayesian validation score (default 0.3).
                                    * composite = relevance × pow(vscore, exponent).
                                    * 0.0 = disabled (pure relevance), 1.0 = full multiplicative.
                                    * Default 0.3 reduces cold-start penalty: new memories (vscore=0.5)
                                    * get ×0.81 instead of ×0.50, while still penalizing memories
                                    * with actual misses (vscore=0.33 → ×0.72). */
    int    tool_retry_limit;       /* max consecutive errors on same tool before forced strategy switch (default 3) */
    int    checkpoint_frequency;   /* save checkpoint every N steps (0 = every step, default 0) */
    /* Memory pruning (Bayesian validation scoring) */
    double prune_min_score;      /* validation score threshold (default 0.35) */
    int    prune_min_evidence;   /* minimum recalls before pruning (default 3) */
    float  consolidation_threshold; /* MaxSim cosine threshold for near-duplicate
                                     * consolidation (default 0.82). Range 0.80-0.90
                                     * in IR literature; model-specific. */
    float  dedup_threshold;          /* MaxSim cosine threshold for reflection
                                     * deduplication (default 0.90). Higher = stricter. */

    /* P0: Memory recall quality gate — score threshold for injection.
     * Memories scoring below this threshold are NOT injected, implementing
     * "abstention" — the system stays silent when no stored experience is
     * relevant. Prevents noise injection that hurts performance.
     *
     * Research basis:
     *   MemFail [arXiv:2605.26667, May 2026] — diagnostic benchmark showing
     *     that injecting weakly-relevant memories HURTS agent performance.
     *   Mem-π [arXiv:2605.21463, May 2026] — generative memory policy that
     *     learns to abstain 30-40% of the time, yielding +22% avg improvement.
     *
     * This is the frozen-model equivalent of Mem-π's learned abstention:
     * instead of training a model to decide when to inject, we use a score
     * threshold on the composite relevance signal. */
    double recall_min_score;     /* min composite score for injection (default 0.15, normalized [0,1]) */

    /* Error-triggered reactive retrieval (P3 from harness-benefit research).
     * When a tool fails, memory is queried with the error text to surface
     * relevant lessons. These 4 parameters control the retrieval behavior. */
    int    error_recall_min_length;   /* min error text length (chars) to trigger recall (default 10) */
    int    error_recall_candidates;   /* max candidates to retrieve from memory (default 3) */
    int    error_recall_max_inject;   /* max entries to inject into chat (default 1) */
    double error_recall_min_relevance; /* min relevance score [0,1] for injection (default 0.25) */

    /* Eviction-triggered re-retrieval — when context is evicted, re-query
     * memory with the breadcrumb summary to re-surface relevant knowledge.
     * arXiv 2605.30621: retrieval at init uses the initial query, but needs
     * evolve. Eviction summary is the optimal query for what was lost. */
    int    eviction_recall_candidates;   /* candidates to retrieve (default 3) */
    double eviction_recall_min_relevance; /* min relevance for injection (default 0.30) */

    /* Cycling-triggered retrieval — when the agent is stuck in a cycle,
     * query memory for alternative approaches. */
    int    cycling_recall_candidates;    /* candidates to retrieve (default 2) */
    double cycling_recall_min_relevance; /* min relevance for injection (default 0.30) */

    /* Temporal event calendar — inject chronological memory overview.
     * arXiv 2605.15184 Finding #5: most impactful single component. */
    int    temporal_calendar;        /* enable temporal event injection (default 1) */
    int    temporal_recent_days;     /* "Recent" window in days (default 7) */
    int    temporal_older_days;      /* "Older" window in days (default 30) */
    int    temporal_max_entries;     /* max entries in calendar (default 20) */

    /* Episodic recall — query session_index for similar past sessions. */
    int    episodic_recall;          /* enable episodic recall at init (default 1) */
    int    episodic_max_results;     /* max session chunks to inject (default 2) */
    double episodic_min_score;       /* min similarity score (default 0.35) */

    /* Associative graph walk — follow refs[] of recalled memories. */
    int    associative_depth;        /* ref-follow depth (0=disabled, default 1) */

    /* Working memory auto-promotion — auto-append findings to scratchpad. */
    int    auto_promote;             /* enable auto-promotion (default 1) */
    int    auto_promote_min_length;  /* min tool result chars to trigger (default 500) */
    int    auto_promote_max_chars;   /* max chars in auto_findings section (default 2000) */

    /* P3: Auto-dream — usage-based memory consolidation trigger.
     *
     * Research basis:
     *   DCPM [arXiv:2606.09483, Jun 2026] — dual-process cognitive memory
     *     with asynchronous "nighttime engine" (System2) that induces schemas
     *     and sweeps for cross-domain collisions. Auto-dream implements the
     *     same pattern: fast synchronous writes during tasks, slow async
     *     consolidation between sessions.
     *   Letta Sleep-Time Compute [2025] — agents process and consolidate
     *     memories during idle time, improving future performance.
     *   Generative Agents [Park et al., 2023] — periodic reflection triggered
     *     by importance threshold accumulation.
     *
     * Trigger logic (usage-based, not calendar-based):
     *   At startup, counts memory entries created since the last dream
     *   (using created_at timestamps in the in-memory index vs .last_dream
     *   file mtime). If the count exceeds dream_reminder_threshold, shows a
     *   warning in the TUI status bar for the user to trigger manually.
     *
     * Why usage-based beats fixed interval:
     *   Real-world data shows daily write rates ranging from 2 to 99 entries.
     *   A fixed 7-day interval would consolidate too late during bursts
     *   (700 new entries) and too early during quiet periods (14 entries).
     *   Mutation count directly measures the amount of unprocessed work. */
    int    dream_reminder_threshold; /* new entries since last dream to show reminder (0 = disabled, default 50) */

    /* [paths] */
    char  *data_dir;             /* empty = ~/.nash/ */

    /* [workspace] — memory segregation via layered workspaces.
     * Global memory (~/.nash/memory/) always exists.
     * Workspace memory (~/.nash/workspaces/<name>/memory/) is optional. */
    char  *workspace;            /* active workspace name (NULL = global-only mode) */
    int    workspace_global_recall;  /* also search global memory during recall (default 1) */
    double workspace_global_weight;  /* score multiplier for global results (default 0.8) */
    int    workspace_isolated;       /* fully isolated — no global leakage (default 0) */

    /* [search] */
    char  *search_engine;        /* "searxng" (default) — kept for config compat */
    char  *searxng_url;

    /* [telegram] — native Telegram Bot bridge */
    char     *telegram_bot_token;   /* bot token from @BotFather */
    long long telegram_chat_id;     /* authorized chat ID */

    /* [model profiles] — loaded from ~/.nash/models/ */
    model_profile_t *model_profiles;
    int              n_model_profiles;

    /* Active model profile (set after model detection in main.c) */
    const char      *system_prompt_extra;  /* points into matched profile, do NOT free */
    const char      *matched_profile_file; /* source file of matched profile (diagnostics) */

    /* Was [thinking] section explicitly present in config.toml? */
    int              thinking_explicit;    /* 1 = yes, model profile won't override */

    /* ── Unified Spec: active tool filter + description overrides ──
     * Set by config_apply_profile() from matched model profile.
     * Used by build_tools_from_registry_filtered() during LLM requests. */
    char **profile_tools_allow;    /* tool whitelist from profile (NULL = all) */
    int    n_profile_tools_allow;
    int    profile_tools_allow_owned; /* 1 = auto-generated by max_tools, needs separate free */
    char **profile_tools_block;    /* tool blacklist from profile (NULL = none) */
    int    n_profile_tools_block;
    char **profile_tool_desc_names;   /* tool description override names */
    char **profile_tool_desc_values;  /* tool description override values */
    int    n_profile_tool_descs;

    /* Active react_flags overrides from matched model profile.
     * Applied as defaults in main.c when constructing react_ctx_t. */
    int    profile_inject_memory;      /* -1 = not set */
    int    profile_inject_prev_result; /* -1 = not set */
    int    profile_enable_reflection;  /* -1 = not set */
    int    profile_enable_pruning;     /* -1 = not set */
    int    profile_enable_compaction;  /* -1 = not set */
    int    profile_enable_scoring;     /* -1 = not set */
} config_t;

/* Load config from file. Returns defaults if file doesn't exist.
 * Caller must free with config_free(). */
config_t *config_load(const char *path);

/* Apply defaults for any unset fields */
void config_set_defaults(config_t *cfg);

/* Free a config_t */
void config_free(config_t *cfg);

/* Write default config to a file (creates if not exists) */
int config_write_default(const char *path);

/* Load all model profiles from a directory (e.g., ~/.nash/models/).
 * Returns 0 on success, -1 on error. Populates cfg->model_profiles. */
int config_load_model_profiles(config_t *cfg, const char *models_dir);

/* Find the best-matching profile for a model name (longest match wins).
 * Returns pointer into cfg->model_profiles, or NULL if no match. */
const model_profile_t *config_match_model(const config_t *cfg, const char *model_name);

/* Free model profiles array */
void config_free_model_profiles(config_t *cfg);

/* Apply a matched model profile as an overlay on cfg.
 * Handles all unified spec fields: client, react, limits, tools, descriptions.
 * Call after config_match_model() finds the best profile. */
void config_apply_profile(config_t *cfg, const model_profile_t *profile);

/* Dump the fully-resolved spec as TOML to the given file descriptor.
 * Serializes config_t after all layers (defaults + config.toml + profile)
 * have been applied. If profile_file is non-NULL, includes it in header. */
void config_dump_spec(const config_t *cfg, FILE *out, const char *profile_file);

/* Load a spec TOML file as an overlay on an existing config.
 * Reads [react] flags, [tools] allow/block, [memory], and other sections
 * from the spec file and applies them on top of cfg. This enables spec
 * round-trip: `nash --spec > spec.toml` then `nash --load-spec spec.toml`.
 * Returns 0 on success, -1 on parse error. */
int config_load_spec_overlay(config_t *cfg, const char *path);

/* Dump the fully-resolved spec as a heap-allocated TOML string.
 * Caller must free() the returned string.  Returns NULL on failure. */
char *config_dump_spec_to_string(const config_t *cfg, const char *profile_file);

#endif

#ifndef CONFIG_H
#define CONFIG_H

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
#define THINKING_UNSET -1  /* before config_set_defaults runs */
#define THINKING_UNSET -1  /* before config_set_defaults runs */

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
} embedding_config_t;

typedef struct {
    /* [server] — kept for backward compatibility */
    char  *api_base;

    /* [provider] — new multi-provider config */
    provider_config_toml_t provider;

    /* [embedding] — semantic memory matching */
    embedding_config_t embedding;

    /* [client] */
    float  temperature;
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
    int    scratchpad_max;       /* chars, 0 = auto */
    int    max_react_steps;      /* steps per react loop */
    int    memory_index_max;     /* max entries in memory index injection */
    int    max_skills_per_query; /* max skills loaded per query */
    int    context_eviction_pct; /* context usage % that triggers eviction */
    int    max_reflection_steps; /* max steps in post-task reflection */
    int    file_read_max_inline; /* max chars for file_read content inline */

    /* Memory pruning (Bayesian validation scoring) */
    double prune_min_score;      /* validation score threshold (default 0.35) */
    int    prune_min_evidence;   /* minimum recalls before pruning (default 3) */

    /* [paths] */
    char  *data_dir;             /* empty = ~/.nash/ */

    /* [search] */
    char  *search_engine;        /* "duckduckgo" or "searxng" */
    char  *searxng_url;
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

#endif

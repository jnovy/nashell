#ifndef CONFIG_H
#define CONFIG_H

/* Nash configuration — loaded from ~/.nash/config.toml */

typedef struct {
    /* [server] */
    char  *api_base;

    /* [client] */
    float  temperature;
    int    max_tokens;
    int    json_mode;
    int    thinking;
    int    stream;

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

#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "react.h"
#include "store.h"
#include "journal.h"
#include "frontend_tui.h"
#include "nash_limits.h"
#include "memory.h"
#include "ui_state.h"
#include "tui.h"
#include "nash_log.h"
#include "str.h"
#include "playbook.h"
#include "regression.h"
#include "postmortem.h"
#include "prompt_optimize.h"
#include "mailbox.h"

/* (load_legacy_scratchpad removed — legacy format handled by scratchpad_parse) */

/* FIX #6: Daemon mode graceful shutdown via signal handler.
 * SIGTERM/SIGINT set this flag; the daemon loop checks it each iteration. */
static volatile sig_atomic_t shutdown_requested = 0;
static void shutdown_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

/* FIX #16: Comparator for qsort — descending string order (newest first) */
static int cmp_str_desc(const void *a, const void *b) {
    return strcmp(*(const char **)b, *(const char **)a);
}

/* Get the nash data directory: ~/.nash/ or config override */
static char *get_nash_dir(const config_t *cfg) {
    if (cfg->data_dir && cfg->data_dir[0]) {
        mkdir(cfg->data_dir, 0755);
        return strdup(cfg->data_dir);
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/.nash", home);
    mkdir(path, 0755);
    return strdup(path);
}

/* Create session directory: <nash_dir>/sessions/<epoch.NNNNN>/ */
char *create_session_dir(const char *nash_dir) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    char sessions_base[1024];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
    mkdir(sessions_base, 0755);

    char path[1088];  /* sessions_base (1024) + "/" + epoch.nanos (~30) */
    snprintf(path, sizeof(path), "%s/%ld.%05ld",
             sessions_base, (long)tp.tv_sec, tp.tv_nsec / 10000);
    mkdir(path, 0755);
    return strdup(path);
}

/* Recursive mkdir: create all path components (like mkdir -p).
 * Returns 0 on success, -1 on failure (errno set). */


/* Check if a directory is empty (no files other than . and ..).
 * Returns 1 if empty, 0 if not empty or on error. */
static int is_dir_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int empty = 1;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
}

/* Helper: get JSON string or default */
static const char *jstr(cJSON *obj, const char *key, const char *def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : def;
}
static double jnum(cJSON *obj, const char *key, double def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsNumber(item)) ? cJSON_GetNumberValue(item) : def;
}
static int jbool(cJSON *obj, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return item ? cJSON_IsTrue(item) : def;
}

/* Apply model profile react_flags overrides to a react_flags_t.
 * Profile values of -1 mean "inherit" (no override). */
static void apply_profile_flags(react_flags_t *flags, const config_t *cfg) {
    if (cfg->profile_inject_memory >= 0)
        flags->inject_memory = cfg->profile_inject_memory;
    if (cfg->profile_inject_prev_result >= 0)
        flags->inject_prev_result = cfg->profile_inject_prev_result;
    if (cfg->profile_enable_reflection >= 0)
        flags->enable_reflection = cfg->profile_enable_reflection;
    if (cfg->profile_enable_pruning >= 0)
        flags->enable_pruning = cfg->profile_enable_pruning;
    if (cfg->profile_enable_compaction >= 0)
        flags->enable_compaction = cfg->profile_enable_compaction;
    if (cfg->profile_enable_scoring >= 0)
        flags->enable_scoring = cfg->profile_enable_scoring;
}

/* Build a tool_filter_t from profile-level tool filter on config.
 * Returns a filter with pointers into cfg (no allocation needed). */
static tool_filter_t build_profile_tool_filter(const config_t *cfg) {
    tool_filter_t tf = {0};
    if (cfg->n_profile_tools_allow > 0) {
        tf.allowed = (const char **)cfg->profile_tools_allow;
        tf.n_allowed = cfg->n_profile_tools_allow;
    }
    if (cfg->n_profile_tools_block > 0) {
        tf.blocked = (const char **)cfg->profile_tools_block;
        tf.n_blocked = cfg->n_profile_tools_block;
    }
    if (cfg->n_profile_tool_descs > 0) {
        tf.desc_names = cfg->profile_tool_desc_names;
        tf.desc_values = cfg->profile_tool_desc_values;
        tf.n_descs = cfg->n_profile_tool_descs;
    }
    return tf;
}

/* Print banner: header art + server props + client overrides */
/* Build banner into str_t — shared implementation for both stdio and TUI.
 * use_ansi: 1 = include ANSI color codes (terminal), 0 = plain text (TUI).
 * session_dir: if non-NULL, appended as "[session: ...]" line. */
static char *build_banner_impl(const config_t *cfg, const char *props_json,
                                const char *nash_dir, const char *session_dir,
                                const char *profile_file, int use_ansi) {
    str_t s = str_new(2048);

    /* ASCII art header */
    str_append_cstr(&s, "\n");
    if (use_ansi) {
        str_append_cstr(&s, "  \033[1m\033[38;2;80;255;120m _  _    __   ____  _  _  ____  __    __   \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;60;220;100m( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;40;190;80m ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \033[0m\n");
        str_append_cstr(&s, "  \033[1m\033[38;2;30;160;60m(___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \033[0m\n");
        str_append_cstr(&s, "\n");
        str_append_cstr(&s, "  \033[38;2;70;200;90m--------- * New Agentic Shell * ---------\033[0m\n");
    } else {
        str_append_cstr(&s, "   _   _   __   ____  _  _  ____  __    __   \n");
        str_append_cstr(&s, "  ( \\ | | / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \n");
        str_append_cstr(&s, "   ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \n");
        str_append_cstr(&s, "  (___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \n");
        str_append_cstr(&s, "\n");
        str_append_cstr(&s, "  --------- * New Agentic Shell * ---------\n");
    }
    str_append_cstr(&s, "\n");

    /* 1. Provider / server info */
    const char *ptype = cfg->provider.type;
    int is_api = ptype && (strcmp(ptype, "vertex") == 0 ||
                           strcmp(ptype, "anthropic") == 0 ||
                           strcmp(ptype, "openai") == 0);

    if (is_api) {
        str_appendf(&s, "provider: %s\n", ptype);
        str_appendf(&s, "  model:    %s\n",
                    cfg->provider.model_id ? cfg->provider.model_id : "(not set)");
        if (cfg->provider.project_id)
            str_appendf(&s, "  project:  %s\n", cfg->provider.project_id);
        if (cfg->provider.region)
            str_appendf(&s, "  region:   %s\n", cfg->provider.region);
        if (cfg->provider.context_size > 0)
            str_appendf(&s, "  ctx:      %d tok (%dk)\n",
                        cfg->provider.context_size,
                        cfg->provider.context_size / 1024);
        else
            str_append_cstr(&s, "  ctx:      unknown\n");
        str_append_cstr(&s, "\n");
    } else if (!props_json) {
        str_appendf(&s, "server: %s (props unavailable)\n\n",
                    cfg->api_base ? cfg->api_base : "(none)");
    } else {
        cJSON *props = cJSON_Parse(props_json);
        if (!props) {
            str_appendf(&s, "server: %s (props parse error)\n\n",
                        cfg->api_base ? cfg->api_base : "(none)");
        } else {
            cJSON *gs = cJSON_GetObjectItem(props, "default_generation_settings");
            cJSON *params = gs ? cJSON_GetObjectItem(gs, "params") : NULL;
            cJSON *caps = cJSON_GetObjectItem(props, "chat_template_caps");
            cJSON *mods = cJSON_GetObjectItem(props, "modalities");
            int n_ctx = (int)jnum(gs, "n_ctx", 0);

            str_appendf(&s, "server: %s\n", cfg->api_base ? cfg->api_base : "(none)");
            str_appendf(&s, "  model:    %s\n", jstr(props, "model_alias", "(unknown)"));
            str_appendf(&s, "  build:    %s\n", jstr(props, "build_info", "?"));
            str_appendf(&s, "  ctx:      %d tok (%dk) | slots: %d\n",
                        n_ctx, n_ctx / 1024, (int)jnum(props, "total_slots", 0));

            if (params) {
                str_appendf(&s, "  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f",
                            jnum(params, "temperature", 0),
                            (int)jnum(params, "top_k", 0),
                            jnum(params, "top_p", 0),
                            jnum(params, "min_p", 0));
                double rp = jnum(params, "repeat_penalty", 1.0);
                if (rp != 1.0) str_appendf(&s, " rep=%.1f", rp);
                str_append_cstr(&s, "\n");
            }

            str_appendf(&s, "  caps:     tools=%s vision=%s reasoning=%s\n",
                        caps && jbool(caps, "supports_tools", 0) ? "yes" : "no",
                        mods && jbool(mods, "vision", 0) ? "yes" : "no",
                        params ? jstr(params, "reasoning_format", "none") : "?");
            str_append_cstr(&s, "\n");
            cJSON_Delete(props);
        }
    }

    /* 2. Client config */
    const char *think_str;
    if (cfg->thinking.mode == THINKING_ON)
        think_str = "yes";
    else if (cfg->thinking.mode == THINKING_EDRM)
        think_str = is_api ? "off (edrm n/a)" : "edrm";
    else
        think_str = "no";
    str_appendf(&s, "client: temp=%.1f max_tokens=%d thinking=%s stream=%s\n",
                cfg->temperature, cfg->max_tokens,
                think_str,
                cfg->stream ? "on" : "off");
    str_appendf(&s, "data:   %s\n", nash_dir);
    char cwd_buf[NASH_PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
        str_appendf(&s, "cwd:    %s\n", cwd_buf);
    if (profile_file)
        str_appendf(&s, "profile: %s\n", profile_file);
    if (session_dir)
        str_appendf(&s, "\n[session: %s]\n", session_dir);
    if (!session_dir)
        str_append_cstr(&s, "\n");

    return str_steal(&s);
}

/* Print banner to stdout with ANSI colors (CLI mode) */
static void print_banner(const config_t *cfg, const char *props_json,
                         const char *nash_dir, const char *profile_file) {
    char *banner = build_banner_impl(cfg, props_json, nash_dir, NULL,
                                     profile_file, 1);
    fputs(banner, stdout);
    free(banner);
}

/* Build banner as a plain-text string for ncurses TUI */
static char *build_banner_string(const config_t *cfg, const char *props_json,
                                  const char *nash_dir, const char *session_dir,
                                  const char *profile_file) {
    return build_banner_impl(cfg, props_json, nash_dir, session_dir,
                             profile_file, 0);
}


/* --- Threading for non-blocking inference --- */
typedef struct {
    react_ctx_t *react;
    char        *query;
    ui_state_t  *ui;
    char        *result;
    atomic_int done;
} infer_args_t;

/* --- Threading for non-blocking inference --- */

static void threaded_event_cb(const react_event_t *ev, void *userdata) {
    infer_args_t *a = (infer_args_t *)userdata;
    pthread_mutex_lock(&a->ui->mtx);
    ui_state_on_event(ev, (void *)a->ui);
    pthread_mutex_unlock(&a->ui->mtx);
}

static void *infer_worker(void *arg) {
    infer_args_t *a = (infer_args_t *)arg;
    a->result = react_run(a->react, a->query, threaded_event_cb, a);
    a->done = 1;
    return NULL;
}

/* ── Session init/cleanup helpers ──────────────────────────────────
 * Unifies the 4 duplicated initialization paths (daemon, headless,
 * mailbox, TUI) into a single function. */

static void session_init_tools(tool_ctx_t *tools, store_t *store,
                               journal_t *journal, memory_t *memory,
                               char *session_dir, config_t *cfg,
                               provider_t *provider) {
    memset(tools, 0, sizeof(*tools));
    tools->store = store;
    tools->journal = journal;
    tools->memory = memory;
    tools->session_dir = session_dir;
    tools->cfg = cfg;
    tools->provider = provider;
    tools->react_loop = journal_max_react_loop(journal) + 1;
    tools->aliases = alias_map_new();
    scratchpad_init(&tools->scratch);
    if (session_dir) {
        scratchpad_load(&tools->scratch, session_dir);
    }
    tools->tool_filter = build_profile_tool_filter(cfg);
}

static void session_init_react(react_ctx_t *react, provider_t *provider,
                               tool_ctx_t *tools, config_t *cfg) {
    memset(react, 0, sizeof(*react));
    react->provider = provider;
    react->tools = tools;
    react->max_steps = cfg->max_react_steps;
    react->verbose = 1;
    react->flags = (react_flags_t)REACT_FLAGS_DEFAULT;
    apply_profile_flags(&react->flags, cfg);
    react->parent_loop = -1;
    pthread_mutex_init(&react->user_ask_mutex, NULL);
    pthread_cond_init(&react->user_ask_cond, NULL);
    pthread_mutex_init(&react->pause_mutex, NULL);
    pthread_cond_init(&react->pause_cond, NULL);
}

static void session_cleanup(tool_ctx_t *tools, react_ctx_t *react,
                            journal_t *journal) {
    pthread_mutex_destroy(&react->user_ask_mutex);
    pthread_cond_destroy(&react->user_ask_cond);
    pthread_mutex_destroy(&react->pause_mutex);
    pthread_cond_destroy(&react->pause_cond);
    free(react->pause_query);
    react->pause_query = NULL;
    tool_free_deferred_consolidations(tools);
    scratchpad_free(&tools->scratch);
    alias_map_free(tools->aliases);
    free(tools->last_spec_hash);
    tools->last_spec_hash = NULL;
    journal_free(journal);
}

/* Unified cleanup for global resources.
 * Replaces 8+ duplicated cleanup sequences across early-return paths.
 * All _free functions handle NULL safely. */
static void cleanup_globals(store_t *shared_store, memory_t *memory,
                            provider_t *provider, char *nash_dir,
                            char *props_json, char *server_model,
                            config_t *cfg) {
    store_free(shared_store);
    memory_free(memory);
    provider_free(provider);
    free(nash_dir);
    free(props_json);
    free(server_model);
    config_free(cfg);
}

int main(int argc, char **argv) {
    /* Load config from ~/.nash/config.toml (or default) */
    char config_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(config_path, sizeof(config_path), "%s/.nash/config.toml", home);

    config_t *cfg = config_load(config_path);

    /* CLI flags override config */
    const char *query = NULL;
    const char *session_dir_arg = NULL;
    const char *play_arg = NULL;
    int regression_mode = 0;
    const char *validate_harness = NULL;  /* "baseline" or "compare" */
    int regression_split = -1;           /* -1 = all, 0 = held-in, 1 = held-out */
    int postmortem_mode = 0;
    int postmortem_sessions = 50;        /* default: scan last 50 sessions */
    int spec_mode = 0;                   /* --spec: dump resolved spec and exit */
    const char *load_spec_path = NULL;    /* --load-spec FILE: overlay spec on config */
    const char *optimize_budget = NULL;   /* --optimize BUDGET: GEPA prompt optimization */
    const char *reflect_model_arg = NULL; /* --reflect-model MODEL: reflection LM for optimization */
    int mailbox_mode = 0;                 /* --mailbox: enable file-based mailbox for user_ask */
    int daemon_mode = 0;                  /* --daemon: watch mailbox inbox for tasks */
    int mailbox_timeout = 0;              /* --mailbox-timeout SECS: user_ask timeout */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            free(cfg->api_base);
            cfg->api_base = strdup(argv[++i]);
            /* --api forces local provider mode — override any [provider]
             * section in config.toml. The user is pointing to a specific
             * llama.cpp/OpenAI-compatible server, not a cloud API. */
            free(cfg->provider.type);
            cfg->provider.type = strdup("local");
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
            play_arg = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            free(cfg->data_dir);
            cfg->data_dir = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_dir_arg = argv[++i];
        } else if (strcmp(argv[i], "--regression") == 0) {
            regression_mode = 1;
        } else if (strcmp(argv[i], "--validate-harness") == 0 && i + 1 < argc) {
            validate_harness = argv[++i];
            regression_mode = 1;
        } else if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
            const char *sp = argv[++i];
            if (strcmp(sp, "held-in") == 0) regression_split = 0;
            else if (strcmp(sp, "held-out") == 0) regression_split = 1;
        } else if (strcmp(argv[i], "--postmortem") == 0) {
            postmortem_mode = 1;
        } else if (strcmp(argv[i], "--postmortem-sessions") == 0 && i + 1 < argc) {
            postmortem_sessions = atoi(argv[++i]);
            postmortem_mode = 1;
        } else if (strcmp(argv[i], "--spec") == 0) {
            spec_mode = 1;
        } else if (strcmp(argv[i], "--load-spec") == 0 && i + 1 < argc) {
            load_spec_path = argv[++i];
        } else if (strcmp(argv[i], "--optimize") == 0 && i + 1 < argc) {
            optimize_budget = argv[++i];
        } else if (strcmp(argv[i], "--reflect-model") == 0 && i + 1 < argc) {
            reflect_model_arg = argv[++i];
        } else if (strcmp(argv[i], "--mailbox") == 0) {
            mailbox_mode = 1;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
            mailbox_mode = 1;  /* daemon implies mailbox */
        } else if (strcmp(argv[i], "--mailbox-timeout") == 0 && i + 1 < argc) {
            mailbox_timeout = atoi(argv[++i]);
            mailbox_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [-p QUERY] [--data-dir PATH] [--session DIR] [--play NAME]\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("  --api URL       LLM server URL (default: %s)\n", cfg->api_base);
            printf("  -p QUERY        Run single query and exit (headless mode)\n");
            printf("  --play NAME     Run a playbook and exit (e.g. --play dream)\n");
            printf("  --data-dir PATH Data directory (default: ~/.nash/)\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("\nSelf-Harness:\n");
            printf("  --regression          Run regression test suite\n");
            printf("  --split SPLIT         Filter: held-in or held-out\n");
            printf("  --validate-harness MODE  baseline or compare\n");
            printf("  --postmortem          Analyze session failures\n");
            printf("  --postmortem-sessions N  Sessions to scan (default 50)\n");
            printf("  --optimize BUDGET     GEPA prompt optimization (light/medium/heavy/N)\n");
            printf("  --reflect-model MODEL Reflection LM for optimization\n");
            printf("\nSpec:\n");
            printf("  --spec                Dump fully-resolved config spec and exit\n");
            printf("  --load-spec FILE      Load a spec TOML as config overlay\n");
            printf("\nMailbox (headless communication):\n");
            printf("  --mailbox             Enable file-based mailbox for user_ask in -p mode\n");
            printf("  --daemon              Watch mailbox inbox for task files (implies --mailbox)\n");
            printf("  --mailbox-timeout N   Timeout in seconds for user_ask answers (0=forever)\n");
            printf("\nConfig: %s\n", config_path);
            config_free(cfg);
            return 0;
        }
    }

    /* Initialize data directory */
    char *nash_dir = get_nash_dir(cfg);

    /* Create models/ directory for per-model profiles */
    {
        char models_dir[1024];
        snprintf(models_dir, sizeof(models_dir), "%s/models", nash_dir);
        mkdir(models_dir, 0755);
        config_load_model_profiles(cfg, models_dir);
    }

    /* Write default config if it doesn't exist */
    config_write_default(config_path);

    /* Apply spec overlay if --load-spec was given.
     * This overrides config.toml settings before provider creation. */
    if (load_spec_path) {
        if (config_load_spec_overlay(cfg, load_spec_path) != 0) {
            fprintf(stderr, "[error] failed to load spec from %s\n", load_spec_path);
            free(nash_dir);
            config_free(cfg);
            return 1;
        }
    }

    /* ── Postmortem mode: no LLM needed ── */
    if (postmortem_mode) {
        postmortem_report_t *pm = postmortem_analyze(nash_dir, postmortem_sessions);
        postmortem_print(pm);

        /* Save evidence bundle */
        char bundle_path[NASH_PATH_MAX];
        snprintf(bundle_path, sizeof(bundle_path), "%s/postmortem.md", nash_dir);
        if (pm->total_failures > 0) {
            postmortem_save(pm, bundle_path);
            fprintf(stderr, "  Evidence bundle saved: %s\n\n", bundle_path);
        }

        postmortem_free(pm);
        free(nash_dir);
        config_free(cfg);
        return 0;
    }

    /* ── Create provider from config ── */
    provider_config_t pcfg = {
        .type           = provider_type_from_str(cfg->provider.type),
        .model_id       = cfg->provider.model_id,
        .api_base       = cfg->api_base,
        .api_key_env    = cfg->provider.api_key_env,
        .project_id     = cfg->provider.project_id,
        .region         = cfg->provider.region,
        .context_size   = cfg->provider.context_size,
        .chars_per_token = cfg->provider.chars_per_token,
        .caching        = cfg->provider.caching,
        .max_tokens     = cfg->max_tokens,
        .temperature    = cfg->temperature,
        .enable_thinking = 0,
        .thinking_budget = -1,
        .llm_timeout     = cfg->llm_timeout,
    };
    provider_t *provider = provider_create(&pcfg);

    /* Fetch model info (local server: /props + /v1/models) */
    int context_size = 0;
    char *server_model = NULL;
    char *props_json = NULL;

    if (provider && provider->fetch_model_info) {
        provider->fetch_model_info(provider, &context_size, &server_model, &props_json);
    } else if (pcfg.type == PROVIDER_LOCAL) {
        /* Fallback for local without provider vtable */
        context_size = llm_fetch_context_size(cfg->api_base);
        server_model = llm_fetch_model_name(cfg->api_base);
        props_json = llm_fetch_props_json(cfg->api_base);
    } else {
        /* API providers: use config values, with sensible defaults */
        context_size = cfg->provider.context_size;
        server_model = cfg->provider.model_id ? strdup(cfg->provider.model_id) : NULL;

        /* BUG FIX: Cloud providers (Vertex, Anthropic, OpenAI) have no /props
         * endpoint to auto-detect context_size. Without a default, context_size
         * stays 0 and context eviction never triggers — causing unbounded
         * context growth until the API rejects with HTTP 400. */
        if (context_size == 0) {
            if (pcfg.type == PROVIDER_VERTEX || pcfg.type == PROVIDER_ANTHROPIC)
                context_size = 200000;  /* Claude models: 200K tokens */
            else if (pcfg.type == PROVIDER_OPENAI)
                context_size = 128000;  /* GPT-4o/4.1: 128K tokens */
        }
    }

    /* Update provider and config with fetched context size */
    if (context_size > 0) {
        if (provider) provider->cfg.context_size = context_size;
        if (cfg->provider.context_size == 0)
            cfg->provider.context_size = context_size;
    }
    if (provider && server_model) {
        free((char *)provider->cfg.model_id);  /* free the copy made by provider_create */
        provider->cfg.model_id = strdup(server_model);  /* replace with server-reported model */
    }

    /* ── Apply model profile (Unified Spec) ── */
    const char *matched_profile_file = NULL;
    if (server_model) {
        const model_profile_t *profile = config_match_model(cfg, server_model);
        if (profile) {
            matched_profile_file = profile->source_file;
            cfg->matched_profile_file = matched_profile_file;
            fprintf(stderr, "[model-profile] matched '%s' from %s\n",
                    profile->match, profile->source_file);

            /* Apply all profile overrides via unified config_apply_profile() */
            config_apply_profile(cfg, profile);

            /* Sync chars_per_token to provider vtable (provider has its own copy) */
            if (provider && cfg->provider.chars_per_token > 0)
                provider->cfg.chars_per_token = cfg->provider.chars_per_token;

            /* native_context warning */
            if (profile->native_context > 0 && context_size > 0 &&
                context_size < profile->native_context / 2) {
                fprintf(stderr, "[model-profile] ⚠ server n_ctx=%d but %s supports %d\n",
                        context_size, profile->match, profile->native_context);
            }
        }
    }

    /* llm_config_t removed — provider_t is the single source of truth. */

    /* ── Spec mode: dump resolved config and exit ── */
    if (spec_mode) {
        config_dump_spec(cfg, stdout, matched_profile_file);
        provider_free(provider);
        free(nash_dir);
        free(props_json);
        free(server_model);
        config_free(cfg);
        return 0;
    }

    /* Print banner (skip in headless playbook mode) */
    if (!play_arg)
        print_banner(cfg, props_json, nash_dir, matched_profile_file);

    /* Shared store + memory */
    store_t *shared_store = store_new(nash_dir);
    memory_t *memory = memory_new(nash_dir);
    if (server_model)
        memory->model = strdup(server_model);
    /* Set recall tuning parameters from config (centralized sync) */
    memory_set_recall_config(memory, cfg->recall_min_score,
                             cfg->recall_blend_semantic,
                             cfg->recall_blend_substring,
                             cfg->vscore_exponent);

    /* Prune stale memories at startup (90 days, access_count < 2) */
    int pruned = memory_prune(memory,
                              cfg->prune_min_score, cfg->prune_min_evidence);
    if (pruned > 0)
        fprintf(stderr, "[info] pruned %d stale memories\n", pruned);

    /* Initialize semantic embeddings for memory matching (if configured) */
    if (cfg->embedding.type && strcmp(cfg->embedding.type, "none") != 0) {
        memory_init_embeddings(memory, cfg->embedding.type,
                               cfg->embedding.model, cfg->embedding.api_base,
                               cfg->embedding.model_path,
                               cfg->embedding.dimension,
                               cfg->embedding.max_input_chars);
    }

    /* Dream reminder — usage-based memory consolidation reminder.
     *
     * Counts entries created since last dream using existing per-entry
     * created_at timestamps (P1) vs .last_dream file mtime.
     * If count >= dream_reminder_threshold, shows a warning in the status bar.
     * Does NOT auto-trigger — user decides when to run /dream.
     *
     * Research basis: Generative Agents [Park et al., 2023] — importance
     * threshold accumulation triggers reflection. Here the "importance"
     * being accumulated is the count of unconsolidated writes. */
    int dream_new_count = 0;
    if (cfg->dream_reminder_threshold > 0 && memory) {
        char dream_ts_path[NASH_PATH_MAX];
        snprintf(dream_ts_path, sizeof(dream_ts_path), "%s/memory/.last_dream",
                 nash_dir);
        struct stat dream_st;

        if (stat(dream_ts_path, &dream_st) != 0) {
            /* No .last_dream file — never dreamed, count all entries */
            dream_new_count = memory->idx.count;
        } else {
            double last_dream_epoch = (double)dream_st.st_mtime;
            for (int i = 0; i < memory->idx.count; i++) {
                if (memory->idx.entries[i].created_at > last_dream_epoch)
                    dream_new_count++;
            }
        }
    }

    /* ── Regression test mode: --regression ── */
    if (regression_mode) {
        /* Create regression directory and seed query bank */
        char regression_dir[NASH_PATH_MAX];
        snprintf(regression_dir, sizeof(regression_dir), "%s/regression", nash_dir);
        regression_write_seed(regression_dir);

        /* Load query banks */
        int n_banks = 0;
        query_bank_t *banks = regression_load_banks(regression_dir, &n_banks);
        if (!banks || n_banks == 0) {
            fprintf(stderr, "[regression] no query banks found in %s\n", regression_dir);
            cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        fprintf(stderr, "[regression] loaded %d query banks\n", n_banks);

        /* Run tests */
        regression_report_t *report = regression_run(
            banks, n_banks, regression_split,
            provider, cfg, memory, shared_store, nash_dir);

        regression_print_report(report);

        int exit_code = 0;

        /* Handle --validate-harness */
        if (validate_harness) {
            char baseline_path[NASH_PATH_MAX];
            snprintf(baseline_path, sizeof(baseline_path),
                     "%s/regression/baseline.json", nash_dir);

            if (strcmp(validate_harness, "baseline") == 0) {
                regression_save_report(report, baseline_path);
                fprintf(stderr, "[regression] baseline saved: %s\n", baseline_path);
            } else if (strcmp(validate_harness, "compare") == 0) {
                regression_report_t *baseline = regression_load_report(baseline_path);
                if (!baseline) {
                    fprintf(stderr, "[regression] no baseline found at %s\n", baseline_path);
                    fprintf(stderr, "[regression] run with --validate-harness baseline first\n");
                    exit_code = 2;
                } else {
                    exit_code = (int)regression_compare(baseline, report);
                    regression_free_report(baseline);
                }
            }
        }

        regression_free_report(report);
        regression_free_banks(banks, n_banks);
        cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
        return exit_code;
    }

    /* ── GEPA Prompt Optimization mode: --optimize BUDGET ── */
    if (optimize_budget) {
        int rounds = optimize_parse_budget(optimize_budget);
        if (rounds < 0) {
            fprintf(stderr, "[optimize] invalid budget '%s' — use light, medium, heavy, or a number\n",
                    optimize_budget);
            cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        /* Create regression directory and seed query banks */
        char regression_dir[NASH_PATH_MAX];
        snprintf(regression_dir, sizeof(regression_dir), "%s/regression", nash_dir);
        regression_write_seed(regression_dir);

        /* Load query banks */
        int n_banks = 0;
        query_bank_t *banks = regression_load_banks(regression_dir, &n_banks);
        if (!banks || n_banks == 0) {
            fprintf(stderr, "[optimize] no query banks found in %s\n", regression_dir);
            cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        /* Create reflection provider (same as student by default) */
        provider_t *reflection_provider = provider;
        if (reflect_model_arg) {
            /* Parse reflect-model as "provider/model" e.g. "anthropic/claude-sonnet-4-20250514" */
            char *slash = strchr(reflect_model_arg, '/');
            if (slash) {
                char ptype[64];
                snprintf(ptype, sizeof(ptype), "%.*s", (int)(slash - reflect_model_arg), reflect_model_arg);
                const char *rmodel = slash + 1;
                provider_config_t rpcfg = {
                    .type           = provider_type_from_str(ptype),
                    .model_id       = rmodel,
                    .api_base       = cfg->api_base,
                    .api_key_env    = cfg->provider.api_key_env,
                    .project_id     = cfg->provider.project_id,
                    .region         = cfg->provider.region,
                    .context_size   = cfg->provider.context_size,
                    .chars_per_token = cfg->provider.chars_per_token,
                    .max_tokens     = cfg->max_tokens,
                    .temperature    = 0.7f,  /* slightly creative for reflection */
                    .enable_thinking = 0,
                    .thinking_budget = -1,
                    .llm_timeout     = cfg->llm_timeout,
                };
                reflection_provider = provider_create(&rpcfg);
                if (!reflection_provider) {
                    fprintf(stderr, "[optimize] failed to create reflection provider '%s'\n",
                            reflect_model_arg);
                    reflection_provider = provider;  /* fallback to student */
                }
            } else {
                fprintf(stderr, "[optimize] --reflect-model format: provider/model (e.g. anthropic/claude-sonnet-4-20250514)\n");
                fprintf(stderr, "[optimize] using student model as reflection model\n");
            }
        }

        /* Find model profile path for writing results */
        const char *profile_path = NULL;
        char profile_path_buf[NASH_PATH_MAX * 2];
        if (server_model) {
            const model_profile_t *profile = config_match_model(cfg, server_model);
            if (profile && profile->source_file) {
                snprintf(profile_path_buf, sizeof(profile_path_buf),
                         "%s/models/%s", nash_dir, profile->source_file);
                profile_path = profile_path_buf;
            }
        }

        /* Configure and run optimization */
        optimize_config_t opt = {
            .max_rounds   = rounds,
            .student      = provider,
            .reflection   = reflection_provider,
            .profile_path = profile_path,
            .split_filter = regression_split,
            .verbose      = 1,
        };

        prompt_candidate_t best = optimize_run(
            &opt, banks, n_banks, cfg, memory, shared_store, nash_dir);

        /* Cleanup */
        optimize_free_candidate(&best);
        regression_free_banks(banks, n_banks);
        if (reflection_provider != provider)
            provider_free(reflection_provider);
        cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
        return 0;
    }

    /* Headless playbook mode: --play NAME */
    if (play_arg) {
        /* Resolve playbook path */
        char pb_path[NASH_PATH_MAX];
        if (strcmp(play_arg, "dream") == 0) {
            snprintf(pb_path, sizeof(pb_path), "%s/playbooks/dream.yaml", nash_dir);
        } else if (strchr(play_arg, '/') || strchr(play_arg, '.')) {
            snprintf(pb_path, sizeof(pb_path), "%s", play_arg);
        } else {
            snprintf(pb_path, sizeof(pb_path), "%s/playbooks/%s.yaml", nash_dir, play_arg);
        }

        playbook_t *pb = playbook_load(pb_path);
        if (!pb && strcmp(play_arg, "dream") == 0) {
            playbook_write_default_dream(pb_path);
            pb = playbook_load(pb_path);
        }
        if (!pb) {
            fprintf(stderr, "Error: cannot load playbook '%s'\n", pb_path);
            cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        fprintf(stderr, "[play] Running playbook '%s' (%d passes)\n",
                pb->name, pb->n_passes);

        playbook_args_t pargs = {
            .playbook = pb,
            .nash_dir = nash_dir,
            .store = shared_store,
            .memory = memory,
            .cfg = cfg,
            .provider = provider,
            .server_model = server_model,
            .ui = NULL,  /* headless — no TUI */
            .playbook_ok = 0,
            .done = 0,
        };

        /* Run synchronously (no thread needed in headless mode) */
        playbook_worker(&pargs);

        int ok = pargs.playbook_ok;
        fprintf(stderr, "[play] Playbook '%s' %s\n",
                pb->name, ok ? "completed successfully" : "FAILED");

        playbook_free(pb);
        cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
        return ok ? 0 : 1;
    }

    /* Daemon mode: watch mailbox inbox for tasks, process them sequentially */
    if (daemon_mode) {
        char mbox_dir[NASH_PATH_MAX];
        if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) {
            fprintf(stderr, "[error] failed to initialize mailbox\n");
            cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }
        fprintf(stderr, "[daemon] nash mailbox daemon started\n");
        fprintf(stderr, "[daemon] inbox: %s/inbox/  (drop task_* files here)\n", mbox_dir);
        fprintf(stderr, "[daemon] outbox: %s/outbox/ (results appear here)\n", mbox_dir);

        /* FIX #6: Install signal handlers for graceful daemon shutdown.
         * SIGTERM/SIGINT set shutdown_requested; the loop checks it
         * each iteration so in-progress tasks complete before exit. */
        signal(SIGTERM, shutdown_handler);
        signal(SIGINT, shutdown_handler);

        while (!shutdown_requested) {
            char *task_id = NULL;
            char *task_query = mailbox_wait_task(mbox_dir, &task_id, 0);
            if (!task_query) {
                if (shutdown_requested) break;
                fprintf(stderr, "[daemon] wait_task returned NULL, retrying...\n");
                sleep(1);
                continue;
            }

            fprintf(stderr, "\n[daemon] === new task: %s ===\n", task_id ? task_id : "unknown");
            fprintf(stderr, "[daemon] query: %.200s%s\n", task_query,
                    strlen(task_query) > 200 ? "..." : "");

            /* Set up a fresh session for each task */
            journal_t *journal = journal_new_lazy(nash_dir);
            tool_ctx_t tools;
            session_init_tools(&tools, shared_store, journal, memory,
                               NULL, cfg, provider);
            react_ctx_t react;
            session_init_react(&react, provider, &tools, cfg);

            mailbox_ctx_t mbox = {
                .react_ctx = &react,
                .mailbox_dir = mbox_dir,
                .session_dir = NULL,
                .timeout_sec = mailbox_timeout,
            };

            char *result = react_run(&react, task_query, mailbox_on_event, &mbox);

            /* Write result to outbox */
            if (task_id) {
                mailbox_write_result(mbox_dir, task_id, result);
            }

            if (result) {
                fprintf(stderr, "[daemon] task %s completed\n", task_id ? task_id : "unknown");
                printf("%s\n", result);
                free(result);
            } else {
                fprintf(stderr, "[daemon] task %s failed (no result)\n", task_id ? task_id : "unknown");
            }

            /* Tier 1 dreaming */
            memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);

            /* Cleanup */
            session_cleanup(&tools, &react, journal);
            free(task_id);
            free(task_query);
        }
        /* FIX #6: Graceful shutdown — cleanup shared resources */
        fprintf(stderr, "[daemon] shutting down...\n");
        web_search_cleanup();
        cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
        return 0;
    }

    /* One-shot headless mode */
    if (query) {
        /* Detect existing session: --session arg, or CWD with journal.jsonl */
        char *session_dir = NULL;
        int lazy_session = 0;
        if (session_dir_arg) {
            char jpath[4112];
            snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir_arg);
            if (access(jpath, F_OK) == 0) {
                session_dir = strdup(session_dir_arg);
            } else {
                fprintf(stderr, "[warn] %s has no journal.jsonl, creating new session\n", session_dir_arg);
            }
        }
        /* NOTE: Do NOT auto-detect session from CWD in headless mode.
         * When a parent nash spawns a child via shell_exec("nash -p ..."),
         * the child inherits the parent's CWD (which IS a session dir with
         * journal.jsonl), causing the child to hijack the parent's session:
         * loading its checkpoint, writing to its journal, and corrupting
         * the parent's state.  Only explicit --session should be honored. */
        journal_t *journal;
        if (!session_dir) {
            /* Lazy session: directory created on first journal_append */
            journal = journal_new_lazy(nash_dir);
            lazy_session = 1;
        } else {
            journal = journal_new(session_dir);
        }
        tool_ctx_t tools;
        session_init_tools(&tools, shared_store, journal, memory,
                           session_dir, cfg, provider);
        react_ctx_t react;
        session_init_react(&react, provider, &tools, cfg);

        /* Mailbox mode: use mailbox_on_event to handle user_ask via files */
        char *result;
        if (mailbox_mode) {
            char mbox_dir[NASH_PATH_MAX];
            if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) {
                fprintf(stderr, "[error] failed to initialize mailbox\n");
                session_cleanup(&tools, &react, journal);
                if (session_dir) free(session_dir);
                cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
                return 1;
            }
            mailbox_ctx_t mbox = {
                .react_ctx = &react,
                .mailbox_dir = mbox_dir,
                .session_dir = session_dir,
                .timeout_sec = mailbox_timeout,
            };
            fprintf(stderr, "[mailbox] enabled — questions in %s/outbox/, answers in %s/inbox/\n",
                    mbox_dir, mbox_dir);
            result = react_run(&react, query, mailbox_on_event, &mbox);
        } else {
            result = react_run(&react, query, tui_on_event, NULL);
        }
        /* Resolve session_dir from journal for lazy sessions.
         * journal_session_dir returns internal pointer — must strdup
         * because journal_free() will free the original. */
        if (lazy_session) {
            const char *jsd = journal_session_dir(journal);
            session_dir = jsd ? strdup(jsd) : NULL;
        }
        /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
        memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
        int have_result = (result != NULL);
        if (result) { printf("%s\n", result); free(result); }
        /* Save scratchpad if session was created */
        if (session_dir && tools.scratch.count > 0) {
            scratchpad_save(&tools.scratch, session_dir);
        }
        tools.react_loop++;  /* increment for next query */
        session_cleanup(&tools, &react, journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
        cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
        return have_result ? 0 : 1;
    }

    /* Interactive TUI mode — ONE session for ALL queries */
    {
        /* Detect existing session: --session arg, or CWD with journal.jsonl */
        char *session_dir = NULL;
        if (session_dir_arg) {
            char jpath[4112];
            snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir_arg);
            if (access(jpath, F_OK) == 0) {
                session_dir = strdup(session_dir_arg);
            } else {
                fprintf(stderr, "[warn] %s has no journal.jsonl, creating new session\n", session_dir_arg);
            }
        }
        if (!session_dir) {
            char cwd[NASH_PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                char jpath[4112];
                snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", cwd);
                if (access(jpath, F_OK) == 0) {
                    session_dir = strdup(cwd);
                }
            }
        }

        /* Interactive TUI needs session_dir immediately for journal display,
         * so always create it eagerly (lazy sessions break TUI rendering). */
        if (!session_dir) {
            session_dir = create_session_dir(nash_dir);
        }
        journal_t *journal = journal_new(session_dir);
        tool_ctx_t tools;
        session_init_tools(&tools, shared_store, journal, memory,
                           session_dir, cfg, provider);
        react_ctx_t react;
        session_init_react(&react, provider, &tools, cfg);

        /* Initialize logging subsystem for TUI error routing */
        nash_log_init(journal, shared_store);
        nash_log_set_tools(&tools);

        /* Create UI state and initialize TUI */
        ui_state_t *ui = ui_state_new(session_dir, shared_store);
        ui->nash_dir = strdup(nash_dir);  /* for /? cross-session search */
        /* Pass model name + context info for nashell-style status bar */
        if (server_model)
            ui->model_name = strdup(server_model);
        ui->context_size = context_size;
        ui->context_used = 0;
        ui->pause_flag = &react.pause_requested;  /* ESC → pause react loop */
        ui->bg_jobs = 0;
        /* Show dream reminder in status bar if threshold exceeded */
        if (cfg->dream_reminder_threshold > 0 && dream_new_count >= cfg->dream_reminder_threshold) {
            char dream_msg[256];
            snprintf(dream_msg, sizeof(dream_msg),
                     "⚠ %d new memories — /dream to consolidate", dream_new_count);
            ui_state_set_status(ui, STATUS_READY, dream_msg);
        } else {
            ui_state_set_status(ui, STATUS_READY, "Ready");
        }
        /* Set banner text for main pane */
        char *banner = build_banner_string(cfg, props_json, nash_dir, session_dir, matched_profile_file);
        ui_state_set_banner(ui, banner);
        free(banner);

        /* Initialize TUI BEFORE loading journal, so visible_rows is set
         * correctly for autoscroll calculations in rebuild_md(). */
        tui_init();
        nash_log_set_ui(ui);  /* enable TUI error routing */
        ui->visible_rows = LINES - 4;  /* terminal height minus chrome (top/bottom bars) */

        /* Load existing journal entries into UI state */
        ui_state_load_journal(ui, journal);

        tui_render(ui);

        /* Wrapper event callback: updates ViewModel + redraws TUI */
        /* We use a struct to pass both ui and tui context */

        /* Main TUI event loop */
        int running = 1;
        /* State machine: 0=idle (main thread only), 1=inference running,
         * 3=playbook running. Values 1 and 3 mean infer_tid is joinable.
         * FIX #13: Made atomic for defense in depth — currently only the
         * main thread reads/writes, but atomic_int prevents data races
         * if future code accesses it from another thread. */
        atomic_int inferring = 0;
        pthread_t infer_tid;
        /* Thread-shared args: static lifetime so they survive across loop
         * iterations.  Thread ownership contract:
         *   Written by main thread BEFORE pthread_create (happens-before).
         *   Read by inference thread during react_run.
         *   .done is atomic_int — polled by main thread, set by infer thread.
         *   Main thread only touches these again AFTER pthread_join. */
        static infer_args_t iargs;
        static playbook_args_t pargs_tui;
        char *pending_redirect = NULL;  /* stashed query when user types during inference */
        while (running) {
            /* Check if playbook thread completed */
            if (inferring == 3 && pargs_tui.done) {
                pthread_join(infer_tid, NULL);
                pthread_mutex_lock(&ui->mtx);
                if (pargs_tui.playbook_ok) {
                    char done_msg[256];
                    snprintf(done_msg, sizeof(done_msg), "Playbook '%s' complete (%d passes)",
                             pargs_tui.playbook->name, pargs_tui.playbook->n_passes);
                    ui_state_set_status(ui, STATUS_DONE, done_msg);
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "Playbook failed");
                }
                pthread_mutex_unlock(&ui->mtx);
                playbook_free(pargs_tui.playbook);
                pargs_tui.playbook = NULL;
                inferring = 0;
                tui_render(ui);
            }

            /* Check if inference thread is paused and waiting for redirect.
             * Update status bar so user knows they can type a new query. */
            if (inferring == 1 && react.pause_waiting &&
                ui->status != STATUS_READY) {
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_READY,
                    "Paused (type query to redirect, Space to resume)");
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
            }

            /* Check if inference thread completed */
            if (inferring == 1 && iargs.done) {
                pthread_join(infer_tid, NULL);
                inferring = 0;
                tools.react_loop++;  /* increment for next query */
                /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
                memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
                char *result = iargs.result;
                pthread_mutex_lock(&ui->mtx);
                if (result) {
                    ui_state_set_status(ui, STATUS_DONE, "Done");
                    /* Refresh journal view to show completed query */
                    ui_state_load_journal(ui, journal);
                } else if (react.pause_requested && !pending_redirect) {
                    /* FIX #3: Only enter pause path if no redirect is pending.
                     * Race condition: if user types while inference is finishing,
                     * the main thread may set pause_requested=1 after the inference
                     * thread has already completed (TOCTOU on iargs.done).
                     * When pending_redirect is set, the user intended to start a
                     * new query, not pause — so skip the pause path and let the
                     * redirect be dispatched on the next iteration (line 1201). */
                    react.pause_requested = 0;  /* reset for next run */
                    react.paused = 1;
                    ui_state_set_status(ui, STATUS_READY,
                        "Paused (Space to resume, type query to redirect)");
                } else {
                    /* Clear stale pause_requested if redirect will take over */
                    react.pause_requested = 0;
                    ui_state_set_status(ui, pending_redirect ? STATUS_READY : STATUS_ERROR,
                        pending_redirect ? "Redirecting..." : "No result");
                }
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                if (react.last_query) free(react.last_query);
                if (react.last_result) free(react.last_result);
                react.last_query = strdup(iargs.query);
                react.last_result = result ? strdup(result) : NULL;
                free(result);
                free(iargs.query);
                iargs.query = NULL;
                pthread_mutex_lock(&ui->mtx);
                ui_state_load_journal(ui, journal);
                ui_state_set_status(ui, STATUS_READY, "Ready");
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
            }
            char *submitted_query = NULL;
            int rc = tui_input(ui, &submitted_query);

            if (rc == -1) {
                /* Quit requested */
                running = 0;
                break;
            }

            /* Auto-dispatch stashed redirect: when inference was paused
             * by user input (pending_redirect != NULL) and either:
             * (a) the thread has joined (!inferring), or
             * (b) the thread is paused and waiting on the condvar.
             * In both cases, inject the stashed query immediately. */
            if (!submitted_query && pending_redirect &&
                (!inferring || react.pause_waiting)) {
                submitted_query = pending_redirect;
                pending_redirect = NULL;
            }

            if (submitted_query) {
                /* Check if inference thread is waiting for user_ask answer */
                if (inferring && react.user_ask_pending) {
                    /* Pass the user's answer to the waiting react loop.
                     * All writes to shared state (user_ask_answer, user_ask_pending)
                     * are done inside the mutex to ensure proper happens-before
                     * ordering with the inference thread's condvar wait. */
                    pthread_mutex_lock(&react.user_ask_mutex);
                    free(react.user_ask_answer);
                    react.user_ask_answer = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    react.user_ask_pending = 0;  /* unblock the react loop */
                    pthread_cond_signal(&react.user_ask_cond);
                    pthread_mutex_unlock(&react.user_ask_mutex);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Check if inference thread is paused and waiting for redirect */
                if (inferring && react.pause_waiting) {
                    /* Pass the user's redirect query to the waiting react loop.
                     * This preserves the full chat context (no new react_run). */
                    pthread_mutex_lock(&react.pause_mutex);
                    free(react.pause_query);
                    react.pause_query = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    pthread_cond_signal(&react.pause_cond);
                    pthread_mutex_unlock(&react.pause_mutex);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Resuming...");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Handle exit/quit commands */
                if (strcmp(submitted_query, "quit") == 0 ||
                    strcmp(submitted_query, "exit") == 0 ||
                    strcmp(submitted_query, "/quit") == 0 ||
                    strcmp(submitted_query, "/exit") == 0) {
                    free(submitted_query);
                    running = 0;
                    break;
                }

                /* FIX #4: Guard slash commands that access shared inference state.
                 * During inference, only user_ask, exit/quit, and regular queries
                 * (which get stashed as pending_redirect) are safe. Slash commands
                 * like /fork read aliases->next_seq which is written by the inference
                 * thread without synchronization. Defer them until inference completes. */
                if (inferring && submitted_query[0] == '/'
                    && strncmp(submitted_query, "/quit", 5) != 0
                    && strncmp(submitted_query, "/exit", 5) != 0) {
                    /* Stash as pending_redirect — will execute after join */
                    free(pending_redirect);
                    pending_redirect = submitted_query;
                    submitted_query = NULL;
                    react.pause_requested = 1;
                    provider->abort_retry = 1;  /* wake provider_sleep early */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Pausing to handle command…");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Handle /fork command */
                if (strncmp(submitted_query, "/fork ", 6) == 0) {
                    int fork_step = atoi(submitted_query + 6);
                    if (fork_step > 0) {
                        char *new_dir = create_session_dir(nash_dir);
                        /* Copy journal lines where step <= fork_step */
                        char src_j[NASH_PATH_MAX], dst_j[NASH_PATH_MAX];
                        snprintf(src_j, sizeof(src_j), "%s/journal.jsonl", session_dir);
                        snprintf(dst_j, sizeof(dst_j), "%s/journal.jsonl", new_dir);
                        FILE *sf = fopen(src_j, "r");
                        FILE *df = fopen(dst_j, "w");
                        if (sf && df) {
                            char jl[NASH_LINE_MAX];
                            while (fgets(jl, sizeof(jl), sf)) {
                                cJSON *e = cJSON_Parse(jl);
                                if (e) {
                                    int s = (int)cJSON_GetNumberValue(
                                        cJSON_GetObjectItem(e, "step"));
                                    if (s <= fork_step) fputs(jl, df);
                                    cJSON_Delete(e);
                                }
                            }
                        }
                        if (sf) fclose(sf);
                        if (df) fclose(df);
                        /* Copy symlinks from all react loops.
                         * Use the alias map's next_seq as the upper bound —
                         * it tracks the actual number of aliases created. */
                        int max_alias = tools.aliases ? tools.aliases->next_seq : fork_step + 5;
                        for (int loop = 0; loop <= tools.react_loop; loop++) {
                            for (int i = 0; i <= max_alias; i++) {
                                char ref[32], sl[NASH_PATH_MAX], tgt[NASH_PATH_MAX], dl[NASH_PATH_MAX];
                                snprintf(ref, sizeof(ref), "R%dS%d",
                                         loop, i);
                                snprintf(sl, sizeof(sl), "%s/%s", session_dir, ref);
                                ssize_t n = readlink(sl, tgt, sizeof(tgt) - 1);
                                if (n > 0) {
                                    tgt[n] = '\0';
                                    snprintf(dl, sizeof(dl), "%s/%s", new_dir, ref);
                                    symlink(tgt, dl);
                                }
                            }
                        }
                        /* Write checkpoint */
                        cJSON *cp = cJSON_CreateObject();
                        cJSON_AddNumberToObject(cp, "version", 1);
                        cJSON_AddNumberToObject(cp, "step", fork_step);
                        cJSON_AddNumberToObject(cp, "react_loop", tools.react_loop);
                        if (react.last_query)
                            cJSON_AddStringToObject(cp, "user_query", react.last_query);
                        /* Legacy scratchpad string removed — checkpoint restore
                         * falls back to scratchpad_parse from checkpoint JSON
                         * if section files don't exist (very old sessions). */
                        char *cpj = cJSON_Print(cp);
                        char cp_path[NASH_PATH_MAX];
                        snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.json", new_dir);
                        write_file(cp_path, cpj, strlen(cpj));
                        free(cpj);
                        cJSON_Delete(cp);
                        /* Persist scratchpad to the forked session directory
                         * so it survives resume. Without this, the forked
                         * session starts with an empty scratchpad. */
                        scratchpad_save(&tools.scratch, new_dir);
                        /* Switch to forked session */
                        journal_free(journal);
                        free(session_dir);
                        session_dir = new_dir;
                        journal = journal_new(session_dir);
                        tools.journal = journal;
                        tools.session_dir = session_dir;
                        alias_map_clear(tools.aliases);
                        ui_state_set_status(ui, STATUS_READY,
                            "Forked — ready for new query");
                        tui_render(ui);
                    }
                    free(submitted_query);
                    continue;
                }

                /* Handle /name command — create a named symlink to the current session */
                if (strncmp(submitted_query, "/name ", 6) == 0) {
                    const char *name = submitted_query + 6;
                    /* Validate: non-empty, no slashes, reasonable length */
                    if (strlen(name) == 0 || strlen(name) > 255 ||
                        strchr(name, '/') != NULL || strchr(name, '\n') != NULL) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/name: invalid name (no slashes, max 255 chars)");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }
                    char sessions_base[1024], link_path[1088];
                    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
                    snprintf(link_path, sizeof(link_path), "%s/%s", sessions_base, name);
                    /* Remove existing symlink if present */
                    unlink(link_path);
                    /* Create symlink */
                    if (symlink(session_dir, link_path) == 0) {
                        /* Extract just the session ID (basename) for display */
                        const char *session_id = strrchr(session_dir, '/');
                        session_id = session_id ? session_id + 1 : session_dir;
                        pthread_mutex_lock(&ui->mtx);
                        char status[512];
                        snprintf(status, sizeof(status),
                            "Named session: %s → %s", name, session_id);
                        ui_state_set_status(ui, STATUS_READY, status);
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                    } else {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/name: failed to create symlink");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                    }
                    free(submitted_query);
                    continue;
                }

                /* Handle /cwd command — change working directory, create if needed */
                if (strncmp(submitted_query, "/cwd ", 5) == 0) {
                    const char *dir = submitted_query + 5;
                    /* Skip leading whitespace */
                    while (*dir == ' ') dir++;
                    if (*dir == '\0') {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/cwd: missing directory argument");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }
                    /* Create directory if it doesn't exist */
                    struct stat st;
                    if (stat(dir, &st) != 0) {
                        if (mkdir_p(dir, 0755) != 0) {
                            char errbuf[512];
                            snprintf(errbuf, sizeof(errbuf),
                                "/cwd: failed to create '%s': %s", dir, strerror(errno));
                            pthread_mutex_lock(&ui->mtx);
                            ui_state_set_status(ui, STATUS_ERROR, errbuf);
                            pthread_mutex_unlock(&ui->mtx);
                            tui_render(ui);
                            free(submitted_query);
                            continue;
                        }
                    }
                    /* Change to the directory */
                    if (chdir(dir) != 0) {
                        char errbuf[512];
                        snprintf(errbuf, sizeof(errbuf),
                            "/cwd: failed to chdir to '%s': %s", dir, strerror(errno));
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR, errbuf);
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }
                    /* Show success with resolved path */
                    char resolved[4096];
                    if (!getcwd(resolved, sizeof(resolved)))
                        snprintf(resolved, sizeof(resolved), "%s", dir);
                    char status_msg[4112];
                    snprintf(status_msg, sizeof(status_msg),
                        "CWD: %s", resolved);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_READY, status_msg);
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* Handle /dream command — alias for /play dream */
                if (strcmp(submitted_query, "/dream") == 0) {
                    free(submitted_query);
                    submitted_query = NULL;

                    if (inferring) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                                            "Wait for inference to finish");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        continue;
                    }

                    /* Load dream playbook from ~/.nash/playbooks/dream.yaml.
                     * If not found, write the default and load it. */
                    char pb_path[NASH_PATH_MAX];
                    snprintf(pb_path, sizeof(pb_path), "%s/playbooks/dream.yaml", nash_dir);
                    playbook_t *dream_pb = playbook_load(pb_path);
                    if (!dream_pb) {
                        playbook_write_default_dream(pb_path);
                        dream_pb = playbook_load(pb_path);
                    }
                    if (!dream_pb) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "Cannot load dream playbook");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        continue;
                    }

                    pargs_tui = (playbook_args_t){
                        .playbook = dream_pb,
                        .nash_dir = nash_dir,
                        .store = shared_store,
                        .memory = memory,
                        .cfg = cfg,
                        .provider = provider,
                        .server_model = server_model,
                        .ui = ui,
                        .playbook_ok = 0,
                        .done = 0,
                    };
                    provider->abort_retry = 0;  /* reset before new inference */
                    pthread_create(&infer_tid, NULL, playbook_worker, &pargs_tui);
                    inferring = 3;
                    tui_render(ui);
                    continue;
                }

                /* Handle /play command — run a playbook */
                if (strncmp(submitted_query, "/play ", 6) == 0) {
                    const char *arg = submitted_query + 6;
                    while (*arg == ' ') arg++;

                    if (inferring) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                                            "Wait for inference to finish");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    if (strcmp(arg, "list") == 0) {
                        /* List available playbooks */
                        int pb_count = 0;
                        playbook_t **pbs = playbook_list(nash_dir, &pb_count);
                        str_t display = str_new(1024);
                        str_appendf(&display, "# Available Playbooks\n\n");
                        if (pb_count == 0) {
                            str_appendf(&display, "No playbooks found in %s/playbooks/\n", nash_dir);
                        } else {
                            for (int i = 0; i < pb_count; i++) {
                                str_appendf(&display, "- **%s**: %s (%d passes)\n",
                                    pbs[i]->name,
                                    pbs[i]->description ? pbs[i]->description : "",
                                    pbs[i]->n_passes);
                                playbook_free(pbs[i]);
                            }
                            free(pbs);
                        }
                        char *banner = str_steal(&display);
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_banner(ui, banner);
                        ui_state_set_status(ui, STATUS_READY, "Playbook list");
                        pthread_mutex_unlock(&ui->mtx);
                        free(banner);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    /* Load playbook by name or path */
                    char pb_path[NASH_PATH_MAX];
                    if (strchr(arg, '/') || strchr(arg, '.')) {
                        snprintf(pb_path, sizeof(pb_path), "%s", arg);
                    } else {
                        snprintf(pb_path, sizeof(pb_path), "%s/playbooks/%s.yaml",
                                 nash_dir, arg);
                    }
                    playbook_t *pb = playbook_load(pb_path);
                    if (!pb) {
                        pthread_mutex_lock(&ui->mtx);
                        char errmsg[NASH_PATH_MAX + 64];
                        snprintf(errmsg, sizeof(errmsg),
                                 "/play: cannot load playbook '%.4080s'", pb_path);
                        ui_state_set_status(ui, STATUS_ERROR, errmsg);
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    pargs_tui = (playbook_args_t){
                        .playbook = pb,
                        .nash_dir = nash_dir,
                        .store = shared_store,
                        .memory = memory,
                        .cfg = cfg,
                        .provider = provider,
                        .server_model = server_model,
                        .ui = ui,
                        .playbook_ok = 0,
                        .done = 0,
                    };
                    provider->abort_retry = 0;  /* reset before new inference */
                    pthread_create(&infer_tid, NULL, playbook_worker, &pargs_tui);
                    inferring = 3;
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* Handle /runs command — list/show playbook run logs */
                if (strcmp(submitted_query, "/runs") == 0 ||
                    strcmp(submitted_query, "/runs list") == 0 ||
                    strncmp(submitted_query, "/runs ", 6) == 0) {
                    const char *sub = submitted_query + 5;
                    while (*sub == ' ') sub++;

                    int show_detail = 0;
                    const char *show_id = NULL;
                    if (strncmp(sub, "show ", 5) == 0) {
                        show_detail = 1;
                        show_id = sub + 5;
                        while (*show_id == ' ') show_id++;
                    }

                    char rdir[NASH_PATH_MAX];
                    snprintf(rdir, sizeof(rdir), "%s/runs", nash_dir);

                    if (show_detail && show_id && *show_id) {
                        /* /runs show <id> — display a specific run log */
                        char rpath[NASH_PATH_MAX + NASH_PATH_MAX];
                        /* Try exact filename, or append .jsonl */
                        if (strstr(show_id, ".jsonl"))
                            snprintf(rpath, sizeof(rpath), "%s/%s", rdir, show_id);
                        else
                            snprintf(rpath, sizeof(rpath), "%s/%s.jsonl", rdir, show_id);

                        FILE *rf = fopen(rpath, "r");
                        if (!rf) {
                            pthread_mutex_lock(&ui->mtx);
                            ui_state_set_status(ui, STATUS_ERROR,
                                "/runs show: run log not found");
                            pthread_mutex_unlock(&ui->mtx);
                            tui_render(ui);
                            free(submitted_query);
                            continue;
                        }

                        str_t display = str_new(2048);
                        str_appendf(&display, "# Run Log: %s\n\n", show_id);
                        char line[NASH_LINE_MAX];
                        while (fgets(line, sizeof(line), rf)) {
                            cJSON *ev = cJSON_Parse(line);
                            if (!ev) continue;
                            const char *e = cJSON_GetStringValue(
                                cJSON_GetObjectItem(ev, "e"));
                            if (!e) { cJSON_Delete(ev); continue; }

                            if (strcmp(e, "start") == 0) {
                                str_appendf(&display, "**Playbook**: %s  \n",
                                    cJSON_GetStringValue(
                                        cJSON_GetObjectItem(ev, "pb")));
                                str_appendf(&display, "**Passes**: %d\n\n",
                                    (int)cJSON_GetNumberValue(
                                        cJSON_GetObjectItem(ev, "n")));
                            } else if (strcmp(e, "pass") == 0) {
                                int idx = (int)cJSON_GetNumberValue(
                                    cJSON_GetObjectItem(ev, "i"));
                                const char *label = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(ev, "l"));
                                const char *sid = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(ev, "sid"));
                                str_appendf(&display,
                                    "- **Pass %d**: %s\n  session: `%s`\n",
                                    idx + 1, label ? label : "?",
                                    sid ? sid : "?");
                            } else if (strcmp(e, "done") == 0) {
                                const char *st = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(ev, "st"));
                                double ts = cJSON_GetNumberValue(
                                    cJSON_GetObjectItem(ev, "ts"));
                                str_appendf(&display, "  status: %s  (%.0f)\n",
                                    st ? st : "?", ts);
                            } else if (strcmp(e, "end") == 0) {
                                const char *st = cJSON_GetStringValue(
                                    cJSON_GetObjectItem(ev, "st"));
                                str_appendf(&display, "\n**Result**: %s\n",
                                    st ? st : "?");
                            }
                            cJSON_Delete(ev);
                        }
                        fclose(rf);

                        char *banner = str_steal(&display);
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_banner(ui, banner);
                        ui_state_set_status(ui, STATUS_READY, "Run log");
                        pthread_mutex_unlock(&ui->mtx);
                        free(banner);
                        tui_render(ui);
                    } else {
                        /* /runs or /runs list — list all run logs */
                        DIR *d = opendir(rdir);
                        str_t display = str_new(2048);
                        str_appendf(&display, "# Playbook Runs\n\n");

                        if (!d) {
                            str_appendf(&display, "No runs yet (%s/runs/ not found)\n", nash_dir);
                        } else {
                            struct dirent *ent;
                            int count = 0;
                            /* FIX #16: Use dynamic array instead of fixed char *names[1024].
                             * Previously, beyond 1024 runs entries were silently dropped.
                             * Also replaced O(N²) bubble sort with qsort. */
                            int names_cap = 128;
                            char **names = malloc(sizeof(char *) * (size_t)names_cap);
                            int nnames = 0;
                            if (names) {
                                while ((ent = readdir(d))) {
                                    int nlen = (int)strlen(ent->d_name);
                                    if (nlen > 6 && strcmp(ent->d_name + nlen - 6, ".jsonl") == 0) {
                                        if (nnames >= names_cap) {
                                            names_cap *= 2;
                                            char **tmp = realloc(names, sizeof(char *) * (size_t)names_cap);
                                            if (!tmp) break;  /* stop collecting on OOM */
                                            names = tmp;
                                        }
                                        names[nnames++] = strdup(ent->d_name);
                                    }
                                }
                            }
                            closedir(d);
                            /* Sort descending (newest first by epoch name) */
                            if (nnames > 1)
                                qsort(names, (size_t)nnames, sizeof(char *), cmp_str_desc);
                            for (int i = 0; i < nnames; i++) {
                                char fpath[NASH_PATH_MAX + NASH_PATH_MAX];
                                snprintf(fpath, sizeof(fpath), "%s/%s", rdir, names[i]);
                                FILE *rf = fopen(fpath, "r");
                                if (rf) {
                                    char line[NASH_LINE_MAX];
                                    if (fgets(line, sizeof(line), rf)) {
                                        cJSON *ev = cJSON_Parse(line);
                                        if (ev) {
                                            const char *pb_name = cJSON_GetStringValue(
                                                cJSON_GetObjectItem(ev, "pb"));
                                            int n = (int)cJSON_GetNumberValue(
                                                cJSON_GetObjectItem(ev, "n"));
                                            /* Check if run completed by scanning for end event */
                                            const char *status_str = "running";
                                            char lastline[NASH_LINE_MAX];
                                            lastline[0] = '\0';
                                            while (fgets(lastline, sizeof(lastline), rf));
                                            if (lastline[0]) {
                                                cJSON *last = cJSON_Parse(lastline);
                                                if (last) {
                                                    const char *le = cJSON_GetStringValue(
                                                        cJSON_GetObjectItem(last, "e"));
                                                    if (le && strcmp(le, "end") == 0) {
                                                        const char *st = cJSON_GetStringValue(
                                                            cJSON_GetObjectItem(last, "st"));
                                                        status_str = (st && strcmp(st, "ok") == 0)
                                                            ? "ok" : "fail";
                                                    }
                                                    cJSON_Delete(last);
                                                }
                                            }
                                            /* Strip .jsonl for display */
                                            char id[256];
                                            snprintf(id, sizeof(id), "%s", names[i]);
                                            char *dot = strstr(id, ".jsonl");
                                            if (dot) *dot = '\0';
                                            str_appendf(&display,
                                                "%d. **%s** — %s (%d passes) [%s]\n",
                                                ++count, id,
                                                pb_name ? pb_name : "?",
                                                n, status_str);
                                            cJSON_Delete(ev);
                                        }
                                    }
                                    fclose(rf);
                                }
                                free(names[i]);
                            }
                            free(names);  /* FIX #16: free dynamic array */
                            if (count == 0)
                                str_appendf(&display, "No runs found\n");
                            else
                                str_appendf(&display, "\nUse `/runs show <id>` for details\n");
                        }

                        char *banner = str_steal(&display);
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_banner(ui, banner);
                        ui_state_set_status(ui, STATUS_READY, "Run list");
                        pthread_mutex_unlock(&ui->mtx);
                        free(banner);
                        tui_render(ui);
                    }
                    free(submitted_query);
                    continue;
                }

                /* Handle /memory_recall <query> — semantic memory search.
                 * Usage: /memory_recall "what I know about X"
                 * Searches the memory store using the same hybrid scoring
                 * (embeddings + substring) that powers agent recall.
                 * Displays results directly in the TUI main pane. */
                if (strncmp(submitted_query, "/memory_recall ", 15) == 0) {
                    const char *query = submitted_query + 15;
                    /* Strip optional quotes */
                    int n = strlen(query);
                    const char *q_start = query;
                    const char *q_end = query + n;
                    if (n >= 2 && q_start[0] == '"' && q_end[-1] == '"') {
                        q_start++; q_end--;
                    } else if (n >= 2 && q_start[0] == '\'' && q_end[-1] == '\'') {
                        q_start++; q_end--;
                    }
                    int q_len = q_end - q_start;
                    if (q_len == 0) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/memory_recall: usage: /memory_recall \"query\"");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }
                    char qbuf[NASH_PATH_MAX];
                    memcpy(qbuf, q_start, q_len);
                    qbuf[q_len] = '\0';

                    memory_results_t results = memory_recall(memory, qbuf, 10);
                    if (results.count == 0) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_READY,
                            "No memories matched query");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    /* Format results as a readable display string */
                    str_t display = str_new(4096);
                    str_appendf(&display, "# Memory Recall: \"%.*s\"\n\n", q_len, q_start);
                    str_appendf(&display, "Found %d matching entries:\n\n", results.count);
                    for (int i = 0; i < results.count; i++) {
                        memory_entry_t *e = &results.entries[i];
                        str_appendf(&display,
                            "### %d. %s  (score: %.3f)\n\n",
                            i + 1, e->key, e->relevance);
                        str_append_cstr(&display, e->value);
                        str_append_cstr(&display, "\n\n");
                        /* Tags */
                        (void)0; /* tags removed */
                        /* Validation score */
                        double vscore = (e->recall_hits + 1.0) /
                                        (e->recall_hits + e->recall_misses + 2.0);
                        str_appendf(&display,
                            "hits: %d misses: %d vscore: %.2f pinned: %s\n\n",
                            e->recall_hits, e->recall_misses,
                            vscore, e->pinned ? "yes" : "no");
                        str_append_cstr(&display, "---\n\n");
                    }

                    char *banner = str_steal(&display);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_banner(ui, banner);
                    ui_state_set_status(ui, STATUS_READY,
                        "Memory recall complete");
                    pthread_mutex_unlock(&ui->mtx);
                    free(banner);
                    memory_results_free(&results);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* Handle /? search — intercept Enter to prevent inference.
                 * Search results are already displayed live in main pane.
                 * On Enter, clear search and input buffer. */
                if (strncmp(submitted_query, "/?", 2) == 0) {
                    pthread_mutex_lock(&ui->mtx);
                    if (ui->search_active) {
                        ui_state_search(ui, NULL);  /* clear search */
                    }
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* If paused, any query (typed or Space-resume) clears the paused flag.
                 * checkpoint_restore will inject the query into restored context. */
                if (react.paused) {
                    react.paused = 0;
                }

                /* Handle /continue: resume from checkpoint with original query.
                 * If user types "continue" or "/continue", read the original
                 * user_query from checkpoint.json and use that instead.
                 * If user types anything else, it passes through as-is —
                 * checkpoint_restore will inject it into the restored context,
                 * effectively saying "resume but with this new instruction." */
                if (strcmp(submitted_query, "continue") == 0 ||
                    strcmp(submitted_query, "/continue") == 0) {
                    char *orig = checkpoint_read_query(session_dir);
                    if (orig) {
                        free(submitted_query);
                        submitted_query = orig;
                    }
                    /* If no checkpoint exists, "continue" falls through as a
                     * regular query — the LLM will see "continue" and can
                     * interpret it in context (e.g., continue a conversation). */
                }

                /* Regular query — spawn inference in background thread */
                if (inferring) {
                    /* User typed while inference is running — auto-pause and
                     * stash the query.  When the react loop breaks at the next
                     * step boundary the stashed query is dispatched immediately,
                     * eliminating the two-step "Space then type" dance. */
                    free(pending_redirect);  /* replace any earlier stash */
                    pending_redirect = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    react.pause_requested = 1;
                    provider->abort_retry = 1;  /* wake provider_sleep early */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Pausing after current step…");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }
                /* ── Tree branching: determine parent_loop ── */
                int parent_loop = -1;  /* default: root */
                if (tools.react_loop > 0) {
                    int viewed_loop = -1;
                    pthread_mutex_lock(&ui->mtx);
                    const char *viewing = ui->current_filepath;
                    if (viewing) {
                        /* Find last '/' or use full string */
                        const char *base = strrchr(viewing, '/');
                        base = base ? base + 1 : viewing;
                        sscanf(base, "reactR%d.md", &viewed_loop);
                    }
                    pthread_mutex_unlock(&ui->mtx);
                    if (viewed_loop >= 0) {
                        parent_loop = viewed_loop;
                    } else {
                        /* Viewing session.md or something else → linear follow-up */
                        parent_loop = tools.react_loop - 1;
                    }
                }
                react.parent_loop = parent_loop;

                /* If branching (parent != latest completed loop), override
                 * result.txt so [PREVIOUS RESULT] matches the branch point.
                 * Scratchpad filtering is done non-destructively in react.c
                 * at injection time (using parent_loop + journal ancestor chain). */
                int is_branch = (parent_loop >= 0 &&
                                 parent_loop != tools.react_loop - 1);
                if (is_branch) {
                    /* Override result.txt with parent's result from scratchpad */
                    char sec_name[32];
                    snprintf(sec_name, sizeof(sec_name), "R%d_result", parent_loop);
                    int idx = scratchpad_find(&tools.scratch, sec_name);
                    if (idx >= 0) {
                        char rpath[NASH_PATH_MAX];
                        snprintf(rpath, sizeof(rpath), "%s/result.txt", session_dir);
                        write_file(rpath, tools.scratch.sections[idx].content,
                                   strlen(tools.scratch.sections[idx].content));
                    }
                }

                /* Build the final query string — add branch context hint if branching */
                char *final_query;
                if (is_branch) {
                    /* Find parent's query text from journal for context */
                    char parent_query_text[256] = "";
                    char jpath2[NASH_PATH_MAX];
                    snprintf(jpath2, sizeof(jpath2), "%s/journal.jsonl", session_dir);
                    FILE *jf2 = fopen(jpath2, "r");
                    if (jf2) {
                        char jline2[NASH_LINE_MAX];
                        while (fgets(jline2, sizeof(jline2), jf2)) {
                            cJSON *entry = cJSON_Parse(jline2);
                            if (!entry) continue;
                            const char *jtool = cJSON_GetStringValue(
                                cJSON_GetObjectItem(entry, "tool"));
                            int rl = (int)cJSON_GetNumberValue(
                                cJSON_GetObjectItem(entry, "react_loop"));
                            if (jtool && strcmp(jtool, "query") == 0 && rl == parent_loop) {
                                cJSON *params = cJSON_GetObjectItem(entry, "params");
                                cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
                                if (text && text->valuestring) {
                                    snprintf(parent_query_text, sizeof(parent_query_text),
                                             "%.250s", text->valuestring);
                                }
                                cJSON_Delete(entry);
                                break;
                            }
                            cJSON_Delete(entry);
                        }
                        fclose(jf2);
                    }
                    size_t fqlen = strlen(submitted_query) + 512;
                    final_query = malloc(fqlen);
                    if (final_query) {
                        snprintf(final_query, fqlen,
                            "[Branched from R%d: \"%s\"]\n%s",
                            parent_loop, parent_query_text, submitted_query);
                    } else {
                        final_query = strdup(submitted_query);
                    }
                } else {
                    final_query = strdup(submitted_query);
                }

                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                ui_state_add_query(ui, submitted_query);
                ui->current_react_loop = tools.react_loop;
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                iargs = (infer_args_t){
                    .react = &react, .query = final_query,
                    .ui = ui, .result = NULL, .done = 0,
                };
                provider->abort_retry = 0;  /* reset before new inference */
                free(submitted_query);  /* strdup'd into final_query; ui_state_add_query also strdup'd */
                pthread_create(&infer_tid, NULL, infer_worker, &iargs);
                inferring = 1;
                tui_render(ui);
            }

            /* Always render if dirty */
            if (ui->dirty) tui_render(ui);

            /* Small sleep to avoid busy-waiting when no input */
            /* Auto-refresh MD every 100ms during inference */
            if (inferring) {
                static struct timespec last_refresh = {0, 0};
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_refresh.tv_sec) * 1000
                                + (now.tv_nsec - last_refresh.tv_nsec) / 1000000;
                if (elapsed_ms >= 100) {
                    last_refresh = now;
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_reload_file(ui);
                    ui->dirty = 1;
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                }
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms */
        }

        /* If inference thread is paused on condvar, wake it up so it can exit.
         * Send a "quit" redirect that will cause the loop to take one more step
         * and then exit naturally (or we just signal to unblock it). */
        if (inferring && react.pause_waiting) {
            pthread_mutex_lock(&react.pause_mutex);
            free(react.pause_query);
            react.pause_query = strdup("quit");
            react.pause_requested = 0;  /* clear so loop doesn't re-pause */
            pthread_cond_signal(&react.pause_cond);
            pthread_mutex_unlock(&react.pause_mutex);
        }
        /* If inference thread is paused on user_ask condvar, unblock it too */
        if (inferring && react.user_ask_pending) {
            pthread_mutex_lock(&react.user_ask_mutex);
            free(react.user_ask_answer);
            react.user_ask_answer = strdup("(quit)");
            react.user_ask_pending = 0;
            pthread_cond_signal(&react.user_ask_cond);
            pthread_mutex_unlock(&react.user_ask_mutex);
        }
        if (inferring) {
            pthread_join(infer_tid, NULL);
            inferring = 0;
            free(iargs.result);
            free(iargs.query);
        }
        free(pending_redirect);  /* clean up any un-dispatched redirect */
        tui_shutdown();
        nash_log_set_ui(NULL);  /* disable TUI error routing */
        ui_state_free(ui);

        /* Save scratchpad */
        if (tools.scratch.count > 0) {
            scratchpad_save(&tools.scratch, session_dir);
        }
        session_cleanup(&tools, &react, journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
    }
    printf("Bye.\n");
    web_search_cleanup();  /* tear down auto-started SearXNG container */
    cleanup_globals(shared_store, memory, provider, nash_dir, props_json, server_model, cfg);
    return 0;
}

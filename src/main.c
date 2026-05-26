#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
#include "memory.h"
#include "ui_state.h"
#include "tui.h"

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
static char *create_session_dir(const char *nash_dir) {
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

/* Print banner: header art + server props + client overrides */
static void print_banner(const config_t *cfg, const char *props_json,
                         const char *nash_dir) {
    /* ASCII art header with gradient green ANSI colors */
    printf("\n");
    printf("  \033[1m\033[38;2;80;255;120m _  _    __   ____  _  _  ____  __    __   \033[0m\n");
    printf("  \033[1m\033[38;2;60;220;100m( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \033[0m\n");
    printf("  \033[1m\033[38;2;40;190;80m ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \033[0m\n");
    printf("  \033[1m\033[38;2;30;160;60m(___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \033[0m\n");
    printf("\n");
    printf("  \033[38;2;70;200;90m--------- * New Agentic Shell * ---------\033[0m\n");
    printf("\n");

    /* 1. Provider / server info */
    const char *ptype = cfg->provider.type;
    int is_api = ptype && (strcmp(ptype, "vertex") == 0 ||
                           strcmp(ptype, "anthropic") == 0 ||
                           strcmp(ptype, "openai") == 0);

    if (is_api) {
        /* API provider: show provider type, model, and relevant details */
        printf("provider: %s\n", ptype);
        printf("  model:    %s\n",
               cfg->provider.model_id ? cfg->provider.model_id : "(not set)");
        if (cfg->provider.project_id)
            printf("  project:  %s\n", cfg->provider.project_id);
        if (cfg->provider.region)
            printf("  region:   %s\n", cfg->provider.region);
        if (cfg->provider.context_size > 0)
            printf("  ctx:      %d tok (%dk)\n",
                   cfg->provider.context_size,
                   cfg->provider.context_size / 1024);
        printf("\n");
    } else if (!props_json) {
        printf("server: %s (props unavailable)\n\n",
               cfg->api_base ? cfg->api_base : "(none)");
    } else {
        cJSON *props = cJSON_Parse(props_json);
        if (!props) {
            printf("server: %s (props parse error)\n\n",
                   cfg->api_base ? cfg->api_base : "(none)");
        } else {
            cJSON *gs = cJSON_GetObjectItem(props, "default_generation_settings");
            cJSON *params = gs ? cJSON_GetObjectItem(gs, "params") : NULL;
            cJSON *caps = cJSON_GetObjectItem(props, "chat_template_caps");
            cJSON *mods = cJSON_GetObjectItem(props, "modalities");
            int n_ctx = (int)jnum(gs, "n_ctx", 0);

            printf("server: %s\n", cfg->api_base ? cfg->api_base : "(none)");
            printf("  model:    %s\n", jstr(props, "model_alias", "(unknown)"));
            printf("  build:    %s\n", jstr(props, "build_info", "?"));
            printf("  ctx:      %d tok (%dk)", n_ctx, n_ctx / 1024);
            printf(" | slots: %d\n", (int)jnum(props, "total_slots", 0));

            if (params) {
                printf("  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f",
                       jnum(params, "temperature", 0),
                       (int)jnum(params, "top_k", 0),
                       jnum(params, "top_p", 0),
                       jnum(params, "min_p", 0));
                double rp = jnum(params, "repeat_penalty", 1.0);
                if (rp != 1.0) printf(" rep=%.1f", rp);
                printf("\n");
            }

            printf("  caps:     tools=%s vision=%s reasoning=%s\n",
                   caps && jbool(caps, "supports_tools", 0) ? "yes" : "no",
                   mods && jbool(mods, "vision", 0) ? "yes" : "no",
                   params ? jstr(params, "reasoning_format", "none") : "?");

            printf("\n");
            cJSON_Delete(props);
        }
    }

    /* 2. Client config */
    const char *think_str = cfg->thinking.mode == THINKING_ON ? "yes" :
                            cfg->thinking.mode == THINKING_EDRM ? "edrm" : "no";
    printf("client: temp=%.1f max_tokens=%d json_mode=%s thinking=%s stream=%s\n",
           cfg->temperature, cfg->max_tokens,
           cfg->json_mode ? "on" : "off",
           think_str,
           cfg->stream ? "on" : "off");
    printf("data:   %s\n", nash_dir);
    printf("\n");
}

/* Build banner as a string for ncurses TUI (no ANSI escapes) */
static char *build_banner_string(const config_t *cfg, const char *props_json,
                                  const char *nash_dir, const char *session_dir) {
    str_t s = str_new(2048);

    str_append_cstr(&s, "\n");
    str_append_cstr(&s, "   _  _    __   ____  _  _  ____  __    __   \n");
    str_append_cstr(&s, "  ( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \n");
    str_append_cstr(&s, "   ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \n");
    str_append_cstr(&s, "  (___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \n");
    str_append_cstr(&s, "\n");
    str_append_cstr(&s, "  --------- * New Agentic Shell * ---------\n");
    str_append_cstr(&s, "\n");

    const char *bptype = cfg->provider.type;
    int bis_api = bptype && (strcmp(bptype, "vertex") == 0 ||
                             strcmp(bptype, "anthropic") == 0 ||
                             strcmp(bptype, "openai") == 0);

    if (bis_api) {
        str_appendf(&s, "provider: %s\n", bptype);
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
                str_appendf(&s, "  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f\n",
                            jnum(params, "temperature", 0),
                            (int)jnum(params, "top_k", 0),
                            jnum(params, "top_p", 0),
                            jnum(params, "min_p", 0));
            }

            str_appendf(&s, "  caps:     tools=%s vision=%s reasoning=%s\n",
                        caps && jbool(caps, "supports_tools", 0) ? "yes" : "no",
                        mods && jbool(mods, "vision", 0) ? "yes" : "no",
                        params ? jstr(params, "reasoning_format", "none") : "?");
            str_append_cstr(&s, "\n");
            cJSON_Delete(props);
        }
    }

    const char *ts = cfg->thinking.mode == THINKING_ON ? "yes" :
                     cfg->thinking.mode == THINKING_EDRM ? "edrm" : "no";
    str_appendf(&s, "client: temp=%.1f max_tokens=%d json_mode=%s thinking=%s stream=%s\n",
                cfg->temperature, cfg->max_tokens,
                cfg->json_mode ? "on" : "off",
                ts,
                cfg->stream ? "on" : "off");
    str_appendf(&s, "data:   %s\n", nash_dir);
    if (session_dir)
        str_appendf(&s, "\n[session: %s]\n", session_dir);

    return str_steal(&s);
}


/* --- Threading for non-blocking inference --- */
typedef struct {
    react_ctx_t *react;
    char        *query;
    ui_state_t  *ui;
    char        *result;
    atomic_int done;
} infer_args_t;

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
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            free(cfg->api_base);
            cfg->api_base = strdup(argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            free(cfg->data_dir);
            cfg->data_dir = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_dir_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [-p QUERY] [--data-dir PATH] [--session DIR]\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("  --api URL       LLM server URL (default: %s)\n", cfg->api_base);
            printf("  -p QUERY        Run single query and exit (headless mode)\n");
            printf("  --data-dir PATH Data directory (default: ~/.nash/)\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("\nConfig: %s\n", config_path);
            config_free(cfg);
            return 0;
        }
    }

    /* Initialize data directory */
    char *nash_dir = get_nash_dir(cfg);

    /* Write default config if it doesn't exist */
    config_write_default(config_path);

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
        /* API providers: use config values */
        context_size = cfg->provider.context_size;
        server_model = cfg->provider.model_id ? strdup(cfg->provider.model_id) : NULL;
    }

    /* Update provider with fetched context size */
    if (provider && context_size > 0) {
        provider->cfg.context_size = context_size;
    }
    if (provider && server_model) {
        provider->cfg.model_id = server_model;
    }

    /* Build LLM config (kept for backward compat: EDRM probe, etc.) */
    llm_config_t llm_cfg = {
        .api_base     = cfg->api_base,
        .model        = server_model,
        .max_tokens   = cfg->max_tokens,
        .temperature  = cfg->temperature,
        .context_size = context_size,
    };

    /* Print banner */
    print_banner(cfg, props_json, nash_dir);

    /* Shared store + memory */
    store_t *shared_store = store_new(nash_dir);
    memory_t *memory = memory_new(nash_dir);

    /* Prune stale memories at startup (90 days, access_count < 2) */
    int pruned = memory_prune(memory,
                              cfg->prune_min_score, cfg->prune_min_evidence);
    if (pruned > 0)
        fprintf(stderr, "[info] pruned %d stale memories\n", pruned);

    /* One-shot headless mode */
    if (query) {
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
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd))) {
                char jpath[4112];
                snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", cwd);
                if (access(jpath, F_OK) == 0) {
                    session_dir = strdup(cwd);
                }
            }
        }
        if (!session_dir) {
            session_dir = create_session_dir(nash_dir);
        }
        journal_t *journal = journal_new(session_dir);
        int start_loop = journal_max_react_loop(journal) + 1;
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = session_dir, .scratchpad = NULL,
            .cfg = cfg,
            .react_loop = start_loop,
            .aliases = alias_map_new(),
        };
        /* Load scratchpad from previous session if it exists */
        {
            char sp_path[4096];
            snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md", session_dir);
            FILE *spf = fopen(sp_path, "r");
            if (spf) {
                fseek(spf, 0, SEEK_END);
                long spsz = ftell(spf);
                if (spsz > 0 && spsz < 32768) {
                    fseek(spf, 0, SEEK_SET);
                    tools.scratchpad = malloc((size_t)spsz + 1);
                    if (tools.scratchpad) {
                        size_t n = fread(tools.scratchpad, 1, (size_t)spsz, spf);
                        tools.scratchpad[n] = '\0';
                    }
                }
                fclose(spf);
            }
        }
        react_ctx_t react = {
            .provider = provider, .llm = &llm_cfg, .tools = &tools,
            .max_steps = cfg->max_react_steps, .verbose = 1,
        };
        char *result = react_run(&react, query, tui_on_event, (void *)session_dir);
        /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
        memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
        if (result) { printf("%s\n", result); free(result); }
        tools.react_loop++;  /* increment for next query */
        if (tools.scratchpad) free(tools.scratchpad);
        alias_map_free(tools.aliases);
        journal_free(journal);
        free(session_dir);
        store_free(shared_store);
        memory_free(memory);
        free(nash_dir);
        free(props_json);
        free(server_model);
        config_free(cfg);
        return result ? 0 : 1;
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
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd))) {
                char jpath[4112];
                snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", cwd);
                if (access(jpath, F_OK) == 0) {
                    session_dir = strdup(cwd);
                }
            }
        }
        if (!session_dir) {
            session_dir = create_session_dir(nash_dir);
        }

        journal_t *journal = journal_new(session_dir);
        int start_loop = journal_max_react_loop(journal) + 1;
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = session_dir, .scratchpad = NULL,
            .cfg = cfg,
            .react_loop = start_loop,
            .aliases = alias_map_new(),
        };
        /* Load scratchpad from previous session if it exists */
        {
            char sp_path[4096];
            snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md", session_dir);
            FILE *spf = fopen(sp_path, "r");
            if (spf) {
                fseek(spf, 0, SEEK_END);
                long spsz = ftell(spf);
                if (spsz > 0 && spsz < 32768) {
                    fseek(spf, 0, SEEK_SET);
                    tools.scratchpad = malloc((size_t)spsz + 1);
                    if (tools.scratchpad) {
                        size_t n = fread(tools.scratchpad, 1, (size_t)spsz, spf);
                        tools.scratchpad[n] = '\0';
                    }
                }
                fclose(spf);
            }
        }
        react_ctx_t react = {
            .provider = provider, .llm = &llm_cfg, .tools = &tools,
            .max_steps = cfg->max_react_steps, .verbose = 1,
        };

        /* Create UI state and initialize TUI */
        ui_state_t *ui = ui_state_new(session_dir, shared_store);
        ui_state_set_status(ui, STATUS_READY, "Ready");
        /* Set banner text for main pane */
        char *banner = build_banner_string(cfg, props_json, nash_dir, session_dir);
        ui_state_set_banner(ui, banner);
        free(banner);

        /* Load existing journal entries into UI state */
        ui_state_load_journal(ui, journal);

        tui_init();
        tui_render(ui);

        /* Wrapper event callback: updates ViewModel + redraws TUI */
        /* We use a struct to pass both ui and tui context */

        /* Main TUI event loop */
        int running = 1;
        int inferring = 0;
        pthread_t infer_tid;
        static infer_args_t iargs;
        while (running) {
            /* Check if inference thread completed */
            if (inferring && iargs.done) {
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
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "No result");
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

            if (submitted_query) {
                /* Handle exit/quit commands */
                if (strcmp(submitted_query, "quit") == 0 ||
                    strcmp(submitted_query, "exit") == 0 ||
                    strcmp(submitted_query, "/quit") == 0 ||
                    strcmp(submitted_query, "/exit") == 0) {
                    free(submitted_query);
                    running = 0;
                    break;
                }

                /* Handle /fork command */
                if (strncmp(submitted_query, "/fork ", 6) == 0) {
                    int fork_step = atoi(submitted_query + 6);
                    if (fork_step > 0) {
                        char *new_dir = create_session_dir(nash_dir);
                        /* Copy journal lines where step <= fork_step */
                        char src_j[4096], dst_j[4096];
                        snprintf(src_j, sizeof(src_j), "%s/journal.jsonl", session_dir);
                        snprintf(dst_j, sizeof(dst_j), "%s/journal.jsonl", new_dir);
                        FILE *sf = fopen(src_j, "r");
                        FILE *df = fopen(dst_j, "w");
                        if (sf && df) {
                            char jl[65536];
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
                        /* Copy symlinks from all react loops */
                        for (int loop = 0; loop <= tools.react_loop; loop++) {
                            for (int i = 0; i <= fork_step + 5; i++) {
                                char ref[32], sl[4096], tgt[4096], dl[4096];
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
                        if (tools.scratchpad)
                            cJSON_AddStringToObject(cp, "scratchpad", tools.scratchpad);
                        char *cpj = cJSON_Print(cp);
                        char cp_path[4096];
                        snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.json", new_dir);
                        FILE *cpf = fopen(cp_path, "w");
                        if (cpf) { fputs(cpj, cpf); fclose(cpf); }
                        free(cpj);
                        cJSON_Delete(cp);
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

                /* Handle /dream command — memory consolidation in a NEW session */
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

                    /* Create a dedicated dream session */
                    char *dream_dir = create_session_dir(nash_dir);
                    journal_t *dream_journal = journal_new(dream_dir);
                    tool_ctx_t dream_tools = {
                        .store = shared_store,
                        .journal = dream_journal,
                        .memory = memory,
                        .session_dir = dream_dir,
                        .scratchpad = NULL,
                        .cfg = cfg,
                        .react_loop = 0,
                        .aliases = alias_map_new(),
                    };
                    react_ctx_t dream_react = {
                        .provider = provider,
                        .llm = &llm_cfg,
                        .tools = &dream_tools,
                        .max_steps = cfg->max_react_steps,
                        .verbose = 1,
                    };

                    char dream_prompt[8192];
                    snprintf(dream_prompt, sizeof(dream_prompt),
                        "You are performing MEMORY CONSOLIDATION for a persistent knowledge store.\n\n"
                        "GOAL: Review, optimize, and consolidate the memory store at: %s\n"
                        "Each memory is a JSON file with fields: key, value, tags, pinned, "
                        "created_at, last_accessed, access_count, recall_hits, recall_misses, "
                        "journal_ref.\n\n"
                        "CAPABILITIES:\n"
                        "- List all memories: shell_exec \"ls %s/\"\n"
                        "- Read any memory: file_read on the JSON file path\n"
                        "- Read git history: shell_exec \"git -C %s log --oneline\" "
                        "to see how memories evolved\n"
                        "- Read original context: file_read on journal_ref path to understand "
                        "WHY a memory was created\n"
                        "- Create/update memories: memory_store (preserves validation scores "
                        "on update)\n"
                        "- Delete files: shell_exec \"rm %s/<filename>\"\n"
                        "- Validation score = (recall_hits+1)/(recall_hits+recall_misses+2) "
                        "-- Beta posterior mean\n\n"
                        "CONSOLIDATION CRITERIA:\n"
                        "- MERGE near-duplicates: combine entries covering the same concept "
                        "into one stronger entry\n"
                        "- RESOLVE contradictions: when two memories conflict, keep the one "
                        "with higher validation score\n"
                        "- GENERALIZE: promote task-specific observations into reusable "
                        "principles\n"
                        "- PRESERVE: never touch pinned memories, keep high-scoring entries "
                        "(score > 0.7) as-is\n"
                        "- When merging, preserve recall_hits/recall_misses from the "
                        "highest-scored source entry\n\n"
                        "CONSTRAINTS:\n"
                        "- Be conservative -- only change what is clearly redundant or "
                        "contradictory\n"
                        "- Read ALL memories before making any changes\n"
                        "- Check git history to understand memory evolution before modifying\n"
                        "- If journal_ref exists, read it to understand the original context\n\n"
                        "WHEN DONE:\n"
                        "- Compose a detailed summary of all changes (merges, deletions, "
                        "generalizations with specific keys)\n"
                        "- Run: shell_exec \"cd %s && git add -A && git commit -m "
                        "'<your full summary here>\n\nConsolidated-by: %s'\"\n"
                        "- Call done with the SAME summary text",
                        memory->dir, memory->dir, memory->dir,
                        memory->dir, memory->dir,
                        server_model ? server_model : "unknown-model");

                    /* Run dreaming in the new session (blocking — TUI shows progress) */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Dreaming...");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);

                    char *dream_result = react_run(&dream_react, dream_prompt,
                                                    threaded_event_cb,
                                                    &(infer_args_t){.ui = ui});

                    pthread_mutex_lock(&ui->mtx);
                    if (dream_result) {
                        ui_state_set_status(ui, STATUS_DONE, "Dream complete");
                    } else {
                        ui_state_set_status(ui, STATUS_ERROR, "Dream failed");
                    }
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);

                    /* Cleanup dream session */
                    free(dream_result);
                    if (dream_tools.scratchpad) free(dream_tools.scratchpad);
                    alias_map_free(dream_tools.aliases);
                    journal_free(dream_journal);
                    free(dream_dir);

                    /* Post-dream: prune with fresh validation scores */
                    memory_prune(memory,
                                 cfg->prune_min_score, cfg->prune_min_evidence);
                    continue;
                }

                /* Regular query — spawn inference in background thread */
                if (inferring) {
                    /* Previous inference still running — reject new query.
                     * react_ctx_t and tools are shared state that can't
                     * support concurrent react loops. */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Still running — wait for completion");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                ui_state_add_query(ui, submitted_query);
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                iargs = (infer_args_t){
                    .react = &react, .query = strdup(submitted_query),
                    .ui = ui, .result = NULL, .done = 0,
                };
                pthread_create(&infer_tid, NULL, infer_worker, &iargs);
                inferring = 1;
                tui_render(ui);
            }

            /* Always render if dirty */
            if (ui->dirty) tui_render(ui);

            /* Small sleep to avoid busy-waiting when no input */
            /* Auto-refresh MD every 1 second during inference */
            if (inferring) {
                static time_t last_refresh = 0;
                time_t now = time(NULL);
                if (now > last_refresh) {
                    last_refresh = now;
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_rebuild_md(ui);
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                }
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms */
        }

        tui_shutdown();
        ui_state_free(ui);

        if (tools.scratchpad) free(tools.scratchpad);
        alias_map_free(tools.aliases);
        journal_free(journal);
        free(session_dir);
    }
    printf("Bye.\n");
    store_free(shared_store);
    memory_free(memory);
    free(nash_dir);
    free(props_json);
    free(server_model);
    config_free(cfg);
    return 0;
}

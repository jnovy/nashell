#include <stdio.h>
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

    char sessions_base[512];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
    mkdir(sessions_base, 0755);

    char path[512];
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

    /* 1. Server props */
    if (!props_json) {
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

    if (!props_json) {
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
    volatile int done;
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

    /* Fetch model info from server */
    int context_size = llm_fetch_context_size(cfg->api_base);
    char *server_model = llm_fetch_model_name(cfg->api_base);
    char *props_json = llm_fetch_props_json(cfg->api_base);

    /* Build LLM config from TOML config */
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
    int pruned = memory_prune(memory, 90, 2);
    if (pruned > 0)
        fprintf(stderr, "[info] pruned %d stale memories\n", pruned);

    /* One-shot headless mode */
    if (query) {
        /* Detect existing session: --session arg, or CWD with journal.jsonl */
        char *session_dir = NULL;
        if (session_dir_arg) {
            char jpath[4096];
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
                char jpath[4096];
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
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = session_dir, .scratchpad = NULL,
            .cfg = cfg,
        };
        react_ctx_t react = {
            .llm = &llm_cfg, .tools = &tools,
            .max_steps = cfg->max_react_steps, .verbose = 1,
        };
        char *result = react_run(&react, query, tui_on_event, (void *)session_dir);
        if (result) { printf("%s\n", result); free(result); }
        tools.react_loop++;  /* increment for next query */
        if (tools.scratchpad) free(tools.scratchpad);
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
            char jpath[4096];
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
                char jpath[4096];
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
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = session_dir, .scratchpad = NULL,
            .cfg = cfg,
        };
        react_ctx_t react = {
            .llm = &llm_cfg, .tools = &tools,
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
                        /* Copy symlinks */
                        for (int i = 0; i <= fork_step + 5; i++) {
                            char ref[32], sl[4096], tgt[4096], dl[4096];
                            snprintf(ref, sizeof(ref), "R%dS%d",
                                     tools.react_loop, i);
                            snprintf(sl, sizeof(sl), "%s/%s", session_dir, ref);
                            ssize_t n = readlink(sl, tgt, sizeof(tgt) - 1);
                            if (n > 0) {
                                tgt[n] = '\0';
                                snprintf(dl, sizeof(dl), "%s/%s", new_dir, ref);
                                symlink(tgt, dl);
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
                        ui_state_set_status(ui, STATUS_READY,
                            "Forked — ready for new query");
                        tui_render(ui);
                    }
                    free(submitted_query);
                    continue;
                }

                /* Regular query — spawn inference in background thread */
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

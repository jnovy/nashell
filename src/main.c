#include <stdio.h>
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
    printf("client: temp=%.1f max_tokens=%d json_mode=%s thinking=%s stream=%s\n",
           cfg->temperature, cfg->max_tokens,
           cfg->json_mode ? "on" : "off",
           cfg->thinking ? "on" : "off",
           cfg->stream ? "on" : "off");
    printf("data:   %s\n", nash_dir);
    printf("\n");
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
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            free(cfg->api_base);
            cfg->api_base = strdup(argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            free(cfg->data_dir);
            cfg->data_dir = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [-p QUERY] [--data-dir PATH]\n");
            printf("  --api URL       LLM server URL (default: %s)\n", cfg->api_base);
            printf("  -p QUERY        Run single query and exit (headless mode)\n");
            printf("  --data-dir PATH Data directory (default: ~/.nash/)\n");
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

    /* One-shot headless mode */
    if (query) {
        char *session_dir = create_session_dir(nash_dir);
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

    /* Interactive REPL mode — ONE session for ALL queries */
    {
        char *session_dir = create_session_dir(nash_dir);
        printf("[session: %s]\n\n", session_dir);

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

        char *line;
        while ((line = readline("nash> ")) != NULL) {
            if (line[0] == '\0') { free(line); continue; }
            if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
                free(line); break;
            }
            add_history(line);

            char *result = react_run(&react, line, tui_on_event, (void *)session_dir);
            if (result) {
                printf("\n--- Result ---\n%s\n\n", result);
            } else {
                printf("\n[no result]\n\n");
            }

            /* Save last exchange for next react loop's context */
            if (react.last_query) free(react.last_query);
            if (react.last_result) free(react.last_result);
            react.last_query = strdup(line);
            react.last_result = result ? strdup(result) : NULL;
            free(result);
            free(line);
        }

        /* Cleanup */
        if (react.last_query) free(react.last_query);
        if (react.last_result) free(react.last_result);
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

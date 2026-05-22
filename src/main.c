#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "llm.h"
#include "tools.h"
#include "react.h"
#include "store.h"
#include "journal.h"
#include "frontend_tui.h"

#define DEFAULT_API_BASE "http://192.168.1.18:8080"

/* Create session directory: .sessions/<epoch.NNNNN>/ */
static char *create_session_dir(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    char path[512];
    snprintf(path, sizeof(path), ".sessions/%ld.%05ld",
             (long)tp.tv_sec, tp.tv_nsec / 10000);
    mkdir(".sessions", 0755);
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
static void print_banner(const llm_config_t *cfg, const char *props_json) {
    /* ASCII art header with gradient blue ANSI colors */
    printf("\n");
    printf("  \033[1m\033[38;2;80;255;120m _  _    __   ____  _  _  ____  __    __   \033[0m\n");
    printf("  \033[1m\033[38;2;60;220;100m( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \033[0m\n");
    printf("  \033[1m\033[38;2;40;190;80m ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \033[0m\n");
    printf("  \033[1m\033[38;2;30;160;60m(___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \033[0m\n");
    printf("\n");
    printf("  \033[38;2;70;200;90m--------- * New Agentic Shell * ---------\033[0m\n");
    printf("\n");

    /* 1. Server props (what the server reports — the baseline) */
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

    /* 2. Client overrides (what nash sends per-request, overriding server defaults) */
    printf("client: temp=%.1f max_tokens=%d json_mode=on thinking=off stream=on\n\n",
           cfg->temperature, cfg->max_tokens);
}

int main(int argc, char **argv) {
    const char *api_base = DEFAULT_API_BASE;
    const char *query    = NULL;  /* -p: one-shot headless mode */

    /* Simple arg parsing */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc)
            api_base = argv[++i];
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc)
            query = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [-p QUERY]\n");
            return 0;
        }
    }

    /* Fetch all server info (once) */
    char *props_json  = llm_fetch_props_json(api_base);
    char *server_model = llm_fetch_model_name(api_base);

    /* Extract context_size from props */
    int context_size = 0;
    if (props_json) {
        cJSON *p = cJSON_Parse(props_json);
        if (p) {
            cJSON *gs = cJSON_GetObjectItem(p, "default_generation_settings");
            if (gs) context_size = (int)jnum(gs, "n_ctx", 0);
            cJSON_Delete(p);
        }
    }

    /* Build the LLM config — this is the actual running configuration */
    llm_config_t llm_cfg = {
        .api_base     = api_base,
        .model        = server_model,
        .max_tokens   = 4096,
        .temperature  = 0.7,
        .context_size = context_size,
    };

    /* Print banner showing client config + server props */
    print_banner(&llm_cfg, props_json);

    /* Shared store at project root (dedup across all sessions) */
    store_t *shared_store = store_new(".");

    /* One-shot headless mode: run query and exit */
    if (query) {
        char *session_dir = create_session_dir();
        journal_t *journal = journal_new(session_dir);
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .session_dir = session_dir, .scratchpad = NULL
        };
        react_ctx_t react = {
            .llm = &llm_cfg, .tools = &tools,
            .max_steps = MAX_REACT_STEPS, .verbose = 1,
        };
        char *result = react_run(&react, query, tui_on_event, (void *)session_dir);
        if (result) { printf("%s\n", result); free(result); }
        if (tools.scratchpad) free(tools.scratchpad);
        journal_free(journal);
        /* NOTE: do NOT free shared_store here — it's shared across all sessions */
        free(session_dir);
        free(props_json);
        free(server_model);
        return result ? 0 : 1;
    }

    /* Interactive REPL mode */
    char *line;
    while ((line = readline("nash> ")) != NULL) {
        if (line[0] == '\0') { free(line); continue; }
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            free(line); break;
        }
        add_history(line);

        char *session_dir = create_session_dir();
        printf("[session: %s]\n", session_dir);

        journal_t *journal = journal_new(session_dir);
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .session_dir = session_dir, .scratchpad = NULL
        };
        react_ctx_t react = {
            .llm = &llm_cfg, .tools = &tools,
            .max_steps = MAX_REACT_STEPS, .verbose = 1,
        };

        char *result = react_run(&react, line, tui_on_event, (void *)session_dir);
        if (result) {
            printf("\n--- Result ---\n%s\n\n", result);
            free(result);
        } else {
            printf("\n[no result]\n\n");
        }

        if (tools.scratchpad) free(tools.scratchpad);
        journal_free(journal);
        /* NOTE: do NOT free shared_store here — it's shared across all queries */
        free(session_dir);
        free(line);
    }

    printf("Bye.\n");
    store_free(shared_store);
    free(props_json);
    free(server_model);
    return 0;
}

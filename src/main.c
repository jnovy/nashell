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

/* Print banner showing the ACTUAL running configuration */
static void print_banner(const llm_config_t *cfg) {
    printf("nash - agentic shell prototype\n");
    printf("model:  %s\n", cfg->model ? cfg->model : "(unknown)");
    printf("server: %s", cfg->api_base ? cfg->api_base : "(none)");
    if (cfg->context_size > 0)
        printf(" (%dk context)", cfg->context_size / 1024);
    printf("\n");
    printf("config: temp=%.1f max_tokens=%d\n", cfg->temperature, cfg->max_tokens);
    printf("\n");
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

    /* Fetch model info from server (once) */
    int context_size = llm_fetch_context_size(api_base);
    char *server_model = llm_fetch_model_name(api_base);

    /* Build the LLM config ONCE — this is the actual running configuration */
    llm_config_t llm_cfg = {
        .api_base     = api_base,
        .model        = server_model,
        .max_tokens   = 4096,
        .temperature  = 0.7,
        .context_size = context_size,
    };

    /* Print banner showing the actual config that will be used */
    print_banner(&llm_cfg);

    /* One-shot headless mode: run query and exit */
    if (query) {
        char *session_dir = create_session_dir();
        store_t   *store   = store_new(session_dir);
        journal_t *journal = journal_new(session_dir);
        tool_ctx_t tools = {
            .store = store, .journal = journal,
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
        store_free(store);
        free(session_dir);
        free(server_model);
        return result ? 0 : 1;
    }

    /* Main input loop */
    char *line;
    while ((line = readline("nash> ")) != NULL) {
        /* Skip empty lines */
        if (line[0] == '\0') {
            free(line);
            continue;
        }

        /* Quit command */
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            free(line);
            break;
        }

        add_history(line);

        /* Create a fresh session for each query */
        char *session_dir = create_session_dir();
        printf("[session: %s]\n", session_dir);

        store_t   *store   = store_new(session_dir);
        journal_t *journal = journal_new(session_dir);

        tool_ctx_t tools = {
            .store       = store,
            .journal     = journal,
            .session_dir = session_dir,
            .scratchpad  = NULL
        };

        react_ctx_t react = {
            .llm       = &llm_cfg,
            .tools     = &tools,
            .max_steps = MAX_REACT_STEPS,
            .verbose   = 1,
        };

        /* Run the react loop with TUI frontend */
        char *result = react_run(&react, line, tui_on_event, (void *)session_dir);

        if (result) {
            printf("\n--- Result ---\n%s\n\n", result);
            free(result);
        } else {
            printf("\n[no result]\n\n");
        }

        /* Cleanup */
        if (tools.scratchpad) free(tools.scratchpad);
        journal_free(journal);
        store_free(store);
        free(session_dir);
        free(line);
    }

    free(server_model);
    printf("Bye.\n");
    return 0;
}

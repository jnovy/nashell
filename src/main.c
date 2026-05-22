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

#define DEFAULT_API_BASE "http://192.168.1.18:8080"
#define DEFAULT_MODEL    "qwen3.6-35b-a3b"

/* Create session directory: .sessions/<timestamp>/ */
static char *create_session_dir(void) {
    char path[512];
    time_t now = time(NULL);
    snprintf(path, sizeof(path), ".sessions/%ld", (long)now);
    mkdir(".sessions", 0755);
    mkdir(path, 0755);
    return strdup(path);
}

static void print_banner(void) {
    printf("nash - agentic shell prototype\n");
    printf("LLM: %s (%s)\n", DEFAULT_API_BASE, DEFAULT_MODEL);
    printf("Type a task, or 'quit' to exit.\n\n");
}

int main(int argc, char **argv) {
    const char *api_base = DEFAULT_API_BASE;
    const char *model    = DEFAULT_MODEL;

    /* Simple arg parsing */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc)
            api_base = argv[++i];
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [--model NAME]\n");
            return 0;
        }
    }

    print_banner();

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

        /* Initialize components */
        llm_config_t llm_cfg = {
            .api_base = api_base,
            .model    = model,
            .max_tokens = 4096,
            .temperature = 0.7
        };

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

        /* Run the react loop */
        char *result = react_run(&react, line);

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

    printf("Bye.\n");
    return 0;
}

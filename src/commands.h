#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdatomic.h>
#include <pthread.h>
#include "tools.h"
#include "react.h"
#include "ui_state.h"
#include "journal.h"
#include "provider.h"
#include "config.h"
#include "store.h"
#include "memory.h"
#include "workspace.h"
#include "playbook.h"

/* Inference state — what the worker thread is doing.
 * Used as an atomic tri-state flag (0 = idle, nonzero = busy). */
enum infer_state {
    INFER_IDLE     = 0,   /* no worker thread running               */
    INFER_REACT    = 1,   /* regular inference (react loop)          */
    INFER_PLAYBOOK = 2,   /* playbook / agent run                    */
};

/* Context struct for TUI slash-command handlers.
 * Bundles all mutable/shared session state needed by commands. */
typedef struct {
    char         *session_dir;
    const char   *nash_dir;
    tool_ctx_t   *tools;
    react_ctx_t  *react;
    ui_state_t   *ui;
    journal_t    *journal;
    provider_t   *provider;
    provider_t   *consolidation_provider; /* [routing].consolidation — NULL = use provider */
    config_t     *cfg;
    store_t      *store;           /* shared_store */
    memory_t     *memory;
    workspace_t  *ws;
    const char   *server_model;
    atomic_int   *inferring;
    pthread_t    *infer_tid;
    playbook_args_t *pargs;        /* playbook args (pargs_tui) */
} command_ctx_t;

/* Return codes for command_dispatch */
#define CMD_CONTINUE  0   /* command handled, continue event loop */
#define CMD_NOT_FOUND 1   /* not a recognized slash command */

/* Dispatch a slash command from the TUI input buffer.
 * submitted_query: the full input string (e.g., "/fork 3").
 * Returns CMD_CONTINUE if handled, CMD_NOT_FOUND if not a command.
 * The function frees submitted_query if it handles the command.
 *
 * For /continue: modifies *out_query to the resolved query string
 * (caller must free). Returns CMD_NOT_FOUND so the caller proceeds
 * with inference dispatch using the modified query. */
int command_dispatch(command_ctx_t *ctx, char **submitted_query);

#endif /* COMMANDS_H */

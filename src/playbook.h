#ifndef PLAYBOOK_H
#define PLAYBOOK_H

#include "react.h"
#include "tools.h"
#include "config.h"
#include "ui_state.h"
#include <stdatomic.h>

/* ── Playbook types ──────────────────────────────────── */

typedef enum {
    PB_SESSION_PER_PASS,   /* fresh session per pass (like dream) */
    PB_SESSION_SHARED,     /* one session for all passes */
} pb_session_mode_t;

typedef enum {
    PB_SCRATCH_SHARED,     /* carry scratchpad across passes */
    PB_SCRATCH_ISOLATED,   /* fresh scratchpad per pass */
} pb_scratch_mode_t;

/* Per-pass react loop overrides.
 * -1 = "not set, inherit from playbook/global default"
 * 0 = explicitly disabled, 1 = explicitly enabled */
typedef struct {
    int  max_steps;           /* 0 = inherit */
    int  inject_memory;       /* -1 = inherit */
    int  inject_prev_result;  /* -1 = inherit */
    int  inject_repomap;      /* -1 = inherit */
    int  enable_reflection;   /* -1 = inherit */
    int  enable_pruning;      /* -1 = inherit */
    int  enable_compaction;   /* -1 = inherit */
    int  enable_scoring;      /* -1 = inherit */
    /* Tool filter */
    char **tools_allow;       /* NULL = inherit */
    int    n_tools_allow;
    char **tools_block;       /* NULL = inherit */
    int    n_tools_block;
} pb_react_overrides_t;

#define PB_REACT_INHERIT { 0, -1, -1, -1, -1, -1, -1, -1, NULL, 0, NULL, 0 }

/* Pass type: LLM react loop (default) or shell script (no LLM). */
typedef enum {
    PB_PASS_REACT,    /* run LLM react loop (default) */
    PB_PASS_SCRIPT,   /* run shell command, no LLM call */
} pb_pass_type_t;

/* Per-pass error policy. */
typedef enum {
    PB_ON_ERROR_ABORT,     /* stop playbook on failure (default) */
    PB_ON_ERROR_CONTINUE,  /* log warning, proceed to next pass */
    PB_ON_ERROR_RETRY,     /* retry once with failure context prefix */
} pb_error_policy_t;

typedef struct {
    char *label;
    char *prompt_template;    /* raw template with {{var}} placeholders */
    pb_pass_type_t type;      /* react (default) or script */
    pb_error_policy_t on_error; /* error policy (default: abort) */
    char *command;            /* shell command for PB_PASS_SCRIPT type */
    char *system_prompt;      /* custom system prompt text (NULL = use default) */
    int   system_prompt_replace; /* 0 = append to base (default), 1 = replace base entirely */
    pb_react_overrides_t react;  /* per-pass overrides */
} pb_pass_t;

typedef struct {
    char *name;
    char *description;
    char *filepath;

    pb_session_mode_t session_mode;
    pb_scratch_mode_t scratch_mode;
    int  pause_between;

    /* Template variables */
    char **var_keys;
    char **var_values;
    int    n_vars;

    /* Post hooks */
    int  post_prune;
    int  post_commit;

    /* React defaults for all passes */
    pb_react_overrides_t react_defaults;

    /* Top-level system prompt (inherited by passes that don't set their own) */
    char *system_prompt;
    int   system_prompt_replace; /* 0 = append to base (default), 1 = replace */

    /* Passes */
    pb_pass_t *passes;
    int        n_passes;
} playbook_t;

/* ── Playbook execution args (for worker thread) ─────── */

typedef struct {
    playbook_t      *playbook;
    char            *nash_dir;
    store_t         *store;
    memory_t        *memory;
    memory_t        *ws_memory;     /* workspace memory — NULL if no workspace */
    config_t        *cfg;
    char            *workspace_override; /* agent workspace -- overrides cfg->workspace for session routing */
    workspace_t     *agent_ws;          /* agent workspace -- two-layer memory for agent runs */
    provider_t      *provider;
    provider_t      *consolidation_provider; /* [routing].consolidation — NULL = use provider */
    char            *server_model;
    ui_state_t      *ui;
    /* State */
    int              current_pass;
    int              playbook_ok;
    atomic_int       done;
    /* Inter-pass pause */
    volatile int     waiting_for_user;
    char            *inter_pass_message;
    /* Agent tracking (for interactive /agent run history logging) */
    char            *agent_id;          /* NULL when not an agent run */
    time_t           agent_start_time;  /* wall-clock start (for duration calc) */
    /* Result text from last pass (caller must free; NULL on failure) */
    char            *result_text;
    /* Session dir of the last pass (caller must free; NULL on failure) */
    char            *last_session_dir;
} playbook_args_t;

/* ── API ─────────────────────────────────────────────── */

/* Load a playbook from a YAML file */
playbook_t *playbook_load(const char *path);

/* Free a playbook */
void playbook_free(playbook_t *pb);

/* Resolve react flags for a specific pass:
 * pass overrides → playbook defaults → global config defaults */
react_flags_t playbook_resolve_flags(const playbook_t *pb, int pass_idx,
                                      const config_t *cfg);

/* Resolve tool filter for a specific pass.
 * Inherits profile tool filter + description overrides from cfg as lowest priority. */
tool_filter_t playbook_resolve_tools(const playbook_t *pb, int pass_idx,
                                      const config_t *cfg);

/* Expand {{var}} placeholders in a prompt string */
char *playbook_expand(const playbook_t *pb, const char *tmpl,
                      int pass_idx, const char *prev_result,
                      const char *memory_dir, const char *model,
                      const char *session_dir, const char *nash_dir);

/* List available playbooks from ~/.nash/playbooks/ */
playbook_t **playbook_list(const char *nash_dir, int *count);

/* Write default dream.yaml */
int playbook_write_default_dream(const char *path);

/* Validate a loaded playbook: check template vars, tool names, pass config.
 * Returns 0 on success, -1 on error.  Writes human-readable diagnostics
 * to errbuf (up to errlen bytes). */
int playbook_validate(const playbook_t *pb, char *errbuf, size_t errlen);

/* Worker thread entry point */
void *playbook_worker(void *arg);

#endif /* PLAYBOOK_H */

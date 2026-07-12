#ifndef REACT_EVENT_H
#define REACT_EVENT_H

#include "cJSON.h"
#include "llm.h"

/* Event types emitted by the react engine */
typedef enum {
    REACT_EVENT_STEP_START,      /* about to call LLM for step N */
    REACT_EVENT_LLM_TOKEN,       /* streaming: one token received from LLM */
    REACT_EVENT_TOOL_START,      /* about to execute tool (action + description set) */
    REACT_EVENT_STEP_COMPLETE,   /* step N finished: tool executed, result available */
    REACT_EVENT_TOOL_OUTPUT,     /* tool result metadata + store ref available */
    REACT_EVENT_ERROR,           /* recoverable error (parse failure, missing action) */
    REACT_EVENT_WARNING,         /* cycling detected, context pressure */
    REACT_EVENT_DONE,            /* task complete, final result */
    REACT_EVENT_USER_ASK,        /* model wants to ask user a question — TUI should
                                  * show the question and collect user's answer.
                                  * question is in event->message, answer goes to
                                  * react_ctx_t->user_ask_answer */
    REACT_EVENT_PROMPT_PROGRESS, /* prompt processing progress from server */
} react_event_type_t;

/* Event data — all fields set to 0/NULL by default, only relevant ones populated */
typedef struct {
    react_event_type_t type;
    int    step;              /* current step number (1-based) */
    int    max_steps;         /* total max steps allowed */
    double step_elapsed;      /* seconds for this step's LLM call */
    double total_elapsed;     /* seconds since task start */

    /* Per-type data */
    const char  *action;      /* tool name: "shell_exec", "file_read", etc. */
    const char  *description; /* key param: command, path, pattern */
    const char  *token;       /* REACT_EVENT_LLM_TOKEN: single token text */
    const char  *message;     /* REACT_EVENT_ERROR/WARNING: error/warning text */
    const char  *result;      /* REACT_EVENT_DONE: final result text */
    const char  *store_ref;   /* store/ path for tool output */
    cJSON       *tool_meta;   /* tool result metadata JSON (borrowed, do not free) */
    llm_stats_t  stats;       /* LLM timing/token stats */
    int          context_size; /* server's n_ctx (for computing context utilization %) */

    /* Prompt processing progress (REACT_EVENT_PROMPT_PROGRESS) */
    int          prompt_progress_processed; /* tokens processed so far */
    int          prompt_progress_total;     /* total tokens to process */

    /* Provenance fields — set by playbook passes, NULL/0 for normal queries */
    const char  *session_dir; /* session directory where journal lives */
    int          react_loop;  /* react loop number within the session */
    int          pass_index;  /* playbook pass index (0-based), -1 for normal */
    int          pass_total;  /* total passes in playbook, 0 for normal */
    const char  *pass_label;  /* playbook pass label (e.g. "reflect"), NULL for normal */
} react_event_t;

/* Frontend callback: implement this to handle react engine events */
typedef void (*react_event_fn)(const react_event_t *event, void *userdata);

#endif

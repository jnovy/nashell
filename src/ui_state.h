#ifndef UI_STATE_H
#define UI_STATE_H

#include <pthread.h>
#include "react_event.h"
#include "journal.h"
#include "store.h"
#include "md_render.h"
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>

/* ── Focus & Status enums ──────────────────────────────── */

typedef enum { FOCUS_JOURNAL, FOCUS_QUERY } ui_focus_t;
typedef enum {
    STATUS_READY,
    STATUS_RUNNING,
    STATUS_AWAITING_INPUT,
    STATUS_DONE,
    STATUS_ERROR
} ui_status_t;

/* ── Navigation stack entry ──────────────────────────────── */

typedef struct {
    char      *filepath;     /* absolute path to .md file */
    int        scroll_y;     /* saved scroll position */
    int        scroll_x;     /* saved horizontal scroll */
    int        cursor_link;  /* saved cursor position */
    md_doc_t  *saved_doc;    /* non-NULL for virtual docs (search results) */
} nav_entry_t;

/* ── Playbook pass info (for session.md rendering) ─────── */

typedef struct {
    char *session_dir;
    char *pass_label;
    int   react_loop;
    int   pass_index;
} pb_pass_info_t;

/* ── Main UI state (the ViewModel) ─────────────────────── */

typedef struct {
    /* ── MD document for main pane ── */
    md_doc_t      *doc;              /* parsed MD document */
    int            scroll_y;         /* vertical scroll offset */
    int            scroll_x;         /* horizontal scroll offset */
    int            visible_rows;     /* main pane height (set by tui.c) */
    int            visible_cols;     /* main pane width  (set by tui.c) */
    int            cursor_link;      /* index into doc->links[] */

    /* ── Navigation stack (hierarchical MD browser) ── */
    nav_entry_t   *nav_stack;        /* stack of parent views */
    int            nav_depth;        /* current depth (0 = session.md) */
    int            nav_cap;          /* allocated capacity */
    char          *current_filepath; /* path of currently displayed file */

    /* ── Source data ── */
    char          *banner;           /* ASCII art + server info */
    char          *session_dir;
    store_t       *store;
    journal_t     *journal;          /* for reading journal.jsonl */

    /* ── Query pane ── */
    ui_focus_t     focus;
    ui_status_t    status;
    char          *status_text;

    char          *input_buffer;
    int            input_len;
    int            input_cap;
    int            cursor_pos;

    /* Query history (arrow up/down on first/last wrapped line) */
    char         **history;
    int            history_count;
    int            history_cap;
    int            history_idx;   /* current position in history navigation */

    /* ── Streaming state ── */
    char          *stream_tokens;
    int            stream_len;
    int            stream_cap;
    int            current_step;
    int            max_steps;
    int            current_react_loop;  /* which react loop is active */

    /* ── Model / context info (for status bar) ── */
    char          *model_name;
    int            context_size;
    int            context_used;
    int            bg_jobs;
    atomic_int    *pause_flag;

    /* ── Preview toggle (expanded store refs) ── */
    char         **expanded_uris;     /* URIs toggled to show preview */
    int            expanded_count;
    int            expanded_cap;

    /* ── user_ask state ── */
    char          *user_ask_question; /* full question text (while awaiting answer) */

    /* ── Scroll control ── */
    int            user_scrolled;    /* 1 = user scrolled away, suppress auto-scroll */
    int            needs_auto_scroll; /* 1 = deferred auto-scroll after next md_render */

    /* ── Cumulative token stats for active react loop ── */
    int            cum_prompt_tokens;       /* total prompt (input) tokens across all steps */
    int            cum_completion_tokens;    /* total completion (output) tokens across all steps */
    double         cum_predicted_per_second; /* last gen speed (t/s) — most recent step */
    double         cum_prompt_per_second;    /* last prompt processing speed (t/s) */
    double         react_total_elapsed;      /* total wall time for the react loop */
    int            cum_llm_steps;            /* number of LLM calls with stats */
    int            react_done;               /* 1 = react loop finished (show final stats) */

    /* ── Spinner phase for "processing..." indicator ── */
    int            spinner_phase;

    /* ── Live streaming progress (for "processing..." → actual metrics) ── */
    struct timespec stream_step_start;    /* CLOCK_MONOTONIC when step started */
    struct timespec stream_first_token;   /* CLOCK_MONOTONIC when first token arrived */
    int            stream_first_token_seen; /* 1 = first token received, timing valid */
    int            stream_token_count;    /* tokens received in current step */

    /* ── Playbook session tracking ── */
    char          *playbook_session_dir; /* session dir of active playbook pass (NULL when no playbook) */
    int            playbook_react_loop;  /* react loop of active playbook pass */

    /* Accumulated info about all playbook passes (for session.md rendering) */
    pb_pass_info_t *pb_passes;
    int              pb_pass_count;
    int              pb_pass_cap;

    /* ── Cross-session scratchpad search ── */
    int            search_active;     /* 1 = search results shown in main pane */
    char          *nash_dir;          /* ~/.nash (for finding sessions) */

    /* ── Deferred regeneration flags ── */
    /* Set by the inference thread's event handler (under mtx) to request
     * expensive file I/O without holding the mutex during the actual I/O.
     * The main loop checks these flags and performs the regeneration. */
    int            needs_react_regen;   /* 1 = regenerate reactRX.md */
    int            needs_session_regen; /* 1 = regenerate session.md */
    int            needs_file_reload;   /* 1 = reload current_filepath */

    /* ── Dirty flag + mutex ── */
    int            dirty;
    pthread_mutex_t mtx;
} ui_state_t;

/* ── Lifecycle ───────────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store);
void        ui_state_free(ui_state_t *ui);

/* ── Navigation ──────────────────────────────────────────── */

void ui_state_tab(ui_state_t *ui);
void ui_state_up(ui_state_t *ui);
void ui_state_down(ui_state_t *ui);
void ui_state_enter(ui_state_t *ui);     /* follow .md link or toggle step */
void ui_state_back(ui_state_t *ui);      /* pop nav stack (Esc) */
void ui_state_page_up(ui_state_t *ui);
void ui_state_page_down(ui_state_t *ui);

/* ── Input editing ───────────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch);
void ui_state_input_backspace(ui_state_t *ui);
void ui_state_input_delete(ui_state_t *ui);
void ui_state_input_left(ui_state_t *ui);
void ui_state_input_right(ui_state_t *ui);
void ui_state_input_home(ui_state_t *ui);
void ui_state_input_end(ui_state_t *ui);

/* ── React event handler ─────────────────────────────────── */

void ui_state_on_event(const react_event_t *ev, void *userdata);

/* ── File-based MD operations ────────────────────────────── */

/* Reload the current file from disk and re-parse.
 * Called by timer refresh and after navigation. */
void ui_state_reload_file(ui_state_t *ui);

/* Generate/update session.md from journal + banner.
 * Called after: new query, step complete, done. */
void ui_state_generate_session_md(ui_state_t *ui);

/* Generate/update reactRX.md for a specific react loop.
 * Called by on_event as steps progress. */
void ui_state_generate_react_md(ui_state_t *ui, int react_loop);

/* ── Status & data updates ───────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text);
void ui_state_set_banner(ui_state_t *ui, const char *banner);
void ui_state_add_query(ui_state_t *ui, const char *query_text);
void ui_state_load_journal(ui_state_t *ui, journal_t *journal);

/* Toggle collapse/expand preview for the currently selected link */
void ui_state_toggle_preview(ui_state_t *ui);

/* ── Cross-session scratchpad search ─────────────────────── */

/* Search all session scratchpads for `query` (case-insensitive).
 * Builds a markdown document with hyperlinks to matching scratchpads
 * and displays it in the main pane.  Must be called with ui->mtx held.
 * Pass NULL or "" to clear search results. */
void ui_state_search(ui_state_t *ui, const char *query);

/* ── Breadcrumb path for status bar ──────────────────────── */

/* Build breadcrumb string like "session.md > reactR2.md".
 * Returns malloc'd string, caller frees. */
char *ui_state_breadcrumb(ui_state_t *ui);

#endif

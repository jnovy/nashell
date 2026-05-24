#ifndef UI_STATE_H
#define UI_STATE_H

#include <pthread.h>
#include "react_event.h"
#include "journal.h"
#include "store.h"
#include <stddef.h>

/* ── Focus & View enums ──────────────────────────────── */

typedef enum { FOCUS_JOURNAL, FOCUS_QUERY } ui_focus_t;
typedef enum {
    STATUS_READY,
    STATUS_RUNNING,
    STATUS_AWAITING_INPUT,
    STATUS_DONE,
    STATUS_ERROR
} ui_status_t;

/* ── Journal pane data ───────────────────────────────── */

/* One step in a query's react loop */
typedef struct {
    char   *ref;              /* "R0S3" */
    char   *tool;             /* "shell_exec" */
    char   *description;      /* "ls -la /tmp" (key param) */
    int     failed;
    int     size;             /* output size in chars */
    double  elapsed;          /* step duration */
    int     step;             /* step number */
} ui_step_t;

/* One query in the tree view */
typedef struct {
    char       *query_text;       /* user's query */
    double      timestamp;        /* epoch when submitted */
    int         react_loop;       /* R0, R1, etc. */
    int         step_count;       /* total steps for this query */
    int         step_cap;
    int         failed_count;     /* failed steps */
    char       *result_preview;   /* first 200 chars of done result (or NULL) */
    int         expanded;         /* 1 = show react steps underneath in tree view */
    ui_step_t  *steps;            /* per-query steps (loaded when expanded) */
} ui_query_t;

/* ── Main UI state (the ViewModel) ───────────────────── */

typedef struct {
    /* ── Journal pane (tree view) ── */
    ui_query_t    *queries;
    int            query_count;
    int            query_cap;
    int            selected_query;   /* cursor in query list */
    int            selected_step;    /* cursor within expanded query's steps (-1 = on query itself) */

    /* ── Bottom pane content (actual file content from store) ── */
    char          *bottom_content;   /* actual content loaded from store */
    int            bottom_lines;     /* total lines */
    int            bottom_scroll;    /* scroll offset */

    /* ── Query pane ── */
    ui_focus_t     focus;            /* which pane has focus */
    ui_status_t    status;
    char          *status_text;      /* "Ready" / "Running step 3/50..." */

    char          *input_buffer;     /* current query being typed */
    int            input_len;
    int            input_cap;
    int            cursor_pos;       /* cursor position in input */

    /* ── Streaming state (live during react loop) ── */
    char          *stream_tokens;    /* accumulated tokens from current LLM call */
    int            stream_len;
    int            stream_cap;
    int            current_step;
    int            max_steps;

    /* ── Session info ── */
    char          *session_dir;
    store_t       *store;            /* for reading tool outputs */

    /* ── Banner (shown in main pane on startup) ── */
    char          *banner;           /* ASCII art + server info (no ANSI escapes) */

    /* ── Preview content (shown in bottom pane during browsing) ── */
    char          *preview;          /* preview text for bottom pane */
    int            preview_scroll;   /* scroll offset in preview */

    /* ── Dirty flag (renderer checks this) ── */
    int            dirty;            /* 1 = needs redraw */
    pthread_mutex_t mtx;             /* protects concurrent access */
} ui_state_t;

/* ── Lifecycle ───────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store);
void        ui_state_free(ui_state_t *ui);

/* ── Navigation (frontend-agnostic) ──────────────────── */

void ui_state_tab(ui_state_t *ui);           /* switch focus */
void ui_state_up(ui_state_t *ui);            /* move cursor up */
void ui_state_down(ui_state_t *ui);          /* move cursor down */
void ui_state_enter(ui_state_t *ui);         /* toggle expand / drill down */
void ui_state_back(ui_state_t *ui);          /* collapse / go back */
void ui_state_page_up(ui_state_t *ui);       /* scroll bottom pane up */
void ui_state_page_down(ui_state_t *ui);     /* scroll bottom pane down */

/* ── Input editing ─────────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch);
void ui_state_input_backspace(ui_state_t *ui);
void ui_state_input_delete(ui_state_t *ui);
void ui_state_input_left(ui_state_t *ui);
void ui_state_input_right(ui_state_t *ui);
void ui_state_input_home(ui_state_t *ui);
void ui_state_input_end(ui_state_t *ui);

/* ── React event handler (updates ViewModel from engine events) ── */

void ui_state_on_event(const react_event_t *ev, void *userdata);

/* ── Journal loading (populate queries from journal.jsonl) ── */

void ui_state_load_journal(ui_state_t *ui, journal_t *journal);
void ui_state_load_manifest(ui_state_t *ui, int query_idx);
void ui_state_load_detail(ui_state_t *ui, int step_idx);
void ui_state_load_bottom_content(ui_state_t *ui);

/* ── Status updates ──────────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text);
void ui_state_add_query(ui_state_t *ui, const char *query_text);
void ui_state_set_banner(ui_state_t *ui, const char *banner);

#endif

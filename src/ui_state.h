#ifndef UI_STATE_H
#define UI_STATE_H

#include "react_event.h"
#include "journal.h"
#include "store.h"
#include <stddef.h>

/* ── Focus & View enums ──────────────────────────────── */

typedef enum { FOCUS_JOURNAL, FOCUS_QUERY } ui_focus_t;
typedef enum { VIEW_QUERIES, VIEW_MANIFEST, VIEW_DETAIL } journal_view_t;
typedef enum {
    STATUS_READY,
    STATUS_RUNNING,
    STATUS_AWAITING_INPUT,
    STATUS_DONE,
    STATUS_ERROR
} ui_status_t;

/* ── Journal pane data ───────────────────────────────── */

/* One query in the query list (VIEW_QUERIES) */
typedef struct {
    char   *query_text;       /* user's query */
    double  timestamp;        /* epoch when submitted */
    int     react_loop;       /* R0, R1, etc. */
    int     step_count;       /* total steps for this query */
    int     failed_count;     /* failed steps */
    char   *result_preview;   /* first 100 chars of done result (or NULL) */
} ui_query_t;

/* One step in the manifest (VIEW_MANIFEST) */
typedef struct {
    char   *ref;              /* "R0S3" */
    char   *tool;             /* "shell_exec" */
    char   *description;      /* "ls -la /tmp" (key param) */
    int     failed;
    int     size;             /* output size in chars */
    double  elapsed;          /* step duration */
    int     step;             /* step number */
} ui_step_t;

/* ── Main UI state (the ViewModel) ───────────────────── */

typedef struct {
    /* ── Journal pane ── */
    ui_query_t    *queries;
    int            query_count;
    int            query_cap;
    int            selected_query;   /* cursor in query list */
    journal_view_t view;             /* current drill-down level */

    /* Expanded manifest (VIEW_MANIFEST) */
    ui_step_t     *steps;
    int            step_count;
    int            step_cap;
    int            selected_step;    /* cursor in step list */

    /* Step detail (VIEW_DETAIL) */
    char          *detail_content;   /* full tool output from store */
    int            detail_lines;     /* total lines in detail */
    int            detail_scroll;    /* scroll offset */

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

    /* ── Dirty flag (renderer checks this) ── */
    int            dirty;            /* 1 = needs redraw */
} ui_state_t;

/* ── Lifecycle ───────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store);
void        ui_state_free(ui_state_t *ui);

/* ── Navigation (frontend-agnostic) ──────────────────── */

void ui_state_tab(ui_state_t *ui);           /* switch focus */
void ui_state_up(ui_state_t *ui);            /* move cursor up */
void ui_state_down(ui_state_t *ui);          /* move cursor down */
void ui_state_enter(ui_state_t *ui);         /* drill down / submit */
void ui_state_back(ui_state_t *ui);          /* go back one level (Escape) */
void ui_state_page_up(ui_state_t *ui);       /* scroll page up (detail view) */
void ui_state_page_down(ui_state_t *ui);     /* scroll page down (detail view) */

/* ── Input editing ───────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch);
void ui_state_input_backspace(ui_state_t *ui);
void ui_state_input_delete(ui_state_t *ui);
void ui_state_input_left(ui_state_t *ui);
void ui_state_input_right(ui_state_t *ui);
void ui_state_input_home(ui_state_t *ui);
void ui_state_input_end(ui_state_t *ui);
const char *ui_state_input_submit(ui_state_t *ui);  /* returns query text, clears input */

/* ── React event handler (updates ViewModel from engine events) ── */

void ui_state_on_event(const react_event_t *ev, void *userdata);

/* ── Journal loading (populate queries from journal.jsonl) ── */

void ui_state_load_journal(ui_state_t *ui, journal_t *journal);
void ui_state_load_manifest(ui_state_t *ui, int query_idx);
void ui_state_load_detail(ui_state_t *ui, int step_idx);

/* ── Status updates ──────────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text);
void ui_state_add_query(ui_state_t *ui, const char *query_text);

/* ── Serialization (for web frontend) ────────────────── */

/* Serialize the visible portion of ui_state to JSON.
 * Caller must free returned string. */
char *ui_state_to_json(const ui_state_t *ui);

#endif

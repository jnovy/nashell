#ifndef UI_STATE_H
#define UI_STATE_H

#include <pthread.h>
#include "react_event.h"
#include "journal.h"
#include "store.h"
#include "md_render.h"
#include <stddef.h>

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
    char *filepath;     /* absolute path to .md file */
    int   scroll_y;     /* saved scroll position */
    int   scroll_x;     /* saved horizontal scroll */
    int   cursor_link;  /* saved cursor position */
} nav_entry_t;

/* ── Main UI state (the ViewModel) ─────────────────────── */

typedef struct {
    /* ── MD document for main pane ── */
    md_doc_t      *doc;              /* parsed MD document */
    int            scroll_y;         /* vertical scroll offset */
    int            scroll_x;         /* horizontal scroll offset */
    int            visible_rows;     /* main pane height (set by tui.c) */
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
    volatile int  *pause_flag;

    /* ── Preview toggle (expanded store refs) ── */
    char         **expanded_uris;     /* URIs toggled to show preview */
    int            expanded_count;
    int            expanded_cap;

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

/* ── Breadcrumb path for status bar ──────────────────────── */

/* Build breadcrumb string like "session.md > reactR2.md".
 * Returns malloc'd string, caller frees. */
char *ui_state_breadcrumb(ui_state_t *ui);

#endif

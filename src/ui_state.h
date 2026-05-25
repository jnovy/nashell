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

/* ── Per-query link state ──────────────────────────────── */

typedef enum {
    LINK_COLLAPSED,     /* just show the query line */
    LINK_SHOW_RESULT,   /* show query + result preview */
    LINK_SHOW_STEPS,
    LINK_SHOW_CONTENT     /* show query + all react steps */
} link_state_t;

/* ── Main UI state (the ViewModel) ───────────────────── */

typedef struct {
    /* ── MD document for main pane ── */
    md_doc_t      *doc;              /* parsed MD document */
    int            scroll_y;         /* vertical scroll offset */
    int            visible_rows;     /* main pane height (set by tui.c) */
    int            cursor_link;      /* index into doc->links[] */

    /* ── Per-link state (indexed by link position) ── */
    link_state_t  *link_states;      /* array: one state per link */
    int            link_states_count;
    int            link_states_cap;

    /* Step content expansion — tracked by URI, not link index */
    char         **expanded_uris;     /* URIs of steps in SHOW_CONTENT state */
    int            expanded_count;
    int            expanded_cap;

    /* ── Source data for MD generation ── */
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

    /* ── Streaming state ── */
    char          *stream_tokens;
    int            stream_len;
    int            stream_cap;
    int            current_step;
    int            max_steps;

    /* ── Dirty flag + mutex ── */
    int            dirty;
    pthread_mutex_t mtx;
} ui_state_t;

/* ── Lifecycle ───────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store);
void        ui_state_free(ui_state_t *ui);

/* ── Navigation ──────────────────────────────────────── */

void ui_state_tab(ui_state_t *ui);
void ui_state_up(ui_state_t *ui);
void ui_state_down(ui_state_t *ui);
void ui_state_enter(ui_state_t *ui);     /* toggle link state */
void ui_state_back(ui_state_t *ui);      /* collapse current link */
void ui_state_page_up(ui_state_t *ui);
void ui_state_page_down(ui_state_t *ui);

/* ── Input editing ───────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch);
void ui_state_input_backspace(ui_state_t *ui);
void ui_state_input_delete(ui_state_t *ui);
void ui_state_input_left(ui_state_t *ui);
void ui_state_input_right(ui_state_t *ui);
void ui_state_input_home(ui_state_t *ui);
void ui_state_input_end(ui_state_t *ui);

/* ── React event handler ─────────────────────────────── */

void ui_state_on_event(const react_event_t *ev, void *userdata);

/* ── MD document regeneration ────────────────────────── */

/* Regenerate the MD document from journal + link states.
 * Called after: link state change, new query, journal reload. */
void ui_state_rebuild_md(ui_state_t *ui);

/* ── Status & data updates ───────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text);
void ui_state_set_banner(ui_state_t *ui, const char *banner);
void ui_state_add_query(ui_state_t *ui, const char *query_text);
void ui_state_load_journal(ui_state_t *ui, journal_t *journal);

#endif

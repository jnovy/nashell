#ifndef MD_RENDER_H
#define MD_RENDER_H

#include <ncurses.h>

/* ── Hyperlink in a parsed MD document ── */
typedef struct {
    char *uri;          /* relative ("reactR0.md") or absolute path */
    char *text;         /* display text */
    int   doc_line;     /* line in source where this link starts */
    int   render_line;  /* line in rendered output (set by md_render, accounts for skipped ``` lines) */
} md_link_t;

/* ── Parsed MD document ── */
typedef struct {
    char       *source;      /* raw MD source (owned) */
    md_link_t  *links;       /* tracked hyperlinks */
    int         link_count;
    int         link_cap;
    /* Rendered lines cache (computed by md_render) */
    int         total_lines; /* total rendered lines (set after md_render) */
    int         max_table_width; /* widest table in display columns (set by md_render) */
} md_doc_t;

/* Parse MD source into a document, extracting [text](uri) links.
 * Caller must free with md_doc_free(). */
md_doc_t *md_parse(const char *source);

/* Free a parsed document */
void md_doc_free(md_doc_t *doc);

/* Render document to an ncurses window.
 * scroll_y: vertical scroll offset (in rendered lines)
 * cursor_link: index into doc->links[] for the selected hyperlink (-1 = none)
 * focus: 1 = this pane has focus (cursor visible), 0 = no focus
 * Returns: total number of rendered lines */
int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int scroll_x,
              int cursor_link, int focus);

/* Get the rendered line number of a link (for auto-scrolling to keep cursor visible) */
int md_link_line(md_doc_t *doc, int link_idx);

/* ── Deferred OSC 8 hyperlinks ──
 * ncurses' waddch cannot pass ESC (0x1B) to the terminal — it renders
 * as ^[ caret notation.  Instead, we collect link positions during
 * md_render and emit the OSC 8 sequences directly to stdout after
 * ncurses' doupdate() has flushed the screen buffer. */

typedef struct {
    int  row;        /* screen row (0-based, relative to window) */
    int  col_start;  /* first column of link text */
    int  col_end;    /* one past last column of link text */
    char uri[4096];  /* resolved URI (file:// prefixed if needed) */
} md_osc8_link_t;

/* Max deferred links per render cycle */
#define MD_OSC8_MAX 64

/* Deferred link list — populated by md_render, flushed by md_osc8_flush */
extern md_osc8_link_t md_osc8_links[];
extern int            md_osc8_count;

/* Emit all deferred OSC 8 sequences directly to stdout.
 * Must be called AFTER ncurses doupdate() so the screen content
 * is already rendered and cursor positioning sequences work.
 * win: the ncurses window containing the rendered text (used to read
 *      link text back via mvwinnstr for re-output between OSC 8 tags).
 * win_row_offset: the window's absolute row on screen (from getbegy). */
void md_osc8_flush(WINDOW *win, int win_row_offset);

/* Find the rendered line number of a heading matching a #fragment anchor.
 * fragment: the anchor string WITHOUT the leading '#' (e.g., "1-current-state").
 * The document must have been rendered at least once (md_render called) so that
 * render_line counts are accurate.  Returns -1 if no matching heading found. */
int md_find_anchor(md_doc_t *doc, const char *fragment);

#endif

#ifndef MD_RENDER_H
#define MD_RENDER_H

#include <ncurses.h>

/* ── Hyperlink in a parsed MD document ── */
typedef struct {
    char *uri;          /* "file://session/R0" */
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
int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int cursor_link, int focus);

/* Get the rendered line number of a link (for auto-scrolling to keep cursor visible) */
int md_link_line(md_doc_t *doc, int link_idx);

#endif

#ifndef MD_RENDER_H
#define MD_RENDER_H

#include <ncurses.h>
#include "md_doc.h"

/* Render document to an ncurses window.
 * scroll_y: vertical scroll offset (in rendered lines)
 * scroll_x: horizontal scroll offset (used for tables and code blocks)
 * cursor_link: index into doc->links[] for the selected hyperlink (-1 = none)
 * focus: 1 = this pane has focus (cursor visible), 0 = no focus
 * Returns: total number of rendered lines */
int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int scroll_x,
              int cursor_link, int focus);

/* -- Deferred OSC 8 hyperlinks --
 * ncurses' waddch cannot pass ESC (0x1B) to the terminal — it renders
 * as ^[ caret notation.  Instead, we collect link positions during
 * md_render and emit the OSC 8 sequences directly to stdout after
 * ncurses' doupdate() has flushed the screen buffer. */

typedef struct {
  int row;        /* screen row (0-based, relative to window) */
  int col_start;  /* first column of link text */
  int col_end;    /* one past last column of link text */
  char uri[4096]; /* resolved URI (file:// prefixed if needed) */
} md_osc8_link_t;

/* Max deferred links per render cycle */
#define MD_OSC8_MAX 64

/* Deferred link list — populated by md_render, flushed by md_osc8_flush */
extern md_osc8_link_t md_osc8_links[];
extern int md_osc8_count;

/* Emit all deferred OSC 8 sequences directly to stdout.
 * Must be called AFTER ncurses doupdate() so the screen content
 * is already rendered and cursor positioning sequences work.
 * win: the ncurses window containing the rendered text (used to read
 *      link text back via mvwinnstr for re-output between OSC 8 tags).
 * win_row_offset: the window's absolute row on screen (from getbegy). */
void md_osc8_flush(WINDOW *win, int win_row_offset);

#endif

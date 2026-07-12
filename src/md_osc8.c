/* md_osc8.c -- OSC 8 terminal hyperlink support. Extracted from md_render.c. */

#include "md_render_internal.h"

/* ── OSC 8 terminal hyperlinks (deferred) ── */

/* Deferred OSC 8 link list — populated during md_render(), flushed
 * after doupdate() by md_osc8_flush().  This avoids the fundamental
 * problem that ncurses' waddch() renders ESC (0x1B) as ^[ caret
 * notation instead of passing it through to the terminal. */
md_osc8_link_t md_osc8_links[MD_OSC8_MAX];
int            md_osc8_count = 0;

/* Record a deferred OSC 8 link to be emitted after doupdate(). */
void defer_osc8_link(int vis_line, int col, const char *uri) {
    if (vis_line < 0) return;  /* off-screen row from partially-scrolled block */
    if (md_osc8_count >= MD_OSC8_MAX) return;
    md_osc8_link_t *lk = &md_osc8_links[md_osc8_count++];
    lk->row = vis_line;
    lk->col_start = col;
    lk->col_end = col;  /* filled in by defer_osc8_end */
    if (uri[0] == '/' && strncmp(uri, "file://", 7) != 0)
        snprintf(lk->uri, sizeof(lk->uri), "file://%s", uri);
    else
        snprintf(lk->uri, sizeof(lk->uri), "%s", uri);
}

/* Mark the end column of the most recent deferred link. */
void defer_osc8_end(int col_end) {
    if (md_osc8_count > 0)
        md_osc8_links[md_osc8_count - 1].col_end = col_end;
}

/* Convert ncurses color component (0-1000 range from init_color) to 0-255. */
static int nc_to_rgb(int nc_val) {
    return nc_val * 255 / 1000;
}

/* Emit SGR escape sequence reproducing the given ncurses attributes. */
static void emit_sgr_for_attrs(attr_t attrs, short pair) {
    printf("\033[0");  /* reset first */
    if (attrs & A_BOLD)      printf(";1");
    if (attrs & A_DIM)       printf(";2");
    if (attrs & A_UNDERLINE) printf(";4");
    if (attrs & A_REVERSE)   printf(";7");
    if (pair > 0) {
        short fg, bg;
        if (pair_content(pair, &fg, &bg) == OK) {
            if (fg >= 0) {
                short r, g, b;
                if (color_content(fg, &r, &g, &b) == OK)
                    printf(";38;2;%d;%d;%d",
                           nc_to_rgb(r), nc_to_rgb(g), nc_to_rgb(b));
            }
            if (bg >= 0) {
                short r, g, b;
                if (color_content(bg, &r, &g, &b) == OK)
                    printf(";48;2;%d;%d;%d",
                           nc_to_rgb(r), nc_to_rgb(g), nc_to_rgb(b));
            }
        }
    }
    printf("m");
}

/* Emit all deferred OSC 8 sequences directly to stdout.
 * Called AFTER doupdate() so screen content is already rendered.
 * win_row_offset: absolute screen row of the window (getbegy). */
void md_osc8_flush(WINDOW *win, int win_row_offset) {
    if (md_osc8_count == 0) return;
    /* Save cursor position (DECSC) — restore after emitting OSC 8
     * sequences so ncurses' internal cursor tracking stays in sync
     * with the real terminal cursor on the next doupdate(). */
    printf("\0337");  /* DECSC: save cursor position */
    for (int i = 0; i < md_osc8_count; i++) {
        md_osc8_link_t *lk = &md_osc8_links[i];
        int abs_row = win_row_offset + lk->row + 1;  /* 1-based */
        int link_len = lk->col_end - lk->col_start;
        if (link_len <= 0 || link_len > 512) continue;

        /* Position cursor at link start */
        printf("\033[%d;%dH", abs_row, lk->col_start + 1);
        /* OSC 8 start: ESC ] 8 ; ; URI ST */
        printf("\033]8;;%s\033\\", lk->uri);

        /* Re-output link text cell-by-cell, preserving ncurses
         * attributes.  mvwinch() returns both character and attrs
         * (bold, reverse, color pair, etc.) so cursor highlights
         * and link colors survive the OSC 8 re-output. */
        attr_t prev_attrs = 0;
        short  prev_pair  = -1;
        for (int c = 0; c < link_len; c++) {
            chtype ch = mvwinch(win, lk->row, lk->col_start + c);
            attr_t attrs = ch & A_ATTRIBUTES;
            short  pair  = (short)PAIR_NUMBER(ch);
            char   chr   = ch & A_CHARTEXT;
            if (chr == '\0') chr = ' ';

            /* Emit SGR only when attributes change */
            if (c == 0 || attrs != prev_attrs || pair != prev_pair) {
                emit_sgr_for_attrs(attrs, pair);
                prev_attrs = attrs;
                prev_pair  = pair;
            }
            putchar(chr);
        }

        /* SGR reset + OSC 8 end */
        printf("\033[0m");
        printf("\033]8;;\033\\");
    }
    /* Restore cursor position (DECRC) so terminal cursor returns to
     * where ncurses left it after doupdate(). */
    printf("\0338");  /* DECRC: restore cursor position */
    fflush(stdout);
    md_osc8_count = 0;
}

/* Check if a URI is a web URL (http:// or https://) */
int is_web_uri(const char *uri) {
    return strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0;
}

/* Check if a URI is linkable via OSC 8: web URLs, file:// URIs, or
 * absolute paths (which get auto-prefixed with file:// at emit time) */
int is_linkable_uri(const char *uri) {
    if (!uri || !*uri) return 0;
    return is_web_uri(uri)
        || strncmp(uri, "file://", 7) == 0
        || uri[0] == '/';
}

/* Legacy wrappers — now just record deferred links instead of
 * writing ESC bytes via waddch (which doesn't work). */
void emit_osc8_link_start(WINDOW *win, const char *uri) {
    int y, x;
    getyx(win, y, x);
    defer_osc8_link(y, x, uri);
}

void emit_osc8_end(WINDOW *win) {
    int y, x;
    getyx(win, y, x);
    defer_osc8_end(x);
    (void)y;
}

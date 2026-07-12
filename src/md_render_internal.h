/*
 * md_render_internal.h -- Shared definitions for md_render, md_diff, md_osc8.
 * Not part of the public API.
 */
#ifndef MD_RENDER_INTERNAL_H
#define MD_RENDER_INTERNAL_H

#include "md_render.h"
#include "nash_limits.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Color pairs (must match tui.c init_pair calls) ── */
#define C_NORMAL    0
#define C_SELECTED  1
#define C_FAILED    2
#define C_SUCCESS   3
#define C_STATUS    4
#define C_DIM       5
#define C_FOCUS     6
#define C_STREAM    7
/* Diff color pairs (must match tui.c) */
#define CP_DIFF_ADD 15
#define CP_DIFF_DEL 16
#define CP_DIFF_ADD_HL 19  /* char-level highlight: brighter bg */
#define CP_DIFF_DEL_HL 20

/* ── Buffer size for line copying ── */
#define LINE_BUF_SIZE NASH_PATH_MAX

/* ── Segment-based inline formatting ── */
#define MAX_INLINE_SEGS 64

typedef struct {
    const char *text;
    int         len;      /* byte length */
    int         attr;     /* ncurses attribute (0 = default) */
    const char *url;      /* URL pointer for [text](url) links (NULL if not a link) */
    int         url_len;  /* URL byte length (0 if not a link) */
} inline_seg_t;

/* ── md_diff.c ── */
int  is_diff_line(const char *line, int line_len);
int  render_diff_line(WINDOW *win, int row, int col,
                      int diff_type, const char *text, int text_len,
                      int cols, int hl_start, int hl_end);
void compute_char_diff(const char *a, int a_len,
                       const char *b, int b_len,
                       int *hl_start_a, int *hl_end_a,
                       int *hl_start_b, int *hl_end_b);
const char *diff_content_after_marker(const char *text, int text_len,
                                      int *out_len);

/* ── md_osc8.c ── */
void defer_osc8_link(int vis_line, int col, const char *uri);
void defer_osc8_end(int col_end);
int  is_linkable_uri(const char *uri);
int  is_web_uri(const char *uri);
void emit_osc8_link_start(WINDOW *win, const char *uri);
void emit_osc8_end(WINDOW *win);

/* ── Shared helpers (md_render.c) ── */
int  utf8_display_len(const char *s, int max_bytes);

#endif /* MD_RENDER_INTERNAL_H */

#include "md_render.h"
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

/* ── Buffer size for line copying ── */
#define LINE_BUF_SIZE 4096

/* ── Segment-based inline formatting ── */

/* Maximum segments per line (generous limit for nested formatting) */
#define MAX_INLINE_SEGS 64

/* An inline formatting segment: text pointer, length, and ncurses attr */
typedef struct {
    const char *text;
    int         len;      /* byte length */
    int         attr;     /* ncurses attribute (0 = default) */
} inline_seg_t;

/* Forward declarations */
static int utf8_display_len(const char *s, int max_bytes);
static int render_segment(WINDOW *win, int row, int col, const char *text,
                          int len, int max_cols);
static int parse_inline(const char *text, int text_len, inline_seg_t *segs, int max_segs);

/* ── Helpers (Items 3-9) ── */

/* Count display columns for a segment (delegates to utf8_display_len).
 * Item 3: eliminated duplicate UTF-8 column counting — seg_display_cols
 * and utf8_display_len were 100% identical. */
static int seg_display_cols(const char *text, int len) {
    return utf8_display_len(text, len);
}

/* Get the byte offset within a segment that corresponds to a given column offset.
 * Returns byte position where the segment should be split. */
static int seg_col_to_byte(const char *text, int seg_len, int col_offset) {
    int col = 0;
    for (int i = 0; i < seg_len && text[i]; i++) {
        if ((text[i] & 0xC0) != 0x80) {
            if (col == col_offset) return i;
            col++;
        }
    }
    return seg_len;
}

/* Apply a new attribute to all segments: OR into existing, set on zero.
 * Item 5: extracted from heading and react-step branches. */
static void apply_attr_to_segs(inline_seg_t *segs, int n, int new_attr) {
    for (int i = 0; i < n; i++) {
        if (segs[i].attr) {
            segs[i].attr |= new_attr;
        } else {
            segs[i].attr = new_attr;
        }
    }
}

/* Copy a source line to a fixed-size buffer (NUL-terminated).
 * Item 6: extracted from main loop and table row loop.
 * Item 8: uses LINE_BUF_SIZE constant. */
static void copy_to_buf(char *buf, size_t buf_size, const char *src, int len) {
    int copy_len = len < (int)buf_size - 1 ? len : (int)buf_size - 1;
    memcpy(buf, src, copy_len);
    buf[copy_len] = '\0';
}

/* Find the last space in [start, limit) that is at least min_pos.
 * Returns the byte index of the space, or -1 if none found.
 * Item 7: shared word-boundary helper for code block wrapping and render_segs_wrapped. */
static int find_word_boundary(const char *text, int limit, int min_pos) {
    for (int k = limit - 1; k > min_pos; k--) {
        if (text[k] == ' ') return k;
    }
    return -1;
}

/* Count the number of wrapped lines a text block will consume.
 * Item 4: extracted from blockquote and regular text off-screen counting. */
static int count_wrapped_lines(const char *text, int text_len, int usable_width) {
    if (text_len <= 0) return 1;
    inline_seg_t segs[MAX_INLINE_SEGS];
    int n = parse_inline(text, text_len, segs, MAX_INLINE_SEGS);
    int total_cols = 0;
    for (int i = 0; i < n; i++)
        total_cols += seg_display_cols(segs[i].text, segs[i].len);
    if (total_cols > usable_width) {
        return (total_cols + usable_width - 1) / usable_width;
    }
    return 1;
}

/* Advance render_line by (lines_consumed - 1).
 * Item 9: makes the scattered render_line++ pattern explicit.
 * Each branch computes lines_consumed and calls this; the universal
 * render_line++ at the bottom of the main loop adds the final +1. */
static void advance_render_line(int *render_line, int lines_consumed) {
    *render_line += lines_consumed - 1;
}

/* ── Inline formatting engine ── */

/* Try to parse a formatted marker (**bold, *italic, `code`).
 * Item 1: eliminates structural duplication in parse_inline().
 *
 * Parameters:
 *   p, end: current parse position and end of text
 *   marker, mlen: opening/closing marker string and its length
 *   attr: attribute to apply if marker is found
 *   segs, n: output segment array and count (passed by pointer for mutation)
 *   max_segs: capacity of segs array
 *
 * Returns: 1 if marker was consumed (success or failure), 0 if not a marker.
 * On success with closing marker: emits one formatted segment, advances p past closing.
 * On success without closing marker: emits literal text (merged or new), advances p to end.
 * On failure: p is unchanged, no segment emitted. */
static int try_parse_marker(const char **pp, const char *end,
                            const char *marker, int mlen, int attr,
                            inline_seg_t *segs, int *n, int max_segs) {
    const char *p = *pp;
    /* Check for opening marker */
    if (p + mlen > end) return 0;
    int is_marker = 1;
    for (int i = 0; i < mlen; i++) {
        if (p[i] != marker[i]) { is_marker = 0; break; }
    }
    if (!is_marker) return 0;

    /* Consume opening marker */
    p += mlen;
    const char *start = p;

    /* Scan for closing marker */
    while (p + mlen <= end) {
        int found = 1;
        for (int i = 0; i < mlen; i++) {
            if (p[i] != marker[i]) { found = 0; break; }
        }
        if (found) break;
        p++;
    }

    if (p + mlen <= end) {
        /* Found closing marker — emit formatted segment */
        segs[*n].text = start;
        segs[*n].len = (int)(p - start);
        segs[*n].attr = attr;
        (*n)++;
        p += mlen;
    } else {
        /* No closing marker — render as literal */
        if (*n > 0 && segs[*n - 1].attr == 0) {
            segs[*n - 1].len += mlen + (int)(p - start);
        } else {
            if (*n < max_segs) {
                segs[*n].text = start - mlen;
                segs[*n].len = mlen + (int)(p - start);
                segs[*n].attr = 0;
                (*n)++;
            }
        }
    }
    *pp = p;
    return 1;
}

/* Parse inline formatting into segments.
 * Handles **bold**, *italic*, `code` markers.
 * Returns number of segments written (0 on error).
 * Segments are coalesced: consecutive segments with same attr are merged. */
static int parse_inline(const char *text, int text_len, inline_seg_t *segs, int max_segs) {
    int n = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && n < max_segs) {
        /* Item 1: use try_parse_marker for all three formatting types */
        if (try_parse_marker(&p, end, "**", 2, A_BOLD, segs, &n, max_segs))
            continue;
        if (try_parse_marker(&p, end, "*", 1, A_UNDERLINE, segs, &n, max_segs))
            continue;
        if (try_parse_marker(&p, end, "`", 1, COLOR_PAIR(C_STREAM), segs, &n, max_segs))
            continue;

        /* Regular text: collect until next formatting marker */
        const char *start = p;
        while (p < end) {
            if (*p == '`') break;
            if (*p == '*' && p + 1 < end && p[1] == '*') break;
            if (*p == '*') break;
            p++;
        }
        int seg_len = (int)(p - start);
        if (seg_len > 0) {
            segs[n].text = start;
            segs[n].len = seg_len;
            segs[n].attr = 0;
            n++;
        }
    }
    return n;
}

/* Render all segments on a single display line, starting at (row, col).
 * max_width: maximum display columns to use.
 * Returns number of display columns consumed. */
static int render_segs_on_line(WINDOW *win, int row, int col,
                               inline_seg_t *segs, int n_segs, int max_width) {
    int x = col;
    for (int i = 0; i < n_segs && x < col + max_width; i++) {
        if (segs[i].len <= 0) continue;
        int remaining = col + max_width - x;
        if (remaining <= 0) break;
        if (segs[i].attr)
            wattron(win, segs[i].attr);
        x += render_segment(win, row, x, segs[i].text, segs[i].len, remaining);
        if (segs[i].attr)
            wattroff(win, segs[i].attr);
    }
    return x - col;
}

/* Split segments at a byte offset within a specific segment index.
 * Returns 1 if split was done (segments after idx shifted right), 0 otherwise. */
static int split_segment(inline_seg_t *segs, int *n_segs, int max_segs,
                         int seg_idx, int split_byte) {
    if (seg_idx < 0 || seg_idx >= *n_segs) return 0;
    inline_seg_t *seg = &segs[seg_idx];
    if (split_byte <= 0 || split_byte >= seg->len) return 0;

    /* Shift segments right to make room for the new segment */
    if (*n_segs + 1 > max_segs) return 0;
    memmove(segs + seg_idx + 2, segs + seg_idx + 1,
            (*n_segs - seg_idx - 1) * sizeof(inline_seg_t));

    /* Split: original becomes first part, new segment is second part */
    segs[seg_idx + 1].text = seg->text + split_byte;
    segs[seg_idx + 1].len = seg->len - split_byte;
    segs[seg_idx + 1].attr = seg->attr;
    seg->len = split_byte;
    (*n_segs)++;
    return 1;
}

/* Render a set of segments with word-wrapping across display lines.
 * Segments are split at word boundaries respecting formatting boundaries.
 * Returns number of display lines consumed. */
static int render_segs_wrapped(WINDOW *win, int start_row, int col,
                               inline_seg_t *segs, int n_segs, int usable_width) {
    (void)col;  /* col is used indirectly via render_segs_on_line */
    if (usable_width < 5) usable_width = 5;

    int current_row = start_row;
    int lines_used = 1;

    while (n_segs > 0) {
        /* Find how many full segments fit on this line */
        int x = 0;
        int fit_count = 0;
        int partial_seg = -1;
        int partial_byte = 0;

        for (int i = 0; i < n_segs; i++) {
            int dc = seg_display_cols(segs[i].text, segs[i].len);
            if (x + dc <= usable_width) {
                x += dc;
                fit_count++;
            } else {
                /* Check if we can fit a partial segment */
                int remaining = usable_width - x;
                if (remaining > 0 && segs[i].len > 0) {
                    /* Find last space within the segment that fits */
                    int split_at = seg_col_to_byte(segs[i].text, segs[i].len, remaining);
                    /* Item 7: use shared word-boundary helper */
                    int min_split = (usable_width / 4 < split_at) ? usable_width / 4 : 1;
                    int last_space = find_word_boundary(segs[i].text, split_at, min_split);
                    if (last_space >= min_split) {
                        partial_seg = i;
                        partial_byte = last_space + 1;
                        /* Adjust x to match */
                        x = 0;
                        for (int j = 0; j < i; j++)
                            x += seg_display_cols(segs[j].text, segs[j].len);
                        x += seg_display_cols(segs[i].text, partial_byte);
                        fit_count = i;
                    } else {
                        /* Hard split at byte boundary */
                        partial_seg = i;
                        partial_byte = split_at;
                        fit_count = i;
                    }
                }
                break;
            }
        }

        /* Render the fitting segments on current row */
        if (fit_count > 0 || partial_seg >= 0) {
            /* If we have a partial segment, split it.
             * Use a flag to track whether the split happened — the condition
             * `partial_byte < segs[partial_seg].len` is invalidated by split_segment()
             * because it sets segs[partial_seg].len = partial_byte. */
            int did_split = 0;
            if (partial_seg >= 0 && partial_byte > 0 && partial_byte < segs[partial_seg].len) {
                split_segment(segs, &n_segs, 256, partial_seg, partial_byte);
                did_split = 1;
            }
            /* After split, render up to and including the first part of the split.
             * Without split, render only the segments that fully fit. */
            int render_count;
            if (did_split) {
                render_count = partial_seg + 1;
            } else {
                render_count = fit_count;
            }
            if (render_count > n_segs) render_count = n_segs;
            if (render_count < 0) render_count = 0;
            render_segs_on_line(win, current_row, col, segs, render_count, usable_width);
        } else {
            /* Even a single character doesn't fit — render one char to avoid infinite loop */
            if (n_segs > 0 && segs[0].len > 0) {
                /* Find byte for 1 column */
                int b = 0;
                while (b < segs[0].len && segs[0].text[b] && (segs[0].text[b] & 0xC0) == 0x80) b++;
                if (b < segs[0].len) {
                    split_segment(segs, &n_segs, 256, 0, b + 1);
                }
                render_segs_on_line(win, current_row, col, segs, 1, usable_width);
            }
        }

        /* Advance: remove rendered segments */
        int advance;
        if (partial_seg >= 0 && partial_byte > 0) {
            /* If we identified a partial segment, advance past the first part
             * (which was either split or already consumed). */
            advance = partial_seg + 1;
        } else {
            advance = (fit_count >= n_segs) ? n_segs : fit_count;
        }
        if (advance > n_segs) advance = n_segs;
        memmove(segs, segs + advance, (n_segs - advance) * sizeof(inline_seg_t));
        n_segs -= advance;

        if (n_segs > 0) {
            current_row++;
            lines_used++;
        }
    }

    return lines_used;
}

/* Render inline formatting with word-wrapping.
 * text: the text to render (may contain **bold**, *italic*, `code` markers)
 * text_len: byte length of text (NUL not included)
 * Returns number of display lines consumed. */
static int render_inline_wrapped(WINDOW *win, int start_row, int col,
                                 const char *text, int text_len, int usable_width) {
    if (text_len <= 0) return 1;
    inline_seg_t segs[MAX_INLINE_SEGS];
    int n = parse_inline(text, text_len, segs, MAX_INLINE_SEGS);
    if (n <= 0) return 1;

    /* Check if segments fit on one line */
    int total_cols = 0;
    for (int i = 0; i < n; i++)
        total_cols += seg_display_cols(segs[i].text, segs[i].len);

    if (total_cols <= usable_width) {
        render_segs_on_line(win, start_row, col, segs, n, usable_width);
        return 1;
    }

    return render_segs_wrapped(win, start_row, col, segs, n, usable_width);
}

/* ── Parse ── */

md_doc_t *md_parse(const char *source) {
    md_doc_t *doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;
    doc->source = strdup(source ? source : "");

    /* Extract [text](uri) links */
    const char *p = doc->source;
    int line_num = 0;
    while (*p) {
        if (*p == '\n') { line_num++; p++; continue; }

        /* Look for [text](uri) pattern */
        if (*p == '[') {
            const char *text_start = p + 1;
            const char *text_end = strchr(text_start, ']');
            if (text_end && text_end[1] == '(') {
                const char *uri_start = text_end + 2;
                const char *uri_end = strchr(uri_start, ')');
                if (uri_end) {
                    /* Found a link */
                    if (doc->link_count >= doc->link_cap) {
                        doc->link_cap = doc->link_cap ? doc->link_cap * 2 : 16;
                        doc->links = realloc(doc->links,
                                             doc->link_cap * sizeof(md_link_t));
                    }
                    md_link_t *lk = &doc->links[doc->link_count++];
                    lk->text = strndup(text_start, (size_t)(text_end - text_start));
                    lk->uri = strndup(uri_start, (size_t)(uri_end - uri_start));
                    lk->doc_line = line_num;
                    lk->render_line = -1;  /* set during md_render() */
                    p = uri_end + 1;
                    continue;
                }
            }
        }
        p++;
    }

    return doc;
}

void md_doc_free(md_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->link_count; i++) {
        free(doc->links[i].text);
        free(doc->links[i].uri);
    }
    free(doc->links);
    free(doc->source);
    free(doc);
}

/* ── Render helpers ── */

/* Count the number of display columns a UTF-8 string occupies.
 * For ASCII, 1 byte = 1 column. For multi-byte UTF-8, we count
 * only the leading bytes (skip continuation bytes 10xxxxxx). */
static int utf8_display_len(const char *s, int max_bytes) {
    int cols = 0;
    for (int i = 0; i < max_bytes && s[i]; i++) {
        /* Skip UTF-8 continuation bytes (10xxxxxx) */
        if ((s[i] & 0xC0) != 0x80)
            cols++;
    }
    return cols;
}

/* Render a text segment to ncurses using mvwaddnstr (handles UTF-8).
 * Returns the number of display columns consumed. */
static int render_segment(WINDOW *win, int row, int col, const char *text,
                          int len, int max_cols) {
    if (len <= 0 || col >= getmaxx(win)) return 0;
    /* Truncate to max_cols display columns */
    int display_cols = utf8_display_len(text, len);
    if (display_cols > max_cols) {
        /* Find byte position that fits in max_cols display columns */
        int cols = 0;
        int byte_pos = 0;
        while (byte_pos < len && cols < max_cols) {
            if ((text[byte_pos] & 0xC0) != 0x80)
                cols++;
            byte_pos++;
        }
        len = byte_pos;
        display_cols = cols;
    }
    mvwaddnstr(win, row, col, text, len);
    return display_cols;
}

/* ── Table rendering (Item 2) ── */

/* Render a block of consecutive table rows with consistent column widths.
 * Item 2: extracted from md_render() to eliminate ~120 lines of inline code.
 *
 * Parameters:
 *   win: ncurses window
 *   src: pointer to first '|' line of the table
 *   num_rows: number of table rows (pre-scanned)
 *   scroll_y: vertical scroll offset
 *   scroll_x: horizontal scroll offset (only used for tables)
 *   rows: window height
 *   cols: window width
 *   render_line: in/out — starts at current render line, ends after last table row
 *   src_line: in/out — source line tracking
 *   out_src: output — updated to point past last table row (for main loop)
 *
 * Returns: number of render lines consumed by the table. */
static int render_table(WINDOW *win, const char *src, int num_rows,
                        int scroll_y, int scroll_x, int rows, int cols,
                        int *render_line, int *src_line, const char **out_src) {
    #define MAX_TABLE_COLS 20

    /* Pass 1: scan ALL consecutive | lines to find max column widths */
    int col_widths[MAX_TABLE_COLS] = {0};
    int num_cols = 0;
    {
        const char *scan = src;
        while (scan && *scan == '|') {
            const char *sp = scan + 1;
            int ci = 0;
            while (*sp && *sp != '\n') {
                if (*sp == '|') { ci++; sp++; continue; }
                const char *cs = sp;
                while (*sp && *sp != '|' && *sp != '\n') sp++;
                const char *ts = cs, *te = sp;
                while (ts < te && *ts == ' ') ts++;
                while (te > ts && *(te-1) == ' ') te--;
                int w = (int)(te - ts);
                int is_dash = 1;
                for (const char *dp = ts; dp < te; dp++)
                    if (*dp != '-' && *dp != ':') { is_dash = 0; break; }
                if (!is_dash && ci < MAX_TABLE_COLS) {
                    if (w > col_widths[ci]) col_widths[ci] = w;
                    if (ci + 1 > num_cols) num_cols = ci + 1;
                }
                if (*sp == '|') { ci++; sp++; }
            }
            const char *nl = strchr(scan, '\n');
            scan = nl ? nl + 1 : NULL;
            if (!scan || *scan != '|') break;
        }
    }

    /* Pass 2: render ALL table rows with consistent col_widths.
     * Tables use horizontal scroll (scroll_x) instead of wrapping. */
    int tbl_sx = scroll_x;
    const char *trow = src;
    for (int tr = 0; tr < num_rows && trow; tr++) {
        const char *trow_eol = strchr(trow, '\n');
        int trow_len = trow_eol ? (int)(trow_eol - trow) : (int)strlen(trow);

        /* Item 6+8: use copy_to_buf with LINE_BUF_SIZE */
        char tbuf[LINE_BUF_SIZE];
        copy_to_buf(tbuf, sizeof(tbuf), trow, trow_len);

        int vis_line = *render_line - scroll_y;
        int visible = (vis_line >= 0 && vis_line < rows);

        if (visible) {
            /* Check if separator */
            int is_sep = 1;
            for (const char *sp = tbuf + 1; *sp; sp++)
                if (*sp != '-' && *sp != '|' && *sp != ' ' && *sp != ':')
                    { is_sep = 0; break; }

            if (is_sep) {
                int x = -tbl_sx;
                wattron(win, COLOR_PAIR(C_DIM));
                for (int ci = 0; ci < num_cols; ci++) {
                    if (x >= 0 && x < cols) mvwaddch(win, vis_line, x, ACS_PLUS);
                    x++;
                    int w = col_widths[ci] + 2;
                    for (int k = 0; k < w; k++) {
                        if (x >= 0 && x < cols) mvwaddch(win, vis_line, x, ACS_HLINE);
                        x++;
                    }
                }
                if (x >= 0 && x < cols) mvwaddch(win, vis_line, x, ACS_PLUS);
                wattroff(win, COLOR_PAIR(C_DIM));
            } else {
                /* Check if header (next row is separator) */
                int is_header = 0;
                const char *nxt = trow_eol ? trow_eol + 1 : NULL;
                if (nxt && *nxt == '|') {
                    int ns = 1;
                    for (const char *np = nxt+1; *np && *np != '\n'; np++)
                        if (*np != '-' && *np != '|' && *np != ' ' && *np != ':')
                            { ns = 0; break; }
                    if (ns) is_header = 1;
                }

                int x = -tbl_sx;
                const char *cp = tbuf + 1;
                int ci = 0;
                while (*cp && *cp != '\n') {
                    if (*cp == '|') { cp++; ci++; continue; }
                    if (x >= 0 && x < cols) {
                        wattron(win, COLOR_PAIR(C_DIM));
                        mvwaddch(win, vis_line, x, ACS_VLINE);
                        wattroff(win, COLOR_PAIR(C_DIM));
                    }
                    x++;
                    const char *cs = cp;
                    while (*cp && *cp != '|' && *cp != '\n') cp++;
                    const char *ts = cs, *te = cp;
                    while (ts < te && *ts == ' ') ts++;
                    while (te > ts && *(te-1) == ' ') te--;
                    int tlen = (int)(te - ts);
                    int pw = (ci < num_cols) ? col_widths[ci] : tlen;
                    if (x >= 0 && x < cols) mvwaddch(win, vis_line, x, ' ');
                    x++;
                    if (is_header) wattron(win, A_BOLD);
                    if (tlen > 0) {
                        for (int ti = 0; ti < tlen; ti++) {
                            if (x >= 0 && x < cols)
                                mvwaddch(win, vis_line, x, (chtype)(unsigned char)ts[ti]);
                            x++;
                        }
                    }
                    if (is_header) wattroff(win, A_BOLD);
                    int tx = x + (pw - tlen) + 1;
                    while (x < tx) {
                        if (x >= 0 && x < cols)
                            mvwaddch(win, vis_line, x, ' ');
                        x++;
                    }
                    if (*cp == '|') { ci++; cp++; }
                }
                if (x >= 0 && x < cols) {
                    wattron(win, COLOR_PAIR(C_DIM));
                    mvwaddch(win, vis_line, x, ACS_VLINE);
                    wattroff(win, COLOR_PAIR(C_DIM));
                }
            }
        }

        /* Advance to next table row */
        if (tr < num_rows - 1) {
            (*render_line)++;
            (*src_line)++;
            trow = trow_eol ? trow_eol + 1 : NULL;
        } else {
            /* Last row — let the main loop advance normally */
            *out_src = trow;
        }
    }

    return num_rows;
}

/* ── Main render function ── */
/*
 * Render document to an ncurses window.
 * scroll_y: vertical scroll offset (in rendered lines)
 * scroll_x: horizontal scroll offset (only used for table rendering;
 *           all other line types ignore horizontal scroll)
 * cursor_link: index into doc->links[] for the selected hyperlink (-1 = none)
 * focus: 1 = this pane has focus (cursor visible), 0 = no focus
 * Returns: total number of rendered lines
 */
int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int scroll_x,
              int cursor_link, int focus) {
    if (!win || !doc || !doc->source) return 0;

    int rows = getmaxy(win);
    int cols = getmaxx(win);

    werase(win);

    const char *src = doc->source;
    int render_line = 0;  /* line number in the full document */
    int src_line = 0;     /* source line number (for link matching) */
    int link_idx = 0;     /* current link index */
    int in_code_block = 0;

    while (*src) {
        /* Extract one line */
        const char *eol = strchr(src, '\n');
        int line_len = eol ? (int)(eol - src) : (int)strlen(src);

        /* Item 6+8: use copy_to_buf with LINE_BUF_SIZE */
        char line_buf[LINE_BUF_SIZE];
        copy_to_buf(line_buf, sizeof(line_buf), src, line_len);

        /* Code block fence toggle — handle BEFORE visibility check
         * so in_code_block state is always correct, and skip render_line++
         * so fence lines don't produce blank lines in the output. */
        if (strncmp(line_buf, "```", 3) == 0) {
            in_code_block = !in_code_block;
            /* Don't render ``` markers and don't increment render_line —
             * they should be invisible (no blank line). */
            src_line++;
            src = eol ? eol + 1 : src + strlen(src);
            continue;
        }

        /* Compute visibility — but process ALL lines for accurate render_line counting.
         * Only skip the actual ncurses drawing calls for off-screen lines. */
        int vis_line = render_line - scroll_y;
        int visible = (vis_line >= 0 && vis_line < rows);

        /* Track links regardless of visibility */
        int is_link_line = (line_buf[0] == '[' && link_idx < doc->link_count &&
                            src_line == doc->links[link_idx].doc_line);

        if (in_code_block) {
            /* Code block content: wrap at cols-2, render in cyan */
            int usable = cols - 2;
            if (usable < 10) usable = 10;
            int remaining = (int)strlen(line_buf);
            const char *wp = line_buf;
            int first = 1;
            int lines_consumed = 0;
            while (remaining > 0) {
                int chunk = remaining > usable ? usable : remaining;
                /* Item 7: use shared word-boundary helper */
                if (chunk < remaining) {
                    int min_pos = usable / 4;
                    int last_space = find_word_boundary(wp, chunk, min_pos);
                    if (last_space > 0) chunk = last_space + 1;
                }
                int vl = render_line - scroll_y;
                if (vl >= 0 && vl < rows) {
                    wattron(win, COLOR_PAIR(C_STREAM));
                    mvwaddnstr(win, vl, first ? 2 : 4, wp, chunk);
                    wattroff(win, COLOR_PAIR(C_STREAM));
                }
                wp += chunk;
                remaining -= chunk;
                lines_consumed++;
            }
            /* Item 9: explicit line advancement */
            if (lines_consumed > 1)
                advance_render_line(&render_line, lines_consumed);

        } else if (is_link_line) {
            /* Hyperlink line */
            int is_cursor = (focus && link_idx == cursor_link);
            md_link_t *lk = &doc->links[link_idx];
            lk->render_line = render_line;
            link_idx++;

            if (visible) {
                if (is_cursor) {
                    wattron(win, A_REVERSE | A_BOLD);
                } else {
                    wattron(win, COLOR_PAIR(C_FOCUS));
                }

                /* Render the link text with inline formatting */
                inline_seg_t segs[MAX_INLINE_SEGS];
                int n = parse_inline(lk->text, (int)strlen(lk->text), segs, MAX_INLINE_SEGS);
                render_segs_on_line(win, vis_line, 0, segs, n, cols);

                /* Pad for reverse video */
                if (is_cursor) {
                    int cur_x = getcurx(win);
                    for (int x = cur_x; x < cols; x++)
                        mvwaddch(win, vis_line, x, ' ');
                    wattroff(win, A_REVERSE | A_BOLD);
                } else {
                    wattroff(win, COLOR_PAIR(C_FOCUS));
                }
            }

        } else if (line_buf[0] == '#') {
            /* Heading — with inline formatting */
            if (visible) {
                int level = 0;
                while (line_buf[level] == '#') level++;
                const char *htext = line_buf + level;
                while (*htext == ' ') htext++;
                int hlen = (int)strlen(htext);
                int pair = (level == 1) ? C_SUCCESS : C_FOCUS;

                /* Parse inline formatting in heading */
                inline_seg_t segs[MAX_INLINE_SEGS];
                int n = parse_inline(htext, hlen, segs, MAX_INLINE_SEGS);

                /* Item 5: use apply_attr_to_segs */
                apply_attr_to_segs(segs, n, COLOR_PAIR(pair) | A_BOLD);
                render_segs_on_line(win, vis_line, 0, segs, n, cols);
            }

        } else if (strncmp(line_buf, "---", 3) == 0) {
            /* Horizontal rule */
            if (visible) {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwhline(win, vis_line, 0, ACS_HLINE, cols);
                wattroff(win, COLOR_PAIR(C_DIM));
            }

        } else if (line_buf[0] == '>' && line_buf[1] == ' ') {
            /* Blockquote — with word-wrapping */
            int usable = cols - 2;
            if (usable < 10) usable = 10;
            const char *bq_text = line_buf + 2;
            int bq_len = (int)strlen(bq_text);

            int lines_consumed = 1;
            if (bq_len > 0) {
                if (visible) {
                    /* Draw the first │ */
                    wattron(win, COLOR_PAIR(C_DIM));
                    mvwaddch(win, vis_line, 0, ACS_VLINE);
                    wattroff(win, COLOR_PAIR(C_DIM));

                    lines_consumed = render_inline_wrapped(win, vis_line, 2, bq_text, bq_len, usable);
                } else {
                    /* Item 4: use count_wrapped_lines for off-screen counting */
                    lines_consumed = count_wrapped_lines(bq_text, bq_len, usable);
                }
            }

            /* Draw │ on continuation lines */
            for (int i = 1; i < lines_consumed; i++) {
                int cvl = (render_line + i) - scroll_y;
                if (cvl >= 0 && cvl < rows) {
                    wattron(win, COLOR_PAIR(C_DIM));
                    mvwaddch(win, cvl, 0, ACS_VLINE);
                    wattroff(win, COLOR_PAIR(C_DIM));
                }
            }

            /* Item 9: explicit line advancement */
            advance_render_line(&render_line, lines_consumed);

        } else if (strncmp(line_buf, "  + ", 4) == 0 ||
                   strncmp(line_buf, "  x ", 4) == 0) {
            /* React step line: success (+) or failure (x) */
            if (visible) {
                int is_fail = (line_buf[2] == 'x');
                int pair = is_fail ? C_FAILED : C_SUCCESS;

                /* Parse inline formatting */
                inline_seg_t segs[MAX_INLINE_SEGS];
                int n = parse_inline(line_buf, (int)strlen(line_buf), segs, MAX_INLINE_SEGS);
                /* Item 5: use apply_attr_to_segs */
                apply_attr_to_segs(segs, n, COLOR_PAIR(pair));
                render_segs_on_line(win, vis_line, 0, segs, n, cols);
            }

        } else if (line_buf[0] == '|') {
            /* Table block — Item 2: delegate to render_table() */
            /* Pre-scan to count consecutive | rows */
            int table_rows = 0;
            {
                const char *scan = src;
                while (scan && *scan == '|') {
                    table_rows++;
                    const char *nl = strchr(scan, '\n');
                    scan = nl ? nl + 1 : NULL;
                    if (!scan || *scan != '|') break;
                }
            }

            const char *out_src = src;
            (void)render_table(win, src, table_rows,
                                scroll_y, scroll_x, rows, cols,
                                &render_line, &src_line, &out_src);
            /* render_table() sets out_src to the last row; skip the main
             * loop's advance for all but the last row. */
            if (out_src != src) {
                /* render_table advanced render_line for all rows except the last.
                 * The last row is left for the main loop's render_line++ below. */
                src = out_src;
                eol = strchr(src, '\n');
                /* Don't re-copy line_buf — the last row was already handled. */
            }

        } else {
            /* Regular text — with inline formatting and word-wrapping */
            int lines_consumed = 1;
            if (line_buf[0] != '\0') {
                if (visible) {
                    lines_consumed = render_inline_wrapped(win, vis_line, 0, line_buf, (int)strlen(line_buf), cols);
                } else {
                    /* Item 4: use count_wrapped_lines for off-screen counting */
                    lines_consumed = count_wrapped_lines(line_buf, (int)strlen(line_buf), cols);
                }
            }
            /* Item 9: explicit line advancement */
            advance_render_line(&render_line, lines_consumed);
        }

        render_line++;
        src_line++;
        src = eol ? eol + 1 : src + strlen(src);
    }

    doc->total_lines = render_line;
    wnoutrefresh(win);
    return render_line;
}

int md_link_line(md_doc_t *doc, int link_idx) {
    if (!doc || link_idx < 0 || link_idx >= doc->link_count)
        return 0;
    /* Use render_line if set (accounts for skipped ``` fence lines),
     * otherwise fall back to doc_line (before first render). */
    int rl = doc->links[link_idx].render_line;
    return (rl >= 0) ? rl : doc->links[link_idx].doc_line;
}

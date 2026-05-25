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

/* Render inline formatting within a line (bold, code, italic).
 * Uses mvwaddnstr for proper UTF-8 handling instead of byte-by-byte mvwaddch. */
static void render_inline(WINDOW *win, int row, int col, const char *text,
                          int max_width, int base_attr) {
    int x = col;
    const char *p = text;
    (void)base_attr;

    while (*p && x < col + max_width) {
        if (*p == '`') {
            /* Inline code: find closing backtick */
            p++;
            const char *start = p;
            while (*p && *p != '`') p++;
            int seg_len = (int)(p - start);
            wattron(win, COLOR_PAIR(C_STREAM));
            x += render_segment(win, row, x, start, seg_len, col + max_width - x);
            wattroff(win, COLOR_PAIR(C_STREAM));
            if (*p == '`') p++;
        } else if (p[0] == '*' && p[1] == '*') {
            /* Bold: find closing ** */
            p += 2;
            const char *start = p;
            while (*p && !(p[0] == '*' && p[1] == '*')) p++;
            int seg_len = (int)(p - start);
            wattron(win, A_BOLD);
            x += render_segment(win, row, x, start, seg_len, col + max_width - x);
            wattroff(win, A_BOLD);
            if (p[0] == '*' && p[1] == '*') p += 2;
        } else if (*p == '*') {
            /* Italic (underline in ncurses): find closing * */
            p++;
            const char *start = p;
            while (*p && *p != '*') p++;
            int seg_len = (int)(p - start);
            wattron(win, A_UNDERLINE);
            x += render_segment(win, row, x, start, seg_len, col + max_width - x);
            wattroff(win, A_UNDERLINE);
            if (*p == '*') p++;
        } else {
            /* Regular text: collect until next formatting marker or end */
            const char *start = p;
            while (*p && *p != '`' && *p != '*' && x < col + max_width) {
                p++;
            }
            int seg_len = (int)(p - start);
            if (seg_len > 0)
                x += render_segment(win, row, x, start, seg_len, col + max_width - x);
        }
    }
}

/* ── Main render function ── */

int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int cursor_link,
              int focus, int scroll_x) {
    if (!win || !doc || !doc->source) return 0;

    int rows = getmaxy(win);
    int cols = getmaxx(win);

    werase(win);

    /* Helper: render text with word-wrapping.
     * Returns the number of extra lines consumed (0 if no wrap needed). */
    #define RENDER_WRAPPED(win, vis_line, indent, text, textlen, cols, rows, render_line) do { \
        int _remaining = (textlen); \
        const char *_wp = (text); \
        int _usable = (cols) - (indent); \
        if (_usable < 10) _usable = 10; \
        int _first = 1; \
        while (_remaining > 0) { \
            int _chunk = _remaining > _usable ? _usable : _remaining; \
            int _vl = (render_line) - scroll_y; \
            if (_vl >= 0 && _vl < (rows)) \
                mvwaddnstr((win), _vl, _first ? (indent) : (indent), _wp, _chunk); \
            _wp += _chunk; \
            _remaining -= _chunk; \
            if (_remaining > 0) { \
                (render_line)++; \
            } \
            _first = 0; \
        } \
    } while(0)

    const char *src = doc->source;
    int render_line = 0;  /* line number in the full document */
    int src_line = 0;     /* source line number (for link matching) */
    int link_idx = 0;     /* current link index */
    int in_code_block = 0;

    while (*src) {
        /* Extract one line */
        const char *eol = strchr(src, '\n');
        int line_len = eol ? (int)(eol - src) : (int)strlen(src);

        /* Copy line to buffer for processing */
        char line_buf[4096];
        int copy_len = line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
        memcpy(line_buf, src, copy_len);
        line_buf[copy_len] = '\0';

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

        /* Check if this line is within the visible window */
        int vis_line = render_line - scroll_y;

        if (vis_line >= 0 && vis_line < rows) {
            if (in_code_block) {
                /* Code block content: render in cyan with line wrapping */
                wattron(win, COLOR_PAIR(C_STREAM));
                int remaining = copy_len;
                const char *wp = line_buf;
                int first = 1;
                while (remaining > 0) {
                    int chunk = remaining > (cols - 2) ? (cols - 2) : remaining;
                    if (vis_line >= 0 && vis_line < rows)
                        mvwaddnstr(win, vis_line, first ? 2 : 4, wp, chunk);
                    wp += chunk;
                    remaining -= chunk;
                    if (remaining > 0) {
                        render_line++;
                        vis_line = render_line - scroll_y;
                        first = 0;
                    }
                }
                wattroff(win, COLOR_PAIR(C_STREAM));

            } else if (line_buf[0] == '#') {
                /* Heading */
                int level = 0;
                while (line_buf[level] == '#') level++;
                const char *htext = line_buf + level;
                while (*htext == ' ') htext++;

                int pair = (level == 1) ? C_SUCCESS : C_FOCUS;
                wattron(win, COLOR_PAIR(pair) | A_BOLD);
                mvwaddnstr(win, vis_line, 0, htext, cols);
                wattroff(win, COLOR_PAIR(pair) | A_BOLD);

            } else if (strncmp(line_buf, "---", 3) == 0) {
                /* Horizontal rule */
                wattron(win, COLOR_PAIR(C_DIM));
                mvwhline(win, vis_line, 0, ACS_HLINE, cols);
                wattroff(win, COLOR_PAIR(C_DIM));

            } else if (line_buf[0] == '[' && link_idx < doc->link_count &&
                       src_line == doc->links[link_idx].doc_line) {
                /* Hyperlink line */
                int is_cursor = (focus && link_idx == cursor_link);
                md_link_t *lk = &doc->links[link_idx];
                lk->render_line = render_line;
                link_idx++;

                if (is_cursor) {
                    wattron(win, A_REVERSE | A_BOLD);
                } else {
                    wattron(win, COLOR_PAIR(C_FOCUS));
                }

                /* Render the link text (not the URI) */
                mvwaddnstr(win, vis_line, 0, lk->text, cols);

                /* Pad for reverse video */
                if (is_cursor) {
                    int cur_x = getcurx(win);
                    for (int x = cur_x; x < cols; x++)
                        mvwaddch(win, vis_line, x, ' ');
                    wattroff(win, A_REVERSE | A_BOLD);
                } else {
                    wattroff(win, COLOR_PAIR(C_FOCUS));
                }

            } else if (line_buf[0] == '>' && line_buf[1] == ' ') {
                /* Blockquote */
                wattron(win, COLOR_PAIR(C_DIM));
                mvwaddch(win, vis_line, 0, ACS_VLINE);
                wattroff(win, COLOR_PAIR(C_DIM));
                render_inline(win, vis_line, 2, line_buf + 2, cols - 2, 0);

            } else if (strncmp(line_buf, "  + ", 4) == 0 ||
                       strncmp(line_buf, "  x ", 4) == 0) {
                /* React step line: success (+) or failure (x) */
                int is_fail = (line_buf[2] == 'x');
                int pair = is_fail ? C_FAILED : C_SUCCESS;
                wattron(win, COLOR_PAIR(pair));
                mvwaddnstr(win, vis_line, 0, line_buf, cols);
                wattroff(win, COLOR_PAIR(pair));

            } else if (line_buf[0] == '|') {
                /* Table block — render ALL consecutive | rows at once
                 * so column widths are consistent across the entire table. */
                #define MAX_TABLE_COLS 20

                /* Pass 1: scan ALL consecutive | lines to find max column widths */
                int col_widths[MAX_TABLE_COLS] = {0};
                int num_cols = 0;
                int table_rows = 0;
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
                        table_rows++;
                        const char *nl = strchr(scan, '\n');
                        scan = nl ? nl + 1 : NULL;
                        if (!scan || *scan != '|') break;
                    }
                }

                /* Pass 2: render ALL table rows with consistent col_widths.
                 * Tables use horizontal scroll (scroll_x) instead of wrapping. */
                int tbl_sx = scroll_x;
                const char *trow = src;
                for (int tr = 0; tr < table_rows && trow; tr++) {
                    const char *trow_eol = strchr(trow, '\n');
                    int trow_len = trow_eol ? (int)(trow_eol - trow) : (int)strlen(trow);
                    char tbuf[4096];
                    int tcopy = trow_len < (int)sizeof(tbuf)-1 ? trow_len : (int)sizeof(tbuf)-1;
                    memcpy(tbuf, trow, tcopy);
                    tbuf[tcopy] = '\0';

                    vis_line = render_line - scroll_y;

                    if (vis_line >= 0 && vis_line < rows) {
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
                                    /* Render cell text, clipping to visible region */
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
                    if (tr < table_rows - 1) {
                        render_line++;
                        src_line++;
                        trow = trow_eol ? trow_eol + 1 : NULL;
                    } else {
                        /* Last row — let the main loop advance normally */
                        /* Update src/eol to point to the last table row */
                        src = trow;
                        eol = trow_eol;
                    }
                }

            } else {
                /* Regular text with inline formatting — wrap to terminal width */
                int tlen = copy_len;
                if (tlen <= cols) {
                    render_inline(win, vis_line, 0, line_buf, cols, 0);
                } else {
                    /* Word-wrap: render in chunks of cols width.
                     * First chunk uses render_inline (formatting), continuation
                     * lines use mvwaddnstr (plain) to avoid splitting markers. */
                    const char *wp = line_buf;
                    int remaining = tlen;
                    int first = 1;
                    while (remaining > 0) {
                        int chunk = remaining > cols ? cols : remaining;
                        int vl = render_line - scroll_y;
                        if (vl >= 0 && vl < rows) {
                            if (first)
                                render_inline(win, vl, 0, wp, cols, 0);
                            else
                                mvwaddnstr(win, vl, 0, wp, chunk);
                        }
                        wp += chunk;
                        remaining -= chunk;
                        first = 0;
                        if (remaining > 0) {
                            render_line++;
                        }
                    }
                }
            }

        /* --- Lines OUTSIDE visible window still need wrapping for line count --- */
        } else {
            /* Off-screen: count wrapped lines so render_line stays accurate */
            int tlen = copy_len;
            if (tlen > cols && !in_code_block && line_buf[0] != '|') {
                int extra = (tlen - 1) / cols;  /* additional lines from wrapping */
                render_line += extra;
            }
            /* Off-screen: still need to count links */
            if (line_buf[0] == '[' && link_idx < doc->link_count &&
                src_line == doc->links[link_idx].doc_line) {
                doc->links[link_idx].render_line = render_line;
                link_idx++;
            }
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

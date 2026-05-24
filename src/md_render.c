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

/* Check if a line is a heading, return level (1-3) or 0 */
static int heading_level(const char *line) {
    int n = 0;
    while (line[n] == '#') n++;
    if (n >= 1 && n <= 3 && (line[n] == ' ' || line[n] == '\0'))
        return n;
    return 0;
}

/* Check if line is a horizontal rule (--- or ===) */
static int is_hrule(const char *line) {
    int dashes = 0;
    const char *p = line;
    while (*p == ' ') p++;
    while (*p == '-' || *p == '=') { dashes++; p++; }
    while (*p == ' ') p++;
    return (*p == '\0' && dashes >= 3);
}

/* Find the link index for a given source line, or -1 */
static int find_link_at_line(md_doc_t *doc, int line_num) {
    for (int i = 0; i < doc->link_count; i++) {
        if (doc->links[i].doc_line == line_num)
            return i;
    }
    return -1;
}

/* Render inline formatting within a line (bold, code, italic) */
static void render_inline(WINDOW *win, int row, int col, const char *text,
                          int max_width, int base_attr) {
    int x = col;
    const char *p = text;

    while (*p && x < col + max_width) {
        if (*p == '`') {
            /* Inline code */
            p++;
            wattron(win, COLOR_PAIR(C_STREAM));
            while (*p && *p != '`' && x < col + max_width) {
                mvwaddch(win, row, x++, *p++);
            }
            wattroff(win, COLOR_PAIR(C_STREAM));
            if (*p == '`') p++;
        } else if (p[0] == '*' && p[1] == '*') {
            /* Bold */
            p += 2;
            wattron(win, A_BOLD);
            while (*p && !(p[0] == '*' && p[1] == '*') && x < col + max_width) {
                mvwaddch(win, row, x++, *p++);
            }
            wattroff(win, A_BOLD);
            if (p[0] == '*' && p[1] == '*') p += 2;
        } else if (*p == '*') {
            /* Italic (rendered as underline in ncurses) */
            p++;
            wattron(win, A_UNDERLINE);
            while (*p && *p != '*' && x < col + max_width) {
                mvwaddch(win, row, x++, *p++);
            }
            wattroff(win, A_UNDERLINE);
            if (*p == '*') p++;
        } else {
            mvwaddch(win, row, x++, *p++);
        }
    }
}

/* ── Main render function ── */

int md_render(WINDOW *win, md_doc_t *doc, int scroll_y, int cursor_link,
              int focus) {
    if (!win || !doc || !doc->source) return 0;

    int rows = getmaxy(win);
    int cols = getmaxx(win);
    werase(win);

    const char *src = doc->source;
    int src_line = 0;       /* line number in source */
    int render_line = 0;    /* line number in rendered output */

    while (*src) {
        /* Extract one source line */
        const char *eol = strchr(src, '\n');
        int line_len = eol ? (int)(eol - src) : (int)strlen(src);
        char line_buf[4096];
        int copy_len = line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
        memcpy(line_buf, src, copy_len);
        line_buf[copy_len] = '\0';

        /* Check if this line contains a link */
        int link_idx = find_link_at_line(doc, src_line);

        /* Determine rendering style */
        int vis_line = render_line - scroll_y;  /* visible row on screen */

        if (vis_line >= 0 && vis_line < rows) {
            int hlevel = heading_level(line_buf);

            if (hlevel > 0) {
                /* Heading */
                const char *htext = line_buf + hlevel;
                while (*htext == ' ') htext++;
                int attr = A_BOLD;
                int pair = (hlevel == 1) ? C_SUCCESS : C_FOCUS;
                wattron(win, attr | COLOR_PAIR(pair));
                mvwaddnstr(win, vis_line, 0, htext, cols);
                wattroff(win, attr | COLOR_PAIR(pair));

            } else if (is_hrule(line_buf)) {
                /* Horizontal rule */
                wattron(win, COLOR_PAIR(C_DIM));
                mvwhline(win, vis_line, 0, ACS_HLINE, cols);
                wattroff(win, COLOR_PAIR(C_DIM));

            } else if (link_idx >= 0) {
                /* Hyperlink line */
                md_link_t *lk = &doc->links[link_idx];
                int is_cursor = (focus && link_idx == cursor_link);

                if (is_cursor) {
                    wattron(win, A_REVERSE | A_BOLD);
                } else {
                    wattron(win, COLOR_PAIR(C_FOCUS));
                }

                /* Render the link text (not the [text](uri) syntax) */
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

            } else {
                /* Regular text with inline formatting */
                render_inline(win, vis_line, 0, line_buf, cols, 0);
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
    return doc->links[link_idx].doc_line;
}

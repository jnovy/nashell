/* md_diff.c -- Diff line detection and rendering. Extracted from md_render.c. */

#include "md_render_internal.h"

int is_diff_line(const char *line, int line_len) {
    if (line_len < 1) return 0;

    /* New format: "  {5d} +code" — leading spaces, digits, space, then +/-/space */
    const char *p = line;
    const char *end = line + line_len;

    /* Skip leading spaces */
    while (p < end && *p == ' ') p++;

    /* Check for digits (line number) */
    const char *digit_start = p;
    while (p < end && *p >= '0' && *p <= '9') p++;

    if (p > digit_start && p < end && *p == ' ') {
        /* Found "  {digits} " — now check the diff marker */
        p++; /* skip the space after line number */
        if (p >= end) return 2; /* context line (empty after number) */
        if (*p == '+') return 1;
        if (*p == '-') return -1;
        return 2; /* context line (space prefix) */
    }

    /* Old format: "+code" / "-code" at column 0 */
    if (line[0] == '+' || line[0] == '-') {
        if (line_len == 1 || line[1] == ' ' || line[1] == '\t') {
            return (line[0] == '+') ? 1 : -1;
        }
    }
    return 0;
}

/* Parse a diff line to find the line number prefix end and content start.
 * For "  {5d} +code": lnum_start points to first digit, lnum_len is digit count,
 * content_start points to the code after +/-.
 * For old format "+ code": lnum_start=NULL, content_start points to code after "+ ". */
static void parse_diff_parts(const char *line, int line_len,
                              const char **lnum_start, int *lnum_len,
                              const char **marker_pos,
                              const char **content_start, int *content_len) {
    const char *p = line;
    const char *end = line + line_len;

    *lnum_start = NULL;
    *lnum_len = 0;
    *marker_pos = NULL;
    *content_start = line;
    *content_len = line_len;

    /* Skip leading spaces */
    while (p < end && *p == ' ') p++;

    /* Check for digits (line number) */
    const char *ds = p;
    while (p < end && *p >= '0' && *p <= '9') p++;

    if (p > ds && p < end && *p == ' ') {
        *lnum_start = ds;
        *lnum_len = (int)(p - ds);
        p++; /* skip space after line number */
        *marker_pos = p;
        if (p < end && (*p == '+' || *p == '-')) {
            *content_start = p; /* include the +/- in colored content */
            *content_len = (int)(end - p);
        } else {
            /* Context line — content starts at the space after marker */
            *content_start = p;
            *content_len = (int)(end - p);
        }
    } else {
        /* Old format: +/- at column 0 */
        *content_start = line;
        *content_len = line_len;
    }
}

/* ── Character-level diff highlighting for paired -/+ lines ── */

/* Compute the byte range within content that differs between two strings.
 * Finds common prefix and common suffix, the middle portion is "changed".
 * hl_start/hl_end are byte offsets into content_a (for del) or content_b (for add).
 * If lines are identical, hl_start == hl_end (empty highlight). */
void compute_char_diff(const char *a, int a_len,
                       const char *b, int b_len,
                       int *hl_start_a, int *hl_end_a,
                       int *hl_start_b, int *hl_end_b) {
    /* Find common prefix length (in bytes) */
    int prefix = 0;
    int min_len = a_len < b_len ? a_len : b_len;
    while (prefix < min_len && a[prefix] == b[prefix])
        prefix++;

    /* Find common suffix length (in bytes), not overlapping prefix */
    int suffix = 0;
    while (suffix < (a_len - prefix) && suffix < (b_len - prefix) &&
           a[a_len - 1 - suffix] == b[b_len - 1 - suffix])
        suffix++;

    *hl_start_a = prefix;
    *hl_end_a   = a_len - suffix;
    *hl_start_b = prefix;
    *hl_end_b   = b_len - suffix;

    /* Clamp: if entire line changed, don't highlight (it's just a full change) */
    if (prefix == 0 && suffix == 0 && a_len > 0 && b_len > 0) {
        /* Check if lines share at least some content — if they share < 30%,
         * skip char highlighting (too different to be useful) */
        int shared = 0;
        for (int i = 0; i < min_len; i++)
            if (a[i] == b[i]) shared++;
        if (shared * 100 / (min_len > 0 ? min_len : 1) < 30) {
            *hl_start_a = *hl_end_a = -1;
            *hl_start_b = *hl_end_b = -1;
        }
    }
}

/* Extract the code content portion from a diff line (after the +/- marker).
 * Returns pointer to content and sets *out_len. The +/- marker itself is skipped. */
const char *diff_content_after_marker(const char *text, int text_len,
                                              int *out_len) {
    const char *lnum_start, *marker_pos, *content_start;
    int lnum_len, content_len;
    parse_diff_parts(text, text_len, &lnum_start, &lnum_len,
                     &marker_pos, &content_start, &content_len);
    /* content_start includes the +/- marker; skip it */
    if (content_len > 0 && (*content_start == '+' || *content_start == '-')) {
        content_start++;
        content_len--;
    }
    *out_len = content_len;
    return content_start;
}

/* Render a diff line with line number (dim) and colored background.
 * diff_type: 1 = add (green bg), -1 = remove (red bg), 2 = context.
 * text: the full line text.
 * text_len: byte length of text.
 * scroll_x: horizontal scroll offset (0 = no scroll).
 * hl_start, hl_end: byte offsets within the code content (after +/- marker)
 *   for character-level highlighting.  -1 = no char highlight.
 * Returns number of display lines consumed. */
int render_diff_line(WINDOW *win, int row, int col,
                             int diff_type, const char *text, int text_len,
                             int cols, int scroll_x,
                             int hl_start, int hl_end) {
    if (text_len <= 0) return 1;

    /* Parse line number and content parts */
    const char *lnum_start, *marker_pos, *content_start;
    int lnum_len, content_len;
    parse_diff_parts(text, text_len, &lnum_start, &lnum_len,
                     &marker_pos, &content_start, &content_len);

    int x = col - scroll_x;

    /* Render leading spaces + line number in dim */
    if (lnum_start) {
        /* Leading spaces before line number */
        int leading = (int)(lnum_start - text);
        if (leading > 0) {
            if (x >= 0 && x < cols) {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwaddnstr(win, row, x, text, leading);
                wattroff(win, COLOR_PAIR(C_DIM));
            }
            x += leading;
        }
        /* Line number in dim */
        if (x >= 0 && x < cols) {
            wattron(win, COLOR_PAIR(C_DIM));
            mvwaddnstr(win, row, x, lnum_start, lnum_len);
            wattroff(win, COLOR_PAIR(C_DIM));
        }
        x += lnum_len;
        /* Space after line number */
        if (x >= 0 && x < cols)
            mvwaddch(win, row, x, ' ');
        x++;
    }

    /* For context lines (diff_type == 2), render content without background */
    if (diff_type == 2) {
        if (content_len > 0 && x >= 0 && x < cols) {
            wattron(win, COLOR_PAIR(C_STREAM));
            mvwaddnstr(win, row, x, content_start, content_len);
            wattroff(win, COLOR_PAIR(C_STREAM));
        }
        return 1;
    }

    /* For add/remove lines, render content with colored background */
    int pair    = (diff_type > 0) ? CP_DIFF_ADD    : CP_DIFF_DEL;
    int pair_hl = (diff_type > 0) ? CP_DIFF_ADD_HL : CP_DIFF_DEL_HL;

    if (content_len > 0) {
        /* Find offset of code content (after +/- marker) within content_start */
        const char *code = content_start;
        int code_len = content_len;
        int marker_bytes = 0;
        if (*content_start == '+' || *content_start == '-') {
            marker_bytes = 1;
            code = content_start + 1;
            code_len = content_len - 1;
        }

        /* Render the +/- marker with normal diff bg */
        if (marker_bytes > 0) {
            if (x >= 0 && x < cols) {
                wattron(win, COLOR_PAIR(pair));
                mvwaddnstr(win, row, x, content_start, marker_bytes);
                wattroff(win, COLOR_PAIR(pair));
            }
            x += marker_bytes;
        }

        /* Render code content with optional char-level highlighting */
        if (hl_start >= 0 && hl_end > hl_start &&
            hl_start < code_len) {
            /* Clamp highlight range */
            if (hl_end > code_len) hl_end = code_len;

            /* Pre-highlight portion */
            if (hl_start > 0) {
                int dw = utf8_display_len(code, hl_start);
                if (x >= 0 && x < cols) {
                    wattron(win, COLOR_PAIR(pair));
                    mvwaddnstr(win, row, x, code, hl_start);
                    wattroff(win, COLOR_PAIR(pair));
                }
                x += dw;
            }
            /* Highlighted portion (brighter bg) */
            int hl_len = hl_end - hl_start;
            int hl_dw = utf8_display_len(code + hl_start, hl_len);
            if (x >= 0 && x < cols) {
                wattron(win, COLOR_PAIR(pair_hl) | A_BOLD);
                mvwaddnstr(win, row, x, code + hl_start, hl_len);
                wattroff(win, COLOR_PAIR(pair_hl) | A_BOLD);
            }
            x += hl_dw;
            /* Post-highlight portion */
            int post_len = code_len - hl_end;
            if (post_len > 0) {
                int post_dw = utf8_display_len(code + hl_end, post_len);
                if (x >= 0 && x < cols) {
                    wattron(win, COLOR_PAIR(pair));
                    mvwaddnstr(win, row, x, code + hl_end, post_len);
                    wattroff(win, COLOR_PAIR(pair));
                }
                x += post_dw;
            }
        } else {
            /* No char highlight — render entire code content */
            if (code_len > 0) {
                int dw = utf8_display_len(code, code_len);
                if (x >= 0 && x < cols) {
                    wattron(win, COLOR_PAIR(pair));
                    mvwaddnstr(win, row, x, code, code_len);
                    wattroff(win, COLOR_PAIR(pair));
                }
                x += dw;
            }
        }
    }

    /* Pad remaining columns with diff background */
    int pad_start = (x > 0) ? x : 0;
    wattron(win, COLOR_PAIR(pair));
    for (int px = pad_start; px < cols; px++)
        mvwaddch(win, row, px, ' ');
    wattroff(win, COLOR_PAIR(pair));

    return 1;
}

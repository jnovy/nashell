#include "md_render.h"
#include "nash_limits.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <utf8proc.h>

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

/* Maximum segments per line (generous limit for nested formatting) */
#define MAX_INLINE_SEGS 64

/* An inline formatting segment: text pointer, length, and ncurses attr */
typedef struct {
    const char *text;
    int         len;      /* byte length */
    int         attr;     /* ncurses attribute (0 = default) */
    const char *url;      /* URL pointer for [text](url) links (NULL if not a link) */
    int         url_len;  /* URL byte length (0 if not a link) */
} inline_seg_t;

/* Forward declarations */
static int utf8_display_len(const char *s, int max_bytes);
static int render_segment(WINDOW *win, int row, int col, const char *text,
                          int len, int max_cols);
static int parse_inline(const char *text, int text_len, inline_seg_t *segs, int max_segs);
static void defer_osc8_link(int vis_line, int col, const char *uri);
static void defer_osc8_end(int col_end);
static int is_linkable_uri(const char *uri);
static int is_web_uri(const char *uri);

/* ── Helpers (Items 3-9) ── */

/* Decode one UTF-8 character starting at s[0..max_bytes-1].
 * Stores the codepoint in *cp and returns the number of bytes consumed.
 * On invalid sequences, sets *cp = s[0] and returns 1 (treat as 1-col). */
static int utf8_decode(const char *s, int max_bytes, wchar_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len; wchar_t val;
    if      ((c & 0xE0) == 0xC0) { len = 2; val = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; val = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; val = c & 0x07; }
    else { *cp = c; return 1; }
    if (len > max_bytes) { *cp = c; return 1; }
    for (int i = 1; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { *cp = c; return 1; }
        val = (val << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *cp = val;
    return len;
}

/* Return the display width (columns) of a Unicode codepoint.
 * Handles CJK, emoji, and other wide characters without wcwidth(). */
static int codepoint_width(wchar_t cp) {
    /* Delegate to utf8proc which has complete Unicode character width tables
     * (East_Asian_Width, Emoji_Presentation, combining marks, etc.).
     * Returns 0 for zero-width/control, 1 for normal, 2 for wide/emoji. */
    int w = utf8proc_charwidth((utf8proc_int32_t)cp);
    /* utf8proc returns 0 for control chars and unassigned codepoints;
     * treat truly unassigned/unrecognized printable as width 1 */
    if (w <= 0 && cp >= 0x20 && cp != 0x7F)
        return (utf8proc_category((utf8proc_int32_t)cp) == UTF8PROC_CATEGORY_CN) ? 1 : w;
    return w > 0 ? w : 0;
}

/* Return the display width (columns) of one UTF-8 character. */
static int utf8_char_width(const char *s, int max_bytes) {
    wchar_t cp;
    utf8_decode(s, max_bytes, &cp);
    int w = codepoint_width(cp);
    return (w > 0) ? w : 1;
}

/* Count display columns for a segment (delegates to utf8_display_len).
 * Item 3: eliminated duplicate UTF-8 column counting — seg_display_cols
 * and utf8_display_len were 100% identical. */
static int seg_display_cols(const char *text, int len) {
    return utf8_display_len(text, len);
}

/* Get the byte offset within a segment that corresponds to a given column offset.
 * Returns byte position where the segment should be split. */
static int seg_col_to_byte(const char *text, int seg_len, int col_offset) {
    int col = 0, i = 0;
    while (i < seg_len && text[i]) {
        if (col >= col_offset) return i;
        int w = utf8_char_width(text + i, seg_len - i);
        wchar_t cp;
        int clen = utf8_decode(text + i, seg_len - i, &cp);
        col += w;
        i += clen;
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
        /* Found closing marker — recursively parse inner content for
         * nested inline elements (e.g., links inside bold: **[text](url)**).
         * Each sub-segment inherits the parent attribute (bold/italic). */
        int inner_len = (int)(p - start);
        int saved_n = *n;
        int sub_n = parse_inline(start, inner_len,
                                 segs + saved_n, max_segs - saved_n);
        if (sub_n > 0) {
            /* Apply parent attr to all sub-segments */
            for (int si = saved_n; si < saved_n + sub_n; si++)
                segs[si].attr |= attr;
            *n = saved_n + sub_n;
        } else {
            /* Fallback: emit as single formatted segment */
            segs[*n].text = start;
            segs[*n].len = inner_len;
            segs[*n].attr = attr;
            segs[*n].url = NULL;
            segs[*n].url_len = 0;
            (*n)++;
        }
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
                segs[*n].url = NULL;
                segs[*n].url_len = 0;
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

        /* Markdown link: [text](url) — render only text with link color */
        if (*p == '[') {
            const char *bracket_end = NULL;
            for (const char *s = p + 1; s < end; s++) {
                if (*s == ']') { bracket_end = s; break; }
                if (*s == '\n') break;  /* links don't span lines */
            }
            if (bracket_end && bracket_end + 1 < end && bracket_end[1] == '(') {
                const char *paren_end = NULL;
                for (const char *s = bracket_end + 2; s < end; s++) {
                    if (*s == ')') { paren_end = s; break; }
                    if (*s == '\n') break;
                }
                if (paren_end) {
                    int link_text_len = (int)(bracket_end - p - 1);
                    if (link_text_len > 0) {
                        segs[n].text = p + 1;
                        segs[n].len = link_text_len;
                        segs[n].attr = COLOR_PAIR(C_FOCUS);
                        segs[n].url = bracket_end + 2;
                        segs[n].url_len = (int)(paren_end - (bracket_end + 2));
                        n++;
                    }
                    p = paren_end + 1;
                    continue;
                }
            }
            /* Not a valid link — fall through, '[' consumed as regular text */
        }

        /* Regular text: collect until next formatting marker or link */
        const char *start = p;
        while (p < end) {
            if (*p == '`') break;
            if (*p == '*' && p + 1 < end && p[1] == '*') break;
            if (*p == '*') break;
            /* Break on '[' only if it looks like a markdown link [text](url) */
            if (*p == '[' && p != start) {
                const char *be = memchr(p + 1, ']', end - p - 1);
                if (be && be + 1 < end && be[1] == '(')
                    break;
            }
            p++;
        }
        int seg_len = (int)(p - start);
        if (seg_len > 0) {
            segs[n].text = start;
            segs[n].len = seg_len;
            segs[n].attr = 0;
            segs[n].url = NULL;
            segs[n].url_len = 0;
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
        /* OSC 8 hyperlink start for link segments */
        int has_osc8 = 0;
        if (segs[i].url && segs[i].url_len > 0) {
            char url_buf[4096];
            int ulen = segs[i].url_len;
            if (ulen >= (int)sizeof(url_buf)) ulen = (int)sizeof(url_buf) - 1;
            memcpy(url_buf, segs[i].url, ulen);
            url_buf[ulen] = '\0';
            if (is_linkable_uri(url_buf)) {
                defer_osc8_link(row, x, url_buf);
                has_osc8 = 1;
            }
        }
        if (segs[i].attr)
            wattron(win, segs[i].attr);
        x += render_segment(win, row, x, segs[i].text, segs[i].len, remaining);
        if (segs[i].attr)
            wattroff(win, segs[i].attr);
        /* OSC 8 hyperlink end */
        if (has_osc8)
            defer_osc8_end(x);
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
    segs[seg_idx + 1].url = seg->url;
    segs[seg_idx + 1].url_len = seg->url_len;
    seg->len = split_byte;
    (*n_segs)++;
    return 1;
}

/* Render a set of segments with word-wrapping across display lines.
 * Segments are split at word boundaries respecting formatting boundaries.
 * Returns number of display lines consumed. */
/* Extended word-wrapped segment renderer.
 * cont_col / cont_width: column and width for continuation lines.
 * Pass cont_col == col && cont_width == usable_width for uniform indent.
 * Pass cont_col == 0  && cont_width == full_cols to wrap continuations
 * at full terminal width (e.g. step-line suffix after [tool](uri)). */
static int render_segs_wrapped_ex(WINDOW *win, int start_row, int col,
                                  inline_seg_t *segs, int n_segs,
                                  int usable_width,
                                  int cont_col, int cont_width) {
    if (usable_width < 5) usable_width = 5;
    if (cont_width < 5) cont_width = 5;

    int current_row = start_row;
    int lines_used = 1;
    int cur_col = col;
    int cur_width = usable_width;

    while (n_segs > 0) {
        /* Find how many full segments fit on this line */
        int x = 0;
        int fit_count = 0;
        int partial_seg = -1;
        int partial_byte = 0;

        for (int i = 0; i < n_segs; i++) {
            int dc = seg_display_cols(segs[i].text, segs[i].len);
            if (x + dc <= cur_width) {
                x += dc;
                fit_count++;
            } else {
                /* Check if we can fit a partial segment */
                int remaining = cur_width - x;
                if (remaining > 0 && segs[i].len > 0) {
                    /* Find last space within the segment that fits */
                    int split_at = seg_col_to_byte(segs[i].text, segs[i].len, remaining);
                    /* Item 7: use shared word-boundary helper */
                    int min_split = (cur_width / 4 < split_at) ? cur_width / 4 : 1;
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
            render_segs_on_line(win, current_row, cur_col, segs, render_count, cur_width);
        } else {
            /* Even a single character doesn't fit — render one char to avoid infinite loop */
            if (n_segs > 0 && segs[0].len > 0) {
                /* Find byte for 1 column */
                int b = 0;
                while (b < segs[0].len && segs[0].text[b] && (segs[0].text[b] & 0xC0) == 0x80) b++;
                if (b < segs[0].len) {
                    split_segment(segs, &n_segs, 256, 0, b + 1);
                }
                render_segs_on_line(win, current_row, cur_col, segs, 1, cur_width);
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
            /* Switch to continuation column/width after first line */
            cur_col = cont_col;
            cur_width = cont_width;
        }
    }

    return lines_used;
}

static int render_segs_wrapped(WINDOW *win, int start_row, int col,
                               inline_seg_t *segs, int n_segs, int usable_width) {
    return render_segs_wrapped_ex(win, start_row, col, segs, n_segs,
                                 usable_width, col, usable_width);
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

/* ── Diff line rendering ── */

/* Check if a line is a diff add/remove/context line.
 * Supports two formats:
 *   Old: "+ code" / "- code" / "  code" (prefix at column 0)
 *   New: "  {5d} +code" / "  {5d} -code" / "  {5d}  code" (with line numbers)
 * Returns: 1 = diff add (+), -1 = diff remove (-), 2 = context, 0 = not a diff line.
 * Sets *content_offset to the byte offset where the actual code content starts
 * (after the +/- prefix), and *lnum_end to the end of the line number prefix. */
static int is_diff_line(const char *line, int line_len) {
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
static void compute_char_diff(const char *a, int a_len,
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
static const char *diff_content_after_marker(const char *text, int text_len,
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
 * hl_start, hl_end: byte offsets within the code content (after +/- marker)
 *   for character-level highlighting.  -1 = no char highlight.
 * Returns number of display lines consumed. */
static int render_diff_line(WINDOW *win, int row, int col,
                             int diff_type, const char *text, int text_len,
                             int cols, int hl_start, int hl_end) {
    if (text_len <= 0) return 1;

    /* Parse line number and content parts */
    const char *lnum_start, *marker_pos, *content_start;
    int lnum_len, content_len;
    parse_diff_parts(text, text_len, &lnum_start, &lnum_len,
                     &marker_pos, &content_start, &content_len);

    int x = col;

    /* Render leading spaces + line number in dim */
    if (lnum_start) {
        /* Leading spaces before line number */
        int leading = (int)(lnum_start - text);
        if (leading > 0) {
            wattron(win, COLOR_PAIR(C_DIM));
            mvwaddnstr(win, row, x, text, leading);
            wattroff(win, COLOR_PAIR(C_DIM));
            x += leading;
        }
        /* Line number in dim */
        wattron(win, COLOR_PAIR(C_DIM));
        mvwaddnstr(win, row, x, lnum_start, lnum_len);
        wattroff(win, COLOR_PAIR(C_DIM));
        x += lnum_len;
        /* Space after line number */
        mvwaddch(win, row, x, ' ');
        x++;
    }

    /* For context lines (diff_type == 2), render content without background */
    if (diff_type == 2) {
        if (content_len > 0) {
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
            wattron(win, COLOR_PAIR(pair));
            mvwaddnstr(win, row, x, content_start, marker_bytes);
            wattroff(win, COLOR_PAIR(pair));
            x += marker_bytes;
        }

        /* Render code content with optional char-level highlighting */
        if (hl_start >= 0 && hl_end > hl_start &&
            hl_start < code_len) {
            /* Clamp highlight range */
            if (hl_end > code_len) hl_end = code_len;

            /* Pre-highlight portion */
            if (hl_start > 0) {
                wattron(win, COLOR_PAIR(pair));
                mvwaddnstr(win, row, x, code, hl_start);
                wattroff(win, COLOR_PAIR(pair));
                x += utf8_display_len(code, hl_start);
            }
            /* Highlighted portion (brighter bg) */
            int hl_len = hl_end - hl_start;
            wattron(win, COLOR_PAIR(pair_hl) | A_BOLD);
            mvwaddnstr(win, row, x, code + hl_start, hl_len);
            wattroff(win, COLOR_PAIR(pair_hl) | A_BOLD);
            x += utf8_display_len(code + hl_start, hl_len);
            /* Post-highlight portion */
            int post_len = code_len - hl_end;
            if (post_len > 0) {
                wattron(win, COLOR_PAIR(pair));
                mvwaddnstr(win, row, x, code + hl_end, post_len);
                wattroff(win, COLOR_PAIR(pair));
                x += utf8_display_len(code + hl_end, post_len);
            }
        } else {
            /* No char highlight — render entire code content */
            if (code_len > 0) {
                wattron(win, COLOR_PAIR(pair));
                mvwaddnstr(win, row, x, code, code_len);
                wattroff(win, COLOR_PAIR(pair));
                x += utf8_display_len(code, code_len);
            }
        }
    }

    /* Pad remaining columns with diff background */
    wattron(win, COLOR_PAIR(pair));
    for (int px = x; px < cols; px++)
        mvwaddch(win, row, px, ' ');
    wattroff(win, COLOR_PAIR(pair));

    return 1;
}

/* ── Parse ── */

md_doc_t *md_parse(const char *source) {
    md_doc_t *doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;
    doc->source = strdup(source ? source : "");

    /* Extract [text](uri) links — skip code fences (``` blocks) */
    const char *p = doc->source;
    int line_num = 0;
    int in_code_fence = 0;
    int at_line_start = 1;  /* track whether p is at the beginning of a line */
    while (*p) {
        if (*p == '\n') { line_num++; p++; at_line_start = 1; continue; }

        /* Detect code fence toggle (``` at start of line) */
        if (at_line_start && *p == '`' && p[1] == '`' && p[2] == '`') {
            in_code_fence = !in_code_fence;
            /* Skip to end of line */
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Skip everything inside code fences */
        if (in_code_fence) { p++; at_line_start = 0; continue; }

        /* Note: 4-space indented code blocks (legacy markdown) are NOT handled
         * here.  Fenced code blocks (```) are properly detected above.  The
         * 4-space convention is unused by LLMs and would break legitimately
         * indented content like session tree entries. */

        at_line_start = 0;

        /* Look for [text](uri) pattern anywhere on the line */
        if (*p == '[') {
            /* Find ] on the SAME line (don't cross line boundaries) */
            const char *text_start = p + 1;
            const char *text_end = NULL;
            for (const char *q = text_start; *q && *q != '\n'; q++) {
                if (*q == ']') { text_end = q; break; }
            }
            if (text_end && text_end[1] == '(') {
                const char *uri_start = text_end + 2;
                /* Find ) on the SAME line */
                const char *uri_end = NULL;
                for (const char *q = uri_start; *q && *q != '\n'; q++) {
                    if (*q == ')') { uri_end = q; break; }
                }
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

/* ── OSC 8 terminal hyperlinks (deferred) ── */

/* Deferred OSC 8 link list — populated during md_render(), flushed
 * after doupdate() by md_osc8_flush().  This avoids the fundamental
 * problem that ncurses' waddch() renders ESC (0x1B) as ^[ caret
 * notation instead of passing it through to the terminal. */
md_osc8_link_t md_osc8_links[MD_OSC8_MAX];
int            md_osc8_count = 0;

/* Record a deferred OSC 8 link to be emitted after doupdate(). */
static void defer_osc8_link(int vis_line, int col, const char *uri) {
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
static void defer_osc8_end(int col_end) {
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
static int is_web_uri(const char *uri) {
    return strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0;
}

/* Check if a URI is linkable via OSC 8: web URLs, file:// URIs, or
 * absolute paths (which get auto-prefixed with file:// at emit time) */
static int is_linkable_uri(const char *uri) {
    if (!uri || !*uri) return 0;
    return is_web_uri(uri)
        || strncmp(uri, "file://", 7) == 0
        || uri[0] == '/';
}

/* Legacy wrappers — now just record deferred links instead of
 * writing ESC bytes via waddch (which doesn't work). */
static void emit_osc8_link_start(WINDOW *win, const char *uri) {
    int y, x;
    getyx(win, y, x);
    defer_osc8_link(y, x, uri);
}

static void emit_osc8_end(WINDOW *win) {
    int y, x;
    getyx(win, y, x);
    defer_osc8_end(x);
    (void)y;
}

/* ── Render helpers ── */

/* Count the number of display columns a UTF-8 string occupies.
 * For ASCII, 1 byte = 1 column. For multi-byte UTF-8, we count
 * only the leading bytes (skip continuation bytes 10xxxxxx). */
static int utf8_display_len(const char *s, int max_bytes) {
    int cols = 0, i = 0;
    while (i < max_bytes && s[i]) {
        int w = utf8_char_width(s + i, max_bytes - i);
        wchar_t cp;
        int clen = utf8_decode(s + i, max_bytes - i, &cp);
        cols += w;
        i += clen;
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
        while (byte_pos < len) {
            int w = utf8_char_width(text + byte_pos, len - byte_pos);
            if (cols + w > max_cols) break;
            wchar_t cp;
            int clen = utf8_decode(text + byte_pos, len - byte_pos, &cp);
            cols += w;
            byte_pos += clen;
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
 *   doc: parsed MD document (for link tracking; may be NULL)
 *   link_idx: in/out — index into doc->links[] (may be NULL)
 *   cursor_link: selected link index for cursor highlight (-1 = none)
 *   focus: 1 = pane has focus (cursor visible), 0 = no focus
 *
 * Returns: number of render lines consumed by the table. */
static int render_table(WINDOW *win, const char *src, int num_rows,
                        int scroll_y, int scroll_x, int rows, int cols,
                        int *render_line, int *src_line, const char **out_src,
                        md_doc_t *doc, int *link_idx, int cursor_link,
                        int focus) {
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
                /* Compute display width excluding inline markdown markers
                 * so column widths reflect rendered text, not raw markup */
                {
                    inline_seg_t tw_segs[MAX_INLINE_SEGS];
                    int tw_n = parse_inline(ts, w, tw_segs, MAX_INLINE_SEGS);
                    int dw = 0;
                    for (int si = 0; si < tw_n; si++)
                        dw += seg_display_cols(tw_segs[si].text, tw_segs[si].len);
                    w = dw;
                }
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
                    if (x >= 0 && x < cols) mvwaddstr(win, vis_line, x, "\xe2\x94\xbc"); /* ┼ */
                    x++;
                    int w = col_widths[ci] + 2;
                    for (int k = 0; k < w; k++) {
                        if (x >= 0 && x < cols) mvwaddstr(win, vis_line, x, "\xe2\x94\x80"); /* ─ */
                        x++;
                    }
                }
                if (x >= 0 && x < cols) mvwaddstr(win, vis_line, x, "\xe2\x94\xbc"); /* ┼ */
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
                        mvwaddstr(win, vis_line, x, "\xe2\x94\x82"); /* │ */
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
                    /* Parse inline markdown in cell text (bold, italic, code, links) */
                    int dcols = 0;
                    if (tlen > 0) {
                        inline_seg_t cell_segs[MAX_INLINE_SEGS];
                        int cell_n = parse_inline(ts, tlen, cell_segs, MAX_INLINE_SEGS);
                        if (is_header)
                            apply_attr_to_segs(cell_segs, cell_n, A_BOLD);
                        /* Track ALL links in this cell for cursor navigation.
                         * A cell may contain multiple links (e.g., "[site](...) / [HN](...)"),
                         * so we must set render_line and advance link_idx for each one. */
                        if (doc && link_idx) {
                            for (int si = 0; si < cell_n; si++) {
                                if (cell_segs[si].url && cell_segs[si].url_len > 0) {
                                    if (*link_idx < doc->link_count) {
                                        doc->links[*link_idx].render_line = *render_line;
                                        if (focus && *link_idx == cursor_link) {
                                            /* Highlight only this specific link segment */
                                            cell_segs[si].attr = A_REVERSE | A_BOLD;
                                        }
                                        (*link_idx)++;
                                    }
                                }
                            }
                        }
                        for (int si = 0; si < cell_n; si++)
                            dcols += seg_display_cols(cell_segs[si].text, cell_segs[si].len);
                        if (x >= 0 && x < cols)
                            render_segs_on_line(win, vis_line, x, cell_segs, cell_n, cols - x);
                        x += dcols;
                    }
                    int tx = x + (pw - dcols) + 1;
                    while (x < tx) {
                        if (x >= 0 && x < cols)
                            mvwaddch(win, vis_line, x, ' ');
                        x++;
                    }
                    if (*cp == '|') { ci++; cp++; }
                }
                if (x >= 0 && x < cols) {
                    wattron(win, COLOR_PAIR(C_DIM));
                    mvwaddstr(win, vis_line, x, "\xe2\x94\x82"); /* │ */
                    wattroff(win, COLOR_PAIR(C_DIM));
                }
            }
        } else {
            /* Off-screen: still need to track links so link_idx stays
             * synchronized with doc->links[].  Scan the row text for
             * [text](uri) patterns and advance link_idx for each. */
            if (doc && link_idx) {
                const char *lp = tbuf;
                while ((lp = strchr(lp, '[')) != NULL) {
                    const char *be = strchr(lp + 1, ']');
                    if (be && be[1] == '(') {
                        const char *pe = strchr(be + 2, ')');
                        if (pe) {
                            if (*link_idx < doc->link_count) {
                                doc->links[*link_idx].render_line = *render_line;
                                (*link_idx)++;
                            }
                            lp = pe + 1;
                            continue;
                        }
                    }
                    lp++;
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

    /* Reset deferred OSC 8 link list for this render cycle */
    md_osc8_count = 0;

    int rows = getmaxy(win);
    int cols = getmaxx(win);

    werase(win);

    const char *src = doc->source;
    int render_line = 0;  /* line number in the full document */
    int src_line = 0;     /* source line number (for link matching) */
    int link_idx = 0;     /* current link index */
    int in_code_block = 0;
    char *code_lang = NULL;  /* language tag from ```lang fence */

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
            if (!in_code_block) {
                /* Opening fence — parse language tag (e.g., ```diff, ```c) */
                const char *tag = line_buf + 3;
                /* Skip leading spaces after ``` */
                while (*tag == ' ' || *tag == '\t') tag++;
                int tag_len = (int)strlen(tag);
                /* Trim trailing backticks if present (e.g., ```diff``` ) */
                while (tag_len > 0 && tag[tag_len - 1] == '`') tag_len--;
                if (tag_len > 0) {
                    code_lang = strndup(tag, tag_len);
                } else {
                    code_lang = NULL;
                }
            } else {
                /* Closing fence — reset language tag */
                free(code_lang);
                code_lang = NULL;
            }
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

        /* Track links regardless of visibility.
         * Catch-up: if link_idx fell behind (e.g., a link was on a heading,
         * blockquote, or table line whose branch didn't advance link_idx),
         * skip past stale entries so subsequent links still render. */
        while (link_idx < doc->link_count &&
               doc->links[link_idx].doc_line < src_line)
            link_idx++;
        int has_link = (link_idx < doc->link_count &&
                        src_line == doc->links[link_idx].doc_line);
        /* A "link line" starts with '[' — the entire line is the link.
         * Lines with embedded links (prefix + [tool](uri) + suffix)
         * are handled by the has_link && !is_link_line branch below. */
        int is_link_line = (line_buf[0] == '[' && has_link);

        if (in_code_block) {
            /* Diff rendering only when ```diff fence was used */
            int diff_type = 0;
            if (code_lang && strcmp(code_lang, "diff") == 0) {
                diff_type = is_diff_line(line_buf, line_len);
            }

            if (diff_type != 0 && visible) {
                /* Compute char-level highlight by pairing -/+ lines
                 * within a hunk.  For a run of N '-' lines followed by
                 * M '+' lines, pair them positionally (1st '-' with 1st '+',
                 * 2nd with 2nd, etc.).  Unpaired lines get no highlight. */
                int hl_start = -1, hl_end = -1;
                if (diff_type == -1 && eol) {
                    /* Find this line's index within the '-' run */
                    int del_idx = 0;
                    {
                        const char *bp = src;
                        while (bp > doc->source) {
                            const char *pe = bp - 1;
                            const char *ps = pe;
                            while (ps > doc->source && ps[-1] != '\n') ps--;
                            int pl = (int)(pe - ps);
                            if (pl <= 0) break;
                            char tb[LINE_BUF_SIZE];
                            copy_to_buf(tb, sizeof(tb), ps, pl);
                            if (is_diff_line(tb, pl) != -1) break;
                            del_idx++;
                            bp = ps;
                        }
                    }
                    /* Scan forward past remaining '-' lines after this one */
                    const char *scan = eol + 1;
                    while (*scan) {
                        const char *ne = strchr(scan, '\n');
                        int nl = ne ? (int)(ne - scan) : (int)strlen(scan);
                        if (nl <= 0) break;
                        char tb[LINE_BUF_SIZE];
                        copy_to_buf(tb, sizeof(tb), scan, nl);
                        if (is_diff_line(tb, nl) != -1) break;
                        scan = ne ? ne + 1 : scan + nl;
                    }
                    /* scan now points to the first '+' line (or end).
                     * Skip del_idx '+' lines to find our pair. */
                    int plus_skip = del_idx;
                    while (*scan && plus_skip > 0) {
                        const char *ne = strchr(scan, '\n');
                        int nl = ne ? (int)(ne - scan) : (int)strlen(scan);
                        if (nl <= 0) break;
                        char tb[LINE_BUF_SIZE];
                        copy_to_buf(tb, sizeof(tb), scan, nl);
                        if (is_diff_line(tb, nl) != 1) break;
                        plus_skip--;
                        scan = ne ? ne + 1 : scan + nl;
                    }
                    if (plus_skip == 0 && *scan) {
                        const char *ne = strchr(scan, '\n');
                        int nl = ne ? (int)(ne - scan) : (int)strlen(scan);
                        if (nl > 0) {
                            char pair_buf[LINE_BUF_SIZE];
                            copy_to_buf(pair_buf, sizeof(pair_buf), scan, nl);
                            if (is_diff_line(pair_buf, nl) == 1) {
                                int a_len, b_len;
                                const char *a = diff_content_after_marker(
                                    line_buf, line_len, &a_len);
                                const char *b = diff_content_after_marker(
                                    pair_buf, nl, &b_len);
                                int hsa, hea, hsb, heb;
                                compute_char_diff(a, a_len, b, b_len,
                                                  &hsa, &hea, &hsb, &heb);
                                hl_start = hsa;
                                hl_end = hea;
                            }
                        }
                    }
                } else if (diff_type == 1) {
                    /* Find this line's index within the '+' run */
                    int add_idx = 0;
                    const char *first_plus = src;
                    {
                        const char *bp = src;
                        while (bp > doc->source) {
                            const char *pe = bp - 1;
                            const char *ps = pe;
                            while (ps > doc->source && ps[-1] != '\n') ps--;
                            int pl = (int)(pe - ps);
                            if (pl <= 0) break;
                            char tb[LINE_BUF_SIZE];
                            copy_to_buf(tb, sizeof(tb), ps, pl);
                            if (is_diff_line(tb, pl) != 1) break;
                            add_idx++;
                            first_plus = ps;
                            bp = ps;
                        }
                    }
                    /* Walk back from first_plus through '-' lines */
                    int del_count = 0;
                    const char *first_del = first_plus;
                    {
                        const char *bp = first_plus;
                        while (bp > doc->source) {
                            const char *pe = bp - 1;
                            const char *ps = pe;
                            while (ps > doc->source && ps[-1] != '\n') ps--;
                            int pl = (int)(pe - ps);
                            if (pl <= 0) break;
                            char tb[LINE_BUF_SIZE];
                            copy_to_buf(tb, sizeof(tb), ps, pl);
                            if (is_diff_line(tb, pl) != -1) break;
                            del_count++;
                            first_del = ps;
                            bp = ps;
                        }
                    }
                    /* Pair with add_idx-th '-' line if it exists */
                    if (add_idx < del_count) {
                        const char *tp = first_del;
                        for (int i = 0; i < add_idx; i++) {
                            const char *ne = strchr(tp, '\n');
                            if (!ne) { tp = NULL; break; }
                            tp = ne + 1;
                        }
                        if (tp) {
                            const char *ne = strchr(tp, '\n');
                            int nl = ne ? (int)(ne - tp) : (int)strlen(tp);
                            if (nl > 0) {
                                char pair_buf[LINE_BUF_SIZE];
                                copy_to_buf(pair_buf, sizeof(pair_buf), tp, nl);
                                if (is_diff_line(pair_buf, nl) == -1) {
                                    int a_len, b_len;
                                    const char *a = diff_content_after_marker(
                                        pair_buf, nl, &a_len);
                                    const char *b = diff_content_after_marker(
                                        line_buf, line_len, &b_len);
                                    int hsa, hea, hsb, heb;
                                    compute_char_diff(a, a_len, b, b_len,
                                                      &hsa, &hea, &hsb, &heb);
                                    hl_start = hsb;
                                    hl_end = heb;
                                }
                            }
                        }
                    }
                }

                /* Diff line: render with line number + colored background */
                int lines_consumed = render_diff_line(
                    win, vis_line, 0, diff_type, line_buf, line_len,
                    cols, hl_start, hl_end);
                if (lines_consumed > 1)
                    advance_render_line(&render_line, lines_consumed);
            } else if (diff_type != 0) {
                /* Diff line but not visible — still counts as 1 line */
            } else {
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
                    first = 0;
                    /* Advance render_line for continuation chunks so
                     * each wrapped segment renders on its own row.
                     * Without this, all chunks render at the same row
                     * and overwrite each other — only the last chunk
                     * was visible, making wrapping appear broken. */
                    if (remaining > 0) render_line++;
                }
            }

        } else if (is_link_line) {
            /* Hyperlink line */
            int is_cursor = (focus && link_idx == cursor_link);
            md_link_t *lk = &doc->links[link_idx];
            lk->render_line = render_line;
            int has_osc8 = visible && is_linkable_uri(lk->uri);
            link_idx++;

            if (visible) {
                if (is_cursor) {
                    wattron(win, A_REVERSE | A_BOLD);
                } else {
                    wattron(win, COLOR_PAIR(C_FOCUS));
                }

                /* Emit OSC 8 hyperlink start */
                if (has_osc8) {
                    emit_osc8_link_start(win, lk->uri);
                }

                /* Render the link text with inline formatting */
                inline_seg_t segs[MAX_INLINE_SEGS];
                int n = parse_inline(lk->text, (int)strlen(lk->text), segs, MAX_INLINE_SEGS);
                render_segs_on_line(win, vis_line, 0, segs, n, cols);

                /* Emit OSC 8 hyperlink end */
                if (has_osc8) {
                    emit_osc8_end(win);
                }

                if (is_cursor) {
                    wattroff(win, A_REVERSE | A_BOLD);
                } else {
                    wattroff(win, COLOR_PAIR(C_FOCUS));
                }
            }

        } else if (line_buf[0] == '#') {
            /* Heading — with inline formatting and optional link support.
             * Search results produce headings like ### [session](path)
             * which must render the link text as a navigable hyperlink. */
            int level = 0;
            while (line_buf[level] == '#') level++;
            const char *htext = line_buf + level;
            while (*htext == ' ') htext++;
            int hlen = (int)strlen(htext);
            int pair = (level == 1) ? C_SUCCESS : C_FOCUS;
            int lines_consumed = 1;

            /* Track link in heading (e.g., ### [text](uri)) */
            md_link_t *head_lk = NULL;
            int head_is_cursor = 0;
            if (has_link) {
                head_lk = &doc->links[link_idx];
                head_lk->render_line = render_line;
                head_is_cursor = (focus && link_idx == cursor_link);
                link_idx++;
            }

            if (visible) {
                if (head_lk) {
                    /* Heading with embedded link: render prefix + link + suffix */
                    const char *bracket = strchr(htext, '[');
                    const char *bracket_end = bracket ? strchr(bracket, ']') : NULL;
                    const char *paren_end = NULL;
                    if (bracket_end && bracket_end[1] == '(')
                        paren_end = strchr(bracket_end + 2, ')');

                    if (bracket && bracket_end && paren_end) {
                        int x = 0;
                        /* 1. Prefix before [ (heading styled) */
                        int prefix_len = (int)(bracket - htext);
                        if (prefix_len > 0) {
                            inline_seg_t segs[MAX_INLINE_SEGS];
                            int n = parse_inline(htext, prefix_len, segs, MAX_INLINE_SEGS);
                            apply_attr_to_segs(segs, n, COLOR_PAIR(pair) | A_BOLD);
                            x += render_segs_on_line(win, vis_line, x, segs, n, cols - x);
                        }

                        /* 2. Link text with link color (or cursor highlight) */
                        int has_osc8 = is_linkable_uri(head_lk->uri);
                        if (head_is_cursor)
                            wattron(win, A_REVERSE | A_BOLD);
                        else
                            wattron(win, COLOR_PAIR(C_FOCUS) | A_BOLD);
                        if (has_osc8)
                            emit_osc8_link_start(win, head_lk->uri);

                        int link_text_len = (int)(bracket_end - bracket - 1);
                        if (link_text_len > 0 && x < cols)
                            x += render_segment(win, vis_line, x,
                                                bracket + 1, link_text_len, cols - x);

                        if (has_osc8)
                            emit_osc8_end(win);
                        if (head_is_cursor)
                            wattroff(win, A_REVERSE | A_BOLD);
                        else
                            wattroff(win, COLOR_PAIR(C_FOCUS) | A_BOLD);

                        /* 3. Suffix after ) (heading styled) */
                        const char *suffix = paren_end + 1;
                        int suffix_len = (int)strlen(suffix);
                        if (suffix_len > 0 && x < cols) {
                            inline_seg_t segs[MAX_INLINE_SEGS];
                            int n = parse_inline(suffix, suffix_len, segs, MAX_INLINE_SEGS);
                            apply_attr_to_segs(segs, n, COLOR_PAIR(pair) | A_BOLD);
                            render_segs_on_line(win, vis_line, x, segs, n, cols - x);
                        }
                    } else {
                        /* Fallback: couldn't parse link boundaries */
                        inline_seg_t segs[MAX_INLINE_SEGS];
                        int n = parse_inline(htext, hlen, segs, MAX_INLINE_SEGS);
                        apply_attr_to_segs(segs, n, COLOR_PAIR(pair) | A_BOLD);
                        render_segs_on_line(win, vis_line, 0, segs, n, cols);
                    }
                } else {
                    /* No link — render with inline formatting + word wrapping */
                    inline_seg_t segs[MAX_INLINE_SEGS];
                    int n = parse_inline(htext, hlen, segs, MAX_INLINE_SEGS);
                    apply_attr_to_segs(segs, n, COLOR_PAIR(pair) | A_BOLD);
                    int total_dcols = 0;
                    for (int k = 0; k < n; k++)
                        total_dcols += seg_display_cols(segs[k].text, segs[k].len);
                    if (total_dcols <= cols) {
                        render_segs_on_line(win, vis_line, 0, segs, n, cols);
                    } else {
                        lines_consumed = render_segs_wrapped(
                            win, vis_line, 0, segs, n, cols);
                    }
                }
            } else {
                /* Off-screen: count wrapped lines for accurate render_line tracking */
                lines_consumed = count_wrapped_lines(htext, hlen, cols);
            }
            advance_render_line(&render_line, lines_consumed);

        } else if (strncmp(line_buf, "---", 3) == 0) {
            /* Horizontal rule — use Unicode ─ (U+2500) instead of ACS_HLINE
             * to avoid garbled output when ACS charset mapping is broken
             * (common in UTF-8 terminals where ncurses ACS falls back to
             * VT100 line-drawing characters that display as garbage). */
            if (visible) {
                wattron(win, COLOR_PAIR(C_DIM));
                wmove(win, vis_line, 0);
                for (int hx = 0; hx < cols; hx++)
                    waddstr(win, "\xe2\x94\x80"); /* ─ U+2500 */
                wattroff(win, COLOR_PAIR(C_DIM));
            }

        } else if (line_buf[0] == '~' && line_buf[1] == '>'
                   && line_buf[2] == ' ') {
            /* Thought paragraph — renders text in normal color (white)
             * with word-wrapping, no decoration bar. */
            const char *gp_text = line_buf + 3;
            int gp_len = (int)strlen(gp_text);
            int lines_consumed = 1;
            if (gp_len > 0) {
                if (visible) {
                    inline_seg_t segs[MAX_INLINE_SEGS];
                    int n = parse_inline(gp_text, gp_len, segs, MAX_INLINE_SEGS);
                    apply_attr_to_segs(segs, n, COLOR_PAIR(C_NORMAL));
                    int total_dcols = 0;
                    for (int k = 0; k < n; k++)
                        total_dcols += seg_display_cols(segs[k].text, segs[k].len);
                    if (total_dcols <= cols) {
                        render_segs_on_line(win, vis_line, 0, segs, n, cols);
                    } else {
                        lines_consumed = render_segs_wrapped(
                            win, vis_line, 0, segs, n, cols);
                    }
                } else {
                    lines_consumed = count_wrapped_lines(gp_text, gp_len, cols);
                }
            }
            advance_render_line(&render_line, lines_consumed);

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
                    mvwaddstr(win, vis_line, 0, "\xe2\x94\x82"); /* │ */
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
                    mvwaddstr(win, cvl, 0, "\xe2\x94\x82"); /* │ */
                    wattroff(win, COLOR_PAIR(C_DIM));
                }
            }

            /* Item 9: explicit line advancement */
            advance_render_line(&render_line, lines_consumed);

        } else if (has_link && !is_link_line) {
            /* React step line: line with an embedded [tool](uri) link
             * but not starting with '[' (those are standalone link lines).
             * Covers both old format "  ✓ N HH:MM [tool](uri) ..."
             * and new format "N RXSY HH:MM:SS [tool](uri) ..." */
            int pair = C_SUCCESS;
            int lines_consumed = 1;

            /* Check for inline link on this step line */
            int step_has_link = has_link;
            md_link_t *step_lk = step_has_link ? &doc->links[link_idx] : NULL;
            int is_cursor = (focus && step_has_link && link_idx == cursor_link);

            if (step_has_link) {
                /* Track link render position regardless of visibility */
                step_lk->render_line = render_line;
                link_idx++;
            }

            /* Track additional links on this same source line so they
             * are navigable via cursor (multi-link lines like
             * "... ([#25047](...)), ... ([#25420](...))"). */
            int first_extra_link = link_idx;
            while (link_idx < doc->link_count &&
                   doc->links[link_idx].doc_line == src_line) {
                doc->links[link_idx].render_line = render_line;
                link_idx++;
            }
            int n_extra = link_idx - first_extra_link;

            /* Parse link boundaries outside visible check — needed for
             * off-screen line counting too */
            const char *bracket = strchr(line_buf, '[');
            const char *bracket_end = bracket ? strchr(bracket, ']') : NULL;
            const char *paren_end = NULL;
            if (bracket_end && bracket_end[1] == '(')
                paren_end = strchr(bracket_end + 2, ')');

            if (visible) {
                if (step_has_link && bracket && bracket_end && paren_end) {
                    int x = 0;
                    /* 1. Render prefix (before [) with step color + inline formatting */
                    int prefix_len = (int)(bracket - line_buf);
                    if (prefix_len >= cols) {
                        /* Prefix is wider than the terminal — the [text](uri)
                         * is content text (e.g. literal "[tool](uri)" inside
                         * a done result), not a navigable link.  Render the
                         * entire line as wrapped inline text instead of the
                         * prefix+link+suffix decomposition which would
                         * truncate the prefix. */
                        lines_consumed = render_inline_wrapped(
                            win, vis_line, 0, line_buf,
                            (int)strlen(line_buf), cols);
                        goto step_line_done;
                    }
                    if (prefix_len > 0) {
                        inline_seg_t segs[MAX_INLINE_SEGS];
                        int n = parse_inline(line_buf, prefix_len, segs, MAX_INLINE_SEGS);
                        apply_attr_to_segs(segs, n, COLOR_PAIR(pair));
                        x += render_segs_on_line(win, vis_line, x, segs, n, cols - x);
                    }

                    /* 2. Render link text with link color + OSC 8 */
                    int step_has_osc8 = is_linkable_uri(step_lk->uri);
                    if (is_cursor)
                        wattron(win, A_REVERSE | A_BOLD);
                    else
                        wattron(win, COLOR_PAIR(C_FOCUS));

                    if (step_has_osc8)
                        emit_osc8_link_start(win, step_lk->uri);

                    int link_text_len = (int)(bracket_end - bracket - 1);
                    if (link_text_len > 0 && x < cols) {
                        x += render_segment(win, vis_line, x,
                                            bracket + 1, link_text_len, cols - x);
                    }

                    if (step_has_osc8)
                        emit_osc8_end(win);

                    if (is_cursor)
                        wattroff(win, A_REVERSE | A_BOLD);
                    else
                        wattroff(win, COLOR_PAIR(C_FOCUS));

                    /* 3. Render suffix (after )) with step color + wrapping.
                     * First line starts at column x; continuation lines
                     * wrap at full terminal width (col 0). */
                    const char *suffix = paren_end + 1;
                    int suffix_len = (int)strlen(suffix);
                    if (suffix_len > 0 && x < cols) {
                        int remaining = cols - x;
                        inline_seg_t segs[MAX_INLINE_SEGS];
                        int n = parse_inline(suffix, suffix_len, segs, MAX_INLINE_SEGS);
                        apply_attr_to_segs(segs, n, COLOR_PAIR(pair));
                        /* Apply cursor highlighting to extra links in suffix */
                        if (n_extra > 0 && focus) {
                            int eli = first_extra_link;
                            for (int si = 0; si < n && eli < first_extra_link + n_extra; si++) {
                                if (segs[si].url && segs[si].url_len > 0) {
                                    if (eli == cursor_link)
                                        segs[si].attr = A_REVERSE | A_BOLD;
                                    eli++;
                                }
                            }
                        }
                        int total_dcols = 0;
                        for (int k = 0; k < n; k++)
                            total_dcols += seg_display_cols(segs[k].text, segs[k].len);
                        if (total_dcols <= remaining) {
                            render_segs_on_line(win, vis_line, x, segs, n, remaining);
                        } else {
                            lines_consumed = render_segs_wrapped_ex(
                                win, vis_line, x, segs, n, remaining,
                                0, cols);
                        }
                    }
                } else if (step_has_link) {
                    /* Fallback: couldn't parse link boundaries, render whole line */
                    inline_seg_t segs[MAX_INLINE_SEGS];
                    int n = parse_inline(line_buf, (int)strlen(line_buf), segs, MAX_INLINE_SEGS);
                    apply_attr_to_segs(segs, n, COLOR_PAIR(pair));
                    render_segs_on_line(win, vis_line, 0, segs, n, cols);
                } else {
                    /* No link — render with inline formatting as before */
                    inline_seg_t segs[MAX_INLINE_SEGS];
                    int n = parse_inline(line_buf, (int)strlen(line_buf), segs, MAX_INLINE_SEGS);
                    apply_attr_to_segs(segs, n, COLOR_PAIR(pair));
                    render_segs_on_line(win, vis_line, 0, segs, n, cols);
                }
            } else {
                /* Off-screen: count wrapped lines for accurate render_line tracking */
                if (bracket && bracket_end && paren_end) {
                    int prefix_len = (int)(bracket - line_buf);
                    if (prefix_len >= cols) {
                        /* Long prefix — whole line is wrapped text */
                        lines_consumed = count_wrapped_lines(
                            line_buf, (int)strlen(line_buf), cols);
                    } else {
                        int link_text_len = (int)(bracket_end - bracket - 1);
                        int x = prefix_len + link_text_len;
                        const char *suffix = paren_end + 1;
                        int suffix_len = (int)strlen(suffix);
                        if (suffix_len > 0 && x < cols) {
                            int remaining = cols - x;
                            /* First line uses remaining width, continuations
                             * use full terminal width (matches render path). */
                            inline_seg_t csegs[MAX_INLINE_SEGS];
                            int cn = parse_inline(suffix, suffix_len, csegs, MAX_INLINE_SEGS);
                            int total_dcols = 0;
                            for (int k = 0; k < cn; k++)
                                total_dcols += seg_display_cols(csegs[k].text, csegs[k].len);
                            if (total_dcols <= remaining) {
                                lines_consumed = 1;
                            } else {
                                int after_first = total_dcols - remaining;
                                lines_consumed = 1 + (after_first + cols - 1) / cols;
                            }
                        }
                    }
                }
            }

step_line_done:
            advance_render_line(&render_line, lines_consumed);

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
                                &render_line, &src_line, &out_src,
                                doc, &link_idx, cursor_link, focus);
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
            /* Regular text — with inline formatting and word-wrapping.
             * Defense-in-depth: if the line contains [text](uri) syntax
             * (e.g., a link on a line type we didn't expect), render it
             * as a proper link instead of showing raw markdown syntax. */
            int lines_consumed = 1;
            if (line_buf[0] != '\0') {
                /* Check for [text](uri) pattern in the line */
                const char *bracket = strchr(line_buf, '[');
                const char *bracket_end = bracket ? strchr(bracket, ']') : NULL;
                const char *paren_end = NULL;
                if (bracket_end && bracket_end[1] == '(')
                    paren_end = strchr(bracket_end + 2, ')');

                if (bracket && bracket_end && paren_end && visible) {
                    /* Render as embedded link: prefix + link + suffix.
                     * The suffix (typically a `command` code span) may be long,
                     * so use word-wrapping for it — continuation lines indent
                     * to the column where the suffix started. */
                    int x = 0;
                    int prefix_len = (int)(bracket - line_buf);
                    if (prefix_len >= cols) {
                        /* Prefix wider than terminal — [text](uri) is literal
                         * content, not a real link.  Render as wrapped text. */
                        lines_consumed = render_inline_wrapped(
                            win, vis_line, 0, line_buf,
                            (int)strlen(line_buf), cols);
                    } else {
                    if (prefix_len > 0) {
                        inline_seg_t segs[MAX_INLINE_SEGS];
                        int n = parse_inline(line_buf, prefix_len, segs, MAX_INLINE_SEGS);
                        x += render_segs_on_line(win, vis_line, x, segs, n, cols - x);
                    }
                    wattron(win, COLOR_PAIR(C_FOCUS));
                    /* Extract URI for OSC 8 clickable link */
                    int uri_len = (int)(paren_end - bracket_end - 2);
                    char uri_buf[512];
                    int do_osc8 = 0;
                    if (uri_len > 0 && uri_len < (int)sizeof(uri_buf)) {
                        memcpy(uri_buf, bracket_end + 2, uri_len);
                        uri_buf[uri_len] = '\0';
                        do_osc8 = is_linkable_uri(uri_buf);
                    }
                    if (do_osc8)
                        emit_osc8_link_start(win, uri_buf);
                    int link_text_len = (int)(bracket_end - bracket - 1);
                    if (link_text_len > 0 && x < cols)
                        x += render_segment(win, vis_line, x,
                                            bracket + 1, link_text_len, cols - x);
                    if (do_osc8)
                        emit_osc8_end(win);
                    wattroff(win, COLOR_PAIR(C_FOCUS));
                    const char *suffix = paren_end + 1;
                    int suffix_len = (int)strlen(suffix);
                    if (suffix_len > 0 && x < cols) {
                        int remaining = cols - x;
                        lines_consumed = render_inline_wrapped(
                            win, vis_line, x, suffix, suffix_len, remaining);
                    }
                    }
                } else if (bracket && bracket_end && paren_end) {
                    /* Off-screen link line: count wrapped lines for suffix */
                    int prefix_len = (int)(bracket - line_buf);
                    if (prefix_len >= cols) {
                        lines_consumed = count_wrapped_lines(
                            line_buf, (int)strlen(line_buf), cols);
                    } else {
                    int link_text_len = (int)(bracket_end - bracket - 1);
                    int x = prefix_len + link_text_len;
                    const char *suffix = paren_end + 1;
                    int suffix_len = (int)strlen(suffix);
                    if (suffix_len > 0 && x < cols) {
                        int remaining = cols - x;
                        lines_consumed = count_wrapped_lines(suffix, suffix_len, remaining);
                    }
                    }
                } else if (visible) {
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

/* Convert a heading text to a GitHub-style anchor slug.
 * Rules: lowercase, spaces/tabs → hyphens, strip non-alphanumeric/non-hyphen,
 * collapse consecutive hyphens, trim leading/trailing hyphens.
 * Writes into buf (max buf_size bytes including NUL). */
static void heading_to_slug(const char *heading, char *buf, int buf_size) {
    int j = 0;
    for (int i = 0; heading[i] && j < buf_size - 1; i++) {
        char c = heading[i];
        if (c >= 'A' && c <= 'Z') {
            buf[j++] = c + ('a' - 'A');
        } else if (c >= 'a' && c <= 'z') {
            buf[j++] = c;
        } else if (c >= '0' && c <= '9') {
            buf[j++] = c;
        } else if (c == ' ' || c == '\t') {
            /* Collapse multiple spaces into one hyphen */
            if (j > 0 && buf[j-1] != '-')
                buf[j++] = '-';
        } else if (c == '-') {
            if (j > 0 && buf[j-1] != '-')
                buf[j++] = '-';
        }
        /* Other characters (punctuation, etc.) are stripped */
    }
    /* Trim trailing hyphen */
    while (j > 0 && buf[j-1] == '-') j--;
    buf[j] = '\0';
}

int md_find_anchor(md_doc_t *doc, const char *fragment) {
    if (!doc || !doc->source || !fragment || !*fragment) return -1;

    const char *src = doc->source;
    int render_line = 0;
    int in_code_fence = 0;

    while (*src) {
        const char *eol = strchr(src, '\n');
        int line_len = eol ? (int)(eol - src) : (int)strlen(src);

        /* Code fence toggle */
        if (line_len >= 3 && src[0] == '`' && src[1] == '`' && src[2] == '`') {
            in_code_fence = !in_code_fence;
            /* ``` fence lines are skipped — no render_line increment */
            src = eol ? eol + 1 : src + line_len;
            continue;
        }

        if (!in_code_fence && src[0] == '#') {
            /* Extract heading text (skip leading #'s and spaces) */
            int level = 0;
            while (level < line_len && src[level] == '#') level++;
            int start = level;
            while (start < line_len && src[start] == ' ') start++;

            /* Convert heading to slug */
            char slug[512];
            char heading_text[512];
            int hlen = line_len - start;
            if (hlen > (int)sizeof(heading_text) - 1)
                hlen = (int)sizeof(heading_text) - 1;
            memcpy(heading_text, src + start, hlen);
            heading_text[hlen] = '\0';

            heading_to_slug(heading_text, slug, sizeof(slug));

            if (strcmp(slug, fragment) == 0) {
                return render_line;
            }
        }

        render_line++;
        src = eol ? eol + 1 : src + line_len;
    }

    return -1;
}

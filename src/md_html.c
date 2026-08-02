/* md_html.c -- Markdown-to-HTML conversion and table handling. Extracted from telegram.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md_html.h"
#include "str.h"
#include "nash_limits.h"

int md_has_table(const char *md) {
    if (!md) return 0;
    const char *p = md;
    while (*p) {
        /* Check at start of string or after newline */
        if (p == md || *(p - 1) == '\n') {
            /* Skip leading whitespace */
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '|') {
                /* Found a | at line start — look for a second | on same line
                 * to confirm it's a table row, not just a shell pipe */
                const char *r = q + 1;
                while (*r && *r != '\n') {
                    if (*r == '\\' && *(r + 1) == '|') { r += 2; continue; } /* skip \| */
                    if (*r == '|') return 1;  /* at least two |'s → table */
                    r++;
                }
            }
        }
        /* Advance to next char */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

/* ── Table-to-bullets converter ──────────────────────────── */

/* Check if a line is a table separator row (e.g. |---|---|---| or | --- | --- |)
 * p points to the first '|' on the line. */
static int is_separator_row(const char *p) {
    if (*p != '|') return 0;
    p++;
    int has_dash = 0;
    while (*p && *p != '\n') {
        if (*p == '-' || *p == ':') has_dash = 1;
        else if (*p == '|' || *p == ' ' || *p == '\t') { /* ok */ }
        else return 0;  /* non-separator character */
        p++;
    }
    return has_dash;
}

/* Find next unescaped '|' starting at p. Returns NULL if not found. */
static char *find_unescaped_pipe(char *p) {
    for (; *p; p++) {
        if (*p == '\\' && *(p + 1) == '|') { p++; continue; } /* skip \| */
        if (*p == '|') return p;
    }
    return NULL;
}

/* Unescape \| → | in-place. */
static void unescape_pipes(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && *(r + 1) == '|') { *w++ = '|'; r += 2; }
        else *w++ = *r++;
    }
    *w = '\0';
}

/* Parse pipe-delimited cells from a table row.
 * Returns number of cells parsed. Cells are trimmed and written to cells[].
 * Each cell points into 'buf' (a mutable copy the caller provides).
 * Escaped pipes (\|) are treated as literal pipe characters, not delimiters. */
static int parse_table_cells(const char *line, const char *line_end,
                             char *buf, int buf_size,
                             char **cells, int max_cells) {
    /* Copy line into buf (clamp to buffer size) */
    int len = (int)(line_end - line);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';

    int ncells = 0;
    char *p = buf;

    /* Skip leading whitespace and optional leading '|' */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') p++;

    while (*p && ncells < max_cells) {
        /* Find next unescaped '|' or end */
        char *sep = find_unescaped_pipe(p);
        char *cell_end = sep ? sep : (buf + len);

        /* Trim trailing whitespace */
        char *te = cell_end - 1;
        while (te >= p && (*te == ' ' || *te == '\t')) te--;
        *(te + 1) = '\0';

        /* Trim leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Skip empty trailing cell (from trailing |) */
        if (*p == '\0' && sep && !*(sep + 1)) break;
        if (*p != '\0' || sep) {
            unescape_pipes(p);
            cells[ncells++] = p;
        }

        if (!sep) break;
        p = sep + 1;
    }
    return ncells;
}

/* Convert markdown tables in text to bullet-point lists.
 * Non-table content is passed through unchanged.
 * Returns a new heap-allocated string. Caller frees. */
char *md_tables_to_bullets(const char *md) {
    if (!md) return NULL;

    size_t md_len = strlen(md);
    /* Allocate generously — bullets are typically shorter than tables */
    str_t out = str_new(md_len + 256);

    const char *p = md;
    while (*p) {
        /* Find end of current line */
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);

        /* Check if this line starts a table (skip whitespace, then |) */
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;

        if (*q == '|') {
            /* Might be a table — look for second unescaped | on same line */
            const char *r = q + 1;
            int second_pipe = 0;
            while (r < eol) {
                if (*r == '\\' && r + 1 < eol && *(r + 1) == '|') { r += 2; continue; } /* skip \| */
                if (*r == '|') { second_pipe = 1; break; }
                r++;
            }

            if (second_pipe) {
                /* Parse header row */
                char hdr_buf[NASH_PATH_MAX];
                char *headers[64];
                int nhdr = parse_table_cells(q, eol, hdr_buf, NASH_PATH_MAX, headers, 64);

                if (nhdr > 0) {
                    /* Save header names (they'll be overwritten by parse_table_cells) */
                    char *saved_headers[64];
                    for (int i = 0; i < nhdr; i++)
                        saved_headers[i] = strdup(headers[i]);

                    /* Advance past header line */
                    const char *next = (*eol == '\n') ? eol + 1 : eol;

                    /* Check for separator row */
                    const char *sep_start = next;
                    while (*sep_start == ' ' || *sep_start == '\t') sep_start++;
                    const char *sep_eol = strchr(next, '\n');
                    if (!sep_eol) sep_eol = next + strlen(next);

                    if (*sep_start == '|' && is_separator_row(sep_start)) {
                        /* Skip separator row */
                        next = (*sep_eol == '\n') ? sep_eol + 1 : sep_eol;
                    }

                    /* Process data rows */
                    while (*next) {
                        const char *row_eol = strchr(next, '\n');
                        if (!row_eol) row_eol = next + strlen(next);

                        const char *rs = next;
                        while (*rs == ' ' || *rs == '\t') rs++;

                        /* Check if still a table row */
                        if (*rs != '|') break;
                        int has_second = 0;
                        for (const char *c = rs + 1; c < row_eol; c++) {
                            if (*c == '\\' && c + 1 < row_eol && *(c + 1) == '|') { c++; continue; } /* skip \| */
                            if (*c == '|') { has_second = 1; break; }
                        }
                        if (!has_second) break;

                        /* Parse data cells */
                        char row_buf[NASH_PATH_MAX];
                        char *cells[64];
                        int ncells = parse_table_cells(rs, row_eol,
                                                      row_buf, NASH_PATH_MAX, cells, 64);

                        /* Emit bullet point */
                        str_append_cstr(&out, "• ");
                        int limit = ncells < nhdr ? ncells : nhdr;
                        for (int i = 0; i < limit; i++) {
                            if (i > 0)
                                str_append_cstr(&out, " · ");
                            str_append_cstr(&out, "**");
                            str_append_cstr(&out, saved_headers[i]);
                            str_append_cstr(&out, ":** ");
                            str_append_cstr(&out, cells[i]);
                        }
                        str_append(&out, "\n", 1);

                        next = (*row_eol == '\n') ? row_eol + 1 : row_eol;
                    }

                    /* Free saved headers */
                    for (int i = 0; i < nhdr; i++)
                        free(saved_headers[i]);

                    p = next;
                    continue;
                }
            }
        }

        /* Not a table line — pass through */
        str_append(&out, p, (size_t)(eol - p));
        if (*eol == '\n') {
            str_append(&out, "\n", 1);
            p = eol + 1;
        } else {
            p = eol;
        }
    }

    return str_steal(&out);
}

/* ── Markdown → Telegram HTML conversion (fallback for pre-10.1 Bot API) ── */

/* Escape HTML special characters: < > & */
static void html_escape_append(str_t *out, const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (text[i]) {
            case '<': str_append_cstr(out, "&lt;"); break;
            case '>': str_append_cstr(out, "&gt;"); break;
            case '&': str_append_cstr(out, "&amp;"); break;
            default:  str_append(out, &text[i], 1); break;
        }
    }
}

/*
 * Convert nash's markdown output to Telegram HTML.
 * This is the FALLBACK path used when sendRichMessage (Bot API 10.1+)
 * is not available. The primary path sends markdown directly.
 *
 * Supported conversions:
 *   **bold**       → <b>bold</b>
 *   *italic*       → <i>italic</i>  (word-boundary only)
 *   _italic_       → <i>italic</i>  (word-boundary only, not file_name)
 *   `code`         → <code>code</code>
 *   ```lang\n...\n```  → <pre><code class="language-lang">...</code></pre>
 *   | table |      → <pre>-wrapped monospace ASCII table
 *   ## Header      → <b>Header</b>
 *   - bullet       → • bullet
 *   [text](url)    → <a href="url">text</a>
 *
 * Returns heap-allocated string. Caller frees.
 */
/* Check if a character is a word boundary for italic detection */
static int is_word_boundary(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '.' || c == ',' || c == ':' || c == ';' || c == '!'
        || c == '?' || c == ')' || c == ']' || c == '}' || c == '"'
        || c == '\'';
}

/* Check if line at position i is a table line (starts with |) */
static int is_table_line(const char *md, int i) {
    /* Skip leading whitespace */
    while (md[i] == ' ' || md[i] == '\t') i++;
    return md[i] == '|';
}

/* Check if line is a table separator (|---|---| or | --- | --- |) */
static int is_table_separator(const char *md, int i) {
    if (!is_table_line(md, i)) return 0;
    /* Must contain at least one - and no alphabetic chars */
    int has_dash = 0;
    while (md[i] && md[i] != '\n') {
        if (md[i] == '-' || md[i] == ':') has_dash = 1;
        else if ((md[i] >= 'a' && md[i] <= 'z') || (md[i] >= 'A' && md[i] <= 'Z'))
            return 0;
        i++;
    }
    return has_dash;
}

char *md_to_html(const char *md) {
    if (!md) return strdup("");

    size_t len = strlen(md);
    str_t out = str_new(len + len / 4 + 64);

    int i = 0;

    while (md[i]) {
        /* Fenced code block: ```lang ... ``` */
        if (md[i] == '`' && md[i+1] == '`' && md[i+2] == '`') {
            i += 3;
            /* Extract optional language */
            int lang_start = i;
            while (md[i] && md[i] != '\n') i++;
            int lang_len = i - lang_start;
            if (md[i] == '\n') i++;

            /* Find closing ``` */
            const char *close = strstr(&md[i], "```");
            size_t block_len = close ? (size_t)(close - &md[i]) : strlen(&md[i]);

            if (lang_len > 0) {
                str_append_cstr(&out, "<pre><code class=\"language-");
                str_append(&out, &md[lang_start], (size_t)lang_len);
                str_append_cstr(&out, "\">");
            } else {
                str_append_cstr(&out, "<pre><code>");
            }
            /* Code block content: escape HTML but preserve whitespace */
            html_escape_append(&out, &md[i], block_len);
            str_append_cstr(&out, "</code></pre>");

            i += (int)block_len;
            if (close) i += 3;  /* skip closing ``` */
            if (md[i] == '\n') i++;  /* skip trailing newline */
            continue;
        }

        /* Line-level patterns (only at start of line or start of string) */
        if (i == 0 || md[i-1] == '\n') {

            /* Markdown table → <pre>-wrapped monospace table
             * (Telegram HTML parse_mode doesn't support <table> tags) */
            if (is_table_line(md, i)) {
                /* --- Pass 1: collect all rows & measure column widths --- */
                #define TBL_MAX_COLS 32
                #define TBL_MAX_ROWS 128
                /* Store cell text as {start, len} into md */
                struct { int s; int n; } cells[TBL_MAX_ROWS][TBL_MAX_COLS];
                int ncols_per_row[TBL_MAX_ROWS];
                int is_sep[TBL_MAX_ROWS];
                int nrows = 0;
                int max_cols = 0;
                int col_width[TBL_MAX_COLS];
                memset(col_width, 0, sizeof(col_width));

                int scan = i;
                while (md[scan] && is_table_line(md, scan) && nrows < TBL_MAX_ROWS) {
                    if (is_table_separator(md, scan)) {
                        is_sep[nrows] = 1;
                        ncols_per_row[nrows] = 0;
                        nrows++;
                        while (md[scan] && md[scan] != '\n') scan++;
                        if (md[scan] == '\n') scan++;
                        continue;
                    }
                    is_sep[nrows] = 0;
                    int line_end = scan;
                    while (md[line_end] && md[line_end] != '\n') line_end++;

                    int p = scan;
                    while (p < line_end && (md[p] == ' ' || md[p] == '\t')) p++;
                    if (p < line_end && md[p] == '|') p++;

                    int col = 0;
                    while (p < line_end && col < TBL_MAX_COLS) {
                        int cs = p, ce = p;
                        while (ce < line_end && md[ce] != '|') ce++;
                        /* Trim whitespace */
                        int ts = cs, te = ce;
                        while (ts < te && (md[ts] == ' ' || md[ts] == '\t')) ts++;
                        while (te > ts && (md[te-1] == ' ' || md[te-1] == '\t')) te--;

                        if (ts == te && ce >= line_end) break; /* trailing | */

                        cells[nrows][col].s = ts;
                        cells[nrows][col].n = te - ts;

                        /* Measure display width (strip ** markers) */
                        int dw = 0;
                        int q = ts;
                        while (q < te) {
                            if (md[q] == '*' && q+1 < te && md[q+1] == '*') {
                                q += 2;
                            } else {
                                dw += utf8_char_width(&md[q]);
                                q += utf8_char_len(&md[q]);
                            }
                        }
                        if (dw > col_width[col]) col_width[col] = dw;

                        p = ce;
                        if (p < line_end && md[p] == '|') p++;
                        col++;
                    }
                    ncols_per_row[nrows] = col;
                    if (col > max_cols) max_cols = col;
                    nrows++;
                    scan = line_end;
                    if (md[scan] == '\n') scan++;
                }

                /* --- Pass 2: render as <pre> aligned ASCII table --- */
                str_append_cstr(&out, "<pre>\n");
                for (int r = 0; r < nrows; r++) {
                    if (is_sep[r]) {
                        /* Render separator: +------+------+ */
                        for (int c = 0; c < max_cols; c++) {
                            str_append_cstr(&out, c == 0 ? "+" : "");
                            for (int k = 0; k < col_width[c] + 2; k++)
                                str_append_cstr(&out, "-");
                            str_append_cstr(&out, "+");
                        }
                        str_append_cstr(&out, "\n");
                        continue;
                    }
                    for (int c = 0; c < max_cols; c++) {
                        str_append_cstr(&out, c == 0 ? "| " : " | ");
                        int dw = 0;
                        if (c < ncols_per_row[r]) {
                            /* Emit cell text, stripping ** bold markers */
                            int cs = cells[r][c].s;
                            int ce = cs + cells[r][c].n;
                            int q = cs;
                            while (q < ce) {
                                if (md[q] == '*' && q+1 < ce && md[q+1] == '*') {
                                    q += 2;
                                } else {
                                    int clen = utf8_char_len(&md[q]);
                                    html_escape_append(&out, &md[q], clen);
                                    dw += utf8_char_width(&md[q]);
                                    q += clen;
                                }
                            }
                        }
                        /* Pad to column width */
                        for (int k = dw; k < col_width[c]; k++)
                            str_append_cstr(&out, " ");
                    }
                    str_append_cstr(&out, " |\n");
                }
                str_append_cstr(&out, "</pre>\n");
                #undef TBL_MAX_COLS
                #undef TBL_MAX_ROWS
                i = scan;
                continue;
            }

            /* Headers: ## Text → <b>Text</b> */
            if (md[i] == '#') {
                int hashes = 0;
                while (md[i + hashes] == '#') hashes++;
                if (md[i + hashes] == ' ') {
                    i += hashes + 1;
                    str_append_cstr(&out, "<b>");
                    while (md[i] && md[i] != '\n') {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    str_append_cstr(&out, "</b>");
                    if (md[i] == '\n') {
                        str_append_cstr(&out, "\n");
                        i++;
                    }
                    continue;
                }
            }

            /* Bullet lists: - item → • item */
            if (md[i] == '-' && md[i+1] == ' ') {
                str_append_cstr(&out, "• ");
                i += 2;
                continue;
            }
            /* Also handle * bullets (but not ** which is bold) */
            if (md[i] == '*' && md[i+1] == ' ') {
                str_append_cstr(&out, "• ");
                i += 2;
                continue;
            }
        }

        /* Inline code: `code` */
        if (md[i] == '`' && md[i+1] != '`') {
            i++;
            str_append_cstr(&out, "<code>");
            while (md[i] && md[i] != '`' && md[i] != '\n') {
                html_escape_append(&out, &md[i], 1);
                i++;
            }
            str_append_cstr(&out, "</code>");
            if (md[i] == '`') i++;
            continue;
        }

        /* Bold: **text** */
        if (md[i] == '*' && md[i+1] == '*') {
            i += 2;
            str_append_cstr(&out, "<b>");
            while (md[i] && !(md[i] == '*' && md[i+1] == '*')) {
                html_escape_append(&out, &md[i], 1);
                i++;
            }
            str_append_cstr(&out, "</b>");
            if (md[i] == '*' && md[i+1] == '*') i += 2;
            continue;
        }

        /* Italic: *text* — single asterisk, word-boundary aware */
        if (md[i] == '*' && md[i+1] != '*' && md[i+1] != ' '
            && md[i+1] != '\0'
            && (i == 0 || is_word_boundary(md[i-1]))) {
            /* Scan for closing * at a word boundary */
            int j = i + 1;
            while (md[j] && md[j] != '\n') {
                if (md[j] == '*' && md[j+1] != '*'
                    && is_word_boundary(md[j+1])) {
                    /* Found valid closing * */
                    i++;
                    str_append_cstr(&out, "<i>");
                    while (i < j) {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    str_append_cstr(&out, "</i>");
                    i++;  /* skip closing * */
                    goto next_char_star;
                }
                j++;
            }
            /* No valid closing * found, output literally */
            html_escape_append(&out, &md[i], 1);
            i++;
            continue;
        next_char_star:
            continue;
        }

        /* Italic: _text_ — only at word boundaries to avoid mangling
         * identifiers like file_name or my_var */
        if (md[i] == '_' && md[i+1] != '_' && md[i+1] != ' '
            && md[i+1] != '\0'
            && (i == 0 || is_word_boundary(md[i-1]))) {
            /* Scan for closing _ at a word boundary */
            int j = i + 1;
            while (md[j] && md[j] != '\n') {
                if (md[j] == '_' && is_word_boundary(md[j+1])) {
                    /* Found valid closing _ */
                    i++;
                    str_append_cstr(&out, "<i>");
                    while (i < j) {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    str_append_cstr(&out, "</i>");
                    i++;  /* skip closing _ */
                    goto next_char;
                }
                j++;
            }
            /* No valid closing _ found, output literally */
            html_escape_append(&out, &md[i], 1);
            i++;
            continue;
        next_char:
            continue;
        }

        /* Links: [text](url) */
        if (md[i] == '[') {
            int start = i + 1;
            int j = start;
            while (md[j] && md[j] != ']' && md[j] != '\n') j++;
            if (md[j] == ']' && md[j+1] == '(') {
                int url_start = j + 2;
                int k = url_start;
                while (md[k] && md[k] != ')' && md[k] != '\n') k++;
                if (md[k] == ')') {
                    str_append_cstr(&out, "<a href=\"");
                    str_append(&out, &md[url_start], (size_t)(k - url_start));
                    str_append_cstr(&out, "\">");
                    html_escape_append(&out, &md[start], (size_t)(j - start));
                    str_append_cstr(&out, "</a>");
                    i = k + 1;
                    continue;
                }
            }
        }

        /* Default: escape and append */
        html_escape_append(&out, &md[i], 1);
        i++;
    }

    return str_steal(&out);
}

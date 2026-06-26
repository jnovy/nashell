#include "tui.h"
#include "nash_limits.h"
#include "md_render.h"
#include "str.h"
#include "cJSON.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

/* ── Windows ─────────────────────────────────────────── */

atomic_int g_tui_active = 0;      /* set by tui_init(), cleared by tui_shutdown() */
atomic_int g_tui_was_started = 0; /* set once by tui_init(), never cleared */

static WINDOW *win_main   = NULL;   /* top pane: MD rendered content */
static WINDOW *win_bottom = NULL;   /* bottom pane: status + input */
static int main_height = 0;
static int bottom_height = 0;
static int paste_mode = 0;          /* bracketed paste: 1 while receiving pasted text */

/* ── Clipboard token store ───────────────────────────────
 * Multi-line pastes are stored here and represented as [clipboard1] etc.
 * in the input buffer. On submission, tokens are expanded back. */
#define MAX_CLIPS 64
static struct {
    char *data;     /* malloc'd content (may contain newlines) */
    int   len;      /* length in bytes */
} clip_store[MAX_CLIPS];
static int clip_count = 0;          /* number of stored clips */

/* Temporary paste accumulation buffer */
#define PASTE_BUF_CAP 262144        /* 256 KB max paste */
static char  paste_buf[PASTE_BUF_CAP];
static int   paste_len = 0;

/* ── Colors ──────────────────────────────────────────── */

#define C_NORMAL    0
#define C_SELECTED  1
#define C_FAILED    2
#define C_SUCCESS   3
#define C_STATUS    4
#define C_DIM       5
#define C_FOCUS     6
#define C_STREAM    7

/* ncurses color indices for our registered RGB colors.
 * We use indices 16..34 in the 256-color palette. */
#define NC_STATUS_BG    16  /* #232637 (35,38,55) */
#define NC_INPUT_BG     17  /* #191E2D (25,30,45) */
#define NC_STATUS_READY 18  /* #B4B9C8 (180,185,200) */
#define NC_STATUS_RUN   19  /* #DCC85A (220,200,90) */
#define NC_STATUS_AWAIT 20  /* #64C8DC (100,200,220) */
#define NC_STATUS_DONE  21  /* #64DC96 (100,220,150) */
#define NC_STATUS_ERR   22  /* #DC5050 (220,80,80) */
#define NC_INPUT_FG     23  /* #DCE5F0 (220,225,240) */
#define NC_INPUT_DIM    24  /* #646982 (100,105,130) */
/* Content colors (match nashell true-color palette) */
#define NC_CT_GREEN     25  /* #A6E3A1 (166,227,161) — headings, success */
#define NC_CT_CYAN      26  /* #89DCEB (137,220,235) — code blocks */
#define NC_CT_YELLOW    27  /* #F9E2AF (249,226,175) — focus, blockquotes */
#define NC_CT_BLUE      28  /* #89B4FA (137,180,250) — links */
#define NC_CT_RED       29  /* #F38BA8 (243,139,168) — errors */
#define NC_CT_WHITE     30  /* #DCE1F0 (220,225,240) — primary text */
#define NC_CT_DIM       31  /* #646982 (100,105,130) — dim text */
#define NC_CT_BG        32  /* #1E2030 (30,32,48) — content background */
#define NC_CT_SELECTED  33  /* #3D4160 (61,65,96) — selection bg */
#define NC_CT_NORMAL    34  /* #DCE1F0 (220,225,240) — normal text */
#define NC_CT_DIFF_ADD_BG 35  /* #1A2E1A (26,46,26) — diff add background */
#define NC_CT_DIFF_DEL_BG 36  /* #2E1A1A (46,26,26) — diff del background */
#define NC_CT_DIFF_ADD_HL 40  /* #2A5C2A (42,92,42) — diff add char-highlight bg */
#define NC_CT_DIFF_DEL_HL 41  /* #5C2A2A (92,42,42) — diff del char-highlight bg */
/* Search highlight colors */
#define NC_CT_SEARCH_FG   37  /* #1E2030 (30,32,48)  — dark text on highlight */
#define NC_CT_SEARCH_BG   38  /* #F9E2AF (249,226,175) — yellow/amber background */
#define NC_CT_SEARCH_CUR  39  /* #FAB387 (250,179,135) — orange bg for current */

/* Color pair numbers for status/input rows */
#define CP_STATUS_READY  8   /* status bar: bg=#232637, fg=#B4B9C8 */
#define CP_STATUS_RUNNING 9  /* status bar: bg=#232637, fg=#DCC85A */
#define CP_STATUS_AWAIT  10  /* status bar: bg=#232637, fg=#64C8DC */
#define CP_STATUS_DONE   11  /* status bar: bg=#232637, fg=#64DC96 */
#define CP_STATUS_ERROR  12  /* status bar: bg=#232637, fg=#DC5050 */
#define CP_INPUT_ACTIVE  13  /* input bar: bg=#191E2D, fg=#DCE5F0 */
#define CP_INPUT_DIM     14  /* input bar: bg=#191E2D, fg=#646982 */
/* Diff color pairs: colored background for +/- diff lines */
#define CP_DIFF_ADD      15  /* diff add: bg=#1A2E1A, fg=#A6E3A1 */
#define CP_DIFF_DEL      16  /* diff del: bg=#2E1A1A, fg=#F38BA8 */
#define CP_DIFF_ADD_HL   19  /* diff add char-highlight: bg=#2A5C2A, fg=#A6E3A1 */
#define CP_DIFF_DEL_HL   20  /* diff del char-highlight: bg=#5C2A2A, fg=#F38BA8 */
/* Search highlight color pairs */
#define CP_SEARCH_MATCH  17  /* search match: bg=yellow, fg=dark */
#define CP_SEARCH_CURRENT 18 /* current match: bg=orange, fg=dark */

/* ── True-color registration ─────────────────────────── */

static int true_color_available = 0;

static void init_true_colors(void) {
    if (!can_change_color() || COLORS < 256) {
        true_color_available = 0;
        return;
    }

    /* Register each RGB color into the ncurses palette.
     * init_color uses 0-1000 scale for each component. */
    init_color(NC_STATUS_BG,    35*1000/255,  38*1000/255,  55*1000/255);
    init_color(NC_INPUT_BG,     25*1000/255,  30*1000/255,  45*1000/255);
    init_color(NC_STATUS_READY, 180*1000/255, 185*1000/255, 200*1000/255);
    init_color(NC_STATUS_RUN,   220*1000/255, 200*1000/255,  90*1000/255);
    init_color(NC_STATUS_AWAIT, 100*1000/255, 200*1000/255, 220*1000/255);
    init_color(NC_STATUS_DONE,  100*1000/255, 220*1000/255, 150*1000/255);
    init_color(NC_STATUS_ERR,   220*1000/255,  80*1000/255,  80*1000/255);
    init_color(NC_INPUT_FG,     220*1000/255, 225*1000/255, 240*1000/255);
    init_color(NC_INPUT_DIM,    100*1000/255, 105*1000/255, 130*1000/255);
    /* Content colors — match nashell's true-color palette */
    init_color(NC_CT_GREEN,    166*1000/255, 227*1000/255, 161*1000/255);
    init_color(NC_CT_CYAN,     137*1000/255, 220*1000/255, 235*1000/255);
    init_color(NC_CT_YELLOW,   249*1000/255, 226*1000/255, 175*1000/255);
    init_color(NC_CT_BLUE,     137*1000/255, 180*1000/255, 250*1000/255);
    init_color(NC_CT_RED,      243*1000/255, 139*1000/255, 168*1000/255);
    init_color(NC_CT_WHITE,    220*1000/255, 225*1000/255, 240*1000/255);
    init_color(NC_CT_DIM,      100*1000/255, 105*1000/255, 130*1000/255);
    init_color(NC_CT_BG,        30*1000/255,  32*1000/255,  48*1000/255);
    init_color(NC_CT_SELECTED,  61*1000/255,  65*1000/255,  96*1000/255);
    init_color(NC_CT_NORMAL,   220*1000/255, 225*1000/255, 240*1000/255);
    /* Diff backgrounds — subtle tinted backgrounds for +/- lines */
    init_color(NC_CT_DIFF_ADD_BG, 26*1000/255, 46*1000/255, 26*1000/255);
    init_color(NC_CT_DIFF_DEL_BG, 46*1000/255, 26*1000/255, 26*1000/255);
    /* Diff char-level highlight backgrounds — brighter tints for changed chars */
    init_color(NC_CT_DIFF_ADD_HL, 42*1000/255, 92*1000/255, 42*1000/255);
    init_color(NC_CT_DIFF_DEL_HL, 92*1000/255, 42*1000/255, 42*1000/255);
    /* Search highlight colors */
    init_color(NC_CT_SEARCH_FG,   30*1000/255,  32*1000/255,  48*1000/255);
    init_color(NC_CT_SEARCH_BG,  249*1000/255, 226*1000/255, 175*1000/255);
    init_color(NC_CT_SEARCH_CUR, 250*1000/255, 179*1000/255, 135*1000/255);

    /* Create color pairs combining bg + fg */
    init_pair(CP_STATUS_READY,  NC_STATUS_BG, NC_STATUS_READY);
    init_pair(CP_STATUS_RUNNING, NC_STATUS_BG, NC_STATUS_RUN);
    init_pair(CP_STATUS_AWAIT,  NC_STATUS_BG, NC_STATUS_AWAIT);
    init_pair(CP_STATUS_DONE,   NC_STATUS_BG, NC_STATUS_DONE);
    init_pair(CP_STATUS_ERROR,  NC_STATUS_BG, NC_STATUS_ERR);
    init_pair(CP_INPUT_ACTIVE,  NC_INPUT_BG,  NC_INPUT_FG);
    init_pair(CP_INPUT_DIM,     NC_INPUT_BG,  NC_INPUT_DIM);
    /* Content pairs: fg on transparent/default bg */
    init_pair(C_NORMAL,   NC_CT_NORMAL, -1);
    init_pair(C_SUCCESS,  NC_CT_GREEN,  -1);
    init_pair(C_FAILED,   NC_CT_RED,    -1);
    init_pair(C_STREAM,   NC_CT_CYAN,   -1);
    init_pair(C_STATUS,   NC_CT_WHITE,  -1);
    init_pair(C_DIM,      NC_CT_DIM,    -1);
    init_pair(C_FOCUS,    NC_CT_YELLOW, -1);
    init_pair(C_SELECTED, NC_CT_SELECTED, NC_CT_WHITE);
    /* Diff pairs: fg on colored background */
    init_pair(CP_DIFF_ADD, NC_CT_GREEN,  NC_CT_DIFF_ADD_BG);
    init_pair(CP_DIFF_DEL, NC_CT_RED,    NC_CT_DIFF_DEL_BG);
    /* Diff char-highlight pairs: brighter bg for changed characters */
    init_pair(CP_DIFF_ADD_HL, NC_CT_GREEN, NC_CT_DIFF_ADD_HL);
    init_pair(CP_DIFF_DEL_HL, NC_CT_RED,   NC_CT_DIFF_DEL_HL);
    /* Search highlight pairs */
    init_pair(CP_SEARCH_MATCH,  NC_CT_SEARCH_FG, NC_CT_SEARCH_BG);
    init_pair(CP_SEARCH_CURRENT, NC_CT_SEARCH_FG, NC_CT_SEARCH_CUR);

    true_color_available = 1;
}

/* ── Init / Shutdown ─────────────────────────────────── */

void tui_init(void) {
    setlocale(LC_ALL, "");
    set_escdelay(25);       /* default is 1000ms — far too slow for Esc-as-back */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_FAILED,   COLOR_RED,   -1);
        init_pair(C_SUCCESS,  COLOR_GREEN,  -1);
        init_pair(C_STATUS,   COLOR_BLACK, COLOR_WHITE);
        init_pair(C_DIM,      COLOR_WHITE,  -1);
        init_pair(C_FOCUS,    COLOR_YELLOW, -1);
        init_pair(C_STREAM,   COLOR_CYAN,   -1);
        /* Fallback search highlight pairs (non-true-color) */
        init_pair(CP_SEARCH_MATCH,  COLOR_BLACK, COLOR_YELLOW);
        init_pair(CP_SEARCH_CURRENT, COLOR_BLACK, COLOR_RED);

        /* Initialize true-color palette and pairs */
        init_true_colors();
    }

    /* Layout: main pane gets most of the screen, bottom gets 2 lines */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    bottom_height = 2;
    main_height = rows - bottom_height;

    win_main   = newwin(main_height, cols, 0, 0);
    win_bottom = newwin(bottom_height, cols, main_height, 0);
    scrollok(win_main, FALSE);
    scrollok(win_bottom, FALSE);
    refresh();

    /* Enable bracketed paste mode so we can distinguish pasted text
     * (which may contain newlines) from typed Enter keypresses. */
    printf("\033[?2004h");
    fflush(stdout);

    atomic_store(&g_tui_active, 1);
    atomic_store(&g_tui_was_started, 1);
}

/* Free all stored clipboard entries */
static void clip_store_clear(void) {
    for (int i = 0; i < clip_count; i++) {
        free(clip_store[i].data);
        clip_store[i].data = NULL;
        clip_store[i].len = 0;
    }
    clip_count = 0;
}

/* Finish a bracketed paste: if multi-line, tokenize; else insert directly.
 * Must be called with ui->mtx held. */
static void finish_paste(ui_state_t *ui) {
    if (paste_len <= 0) return;

    /* Check if paste contains a newline */
    int has_nl = 0;
    for (int i = 0; i < paste_len; i++) {
        if (paste_buf[i] == '\n') { has_nl = 1; break; }
    }

    if (!has_nl) {
        /* Single-line paste: insert raw UTF-8 bytes into input buffer.
         * paste_buf already contains properly encoded UTF-8 from the
         * paste accumulation path, so we insert bytes directly rather
         * than going through ui_state_input_char() which would try to
         * re-encode each byte as a codepoint. */
        int nbytes = paste_len;
        /* Ensure capacity */
        while (ui->input_len + nbytes >= ui->input_cap - 1) {
            ui->input_cap *= 2;
            ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
        }
        /* Make room at cursor position */
        memmove(ui->input_buffer + ui->cursor_pos + nbytes,
                ui->input_buffer + ui->cursor_pos,
                (size_t)(ui->input_len - ui->cursor_pos + 1));
        /* Copy paste buffer bytes directly */
        memcpy(ui->input_buffer + ui->cursor_pos, paste_buf, (size_t)nbytes);
        ui->cursor_pos += nbytes;
        ui->input_len += nbytes;
        ui->dirty = 1;
    } else {
        /* Multi-line paste: store in clip_store, insert token */
        if (clip_count < MAX_CLIPS) {
            int idx = clip_count++;
            clip_store[idx].data = malloc((size_t)paste_len + 1);
            memcpy(clip_store[idx].data, paste_buf, (size_t)paste_len);
            clip_store[idx].data[paste_len] = '\0';
            clip_store[idx].len = paste_len;

            /* Insert [clipboardN] token (1-based) into input buffer */
            char token[32];
            snprintf(token, sizeof(token), "[clipboard%d]", idx + 1);
            for (int i = 0; token[i]; i++)
                ui_state_input_char(ui, token[i]);
        }
        /* else: too many clips, silently drop */
    }
    paste_len = 0;
}

/* Expand [clipboardN] tokens in a string, returning a new malloc'd string.
 * Caller must free the result. */
static char *expand_clipboard_tokens(const char *input, int input_len) {
    if (clip_count == 0) return strndup(input, (size_t)input_len);

    /* Worst case: every token expands to max clip size */
    size_t cap = (size_t)input_len + 1;
    for (int i = 0; i < clip_count; i++)
        cap += (size_t)clip_store[i].len + 32;
    char *out = malloc(cap);
    int olen = 0;
    int pos = 0;

    while (pos < input_len) {
        if (input[pos] == '[') {
            /* Try to match [clipboardN] */
            int matched = 0;
            for (int ci = 0; ci < clip_count; ci++) {
                char token[32];
                int tlen = snprintf(token, sizeof(token), "[clipboard%d]", ci + 1);
                if (pos + tlen <= input_len &&
                    memcmp(input + pos, token, (size_t)tlen) == 0) {
                    /* Replace token with clip content */
                    memcpy(out + olen, clip_store[ci].data,
                           (size_t)clip_store[ci].len);
                    olen += clip_store[ci].len;
                    pos += tlen;
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                out[olen++] = input[pos++];
            }
        } else {
            out[olen++] = input[pos++];
        }
    }
    out[olen] = '\0';
    return out;
}

void tui_shutdown(void) {
    atomic_store(&g_tui_active, 0);

    /* Disable bracketed paste mode */
    printf("\033[?2004l");
    fflush(stdout);

    clip_store_clear();

    if (win_main)   delwin(win_main);
    if (win_bottom) delwin(win_bottom);
    win_main = win_bottom = NULL;
    endwin();
}

/* ── Resize handling ─────────────────────────────────── */

/* Prompt is "> " (2 chars). All wrapping math uses this constant. */
#define INPUT_PROMPT_W 2

/* ── Newline-aware wrapping helpers ──────────────────────
 *
 * The input buffer may contain '\n' characters (from Alt+Enter or paste).
 * We split the buffer into "logical lines" at each '\n', then each logical
 * line wraps independently at terminal width.
 *
 * The FIRST logical line's first display row is shortened by INPUT_PROMPT_W
 * (for the "> " prompt). All other display rows use full terminal width.
 * Subsequent logical lines also use full width on all rows (no prompt).
 */

/* Count wrapped display rows for a single logical line of length `len`.
 * `first_row_w` = available width on its first display row.
 * `cont_w`      = full terminal width (for continuation rows). */
static int wrap_rows_for_segment(int len, int first_row_w, int cont_w) {
    if (first_row_w <= 0) first_row_w = 1;
    if (cont_w <= 0) cont_w = 1;
    if (len <= 0) return 1;  /* empty line still occupies one row */
    if (len <= first_row_w) return 1;
    int remaining = len - first_row_w;
    return 1 + (remaining + cont_w - 1) / cont_w;
}

/* Buffer-aware: counts display lines considering '\n' characters.
 * Uses UTF-8 display width (columns) for wrapping, not byte count. */
static int calc_input_lines_buf(const char *buf, int buf_len, int cols) {
    if (buf_len <= 0 || !buf) return 1;
    int first_w = cols - INPUT_PROMPT_W;
    if (first_w <= 0) first_w = 1;
    int cont_w = cols > 0 ? cols : 1;

    int total_rows = 0;
    int pos = 0;
    int logical_line = 0;  /* 0 = first logical line (has prompt) */

    while (pos <= buf_len) {
        /* Find end of this logical line */
        int line_start = pos;
        while (pos < buf_len && buf[pos] != '\n') pos++;
        int line_disp_w = utf8_display_width(buf + line_start, pos - line_start);

        /* First row width depends on whether this is the first logical line */
        int frw = (logical_line == 0) ? first_w : cont_w;
        total_rows += wrap_rows_for_segment(line_disp_w, frw, cont_w);

        if (pos < buf_len) pos++;  /* skip '\n' */
        else break;
        logical_line++;
    }

    return total_rows > 0 ? total_rows : 1;
}

/* Buffer-aware cursor→rowcol: handles '\n' in the input buffer.
 * Uses UTF-8 display widths for cursor column positioning. */
static void cursor_to_rowcol_buf(const char *buf, int buf_len,
                                  int cursor_pos, int cols,
                                  int *out_row, int *out_col) {
    int first_w = cols - INPUT_PROMPT_W;
    if (first_w <= 0) first_w = 1;
    int cont_w = cols > 0 ? cols : 1;

    int display_row = 0;
    int pos = 0;
    int logical_line = 0;

    while (pos <= buf_len) {
        /* Find end of this logical line */
        int line_start = pos;
        while (pos < buf_len && buf[pos] != '\n') pos++;
        int line_byte_len = pos - line_start;

        /* Is the cursor within this logical line? */
        if (cursor_pos >= line_start &&
            (cursor_pos < pos || (cursor_pos == pos && (pos >= buf_len || buf[pos] == '\n')))) {
            /* Cursor is in this logical line — compute display column offset */
            int cursor_byte_offset = cursor_pos - line_start;
            int cursor_disp_w = utf8_display_width(buf + line_start, cursor_byte_offset);
            int frw = (logical_line == 0) ? first_w : cont_w;
            if (cursor_disp_w <= frw) {
                *out_row = display_row;
                *out_col = (logical_line == 0)
                           ? INPUT_PROMPT_W + cursor_disp_w
                           : cursor_disp_w;
            } else {
                int rem = cursor_disp_w - frw;
                *out_row = display_row + 1 + rem / cont_w;
                *out_col = rem % cont_w;
            }
            return;
        }

        /* Advance display_row by the number of wrapped rows for this line */
        int frw = (logical_line == 0) ? first_w : cont_w;
        int line_disp_w = utf8_display_width(buf + line_start, line_byte_len);
        display_row += wrap_rows_for_segment(line_disp_w, frw, cont_w);

        if (pos < buf_len) pos++;  /* skip '\n' */
        else break;
        logical_line++;
    }

    /* Fallback: cursor at end */
    *out_row = display_row > 0 ? display_row - 1 : 0;
    *out_col = 0;
}

static void resize_panes_with_input_buf(const char *buf, int input_len,
                                         int input_cursor);

static void resize_panes_with_input_buf(const char *buf, int input_len,
                                         int input_cursor) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Dynamic bottom height: 1 (status) + wrapped input lines */
    int input_lines = calc_input_lines_buf(buf, input_len, cols);
    /* Also ensure cursor row is visible */
    int cursor_row, cursor_col;
    cursor_to_rowcol_buf(buf, input_len, input_cursor, cols,
                         &cursor_row, &cursor_col);
    if (cursor_row + 1 > input_lines) input_lines = cursor_row + 1;
    /* Cap at 50% of screen */
    int max_bottom = rows / 2;
    if (max_bottom < 2) max_bottom = 2;
    bottom_height = 1 + input_lines;
    if (bottom_height > max_bottom) bottom_height = max_bottom;
    if (bottom_height < 2) bottom_height = 2;

    main_height = rows - bottom_height;

    wresize(win_main, main_height, cols);
    mvwin(win_main, 0, 0);
    wresize(win_bottom, bottom_height, cols);
    mvwin(win_bottom, main_height, 0);
}

/* ── In-page search: highlight overlay ──────────────── */

/* Case-insensitive substring search (local to tui.c). */
static const char *tui_ci_strstr(const char *haystack, const char *needle) {
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++; n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/* Scan the entire document source for matches and record their rendered
 * line numbers.  This mirrors md_render's line counting logic (skipping
 * ``` fence lines, counting wrapped code lines) so that 'n' can scroll
 * to the correct position.  Must be called after md_render() so that
 * doc->total_lines is accurate. */
static void page_search_scan_matches(ui_state_t *ui) {
    ui->page_search_total = 0;
    if (!ui->page_search_term || !ui->page_search_term[0]) return;
    if (!ui->doc || !ui->doc->source) return;

    const char *term = ui->page_search_term;
    const char *src = ui->doc->source;
    int render_line = 0;
    int in_code_block = 0;

    while (*src) {
        const char *eol = strchr(src, '\n');
        int line_len = eol ? (int)(eol - src) : (int)strlen(src);

        /* Check for code fence toggle */
        if (line_len >= 3 && src[0] == '`' && src[1] == '`' && src[2] == '`') {
            in_code_block = !in_code_block;
            src = eol ? eol + 1 : src + line_len;
            continue;  /* fence lines don't get a render_line */
        }

        /* Check if this line contains the search term */
        char line_buf[NASH_PATH_MAX];
        int copy_len = line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
        memcpy(line_buf, src, (size_t)copy_len);
        line_buf[copy_len] = '\0';

        if (tui_ci_strstr(line_buf, term)) {
            /* Record this match line */
            if (ui->page_search_total >= ui->page_search_lines_cap) {
                int new_cap = ui->page_search_lines_cap ? ui->page_search_lines_cap * 2 : 64;
                int *new_arr = realloc(ui->page_search_lines, (size_t)new_cap * sizeof(int));
                if (new_arr) {
                    ui->page_search_lines = new_arr;
                    ui->page_search_lines_cap = new_cap;
                }
            }
            if (ui->page_search_total < ui->page_search_lines_cap) {
                ui->page_search_lines[ui->page_search_total++] = render_line;
            }
        }

        render_line++;
        src = eol ? eol + 1 : src + line_len;
    }

    /* Clamp current match index */
    if (ui->page_search_total > 0) {
        if (ui->page_search_current >= ui->page_search_total)
            ui->page_search_current = 0;
    } else {
        ui->page_search_current = 0;
    }
}

/* Apply search highlighting to the already-rendered main window.
 * Reads back each visible row, finds case-insensitive matches of
 * the search term.  Uses CP_SEARCH_MATCH (yellow bg) for normal matches
 * and CP_SEARCH_CURRENT (orange bg) for the match at page_search_current,
 * so the cursor position is visually distinct. */
static void page_search_highlight(ui_state_t *ui) {
    if (!ui->page_search_term || !ui->page_search_term[0]) return;

    int rows = getmaxy(win_main);
    int cols = getmaxx(win_main);
    int term_len = (int)strlen(ui->page_search_term);

    /* Determine which rendered line the current match is on */
    int cur_render_line = -1;
    if (ui->page_search_total > 0 && ui->page_search_lines &&
        ui->page_search_current < ui->page_search_total) {
        cur_render_line = ui->page_search_lines[ui->page_search_current];
    }

    for (int r = 0; r < rows; r++) {
        /* Read the rendered text from the window */
        char row_buf[NASH_PATH_MAX];
        int n = (cols < (int)sizeof(row_buf) - 1) ? cols : (int)sizeof(row_buf) - 1;
        int got = mvwinnstr(win_main, r, 0, row_buf, n);
        if (got <= 0) continue;
        row_buf[got] = '\0';

        /* The rendered line number for this screen row */
        int render_line = ui->scroll_y + r;

        /* Is this the row containing the current match? */
        int is_current_line = (render_line == cur_render_line);

        /* Find all case-insensitive matches in this row */
        const char *p = row_buf;
        while ((p = tui_ci_strstr(p, ui->page_search_term)) != NULL) {
            /* Convert byte offset to display column position */
            int col = utf8_display_width(row_buf, (int)(p - row_buf));
            /* Convert match byte length to display width */
            int match_width = utf8_display_width(p, term_len);
            if (match_width <= 0) match_width = 1;
            if (col < cols) {
                short pair = is_current_line ? CP_SEARCH_CURRENT : CP_SEARCH_MATCH;
                mvwchgat(win_main, r, col, match_width, A_BOLD, pair, NULL);
            }
            p += term_len;
        }
    }
}

/* ── Render main pane (MD document) ──────────────────── */

static void render_main(ui_state_t *ui) {
    int rows = getmaxy(win_main);
    ui->visible_rows = rows;  /* tell ui_state how tall the main pane is */
    int cols = getmaxx(win_main);
    ui->visible_cols = cols;  /* tell ui_state how wide the main pane is */

    werase(win_main);

    if (ui->doc) {
        int focus = (ui->focus == FOCUS_JOURNAL);
        md_render(win_main, ui->doc, ui->scroll_y, ui->scroll_x,
                 ui->cursor_link, focus);

        /* Deferred auto-scroll: now that md_render() has set doc->total_lines
         * and link render_lines, we can compute the correct scroll_y.
         * Before this, auto_scroll_bottom ran right after md_parse which
         * hadn't set these values yet (total_lines=0, render_line=-1). */
        if (ui->needs_auto_scroll) {
            ui->needs_auto_scroll = 0;

            /* Move cursor to last link */
            if (ui->doc->link_count > 0)
                ui->cursor_link = ui->doc->link_count - 1;

            int vis = rows > 0 ? rows : 20;

            if (ui->doc->link_count > 0) {
                int link_line = md_link_line(ui->doc, ui->doc->link_count - 1);
                int half = vis / 2;
                int target = link_line - half;
                int max_scroll = ui->doc->total_lines - vis;
                if (max_scroll < 0) max_scroll = 0;
                if (target < 0) target = 0;
                if (target > max_scroll) target = max_scroll;
                ui->scroll_y = target;
            } else {
                int max_scroll = ui->doc->total_lines - vis;
                if (max_scroll < 0) max_scroll = 0;
                ui->scroll_y = max_scroll;
            }

            /* Re-render with corrected scroll position */
            werase(win_main);
            md_render(win_main, ui->doc, ui->scroll_y, ui->scroll_x,
                     ui->cursor_link, focus);
        }
    } else {
        wattron(win_main, COLOR_PAIR(C_DIM));
        mvwaddstr(win_main, 0, 0, "  Loading...");
        wattroff(win_main, COLOR_PAIR(C_DIM));
    }

    /* In-page search: scan for match positions, then highlight visible matches */
    if (ui->page_search_term && ui->page_search_term[0]) {
        page_search_scan_matches(ui);
        page_search_highlight(ui);
    }

    wnoutrefresh(win_main);
}

/* ── Render bottom pane (nashell-style status + input) ─ */

/* Render a full-width row using ncurses attributes (not ANSI escapes).
 * pair_num selects the bg/fg combination from the registered color pairs. */
static void render_ncurses_row(WINDOW *win, int row, int cols,
                                int pair_num,
                                const char *content) {
    /* Write content then pad to end of row with mvwaddch using the correct
     * color pair. wclrtoeol() fills with win->_nc_bkgd (window background
     * from wbkgdset), NOT the current attribute from wattron — so the
     * status bar color would be lost after the text.
     *
     * Using explicit mvwaddch padding also handles UTF-8 correctly:
     * mvwaddnstr advances the cursor by display columns (not bytes), so
     * getyx() returns the correct column after multi-byte chars.
     * E.g. "⟳" is 3 bytes but 1 column — byte-based padding would
     * start 2 columns too late. */
    attr_t attr = COLOR_PAIR(pair_num);
    wattron(win, attr);
    if (content && content[0]) {
        mvwaddnstr(win, row, 0, content, -1);
    }
    /* Pad from current cursor position to end of row.
     * If the content exactly filled the row, mvwaddnstr wraps the cursor
     * to the next row (getcury != row, getcurx == 0).  In that case the
     * row is already fully covered — padding would overwrite it with
     * spaces because the loop uses the original `row` parameter. */
    {
        int cur_y = getcury(win);
        int cur_x = getcurx(win);

        if (cur_y == row) {
            for (int c = cur_x; c < cols; c++) {
                mvwaddch(win, row, c, ' ' | attr);
            }
        }
        /* else: cursor wrapped → row is fully filled, no padding needed */
    }
    wattroff(win, attr);
}

static void render_bottom(ui_state_t *ui) {
    int cols = getmaxx(win_bottom);
    int bh   = getmaxy(win_bottom);

    werase(win_bottom);

    /* ── Row 0: Status bar (nashell-style) ──
     * Layout: [icon][status_text] [model_name │ ctx XX% │ 📡 bg:N]
     * Colors: dark blue bg (#232637), light text (#DCE5F0) */

    /* Build status line content */
    char status_line[1024];
    int slen = 0;

    /* Status icon + text + select color pair */
    const char *icon;
    int status_pair;
    switch (ui->status) {
        case STATUS_RUNNING:
            icon = "⟳";
            status_pair = CP_STATUS_RUNNING;
            break;
        case STATUS_AWAITING_INPUT:
            icon = "?";
            status_pair = CP_STATUS_AWAIT;
            break;
        case STATUS_DONE:
            icon = "✓";
            status_pair = CP_STATUS_DONE;
            break;
        case STATUS_ERROR:
            icon = "✗";
            status_pair = CP_STATUS_ERROR;
            break;
        default:
            icon = " ";
            status_pair = CP_STATUS_READY;
            break;
    }

    slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                     "%s ", icon);
    if (ui->status_text && ui->status_text[0]) {
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         "%s", ui->status_text);
    }

    /* Breadcrumb path (shows current file in nav stack) */
    {
        char *crumb = ui_state_breadcrumb(ui);
        if (crumb && crumb[0]) {
            if (ui->workspace_name && ui->workspace_name[0])
                slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                                 " │ %s %s", ui->workspace_name, crumb);
            else
                slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                                 " │ %s", crumb);
        }
        free(crumb);
    }

    /* Right side: model │ ctx │ bg */
    if (ui->model_name && ui->model_name[0]) {
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         " │");
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         " %s", ui->model_name);
    }
    if (ui->context_size > 0 && ui->context_used > 0) {
        double ctx_pct = 100.0 * ui->context_used / ui->context_size;
        if (ctx_pct >= 1.0)
            slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                             " │ ctx %d%%", (int)ctx_pct);
        else
            slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                             " │ ctx <1%%");
    } else if (ui->context_size > 0) {
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         " │ ctx 0%%");
    }
    if (ui->bg_jobs > 0) {
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         " │ bg:%d", ui->bg_jobs);
    }

    /* Status bar: dark blue bg (#232637), light text (#DCE5F0) */
    render_ncurses_row(win_bottom, 0, cols, status_pair, status_line);

    /* ── Row 1+: Input prompt (nashell-style) ──
     * Layout: [>][▌cursor or input text]
     * Colors: slightly lighter dark blue bg (#191E2D), light text (#DCE5F0) */
    int input_start_row = 1;

    if (ui->focus == FOCUS_QUERY) {
        /* Render input text with newline-aware wrapping across multiple rows.
         * The buffer is split into logical lines at '\n' characters.
         * First logical line's first row shows "> " prompt (INPUT_PROMPT_W).
         * All other rows use full terminal width. */
        int first_w = cols - INPUT_PROMPT_W;
        if (first_w <= 0) first_w = 1;
        int cont_w = cols > 0 ? cols : 1;
        const char *text = ui->input_buffer;
        int text_len = (ui->input_buffer && ui->input_len > 0) ? ui->input_len : 0;
        int row = input_start_row;
        int text_pos = 0;
        int logical_line = 0;

        while (text_pos <= text_len && row < bh) {
            /* Find end of this logical line (up to '\n' or end of buffer) */
            int line_start = text_pos;
            while (text_pos < text_len && text[text_pos] != '\n') text_pos++;
            int line_len = text_pos - line_start;

            /* First row of this logical line */
            int is_first_logical = (logical_line == 0);
            int frw = is_first_logical ? first_w : cont_w;

            /* Render first row of this logical line.
             * Use utf8_bytes_for_width to find how many bytes fit in
             * the available display columns, so multi-byte UTF-8 chars
             * are not split and wrapping is correct. */
            {
                char row_buf[1024];
                int rlen = 0;
                if (is_first_logical) {
                    rlen += snprintf(row_buf + rlen, sizeof(row_buf) - rlen, "> ");
                }
                int chunk = utf8_bytes_for_width(text + line_start, line_len, frw);
                if (chunk > 0 && text) {
                    if (chunk > (int)sizeof(row_buf) - rlen - 1)
                        chunk = (int)sizeof(row_buf) - rlen - 1;
                    memcpy(row_buf + rlen, text + line_start, (size_t)chunk);
                    rlen += chunk;
                }
                row_buf[rlen] = '\0';
                render_ncurses_row(win_bottom, row, cols,
                                    CP_INPUT_ACTIVE, row_buf);
                row++;
                int seg_pos = chunk;

                /* Continuation rows within this logical line (wrapping) */
                while (seg_pos < line_len && row < bh) {
                    int wchunk = utf8_bytes_for_width(text + line_start + seg_pos,
                                                     line_len - seg_pos, cont_w);
                    if (wchunk <= 0) wchunk = 1; /* safety: advance at least 1 byte */
                    char wbuf[1024];
                    if (wchunk > (int)sizeof(wbuf) - 1)
                        wchunk = (int)sizeof(wbuf) - 1;
                    memcpy(wbuf, text + line_start + seg_pos, (size_t)wchunk);
                    wbuf[wchunk] = '\0';
                    render_ncurses_row(win_bottom, row, cols,
                                        CP_INPUT_ACTIVE, wbuf);
                    seg_pos += wchunk;
                    row++;
                }
            }

            /* Skip the '\n' character */
            if (text_pos < text_len) text_pos++;
            else break;
            logical_line++;
        }

        /* Fill any remaining rows with empty input-colored background */
        for (int r = row; r < bh; r++) {
            render_ncurses_row(win_bottom, r, cols,
                                CP_INPUT_ACTIVE, "");
        }

        /* Position cursor using buffer-aware wrapping math */
        int cursor_row, cursor_col;
        cursor_to_rowcol_buf(ui->input_buffer, ui->input_len,
                             ui->cursor_pos, cols,
                             &cursor_row, &cursor_col);
        cursor_row += input_start_row;  /* offset by status bar row */
        if (cursor_row >= bh) cursor_row = bh - 1;
        if (cursor_col >= cols) cursor_col = cols - 1;
        wmove(win_bottom, cursor_row, cursor_col);
    } else {
        /* Not focused: show a dim prompt */
        render_ncurses_row(win_bottom, input_start_row, cols,
                            CP_INPUT_DIM, "> ");

        /* Fill remaining rows with input background */
        for (int r = input_start_row + 1; r < bh; r++) {
            render_ncurses_row(win_bottom, r, cols,
                                CP_INPUT_ACTIVE, "");
        }
    }

    wnoutrefresh(win_bottom);
}

/* ── Main render entry point ─────────────────────────── */

void tui_render(ui_state_t *ui) {
    if (!ui) return;

    pthread_mutex_lock(&ui->mtx);
    if (!ui->dirty) {
        pthread_mutex_unlock(&ui->mtx);
        return;
    }

    resize_panes_with_input_buf(ui->input_buffer, ui->input_len, ui->cursor_pos);

    /* Clear stdscr to prevent stale background content showing through */
    werase(stdscr);
    wnoutrefresh(stdscr);

    /* Render both panes (each does werase + draw + wnoutrefresh) */
    render_main(ui);
    render_bottom(ui);

    /* Force ncurses to redraw every character (not just changes).
     * This implements true double-buffering: the entire screen is
     * recomposed from scratch on every render cycle. */
    touchwin(win_main);
    touchwin(win_bottom);
    wnoutrefresh(win_main);
    wnoutrefresh(win_bottom);

    /* Set cursor visibility based on focus */
    curs_set(ui->focus == FOCUS_QUERY ? 1 : 0);

    /* Single doupdate() flushes ALL window changes to terminal at once.
     * This is ncurses' built-in double-buffer: all changes are computed
     * in memory, then written to the terminal in one batch. */
    doupdate();

    /* Emit deferred OSC 8 hyperlink sequences directly to stdout.
     * Must happen AFTER doupdate() since ncurses' waddch cannot pass
     * ESC bytes to the terminal (renders them as ^[ caret notation). */
    if (md_osc8_count > 0)
        md_osc8_flush(getbegy(win_main));

    ui->dirty = 0;

    pthread_mutex_unlock(&ui->mtx);
}

/* ── Input handling ──────────────────────────────────── */

int tui_input(ui_state_t *ui, char **out_query) {
    if (!ui) return 0;
    *out_query = NULL;

    int ch = getch();
    if (ch == ERR) return 0;

    pthread_mutex_lock(&ui->mtx);

    /* During bracketed paste, intercept all printable chars + newline + tab
     * and accumulate into paste_buf. Only ESC sequences (for detecting the
     * paste-end bracket ESC[201~) pass through normally. */
    if (paste_mode && ui->focus == FOCUS_QUERY) {
        if (ch == '\n' || ch == KEY_ENTER) {
            if (paste_len < PASTE_BUF_CAP - 1)
                paste_buf[paste_len++] = '\n';
            goto paste_done;
        } else if (ch == '\t') {
            if (paste_len < PASTE_BUF_CAP - 1)
                paste_buf[paste_len++] = '\t';
            goto paste_done;
        } else if (ch >= 32 && ch != 127) {
            /* Accept printable ASCII, raw UTF-8 bytes, and Unicode codepoints.
             *
             * During bracketed paste with nodelay(TRUE), getch() often returns
             * raw UTF-8 bytes (0x80-0xFF) instead of assembled codepoints.
             * Values in 0x80-0xFF are stored as-is (they're already valid
             * UTF-8 bytes — lead or continuation). Values >= 0x100 are
             * assembled codepoints from ncursesw and need encoding. */
            char utf8[4];
            int nb;
            if (ch < 0x100) {
                /* ASCII (< 0x80) or raw UTF-8 byte (0x80-0xFF): store as-is */
                utf8[0] = (char)ch; nb = 1;
            } else if (ch < 0x800) {
                utf8[0] = (char)(0xC0 | (ch >> 6));
                utf8[1] = (char)(0x80 | (ch & 0x3F)); nb = 2;
            } else if (ch < 0x10000) {
                utf8[0] = (char)(0xE0 | (ch >> 12));
                utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
                utf8[2] = (char)(0x80 | (ch & 0x3F)); nb = 3;
            } else if (ch < 0x110000) {
                utf8[0] = (char)(0xF0 | (ch >> 18));
                utf8[1] = (char)(0x80 | ((ch >> 12) & 0x3F));
                utf8[2] = (char)(0x80 | ((ch >> 6) & 0x3F));
                utf8[3] = (char)(0x80 | (ch & 0x3F)); nb = 4;
            } else {
                goto paste_done;  /* invalid codepoint */
            }
            if (paste_len + nb < PASTE_BUF_CAP) {
                memcpy(paste_buf + paste_len, utf8, (size_t)nb);
                paste_len += nb;
            }
            goto paste_done;
        }
        /* else: ESC, special keys — fall through to normal handling
         * (needed to detect ESC[201~ paste-end sequence) */
    }

    /* FIX: Handle terminal resize (SIGWINCH → KEY_RESIZE from ncurses).
     * Without this, resize during operation can leave windows with stale
     * dimensions, causing rendering corruption or out-of-bounds writes.
     * endwin()+refresh() lets ncurses re-read terminal dimensions,
     * then resize_panes recalculates window layout. */
    if (ch == KEY_RESIZE) {
        endwin();
        refresh();
        resize_panes_with_input_buf(ui->input_buffer, ui->input_len,
                                     ui->cursor_pos);
        ui->dirty = 1;
        pthread_mutex_unlock(&ui->mtx);
        return 1;
    }

    switch (ch) {
    case '\t':
    case KEY_BTAB:
        ui_state_tab(ui);
        break;

    case KEY_UP:
        if (ui->focus == FOCUS_QUERY) {
            /* Navigate up within wrapped input lines (newline-aware) */
            int cols_now = getmaxx(stdscr);
            int crow, ccol;
            cursor_to_rowcol_buf(ui->input_buffer, ui->input_len,
                                 ui->cursor_pos, cols_now, &crow, &ccol);
            if (crow > 0) {
                /* Find the buffer position one display row up.
                 * Walk backward from cursor to find the start of the current
                 * display row, then go one more row back. */
                int fw = cols_now - INPUT_PROMPT_W; if (fw < 1) fw = 1;
                int cw = cols_now > 0 ? cols_now : 1;

                /* Scan backward to find a position on the previous display row.
                 * Use UTF-8 display widths to find byte position on prior row. */
                int cur_w = (crow == 0) ? fw : cw;
                /* Walk backward in the buffer by cur_w display columns */
                int new_pos = ui->cursor_pos;
                { int cols_back = 0;
                  while (new_pos > 0 && cols_back < cur_w) {
                      const char *prev = utf8_prev(ui->input_buffer, ui->input_buffer + new_pos);
                      int cw2 = utf8_char_width(prev);
                      if (cols_back + cw2 > cur_w) break;
                      cols_back += cw2;
                      new_pos = (int)(prev - ui->input_buffer);
                  }
                }
                if (new_pos < 0) new_pos = 0;
                /* Don't cross a newline boundary — clamp to start of current
                 * logical line if we'd jump past a '\n' */
                int nl_pos = ui->cursor_pos - 1;
                while (nl_pos >= new_pos) {
                    if (ui->input_buffer[nl_pos] == '\n') {
                        /* There's a newline between new_pos and cursor.
                         * Move to the position on the line above the '\n'. */
                        /* Find the start of the line above */
                        int above_start = nl_pos;
                        while (above_start > 0 && ui->input_buffer[above_start - 1] != '\n')
                            above_start--;
                        int above_byte_len = nl_pos - above_start;
                        /* Try to land at same display column offset */
                        int byte_off = utf8_bytes_for_width(
                            ui->input_buffer + above_start, above_byte_len, ccol);
                        new_pos = above_start + byte_off;
                        break;
                    }
                    nl_pos--;
                }
                if (new_pos < 0) new_pos = 0;
                if (new_pos > ui->input_len) new_pos = ui->input_len;
                ui->cursor_pos = new_pos;
                ui->dirty = 1;
            } else {
                /* On first display row — load previous history */
                if (ui->history_count > 0 && ui->history_idx > 0) {
                    /* Stash in-progress input when first entering history */
                    if (ui->history_idx == ui->history_count) {
                        free(ui->saved_input);
                        ui->saved_input = malloc((size_t)(ui->input_len + 1));
                        if (ui->saved_input) {
                            memcpy(ui->saved_input, ui->input_buffer, (size_t)ui->input_len);
                            ui->saved_input[ui->input_len] = '\0';
                        }
                        ui->saved_input_len = ui->input_len;
                    }
                    ui->history_idx--;
                    int hlen = (int)strlen(ui->history[ui->history_idx]);
                    if (hlen >= ui->input_cap) {
                        ui->input_cap = hlen + 64;
                        ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
                    }
                    memcpy(ui->input_buffer, ui->history[ui->history_idx], (size_t)hlen);
                    ui->input_len = hlen;
                    ui->cursor_pos = hlen;
                    ui->dirty = 1;
                }
            }
        } else {
            ui_state_up(ui);
        }
        break;

    case KEY_DOWN:
        if (ui->focus == FOCUS_QUERY) {
            /* Navigate down within wrapped input lines (newline-aware) */
            int cols_now = getmaxx(stdscr);
            int crow, ccol;
            cursor_to_rowcol_buf(ui->input_buffer, ui->input_len,
                                 ui->cursor_pos, cols_now, &crow, &ccol);
            int total_rows = calc_input_lines_buf(ui->input_buffer,
                                                   ui->input_len, cols_now);
            if (crow < total_rows - 1) {
                /* Move down one display row */
                int fw = cols_now - INPUT_PROMPT_W; if (fw < 1) fw = 1;
                int cw = cols_now > 0 ? cols_now : 1;
                int cur_w = (crow == 0) ? fw : cw;

                /* Walk forward by cur_w display columns to find scan boundary */
                int scan_end = ui->cursor_pos;
                { int cols_fwd = 0;
                  while (scan_end < ui->input_len && cols_fwd < cur_w) {
                      if (ui->input_buffer[scan_end] == '\n') break;
                      int cw2 = utf8_char_width(ui->input_buffer + scan_end);
                      cols_fwd += cw2;
                      scan_end += utf8_char_len(ui->input_buffer + scan_end);
                  }
                }
                /* Check if there's a '\n' between cursor and scan_end */
                int nl_found = -1;
                for (int i = ui->cursor_pos; i < scan_end; i++) {
                    if (ui->input_buffer[i] == '\n') {
                        nl_found = i;
                        break;
                    }
                }
                int new_pos;
                if (nl_found >= 0) {
                    /* There's a newline — move to the next logical line */
                    int next_start = nl_found + 1;
                    int next_end = next_start;
                    while (next_end < ui->input_len && ui->input_buffer[next_end] != '\n')
                        next_end++;
                    int next_byte_len = next_end - next_start;
                    /* Land at same display column offset */
                    int byte_off = utf8_bytes_for_width(
                        ui->input_buffer + next_start, next_byte_len, ccol);
                    new_pos = next_start + byte_off;
                } else {
                    /* No newline — jump forward by cw display columns */
                    int fwd_bytes = utf8_bytes_for_width(
                        ui->input_buffer + ui->cursor_pos,
                        ui->input_len - ui->cursor_pos, cw);
                    new_pos = ui->cursor_pos + fwd_bytes;
                }
                if (new_pos > ui->input_len) new_pos = ui->input_len;
                ui->cursor_pos = new_pos;
                ui->dirty = 1;
            } else {
                /* On last display row — load next history or restore saved input */
                if (ui->history_count > 0 && ui->history_idx < ui->history_count) {
                    ui->history_idx++;
                    if (ui->history_idx == ui->history_count) {
                        /* Past end — restore stashed in-progress input */
                        int hlen = ui->saved_input ? ui->saved_input_len : 0;
                        if (hlen >= ui->input_cap) {
                            ui->input_cap = hlen + 64;
                            ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
                        }
                        if (hlen > 0)
                            memcpy(ui->input_buffer, ui->saved_input, (size_t)hlen);
                        ui->input_len = hlen;
                        ui->cursor_pos = hlen;
                        free(ui->saved_input);
                        ui->saved_input = NULL;
                        ui->saved_input_len = 0;
                    } else {
                        int hlen = (int)strlen(ui->history[ui->history_idx]);
                        if (hlen >= ui->input_cap) {
                            ui->input_cap = hlen + 64;
                            ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
                        }
                        memcpy(ui->input_buffer, ui->history[ui->history_idx], (size_t)hlen);
                        ui->input_len = hlen;
                        ui->cursor_pos = hlen;
                    }
                    ui->dirty = 1;
                }
            }
        } else {
            ui_state_down(ui);
        }
        break;

    case KEY_PPAGE:
        ui_state_page_up(ui);
        break;

    case KEY_NPAGE:
        ui_state_page_down(ui);
        break;

    case '\n':
    case KEY_ENTER:
        if (ui->focus == FOCUS_JOURNAL) {
            ui_state_enter(ui);
        } else if (ui->focus == FOCUS_QUERY) {
            if (paste_mode) {
                /* During bracketed paste, Enter appends newline to paste buffer */
                if (paste_len < PASTE_BUF_CAP - 1)
                    paste_buf[paste_len++] = '\n';
            } else if (ui->search_active && ui->input_len >= 2 &&
                       ui->input_buffer[0] == '/' && ui->input_buffer[1] == '?') {
                /* Search mode: Enter follows the highlighted link in results.
                 * Switch focus to main pane and navigate to the selected link. */
                ui->focus = FOCUS_JOURNAL;
                ui->input_buffer[0] = '\0';
                ui->input_len = 0;
                ui->cursor_pos = 0;
                ui_state_enter(ui);
                ui->dirty = 1;
            } else if (ui->page_search_term && ui->input_len >= 1 &&
                       ui->input_buffer[0] == '?') {
                /* In-page search: Enter switches focus to main pane.
                 * Highlights stay active; 'n' navigates to next match. */
                ui->focus = FOCUS_JOURNAL;
                /* Scroll to first match if available */
                if (ui->page_search_total > 0) {
                    ui->page_search_current = 0;
                    int target = ui->page_search_lines[0];
                    int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
                    int half = vis / 2;
                    int scroll = target - half;
                    if (scroll < 0) scroll = 0;
                    ui->scroll_y = scroll;
                    ui->user_scrolled = 1;
                }
                ui->dirty = 1;
            } else if (ui->input_len > 0) {
                /* Submit query — expand clipboard tokens, save to history */
                *out_query = expand_clipboard_tokens(ui->input_buffer,
                                                      ui->input_len);
                /* Add to history (grow array if needed) */
                if (ui->history_count >= ui->history_cap) {
                    int new_cap = ui->history_cap ? ui->history_cap * 2 : 32;
                    char **new_hist = realloc(ui->history,
                                              (size_t)new_cap * sizeof(char *));
                    if (new_hist) {
                        ui->history = new_hist;
                        ui->history_cap = new_cap;
                    }
                }
                if (ui->history_count < ui->history_cap) {
                    ui->history[ui->history_count++] = strdup(*out_query);
                }
                ui->history_idx = ui->history_count;  /* past end = fresh input */
                clip_store_clear();  /* discard expanded clipboard entries */
                ui->input_buffer[0] = '\0';
                ui->input_len = 0;
                ui->cursor_pos = 0;
                ui->focus = FOCUS_JOURNAL;  /* switch focus to main pane */
                ui->dirty = 1;
            }
        }
        break;

    case 27: { /* Escape — detect Alt+Enter and bracketed paste sequences */
        /* Peek at next character to distinguish ESC from Alt+key / paste bracket.
         * nodelay is already TRUE, so getch() returns ERR if no char is pending. */
        int next = getch();
        if (next == '\n' || next == '\r') {
            /* Alt+Enter: insert newline into query */
            if (ui->focus == FOCUS_QUERY) {
                ui_state_input_char(ui, '\n');
            }
        } else if (next == '[') {
            /* Could be a bracketed paste sequence: ESC [ 2 0 0 ~ or ESC [ 2 0 1 ~ */
            int seq[4];
            int got = 0;
            for (int i = 0; i < 4; i++) {
                seq[i] = getch();
                if (seq[i] == ERR) break;
                got++;
            }
            if (got == 4 && seq[0] == '2' && seq[1] == '0' && seq[3] == '~') {
                if (seq[2] == '0') {
                    paste_mode = 1;  /* Start of bracketed paste */
                    paste_len = 0;   /* Reset paste accumulation buffer */
                } else if (seq[2] == '1') {
                    finish_paste(ui); /* Store/insert pasted content */
                    paste_mode = 0;  /* End of bracketed paste */
                }
            }
            /* Other ESC [ sequences are consumed (arrow keys etc. handled by ncurses) */
        } else if (next == ERR) {
            /* Plain Escape — clear page search or navigate back */
            if (ui->focus == FOCUS_JOURNAL && ui->page_search_term) {
                /* Clear in-page search and return to input */
                free(ui->page_search_term);
                ui->page_search_term = NULL;
                ui->page_search_total = 0;
                ui->page_search_current = 0;
                ui->focus = FOCUS_QUERY;
                /* Clear the ?search input */
                ui->input_buffer[0] = '\0';
                ui->input_len = 0;
                ui->cursor_pos = 0;
                ui->dirty = 1;
            } else if (ui->focus == FOCUS_JOURNAL) {
                ui_state_back(ui);
            }
        }
        /* else: some other Alt+key combo, ignore */
        break;
    }

    case ' ':  /* Space — toggle pause/resume */
        if (ui->focus == FOCUS_JOURNAL &&
            ui->status == STATUS_RUNNING && ui->pause_flag) {
            /* Running → pause: abort the in-progress HTTP call so the
             * react loop reaches the pause_requested check immediately
             * instead of waiting for the full LLM response to complete. */
            *ui->pause_flag = 1;
            if (ui->abort_flag) *ui->abort_flag = 1;
            ui_state_set_status(ui, STATUS_READY, "Pausing...");
            ui->dirty = 1;
            break;
        } else if (ui->focus == FOCUS_JOURNAL &&
                   ui->status == STATUS_READY && ui->session_dir) {
            /* Paused → resume with original query (toggle) */
            char cp_path[NASH_PATH_MAX];
            snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.json", ui->session_dir);
            if (access(cp_path, F_OK) == 0) {
                /* Checkpoint exists — read original query and return it */
                {
                    cJSON *cp = slurp_json(cp_path);
                    if (cp) {
                        cJSON *q = cJSON_GetObjectItem(cp, "user_query");
                        if (q && q->valuestring && q->valuestring[0]) {
                            *out_query = strdup(q->valuestring);
                            ui_state_set_status(ui, STATUS_READY,
                                "Resuming (Space toggle — type query + Enter for new direction)");
                            ui->dirty = 1;
                        }
                        cJSON_Delete(cp);
                    }
                }
                if (*out_query) break;
            }
        }
        /* Otherwise fall through to default (typing space in query) */
        goto handle_default;

    case KEY_LEFT:
        if (ui->focus == FOCUS_QUERY) ui_state_input_left(ui);
        else if (ui->focus == FOCUS_JOURNAL && ui->scroll_x > 0) {
            ui->scroll_x -= 4;
            if (ui->scroll_x < 0) ui->scroll_x = 0;
            ui->dirty = 1;
        }
        break;

    case KEY_RIGHT:
        if (ui->focus == FOCUS_QUERY) ui_state_input_right(ui);
        else if (ui->focus == FOCUS_JOURNAL) {
            ui->scroll_x += 4;
            ui->dirty = 1;
        }
        break;

    case KEY_HOME:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_home(ui);
        } else if (ui->focus == FOCUS_JOURNAL) {
            /* HOME in journal: scroll to top of current md file */
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->user_scrolled = 1;  /* prevent auto-scroll override */
            /* Move cursor to first visible link */
            if (ui->doc && ui->doc->link_count > 0) {
                int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
                ui->cursor_link = 0;
                for (int i = 0; i < ui->doc->link_count; i++) {
                    int ll = md_link_line(ui->doc, i);
                    if (ll >= 0 && ll < vis) {
                        ui->cursor_link = i;
                        break;
                    }
                }
            }
            ui->dirty = 1;
        }
        break;

    case KEY_END:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_end(ui);
        } else if (ui->focus == FOCUS_JOURNAL) {
            /* END in journal: scroll to bottom, resume auto-scroll */
            ui->user_scrolled = 0;
            if (ui->doc) {
                int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
                int max_scroll = ui->doc->total_lines - vis;
                if (max_scroll < 0) max_scroll = 0;
                ui->scroll_y = max_scroll;
                /* Move cursor to last link */
                if (ui->doc->link_count > 0)
                    ui->cursor_link = ui->doc->link_count - 1;
            }
            ui->dirty = 1;
        }
        break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (ui->focus == FOCUS_QUERY) ui_state_input_backspace(ui);
        break;

    case KEY_DC:
        if (ui->focus == FOCUS_QUERY) ui_state_input_delete(ui);
        break;

    case 'n':
        if (ui->focus == FOCUS_JOURNAL && ui->page_search_term &&
            ui->page_search_total > 0) {
            /* Jump to next search match */
            ui->page_search_current = (ui->page_search_current + 1) %
                                       ui->page_search_total;
            int target = ui->page_search_lines[ui->page_search_current];
            int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
            int half = vis / 2;
            int scroll = target - half;
            int max_scroll = (ui->doc ? ui->doc->total_lines : 0) - vis;
            if (max_scroll < 0) max_scroll = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > max_scroll) scroll = max_scroll;
            ui->scroll_y = scroll;
            ui->user_scrolled = 1;
            ui->dirty = 1;
            break;
        }
        /* fall through to typing in query */
        goto handle_default;

    case 'N':
        if (ui->focus == FOCUS_JOURNAL && ui->page_search_term &&
            ui->page_search_total > 0) {
            /* Jump to previous search match */
            ui->page_search_current = (ui->page_search_current - 1 +
                                        ui->page_search_total) %
                                       ui->page_search_total;
            int target = ui->page_search_lines[ui->page_search_current];
            int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
            int half = vis / 2;
            int scroll = target - half;
            int max_scroll = (ui->doc ? ui->doc->total_lines : 0) - vis;
            if (max_scroll < 0) max_scroll = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > max_scroll) scroll = max_scroll;
            ui->scroll_y = scroll;
            ui->user_scrolled = 1;
            ui->dirty = 1;
            break;
        }
        /* fall through to typing in query */
        goto handle_default;

    case 'c':
        if (ui->focus == FOCUS_JOURNAL) {
            ui_state_toggle_preview(ui);
            break;
        }
        /* fall through to typing in query */
        goto handle_default;

    default:
    handle_default:
        if (ui->focus == FOCUS_QUERY && ch >= 32 && ch != 127) {
            if (paste_mode) {
                /* During bracketed paste with nodelay(TRUE), getch() may
                 * return raw UTF-8 bytes (0x80-0xFF) instead of assembled
                 * codepoints. Store bytes as-is; only encode for ch >= 0x100. */
                char utf8[4];
                int nb;
                if (ch < 0x100) {
                    /* ASCII or raw UTF-8 byte: store as-is */
                    utf8[0] = (char)ch; nb = 1;
                } else if (ch < 0x800) {
                    utf8[0] = (char)(0xC0 | (ch >> 6));
                    utf8[1] = (char)(0x80 | (ch & 0x3F)); nb = 2;
                } else if (ch < 0x10000) {
                    utf8[0] = (char)(0xE0 | (ch >> 12));
                    utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    utf8[2] = (char)(0x80 | (ch & 0x3F)); nb = 3;
                } else if (ch < 0x110000) {
                    utf8[0] = (char)(0xF0 | (ch >> 18));
                    utf8[1] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    utf8[2] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    utf8[3] = (char)(0x80 | (ch & 0x3F)); nb = 4;
                } else {
                    break;  /* invalid codepoint */
                }
                if (paste_len + nb < PASTE_BUF_CAP) {
                    memcpy(paste_buf + paste_len, utf8, (size_t)nb);
                    paste_len += nb;
                }
            } else {
                ui_state_input_char(ui, ch);
            }
        }
        break;
    }

    /* ── Live search: detect /? prefix and trigger search ── */
    if (ui->focus == FOCUS_QUERY && !paste_mode &&
        ui->input_len >= 2 && ui->input_buffer[0] == '/' && ui->input_buffer[1] == '?') {
        if (ui->input_len >= 5) {
            /* Have at least 3 chars after "/?": trigger search */
            ui->input_buffer[ui->input_len] = '\0';
            ui_state_search(ui, ui->input_buffer + 2);
            /* Focus stays on query pane so user can refine search.
             * Tab switches to main pane to navigate results. */
        } else if (ui->search_active) {
            /* Query too short — clear search results */
            ui_state_search(ui, NULL);
        }
    } else if (ui->search_active && ui->focus == FOCUS_QUERY) {
        /* Input no longer starts with /? — clear search */
        ui_state_search(ui, NULL);
    }

    /* ── In-page search: detect ? prefix and trigger highlighting ── */
    if (ui->focus == FOCUS_QUERY && !paste_mode &&
        ui->input_len >= 1 && ui->input_buffer[0] == '?' &&
        !(ui->input_len >= 2 && ui->input_buffer[1] == '/')) {
        if (ui->input_len >= 4) {
            /* Have at least 3 chars after "?": set page search term */
            ui->input_buffer[ui->input_len] = '\0';
            free(ui->page_search_term);
            ui->page_search_term = strdup(ui->input_buffer + 1);
            ui->dirty = 1;
        } else {
            /* Query too short — clear page search */
            free(ui->page_search_term);
            ui->page_search_term = NULL;
            ui->page_search_total = 0;
            ui->dirty = 1;
        }
    } else if (ui->page_search_term && ui->focus == FOCUS_QUERY &&
               !(ui->input_len >= 1 && ui->input_buffer[0] == '?')) {
        /* Input no longer starts with ? — clear page search */
        free(ui->page_search_term);
        ui->page_search_term = NULL;
        ui->page_search_total = 0;
        ui->dirty = 1;
    }

paste_done:
    pthread_mutex_unlock(&ui->mtx);
    return 1;
}

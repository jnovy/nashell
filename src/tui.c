#include "tui.h"
#include "md_render.h"
#include "str.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>

/* ── Windows ─────────────────────────────────────────── */

int g_tui_active = 0;  /* set by tui_init(), cleared by tui_shutdown() */

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

    true_color_available = 1;
}

/* ── Init / Shutdown ─────────────────────────────────── */

void tui_init(void) {
    setlocale(LC_ALL, "");
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

    g_tui_active = 1;
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
        /* Single-line paste: insert directly into input buffer */
        for (int i = 0; i < paste_len; i++) {
            int ch = (unsigned char)paste_buf[i];
            if (ch >= 32 && ch < 127)
                ui_state_input_char(ui, ch);
        }
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
    g_tui_active = 0;

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

/* Buffer-aware: counts display lines considering '\n' characters. */
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
        int line_len = pos - line_start;

        /* First row width depends on whether this is the first logical line */
        int frw = (logical_line == 0) ? first_w : cont_w;
        total_rows += wrap_rows_for_segment(line_len, frw, cont_w);

        if (pos < buf_len) pos++;  /* skip '\n' */
        else break;
        logical_line++;
    }

    return total_rows > 0 ? total_rows : 1;
}

/* Buffer-aware cursor→rowcol: handles '\n' in the input buffer. */
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
        int line_len = pos - line_start;

        /* Is the cursor within this logical line? */
        int cursor_offset = cursor_pos - line_start;
        if (cursor_pos >= line_start &&
            (cursor_pos < pos || (cursor_pos == pos && (pos >= buf_len || buf[pos] == '\n')))) {
            /* Cursor is in this logical line */
            int frw = (logical_line == 0) ? first_w : cont_w;
            if (cursor_offset <= frw) {
                *out_row = display_row;
                *out_col = (logical_line == 0)
                           ? INPUT_PROMPT_W + cursor_offset
                           : cursor_offset;
            } else {
                int rem = cursor_offset - frw;
                *out_row = display_row + 1 + rem / cont_w;
                *out_col = rem % cont_w;
            }
            return;
        }

        /* Advance display_row by the number of wrapped rows for this line */
        int frw = (logical_line == 0) ? first_w : cont_w;
        display_row += wrap_rows_for_segment(line_len, frw, cont_w);

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

/* ── Render main pane (MD document) ──────────────────── */

static void render_main(ui_state_t *ui) {
    int rows = getmaxy(win_main);
    ui->visible_rows = rows;  /* tell ui_state how tall the main pane is */
    int cols = getmaxx(win_main);
    (void)rows;

    werase(win_main);

    if (ui->doc) {
        int focus = (ui->focus == FOCUS_JOURNAL);
        md_render(win_main, ui->doc, ui->scroll_y, ui->scroll_x,
                 ui->cursor_link, focus);
    } else {
        wattron(win_main, COLOR_PAIR(C_DIM));
        mvwaddstr(win_main, 0, 0, "  Loading...");
        wattroff(win_main, COLOR_PAIR(C_DIM));
    }

    wnoutrefresh(win_main);
    (void)cols;
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
    /* Pad from current cursor position to end of row */
    {
        int cur_x = getcurx(win);
        
        for (int c = cur_x; c < cols; c++) {
            mvwaddch(win, row, c, ' ' | attr);
        }
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
        int ctx_pct = (int)(100.0 * ui->context_used / ui->context_size);
        slen += snprintf(status_line + slen, sizeof(status_line) - slen,
                         " │ ctx %d%%", ctx_pct);
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

            /* Render first row of this logical line */
            {
                char row_buf[1024];
                int rlen = 0;
                if (is_first_logical) {
                    rlen += snprintf(row_buf + rlen, sizeof(row_buf) - rlen, "> ");
                }
                int chunk = line_len < frw ? line_len : frw;
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
                    int wchunk = line_len - seg_pos;
                    if (wchunk > cont_w) wchunk = cont_w;
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
        } else if (ch >= 32 && ch < 127) {
            if (paste_len < PASTE_BUF_CAP - 1)
                paste_buf[paste_len++] = (char)ch;
            goto paste_done;
        }
        /* else: ESC, special keys — fall through to normal handling
         * (needed to detect ESC[201~ paste-end sequence) */
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
                 * Simple approach: try subtracting the current row's width. */
                int cur_w = (crow == 0) ? fw : cw;
                int new_pos = ui->cursor_pos - cur_w;
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
                        int above_len = nl_pos - above_start;
                        /* Try to land at same column offset */
                        new_pos = above_start + (ccol < above_len ? ccol : above_len);
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

                /* Check if there's a '\n' between cursor and cursor+cur_w */
                int scan_end = ui->cursor_pos + cur_w;
                if (scan_end > ui->input_len) scan_end = ui->input_len;
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
                    int next_len = next_end - next_start;
                    new_pos = next_start + (ccol < next_len ? ccol : next_len);
                } else {
                    new_pos = ui->cursor_pos + cur_w;
                }
                if (new_pos > ui->input_len) new_pos = ui->input_len;
                ui->cursor_pos = new_pos;
                ui->dirty = 1;
            } else {
                /* On last display row — load next history or do nothing */
                if (ui->history_count > 0 && ui->history_idx < ui->history_count - 1) {
                    ui->history_idx++;
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
            /* Plain Escape — navigate back */
            if (ui->focus == FOCUS_JOURNAL) {
                ui_state_back(ui);
            }
        }
        /* else: some other Alt+key combo, ignore */
        break;
    }

    case ' ':  /* Space — pause react loop when running */
        if (ui->focus == FOCUS_JOURNAL &&
            ui->status == STATUS_RUNNING && ui->pause_flag) {
            *ui->pause_flag = 1;
            ui_state_set_status(ui, STATUS_READY, "Pausing after current step...");
            ui->dirty = 1;
            break;
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
        if (ui->focus == FOCUS_QUERY) ui_state_input_home(ui);
        break;

    case KEY_END:
        if (ui->focus == FOCUS_QUERY) ui_state_input_end(ui);
        break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (ui->focus == FOCUS_QUERY) ui_state_input_backspace(ui);
        break;

    case KEY_DC:
        if (ui->focus == FOCUS_QUERY) ui_state_input_delete(ui);
        break;

    case 'c':
        if (ui->focus == FOCUS_JOURNAL) {
            ui_state_toggle_preview(ui);
            break;
        }
        /* fall through to typing in query */
        goto handle_default;

    default:
    handle_default:
        if (ui->focus == FOCUS_QUERY && ch >= 32 && ch < 127) {
            if (paste_mode) {
                /* During bracketed paste, accumulate into paste buffer */
                if (paste_len < PASTE_BUF_CAP - 1)
                    paste_buf[paste_len++] = (char)ch;
            } else {
                ui_state_input_char(ui, ch);
            }
        }
        break;
    }

paste_done:
    pthread_mutex_unlock(&ui->mtx);
    return 1;
}

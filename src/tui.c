#include "tui.h"
#include "md_render.h"
#include "str.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>

/* ── Windows ─────────────────────────────────────────── */

static WINDOW *win_main   = NULL;   /* top pane: MD rendered content */
static WINDOW *win_bottom = NULL;   /* bottom pane: status + input */
static int main_height = 0;
static int bottom_height = 0;

/* ── Colors ──────────────────────────────────────────── */

#define C_NORMAL    0
#define C_SELECTED  1
#define C_FAILED    2
#define C_SUCCESS   3
#define C_STATUS    4
#define C_DIM       5
#define C_FOCUS     6
#define C_STREAM    7

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
}

void tui_shutdown(void) {
    if (win_main)   delwin(win_main);
    if (win_bottom) delwin(win_bottom);
    win_main = win_bottom = NULL;
    endwin();
}

/* ── Resize handling ─────────────────────────────────── */

/* Calculate number of wrapped display lines for input text.
 * First line has prompt "nash> " (6 chars), continuation lines use full width. */
static int calc_input_lines(int input_len, int cols) {
    if (input_len <= 0) return 1;
    int first_w = cols - 6;  /* first line width (after prompt) */
    if (first_w <= 0) first_w = 1;
    if (input_len <= first_w) return 1;
    int remaining = input_len - first_w;
    int cont_w = cols > 0 ? cols : 1;
    return 1 + (remaining + cont_w - 1) / cont_w;
}

/* Convert linear cursor_pos to (row, col) in the wrapped display.
 * Row 0 starts at col 6 (after prompt), subsequent rows at col 0. */
static void cursor_to_rowcol(int cursor_pos, int cols,
                              int *out_row, int *out_col) {
    int first_w = cols - 6;
    if (first_w <= 0) first_w = 1;
    if (cursor_pos <= first_w) {
        *out_row = 0;
        *out_col = 6 + cursor_pos;
    } else {
        int remaining = cursor_pos - first_w;
        int cont_w = cols > 0 ? cols : 1;
        *out_row = 1 + remaining / cont_w;
        *out_col = remaining % cont_w;
    }
}

/* Convert cursor position to row/col given first-line and continuation widths */
static void input_pos_to_rowcol(int pos, int first_w, int cont_w,
                                 int *out_row, int *out_col) {
    if (first_w <= 0) first_w = 1;
    if (cont_w <= 0) cont_w = 1;
    if (pos <= first_w) {
        *out_row = 0;
        *out_col = pos;  /* 0-based within content area; caller adds prompt offset */
    } else {
        int remaining = pos - first_w;
        *out_row = 1 + remaining / cont_w;
        *out_col = remaining % cont_w;
    }
}

/* Count total wrapped lines for input of given length */
static int input_wrapped_lines(int input_len, int first_w, int cont_w) {
    if (first_w <= 0) first_w = 1;
    if (cont_w <= 0) cont_w = 1;
    if (input_len <= first_w) return 1;
    int remaining = input_len - first_w;
    return 1 + (remaining + cont_w - 1) / cont_w;
}

static void resize_panes_with_input(int input_len, int input_cursor);

static void resize_panes_with_input(int input_len, int input_cursor) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Dynamic bottom height: 1 (status) + wrapped input lines */
    int input_lines = calc_input_lines(input_len, cols);
    /* Also ensure cursor row is visible */
    int cursor_row, cursor_col;
    cursor_to_rowcol(input_cursor, cols, &cursor_row, &cursor_col);
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

/* ── Render bottom pane (status + input) ─────────────── */

static void render_bottom(ui_state_t *ui) {
    int cols = getmaxx(win_bottom);

    werase(win_bottom);

    /* Row 0: separator + status */
    mvwhline(win_bottom, 0, 0, ACS_HLINE, cols);

    if (ui->status_text) {
        const char *indicator;
        int pair;
        switch (ui->status) {
            case STATUS_RUNNING:        indicator = "* "; pair = C_FOCUS; break;
            case STATUS_AWAITING_INPUT: indicator = "? "; pair = C_FOCUS; break;
            case STATUS_DONE:           indicator = "  "; pair = C_SELECTED; break;
            case STATUS_ERROR:          indicator = "! "; pair = C_FAILED; break;
            default:                    indicator = "  "; pair = C_DIM; break;
        }
        (void)cols;  /* status is now left-aligned */
        wattron(win_bottom, COLOR_PAIR(pair));
        mvwprintw(win_bottom, 0, 1, " %s%s ", indicator, ui->status_text);
        wattroff(win_bottom, COLOR_PAIR(pair));
    }

    /* Row 1: input prompt */
    wattron(win_bottom, COLOR_PAIR(C_SELECTED) | A_BOLD);
    mvwaddstr(win_bottom, 1, 0, "nash> ");
    wattroff(win_bottom, COLOR_PAIR(C_SELECTED) | A_BOLD);

    if (ui->input_buffer && ui->input_len > 0) {
        int first_w = cols - 6;  /* first line width (after "nash> ") */
        if (first_w < 1) first_w = 1;
        int cont_w = cols;       /* continuation line width */
        if (cont_w < 1) cont_w = 1;

        /* Render text across wrapped lines */
        int pos = 0;
        int row = 1;  /* first input row */
        int bh = getmaxy(win_bottom);
        while (pos < ui->input_len && row < bh) {
            int line_w = (row == 1) ? first_w : cont_w;
            int remain = ui->input_len - pos;
            int show = remain < line_w ? remain : line_w;
            int col_start = (row == 1) ? 6 : 0;
            mvwaddnstr(win_bottom, row, col_start,
                       ui->input_buffer + pos, show);
            pos += show;
            row++;
        }
    }

    /* Position cursor in wrapped multi-line input */
    if (ui->focus == FOCUS_QUERY) {
        int first_w = cols - 6;
        if (first_w < 1) first_w = 1;
        int cont_w = cols;
        if (cont_w < 1) cont_w = 1;
        int crow, ccol;
        input_pos_to_rowcol(ui->cursor_pos, first_w, cont_w, &crow, &ccol);
        int abs_row = 1 + crow;  /* row 0 is status bar */
        int abs_col = (crow == 0) ? 6 + ccol : ccol;
        int bh = getmaxy(win_bottom);
        if (abs_row >= bh) abs_row = bh - 1;
        if (abs_col >= cols) abs_col = cols - 1;
        wmove(win_bottom, abs_row, abs_col);
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

    resize_panes_with_input(ui->input_len, ui->cursor_pos);

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

    switch (ch) {
    case '\t':
    case KEY_BTAB:
        ui_state_tab(ui);
        break;

    case KEY_UP:
        if (ui->focus == FOCUS_QUERY) {
            /* Navigate up within wrapped input lines */
            int cols_now = getmaxx(stdscr);
            int fw = cols_now - 6; if (fw < 1) fw = 1;
            int cw = cols_now;     if (cw < 1) cw = 1;
            int crow, ccol;
            input_pos_to_rowcol(ui->cursor_pos, fw, cw, &crow, &ccol);
            if (crow > 0) {
                /* Move up one wrapped line.
                 * When moving to row 0, clamp to first-line width (fw)
                 * since row 0 is shorter due to "nash> " prompt (6 chars). */
                int cur_w = (crow == 0) ? fw : cw;  /* width of current row */
                int new_pos = ui->cursor_pos - cur_w;
                if (new_pos < 0) new_pos = 0;
                /* Clamp to target row's width when landing on row 0 */
                if (crow == 1 && new_pos > fw) new_pos = fw;
                ui->cursor_pos = new_pos;
                ui->dirty = 1;
            } else {
                /* On first line — load previous history */
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
            /* Navigate down within wrapped input lines */
            int cols_now = getmaxx(stdscr);
            int fw = cols_now - 6; if (fw < 1) fw = 1;
            int cw = cols_now;     if (cw < 1) cw = 1;
            int crow, ccol;
            input_pos_to_rowcol(ui->cursor_pos, fw, cw, &crow, &ccol);
            int total_rows = input_wrapped_lines(ui->input_len, fw, cw);
            if (crow < total_rows - 1) {
                /* Move down one wrapped line */
                int cur_w = (crow == 0) ? fw : cw;
                int new_pos = ui->cursor_pos + cur_w;
                if (new_pos > ui->input_len) new_pos = ui->input_len;
                ui->cursor_pos = new_pos;
                ui->dirty = 1;
            } else {
                /* On last line — load next history or clear */
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
                /* On last line with no more history — do nothing */
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
        } else if (ui->focus == FOCUS_QUERY && ui->input_len > 0) {
            /* Submit query — save to history first */
            *out_query = strndup(ui->input_buffer, ui->input_len);
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
            ui->input_buffer[0] = '\0';
            ui->input_len = 0;
            ui->cursor_pos = 0;
            ui->dirty = 1;
        }
        break;

    case 27:  /* Escape */
        if (ui->focus == FOCUS_JOURNAL) {
            ui_state_back(ui);
        }
        break;

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

    case 'q':
        if (ui->focus == FOCUS_JOURNAL) {
            pthread_mutex_unlock(&ui->mtx);
            return -1;  /* quit */
        }
        /* fall through to typing */
        /* FALLTHROUGH */

    default:
        if (ui->focus == FOCUS_QUERY && ch >= 32 && ch < 127) {
            ui_state_input_char(ui, ch);
        }
        break;
    }

    pthread_mutex_unlock(&ui->mtx);
    return 1;
}

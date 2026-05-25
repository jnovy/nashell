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

static void resize_panes(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    bottom_height = 2;
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
        md_render(win_main, ui->doc, ui->scroll_y, ui->cursor_link, focus);
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
        int slen = (int)strlen(ui->status_text) + 4;
        int sx = cols - slen - 1;
        if (sx < 0) sx = 0;
        wattron(win_bottom, COLOR_PAIR(pair));
        mvwprintw(win_bottom, 0, sx, " %s%s ", indicator, ui->status_text);
        wattroff(win_bottom, COLOR_PAIR(pair));
    }

    /* Row 1: input prompt */
    wattron(win_bottom, COLOR_PAIR(C_SELECTED) | A_BOLD);
    mvwaddstr(win_bottom, 1, 0, "nash> ");
    wattroff(win_bottom, COLOR_PAIR(C_SELECTED) | A_BOLD);

    if (ui->input_buffer && ui->input_len > 0) {
        int maxw = cols - 6;
        int start = 0;
        if (ui->cursor_pos > maxw)
            start = ui->cursor_pos - maxw;
        int show = ui->input_len - start;
        if (show > maxw) show = maxw;
        waddnstr(win_bottom, ui->input_buffer + start, show);
    }

    /* Position cursor */
    if (ui->focus == FOCUS_QUERY) {
        int cx = 6 + ui->cursor_pos;
        int maxw = cols - 6;
        if (ui->cursor_pos > maxw)
            cx = 6 + maxw;
        wmove(win_bottom, 1, cx < cols ? cx : cols - 1);
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

    resize_panes();

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
        ui_state_up(ui);
        break;

    case KEY_DOWN:
        ui_state_down(ui);
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
            /* Submit query */
            *out_query = strndup(ui->input_buffer, ui->input_len);
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
        break;

    case KEY_RIGHT:
        if (ui->focus == FOCUS_QUERY) ui_state_input_right(ui);
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

#include "tui.h"
#include "str.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Windows ─────────────────────────────────────────── */

static WINDOW *win_journal = NULL;   /* top pane: journal navigation */
static WINDOW *win_query   = NULL;   /* bottom pane: status + input */
static int journal_height = 0;
static int query_height = 0;

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
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);  /* non-blocking input */
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_FAILED,   COLOR_RED,   -1);
        init_pair(C_SUCCESS,  COLOR_GREEN,  -1);
        init_pair(C_STATUS,   COLOR_BLACK, COLOR_WHITE);
        init_pair(C_DIM,      COLOR_WHITE,  -1);  /* actually gray */
        init_pair(C_FOCUS,    COLOR_YELLOW, -1);
        init_pair(C_STREAM,   COLOR_CYAN,   -1);
    }

    /* Split: journal gets 70% of height, query gets 30% (min 4 lines) */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    query_height = rows * 30 / 100;
    if (query_height < 4) query_height = 4;
    journal_height = rows - query_height;

    win_journal = newwin(journal_height, cols, 0, 0);
    win_query   = newwin(query_height, cols, journal_height, 0);
    scrollok(win_journal, FALSE);
    scrollok(win_query, FALSE);
    keypad(win_journal, TRUE);
    keypad(win_query, TRUE);
}

void tui_shutdown(void) {
    if (win_journal) delwin(win_journal);
    if (win_query)   delwin(win_query);
    win_journal = win_query = NULL;
    endwin();
}

/* ── Helpers ─────────────────────────────────────────── */

static void format_timestamp(double ts, char *buf, size_t sz) {
    time_t t = (time_t)ts;
    struct tm *tm = localtime(&t);
    if (tm) {
        strftime(buf, sz, "%H:%M:%S", tm);
    } else {
        snprintf(buf, sz, "??:??:??");
    }
}

static void draw_border(WINDOW *w, int focused, const char *title) {
    if (focused) {
        wattron(w, COLOR_PAIR(C_FOCUS));
        box(w, 0, 0);
        wattroff(w, COLOR_PAIR(C_FOCUS));
    } else {
        box(w, 0, 0);
    }
    if (title) {
        int cols = getmaxx(w);
        int tlen = (int)strlen(title);
        if (tlen > cols - 4) tlen = cols - 4;
        mvwprintw(w, 0, 2, " %.*s ", tlen, title);
    }
}

/* ── Render journal pane ─────────────────────────────── */

static void render_journal(ui_state_t *ui) {
    werase(win_journal);
    int rows = getmaxy(win_journal) - 2;  /* minus border */
    int cols = getmaxx(win_journal) - 2;

    int focused = (ui->focus == FOCUS_JOURNAL);

    switch (ui->view) {
    case VIEW_QUERIES: {
        draw_border(win_journal, focused, "Journal — Queries");

        /* Scrollable query list */
        int start = 0;
        if (ui->selected_query >= rows) start = ui->selected_query - rows + 1;

        for (int i = 0; i < rows && (start + i) < ui->query_count; i++) {
            int idx = start + i;
            ui_query_t *q = &ui->queries[idx];
            int selected = (idx == ui->selected_query && focused);

            char ts_buf[16];
            format_timestamp(q->timestamp, ts_buf, sizeof(ts_buf));

            if (selected) wattron(win_journal, COLOR_PAIR(C_SELECTED));

            /* Format: "HH:MM:SS [R0] query text... (N steps, M failed)" */
            char line[512];
            if (q->failed_count > 0) {
                snprintf(line, sizeof(line), "%s [R%d] %.60s (%d steps, %d failed)",
                         ts_buf, q->react_loop, q->query_text,
                         q->step_count, q->failed_count);
            } else {
                snprintf(line, sizeof(line), "%s [R%d] %.60s (%d steps)",
                         ts_buf, q->react_loop, q->query_text, q->step_count);
            }
            mvwprintw(win_journal, i + 1, 1, "%-*.*s", cols, cols, line);

            if (selected) wattroff(win_journal, COLOR_PAIR(C_SELECTED));
        }

        if (ui->query_count == 0) {
            wattron(win_journal, COLOR_PAIR(C_DIM));
            mvwprintw(win_journal, 1, 1, "(no queries yet — type below and press Enter)");
            wattroff(win_journal, COLOR_PAIR(C_DIM));
        }
        break;
    }

    case VIEW_MANIFEST: {
        /* Show query header + step list */
        char title[128];
        if (ui->selected_query >= 0 && ui->selected_query < ui->query_count) {
            snprintf(title, sizeof(title), "Journal — R%d Steps (Esc=back)",
                     ui->queries[ui->selected_query].react_loop);
        } else {
            snprintf(title, sizeof(title), "Journal — Steps (Esc=back)");
        }
        draw_border(win_journal, focused, title);

        int start = 0;
        if (ui->selected_step >= rows) start = ui->selected_step - rows + 1;

        for (int i = 0; i < rows && (start + i) < ui->step_count; i++) {
            int idx = start + i;
            ui_step_t *s = &ui->steps[idx];
            int selected = (idx == ui->selected_step && focused);

            if (selected) wattron(win_journal, COLOR_PAIR(C_SELECTED));

            const char *mark = s->failed ? "✗" : "✓";
            int mark_color = s->failed ? C_FAILED : C_SUCCESS;

            /* "✓ R0S3: shell_exec "ls -la" → 1082 chars" */
            char line[512];
            if (s->failed) {
                snprintf(line, sizeof(line), "%s %s: %s \"%.*s\"",
                         mark, s->ref, s->tool, 60, s->description);
            } else {
                snprintf(line, sizeof(line), "%s %s: %s \"%.*s\" → %d chars",
                         mark, s->ref, s->tool, 50, s->description, s->size);
            }

            if (!selected) wattron(win_journal, COLOR_PAIR(mark_color));
            mvwprintw(win_journal, i + 1, 1, "%-*.*s", cols, cols, line);
            if (!selected) wattroff(win_journal, COLOR_PAIR(mark_color));
            if (selected) wattroff(win_journal, COLOR_PAIR(C_SELECTED));
        }
        break;
    }

    case VIEW_DETAIL: {
        char title[128];
        if (ui->selected_step >= 0 && ui->selected_step < ui->step_count) {
            snprintf(title, sizeof(title), "Detail — %s (Esc=back, PgUp/PgDn)",
                     ui->steps[ui->selected_step].ref);
        } else {
            snprintf(title, sizeof(title), "Detail (Esc=back)");
        }
        draw_border(win_journal, focused, title);

        if (ui->detail_content) {
            /* Render content with scroll offset */
            const char *p = ui->detail_content;
            int line_num = 0;
            while (*p && line_num < ui->detail_scroll) {
                if (*p == '\n') line_num++;
                p++;
            }
            for (int i = 0; i < rows && *p; i++) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                if (len > cols) len = cols;
                mvwprintw(win_journal, i + 1, 1, "%.*s", len, p);
                p = eol ? eol + 1 : p + len;
            }
        } else {
            wattron(win_journal, COLOR_PAIR(C_DIM));
            mvwprintw(win_journal, 1, 1, "(no content available)");
            wattroff(win_journal, COLOR_PAIR(C_DIM));
        }
        break;
    }
    }

    wrefresh(win_journal);
}

/* ── Render query pane ───────────────────────────────── */

static void render_query(ui_state_t *ui) {
    werase(win_query);
    int rows = getmaxy(win_query) - 2;
    int cols = getmaxx(win_query) - 2;
    int focused = (ui->focus == FOCUS_QUERY);

    draw_border(win_query, focused, "Query");

    /* Line 1: Status bar */
    wattron(win_query, COLOR_PAIR(C_STATUS));
    char status_line[512];
    const char *status_icon =
        ui->status == STATUS_READY ? "●" :
        ui->status == STATUS_RUNNING ? "▶" :
        ui->status == STATUS_AWAITING_INPUT ? "?" :
        ui->status == STATUS_DONE ? "✓" : "✗";
    snprintf(status_line, sizeof(status_line), " %s %s",
             status_icon, ui->status_text ? ui->status_text : "");
    mvwprintw(win_query, 1, 1, "%-*.*s", cols, cols, status_line);
    wattroff(win_query, COLOR_PAIR(C_STATUS));

    /* Line 2: Streaming tokens (if running) */
    if (ui->stream_len > 0 && rows > 2) {
        wattron(win_query, COLOR_PAIR(C_STREAM));
        /* Show last N chars of streaming tokens that fit */
        int avail = cols;
        int start_offset = 0;
        if (ui->stream_len > avail)
            start_offset = ui->stream_len - avail;
        /* Find last line in stream */
        const char *last_nl = strrchr(ui->stream_tokens + start_offset, '\n');
        const char *show = last_nl ? last_nl + 1 : ui->stream_tokens + start_offset;
        int show_len = (int)strlen(show);
        if (show_len > cols) show_len = cols;
        mvwprintw(win_query, 2, 1, "%.*s", show_len, show);
        wattroff(win_query, COLOR_PAIR(C_STREAM));
    }

    /* Last line: Input prompt */
    int input_row = getmaxy(win_query) - 2;
    if (input_row < 2) input_row = 2;

    mvwprintw(win_query, input_row, 1, "nash> ");
    int prompt_len = 6;

    if (ui->input_buffer) {
        int avail = cols - prompt_len;
        int show_start = 0;
        if (ui->cursor_pos > avail)
            show_start = ui->cursor_pos - avail + 1;
        int show_len = ui->input_len - show_start;
        if (show_len > avail) show_len = avail;
        mvwprintw(win_query, input_row, 1 + prompt_len,
                  "%.*s", show_len, ui->input_buffer + show_start);

        /* Position cursor */
        if (focused) {
            int cx = 1 + prompt_len + ui->cursor_pos - show_start;
            wmove(win_query, input_row, cx);
        }
    }

    wrefresh(win_query);
}

/* ── Main render ─────────────────────────────────────── */

void tui_render(ui_state_t *ui) {
    if (!ui->dirty) return;

    /* Handle terminal resize */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int new_qh = rows * 30 / 100;
    if (new_qh < 4) new_qh = 4;
    int new_jh = rows - new_qh;

    if (new_jh != journal_height || new_qh != query_height) {
        journal_height = new_jh;
        query_height = new_qh;
        wresize(win_journal, journal_height, cols);
        wresize(win_query, query_height, cols);
        mvwin(win_query, journal_height, 0);
    }

    render_journal(ui);
    render_query(ui);

    /* Position cursor in the active pane */
    if (ui->focus == FOCUS_QUERY) {
        int input_row = getmaxy(win_query) - 2;
        if (input_row < 2) input_row = 2;
        int cx = 1 + 6 + ui->cursor_pos;
        wmove(win_query, input_row, cx);
        wrefresh(win_query);
    }

    ui->dirty = 0;
    doupdate();
}

/* ── Input handling ──────────────────────────────────── */

int tui_input(ui_state_t *ui, char **out_query) {
    *out_query = NULL;

    int ch = getch();
    if (ch == ERR) return 0;  /* no input */

    /* Global keys */
    switch (ch) {
    case '\t':
    case KEY_BTAB:
        ui_state_tab(ui);
        return 1;

    case KEY_F(10):
    case 'q' - 'a' + 1:  /* Ctrl-Q */
        return -1;  /* quit */

    case KEY_RESIZE:
        ui->dirty = 1;
        return 1;
    }

    /* Focus-specific keys */
    if (ui->focus == FOCUS_JOURNAL) {
        switch (ch) {
        case KEY_UP:
        case 'k':
            ui_state_up(ui);
            return 1;
        case KEY_DOWN:
        case 'j':
            ui_state_down(ui);
            return 1;
        case '\n':
        case KEY_ENTER:
            ui_state_enter(ui);
            return 1;
        case 27:  /* Escape */
        case KEY_BACKSPACE:
        case 127:
            ui_state_back(ui);
            return 1;
        case KEY_PPAGE:
            ui_state_page_up(ui);
            return 1;
        case KEY_NPAGE:
            ui_state_page_down(ui);
            return 1;
        }
    } else {
        /* FOCUS_QUERY — text editing */
        switch (ch) {
        case '\n':
        case KEY_ENTER: {
            const char *text = ui_state_input_submit(ui);
            if (text) {
                *out_query = strdup(text);
                /* Clear input after submit */
                ui->input_buffer[0] = '\0';
                ui->input_len = 0;
                ui->cursor_pos = 0;
                ui->dirty = 1;
            }
            return 1;
        }
        case KEY_BACKSPACE:
        case 127:
        case 8:
            ui_state_input_backspace(ui);
            return 1;
        case KEY_DC:
            ui_state_input_delete(ui);
            return 1;
        case KEY_LEFT:
            ui_state_input_left(ui);
            return 1;
        case KEY_RIGHT:
            ui_state_input_right(ui);
            return 1;
        case KEY_HOME:
        case 1:  /* Ctrl-A */
            ui_state_input_home(ui);
            return 1;
        case KEY_END:
        case 5:  /* Ctrl-E */
            ui_state_input_end(ui);
            return 1;
        default:
            if (ch >= 32 && ch < 127) {
                ui_state_input_char(ui, ch);
                return 1;
            }
            break;
        }
    }

    return 0;
}

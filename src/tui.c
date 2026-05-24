#include "tui.h"
#include "str.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <unistd.h>

/* ── Windows ─────────────────────────────────────────── */

static WINDOW *win_main   = NULL;   /* top pane: banner + tree view */
static WINDOW *win_bottom = NULL;   /* bottom pane: content / status+input */
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

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    /* Bottom pane starts as 2 lines (status + input) */
    bottom_height = 2;
    main_height = rows - bottom_height;

    win_main   = newwin(main_height, cols, 0, 0);
    win_bottom = newwin(bottom_height, cols, main_height, 0);
    scrollok(win_main, FALSE);
    scrollok(win_bottom, FALSE);
    keypad(win_main, TRUE);
    keypad(win_bottom, TRUE);
}

void tui_shutdown(void) {
    if (win_main)   delwin(win_main);
    if (win_bottom) delwin(win_bottom);
    endwin();
}

/* ── Resize panes based on content ───────────────────── */

static void resize_panes(ui_state_t *ui) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Bottom pane sizing:
     * - Default: 2 lines (status + input)
     * - When browsing with content: up to half screen
     * - When content loaded: show content lines + 1 status line, capped at half */
    int want_bottom = 2;  /* minimum: status + input */

    if (ui->bottom_content && ui->bottom_lines > 0) {
        want_bottom = ui->bottom_lines + 1;  /* content + status */
        if (want_bottom > rows / 2) want_bottom = rows / 2;
        if (want_bottom < 2) want_bottom = 2;
    }

    if (want_bottom != bottom_height || rows - want_bottom != main_height) {
        bottom_height = want_bottom;
        main_height = rows - bottom_height;

        wresize(win_main, main_height, cols);
        mvwin(win_main, 0, 0);
        wresize(win_bottom, bottom_height, cols);
        mvwin(win_bottom, main_height, 0);
        ui->dirty = 1;
    }
}

/* ── Helper: count lines in string ───────────────────── */

static int count_str_lines(const char *s) {
    if (!s || !s[0]) return 0;
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n + 1;  /* last line may not end with \n */
}

/* ── Helper: read store content via session symlink ──── */

static char *read_ref_content(ui_state_t *ui, const char *ref, int max_bytes) {
    if (!ui || !ui->session_dir || !ref) return NULL;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", ui->session_dir, ref);
    FILE *f = fopen(path, "r");  /* symlink resolves automatically */
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (sz > max_bytes) sz = max_bytes;
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── Render main pane (banner + tree view) ───────────── */

static void render_main(ui_state_t *ui) {
    int rows = getmaxy(win_main);
    int cols = getmaxx(win_main);

    werase(win_main);

    int line = 0;

    /* Show banner if present */
    if (ui->banner && ui->banner[0]) {
        const char *p = ui->banner;
        while (*p && line < rows - 1) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > cols) len = cols;
            wattron(win_main, COLOR_PAIR(C_SUCCESS));
            mvwaddnstr(win_main, line, 0, p, len);
            wattroff(win_main, COLOR_PAIR(C_SUCCESS));
            line++;
            p = nl ? nl + 1 : p + strlen(p);
        }
        line++;  /* blank line after banner */
    }

    /* Tree view: queries with expandable steps */
    if (ui->query_count == 0 && line < rows) {
        wattron(win_main, COLOR_PAIR(C_DIM));
        mvwaddstr(win_main, line, 2, "(no queries yet - type below and press Enter)");
        wattroff(win_main, COLOR_PAIR(C_DIM));
    } else {
        /* Build flat list of visible items for cursor tracking */
        int item_idx = 0;  /* tracks position for cursor highlight */

        for (int qi = 0; qi < ui->query_count && line < rows; qi++) {
            ui_query_t *q = &ui->queries[qi];

            /* Format timestamp */
            char ts_buf[32] = "";
            if (q->timestamp > 0) {
                time_t t = (time_t)q->timestamp;
                struct tm *tm = localtime(&t);
                if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm);
            }

            /* Query line: ▸/▼ timestamp  query text */
            char qline[512];
            snprintf(qline, sizeof(qline), "  %s %s  %s",
                     q->expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb8",  /* ▼ or ▸ */
                     ts_buf,
                     q->query_text ? q->query_text : "(empty)");

            int is_selected = (ui->focus == FOCUS_JOURNAL &&
                               qi == ui->selected_query &&
                               ui->selected_step < 0);

            if (is_selected) wattron(win_main, A_REVERSE);
            mvwaddnstr(win_main, line, 0, qline, cols);
            /* Pad to full width for reverse video */
            int qlen = (int)strlen(qline);
            if (is_selected && qlen < cols) {
                for (int x = qlen; x < cols; x++)
                    mvwaddch(win_main, line, x, ' ');
            }
            if (is_selected) wattroff(win_main, A_REVERSE);
            line++;

            /* If expanded, show steps underneath */
            if (q->expanded && q->steps && q->step_count > 0) {
                for (int si = 0; si < q->step_count && line < rows; si++) {
                    ui_step_t *s = &q->steps[si];

                    char sline[512];
                    const char *mark = s->failed ? "\xe2\x9c\x97" : "\xe2\x9c\x93";  /* ✗ or ✓ */
                    if (s->failed) {
                        snprintf(sline, sizeof(sline), "      %s %s: %s \"%s\"",
                                 mark,
                                 s->ref ? s->ref : "?",
                                 s->tool ? s->tool : "?",
                                 s->description ? s->description : "");
                    } else {
                        snprintf(sline, sizeof(sline), "      %s %s: %s \"%s\" -> %d chars",
                                 mark,
                                 s->ref ? s->ref : "?",
                                 s->tool ? s->tool : "?",
                                 s->description ? s->description : "",
                                 s->size);
                    }

                    int step_selected = (ui->focus == FOCUS_JOURNAL &&
                                         qi == ui->selected_query &&
                                         si == ui->selected_step);

                    if (step_selected) wattron(win_main, A_REVERSE);
                    if (s->failed) wattron(win_main, COLOR_PAIR(C_FAILED));
                    else wattron(win_main, COLOR_PAIR(C_SUCCESS));

                    mvwaddnstr(win_main, line, 0, sline, cols);
                    int slen = (int)strlen(sline);
                    if (step_selected && slen < cols) {
                        for (int x = slen; x < cols; x++)
                            mvwaddch(win_main, line, x, ' ');
                    }

                    if (s->failed) wattroff(win_main, COLOR_PAIR(C_FAILED));
                    else wattroff(win_main, COLOR_PAIR(C_SUCCESS));
                    if (step_selected) wattroff(win_main, A_REVERSE);
                    line++;
                }
            }
        }
    }

    /* Streaming tokens during inference */
    if (ui->status == STATUS_RUNNING && ui->stream_tokens && ui->stream_len > 0) {
        line++;
        wattron(win_main, COLOR_PAIR(C_STREAM));
        const char *p = ui->stream_tokens;
        while (*p && line < rows) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > cols) len = cols;
            mvwaddnstr(win_main, line, 4, p, len);
            line++;
            p = nl ? nl + 1 : p + strlen(p);
        }
        wattroff(win_main, COLOR_PAIR(C_STREAM));
    }

    wnoutrefresh(win_main);
}

/* ── Render bottom pane (content + status + input) ───── */

static void render_bottom(ui_state_t *ui) {
    int rows = getmaxy(win_bottom);
    int cols = getmaxx(win_bottom);

    werase(win_bottom);

    /* Separator line at top of bottom pane */
    wattron(win_bottom, COLOR_PAIR(C_DIM));
    mvwhline(win_bottom, 0, 0, ACS_HLINE, cols);
    wattroff(win_bottom, COLOR_PAIR(C_DIM));

    /* Show actual content if loaded */
    if (ui->bottom_content && ui->bottom_content[0] && rows > 2) {
        const char *p = ui->bottom_content;
        /* Skip to scroll offset */
        for (int i = 0; i < ui->bottom_scroll && *p; i++) {
            const char *nl = strchr(p, '\n');
            if (nl) p = nl + 1; else break;
        }
        /* Render content lines */
        for (int line = 1; line < rows - 1 && *p; line++) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > cols) len = cols;
            mvwaddnstr(win_bottom, line, 0, p, len);
            p = nl ? nl + 1 : p + strlen(p);
        }
    }

    /* Status + input on last line */
    int last_line = rows - 1;

    /* Status indicator */
    const char *status_icon = "";
    int status_color = C_DIM;
    switch (ui->status) {
        case STATUS_READY:          status_icon = "Ready"; status_color = C_SUCCESS; break;
        case STATUS_RUNNING:        status_icon = "Running"; status_color = C_FOCUS; break;
        case STATUS_AWAITING_INPUT: status_icon = "Awaiting"; status_color = C_FOCUS; break;
        case STATUS_DONE:           status_icon = "Done"; status_color = C_SUCCESS; break;
        case STATUS_ERROR:          status_icon = "Error"; status_color = C_FAILED; break;
    }

    wattron(win_bottom, COLOR_PAIR(status_color));
    mvwprintw(win_bottom, last_line, 0, "%s", status_icon);
    wattroff(win_bottom, COLOR_PAIR(status_color));

    /* Show status text if running */
    if (ui->status_text && ui->status == STATUS_RUNNING) {
        wattron(win_bottom, COLOR_PAIR(C_DIM));
        wprintw(win_bottom, " %s", ui->status_text);
        wattroff(win_bottom, COLOR_PAIR(C_DIM));
    }

    /* Input prompt */
    int prompt_x = 0;
    if (ui->focus == FOCUS_QUERY) {
        /* Show input on the last line after status */
        prompt_x = getcurx(win_bottom) + 2;
        mvwaddstr(win_bottom, last_line, prompt_x, "nash> ");
        prompt_x += 6;
        if (ui->input_buffer && ui->input_len > 0) {
            waddnstr(win_bottom, ui->input_buffer, ui->input_len);
        }
        /* Position cursor */
        wmove(win_bottom, last_line, prompt_x + ui->cursor_pos);
    }

    wnoutrefresh(win_bottom);
}

/* ── Public API ──────────────────────────────────────── */

void tui_render(ui_state_t *ui) {
    if (!ui) return;

    pthread_mutex_lock(&ui->mtx);
    if (!ui->dirty) {
        pthread_mutex_unlock(&ui->mtx);
        return;
    }

    resize_panes(ui);
    render_main(ui);
    render_bottom(ui);
    doupdate();
    ui->dirty = 0;

    pthread_mutex_unlock(&ui->mtx);
}

int tui_input(ui_state_t *ui, char **out_query) {
    if (!ui) return 0;
    *out_query = NULL;

    int ch = getch();
    if (ch == ERR) return 0;  /* no input */

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

    case '\n':
    case KEY_ENTER:
        if (ui->focus == FOCUS_QUERY && ui->input_len > 0) {
            /* Submit query */
            *out_query = strndup(ui->input_buffer, ui->input_len);
            ui->input_len = 0;
            ui->cursor_pos = 0;
            memset(ui->input_buffer, 0, ui->input_cap);
        } else if (ui->focus == FOCUS_JOURNAL) {
            ui_state_enter(ui);
        }
        ui->dirty = 1;
        break;

    case 27:  /* Escape */
        ui_state_back(ui);
        break;

    case KEY_PPAGE:
        ui_state_page_up(ui);
        break;

    case KEY_NPAGE:
        ui_state_page_down(ui);
        break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_backspace(ui);
        }
        break;

    case KEY_DC:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_delete(ui);
        }
        break;

    case KEY_LEFT:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_left(ui);
        }
        break;

    case KEY_RIGHT:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_right(ui);
        }
        break;

    case KEY_HOME:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_home(ui);
        }
        break;

    case KEY_END:
        if (ui->focus == FOCUS_QUERY) {
            ui_state_input_end(ui);
        }
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

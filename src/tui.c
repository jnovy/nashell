#include "tui.h"
#include <time.h>
#include "str.h"
#include <ncurses.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>

/* ── globals ──────────────────────────────────────── */

static WINDOW *win_main   = NULL;   /* top pane: banner + journal */
static WINDOW *win_bottom = NULL;   /* bottom pane: status + input / preview */
static int     term_rows, term_cols;

/* ── init / shutdown ──────────────────────────────── */

void tui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);   /* non-blocking getch */
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN,  -1);   /* success */
        init_pair(2, COLOR_RED,    -1);   /* failure */
        init_pair(3, COLOR_CYAN,   -1);   /* info/thinking */
        init_pair(4, COLOR_YELLOW, -1);   /* status */
        init_pair(5, COLOR_WHITE,  -1);   /* normal */
        init_pair(6, COLOR_MAGENTA,-1);   /* header */
    }

    getmaxyx(stdscr, term_rows, term_cols);

    /* Create initial windows — bottom = 2 lines (status + input) */
    int bottom_h = 2;
    int main_h = term_rows - bottom_h;
    win_main   = newwin(main_h, term_cols, 0, 0);
    win_bottom = newwin(bottom_h, term_cols, main_h, 0);

    scrollok(win_main, FALSE);
    scrollok(win_bottom, FALSE);
    keypad(win_main, TRUE);
    keypad(win_bottom, TRUE);

    refresh();
}

void tui_shutdown(void) {
    if (win_main)   delwin(win_main);
    if (win_bottom) delwin(win_bottom);
    win_main = win_bottom = NULL;
    endwin();
}

/* ── helpers ──────────────────────────────────────── */

/* Compute bottom pane height based on content */
static int compute_bottom_height(ui_state_t *ui) {
    int max_bottom = term_rows / 2;  /* max 50% of screen */

    if (ui->focus == FOCUS_QUERY) {
        /* Query mode: 2 lines (status + input) when empty,
         * grows with multi-line input up to max */
        int lines = 2;  /* status + input */
        if (ui->input_len > 0) {
            /* Count wrapped lines */
            int input_lines = (ui->input_len / (term_cols - 6)) + 1;
            lines = 1 + input_lines;  /* status + input lines */
        }
        if (lines > max_bottom) lines = max_bottom;
        return lines;
    }

    if (ui->focus == FOCUS_JOURNAL) {
        /* Journal browsing: bottom shows preview */
        if (ui->view == VIEW_QUERIES && ui->query_count > 0) {
            /* Preview: show first few steps of selected query */
            int preview_lines = 8;
            if (preview_lines > max_bottom) preview_lines = max_bottom;
            return preview_lines;
        }
        if (ui->view == VIEW_MANIFEST && ui->step_count > 0) {
            /* Preview: show step detail */
            int preview_lines = 10;
            if (preview_lines > max_bottom) preview_lines = max_bottom;
            return preview_lines;
        }
    }

    return 2;  /* default: status + input */
}

/* Resize windows if needed */
static void resize_panes(ui_state_t *ui) {
    int new_rows, new_cols;
    getmaxyx(stdscr, new_rows, new_cols);

    int bottom_h = compute_bottom_height(ui);
    int main_h = new_rows - bottom_h;
    if (main_h < 3) main_h = 3;
    bottom_h = new_rows - main_h;

    if (new_rows != term_rows || new_cols != term_cols ||
        getmaxy(win_main) != main_h || getmaxy(win_bottom) != bottom_h) {
        term_rows = new_rows;
        term_cols = new_cols;

        wresize(win_main, main_h, term_cols);
        mvwin(win_main, 0, 0);

        wresize(win_bottom, bottom_h, term_cols);
        mvwin(win_bottom, main_h, 0);

        ui->dirty = 1;
    }
}

/* Print a string, truncating at width */
static void wprint_trunc(WINDOW *w, int y, int x, const char *s, int maxw) {
    if (!s || maxw <= 0) return;
    wmove(w, y, x);
    int col = 0;
    for (const char *p = s; *p && col < maxw; p++, col++)
        waddch(w, (unsigned char)*p);
}

/* ── render main pane ─────────────────────────────── */

static void render_main(ui_state_t *ui) {
    int rows = getmaxy(win_main);
    int cols = getmaxx(win_main);

    werase(win_main);

    if (ui->view == VIEW_QUERIES || ui->view == VIEW_MANIFEST) {
        /* Show banner first, then journal content */
        int line = 0;

        /* Render banner if present */
        if (ui->banner) {
            const char *p = ui->banner;
            while (*p && line < rows - 1) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                if (len > cols) len = cols;

                /* Color the banner lines */
                if (line < 6) {
                    wattron(win_main, COLOR_PAIR(1) | A_BOLD);
                    mvwaddnstr(win_main, line, 0, p, len);
                    wattroff(win_main, COLOR_PAIR(1) | A_BOLD);
                } else {
                    wattron(win_main, COLOR_PAIR(3));
                    mvwaddnstr(win_main, line, 0, p, len);
                    wattroff(win_main, COLOR_PAIR(3));
                }
                line++;
                p = eol ? eol + 1 : p + strlen(p);
            }
            line++;  /* blank line after banner */
        }

        /* Show queries */
        if (ui->view == VIEW_QUERIES) {
            if (ui->query_count == 0 && !ui->banner) {
                wattron(win_main, COLOR_PAIR(3));
                mvwaddstr(win_main, line, 0, "  (no queries yet - type below and press Enter)");
                wattroff(win_main, COLOR_PAIR(3));
            } else {
                for (int i = 0; i < ui->query_count && line < rows; i++) {
                    ui_query_t *q = &ui->queries[i];
                    int selected = (i == ui->selected_query && ui->focus == FOCUS_JOURNAL);

                    if (selected) wattron(win_main, A_REVERSE);

                    /* Format: "2026-05-24 10:51  query text..." */
                    char ts[32] = "";
                    if (q->timestamp > 0) {
                        time_t t = (time_t)q->timestamp;
                        struct tm *tm = localtime(&t);
                        if (tm) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm);
                    }

                    char buf[512];
                    snprintf(buf, sizeof(buf), "  %s  %s",
                             ts, q->query_text ? q->query_text : "?");
                    wprint_trunc(win_main, line, 0, buf, cols);

                    /* Fill rest of line for reverse video */
                    if (selected) {
                        int cur_x = getcurx(win_main);
                        for (int x = cur_x; x < cols; x++)
                            waddch(win_main, ' ');
                        wattroff(win_main, A_REVERSE);
                    }
                    line++;
                }
            }
        } else if (ui->view == VIEW_MANIFEST) {
            /* Show steps for selected query */
            wattron(win_main, COLOR_PAIR(4) | A_BOLD);
            if (ui->selected_query >= 0 && ui->selected_query < ui->query_count) {
                char hdr[256];
                snprintf(hdr, sizeof(hdr), "  Query R%d: %s",
                         ui->queries[ui->selected_query].react_loop,
                         ui->queries[ui->selected_query].query_text ?
                         ui->queries[ui->selected_query].query_text : "?");
                wprint_trunc(win_main, line, 0, hdr, cols);
            }
            wattroff(win_main, COLOR_PAIR(4) | A_BOLD);
            line += 2;

            for (int i = 0; i < ui->step_count && line < rows; i++) {
                ui_step_t *s = &ui->steps[i];
                int selected = (i == ui->selected_step && ui->focus == FOCUS_JOURNAL);

                if (selected) wattron(win_main, A_REVERSE);

                /* Format: "  V R0S3: shell_exec "ls -la" -> 473 chars" */
                char mark = s->failed ? 'X' : 'V';
                int pair = s->failed ? 2 : 1;
                wattron(win_main, COLOR_PAIR(pair));
                mvwprintw(win_main, line, 0, "  %c ", mark);
                wattroff(win_main, COLOR_PAIR(pair));

                char buf[512];
                if (s->failed) {
                    snprintf(buf, sizeof(buf), "%s: %s \"%s\"",
                             s->ref ? s->ref : "?",
                             s->tool ? s->tool : "?",
                             s->description ? s->description : "");
                } else {
                    snprintf(buf, sizeof(buf), "%s: %s \"%s\" -> %d chars",
                             s->ref ? s->ref : "?",
                             s->tool ? s->tool : "?",
                             s->description ? s->description : "",
                             s->size);
                }
                wprint_trunc(win_main, line, 4, buf, cols - 4);

                if (selected) {
                    int cur_x = getcurx(win_main);
                    for (int x = cur_x; x < cols; x++)
                        waddch(win_main, ' ');
                    wattroff(win_main, A_REVERSE);
                }
                line++;
            }
        }
    } else if (ui->view == VIEW_DETAIL) {
        /* Show full tool output */
        if (ui->detail_content) {
            const char *p = ui->detail_content;
            int line = -ui->detail_scroll;  /* scroll offset */
            while (*p && line < rows) {
                const char *eol = strchr(p, '\n');
                int len = eol ? (int)(eol - p) : (int)strlen(p);
                if (line >= 0) {
                    if (len > cols) len = cols;
                    mvwaddnstr(win_main, line, 0, p, len);
                }
                line++;
                p = eol ? eol + 1 : p + strlen(p);
            }
        } else {
            mvwaddstr(win_main, 0, 0, "  (no content)");
        }
    }

    wnoutrefresh(win_main);
}

/* ── render bottom pane ───────────────────────────── */

static void render_bottom(ui_state_t *ui) {
    int rows = getmaxy(win_bottom);
    int cols = getmaxx(win_bottom);

    werase(win_bottom);

    /* Row 0: separator line */
    wattron(win_bottom, COLOR_PAIR(3));
    for (int x = 0; x < cols; x++)
        mvwaddch(win_bottom, 0, x, ACS_HLINE);
    wattroff(win_bottom, COLOR_PAIR(3));

    /* Row 0 right side: status indicator */
    if (ui->status_text) {
        const char *indicator;
        int pair;
        switch (ui->status) {
            case STATUS_RUNNING:        indicator = "* "; pair = 4; break;
            case STATUS_AWAITING_INPUT: indicator = "? "; pair = 4; break;
            case STATUS_DONE:           indicator = "  "; pair = 1; break;
            case STATUS_ERROR:          indicator = "! "; pair = 2; break;
            default:                    indicator = "  "; pair = 5; break;
        }
        int slen = (int)strlen(ui->status_text) + 4;
        int sx = cols - slen - 1;
        if (sx < 0) sx = 0;
        wattron(win_bottom, COLOR_PAIR(pair));
        mvwprintw(win_bottom, 0, sx, " %s%s ", indicator, ui->status_text);
        wattroff(win_bottom, COLOR_PAIR(pair));
    }

    /* Row 1+: input line or preview */
    if (rows > 1) {
        if (ui->focus == FOCUS_QUERY || ui->view == VIEW_QUERIES) {
            /* Show input prompt */
            wattron(win_bottom, COLOR_PAIR(1) | A_BOLD);
            mvwaddstr(win_bottom, 1, 0, "nash> ");
            wattroff(win_bottom, COLOR_PAIR(1) | A_BOLD);

            /* Show input text */
            if (ui->input_buffer && ui->input_len > 0) {
                int maxw = cols - 6;
                int start = 0;
                if (ui->cursor_pos > maxw)
                    start = ui->cursor_pos - maxw;
                waddnstr(win_bottom, ui->input_buffer + start,
                         maxw < ui->input_len - start ? maxw : ui->input_len - start);
            }
        } else if (ui->view == VIEW_MANIFEST && ui->focus == FOCUS_JOURNAL) {
            /* Preview: show selected step's output preview */
            if (ui->selected_step >= 0 && ui->selected_step < ui->step_count) {
                ui_step_t *s = &ui->steps[ui->selected_step];
                wattron(win_bottom, COLOR_PAIR(3));
                mvwprintw(win_bottom, 1, 0, "  [%s output - press Enter for full view]",
                          s->ref ? s->ref : "?");
                wattroff(win_bottom, COLOR_PAIR(3));
            }
        }
    }

    /* Position cursor at input */
    if (ui->focus == FOCUS_QUERY && rows > 1) {
        int cx = 6 + ui->cursor_pos;
        if (cx >= cols) cx = cols - 1;
        wmove(win_bottom, 1, cx);
        curs_set(1);
    } else {
        curs_set(0);
    }

    wnoutrefresh(win_bottom);
}

/* ── public API ───────────────────────────────────── */

void tui_render(ui_state_t *ui) {
    if (!ui || !ui->dirty) return;

    resize_panes(ui);
    render_main(ui);
    render_bottom(ui);
    doupdate();
    ui->dirty = 0;
}

int tui_input(ui_state_t *ui, char **out_query) {
    if (out_query) *out_query = NULL;

    int ch = getch();
    if (ch == ERR) return 0;  /* no input */

    /* Handle resize */
    if (ch == KEY_RESIZE) {
        ui->dirty = 1;
        return 1;
    }

    /* TAB: switch focus */
    if (ch == '\t' || ch == 9) {
        ui_state_tab(ui);
        return 1;
    }

    /* Escape: go back */
    if (ch == 27) {
        ui_state_back(ui);
        return 1;
    }

    /* Ctrl-C / Ctrl-D: quit */
    if (ch == 3 || ch == 4) return -1;

    /* Focus-specific input */
    if (ui->focus == FOCUS_JOURNAL) {
        switch (ch) {
            case KEY_UP:   case 'k': ui_state_up(ui); break;
            case KEY_DOWN: case 'j': ui_state_down(ui); break;
            case KEY_PPAGE: ui_state_page_up(ui); break;
            case KEY_NPAGE: ui_state_page_down(ui); break;
            case '\n': case KEY_ENTER: case '\r':
                ui_state_enter(ui);
                break;
            case 'q':
                return -1;
        }
    } else if (ui->focus == FOCUS_QUERY) {
        switch (ch) {
            case '\n': case KEY_ENTER: case '\r':
                /* Submit query */
                if (ui->input_buffer && ui->input_len > 0) {
                    if (out_query) {
                        *out_query = strndup(ui->input_buffer, ui->input_len);
                    }
                    /* Check for quit */
                    if (ui->input_len == 4 &&
                        (strncmp(ui->input_buffer, "quit", 4) == 0 ||
                         strncmp(ui->input_buffer, "exit", 4) == 0)) {
                        return -1;
                    }
                    /* Clear input */
                    ui->input_len = 0;
                    ui->cursor_pos = 0;
                    ui->dirty = 1;
                }
                break;
            case KEY_BACKSPACE: case 127: case 8:
                ui_state_input_backspace(ui);
                break;
            case KEY_DC:
                ui_state_input_delete(ui);
                break;
            case KEY_LEFT:
                ui_state_input_left(ui);
                break;
            case KEY_RIGHT:
                ui_state_input_right(ui);
                break;
            case KEY_HOME:
                ui_state_input_home(ui);
                break;
            case KEY_END:
                ui_state_input_end(ui);
                break;
            default:
                if (ch >= 32 && ch < 127) {
                    ui_state_input_char(ui, ch);
                }
                break;
        }
    }

    return 1;
}

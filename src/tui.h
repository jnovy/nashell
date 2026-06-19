#ifndef TUI_H
#define TUI_H

#include <stdatomic.h>
#include "ui_state.h"

/* Global flag: 1 when ncurses TUI is active, 0 otherwise.
 * Library code must check this before writing to stderr/stdout
 * to avoid corrupting the ncurses display.
 *
 * Thread safety: written by main thread (tui_init/tui_shutdown),
 * read by inference thread (nash_log). Uses atomic_int to avoid
 * data races without requiring a mutex for this single flag. */
extern atomic_int g_tui_active;

/* Set to 1 once tui_init() is called.  Never reset.
 * Used by provider.c to distinguish "TUI never started" (daemon/headless)
 * from "TUI was running but shut down" — only the latter should abort
 * in-flight curl requests.  Without this, daemon mode aborts every LLM
 * call because g_tui_active==0 looks like "TUI shut down". */
extern atomic_int g_tui_was_started;

/* Initialize ncurses TUI — creates windows, sets up colors */
void tui_init(void);

/* Shutdown ncurses TUI — restores terminal */
void tui_shutdown(void);

/* Render the current ui_state to the terminal.
 * Only redraws if ui->dirty is set. */
void tui_render(ui_state_t *ui);

/* Process one input event (non-blocking).
 * Returns: 0 = no input, 1 = input processed, -1 = quit requested
 * If a query was submitted, *out_query is set (caller frees). */
int tui_input(ui_state_t *ui, char **out_query);

#endif

#ifndef TUI_H
#define TUI_H

#include "ui_state.h"

/* Global flag: 1 when ncurses TUI is active, 0 otherwise.
 * Library code must check this before writing to stderr/stdout
 * to avoid corrupting the ncurses display. */
extern int g_tui_active;

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

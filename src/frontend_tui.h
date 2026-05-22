#ifndef FRONTEND_TUI_H
#define FRONTEND_TUI_H

#include "react_event.h"

/* Simple TUI event handler — prints to stderr with ANSI escapes.
 * userdata should be the session_dir (const char *) for store/ file reading. */
void tui_on_event(const react_event_t *ev, void *userdata);

#endif

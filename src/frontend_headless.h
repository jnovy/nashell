#ifndef FRONTEND_HEADLESS_H
#define FRONTEND_HEADLESS_H

#include "react_event.h"

/* Headless ANSI stderr event handler -- prints to stderr with ANSI escapes.
 * userdata should be the session_dir (const char *) for store/ file reading. */
void headless_on_event(const react_event_t *ev, void *userdata);

#endif

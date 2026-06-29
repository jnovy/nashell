/* device.h — Device control session: display + input bound together.
 *
 * A device_session_t owns one display backend and one input backend,
 * tracking session state (action counts, safety limits).
 *
 * See docs/design-device-control.md §3.4 */
#ifndef DEVICE_H
#define DEVICE_H

#include "display.h"
#include "input.h"

/* A device session: display capture + input injection */
typedef struct {
    display_t      *display;
    input_t        *input;

    /* Session tracking */
    int             screenshot_count;   /* total screenshots taken */
    int             action_count;       /* total actions performed */
    int             consecutive_errors; /* for retry/abort logic */

    /* Safety */
    int             action_delay_ms;    /* ms to wait after each action (default 500) */
    int             screenshot_delay_ms;/* ms to wait after action before screenshot */
} device_session_t;

/* Open a device session with the given configs.
 * Returns NULL on error (display or input open failure). */
device_session_t *device_session_open(const display_config_t *dcfg,
                                       const input_config_t *icfg,
                                       int action_delay_ms,
                                       int screenshot_delay_ms);

/* Close a device session (closes display and input). */
void device_session_close(device_session_t *s);

#endif /* DEVICE_H */

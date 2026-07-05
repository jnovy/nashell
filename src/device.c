/* device.c — Device session lifecycle.
 *
 * Binds a display backend + input backend together into a session
 * with safety counters and limits.
 *
 * See docs/design-device-control.md §3.4 */

#include "device.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>

device_session_t *device_session_open(const display_config_t *dcfg,
                                       const input_config_t *icfg,
                                       const stream_config_t *scfg,
                                       int action_delay_ms,
                                       int screenshot_delay_ms) {
    device_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* Open display backend */
    s->display = display_open(dcfg);
    if (!s->display) {
        nash_log("[device] failed to open display backend");
        free(s);
        return NULL;
    }

    /* Open input backend */
    s->input = input_open(icfg);
    if (!s->input) {
        nash_log("[device] failed to open input backend");
        display_close(s->display);
        free(s);
        return NULL;
    }

    /* For VNC: share the socket fd between display and input */
    if (dcfg->type == DISPLAY_VNC && icfg->type == INPUT_VNC) {
        int sock_fd = display_get_vnc_sock_fd(s->display);
        if (sock_fd >= 0) {
            input_vnc_set_sock_fd(s->input, sock_fd);
        }
    }

    /* Start continuous HEVC capture stream if configured */
    if (scfg) {
        s->stream = stream_start(s->display, scfg);
        if (!s->stream) {
            nash_log("[device] stream not started (continuing with on-demand capture)");
        }
    }

    /* Safety defaults */
    s->action_delay_ms    = action_delay_ms >= 0 ? action_delay_ms : 500;
    s->screenshot_delay_ms = screenshot_delay_ms >= 0 ? screenshot_delay_ms : 300;

    s->screenshot_count   = 0;
    s->action_count       = 0;
    s->consecutive_errors = 0;

    return s;
}

void device_session_close(device_session_t *s) {
    if (!s) return;
    if (s->stream)  stream_stop(s->stream);
    if (s->input)   input_close(s->input);
    if (s->display) display_close(s->display);
    free(s);
}

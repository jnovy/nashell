/* input.h — Keyboard/mouse injection abstraction for device control.
 *
 * Backends:
 *   INPUT_VNC        — Direct RFB key/pointer events (no external library)
 *   INPUT_HID_BRIDGE — RP2040 serial HID bridge (Phase 2)
 *   INPUT_CMD        — External command (e.g. xdotool) (Phase 3)
 *
 * All mouse coordinates are in MODEL space (the display's declared
 * dimensions).  Backends map to native resolution internally.
 *
 * See docs/design-device-control.md §3.2 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

typedef enum {
    INPUT_VNC,          /* RFB key/pointer events */
    INPUT_HID_BRIDGE,   /* RP2040 serial HID bridge */
    INPUT_CMD,          /* External command (e.g. xdotool) */
} input_type_t;

typedef struct input_t input_t;

/* Configuration for input backends */
typedef struct {
    input_type_t type;

    /* HID bridge backend (Phase 2) */
    char *serial_port;       /* e.g. "/dev/ttyUSB0" */
    int   serial_baud;       /* default 115200 */

    /* Command backend (Phase 3) */
    char *key_cmd_fmt;       /* e.g. "xdotool key %s" */
    char *type_cmd_fmt;      /* e.g. "xdotool type '%s'" */
    char *click_cmd_fmt;     /* e.g. "xdotool mousemove %d %d click %d" */

    /* Screen dimensions for coordinate mapping.
     * The model emits coordinates in the display's declared space.
     * If the target's actual resolution differs, the input backend
     * maps: target_x = model_x * native_w / model_w */
    int   native_width;      /* target's actual screen width */
    int   native_height;     /* target's actual screen height */
    int   model_width;       /* display width the model sees */
    int   model_height;      /* display height the model sees */
} input_config_t;

/* Create an input backend from config. Returns NULL on error. */
input_t *input_open(const input_config_t *cfg);

/* For VNC: share the socket fd from display backend.
 * Must be called after input_open() for INPUT_VNC type. */
void input_vnc_set_sock_fd(input_t *in, int sock_fd);

/* Update coordinate mapping dimensions (auto-detected from display).
 * Called after display_open() to set native resolution from VNC/webcam.
 * model_w/model_h = coordinate space the LLM sees (same as native when
 * no downscaling is applied). */
void input_set_dimensions(input_t *in, int native_w, int native_h,
                          int model_w, int model_h);

/* Keyboard actions */
int input_key(input_t *in, const char *keyname);            /* press+release */
int input_combo(input_t *in, const char **keys, int nkeys); /* key combination */
int input_type(input_t *in, const char *text);              /* type string */

/* Mouse actions — coordinates in MODEL space (mapped internally) */
int input_mouse_move(input_t *in, int x, int y);
int input_click(input_t *in, int x, int y, const char *button);
int input_double_click(input_t *in, int x, int y, const char *button);
int input_scroll(input_t *in, int x, int y, const char *direction, int amount);
int input_drag(input_t *in, int x1, int y1, int x2, int y2, const char *button);

/* Close and free the input backend. */
void input_close(input_t *in);

/* ── Backend-specific open functions (defined in input_*.c) ──── */

input_t *input_open_vnc(const input_config_t *cfg);

#endif /* INPUT_H */

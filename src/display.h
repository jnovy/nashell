/* display.h — Screen capture abstraction for device control.
 *
 * Backends:
 *   DISPLAY_VNC    — Direct RFB 3.8 protocol (no external library)
 *   DISPLAY_WEBCAM — V4L2 webcam / HDMI capture card (Phase 2)
 *   DISPLAY_CMD    — External command (Phase 3)
 *
 * See docs/design-device-control.md §3.1 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    DISPLAY_VNC,        /* VNC/RFB client */
    DISPLAY_WEBCAM,     /* V4L2 webcam / HDMI capture card */
    DISPLAY_CMD,        /* External command (e.g. "scrot -o /tmp/ss.png") */
} display_type_t;

typedef struct display_t display_t;

/* Configuration for display backends */
typedef struct {
    display_type_t type;

    /* VNC backend */
    char *vnc_host;          /* e.g. "192.168.1.50" or "localhost" */
    int   vnc_port;          /* default 5900 */
    char *vnc_password;      /* VNC auth password (NULL = no auth) */

    /* Webcam backend (Phase 2) */
    char *webcam_device;     /* e.g. "/dev/video0" */
    int   webcam_index;      /* alternative: camera index (0, 1, ...) */

    /* Webcam calibration (Phase 2) */
    char *calibration_file;  /* path to calibration data, NULL = no correction */

    /* Command backend (Phase 3) */
    char *capture_cmd;       /* shell command that produces a PNG on stdout */

    /* Common: declared display dimensions for coordinate space */
    int   width;             /* target screen width (pixels) */
    int   height;            /* target screen height (pixels) */

    /* Screenshot output directory */
    char *screenshot_dir;    /* default: /tmp/device_screenshots */
} display_config_t;

/* Create a display backend from config. Returns NULL on error. */
display_t *display_open(const display_config_t *cfg);

/* Capture a screenshot and save to disk.
 * Returns path to the saved image (malloc'd), or NULL on error.
 * Caller must free() the path. */
char *display_capture(display_t *d);

/* Update the in-memory framebuffer from the display source without
 * writing to disk.  For VNC: sends FramebufferUpdateRequest + processes
 * the server response.  Returns 0 on success, -1 on error.
 * Used by the continuous capture stream (stream.c). */
int display_update_framebuffer(display_t *d);

/* Copy the current BGRA framebuffer into a caller-provided buffer.
 * buf must be at least native_w * native_h * 4 bytes.
 * Not thread-safe -- caller must provide external synchronization. */
void display_copy_framebuffer(display_t *d, uint8_t *buf);

/* Write a BGRA framebuffer to a JPEG file on disk.
 * Returns 0 on success, -1 on error. */
int display_write_jpeg(const char *path, const uint8_t *bgra,
                       int width, int height, int quality);

/* Generate a timestamped screenshot path in the display's screenshot_dir.
 * Returns malloc'd string, caller must free(). */
char *display_make_screenshot_path(display_t *d);

/* Get the native display dimensions (from VNC framebuffer or webcam).
 * Returns 0 on success, -1 if unknown. */
int display_get_dimensions(display_t *d, int *w, int *h);

/* Close and free the display backend. */
void display_close(display_t *d);

/* ── Backend-specific open functions (defined in display_*.c) ──── */

display_t *display_open_vnc(const display_config_t *cfg);

/* Get the raw socket fd from a VNC display (for input backend sharing) */
int display_get_vnc_sock_fd(display_t *d);

#endif /* DISPLAY_H */

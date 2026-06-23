/* display.c — Display backend dispatcher.
 *
 * Routes display_open/capture/close to the active backend.
 * See docs/design-device-control.md §3.1, §6.1 */

#include "display.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

/* Internal display structure — must match layout in display_vnc.c exactly */
struct display_t {
    display_config_t  cfg;
    display_type_t    type;

    /* VNC-specific (only valid when type == DISPLAY_VNC) */
    int               sock_fd;      /* Raw TCP socket to VNC server */
    int               native_w, native_h;

    /* Framebuffer (BGRA, for VNC backend) */
    uint8_t          *framebuffer;

    /* 4 persistent zlib streams for Tight encoding */
    z_stream          zstreams[4];
    int               zstream_inited[4];

    /* Backend function pointers */
    char *(*capture_fn)(display_t *d);
    void  (*close_fn)(display_t *d);
};

/* ── Screenshot path generation ──────────────────────────────── */

static void ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0)
        mkdir(dir, 0755);
}

static char *make_screenshot_path(const char *dir) {
    ensure_dir(dir);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char *path = malloc(512);
    if (!path) return NULL;
    snprintf(path, 512, "%s/screen_%ld_%03ld.jpg",
             dir, (long)ts.tv_sec, ts.tv_nsec / 1000000);
    return path;
}

/* ── Public API ──────────────────────────────────────────────── */

display_t *display_open(const display_config_t *cfg) {
    switch (cfg->type) {
    case DISPLAY_VNC:
        return display_open_vnc(cfg);
    case DISPLAY_WEBCAM:
        nash_log("[display] webcam backend not yet implemented (Phase 2)");
        return NULL;
    case DISPLAY_CMD:
        nash_log("[display] command backend not yet implemented (Phase 3)");
        return NULL;
    default:
        nash_log("[display] unknown backend type %d", cfg->type);
        return NULL;
    }
}

char *display_capture(display_t *d) {
    if (!d || !d->capture_fn) return NULL;
    return d->capture_fn(d);
}

int display_get_dimensions(display_t *d, int *w, int *h) {
    if (!d) return -1;
    if (d->native_w > 0 && d->native_h > 0) {
        if (w) *w = d->native_w;
        if (h) *h = d->native_h;
        return 0;
    }
    return -1;
}

void display_close(display_t *d) {
    if (!d) return;
    if (d->close_fn) d->close_fn(d);
    else free(d);
}

/* Accessor for VNC socket fd (used by input_vnc to share connection) */
int display_get_vnc_sock_fd(display_t *d) {
    if (d && d->type == DISPLAY_VNC)
        return d->sock_fd;
    return -1;
}

/* ── Helpers exported to backends ──────────────────────────── */

char *display_make_screenshot_path(display_t *d) {
    const char *dir = d->cfg.screenshot_dir;
    if (!dir || !dir[0]) dir = "/tmp/device_screenshots";
    return make_screenshot_path(dir);
}

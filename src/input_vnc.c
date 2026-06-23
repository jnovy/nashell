/* input_vnc.c — VNC/RFB input backend via direct RFB protocol.
 *
 * Injects keyboard and mouse events over a raw TCP socket using
 * RFB KeyEvent (type 4) and PointerEvent (type 5) messages.
 * Shares the socket fd from the VNC display backend.
 *
 * No external VNC library — same approach as nashell/vnc_control.py.
 *
 * See docs/design-device-control.md §6.2 */

#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>

/* ── RFB message constants ──────────────────────────────────── */

#define RFB_KEY_EVENT       4
#define RFB_POINTER_EVENT   5

/* VNC mouse button masks */
#define VNC_BUTTON1    0x01   /* left */
#define VNC_BUTTON2    0x02   /* middle */
#define VNC_BUTTON3    0x04   /* right */
#define VNC_BUTTON4    0x08   /* scroll up */
#define VNC_BUTTON5    0x10   /* scroll down */
#define VNC_BUTTON6    0x20   /* scroll left */
#define VNC_BUTTON7    0x40   /* scroll right */

/* ── Internal structure (must match input.c layout) ──────────── */

struct input_t {
    input_config_t cfg;
    input_type_t   type;

    /* Raw socket fd — shared from display backend */
    void          *vnc_client;   /* cast: (intptr_t)sock_fd stored here */

    int  (*key_fn)(input_t *in, const char *keyname);
    int  (*combo_fn)(input_t *in, const char **keys, int nkeys);
    int  (*type_fn)(input_t *in, const char *text);
    int  (*move_fn)(input_t *in, int x, int y);
    int  (*click_fn)(input_t *in, int x, int y, const char *button);
    int  (*dblclick_fn)(input_t *in, int x, int y, const char *button);
    int  (*scroll_fn)(input_t *in, int x, int y, const char *direction, int amount);
    int  (*drag_fn)(input_t *in, int x1, int y1, int x2, int y2, const char *button);
    void (*close_fn)(input_t *in);
};

/* Get socket fd from the opaque vnc_client pointer */
#include <stdint.h>
static int get_sock_fd(input_t *in) {
    return (int)(intptr_t)in->vnc_client;
}

/* ── Send helpers ───────────────────────────────────────────── */

/* Send exactly n bytes. Returns 0 on success, -1 on error. */
static int send_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t w = send(fd, p, remaining, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            fprintf(stderr, "[input_vnc] send failed: %s\n", strerror(errno));
            return -1;
        }
        p += w;
        remaining -= (size_t)w;
    }
    return 0;
}

/* ── Byte-order helpers ─────────────────────────────────────── */

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF;
    p[3] = v & 0xFF;
}

/* ── RFB event senders ──────────────────────────────────────── */

/* Send RFB KeyEvent: msg(1) + downFlag(1) + pad(2) + keysym(4) = 8 bytes */
static int rfb_key_event(int fd, uint32_t keysym, int down) {
    uint8_t msg[8] = {0};
    msg[0] = RFB_KEY_EVENT;
    msg[1] = down ? 1 : 0;
    /* msg[2..3] = padding */
    wr32(msg + 4, keysym);
    return send_exact(fd, msg, 8);
}

/* Send RFB PointerEvent: msg(1) + buttonMask(1) + x(2) + y(2) = 6 bytes */
static int rfb_pointer_event(int fd, int x, int y, uint8_t button_mask) {
    uint8_t msg[6];
    msg[0] = RFB_POINTER_EVENT;
    msg[1] = button_mask;
    wr16(msg + 2, (uint16_t)x);
    wr16(msg + 4, (uint16_t)y);
    return send_exact(fd, msg, 6);
}

/* ── X11 keysym mapping ─────────────────────────────────────── */

/* Map common key names to X11 keysyms (XK_*) */
static uint32_t keyname_to_keysym(const char *name) {
    /* Special keys */
    if (strcasecmp(name, "enter") == 0 || strcasecmp(name, "return") == 0)
        return 0xFF0D;  /* XK_Return */
    if (strcasecmp(name, "tab") == 0)       return 0xFF09;  /* XK_Tab */
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0)
        return 0xFF1B;  /* XK_Escape */
    if (strcasecmp(name, "backspace") == 0) return 0xFF08;  /* XK_BackSpace */
    if (strcasecmp(name, "delete") == 0)    return 0xFFFF;  /* XK_Delete */
    if (strcasecmp(name, "insert") == 0)    return 0xFF63;  /* XK_Insert */
    if (strcasecmp(name, "home") == 0)      return 0xFF50;  /* XK_Home */
    if (strcasecmp(name, "end") == 0)       return 0xFF57;  /* XK_End */
    if (strcasecmp(name, "pageup") == 0 || strcasecmp(name, "page_up") == 0)
        return 0xFF55;  /* XK_Page_Up */
    if (strcasecmp(name, "pagedown") == 0 || strcasecmp(name, "page_down") == 0)
        return 0xFF56;  /* XK_Page_Down */
    if (strcasecmp(name, "space") == 0)     return 0x0020;  /* XK_space */

    /* Arrow keys */
    if (strcasecmp(name, "up") == 0)        return 0xFF52;  /* XK_Up */
    if (strcasecmp(name, "down") == 0)      return 0xFF54;  /* XK_Down */
    if (strcasecmp(name, "left") == 0)      return 0xFF51;  /* XK_Left */
    if (strcasecmp(name, "right") == 0)     return 0xFF53;  /* XK_Right */

    /* Modifier keys */
    if (strcasecmp(name, "shift") == 0)     return 0xFFE1;  /* XK_Shift_L */
    if (strcasecmp(name, "ctrl") == 0 || strcasecmp(name, "control") == 0)
        return 0xFFE3;  /* XK_Control_L */
    if (strcasecmp(name, "alt") == 0)       return 0xFFE9;  /* XK_Alt_L */
    if (strcasecmp(name, "super") == 0 || strcasecmp(name, "meta") == 0 ||
        strcasecmp(name, "win") == 0)
        return 0xFFEB;  /* XK_Super_L */

    /* Function keys */
    if (name[0] == 'f' || name[0] == 'F') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12)
            return 0xFFBE + (uint32_t)(n - 1);  /* XK_F1 .. XK_F12 */
    }

    /* Single printable character → its ASCII keysym */
    if (name[0] && !name[1])
        return (uint32_t)(unsigned char)name[0];

    fprintf(stderr, "[input_vnc] unknown key name: %s\n", name);
    return 0;
}

/* ── Coordinate mapping ─────────────────────────────────────── */

/* Map model-space coordinates to native VNC coordinates */
static void map_coords(input_t *in, int mx, int my, int *nx, int *ny) {
    if (in->cfg.model_width > 0 && in->cfg.native_width > 0) {
        *nx = mx * in->cfg.native_width / in->cfg.model_width;
        *ny = my * in->cfg.native_height / in->cfg.model_height;
    } else {
        *nx = mx;
        *ny = my;
    }
}

/* Get VNC button mask from button name */
static uint8_t button_mask_from_name(const char *button) {
    if (!button || strcasecmp(button, "left") == 0)
        return VNC_BUTTON1;
    if (strcasecmp(button, "right") == 0)
        return VNC_BUTTON3;
    if (strcasecmp(button, "middle") == 0)
        return VNC_BUTTON2;
    return VNC_BUTTON1;
}

/* ── VNC input operations ───────────────────────────────────── */

static int vnc_key(input_t *in, const char *keyname) {
    int fd = get_sock_fd(in);
    if (fd < 0) return -1;

    /* Handle combo notation: "ctrl+c" → parse and delegate to combo */
    if (strchr(keyname, '+')) {
        char buf[256];
        strncpy(buf, keyname, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        const char *parts[8];
        int n = 0;
        char *tok = strtok(buf, "+");
        while (tok && n < 8) {
            parts[n++] = tok;
            tok = strtok(NULL, "+");
        }
        return in->combo_fn(in, parts, n);
    }

    uint32_t keysym = keyname_to_keysym(keyname);
    if (keysym == 0) return -1;

    if (rfb_key_event(fd, keysym, 1) < 0) return -1;
    usleep(10000);  /* 10ms */
    if (rfb_key_event(fd, keysym, 0) < 0) return -1;
    return 0;
}

static int vnc_combo(input_t *in, const char **keys, int nkeys) {
    int fd = get_sock_fd(in);
    if (fd < 0 || nkeys <= 0) return -1;

    /* Press all keys */
    for (int i = 0; i < nkeys; i++) {
        uint32_t ks = keyname_to_keysym(keys[i]);
        if (ks == 0) return -1;
        if (rfb_key_event(fd, ks, 1) < 0) return -1;
        usleep(5000);
    }

    /* Release in reverse order */
    for (int i = nkeys - 1; i >= 0; i--) {
        uint32_t ks = keyname_to_keysym(keys[i]);
        if (rfb_key_event(fd, ks, 0) < 0) return -1;
        usleep(5000);
    }
    return 0;
}

static int vnc_type_text(input_t *in, const char *text) {
    int fd = get_sock_fd(in);
    if (fd < 0 || !text) return -1;

    for (int i = 0; text[i]; i++) {
        uint32_t ks = (uint32_t)(unsigned char)text[i];
        /* Handle uppercase: shift + lowercase keysym */
        int need_shift = isupper((unsigned char)text[i]);
        if (need_shift)
            rfb_key_event(fd, 0xFFE1, 1);  /* Shift press */
        rfb_key_event(fd, ks, 1);
        usleep(5000);
        rfb_key_event(fd, ks, 0);
        if (need_shift)
            rfb_key_event(fd, 0xFFE1, 0);  /* Shift release */
        usleep(5000);
    }
    return 0;
}

static int vnc_move(input_t *in, int x, int y) {
    int fd = get_sock_fd(in);
    if (fd < 0) return -1;

    int nx, ny;
    map_coords(in, x, y, &nx, &ny);
    return rfb_pointer_event(fd, nx, ny, 0);
}

static int vnc_click(input_t *in, int x, int y, const char *button) {
    int fd = get_sock_fd(in);
    if (fd < 0) return -1;

    int nx, ny;
    map_coords(in, x, y, &nx, &ny);
    uint8_t mask = button_mask_from_name(button);

    /* Move, press, release */
    if (rfb_pointer_event(fd, nx, ny, mask) < 0) return -1;
    usleep(50000);  /* 50ms hold */
    return rfb_pointer_event(fd, nx, ny, 0);
}

static int vnc_double_click(input_t *in, int x, int y, const char *button) {
    vnc_click(in, x, y, button);
    usleep(100000);  /* 100ms between clicks */
    vnc_click(in, x, y, button);
    return 0;
}

static int vnc_scroll(input_t *in, int x, int y,
                      const char *direction, int amount) {
    int fd = get_sock_fd(in);
    if (fd < 0) return -1;

    int nx, ny;
    map_coords(in, x, y, &nx, &ny);

    /* VNC scroll uses buttons 4 (up), 5 (down), 6 (left), 7 (right) */
    uint8_t btn;
    if (strcasecmp(direction, "up") == 0)
        btn = VNC_BUTTON4;
    else if (strcasecmp(direction, "down") == 0)
        btn = VNC_BUTTON5;
    else if (strcasecmp(direction, "left") == 0)
        btn = VNC_BUTTON6;
    else if (strcasecmp(direction, "right") == 0)
        btn = VNC_BUTTON7;
    else {
        fprintf(stderr, "[input_vnc] unknown scroll direction: %s\n", direction);
        return -1;
    }

    if (amount <= 0) amount = 3;

    /* Move to position first */
    rfb_pointer_event(fd, nx, ny, 0);
    usleep(10000);

    /* Send scroll events */
    for (int i = 0; i < amount; i++) {
        rfb_pointer_event(fd, nx, ny, btn);
        usleep(20000);
        rfb_pointer_event(fd, nx, ny, 0);
        usleep(20000);
    }
    return 0;
}

static int vnc_drag(input_t *in, int x1, int y1, int x2, int y2,
                    const char *button) {
    int fd = get_sock_fd(in);
    if (fd < 0) return -1;

    int nx1, ny1, nx2, ny2;
    map_coords(in, x1, y1, &nx1, &ny1);
    map_coords(in, x2, y2, &nx2, &ny2);
    uint8_t mask = button_mask_from_name(button);

    /* Move to start, press, move to end, release */
    rfb_pointer_event(fd, nx1, ny1, 0);
    usleep(50000);
    rfb_pointer_event(fd, nx1, ny1, mask);
    usleep(50000);

    /* Interpolate movement for smooth drag */
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int ix = nx1 + (nx2 - nx1) * i / steps;
        int iy = ny1 + (ny2 - ny1) * i / steps;
        rfb_pointer_event(fd, ix, iy, mask);
        usleep(20000);
    }

    rfb_pointer_event(fd, nx2, ny2, 0);  /* release */
    return 0;
}

/* ── Close ──────────────────────────────────────────────────── */

static void vnc_close(input_t *in) {
    /* Don't close the socket — display owns it */
    free(in);
}

/* ── Open ───────────────────────────────────────────────────── */

input_t *input_open_vnc(const input_config_t *cfg) {
    input_t *in = calloc(1, sizeof(*in));
    if (!in) return NULL;

    in->type = INPUT_VNC;
    in->cfg = *cfg;
    in->vnc_client = NULL;  /* Set later via input_vnc_set_client() */

    in->key_fn      = vnc_key;
    in->combo_fn    = vnc_combo;
    in->type_fn     = vnc_type_text;
    in->move_fn     = vnc_move;
    in->click_fn    = vnc_click;
    in->dblclick_fn = vnc_double_click;
    in->scroll_fn   = vnc_scroll;
    in->drag_fn     = vnc_drag;
    in->close_fn    = vnc_close;

    return in;
}

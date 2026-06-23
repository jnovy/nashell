/* input.c — Input backend dispatcher.
 *
 * Routes input_open/key/type/click/close to the active backend.
 * See docs/design-device-control.md §3.2 */

#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Internal input structure — must match layout in input_vnc.c */
struct input_t {
    input_config_t cfg;
    input_type_t   type;

    /* VNC-specific: raw socket fd stored as (void *)(intptr_t)fd */
    void          *vnc_client;   /* cast: (intptr_t)sock_fd */

    /* Backend function pointers */
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

/* ── Public API ──────────────────────────────────────────────── */

input_t *input_open(const input_config_t *cfg) {
    switch (cfg->type) {
    case INPUT_VNC:
        return input_open_vnc(cfg);
    case INPUT_HID_BRIDGE:
        fprintf(stderr, "[input] HID bridge backend not yet implemented (Phase 2)\n");
        return NULL;
    case INPUT_CMD:
        fprintf(stderr, "[input] command backend not yet implemented (Phase 3)\n");
        return NULL;
    default:
        fprintf(stderr, "[input] unknown backend type %d\n", cfg->type);
        return NULL;
    }
}

void input_vnc_set_sock_fd(input_t *in, int sock_fd) {
    if (in && in->type == INPUT_VNC)
        in->vnc_client = (void *)(intptr_t)sock_fd;
}

int input_key(input_t *in, const char *keyname) {
    if (!in || !in->key_fn) return -1;
    return in->key_fn(in, keyname);
}

int input_combo(input_t *in, const char **keys, int nkeys) {
    if (!in || !in->combo_fn) return -1;
    return in->combo_fn(in, keys, nkeys);
}

int input_type(input_t *in, const char *text) {
    if (!in || !in->type_fn) return -1;
    return in->type_fn(in, text);
}

int input_mouse_move(input_t *in, int x, int y) {
    if (!in || !in->move_fn) return -1;
    return in->move_fn(in, x, y);
}

int input_click(input_t *in, int x, int y, const char *button) {
    if (!in || !in->click_fn) return -1;
    return in->click_fn(in, x, y, button);
}

int input_double_click(input_t *in, int x, int y, const char *button) {
    if (!in || !in->dblclick_fn) return -1;
    return in->dblclick_fn(in, x, y, button);
}

int input_scroll(input_t *in, int x, int y, const char *direction, int amount) {
    if (!in || !in->scroll_fn) return -1;
    return in->scroll_fn(in, x, y, direction, amount);
}

int input_drag(input_t *in, int x1, int y1, int x2, int y2, const char *button) {
    if (!in || !in->drag_fn) return -1;
    return in->drag_fn(in, x1, y1, x2, y2, button);
}

void input_close(input_t *in) {
    if (!in) return;
    if (in->close_fn) in->close_fn(in);
    else free(in);
}

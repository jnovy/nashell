/* tool_device.c — device_control tool handler.
 *
 * Enables the LLM to see and control a device's GUI.
 * Uses lazy-initialized device_session_t from config.
 * Phase 1: VNC backend only.
 *
 * See docs/design-device-control.md §4 */

#include "tools_internal.h"
#include "device.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Lazy-initialized device session (one per process) ──────── */

static device_session_t *g_device_session = NULL;

static device_session_t *get_or_create_session(tool_ctx_t *ctx) {
    if (g_device_session) return g_device_session;

    const config_t *cfg = ctx->cfg;

    /* Build display config from config_t */
    display_config_t dcfg = {0};
    dcfg.type = cfg->device_control.display_type;
    dcfg.vnc_host = cfg->device_control.vnc_host;
    dcfg.vnc_port = cfg->device_control.vnc_port;
    dcfg.vnc_password = cfg->device_control.vnc_password;
    dcfg.webcam_device = cfg->device_control.webcam_device;
    dcfg.calibration_file = cfg->device_control.calibration_file;
    dcfg.capture_cmd = cfg->device_control.capture_cmd;
    dcfg.screenshot_dir = cfg->device_control.screenshot_dir;

    /* Build input config */
    input_config_t icfg = {0};
    icfg.type = cfg->device_control.input_type;
    icfg.serial_port = cfg->device_control.serial_port;
    icfg.serial_baud = cfg->device_control.serial_baud;
    icfg.key_cmd_fmt = cfg->device_control.key_cmd;
    icfg.type_cmd_fmt = cfg->device_control.type_cmd;
    icfg.click_cmd_fmt = cfg->device_control.click_cmd;
    /* Dimensions auto-detected from display backend after open.
     * input_set_dimensions() called below with VNC framebuffer size. */

    g_device_session = device_session_open(
        &dcfg, &icfg,
        cfg->device_control.max_actions,
        cfg->device_control.action_delay_ms,
        cfg->device_control.screenshot_delay_ms);

    if (!g_device_session) {
        nash_log("[tool_device] failed to open device session");
    } else {
        /* Auto-detect dimensions from display backend (VNC ServerInit, etc.)
         * and set input coordinate mapping accordingly. */
        int fw = 0, fh = 0;
        display_get_dimensions(g_device_session->display, &fw, &fh);
        if (fw > 0 && fh > 0) {
            /* Model sees native coords directly (no downscaling yet) */
            input_set_dimensions(g_device_session->input, fw, fh, fw, fh);
            nash_log("[tool_device] device session ready (%dx%d)", fw, fh);
        } else {
            nash_log("[tool_device] device session ready (dimensions unknown)");
        }
    }

    return g_device_session;
}

/* ── Action handlers ────────────────────────────────────────── */

static tool_result_t do_screenshot(device_session_t *s) {
    char *path = display_capture(s->display);
    if (!path) return tools_make_error("screenshot capture failed");

    s->screenshot_count++;

    /* For Phase 1: return screenshot path and dimensions.
     * Phase 2+ will add perception pipeline (OmniParser + OCR). */
    int w = 0, h = 0;
    display_get_dimensions(s->display, &w, &h);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "screenshot");
    cJSON_AddStringToObject(meta, "screenshot_path", path);
    cJSON_AddNumberToObject(meta, "width", w);
    cJSON_AddNumberToObject(meta, "height", h);
    cJSON_AddNumberToObject(meta, "screenshot_number", s->screenshot_count);
    cJSON_AddStringToObject(meta, "note",
        "Screenshot saved to disk. Perception pipeline (widget detection + OCR) "
        "will be added in Phase 2. For now, use image_analyze on the screenshot_path "
        "to see the screen content.");

    free(path);
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_click(device_session_t *s, cJSON *params, int double_click) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    if (!jx || !jy)
        return tools_make_error("click requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    cJSON *jbtn = cJSON_GetObjectItem(params, "button");
    const char *button = (jbtn && jbtn->valuestring) ? jbtn->valuestring : "left";

    int rc;
    if (double_click)
        rc = input_double_click(s->input, x, y, button);
    else
        rc = input_click(s->input, x, y, button);

    if (rc != 0) return tools_make_error("click failed");

    s->action_count++;
    s->consecutive_errors = 0;

    /* Wait for UI to settle */
    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", double_click ? "double_click" : "click");
    cJSON_AddNumberToObject(meta, "x", x);
    cJSON_AddNumberToObject(meta, "y", y);
    cJSON_AddStringToObject(meta, "button", button);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_move(device_session_t *s, cJSON *params) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    if (!jx || !jy)
        return tools_make_error("mouse_move requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    int rc = input_mouse_move(s->input, x, y);
    if (rc != 0) return tools_make_error("mouse_move failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "mouse_move");
    cJSON_AddNumberToObject(meta, "x", x);
    cJSON_AddNumberToObject(meta, "y", y);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_triple_click(device_session_t *s, cJSON *params) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    if (!jx || !jy)
        return tools_make_error("triple_click requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    cJSON *jbtn = cJSON_GetObjectItem(params, "button");
    const char *button = (jbtn && jbtn->valuestring) ? jbtn->valuestring : "left";

    int rc = input_triple_click(s->input, x, y, button);
    if (rc != 0) return tools_make_error("triple_click failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "triple_click");
    cJSON_AddNumberToObject(meta, "x", x);
    cJSON_AddNumberToObject(meta, "y", y);
    cJSON_AddStringToObject(meta, "button", button);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_long_press(device_session_t *s, cJSON *params) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    if (!jx || !jy)
        return tools_make_error("long_press requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    cJSON *jbtn = cJSON_GetObjectItem(params, "button");
    const char *button = (jbtn && jbtn->valuestring) ? jbtn->valuestring : "left";

    cJSON *jhold = cJSON_GetObjectItem(params, "hold_ms");
    int hold_ms = jhold ? jhold->valueint : 500;

    int rc = input_long_press(s->input, x, y, button, hold_ms);
    if (rc != 0) return tools_make_error("long_press failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "long_press");
    cJSON_AddNumberToObject(meta, "x", x);
    cJSON_AddNumberToObject(meta, "y", y);
    cJSON_AddStringToObject(meta, "button", button);
    cJSON_AddNumberToObject(meta, "hold_ms", hold_ms);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_type_text(device_session_t *s, cJSON *params) {
    cJSON *jtext = cJSON_GetObjectItem(params, "text");
    if (!jtext || !jtext->valuestring)
        return tools_make_error("type requires 'text' parameter (the string to type into the focused element)");

    int rc = input_type(s->input, jtext->valuestring);
    if (rc != 0) return tools_make_error("type failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "type");
    cJSON_AddNumberToObject(meta, "chars_typed", (int)strlen(jtext->valuestring));
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_key(device_session_t *s, cJSON *params) {
    cJSON *jkey = cJSON_GetObjectItem(params, "key_name");
    if (!jkey || !jkey->valuestring)
        return tools_make_error("key requires 'key_name' parameter (e.g. enter, tab, escape, backspace, ctrl+c, alt+tab)");

    int rc = input_key(s->input, jkey->valuestring);
    if (rc != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "key '%s' failed — check spelling. Common keys: enter, tab, escape, backspace, delete, space, ctrl+c, alt+tab, super", jkey->valuestring);
        return tools_make_error(msg);
    }

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "key");
    cJSON_AddStringToObject(meta, "key_name", jkey->valuestring);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_scroll(device_session_t *s, cJSON *params) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    int x = jx ? jx->valueint : 0;
    int y = jy ? jy->valueint : 0;

    cJSON *jdir = cJSON_GetObjectItem(params, "direction");
    if (!jdir || !jdir->valuestring)
        return tools_make_error("scroll requires 'direction' parameter (up, down, left, right)");

    cJSON *jamt = cJSON_GetObjectItem(params, "amount");
    int amount = jamt ? jamt->valueint : 3;

    int rc = input_scroll(s->input, x, y, jdir->valuestring, amount);
    if (rc != 0) return tools_make_error("scroll failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "scroll");
    cJSON_AddStringToObject(meta, "direction", jdir->valuestring);
    cJSON_AddNumberToObject(meta, "amount", amount);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_drag(device_session_t *s, cJSON *params) {
    cJSON *jsx = cJSON_GetObjectItem(params, "start_x");
    cJSON *jsy = cJSON_GetObjectItem(params, "start_y");
    cJSON *jex = cJSON_GetObjectItem(params, "end_x");
    cJSON *jey = cJSON_GetObjectItem(params, "end_y");
    if (!jsx || !jsy || !jex || !jey)
        return tools_make_error("drag requires start_x, start_y, end_x, end_y (integer pixel coordinates)");

    cJSON *jbtn = cJSON_GetObjectItem(params, "button");
    const char *button = (jbtn && jbtn->valuestring) ? jbtn->valuestring : "left";

    int rc = input_drag(s->input,
                        jsx->valueint, jsy->valueint,
                        jex->valueint, jey->valueint, button);
    if (rc != 0) return tools_make_error("drag failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "drag");
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

static tool_result_t do_wait(device_session_t *s, cJSON *params) {
    cJSON *jdur = cJSON_GetObjectItem(params, "duration_ms");
    int dur_ms = jdur ? jdur->valueint : 1000;

    /* Clamp to max 10 seconds */
    if (dur_ms > 10000) dur_ms = 10000;
    if (dur_ms < 0) dur_ms = 0;

    usleep(dur_ms * 1000);

    (void)s;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "wait");
    cJSON_AddNumberToObject(meta, "duration_ms", dur_ms);
    cJSON_AddStringToObject(meta, "status", "ok");
    return tools_make_result(1, meta, NULL);
}

/* ── Main tool entry point ──────────────────────────────────── */

tool_result_t tool_device_control(tool_ctx_t *ctx, cJSON *params) {
    tools_inject_thought(ctx, params);

    /* Get the command (renamed from "action" to avoid collision with
     * the tool-routing "action" key in the unified JSON object —
     * see provider_anthropic.c build_unified) */
    cJSON *jaction = cJSON_GetObjectItem(params, "command");
    if (!jaction || !jaction->valuestring)
        return tools_make_error(
            "device_control requires a 'command' parameter "
            "(screenshot, click, double_click, triple_click, type, key, scroll, drag, "
            "mouse_move, long_press, wait)");
    const char *action = jaction->valuestring;

    /* Check if device control is configured */
    if (!ctx->cfg->device_control.vnc_host &&
        !ctx->cfg->device_control.webcam_device &&
        !ctx->cfg->device_control.capture_cmd) {
        return tools_make_error(
            "device_control is not configured. Add a [device_control] section "
            "to config.toml with at least vnc_host or webcam_device.");
    }

    /* Get or create session (lazy init) */
    device_session_t *s = get_or_create_session(ctx);
    if (!s)
        return tools_make_error(
            "failed to initialize device session. Check [device_control] config "
            "and ensure the VNC server is reachable.");

    /* Safety: check action limit */
    if (s->action_count >= s->max_actions) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "action limit reached (%d/%d). Take a screenshot to "
                 "verify state, then call done().",
                 s->action_count, s->max_actions);
        return tools_make_error(msg);
    }

    /* Dispatch command and capture result for journaling */
    tool_result_t tr;
    if (strcmp(action, "screenshot") == 0)
        tr = do_screenshot(s);
    else if (strcmp(action, "click") == 0)
        tr = do_click(s, params, 0);
    else if (strcmp(action, "double_click") == 0)
        tr = do_click(s, params, 1);
    else if (strcmp(action, "type") == 0)
        tr = do_type_text(s, params);
    else if (strcmp(action, "key") == 0)
        tr = do_key(s, params);
    else if (strcmp(action, "scroll") == 0)
        tr = do_scroll(s, params);
    else if (strcmp(action, "drag") == 0)
        tr = do_drag(s, params);
    else if (strcmp(action, "wait") == 0)
        tr = do_wait(s, params);
    else if (strcmp(action, "mouse_move") == 0)
        tr = do_move(s, params);
    else if (strcmp(action, "triple_click") == 0)
        tr = do_triple_click(s, params);
    else if (strcmp(action, "long_press") == 0)
        tr = do_long_press(s, params);
    else if (strcmp(action, "screenshot_diff") == 0 ||
             strcmp(action, "screenshot_region") == 0 ||
             strcmp(action, "find") == 0) {
        char emsg[128];
        snprintf(emsg, sizeof(emsg),
                 "'%s' action requires the perception pipeline (Phase 2)", action);
        tr = tools_make_error(emsg);
    } else {
        char emsg[256];
        snprintf(emsg, sizeof(emsg),
                 "unknown device_control command: '%s'. "
                 "Valid commands: screenshot, click, double_click, triple_click, type, key, "
                 "scroll, drag, mouse_move, long_press, wait",
                 action);
        tr = tools_make_error(emsg);
    }

    /* Journal the result so it appears in reactRX.md */
    {
        char *meta_str = tr.meta ? cJSON_PrintUnformatted(tr.meta) : NULL;
        char *hash = store_save(ctx->store, meta_str ? meta_str : "{}");
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        const char *err = NULL;
        if (!tr.success && tr.meta) {
            cJSON *ej = cJSON_GetObjectItem(tr.meta, "error");
            if (ej && ej->valuestring) err = ej->valuestring;
        }
        tool_journal(ctx, "device_control", params, alias,
                    meta_str ? strlen(meta_str) : 0, 0, err, NULL);
        free(alias);
        free(hash);
        free(meta_str);
    }

    return tr;
}

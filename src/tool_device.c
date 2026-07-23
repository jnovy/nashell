/* tool_device.c — device_control tool handler.
 *
 * Enables the LLM to see and control a device's GUI.
 * Uses lazy-initialized device_session_t from config.
 * Phase 1: VNC backend only.
 *
 * See docs/design-device-control.md §4 */

#include "tools_internal.h"
#include "tool_plugin.h"
#include "device.h"
#include "config.h"
#include "perception.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>

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

    /* Build stream config (continuous HEVC capture) */
    stream_config_t scfg = {0};
    stream_config_t *scfg_ptr = NULL;
    if (cfg->device_control.stream_enabled) {
        scfg.fps = cfg->device_control.stream_fps;
        scfg.quality = cfg->device_control.stream_quality;
        scfg.retention_secs = cfg->device_control.stream_retention;
        scfg.preset = cfg->device_control.stream_preset;
        scfg.keyframe_interval = cfg->device_control.stream_keyframe_interval;
        scfg.stream_dir = cfg->device_control.screenshot_dir;
        scfg_ptr = &scfg;
    }

    g_device_session = device_session_open(
        &dcfg, &icfg, scfg_ptr,
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

static tool_result_t do_screenshot(device_session_t *s, cJSON *params) {
    char *path = NULL;
    int w = 0, h = 0;

    if (s->stream) {
        /* Stream mode: extract frame instantly from background capture.
         * Optional seconds_ago parameter for historical frame access. */
        struct timespec *ts_ptr = NULL;
        struct timespec ts;
        cJSON *jtime = cJSON_GetObjectItem(params, "seconds_ago");
        if (jtime && cJSON_IsNumber(jtime)) {
            double ago = jtime->valuedouble;
            if (ago < 0) ago = 0;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec -= (time_t)ago;
            long frac_ns = (long)((ago - (double)(time_t)ago) * 1e9);
            ts.tv_nsec -= frac_ns;
            if (ts.tv_nsec < 0) { ts.tv_sec--; ts.tv_nsec += 1000000000L; }
            ts_ptr = &ts;
        }
        path = stream_extract_frame(s->stream, ts_ptr, &w, &h);
    } else {
        /* On-demand mode: direct VNC capture (legacy path) */
        path = display_capture(s->display);
        if (path) display_get_dimensions(s->display, &w, &h);
    }

    if (!path) return tools_make_error("screenshot capture failed");

    s->screenshot_count++;

    /* Run perception pipeline: native Tesseract OCR */
    cJSON *perception = perception_analyze(path);
    if (!perception) {
        free(path);
        return tools_make_error("perception analysis failed");
    }

    /* Check for error field */
    cJSON *perr = cJSON_GetObjectItem(perception, "error");
    if (perr && cJSON_IsString(perr)) {
        char emsg[512];
        snprintf(emsg, sizeof(emsg), "perception error: %s", perr->valuestring);
        cJSON_Delete(perception);
        free(path);
        return tools_make_error(emsg);
    }

    /* Pass the full perception JSON to the LLM — add meta fields into it */
    cJSON_AddStringToObject(perception, "action", "screenshot");
    cJSON_ReplaceItemInObject(perception, "screenshot_path",
                              cJSON_CreateString(path));
    cJSON_AddNumberToObject(perception, "width", w);
    cJSON_AddNumberToObject(perception, "height", h);
    cJSON_AddNumberToObject(perception, "screenshot_number", s->screenshot_count);

    free(path);
    return tools_make_result(1, perception, NULL);
}

static tool_result_t do_click(device_session_t *s, cJSON *params,
                              const char *action_name, const char *button,
                              int dbl_click) {
    cJSON *jx = cJSON_GetObjectItem(params, "x");
    cJSON *jy = cJSON_GetObjectItem(params, "y");
    if (!jx || !jy)
        return tools_make_error("click requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    int rc;
    if (dbl_click)
        rc = input_double_click(s->input, x, y, button);
    else
        rc = input_click(s->input, x, y, button);

    if (rc != 0) return tools_make_error("click action failed");

    s->action_count++;
    s->consecutive_errors = 0;

    /* Wait for UI to settle */
    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", action_name);
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
        return tools_make_error("move requires 'x' and 'y' coordinates (integer pixel values)");

    int x = jx->valueint;
    int y = jy->valueint;

    int rc = input_mouse_move(s->input, x, y);
    if (rc != 0) return tools_make_error("move failed");

    s->action_count++;
    s->consecutive_errors = 0;

    if (s->action_delay_ms > 0)
        usleep(s->action_delay_ms * 1000);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "action", "move");
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
            "(screenshot, left_click, right_click, middle_click, double_click, "
            "triple_click, type, key, scroll, drag, move, long_press)");
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

    /* Dispatch command and capture result for journaling */
    tool_result_t tr;
    if (strcmp(action, "screenshot") == 0 ||
        strcmp(action, "screenshot_parse") == 0)   /* backward compat alias */
        tr = do_screenshot(s, params);
    else if (strcmp(action, "left_click") == 0)
        tr = do_click(s, params, "left_click", "left", 0);
    else if (strcmp(action, "right_click") == 0)
        tr = do_click(s, params, "right_click", "right", 0);
    else if (strcmp(action, "middle_click") == 0)
        tr = do_click(s, params, "middle_click", "middle", 0);
    else if (strcmp(action, "double_click") == 0)
        tr = do_click(s, params, "double_click", "left", 1);
    else if (strcmp(action, "triple_click") == 0)
        tr = do_triple_click(s, params);
    else if (strcmp(action, "type") == 0)
        tr = do_type_text(s, params);
    else if (strcmp(action, "key") == 0)
        tr = do_key(s, params);
    else if (strcmp(action, "scroll") == 0)
        tr = do_scroll(s, params);
    else if (strcmp(action, "drag") == 0)
        tr = do_drag(s, params);
    else if (strcmp(action, "move") == 0)
        tr = do_move(s, params);
    else if (strcmp(action, "long_press") == 0)
        tr = do_long_press(s, params);
    else if (strcmp(action, "stream_status") == 0) {
        if (!s->stream) {
            tr = tools_make_error("stream not enabled. Set stream_enabled = true in [device_control] config.");
        } else {
            struct timespec oldest = {0}, newest = {0};
            uint64_t total = 0;
            stream_get_range(s->stream, &oldest, &newest, &total);
            cJSON *meta = cJSON_CreateObject();
            cJSON_AddStringToObject(meta, "action", "stream_status");
            cJSON_AddNumberToObject(meta, "total_frames", (double)total);
            cJSON_AddNumberToObject(meta, "history_available_secs",
                (double)(newest.tv_sec - oldest.tv_sec));
            cJSON_AddStringToObject(meta, "status", "ok");
            tr = tools_make_result(1, meta, NULL);
        }
    }
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
                 "Valid: screenshot, left_click, right_click, middle_click, "
                 "double_click, triple_click, type, key, scroll, drag, move, long_press",
                 action);
        tr = tools_make_error(emsg);
    }

    /* Journal the result so it appears in reactRX.md */
    {
        char *store_content = tr.meta ? cJSON_Print(tr.meta) : strdup("{}");
        char *hash = store_save(ctx->store, store_content ? store_content : "{}");
        char *alias = tool_register_alias(ctx, hash ? hash : "");
        const char *err = NULL;
        if (!tr.success && tr.meta) {
            cJSON *ej = cJSON_GetObjectItem(tr.meta, "error");
            if (ej && ej->valuestring) err = ej->valuestring;
        }
        tool_journal(ctx, "device_control", params, alias,
                    store_content ? strlen(store_content) : 0, 0, err, NULL);
        free(alias);
        free(hash);
        free(store_content);
    }

    return tr;
}

/* ── Stream lifecycle cleanup ──────────────────────────────────
 *
 * Called from the react loop when the `done` tool fires.
 * Saves the HEVC capture stream into the session directory as
 *   stream-<unix_timestamp>-<duration_secs>.hevc
 * then tears down the device session so the VNC connection and
 * background thread are released cleanly. */

void tool_device_cleanup(const char *session_dir) {
    if (!g_device_session) return;

    /* Save the stream capture to the session directory */
    if (g_device_session->stream) {
        char *path = stream_save(g_device_session->stream, session_dir);
        free(path);
        g_device_session->stream = NULL;  /* prevent double-free in close */
    }

    perception_cleanup();
    device_session_close(g_device_session);
    g_device_session = NULL;
}

/* ── plugin registration ──────────────────────────────── */

static const char *dc_commands[] = {
    "screenshot", "left_click", "right_click", "middle_click",
    "double_click", "triple_click", "type", "key", "scroll",
    "drag", "move", "long_press", NULL
};

static const tool_param_t device_control_params[] = {
    {"command",   "string",  "Command to execute",                                          1, dc_commands, NULL},
    {"x",         "integer", "X coordinate (pixels)",                                       0, NULL, NULL},
    {"y",         "integer", "Y coordinate (pixels)",                                       0, NULL, NULL},
    {"text",      "string",  "Text to type (for type command)",                             0, NULL, NULL},
    {"key_name",  "string",  "Key name (for key command): enter, tab, escape, ctrl+c, etc.",0, NULL, NULL},
    {"button",    "string",  "Mouse button for drag: left, right, middle (default: left)",  0, NULL, NULL},
    {"direction", "string",  "Scroll direction: up, down, left, right",                     0, NULL, NULL},
    {"amount",    "integer", "Scroll amount (default: 3)",                                  0, NULL, NULL},
    {"start_x",   "integer", "Drag start X",                                                0, NULL, NULL},
    {"start_y",   "integer", "Drag start Y",                                                0, NULL, NULL},
    {"end_x",     "integer", "Drag end X",                                                  0, NULL, NULL},
    {"end_y",     "integer", "Drag end Y",                                                  0, NULL, NULL},
    {"hold_ms",   "integer", "Hold duration in ms for long_press (default: 500, min: 100, max: 10000)", 0, NULL, NULL},
    {0}
};

static const tool_plugin_t device_control_plugin = {
    .abi_version = TOOL_PLUGIN_ABI_VERSION,
    .name        = "device_control",
    .version     = "1.0.0",
    .description =
        "Control a device's GUI (computer, phone, tablet, kiosk). "
        "Workflow: screenshot to see+parse the screen (OmniParser + OCR), then act, then screenshot to verify.\\n"
        "Commands and parameters:\\n"
        "- screenshot: capture + run OmniParser widget detection + Tesseract OCR. Returns detected widgets with coordinates and all screen text\\n"
        "- left_click: x, y (required). Standard click\\n"
        "- right_click: x, y (required). Context menu\\n"
        "- middle_click: x, y (required)\\n"
        "- double_click: x, y (required). Open file / select word\\n"
        "- triple_click: x, y (required). Select entire line or paragraph\\n"
        "- type: text (required). Types into the focused element\\n"
        "- key: key_name (required). Examples: enter, tab, escape, backspace, space, delete, ctrl+c, alt+tab, super\\n"
        "- scroll: direction (required: up/down/left/right), optional x, y, amount (default 3)\\n"
        "- drag: start_x, start_y, end_x, end_y (all required), optional button\\n"
        "- move: x, y (required). Move cursor without clicking\\n"
        "- long_press: x, y (required), hold_ms (default 500). For mobile/tablet context menus\\n"
        "Only for visual/GUI tasks. For CLI delays, use shell_exec with sleep.\\n"
        "IMPORTANT: Prefer keyboard shortcuts (key command with ctrl+s, alt+f4, ctrl+t, etc.) over clicking UI elements when the shortcut is known -- they are faster and more reliable than locating and clicking buttons.\\n"
        "IMPORTANT: device_control is the EXCLUSIVE interface for GUI interaction. "
        "Do NOT use shell_exec with xdotool, xclip, xsel, wmctrl, xprop, xwininfo, "
        "import, scrot, gnome-screenshot, or any other CLI tool to manipulate or capture the GUI. "
        "All clicking, typing, scrolling, dragging, and screenshots must go through device_control commands.",
    .params      = device_control_params,
    .execute     = (void *)tool_device_control,
    .caps        = 0,
    .flags       = TOOL_FLAG_DEFAULT_OFF,
    .group       = NULL,
};
TOOL_PLUGIN_REGISTER(device_control_plugin)

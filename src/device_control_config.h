/* device_control_config.h - Device control configuration.
 *
 * Extracted from config.h so the device_control plugin can use the
 * same struct layout without pulling in all nash internals.
 * Both nash (config.h) and the external plugin include this header. */

#ifndef DEVICE_CONTROL_CONFIG_H
#define DEVICE_CONTROL_CONFIG_H

typedef struct {
    /* Display backend: 0=VNC, 1=webcam, 2=command */
    int    display_type;
    char  *vnc_host;
    int    vnc_port;          /* default 5900 */
    char  *vnc_password;
    char  *webcam_device;
    char  *calibration_file;
    char  *capture_cmd;
    char  *screenshot_dir;    /* default: /tmp/device_screenshots */

    /* Input backend: 0=VNC, 1=HID bridge, 2=command */
    int    input_type;
    char  *serial_port;
    int    serial_baud;       /* default 115200 */
    char  *key_cmd;
    char  *type_cmd;
    char  *click_cmd;

    /* Safety */
    int    action_delay_ms;   /* pause after each action (default 500) */
    int    screenshot_delay_ms; /* wait before screenshot (default 300) */

    /* Continuous HEVC capture stream */
    int    stream_enabled;    /* 1 = start background capture (default 0) */
    int    stream_fps;        /* frames per second (1-30, default 4) */
    int    stream_quality;    /* HEVC CRF value (0-51, default 28) */
    int    stream_retention;  /* seconds of history (default 300) */
    char  *stream_preset;     /* x265 preset (default: "medium") */
    int    stream_keyframe_interval; /* 0 = auto (= fps) */
} device_control_config_t;

#endif /* DEVICE_CONTROL_CONFIG_H */

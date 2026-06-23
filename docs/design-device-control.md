# Nash Device Control — Design Document

## 1. Overview

Add a `device_control` tool to nash that enables the LLM agent to see and
control any device's screen — computers, phones, tablets, kiosks, industrial
equipment, or any machine with a visible display.  The architecture uses a
**perception pipeline** (OmniParser + OCR) that returns structured text
metadata to the model instead of raw images, making it work with any LLM
(no vision capability required) and keeping context usage minimal (~200
tokens per screenshot vs ~2000+ for base64 images).

Two distinct hardware paths are supported:

```
                 ┌─────────────────────────────────────────────────────────┐
                 │                      NASH (laptop)                      │
                 │                                                         │
                 │  react.c ──► tool_device.c ──► display_backend_t        │
                 │       │              │              ├─ VNC(direct RFB) │
                 │       │              │              └─ Webcam (v4l2)    │
                 │       │              │                                  │
                 │       │              ├──► input_backend_t               │
                 │       │              │       ├─ VNC (key/pointer events)│
                 │       │              │       └─ HID bridge (serial UART)│
                 │       │              │                                  │
                 │       │              └──► perception_pipeline_t         │
                 │       │                     ├─ OmniParser (YOLO widgets)│
                 │       │                     ├─ Tesseract OCR (fast)     │
                 │       │                     └─ EasyOCR (thorough)       │
                 │       │                                                 │
                 │       └◄──── structured text metadata (widgets + text)  │
                 └─────────────────────────────────────────────────────────┘
                             │                        │
           ┌─────────────────┘                        └──────────────────┐
           ▼  (Path A: VNC)                              (Path B: KVM)  ▼
   ┌───────────────┐                              ┌──────────────────────┐
   │ TARGET (VNC)  │                              │ TARGET (any device)  │
   │               │                              │                      │
   │ VNC server    │◄── RFB protocol ──           │ USB port ◄── RP2040  │
   │ :5900         │                              │ (sees keyboard+mouse)│
   │               │                              │       ▲              │
   └───────────────┘                              │       │ UART         │
                                                  │  CP2102 adapter      │
   Works with:                                    │  /dev/ttyUSBx        │
   · Computers (Linux/Win/Mac)                    │                      │
   · VMs / containers                             │  Screen ──► Webcam   │
   · Cloud instances                              │              on nash │
                                                  │              laptop  │
                                                  └──────────────────────┘
                                                  Works with:
                                                  · Computers (any OS)
                                                  · Phones (USB OTG)
                                                  · Tablets
                                                  · Kiosks, BIOS/UEFI
                                                  · Air-gapped machines
                                                  · Industrial equipment
```

**Path A (VNC):** Both display capture and input injection go through the
VNC/RFB protocol.  The target runs a VNC server (any OS).  Nash connects as
a VNC client: screenshots via framebuffer reads, input via RFB key/pointer
events.  Zero hardware needed.

**Path B (KVM/HID Bridge):** Physical hardware control.  The RP2040
microcontroller appears as a USB keyboard+mouse to the target.  Nash sends
serial commands over UART via `/dev/ttyUSBx`.  Display capture is via a
webcam pointed at the target's screen (or an HDMI capture card).  Works on
air-gapped machines, BIOS/UEFI, phones, tablets, kiosks — anything with a
USB port and a visible screen.

## 2. Perception Pipeline — Metadata, Not Images

### 2.1 Design Principle

The model **never sees raw screenshot images**.  Instead, every screenshot
goes through a perception pipeline that extracts structured metadata:

```
Screenshot (JPEG/PNG on disk)
    │
    ├──► OmniParser (YOLO v8 widget detection)
    │       → list of {bbox, center, size, conf} for clickable elements
    │
    └──► OCR Engine (Tesseract or EasyOCR)
            → list of {text, bbox, center, conf} for on-screen text
    │
    ├──► Widget-Text Matching (bbox overlap)
    │       → widgets get text labels from overlapping OCR regions
    │
    └──► Structured text summary returned to model
```

**What the model receives** (typically ~200 tokens):

```
Screenshot: /tmp/device_screenshots/screen_1719130000.jpg (15 widgets, 42 texts)

Screen text (42 items):
  [95%] "File" at (45,12) bbox=[20,2,70,22]
  [90%] "Edit" at (95,12) bbox=[75,2,115,22]
  [88%] "View" at (145,12) bbox=[125,2,165,22]
  [92%] "Terminal - bash" at (400,8) bbox=[300,0,500,20]
  ...

Clickable elements (15 widgets):
  "File" at (45,12) bbox=[20,2,70,22] [50x20px] conf=89%
  "Close" at (1250,8) bbox=[1235,0,1265,16] [30x16px] conf=95%
  ...
```

**What stays on disk** (for debugging, not in context): the actual JPEG
screenshot image at `/tmp/device_screenshots/screen_TIMESTAMP.jpg`.

### 2.2 Why Metadata, Not Images

| | Vision (base64 image) | Perception (text metadata) |
|-|----------------------|---------------------------|
| Context cost | ~2000-5000 tokens per screenshot | ~200 tokens per screenshot |
| Model requirement | Vision-capable (GPT-4V, Claude 3) | Any text LLM works |
| Screenshots in context | 3-5 before overflow | 50+ without issue |
| Eviction pressure | Screenshots dominate compaction | Negligible |
| Action precision | Model guesses coordinates from pixels | Exact bbox coordinates from YOLO/OCR |
| Latency | Fast (just encode) | +2-3s for OmniParser + Tesseract |

The perception pipeline approach is proven in nashell's `vnc_control.py`
which successfully uses OmniParser + OCR for GUI automation without any
vision model.

### 2.3 Perception Tiers

```c
typedef enum {
    PERCEPTION_NONE,        /* No analysis — just capture image to disk */
    PERCEPTION_WIDGETS,     /* OmniParser only (~2s) — clickable elements */
    PERCEPTION_FAST,        /* OmniParser + Tesseract (~3s) — DEFAULT */
    PERCEPTION_FULL,        /* OmniParser + EasyOCR (~18s) — highest accuracy */
    PERCEPTION_VISION,      /* Raw image to vision model (fallback for complex UIs) */
} perception_tier_t;
```

The default tier (`PERCEPTION_FAST`) runs OmniParser and Tesseract in
parallel threads.  This balances speed (~3s) with rich perception (both
clickable widgets and on-screen text).

### 2.4 Additional Perception Modes

**Screenshot Diff:** Compare current and previous analysis, return only
changes (added/removed widgets and texts).  Dramatically reduces token
usage for monitoring tasks:

```
Screenshot diff: /tmp/device_screenshots/screen_1719130010.jpg
  Previous: 15 widgets, 42 texts
  Current:  16 widgets, 44 texts

ADDED widgets (1):
  + "Save" at (200,400) bbox=[180,390,220,410]
NEW text (2):
  + "File saved successfully" at (400,500)
  + "3:42 PM" at (1880,12)
```

**Screenshot Region:** Mask everything outside a specified region, analyze
only that area.  Coordinates remain in full-screen space (no remapping
needed).

### 2.5 Vision Mode Fallback

For complex visual UIs where OCR/YOLO miss important elements (games,
image editors, CAD tools), a `PERCEPTION_VISION` tier sends the screenshot
as a base64 image to a vision-capable model.  This reuses the pattern from
`tool_image.c` and is opt-in:

```c
/* Only used when perception = "vision" */
/* Uses existing tool_image.c base64_encode() + provider multimodal support */
```

## 3. Abstraction Layers

### 3.1 Display Backend (`display_t`)

Captures the target's screen as a JPEG/PNG image on disk.

```c
/* src/display.h */
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

    /* Webcam backend */
    char *webcam_device;     /* e.g. "/dev/video0" */
    int   webcam_index;      /* alternative: camera index (0, 1, ...) */

    /* Webcam calibration (see section 5) */
    char *calibration_file;  /* path to calibration data, NULL = no correction */

    /* Command backend */
    char *capture_cmd;       /* shell command that produces a PNG on stdout */

    /* Common */
    int   width;             /* declared display width (pixels) — for coordinate space */
    int   height;            /* declared display height */
} display_config_t;

/* Create a display backend from config. Returns NULL on error. */
display_t *display_open(const display_config_t *cfg);

/* Capture a screenshot and save to disk.
 * Returns path to the saved image (malloc'd), or NULL on error.
 * Caller must free() the path.
 * For webcam backend: applies lens distortion correction if calibrated. */
char *display_capture(display_t *d);

/* Get the declared coordinate space dimensions.
 * For VNC: native framebuffer size.
 * For webcam: calibrated screen area (after perspective correction). */
void display_get_dimensions(display_t *d, int *w, int *h);

/* Close and free the display backend. */
void display_close(display_t *d);

#endif /* DISPLAY_H */
```

### 3.2 Input Backend (`input_t`)

Injects keyboard and mouse events into the target.

```c
/* src/input.h */
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

    /* HID bridge backend */
    char *serial_port;       /* e.g. "/dev/ttyUSB0" */
    int   serial_baud;       /* default 115200 */

    /* Command backend */
    char *key_cmd_fmt;       /* e.g. "xdotool key %s" */
    char *type_cmd_fmt;      /* e.g. "xdotool type '%s'" */
    char *click_cmd_fmt;     /* e.g. "xdotool mousemove %d %d click %d" */

    /* Screen dimensions for coordinate mapping.
     * The model emits coordinates in the display's declared space.
     * If the target's actual resolution differs (e.g. HID bridge
     * with SCREEN_WIDTH=1920 but model sees 1280×800), the input
     * backend maps coordinates: target_x = model_x * native_w / model_w */
    int   native_width;      /* target's actual screen width */
    int   native_height;     /* target's actual screen height */
    int   model_width;       /* display width the model sees (from display backend) */
    int   model_height;      /* display height the model sees */
} input_config_t;

/* Create an input backend from config. Returns NULL on error. */
input_t *input_open(const input_config_t *cfg);

/* Keyboard actions */
int input_key(input_t *in, const char *keyname);           /* press+release */
int input_combo(input_t *in, const char **keys, int nkeys); /* key combination */
int input_type(input_t *in, const char *text);              /* type string */

/* Mouse actions — coordinates in MODEL space (mapped internally) */
int input_mouse_move(input_t *in, int x, int y);   /* absolute position */
int input_click(input_t *in, int x, int y, const char *button); /* click at pos */
int input_double_click(input_t *in, int x, int y, const char *button);
int input_scroll(input_t *in, int x, int y, const char *direction, int amount);
int input_drag(input_t *in, int x1, int y1, int x2, int y2, const char *button);

/* Close and free the input backend. */
void input_close(input_t *in);

#endif /* INPUT_H */
```

### 3.3 Perception Pipeline (`perception_t`)

Extracts structured metadata from screenshots.  Runs OmniParser and OCR
in parallel using a thread pool.

```c
/* src/perception.h */
#ifndef PERCEPTION_H
#define PERCEPTION_H

#include <stddef.h>

typedef enum {
    PERCEPTION_NONE,
    PERCEPTION_WIDGETS,     /* OmniParser only */
    PERCEPTION_FAST,        /* OmniParser + Tesseract (default) */
    PERCEPTION_FULL,        /* OmniParser + EasyOCR */
    PERCEPTION_VISION,      /* Raw image → vision model */
} perception_tier_t;

typedef struct {
    int   bbox[4];          /* [x1, y1, x2, y2] */
    int   center[2];        /* [cx, cy] */
    int   size[2];          /* [width, height] */
    float conf;             /* 0.0-1.0 */
    char *text;             /* label from OCR matching (NULL if unlabeled) */
} widget_t;

typedef struct {
    char *text;             /* recognized text */
    int   bbox[4];          /* [x1, y1, x2, y2] */
    int   center[2];        /* [cx, cy] */
    float conf;             /* 0.0-1.0 */
} ocr_text_t;

typedef struct {
    char        *screenshot_path;  /* path to image on disk */
    widget_t    *widgets;          /* detected clickable elements */
    int          n_widgets;
    ocr_text_t  *texts;            /* recognized text regions */
    int          n_texts;
    perception_tier_t tier;        /* which tier was used */
    double       elapsed_ms;       /* perception latency */
} perception_result_t;

typedef struct perception_t perception_t;

typedef struct {
    perception_tier_t tier;
    char *omniparser_model;     /* path to YOLO model.pt */
    float widget_conf_threshold; /* min confidence for widgets (default 0.15) */
    char *screenshot_dir;        /* where to save screenshots (default /tmp/device_screenshots) */
} perception_config_t;

/* Create perception pipeline. */
perception_t *perception_open(const perception_config_t *cfg);

/* Analyze a screenshot. Returns result with widgets + texts.
 * The screenshot_path in result is owned by the result and freed on close. */
perception_result_t *perception_analyze(perception_t *p, const char *image_path);

/* Diff two analysis results. Returns text summary of changes.
 * Caller must free the returned string. */
char *perception_diff(const perception_result_t *prev,
                      const perception_result_t *curr);

/* Format result as human-readable text summary for the model.
 * Caller must free the returned string. */
char *perception_format(const perception_result_t *r);

/* Find widget/text matching a query string. Returns center coords or -1,-1. */
int perception_find(const perception_result_t *r, const char *query,
                    int *out_x, int *out_y);

/* Free a perception result. */
void perception_result_free(perception_result_t *r);

/* Close and free the pipeline. */
void perception_close(perception_t *p);

#endif /* PERCEPTION_H */
```

**Implementation** (`src/perception.c`): The actual perception runs as
external processes to avoid linking YOLO/Python into nash's C binary:

```c
/* OmniParser: shell out to Python one-liner that loads YOLO + runs inference */
static widget_t *run_omniparser(const char *image_path, int *n_out) {
    /* python3 -c "from ultralytics import YOLO; m = YOLO('model.pt');
     * r = m('image.jpg', verbose=False); import json;
     * print(json.dumps([{'bbox':[...], 'conf':...} for ...]))" */
    /* Parse JSON output → widget_t array */
}

/* Tesseract: shell out to tesseract CLI */
static ocr_text_t *run_tesseract(const char *image_path, int *n_out) {
    /* tesseract image.jpg stdout --psm 3 tsv */
    /* Parse TSV → ocr_text_t array */
}

/* Run both in parallel using fork() or pthread */
perception_result_t *perception_analyze(perception_t *p, const char *path) {
    /* if tier >= PERCEPTION_WIDGETS: fork OmniParser */
    /* if tier >= PERCEPTION_FAST: fork Tesseract (parallel) */
    /* wait for both, merge results */
    /* Match OCR text to widgets by bbox overlap */
}
```

### 3.4 Device Session (`device_session_t`)

Binds a display + input + perception pipeline together with session state.

```c
/* src/device.h */
#ifndef DEVICE_H
#define DEVICE_H

#include "display.h"
#include "input.h"
#include "perception.h"

/* A device session: display capture + input injection + perception */
typedef struct {
    display_t      *display;
    input_t        *input;
    perception_t   *perception;

    /* Session tracking */
    int             screenshot_count;   /* total screenshots taken */
    int             action_count;       /* total actions performed */
    int             consecutive_errors; /* for retry/abort logic */

    /* Previous perception result (for diff mode) */
    perception_result_t *prev_result;

    /* Safety */
    int             max_actions;        /* hard limit per react loop (default 50) */
    int             action_delay_ms;    /* ms to wait after each action (default 500) */
    int             screenshot_delay_ms;/* ms to wait after action before screenshot (default 300) */
} device_session_t;

/* Open a device session with the given configs. */
device_session_t *device_session_open(const display_config_t *dcfg,
                                       const input_config_t *icfg,
                                       const perception_config_t *pcfg);

/* Close a device session (closes display, input, and perception). */
void device_session_close(device_session_t *s);

#endif /* DEVICE_H */
```

## 4. The `device_control` Tool

### 4.1 Tool Definition (for `tools_registry.c`)

```c
{"device_control",
 "Control a device's GUI (computer, phone, tablet, kiosk). Takes a screenshot "
 "with perception analysis (widget detection + OCR) and/or performs keyboard/"
 "mouse actions. Coordinates come from the perception output's bbox/center "
 "fields. Actions: screenshot, screenshot_diff, screenshot_region, click, "
 "double_click, type, key, scroll, drag, wait, find.",
 "{\"type\":\"object\",\"properties\":{"
   "\"action\":{\"type\":\"string\",\"description\":"
     "\"Action: screenshot, screenshot_diff, screenshot_region, click, "
     "double_click, type, key, scroll, drag, wait, find\"},"
   "\"x\":{\"type\":\"integer\",\"description\":\"X coordinate (pixels)\"},"
   "\"y\":{\"type\":\"integer\",\"description\":\"Y coordinate (pixels)\"},"
   "\"text\":{\"type\":\"string\",\"description\":"
     "\"Text to type (for type action) or text to find (for find action)\"},"
   "\"key_name\":{\"type\":\"string\",\"description\":"
     "\"Key name (for key action): enter, tab, escape, ctrl+c, etc.\"},"
   "\"button\":{\"type\":\"string\",\"description\":"
     "\"Mouse button: left, right, middle (default: left)\"},"
   "\"direction\":{\"type\":\"string\",\"description\":"
     "\"Scroll direction: up, down, left, right\"},"
   "\"amount\":{\"type\":\"integer\",\"description\":"
     "\"Scroll amount (default: 3)\"},"
   "\"start_x\":{\"type\":\"integer\",\"description\":\"Drag start X\"},"
   "\"start_y\":{\"type\":\"integer\",\"description\":\"Drag start Y\"},"
   "\"end_x\":{\"type\":\"integer\",\"description\":\"Drag end X\"},"
   "\"end_y\":{\"type\":\"integer\",\"description\":\"Drag end Y\"},"
   "\"perception\":{\"type\":\"string\",\"description\":"
     "\"Perception tier: none, widgets, fast (default), full, vision\"},"
   "\"cx\":{\"type\":\"integer\",\"description\":\"Region center X (for screenshot_region)\"},"
   "\"cy\":{\"type\":\"integer\",\"description\":\"Region center Y (for screenshot_region)\"},"
   "\"dx\":{\"type\":\"integer\",\"description\":\"Region half-width (default 200)\"},"
   "\"dy\":{\"type\":\"integer\",\"description\":\"Region half-height (default 150)\"},"
   "\"duration_ms\":{\"type\":\"integer\",\"description\":"
     "\"Wait duration in ms (for wait action, max 10000)\"}"
 "},\"required\":[\"action\"]}"},
```

### 4.2 Tool Handler Flow (`src/tool_device.c`)

```
tool_device_control(ctx, params)
│
├─ Parse "action" from params
├─ Get or create device_session from ctx (lazy init)
│
├─ action == "screenshot"?
│   ├─ display_capture() → saves image to disk
│   ├─ perception_analyze() → widgets + texts
│   ├─ perception_format() → structured text summary
│   ├─ Store text summary to shared store
│   └─ Return tool_result_t with text metadata (NO image in context)
│
├─ action == "screenshot_diff"?
│   ├─ display_capture() → new screenshot
│   ├─ perception_analyze() → current result
│   ├─ perception_diff(prev, current) → changes only
│   ├─ Update prev_result cache
│   └─ Return only added/removed widgets and texts
│
├─ action == "screenshot_region"?
│   ├─ display_capture() → full screenshot
│   ├─ Mask everything outside region (cx,cy ± dx,dy)
│   ├─ perception_analyze() on masked image
│   ├─ Coordinates remain in full-screen space
│   └─ Return region analysis
│
├─ action == "click" / "double_click"?
│   ├─ Extract x, y, button from params
│   ├─ input_click() / input_double_click()
│   ├─ sleep(screenshot_delay_ms)
│   ├─ Optional: auto-screenshot + perception
│   └─ Return result with confirmation
│
├─ action == "type"?
│   ├─ Extract text from params
│   ├─ input_type()
│   └─ Return result
│
├─ action == "key"?
│   ├─ Parse key_name (handle "ctrl+c" → combo)
│   ├─ input_key() or input_combo()
│   └─ Return result
│
├─ action == "scroll"?
│   ├─ Extract x, y, direction, amount
│   ├─ input_scroll()
│   └─ Return result
│
├─ action == "drag"?
│   ├─ Extract start_x/y, end_x/y, button
│   ├─ input_drag()
│   └─ Return result
│
├─ action == "find"?
│   ├─ Take screenshot + perception if needed
│   ├─ perception_find(result, text) → coordinates
│   └─ Return found element's coordinates and context
│
├─ action == "wait"?
│   ├─ Poll with Tesseract until text appears or timeout
│   └─ Return result with match or timeout error
│
└─ Unknown action → tools_make_error()
```

### 4.3 Tool Result — Text Metadata Only

The tool returns **structured text**, not images.  The screenshot image is
saved to disk for debugging but never enters the model's context:

```c
/* Example tool result for device_control(action="screenshot") */
tool_result_t tool_device_control(tool_ctx_t *ctx, cJSON *params) {
    /* ... capture + perception ... */

    /* Format perception result as text */
    char *summary = perception_format(result);

    /* Store summary to shared store */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "summary", summary);
    cJSON_AddStringToObject(meta, "screenshot_path", result->screenshot_path);
    cJSON_AddNumberToObject(meta, "widget_count", result->n_widgets);
    cJSON_AddNumberToObject(meta, "text_count", result->n_texts);

    free(summary);
    perception_result_free(result);

    /* No image_b64, no image_mime — just text metadata */
    return tools_make_result(meta, NULL, TOOL_IMPORTANCE_HIGH);
}
```

Compare with the vision-model approach (which sends base64 images and
consumes ~5000 tokens per screenshot): the perception approach returns
~200 tokens of structured text with exact coordinates, and the model
can take as many screenshots as needed without context pressure.

## 5. Webcam Calibration

When using a webcam to capture the target's screen (Path B / KVM), the
camera image differs from what's actually on screen due to:

1. **Lens distortion** — barrel/pincushion from wide-angle lenses
2. **Perspective distortion** — camera not perfectly perpendicular to screen
3. **Field of view mismatch** — webcam sees more than just the screen
4. **Color/brightness** — different from actual screen pixels (affects OCR)

### 5.1 Calibration Data

```c
/* src/calibration.h */
#ifndef CALIBRATION_H
#define CALIBRATION_H

typedef struct {
    /* Camera intrinsic matrix (3×3) — from OpenCV calibration
     * [fx  0  cx]
     * [ 0 fy  cy]
     * [ 0  0   1]  */
    double camera_matrix[9];

    /* Distortion coefficients (k1, k2, p1, p2, k3) — radial + tangential */
    double dist_coeffs[5];

    /* Perspective transform (3×3 homography matrix)
     * Maps webcam pixel → screen pixel after undistortion.
     * Computed from 4+ known screen corner positions in webcam frame. */
    double homography[9];

    /* Screen region in webcam frame (after undistortion)
     * Used to crop to just the screen area */
    int    screen_roi[4];   /* [x, y, width, height] in webcam pixels */

    /* Target screen resolution */
    int    screen_width;
    int    screen_height;

    /* Validation: reprojection error from calibration */
    double reproj_error;    /* pixels, should be < 1.0 for good calibration */

    /* Calibration timestamp */
    long   calibrated_at;   /* unix timestamp */
} calibration_data_t;

/* Load calibration from JSON file. Returns 0 on success. */
int calibration_load(calibration_data_t *cal, const char *path);

/* Save calibration to JSON file. Returns 0 on success. */
int calibration_save(const calibration_data_t *cal, const char *path);

/* Apply calibration to a webcam frame:
 * 1. Undistort (remove lens distortion)
 * 2. Perspective-correct (rectify to flat screen plane)
 * 3. Crop to screen ROI
 * 4. Resize to target screen dimensions
 * Returns malloc'd corrected image data, or NULL on error. */
unsigned char *calibration_apply(const calibration_data_t *cal,
                                  const unsigned char *frame,
                                  int frame_w, int frame_h,
                                  size_t *out_len);

/* Map a point from corrected image space to target screen coordinates.
 * Used after calibration_apply() for coordinate mapping. */
void calibration_map_point(const calibration_data_t *cal,
                            int img_x, int img_y,
                            int *screen_x, int *screen_y);

#endif /* CALIBRATION_H */
```

### 5.2 Calibration Procedure

The calibration process runs via the standalone `nash-calibrate` binary
(or invoked by `nash --calibrate-webcam` which exec's it):

**Step 1: Lens distortion calibration (checkerboard)**
```
1. Display a checkerboard pattern on the target screen
   (nash sends the image via HID bridge or the user opens a URL)
2. Capture 10-20 webcam frames from slightly different angles
   (or the same angle — single-shot is sufficient for fixed setups)
3. Run `nash-calibrate` (separate C binary linking libopencv)
   → calls cv::findChessboardCorners() + cv::calibrateCamera()
   → produces camera_matrix + dist_coeffs
4. Reprojection error should be < 1.0 pixel
```

**Step 2: Perspective correction (4-point transform)**
```
1. Display 4 reference markers at known screen positions
   (e.g., bright dots at corners: [0,0], [W,0], [W,H], [0,H])
2. Capture webcam frame, detect marker positions in frame
3. Compute homography matrix from 4 point correspondences:
   webcam_corners → screen_corners
4. This transforms the distorted, angled webcam view into a
   flat, correctly-proportioned image matching the screen
```

**Step 3: Validation**
```
1. Display a grid of known points on target screen
2. Capture + undistort + perspective-correct
3. Compare detected positions with known positions
4. Report accuracy (should be < 2px error for reliable clicking)
```

### 5.3 Integration with Display Backend

```c
/* In display_webcam.c */
char *display_capture_webcam(display_t *d) {
    /* 1. Grab raw frame from V4L2 */
    unsigned char *raw = v4l2_grab_frame(d);

    /* 2. If calibration data available: undistort + perspective correct */
    if (d->calibration) {
        unsigned char *corrected = calibration_apply(
            d->calibration, raw, d->raw_w, d->raw_h, &corrected_len);
        /* corrected is now a flat, properly-oriented image of just the screen */
        /* Save corrected image */
        save_jpeg(corrected, d->calibration->screen_width,
                  d->calibration->screen_height, path);
        free(corrected);
    } else {
        /* No calibration — save raw frame as-is (coordinates will be wrong) */
        save_jpeg(raw, d->raw_w, d->raw_h, path);
    }

    free(raw);
    return strdup(path);
}
```

### 5.4 Calibration Storage

Calibration data is stored as a JSON file referenced from config:

```json
{
    "camera_matrix": [1420.5, 0, 960.0, 0, 1420.5, 540.0, 0, 0, 1],
    "dist_coeffs": [-0.0234, 0.0156, -0.0001, 0.0003, -0.0089],
    "homography": [0.98, -0.02, 10.5, 0.01, 0.99, -5.3, 0.00001, 0.00002, 1.0],
    "screen_roi": [120, 80, 1680, 960],
    "screen_width": 1920,
    "screen_height": 1080,
    "reproj_error": 0.72,
    "calibrated_at": 1719130000
}
```

### 5.5 When to Recalibrate

Recalibration is needed when:
- Camera is moved or repositioned
- Target screen resolution changes
- Zoom level changes
- Image shows obvious misalignment (clicks miss targets)

The tool can detect drift by periodically checking reprojection error
against known reference points and warning when accuracy degrades.

### 5.6 Implementation: Separate C Binary (`nash-calibrate`)

OpenCV is only needed for *generating* calibration data — a one-time
operation. To avoid linking it into the main `nash` binary, calibration
runs as a **separate C binary** (`nash-calibrate`) that links libopencv:

```bash
# Separate binary, built only when libopencv is available:
nash-calibrate --webcam /dev/video0 --output calibration.json

# What it does:
#   1. Captures checkerboard frames via V4L2 (reuses display_webcam.c)
#   2. Calls cv::findChessboardCorners() + cv::calibrateCamera() (OpenCV C++ API)
#   3. Computes perspective homography from 4-point markers
#   4. Generates remap tables (binary float32 arrays)
#   5. Writes calibration.json + remap_x.bin + remap_y.bin
#   6. Exits
```

**Build integration:**
```makefile
# In Makefile — only built if libopencv is found:
ifdef HAVE_OPENCV
nash-calibrate: src/nash_calibrate.c src/display_webcam.c src/calibration.c
	$(CC) $(CFLAGS) -o $@ $^ $(shell pkg-config --libs opencv4) -lv4l2
endif
```

This follows the same optional-dependency pattern used elsewhere in nash:
the calibration feature compiles only when the library is available,
and the main binary never links it.

The calibration binary generates **remap tables** (two 2D arrays of
float32 mapping each output pixel to its source position in the raw frame).
At runtime, `calibration_apply()` uses these tables with bilinear
interpolation — pure C, no OpenCV dependency:

```c
/* Remap table approach — O(w*h) per frame, no OpenCV at runtime */
unsigned char *calibration_apply(const calibration_data_t *cal,
                                  const unsigned char *frame,
                                  int frame_w, int frame_h,
                                  size_t *out_len) {
    int out_w = cal->screen_width;
    int out_h = cal->screen_height;
    unsigned char *out = malloc(out_w * out_h * 3);

    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            /* Look up source position from remap table */
            float sx = cal->map_x[y * out_w + x];
            float sy = cal->map_y[y * out_w + x];
            /* Bilinear interpolation from source frame */
            out[(y * out_w + x) * 3 + 0] = bilinear(frame, frame_w, frame_h, sx, sy, 0);
            out[(y * out_w + x) * 3 + 1] = bilinear(frame, frame_w, frame_h, sx, sy, 1);
            out[(y * out_w + x) * 3 + 2] = bilinear(frame, frame_w, frame_h, sx, sy, 2);
        }
    }

    *out_len = out_w * out_h * 3;
    return out;
}
```

## 6. Backend Implementations

### 6.1 VNC Display Backend (`src/display_vnc.c`)

Implements the **RFB 3.8 protocol directly** over a raw TCP socket —
no external VNC library.  Same approach as `nashell/vnc_control.py`
(`DirectVNCBackend`).  Dependencies are only libjpeg (JPEG encoding),
zlib (Tight encoding decompression), and OpenSSL/libcrypto (VNC DES
authentication — already linked by nash).

```c
#include <sys/socket.h>
#include <zlib.h>
#include <jpeglib.h>
#include <openssl/des.h>  /* VNC DES auth (bit-reversed key) */

struct display_t {
    display_config_t cfg;
    display_type_t   type;

    int              sock_fd;        /* raw TCP socket to VNC server */
    int              native_w, native_h;
    uint8_t         *framebuffer;    /* BGRA, native resolution */
    z_stream         zstreams[4];    /* persistent Tight decompressors */
    int              zstream_inited[4];

    char *(*capture_fn)(display_t *d);
    void  (*close_fn)(display_t *d);
};

/* RFB 3.8 handshake (rfb_connect):
 * 1. TCP connect via getaddrinfo()
 * 2. Read 12-byte server version, send "RFB 003.008\n"
 * 3. Security negotiation: None (type 1) or VNC Auth (type 2)
 *    VNC Auth: read 16-byte challenge, DES encrypt with
 *    bit-reversed password, send 16-byte response
 * 4. Read 4-byte SecurityResult (must be 0)
 * 5. Send ClientInit (shared=1)
 * 6. Read 24-byte ServerInit → width, height
 * 7. SetPixelFormat: 32-bit BGRA
 * 8. SetEncodings: Tight(7), Raw(0), CopyRect(1),
 *    DesktopSize(-223), JPEG quality 9(-23)
 */

char *display_capture_vnc(display_t *d) {
    /* Send FramebufferUpdateRequest (10 bytes) */
    uint8_t req[10] = {3, 0, ...};  /* full framebuffer */
    send_exact(d->sock_fd, req, 10);

    /* Read FramebufferUpdate response, parse rectangles:
     * - Tight (enc 7): JPEG sub-encoding (zero-copy),
     *   Fill, Basic with Palette/Copy/Gradient + zlib
     * - Raw (enc 0): w*h*4 bytes direct to framebuffer
     * - CopyRect (enc 1): copy within framebuffer
     * - DesktopSize (-223): resize event */

    /* Encode BGRA framebuffer as JPEG via libjpeg */
    write_jpeg(path, d->framebuffer, d->native_w, d->native_h, 85);
    return path;
}
```

### 6.2 VNC Input Backend (`src/input_vnc.c`)

Sends RFB KeyEvent and PointerEvent messages over the same raw TCP
socket (shared from display backend via `input_vnc_set_sock_fd()`):

```c
struct input_t {
    input_config_t cfg;
    input_type_t   type;

    /* VNC-specific — shares the socket fd from display */
    void          *vnc_client;  /* cast: (intptr_t)sock_fd */
};

/* RFB PointerEvent: msg(1) + buttonMask(1) + x(2) + y(2) = 6 bytes */
int input_click_vnc(input_t *in, int x, int y, const char *button) {
    int nx = x * in->cfg.native_width / in->cfg.model_width;
    int ny = y * in->cfg.native_height / in->cfg.model_height;

    uint8_t mask = (strcmp(button, "right") == 0) ? 0x04
                 : (strcmp(button, "middle") == 0) ? 0x02 : 0x01;

    uint8_t msg[6] = {5, mask, nx>>8, nx&0xFF, ny>>8, ny&0xFF};
    send_exact(fd, msg, 6);        /* press */
    usleep(50000);
    msg[1] = 0;
    send_exact(fd, msg, 6);        /* release */
    return 0;
}

/* RFB KeyEvent: msg(1) + downFlag(1) + pad(2) + keysym(4) = 8 bytes */
int input_key_vnc(input_t *in, const char *keyname) {
    uint32_t keysym = keyname_to_keysym(keyname);
    uint8_t msg[8] = {4, 1, 0, 0, keysym>>24, ...};
    send_exact(fd, msg, 8);   /* press */
    usleep(10000);
    msg[1] = 0;
    send_exact(fd, msg, 8);   /* release */
    return 0;
}
```

### 6.3 HID Bridge Input Backend (`src/input_hid.c`)

Wraps the RP2040 serial protocol in C (currently Python in
`rp2040-hid-bridge/laptop/hid_bridge.py`):

```c
#include <termios.h>
#include <fcntl.h>

struct input_t {
    input_config_t cfg;
    input_type_t   type;

    /* HID bridge specific */
    int            serial_fd;
};

input_t *input_open_hid(const input_config_t *cfg) {
    input_t *in = calloc(1, sizeof(*in));
    in->type = INPUT_HID_BRIDGE;
    in->cfg = *cfg;

    in->serial_fd = open(cfg->serial_port, O_RDWR | O_NOCTTY);
    if (in->serial_fd < 0) { free(in); return NULL; }

    struct termios tty;
    tcgetattr(in->serial_fd, &tty);
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_lflag = 0;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 20;  /* 2 second timeout */
    tcsetattr(in->serial_fd, TCSANOW, &tty);

    /* Flush garbage, send STATUS to verify */
    hid_send(in, "STATUS");
    return in;
}

/* Send command, read "OK"/"ERR" response */
static int hid_send(input_t *in, const char *cmd) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s\n", cmd);
    write(in->serial_fd, buf, len);
    tcdrain(in->serial_fd);

    /* Read response line */
    char resp[128];
    int pos = 0;
    while (pos < 127) {
        int n = read(in->serial_fd, &resp[pos], 1);
        if (n <= 0) break;
        if (resp[pos] == '\n') break;
        pos++;
    }
    resp[pos] = '\0';
    return (strncmp(resp, "OK", 2) == 0) ? 0 : -1;
}

int input_click_hid(input_t *in, int x, int y, const char *button) {
    /* Map model coords → target native coords */
    int nx = x * in->cfg.native_width / in->cfg.model_width;
    int ny = y * in->cfg.native_height / in->cfg.model_height;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "CLICKAT %d %d %s", nx, ny, button);
    return hid_send(in, cmd);
}

int input_type_hid(input_t *in, const char *text) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "TYPE %s", text);
    return hid_send(in, cmd);
}

int input_key_hid(input_t *in, const char *keyname) {
    /* Handle "ctrl+c" → "COMBO ctrl c" */
    if (strchr(keyname, '+')) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", keyname);
        for (char *p = buf; *p; p++) if (*p == '+') *p = ' ';
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "COMBO %s", buf);
        return hid_send(in, cmd);
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "KEY %s", keyname);
    return hid_send(in, cmd);
}

int input_drag_hid(input_t *in, int x1, int y1, int x2, int y2,
                    const char *button) {
    int nx1 = x1 * in->cfg.native_width / in->cfg.model_width;
    int ny1 = y1 * in->cfg.native_height / in->cfg.model_height;
    int nx2 = x2 * in->cfg.native_width / in->cfg.model_width;
    int ny2 = y2 * in->cfg.native_height / in->cfg.model_height;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "DRAG %d %d %d %d %s",
             nx1, ny1, nx2, ny2, button);
    return hid_send(in, cmd);
}

int input_scroll_hid(input_t *in, int x, int y,
                      const char *direction, int amount) {
    /* First move to position, then scroll */
    int nx = x * in->cfg.native_width / in->cfg.model_width;
    int ny = y * in->cfg.native_height / in->cfg.model_height;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "MOVETO %d %d", nx, ny);
    hid_send(in, cmd);
    int scroll_val = (strcmp(direction, "down") == 0) ? -amount : amount;
    snprintf(cmd, sizeof(cmd), "SCROLL %d", scroll_val);
    return hid_send(in, cmd);
}
```

### 6.4 Webcam Display Backend (`src/display_webcam.c`)

For the HID bridge path, display capture uses a webcam or HDMI capture card
via V4L2.  **Applies calibration correction when available.**

```c
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "calibration.h"

struct display_t {
    display_config_t cfg;
    display_type_t   type;

    /* Webcam-specific */
    int              cam_fd;
    void            *cam_buffer;
    size_t           cam_buflen;
    int              raw_w, raw_h;    /* raw webcam resolution */

    /* Calibration (NULL = uncalibrated, coordinates may be inaccurate) */
    calibration_data_t *calibration;
};

display_t *display_open_webcam(const display_config_t *cfg) {
    display_t *d = calloc(1, sizeof(*d));
    d->type = DISPLAY_WEBCAM;
    d->cfg = *cfg;

    d->cam_fd = open(cfg->webcam_device, O_RDWR);
    if (d->cam_fd < 0) { free(d); return NULL; }

    /* Set capture format */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg->width > 0 ? cfg->width : 1920;
    fmt.fmt.pix.height = cfg->height > 0 ? cfg->height : 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    ioctl(d->cam_fd, VIDIOC_S_FMT, &fmt);

    d->raw_w = fmt.fmt.pix.width;
    d->raw_h = fmt.fmt.pix.height;

    /* mmap buffer setup... */

    /* Load calibration if specified */
    if (cfg->calibration_file) {
        d->calibration = calloc(1, sizeof(calibration_data_t));
        if (calibration_load(d->calibration, cfg->calibration_file) != 0) {
            fprintf(stderr, "WARNING: Failed to load webcam calibration from %s\n",
                    cfg->calibration_file);
            fprintf(stderr, "  Coordinates will be inaccurate. Run: nash --calibrate-webcam\n");
            free(d->calibration);
            d->calibration = NULL;
        }
    }

    return d;
}

char *display_capture_webcam(display_t *d) {
    /* 1. Grab raw frame via V4L2 DQBUF */
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    ioctl(d->cam_fd, VIDIOC_DQBUF, &buf);

    unsigned char *frame = d->cam_buffer;
    size_t frame_len = buf.bytesused;

    /* Re-queue buffer */
    ioctl(d->cam_fd, VIDIOC_QBUF, &buf);

    char *path = make_screenshot_path();

    /* 2. Apply calibration if available */
    if (d->calibration) {
        /* Undistort + perspective-correct + crop to screen area */
        size_t corrected_len;
        unsigned char *corrected = calibration_apply(
            d->calibration, frame, d->raw_w, d->raw_h, &corrected_len);
        if (corrected) {
            /* Save corrected image — dimensions match target screen */
            save_jpeg(corrected,
                      d->calibration->screen_width,
                      d->calibration->screen_height,
                      path, 85);
            free(corrected);
            return path;
        }
        /* Calibration failed — fall through to raw */
    }

    /* 3. No calibration — save raw frame */
    save_raw_frame(frame, frame_len, path);
    return path;
}
```

## 7. Configuration (`config.toml`)

```toml
[device_control]
# Display backend: "vnc", "webcam", "command"
display = "vnc"

# Input backend: "vnc", "hid", "command"
input = "vnc"

# VNC settings
vnc_host = "192.168.1.50"
vnc_port = 5900
vnc_password = ""          # or set NASH_VNC_PASSWORD env var

# HID bridge settings (for input = "hid")
serial_port = "/dev/ttyUSB0"
serial_baud = 115200

# Webcam settings (for display = "webcam")
webcam_device = "/dev/video0"

# Webcam calibration file (required for accurate coordinate mapping)
# Generated by: nash --calibrate-webcam
calibration_file = ""      # e.g. "~/.nash/webcam_calibration.json"

# Target screen resolution (for coordinate mapping with HID bridge)
screen_width = 1920
screen_height = 1080

# Perception pipeline
perception = "fast"        # "none", "widgets", "fast" (default), "full", "vision"
omniparser_model = "~/models/omniparser/icon_detect/model.pt"
screenshot_dir = "/tmp/device_screenshots"

# Safety limits
max_actions = 50           # hard limit per react loop
action_delay_ms = 500      # pause after each action
screenshot_delay_ms = 300  # wait before screenshot (let UI settle)

# External commands (for display/input = "command")
# capture_cmd = "scrot -o /dev/stdout"
# key_cmd = "xdotool key %s"
# type_cmd = "xdotool type '%s'"
# click_cmd = "xdotool mousemove %d %d click %d"
```

```c
/* In config.h — add to config_t struct */
typedef struct {
    /* Display */
    display_type_t display_type;
    char *vnc_host;
    int   vnc_port;
    char *vnc_password;
    char *webcam_device;
    char *calibration_file;
    char *capture_cmd;
    int   screen_width;       /* target native resolution */
    int   screen_height;

    /* Perception */
    perception_tier_t perception_tier;
    char *omniparser_model;
    char *screenshot_dir;

    /* Input */
    input_type_t input_type;
    char *serial_port;
    int   serial_baud;
    char *key_cmd;
    char *type_cmd;
    char *click_cmd;

    /* Safety */
    int   max_actions;
    int   action_delay_ms;
    int   screenshot_delay_ms;
} device_control_config_t;

/* Add to config_t: */
/* [device_control] — GUI control */
device_control_config_t device_control;
```

## 8. Integration into Nash React Loop

### 8.1 Context-Efficient Operation

Since the perception pipeline returns only structured text metadata (~200
tokens), there is **no context pressure from screenshots**.  Unlike a
vision-based approach where 5-10 base64 images would fill the context
window, the model can call `device_control(action="screenshot")` dozens of
times without concern.  Each call returns a concise widget+text summary.

The actual screenshot images are saved to disk for debugging and
human review but never enter the model's context.

### 8.2 Agentic Loop Integration

The device control workflow fits naturally into nash's react loop:

```
User: "Open Firefox and navigate to google.com"
  ↓
React step 1: model calls device_control(action="screenshot")
  → tool returns text: "15 widgets, 42 texts — desktop with taskbar..."
  ↓
React step 2: model reads widget list, calls device_control(action="click", x=50, y=740)
  → tool clicks, returns "Clicked at (50,740)"
  ↓
React step 3: model calls device_control(action="screenshot")
  → tool returns updated widget/text list showing Firefox opened
  ↓
React step 4: model calls device_control(action="click", x=400, y=50)
  → clicks URL bar
  ↓
React step 5: model calls device_control(action="type", text="google.com")
  ↓
React step 6: model calls device_control(action="key", key_name="enter")
  ↓
React step 7: model calls device_control(action="screenshot")
  → confirms Google loaded
  ↓
React step 8: model calls done("Firefox opened, navigated to google.com")
```

Or with screenshot_diff for efficiency:

```
React step 2: model calls device_control(action="click", x=50, y=740)
React step 3: model calls device_control(action="screenshot_diff")
  → only shows what CHANGED: "ADDED: Firefox window title, URL bar, ..."
```

No special "device-control loop" needed — the existing react loop IS the
agent loop.  The model decides when to take screenshots and actions just
like it decides when to call `file_read` or `shell_exec`.

## 9. File Structure

```
src/
├── display.h              # Display backend abstraction
├── display.c              # Backend dispatcher (open/capture/close)
├── display_vnc.c          # VNC/RFB display backend
├── display_webcam.c       # V4L2 webcam display backend (with calibration)
├── display_cmd.c          # External command display backend
├── input.h                # Input backend abstraction
├── input.c                # Backend dispatcher
├── input_vnc.c            # VNC/RFB input backend
├── input_hid.c            # RP2040 HID bridge serial input backend
├── input_cmd.c            # External command input backend
├── perception.h           # Perception pipeline abstraction
├── perception.c           # OmniParser + OCR integration
├── calibration.h          # Webcam calibration (undistort + perspective)
├── calibration.c          # Calibration data load/save/apply
├── nash_calibrate.c       # Standalone calibration binary (links libopencv)
├── device.h               # Device session (display + input + perception)
├── device.c               # Session lifecycle
├── tool_device.c          # The device_control tool handler
└── (existing files...)
```

## 10. Implementation Plan

### Phase 1: Core Abstractions + VNC (Week 1)
1. **perception.h / perception.c** — Perception pipeline (OmniParser + OCR)
2. **display.h / input.h** — Define the abstract interfaces
3. **display_vnc.c** — Implement VNC screenshot capture via direct RFB 3.8 protocol
4. **input_vnc.c** — Implement VNC keyboard/pointer injection
5. **device.h / device.c** — Bind display + input + perception, session lifecycle
6. **tool_device.c** — The tool handler, dispatches actions
7. **Config** — Add `[device_control]` section to config.toml parsing
8. **Registry** — Add to TOOL_REGISTRY, TOOL_HANDLERS, increment count

### Phase 2: HID Bridge + Webcam (Week 2)
1. **input_hid.c** — Serial UART communication in C (port from Python)
2. **display_webcam.c** — V4L2 webcam capture
3. **calibration.h / calibration.c** — Lens distortion + perspective correction
4. **nash-calibrate** — Standalone C calibration binary (links libopencv)
5. **Coordinate mapping** — Model coords → target native coords
6. **End-to-end test** — Control a physical device

### Phase 3: Polish (Week 3)
1. **display_cmd.c / input_cmd.c** — External command backends
2. **Perception optimization** — Caching, diff mode, region mode
3. **Safety** — Action limits, delay tuning, error recovery
4. **Vision tier** — Optional base64 image for vision models
5. **Documentation** — README, config examples, troubleshooting

### Phase 4: Advanced (Future)
1. **Pico W WiFi** — No serial adapter needed (WiFi → RP2040)
2. **HDMI capture** — USB HDMI capture card as display backend
3. **Phone/tablet profiles** — Touch gestures, different coordinate spaces
4. **Recording/playback** — Record action sequences for replay
5. **Multi-device** — Control multiple devices simultaneously

## 11. Dependencies

| Dependency | Package | Purpose | Required? |
|-----------|---------|---------|-----------|
| V4L2 | (kernel headers) | Webcam capture | For webcam path |
| termios | (libc) | Serial port | For HID bridge path |
| OmniParser | YOLO model file | Widget detection | For perception |
| Tesseract | `tesseract-ocr` | Fast OCR | For perception (fast tier) |
| EasyOCR | `pip install easyocr` | Thorough OCR | For perception (full tier) |
| OpenCV | `python3-opencv` | Webcam calibration | Calibration only |
| libjpeg | `libjpeg-dev` | JPEG save to disk | Yes |

The VNC backend uses direct RFB 3.8 protocol (no external VNC library)
and is always compiled in.  Other backends are optional and compile-gated
with `#ifdef HAVE_V4L2`, etc. in the Makefile.

Perception dependencies (OmniParser, Tesseract, EasyOCR) are invoked as
external processes — they don't need to be linked into the nash binary.

## 12. Safety Considerations

1. **Action limits**: Hard cap per react loop (`max_actions = 50`)
2. **Delays**: Configurable pause between actions to prevent runaway clicks
3. **No terminal access by default**: Unlike Claude Code, nash runs on the
   CONTROLLING machine, not the target.  The target has no way to inject
   prompts back (unless the model reads on-screen text — mitigated by
   the model's own judgment and the perception pipeline acting as a filter)
4. **VNC authentication**: Password-based auth is bare minimum.  For
   production, use SSH tunnel + VNC over localhost.
5. **HID bridge is physically one-way**: Serial → RP2040 → USB HID.  The
   target cannot send data back through the HID bridge.  Display is
   via separate webcam.  True air-gap safety.
6. **Abort**: TUI `Esc` or `Ctrl+C` in nash aborts the react loop,
   which stops any further `device_control` calls.  In-flight serial
   commands complete (single keypress/click) but no new ones are sent.
7. **Perception as safety filter**: Since the model sees structured text
   metadata instead of raw pixels, it's harder for on-screen prompt
   injection to influence the model.  Attackers would need to craft text
   that both OCR reads AND convinces the model — a higher bar than visual
   injection against a vision model.

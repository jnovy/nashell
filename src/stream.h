/* stream.h -- Continuous frame capture + HEVC encoding + historical access.
 *
 * A background thread captures frames from the display backend at a
 * configurable FPS, encodes them to HEVC (H.265) via libx265, and
 * maintains a ring buffer index for O(1) lookup by timestamp.
 *
 * The `screenshot` command extracts frames from the stream instantly
 * (no VNC roundtrip), and the `seconds_ago` parameter enables
 * historical frame access within the retention window.
 *
 * Requires: libx265 (encoding), libde265 (decoding historical frames),
 *           libjpeg (JPEG output for perception pipeline).
 * Compile with -DHAVE_X265 to enable; falls back to on-demand capture
 * when disabled.
 *
 * See docs/design-device-control.md (Continuous HEVC Stream redesign) */
#ifndef STREAM_H
#define STREAM_H

#include "display.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* Per-frame metadata in the ring buffer index */
typedef struct {
    uint64_t        frame_number;       /* monotonic counter */
    struct timespec timestamp;          /* wall-clock capture time */
    int64_t         byte_offset;        /* offset into the .hevc file */
    int32_t         nal_size;           /* size of this frame's NAL unit(s) */
    int             is_keyframe;        /* IDR frame? (can decode independently) */
} frame_entry_t;

typedef struct {
    int         fps;                    /* target capture rate (default 10) */
    int         keyframe_interval;      /* IDR every N frames (0 = auto = fps) */
    int         quality;                /* CRF value for x265 (default 28, 0-51) */
    int         retention_secs;         /* seconds of history to keep (default 300) */
    const char *stream_dir;             /* output dir (default: screenshot_dir) */
    const char *preset;                 /* x265 preset (default: "ultrafast") */
} stream_config_t;

typedef struct stream_t stream_t;

/* Create and start the continuous capture stream.
 * Spawns a background thread that:
 *   1. Requests framebuffer updates from display at `fps` rate
 *   2. Encodes each frame to HEVC via libx265
 *   3. Appends NAL units to a rolling .hevc file
 *   4. Maintains a ring buffer of frame_entry_t for O(1) lookup by time
 * Returns NULL on error (e.g., libx265 not available). */
stream_t *stream_start(display_t *display, const stream_config_t *cfg);

/* Extract a frame from the stream.
 *   timestamp=NULL  -> latest frame (the "screenshot now" case)
 *   timestamp!=NULL -> closest frame to that wall-clock time
 *
 * Returns path to a JPEG file on disk.
 * Caller must free() the returned path. */
char *stream_extract_frame(stream_t *s, const struct timespec *timestamp,
                           int *out_w, int *out_h);

/* Get the current framebuffer (mutex-protected copy).
 * Fastest path -- no HEVC decode, just memcpy.
 * Returns malloc'd BGRA buffer. Caller must free(). */
uint8_t *stream_get_current_frame(stream_t *s, int *out_w, int *out_h);

/* Query available history range */
void stream_get_range(stream_t *s, struct timespec *oldest,
                      struct timespec *newest, uint64_t *total_frames);

/* Pause/resume capture (e.g., during long non-GUI operations) */
void stream_pause(stream_t *s);
void stream_resume(stream_t *s);

/* Stop the stream, save the .hevc file to dest_dir with name:
 *   stream-<unix_timestamp>-<duration_secs>.hevc
 * If dest_dir is NULL, the file is removed (discard mode).
 * Returns malloc'd path to the saved file, or NULL on error/discard.
 * Caller must free() the returned path.
 * Frees the stream_t — pointer is invalid after this call. */
char *stream_save(stream_t *s, const char *dest_dir);

/* Stop and free the stream, discarding the capture file.
 * Equivalent to stream_save(s, NULL). */
void stream_stop(stream_t *s);

#endif /* STREAM_H */

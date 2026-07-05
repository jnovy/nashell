/* stream.c -- Continuous frame capture + HEVC encoding + historical access.
 *
 * Background thread captures display frames at configurable FPS, encodes
 * to HEVC via libx265, stores in a rolling .hevc file with a ring buffer
 * index for O(1) historical frame lookup by timestamp.
 *
 * When HAVE_X265 is not defined, all functions are stubs that return
 * errors, preserving backward compatibility with on-demand capture.
 *
 * See docs/design-device-control.md (Continuous HEVC Stream redesign) */

#include "stream.h"
#include "display.h"
#include "nash_log.h"
#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifdef HAVE_X265
#include <x265.h>
#include <libde265/de265.h>

/* ── Internal structure ──────────────────────────────────────── */

struct stream_t {
    display_t       *display;           /* borrowed, not owned */
    stream_config_t  cfg;

    /* Thread control */
    pthread_t        thread;
    pthread_mutex_t  mutex;             /* protects framebuffer + index */
    volatile int     running;
    volatile int     paused;

    /* Current framebuffer (always up-to-date, mutex-protected) */
    uint8_t         *framebuffer;       /* BGRA, native_w * native_h * 4 */
    int              native_w, native_h;
    int              fb_valid;          /* set after first successful capture */

    /* HEVC encoder (libx265) */
    x265_encoder    *encoder;
    x265_param      *x265_params;

    /* I420 conversion buffer */
    uint8_t         *yuv_buf;           /* Y + U + V planes */
    size_t           yuv_size;

    /* Frame index (ring buffer) */
    frame_entry_t   *index;             /* circular buffer */
    int              index_cap;         /* capacity = retention_secs * fps */
    int              index_head;        /* next write position */
    int              index_count;       /* current count (<= cap) */
    uint64_t         frame_counter;     /* monotonic */
    uint32_t         prev_fb_crc;       /* CRC32 of previous framebuffer */
    int              have_prev_crc;     /* true after first frame captured */
    uint64_t         frames_skipped;    /* duplicate frames not encoded */

    /* Output file */
    FILE            *hevc_file;         /* append-only .hevc stream */
    int64_t          file_offset;       /* current write position */
    char            *hevc_path;         /* path to the .hevc file */
    char            *stream_dir;        /* output directory (owned copy) */
};

/* ── Color space conversion ──────────────────────────────────── */

/* BGRA -> I420 (YCbCr 4:2:0).  Standard BT.601 coefficients. */
static void bgra_to_i420(const uint8_t *bgra, int w, int h,
                          uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane) {
    int uv_w = (w + 1) / 2;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            const uint8_t *px = bgra + ((size_t)row * w + col) * 4;
            int b = px[0], g = px[1], r = px[2];
            /* BT.601 */
            int yv =  ((66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
            y_plane[row * w + col] = (uint8_t)(yv < 0 ? 0 : yv > 255 ? 255 : yv);

            if ((row & 1) == 0 && (col & 1) == 0) {
                int uv_row = row / 2;
                int uv_col = col / 2;
                int uv = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
                int vv = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
                u_plane[uv_row * uv_w + uv_col] = (uint8_t)(uv < 0 ? 0 : uv > 255 ? 255 : uv);
                v_plane[uv_row * uv_w + uv_col] = (uint8_t)(vv < 0 ? 0 : vv > 255 ? 255 : vv);
            }
        }
    }
}

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
    snprintf(path, 512, "%s/stream_%ld_%03ld.jpg",
             dir, (long)ts.tv_sec, ts.tv_nsec / 1000000);
    return path;
}

/* ── Ring buffer helpers ─────────────────────────────────────── */

/* Binary search for the frame closest to the given timestamp.
 * Ring buffer entries are in chronological order.
 * Returns index into s->index, or -1 if empty.
 * Caller must hold s->mutex. */
static int find_closest_frame(stream_t *s, const struct timespec *ts) {
    if (s->index_count == 0) return -1;

    /* Convert target to a simple comparable value (seconds + nanoseconds) */
    double target = (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;

    int best = -1;
    double best_diff = 1e18;

    /* Linear scan -- ring buffer is at most retention_secs * fps entries
     * (e.g., 300 * 10 = 3000), so linear scan is fine. */
    int start;
    if (s->index_count < s->index_cap)
        start = 0;
    else
        start = s->index_head;  /* oldest entry */

    for (int i = 0; i < s->index_count; i++) {
        int idx = (start + i) % s->index_cap;
        double t = (double)s->index[idx].timestamp.tv_sec +
                   (double)s->index[idx].timestamp.tv_nsec / 1e9;
        double diff = target > t ? target - t : t - target;
        if (diff < best_diff) {
            best_diff = diff;
            best = idx;
        }
    }
    return best;
}

/* Find the nearest preceding keyframe for a given ring buffer index.
 * Caller must hold s->mutex. */
static int find_preceding_keyframe(stream_t *s, int target_idx) {
    frame_entry_t *target = &s->index[target_idx];

    /* Walk backward from target_idx */
    int start;
    if (s->index_count < s->index_cap)
        start = 0;
    else
        start = s->index_head;

    /* Find position of target_idx in ring order */
    int target_pos = -1;
    for (int i = 0; i < s->index_count; i++) {
        if ((start + i) % s->index_cap == target_idx) {
            target_pos = i;
            break;
        }
    }
    if (target_pos < 0) return target_idx;

    /* Walk backward to find nearest keyframe */
    for (int i = target_pos; i >= 0; i--) {
        int idx = (start + i) % s->index_cap;
        if (s->index[idx].is_keyframe)
            return idx;
    }

    /* No keyframe found -- return the target itself as fallback */
    (void)target;
    return target_idx;
}

/* ── Background capture thread ───────────────────────────────── */

static void *capture_thread(void *arg) {
    stream_t *s = arg;
    long interval_ns = 1000000000L / s->cfg.fps;

    nash_log("[stream] capture thread started: %d fps, CRF %d, preset %s",
             s->cfg.fps, s->cfg.quality, s->cfg.preset);

    while (s->running) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (!s->paused) {
            /* 1. Update framebuffer from display backend */
            int rc = display_update_framebuffer(s->display);

            if (rc == 0) {
                int w = 0, h = 0;
                display_get_dimensions(s->display, &w, &h);

                pthread_mutex_lock(&s->mutex);

                /* Handle resolution changes */
                if (w != s->native_w || h != s->native_h) {
                    s->native_w = w;
                    s->native_h = h;
                    free(s->framebuffer);
                    s->framebuffer = malloc((size_t)w * h * 4);

                    /* Reallocate YUV buffer */
                    size_t y_size = (size_t)w * h;
                    size_t uv_size = ((size_t)(w + 1) / 2) * ((h + 1) / 2);
                    s->yuv_size = y_size + 2 * uv_size;
                    free(s->yuv_buf);
                    s->yuv_buf = malloc(s->yuv_size);

                    if (!s->framebuffer || !s->yuv_buf) {
                        nash_log("[stream] allocation failed on resize to %dx%d", w, h);
                        pthread_mutex_unlock(&s->mutex);
                        continue;
                    }

                    /* Re-create encoder with new dimensions */
                    if (s->encoder) {
                        x265_encoder_close(s->encoder);
                        s->encoder = NULL;
                    }
                    x265_param_default_preset(s->x265_params, s->cfg.preset, "ssim");
                    s->x265_params->sourceWidth = w;
                    s->x265_params->sourceHeight = h;
                    s->x265_params->fpsNum = s->cfg.fps;
                    s->x265_params->fpsDenom = 1;
                    s->x265_params->rc.rateControlMode = X265_RC_CRF;
                    s->x265_params->rc.rfConstant = (double)s->cfg.quality;
                    s->x265_params->bRepeatHeaders = 1;
                    s->x265_params->internalCsp = X265_CSP_I420;
                    s->x265_params->logLevel = X265_LOG_ERROR;
                    /* Keyframe interval */
                    int kf = s->cfg.keyframe_interval > 0 ? s->cfg.keyframe_interval : s->cfg.fps;
                    s->x265_params->keyframeMax = kf;
                    s->x265_params->keyframeMin = kf;

                    s->encoder = x265_encoder_open(s->x265_params);
                    if (!s->encoder) {
                        nash_log("[stream] failed to re-create x265 encoder for %dx%d", w, h);
                        pthread_mutex_unlock(&s->mutex);
                        continue;
                    }
                    s->have_prev_crc = 0;  /* force encode after resize */
                }

                /* 2. Copy framebuffer */
                display_copy_framebuffer(s->display, s->framebuffer);
                s->fb_valid = 1;

                /* 2b. Duplicate frame detection -- skip if unchanged */
                size_t fb_size = (size_t)w * h * 4;
                uint32_t fb_crc = compress_crc32((const char *)s->framebuffer,
                                                  fb_size);
                if (s->have_prev_crc && fb_crc == s->prev_fb_crc) {
                    s->frames_skipped++;
                    s->frame_counter++;
                    pthread_mutex_unlock(&s->mutex);
                    goto next_frame;
                }
                s->prev_fb_crc = fb_crc;
                s->have_prev_crc = 1;

                /* 3. Convert BGRA -> I420 */
                size_t y_size = (size_t)w * h;
                size_t uv_w = (size_t)(w + 1) / 2;
                size_t uv_h = (size_t)(h + 1) / 2;
                uint8_t *y_plane = s->yuv_buf;
                uint8_t *u_plane = y_plane + y_size;
                uint8_t *v_plane = u_plane + uv_w * uv_h;
                bgra_to_i420(s->framebuffer, w, h, y_plane, u_plane, v_plane);

                /* 4. Encode frame to HEVC */
                x265_picture pic_in;
                x265_picture_init(s->x265_params, &pic_in);
                pic_in.planes[0] = y_plane;
                pic_in.planes[1] = u_plane;
                pic_in.planes[2] = v_plane;
                pic_in.stride[0] = w;
                pic_in.stride[1] = (int)uv_w;
                pic_in.stride[2] = (int)uv_w;
                pic_in.pts = (int64_t)s->frame_counter;

                /* Force IDR at keyframe interval */
                int is_keyframe = (s->frame_counter % (uint64_t)s->x265_params->keyframeMax == 0);
                if (is_keyframe)
                    pic_in.sliceType = X265_TYPE_IDR;

                x265_nal *nals = NULL;
                uint32_t nal_count = 0;
                x265_picture pic_out;
                int ret = x265_encoder_encode(s->encoder, &nals, &nal_count,
                                              &pic_in, &pic_out);

                if (ret > 0 && nals && nal_count > 0 && s->hevc_file) {
                    /* 5. Write NAL units to file */
                    int64_t offset = s->file_offset;
                    int32_t total_nal_size = 0;
                    for (uint32_t i = 0; i < nal_count; i++) {
                        fwrite(nals[i].payload, 1, (size_t)nals[i].sizeBytes,
                               s->hevc_file);
                        total_nal_size += nals[i].sizeBytes;
                    }
                    fflush(s->hevc_file);
                    s->file_offset += total_nal_size;

                    /* 6. Update ring buffer index */
                    frame_entry_t *entry = &s->index[s->index_head];
                    entry->frame_number = s->frame_counter;
                    clock_gettime(CLOCK_REALTIME, &entry->timestamp);
                    entry->byte_offset = offset;
                    entry->nal_size = total_nal_size;
                    entry->is_keyframe = is_keyframe;
                    s->index_head = (s->index_head + 1) % s->index_cap;
                    if (s->index_count < s->index_cap) s->index_count++;
                }

                s->frame_counter++;
                pthread_mutex_unlock(&s->mutex);
            }
        }

next_frame:
        /* Sleep for remainder of interval */
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                          (t1.tv_nsec - t0.tv_nsec);
        long sleep_ns = interval_ns - elapsed_ns;
        if (sleep_ns > 1000000L) {  /* at least 1ms */
            struct timespec sl = { .tv_sec = sleep_ns / 1000000000L,
                                   .tv_nsec = sleep_ns % 1000000000L };
            nanosleep(&sl, NULL);
        }
    }

    nash_log("[stream] capture thread exiting (%" PRIu64 " frames captured, "
             "%" PRIu64 " duplicates skipped)",
             s->frame_counter, s->frames_skipped);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

stream_t *stream_start(display_t *display, const stream_config_t *cfg) {
    if (!display || !cfg) {
        nash_log("[stream] stream_start: NULL display=%p cfg=%p", (void*)display, (void*)cfg);
        return NULL;
    }

    stream_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->display = display;
    s->cfg = *cfg;

    /* Apply defaults */
    if (s->cfg.fps <= 0) s->cfg.fps = 4;
    if (s->cfg.fps > 30) s->cfg.fps = 30;
    if (s->cfg.quality <= 0) s->cfg.quality = 28;
    if (s->cfg.quality > 51) s->cfg.quality = 51;
    if (s->cfg.retention_secs <= 0) s->cfg.retention_secs = 300;
    if (!s->cfg.preset) s->cfg.preset = "medium";
    if (!s->cfg.stream_dir) s->cfg.stream_dir = "/tmp/device_screenshots";

    /* Own a copy of stream_dir */
    s->stream_dir = strdup(s->cfg.stream_dir);
    s->cfg.stream_dir = s->stream_dir;

    /* Get initial dimensions */
    int w = 0, h = 0;
    display_get_dimensions(display, &w, &h);
    if (w <= 0 || h <= 0) {
        nash_log("[stream] cannot start: display dimensions %dx%d", w, h);
        free(s->stream_dir);
        free(s);
        return NULL;
    }
    s->native_w = w;
    s->native_h = h;
    nash_log("[stream] init: %dx%d, fps=%d, quality=%d, preset=%s, dir=%s",
             w, h, s->cfg.fps, s->cfg.quality, s->cfg.preset, s->stream_dir);

    /* Allocate framebuffer */
    s->framebuffer = malloc((size_t)w * h * 4);
    if (!s->framebuffer) { nash_log("[stream] alloc framebuffer failed"); goto fail; }

    /* Allocate YUV conversion buffer */
    size_t y_size = (size_t)w * h;
    size_t uv_w = (size_t)(w + 1) / 2;
    size_t uv_h = (size_t)(h + 1) / 2;
    s->yuv_size = y_size + 2 * uv_w * uv_h;
    s->yuv_buf = malloc(s->yuv_size);
    if (!s->yuv_buf) { nash_log("[stream] alloc yuv failed"); goto fail; }

    /* Allocate ring buffer index */
    s->index_cap = s->cfg.retention_secs * s->cfg.fps;
    if (s->index_cap <= 0) s->index_cap = 3000;
    s->index = calloc((size_t)s->index_cap, sizeof(frame_entry_t));
    if (!s->index) { nash_log("[stream] alloc index failed"); goto fail; }

    /* Initialize x265 encoder */
    s->x265_params = x265_param_alloc();
    if (!s->x265_params) { nash_log("[stream] x265_param_alloc failed"); goto fail; }

    x265_param_default_preset(s->x265_params, s->cfg.preset, "ssim");
    s->x265_params->sourceWidth = w;
    s->x265_params->sourceHeight = h;
    s->x265_params->fpsNum = s->cfg.fps;
    s->x265_params->fpsDenom = 1;
    s->x265_params->rc.rateControlMode = X265_RC_CRF;
    s->x265_params->rc.rfConstant = (double)s->cfg.quality;
    s->x265_params->bRepeatHeaders = 1;
    s->x265_params->internalCsp = X265_CSP_I420;
    s->x265_params->logLevel = X265_LOG_ERROR;
    /* Keyframe interval */
    int kf = s->cfg.keyframe_interval > 0 ? s->cfg.keyframe_interval : s->cfg.fps;
    s->x265_params->keyframeMax = kf;
    s->x265_params->keyframeMin = kf;

    nash_log("[stream] opening x265 encoder: %dx%d CRF %.1f preset=%s kf=%d",
             w, h, s->x265_params->rc.rfConstant, s->cfg.preset, kf);
    s->encoder = x265_encoder_open(s->x265_params);
    if (!s->encoder) {
        nash_log("[stream] x265_encoder_open failed for %dx%d", w, h);
        goto fail;
    }
    nash_log("[stream] x265 encoder opened successfully");

    /* Open output .hevc file */
    ensure_dir(s->stream_dir);
    s->hevc_path = malloc(512);
    if (!s->hevc_path) goto fail;
    snprintf(s->hevc_path, 512, "%s/capture.hevc", s->stream_dir);
    s->hevc_file = fopen(s->hevc_path, "wb");
    if (!s->hevc_file) {
        nash_log("[stream] failed to open %s: %s", s->hevc_path, strerror(errno));
        goto fail;
    }

    /* Initialize mutex and start thread */
    pthread_mutex_init(&s->mutex, NULL);
    s->running = 1;
    s->paused = 0;

    if (pthread_create(&s->thread, NULL, capture_thread, s) != 0) {
        nash_log("[stream] failed to create capture thread");
        s->running = 0;
        goto fail;
    }

    nash_log("[stream] started: %dx%d @ %d fps, CRF %d, retention %ds (%d frame index slots)",
             w, h, s->cfg.fps, s->cfg.quality, s->cfg.retention_secs, s->index_cap);
    return s;

fail:
    nash_log("[stream] stream_start failed (goto fail reached)");
    if (s->encoder) x265_encoder_close(s->encoder);
    if (s->x265_params) x265_param_free(s->x265_params);
    if (s->hevc_file) fclose(s->hevc_file);
    free(s->hevc_path);
    free(s->index);
    free(s->yuv_buf);
    free(s->framebuffer);
    free(s->stream_dir);
    free(s);
    return NULL;
}

char *stream_extract_frame(stream_t *s, const struct timespec *timestamp,
                           int *out_w, int *out_h) {
    if (!s) return NULL;

    pthread_mutex_lock(&s->mutex);

    if (!timestamp || !s->fb_valid) {
        /* FAST PATH: "now" -- snapshot the current framebuffer.
         * No HEVC decode needed. */
        if (!s->fb_valid) {
            /* Wait for capture thread to deliver the first frame.
             * Poll with 50ms sleeps, up to 5s total. */
            pthread_mutex_unlock(&s->mutex);
            for (int i = 0; i < 100 && s->running && !s->fb_valid; i++)
                usleep(50000);  /* 50ms */
            pthread_mutex_lock(&s->mutex);
            if (!s->fb_valid) {
                pthread_mutex_unlock(&s->mutex);
                return NULL;
            }
        }
        int w = s->native_w, h = s->native_h;
        uint8_t *copy = malloc((size_t)w * h * 4);
        if (!copy) {
            pthread_mutex_unlock(&s->mutex);
            return NULL;
        }
        memcpy(copy, s->framebuffer, (size_t)w * h * 4);
        pthread_mutex_unlock(&s->mutex);

        /* Write JPEG to disk for perception pipeline */
        char *path = make_screenshot_path(s->stream_dir);
        if (!path) { free(copy); return NULL; }
        if (display_write_jpeg(path, copy, w, h, 85) != 0) {
            free(copy);
            free(path);
            return NULL;
        }
        free(copy);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return path;
    }

    /* HISTORICAL PATH: find closest frame by timestamp */
    int best_idx = find_closest_frame(s, timestamp);
    if (best_idx < 0) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    frame_entry_t target = s->index[best_idx];

    /* Find the nearest preceding keyframe (IDR) */
    int keyframe_idx = find_preceding_keyframe(s, best_idx);
    frame_entry_t kf = s->index[keyframe_idx];

    /* Read HEVC data from keyframe to target frame */
    int64_t read_offset = kf.byte_offset;
    int64_t read_end = target.byte_offset + target.nal_size;
    int64_t read_size = read_end - read_offset;

    if (read_size <= 0 || read_size > 100 * 1024 * 1024) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    uint8_t *hevc_data = malloc((size_t)read_size);
    if (!hevc_data) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    /* Seek and read from the .hevc file */
    if (fseeko(s->hevc_file, read_offset, SEEK_SET) != 0) {
        free(hevc_data);
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }
    size_t nread = fread(hevc_data, 1, (size_t)read_size, s->hevc_file);
    /* Seek back to end for future writes */
    fseeko(s->hevc_file, 0, SEEK_END);

    pthread_mutex_unlock(&s->mutex);

    if ((int64_t)nread != read_size) {
        free(hevc_data);
        return NULL;
    }

    /* Decode with libde265 */
    de265_decoder_context *dec = de265_new_decoder();
    if (!dec) {
        free(hevc_data);
        return NULL;
    }
    de265_set_parameter_bool(dec, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 1);
    de265_start_worker_threads(dec, 1);

    de265_push_data(dec, hevc_data, (int)read_size, 0, NULL);
    de265_flush_data(dec);

    /* Decode frames until we get the last one (the target) */
    const struct de265_image *img = NULL;
    const struct de265_image *last_img = NULL;
    int more = 1;
    while (more) {
        de265_error err = de265_decode(dec, &more);
        if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA)
            break;
        img = de265_get_next_picture(dec);
        if (img) last_img = img;
    }

    char *path = NULL;
    if (last_img) {
        int img_w = de265_get_image_width(last_img, 0);
        int img_h = de265_get_image_height(last_img, 0);
        int y_stride = 0, u_stride = 0, v_stride = 0;
        const uint8_t *y_plane = de265_get_image_plane(last_img, 0, &y_stride);
        const uint8_t *u_plane = de265_get_image_plane(last_img, 1, &u_stride);
        const uint8_t *v_plane = de265_get_image_plane(last_img, 2, &v_stride);

        /* Convert I420 -> BGRA */
        uint8_t *bgra = malloc((size_t)img_w * img_h * 4);
        if (bgra && y_plane && u_plane && v_plane) {
            for (int row = 0; row < img_h; row++) {
                for (int col = 0; col < img_w; col++) {
                    int yv = y_plane[row * y_stride + col];
                    int uv = u_plane[(row / 2) * u_stride + col / 2];
                    int vv = v_plane[(row / 2) * v_stride + col / 2];

                    /* BT.601 YCbCr -> RGB */
                    int c = yv - 16;
                    int d = uv - 128;
                    int e = vv - 128;
                    int r = (298 * c + 409 * e + 128) >> 8;
                    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                    int b = (298 * c + 516 * d + 128) >> 8;
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;

                    uint8_t *px = bgra + ((size_t)row * img_w + col) * 4;
                    px[0] = (uint8_t)b;
                    px[1] = (uint8_t)g;
                    px[2] = (uint8_t)r;
                    px[3] = 255;
                }
            }
            /* Write JPEG */
            path = make_screenshot_path(s->stream_dir);
            if (path) {
                if (display_write_jpeg(path, bgra, img_w, img_h, 85) != 0) {
                    free(path);
                    path = NULL;
                }
            }
            if (out_w) *out_w = img_w;
            if (out_h) *out_h = img_h;
        }
        free(bgra);
    }

    de265_free_decoder(dec);
    free(hevc_data);
    return path;
}

uint8_t *stream_get_current_frame(stream_t *s, int *out_w, int *out_h) {
    if (!s) return NULL;

    pthread_mutex_lock(&s->mutex);
    if (!s->fb_valid) {
        /* Wait for capture thread to deliver the first frame. */
        pthread_mutex_unlock(&s->mutex);
        for (int i = 0; i < 100 && s->running && !s->fb_valid; i++)
            usleep(50000);  /* 50ms, up to 5s total */
        pthread_mutex_lock(&s->mutex);
        if (!s->fb_valid) {
            pthread_mutex_unlock(&s->mutex);
            return NULL;
        }
    }
    int w = s->native_w, h = s->native_h;
    uint8_t *copy = malloc((size_t)w * h * 4);
    if (copy)
        memcpy(copy, s->framebuffer, (size_t)w * h * 4);
    pthread_mutex_unlock(&s->mutex);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return copy;
}

void stream_get_range(stream_t *s, struct timespec *oldest,
                      struct timespec *newest, uint64_t *total_frames) {
    if (!s) return;

    pthread_mutex_lock(&s->mutex);
    if (total_frames) *total_frames = s->frame_counter;

    if (s->index_count > 0) {
        int oldest_idx, newest_idx;
        if (s->index_count < s->index_cap) {
            oldest_idx = 0;
            newest_idx = s->index_count - 1;
        } else {
            oldest_idx = s->index_head;  /* oldest is at head (about to be overwritten) */
            newest_idx = (s->index_head + s->index_cap - 1) % s->index_cap;
        }
        if (oldest) *oldest = s->index[oldest_idx].timestamp;
        if (newest) *newest = s->index[newest_idx].timestamp;
    } else {
        struct timespec zero = {0, 0};
        if (oldest) *oldest = zero;
        if (newest) *newest = zero;
    }
    pthread_mutex_unlock(&s->mutex);
}

void stream_pause(stream_t *s) {
    if (s) s->paused = 1;
}

void stream_resume(stream_t *s) {
    if (s) s->paused = 0;
}

char *stream_save(stream_t *s, const char *dest_dir) {
    if (!s) return NULL;

    /* 1. Stop the capture thread */
    s->running = 0;
    pthread_join(s->thread, NULL);

    /* 2. Flush any remaining frames from encoder */
    if (s->encoder && s->hevc_file) {
        x265_nal *nals = NULL;
        uint32_t nal_count = 0;
        x265_picture pic_out;
        while (x265_encoder_encode(s->encoder, &nals, &nal_count,
                                    NULL, &pic_out) > 0) {
            for (uint32_t i = 0; i < nal_count; i++) {
                fwrite(nals[i].payload, 1, (size_t)nals[i].sizeBytes,
                       s->hevc_file);
            }
        }
    }

    if (s->encoder) x265_encoder_close(s->encoder);
    if (s->x265_params) x265_param_free(s->x265_params);
    if (s->hevc_file) { fclose(s->hevc_file); s->hevc_file = NULL; }

    /* 3. Compute duration and build destination filename */
    char *saved_path = NULL;
    if (dest_dir && s->hevc_path && s->index_count > 0) {
        /* Find oldest and newest timestamps in the ring buffer */
        int oldest_idx, newest_idx;
        if (s->index_count < s->index_cap) {
            oldest_idx = 0;
            newest_idx = s->index_count - 1;
        } else {
            oldest_idx = s->index_head;  /* oldest is at head (about to be overwritten) */
            newest_idx = (s->index_head + s->index_cap - 1) % s->index_cap;
        }
        struct timespec t_oldest = s->index[oldest_idx].timestamp;
        struct timespec t_newest = s->index[newest_idx].timestamp;
        long duration = t_newest.tv_sec - t_oldest.tv_sec;
        if (duration < 0) duration = 0;

        /* Use the stream start time (oldest frame) as the timestamp */
        time_t start_ts = t_oldest.tv_sec;

        /* Build: dest_dir/stream-<unix_timestamp>-<duration>.hevc */
        char dest[1024];
        snprintf(dest, sizeof(dest), "%s/stream-%ld-%ld.hevc",
                 dest_dir, (long)start_ts, duration);

        /* Try rename first (fast, same filesystem). Fall back to copy. */
        if (rename(s->hevc_path, dest) == 0) {
            saved_path = strdup(dest);
            nash_log("[stream] saved capture to %s (%ld frames, %lds)",
                     dest, (long)s->frame_counter, duration);
        } else {
            /* Cross-filesystem: copy */
            FILE *src_f = fopen(s->hevc_path, "rb");
            FILE *dst_f = fopen(dest, "wb");
            if (src_f && dst_f) {
                char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), src_f)) > 0)
                    fwrite(buf, 1, n, dst_f);
                saved_path = strdup(dest);
                nash_log("[stream] copied capture to %s (%ld frames, %lds)",
                         dest, (long)s->frame_counter, duration);
            } else {
                nash_log("[stream] failed to save capture to %s: %s",
                         dest, strerror(errno));
            }
            if (src_f) fclose(src_f);
            if (dst_f) fclose(dst_f);
            unlink(s->hevc_path);  /* remove temp file */
        }
    } else if (s->hevc_path) {
        /* Discard mode: remove the temp file */
        unlink(s->hevc_path);
    }

    /* 4. Free all resources */
    pthread_mutex_destroy(&s->mutex);
    free(s->hevc_path);
    free(s->index);
    free(s->yuv_buf);
    free(s->framebuffer);
    free(s->stream_dir);
    free(s);

    return saved_path;
}

void stream_stop(stream_t *s) {
    free(stream_save(s, NULL));
}

#else /* !HAVE_X265 -- stub implementations */

struct stream_t {
    int dummy;
};

stream_t *stream_start(display_t *display, const stream_config_t *cfg) {
    (void)display; (void)cfg;
    nash_log("[stream] HEVC streaming not available (compiled without HAVE_X265)");
    return NULL;
}

char *stream_extract_frame(stream_t *s, const struct timespec *timestamp,
                           int *out_w, int *out_h) {
    (void)s; (void)timestamp; (void)out_w; (void)out_h;
    return NULL;
}

uint8_t *stream_get_current_frame(stream_t *s, int *out_w, int *out_h) {
    (void)s; (void)out_w; (void)out_h;
    return NULL;
}

void stream_get_range(stream_t *s, struct timespec *oldest,
                      struct timespec *newest, uint64_t *total_frames) {
    (void)s; (void)oldest; (void)newest; (void)total_frames;
}

void stream_pause(stream_t *s)  { (void)s; }
void stream_resume(stream_t *s) { (void)s; }
char *stream_save(stream_t *s, const char *dest_dir)
    { (void)s; (void)dest_dir; return NULL; }
void stream_stop(stream_t *s)   { (void)s; }

#endif /* HAVE_X265 */

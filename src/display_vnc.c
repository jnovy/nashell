/* display_vnc.c — VNC/RFB display backend via direct RFB 3.8 protocol.
 *
 * Connects to a VNC server over a raw TCP socket, captures framebuffer
 * as JPEG screenshots.  No external VNC library — implements the RFB
 * protocol directly (same approach as nashell/vnc_control.py).
 *
 * Dependencies: libjpeg (JPEG encoding), zlib (Tight encoding).
 *
 * See docs/design-device-control.md §6.1 */

#include "display.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <poll.h>
#include <zlib.h>
#include <jpeglib.h>

/* ── RFB protocol constants ────────────────────────────────── */

/* Client → Server message types */
#define RFB_SET_PIXEL_FORMAT     0
#define RFB_SET_ENCODINGS        2
#define RFB_FB_UPDATE_REQUEST    3
#define RFB_KEY_EVENT            4
#define RFB_POINTER_EVENT        5

/* Server → Client message types */
#define RFB_FB_UPDATE            0
#define RFB_SET_COLOUR_MAP       1
#define RFB_BELL                 2
#define RFB_SERVER_CUT_TEXT      3

/* Encoding types */
#define RFB_ENC_RAW              0
#define RFB_ENC_COPYRECT         1
#define RFB_ENC_TIGHT            7
#define RFB_ENC_DESKTOP_SIZE     ((int32_t)-223)
#define RFB_ENC_JPEG_QUALITY_9   ((int32_t)-23)

/* VNC security types */
#define RFB_SEC_NONE             1
#define RFB_SEC_VNC_AUTH         2

/* ── Internal structure ─────────────────────────────────────── */

struct display_t {
    display_config_t  cfg;
    display_type_t    type;

    /* Raw socket connection */
    int               sock_fd;        /* TCP socket to VNC server */
    int               native_w, native_h;

    /* Framebuffer (BGRA, updated by framebuffer updates) */
    uint8_t          *framebuffer;    /* native_w * native_h * 4 bytes */

    /* 4 persistent zlib streams for Tight encoding */
    z_stream          zstreams[4];
    int               zstream_inited[4];

    /* Backend function pointers (for display.c dispatch) */
    char *(*capture_fn)(struct display_t *d);
    void  (*close_fn)(struct display_t *d);
};

/* ── Reliable recv ──────────────────────────────────────────── */

/* Read exactly n bytes from socket.  Returns 0 on success, -1 on error. */
static int recv_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = recv(fd, p, remaining, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            nash_log("[display_vnc] connection closed (wanted %zu, got %zu)",
                    n, n - remaining);
            return -1;
        }
        p += r;
        remaining -= (size_t)r;
    }
    return 0;
}

/* Send exactly n bytes.  Returns 0 on success, -1 on error. */
static int send_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t w = send(fd, p, remaining, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            nash_log("[display_vnc] send failed: %s", strerror(errno));
            return -1;
        }
        p += w;
        remaining -= (size_t)w;
    }
    return 0;
}

/* ── Byte-order helpers ─────────────────────────────────────── */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static int32_t rd32s(const uint8_t *p) {
    uint32_t u = rd32(p);
    int32_t s;
    memcpy(&s, &u, 4);
    return s;
}

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

/* ── VNC DES authentication ─────────────────────────────────── */

/* VNC auth uses DES with bit-reversed key bytes.
 * We use OpenSSL's DES (already linked via -lcrypto).
 * The legacy DES API is deprecated since OpenSSL 3.0 but there is
 * no replacement — VNC authentication requires exactly this cipher. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <openssl/des.h>

static uint8_t bit_reverse(uint8_t b) {
    return (uint8_t)(
        ((b & 0x01) << 7) | ((b & 0x02) << 5) | ((b & 0x04) << 3) |
        ((b & 0x08) << 1) | ((b & 0x10) >> 1) | ((b & 0x20) >> 3) |
        ((b & 0x40) >> 5) | ((b & 0x80) >> 7)
    );
}

static int vnc_des_encrypt(const char *password, const uint8_t *challenge,
                            uint8_t *response) {
    /* Prepare key: password padded to 8 bytes, each byte bit-reversed */
    uint8_t key[8] = {0};
    size_t pwlen = password ? strlen(password) : 0;
    if (pwlen > 8) pwlen = 8;
    for (size_t i = 0; i < pwlen; i++)
        key[i] = bit_reverse((uint8_t)password[i]);

    DES_key_schedule ks;
    DES_set_key_unchecked((const_DES_cblock *)key, &ks);

    /* Encrypt two 8-byte blocks */
    DES_ecb_encrypt((const_DES_cblock *)challenge,
                    (DES_cblock *)response, &ks, DES_ENCRYPT);
    DES_ecb_encrypt((const_DES_cblock *)(challenge + 8),
                    (DES_cblock *)(response + 8), &ks, DES_ENCRYPT);
    return 0;
}
#pragma GCC diagnostic pop

/* ── RFB connection ─────────────────────────────────────────── */

static int rfb_connect(display_t *d) {
    const char *host = d->cfg.vnc_host ? d->cfg.vnc_host : "localhost";
    int port = d->cfg.vnc_port > 0 ? d->cfg.vnc_port : 5900;

    /* DNS resolution */
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        nash_log("[display_vnc] DNS lookup failed for %s: %s",
                host, gai_strerror(rc));
        return -1;
    }

    /* TCP connect */
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        nash_log("[display_vnc] socket(): %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    /* Set TCP_NODELAY for low-latency event sending */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Connect with timeout via poll */
    struct timeval tv = {15, 0};  /* 15s timeout */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        nash_log("[display_vnc] connect to %s:%d failed: %s",
                host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    /* ── RFB 3.8 Handshake ────────────────────────────────── */

    /* 1. Read server version (12 bytes: "RFB 003.008\n") */
    uint8_t ver[12];
    if (recv_exact(fd, ver, 12) < 0) goto fail;

    /* 2. Send client version */
    if (send_exact(fd, "RFB 003.008\n", 12) < 0) goto fail;

    /* 3. Security negotiation */
    uint8_t num_types;
    if (recv_exact(fd, &num_types, 1) < 0) goto fail;

    if (num_types == 0) {
        /* Connection rejected — read reason */
        uint8_t len_buf[4];
        if (recv_exact(fd, len_buf, 4) == 0) {
            uint32_t msg_len = rd32(len_buf);
            if (msg_len > 0 && msg_len < 4096) {
                char *msg = malloc(msg_len + 1);
                if (msg && recv_exact(fd, msg, msg_len) == 0) {
                    msg[msg_len] = '\0';
                    nash_log("[display_vnc] server rejected: %s", msg);
                }
                free(msg);
            }
        }
        goto fail;
    }

    uint8_t *sec_types = malloc(num_types);
    if (!sec_types) goto fail;
    if (recv_exact(fd, sec_types, num_types) < 0) { free(sec_types); goto fail; }

    /* Prefer no auth, fall back to VNC auth */
    int has_none = 0, has_vnc = 0;
    for (int i = 0; i < num_types; i++) {
        if (sec_types[i] == RFB_SEC_NONE) has_none = 1;
        if (sec_types[i] == RFB_SEC_VNC_AUTH) has_vnc = 1;
    }
    free(sec_types);

    if (has_none) {
        uint8_t choice = RFB_SEC_NONE;
        if (send_exact(fd, &choice, 1) < 0) goto fail;
    } else if (has_vnc) {
        uint8_t choice = RFB_SEC_VNC_AUTH;
        if (send_exact(fd, &choice, 1) < 0) goto fail;

        /* Read 16-byte challenge */
        uint8_t challenge[16], response[16];
        if (recv_exact(fd, challenge, 16) < 0) goto fail;

        if (!d->cfg.vnc_password || !d->cfg.vnc_password[0]) {
            nash_log("[display_vnc] server requires password but none configured");
            goto fail;
        }

        vnc_des_encrypt(d->cfg.vnc_password, challenge, response);
        if (send_exact(fd, response, 16) < 0) goto fail;
    } else {
        nash_log("[display_vnc] no supported security type");
        goto fail;
    }

    /* 4. Read SecurityResult (4 bytes, must be 0) */
    uint8_t result_buf[4];
    if (recv_exact(fd, result_buf, 4) < 0) goto fail;
    if (rd32(result_buf) != 0) {
        nash_log("[display_vnc] authentication failed");
        goto fail;
    }

    /* 5. ClientInit — shared=1 */
    uint8_t shared = 1;
    if (send_exact(fd, &shared, 1) < 0) goto fail;

    /* 6. ServerInit — 24 bytes: width(2) + height(2) + pixel_format(16) + name_len(4) */
    uint8_t sinit[24];
    if (recv_exact(fd, sinit, 24) < 0) goto fail;
    d->native_w = rd16(sinit);
    d->native_h = rd16(sinit + 2);
    uint32_t name_len = rd32(sinit + 20);

    /* Read desktop name (and discard) */
    if (name_len > 0 && name_len < 65536) {
        char *name = malloc(name_len + 1);
        if (name) {
            if (recv_exact(fd, name, name_len) < 0) { free(name); goto fail; }
            name[name_len] = '\0';
            nash_log("[display_vnc] connected to %s:%d — \"%s\" (%dx%d)",
                    host, port, name, d->native_w, d->native_h);
            free(name);
        }
    }

    /* 7. SetPixelFormat — 32-bit BGRA (matches nashell's format)
     * msg(1) + pad(3) + bpp(1) + depth(1) + bigEndian(1) + trueColor(1) +
     * rMax(2) + gMax(2) + bMax(2) + rShift(1) + gShift(1) + bShift(1) + pad(3)
     * = 20 bytes */
    uint8_t pixfmt[20] = {0};
    pixfmt[0]  = RFB_SET_PIXEL_FORMAT;  /* msg type */
    /* pad[1..3] = 0 */
    pixfmt[4]  = 32;   /* bpp */
    pixfmt[5]  = 24;   /* depth */
    pixfmt[6]  = 0;    /* big-endian = false */
    pixfmt[7]  = 1;    /* true-colour = true */
    wr16(pixfmt + 8,  255);  /* red-max */
    wr16(pixfmt + 10, 255);  /* green-max */
    wr16(pixfmt + 12, 255);  /* blue-max */
    pixfmt[14] = 16;   /* red-shift */
    pixfmt[15] = 8;    /* green-shift */
    pixfmt[16] = 0;    /* blue-shift */
    /* pad[17..19] = 0 */
    if (send_exact(fd, pixfmt, 20) < 0) goto fail;

    /* 8. SetEncodings — prefer Tight, then Raw, CopyRect, DesktopSize,
     *    JPEG quality 9 pseudo-encoding (-23) */
    int32_t encodings[] = {
        RFB_ENC_TIGHT,
        RFB_ENC_RAW,
        RFB_ENC_COPYRECT,
        RFB_ENC_DESKTOP_SIZE,
        RFB_ENC_JPEG_QUALITY_9,
    };
    int nenc = (int)(sizeof(encodings) / sizeof(encodings[0]));

    uint8_t enc_msg[4 + 5 * 4];  /* header + 5 encodings */
    enc_msg[0] = RFB_SET_ENCODINGS;
    enc_msg[1] = 0;  /* pad */
    wr16(enc_msg + 2, (uint16_t)nenc);
    for (int i = 0; i < nenc; i++) {
        uint32_t u;
        memcpy(&u, &encodings[i], 4);
        wr32(enc_msg + 4 + i * 4, u);
    }
    if (send_exact(fd, enc_msg, 4 + nenc * 4) < 0) goto fail;

    /* Allocate framebuffer */
    d->framebuffer = calloc((size_t)d->native_w * d->native_h * 4, 1);
    if (!d->framebuffer) goto fail;

    d->sock_fd = fd;
    return 0;

fail:
    close(fd);
    return -1;
}

/* ── Tight compact length ───────────────────────────────────── */

/* Read 1-3 byte compact length (7 bits per byte, high bit = continuation) */
static int read_compact_len(int fd, uint32_t *out) {
    uint8_t b;
    if (recv_exact(fd, &b, 1) < 0) return -1;
    uint32_t len = b & 0x7F;
    if (b & 0x80) {
        if (recv_exact(fd, &b, 1) < 0) return -1;
        len |= (uint32_t)(b & 0x7F) << 7;
        if (b & 0x80) {
            if (recv_exact(fd, &b, 1) < 0) return -1;
            len |= (uint32_t)b << 14;
        }
    }
    *out = len;
    return 0;
}

/* ── Tight encoding parser ──────────────────────────────────── */

/* Parse a single Tight-encoded rectangle and write RGB into framebuffer.
 * Returns 0 on success, -1 on error.
 * If the rect is JPEG, writes decoded pixels into the framebuffer. */
static int parse_tight_rect(display_t *d, int rx, int ry, int rw, int rh) {
    int fd = d->sock_fd;
    uint8_t ctl;
    if (recv_exact(fd, &ctl, 1) < 0) return -1;

    /* Reset zlib streams per bits 0-3 */
    for (int i = 0; i < 4; i++) {
        if (ctl & (1 << i)) {
            if (d->zstream_inited[i]) {
                inflateEnd(&d->zstreams[i]);
                d->zstream_inited[i] = 0;
            }
        }
    }

    if (ctl & 0x80) {
        if (ctl & 0x10) {
            /* ── JPEG sub-encoding (0x90) ──────────────────── */
            uint32_t jpeg_len;
            if (read_compact_len(fd, &jpeg_len) < 0) return -1;

            uint8_t *jpeg_data = malloc(jpeg_len);
            if (!jpeg_data) return -1;
            if (recv_exact(fd, jpeg_data, jpeg_len) < 0) { free(jpeg_data); return -1; }

            /* Decode JPEG → RGB and blit into framebuffer as BGRA */
            struct jpeg_decompress_struct cinfo;
            struct jpeg_error_mgr jerr;
            cinfo.err = jpeg_std_error(&jerr);
            jpeg_create_decompress(&cinfo);
            jpeg_mem_src(&cinfo, jpeg_data, jpeg_len);
            if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
                jpeg_destroy_decompress(&cinfo);
                free(jpeg_data);
                return -1;
            }
            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);

            int jw = (int)cinfo.output_width;
            int jh = (int)cinfo.output_height;
            uint8_t *row_buf = malloc((size_t)jw * 3);
            if (!row_buf) {
                jpeg_destroy_decompress(&cinfo);
                free(jpeg_data);
                return -1;
            }

            for (int y = 0; y < jh && y < rh; y++) {
                JSAMPROW rp = row_buf;
                jpeg_read_scanlines(&cinfo, &rp, 1);
                /* Blit RGB row into BGRA framebuffer */
                int dy = ry + y;
                if (dy < 0 || dy >= d->native_h) continue;
                for (int x = 0; x < jw && x < rw; x++) {
                    int dx = rx + x;
                    if (dx < 0 || dx >= d->native_w) continue;
                    uint8_t *dst = d->framebuffer + ((size_t)dy * d->native_w + dx) * 4;
                    dst[0] = row_buf[x * 3 + 2]; /* B */
                    dst[1] = row_buf[x * 3 + 1]; /* G */
                    dst[2] = row_buf[x * 3 + 0]; /* R */
                    dst[3] = 255;                 /* A */
                }
            }

            free(row_buf);
            jpeg_finish_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            free(jpeg_data);
            return 0;

        } else {
            /* ── Fill sub-encoding (0x80) ──────────────────── */
            uint8_t color[3];  /* tpixel RGB */
            if (recv_exact(fd, color, 3) < 0) return -1;

            for (int y = 0; y < rh; y++) {
                int dy = ry + y;
                if (dy < 0 || dy >= d->native_h) continue;
                for (int x = 0; x < rw; x++) {
                    int dx = rx + x;
                    if (dx < 0 || dx >= d->native_w) continue;
                    uint8_t *dst = d->framebuffer + ((size_t)dy * d->native_w + dx) * 4;
                    dst[0] = color[2]; /* B */
                    dst[1] = color[1]; /* G */
                    dst[2] = color[0]; /* R */
                    dst[3] = 255;
                }
            }
            return 0;
        }
    }

    /* ── Basic compression ─────────────────────────────────── */
    int stream_id = (ctl >> 4) & 0x03;
    int read_filter = (ctl >> 6) & 0x01;

    int filter_id = 0;  /* CopyFilter */
    if (read_filter) {
        uint8_t fid;
        if (recv_exact(fd, &fid, 1) < 0) return -1;
        filter_id = fid;
    }

    int tpixel_size = 3;  /* RGB */
    int data_size;
    int num_colors = 0;
    uint8_t palette[256][3];

    if (filter_id == 1) {
        /* PaletteFilter */
        uint8_t nc;
        if (recv_exact(fd, &nc, 1) < 0) return -1;
        num_colors = nc + 1;
        for (int i = 0; i < num_colors; i++) {
            if (recv_exact(fd, palette[i], 3) < 0) return -1;
        }
        if (num_colors == 2) {
            int row_bytes = (rw + 7) / 8;
            data_size = row_bytes * rh;
        } else {
            data_size = rw * rh;
        }
    } else {
        /* CopyFilter (0) or GradientFilter (2) */
        data_size = rw * rh * tpixel_size;
    }

    /* Read data: < 12 bytes = uncompressed, else zlib */
    uint8_t *raw = malloc(data_size > 0 ? (size_t)data_size : 1);
    if (!raw) return -1;

    if (data_size < 12) {
        if (recv_exact(fd, raw, (size_t)data_size) < 0) { free(raw); return -1; }
    } else {
        uint32_t comp_len;
        if (read_compact_len(fd, &comp_len) < 0) { free(raw); return -1; }

        uint8_t *comp = malloc(comp_len);
        if (!comp) { free(raw); return -1; }
        if (recv_exact(fd, comp, comp_len) < 0) { free(comp); free(raw); return -1; }

        /* Decompress with persistent zlib stream */
        z_stream *zs = &d->zstreams[stream_id];
        if (!d->zstream_inited[stream_id]) {
            memset(zs, 0, sizeof(*zs));
            if (inflateInit(zs) != Z_OK) { free(comp); free(raw); return -1; }
            d->zstream_inited[stream_id] = 1;
        }

        zs->next_in = comp;
        zs->avail_in = comp_len;
        zs->next_out = raw;
        zs->avail_out = (uInt)data_size;

        int zrc = inflate(zs, Z_SYNC_FLUSH);
        free(comp);
        if (zrc != Z_OK && zrc != Z_STREAM_END) {
            nash_log("[display_vnc] zlib inflate failed: %d", zrc);
            free(raw);
            return -1;
        }
    }

    /* Convert to framebuffer pixels */
    if (filter_id == 1) {
        /* Palette */
        if (num_colors == 2) {
            int row_bytes = (rw + 7) / 8;
            for (int y = 0; y < rh; y++) {
                int dy = ry + y;
                if (dy < 0 || dy >= d->native_h) { continue; }
                for (int x = 0; x < rw; x++) {
                    int dx = rx + x;
                    if (dx < 0 || dx >= d->native_w) continue;
                    int byte_idx = y * row_bytes + x / 8;
                    int bit = (raw[byte_idx] >> (7 - x % 8)) & 1;
                    uint8_t *dst = d->framebuffer + ((size_t)dy * d->native_w + dx) * 4;
                    dst[0] = palette[bit][2]; /* B */
                    dst[1] = palette[bit][1]; /* G */
                    dst[2] = palette[bit][0]; /* R */
                    dst[3] = 255;
                }
            }
        } else {
            for (int y = 0; y < rh; y++) {
                int dy = ry + y;
                if (dy < 0 || dy >= d->native_h) { continue; }
                for (int x = 0; x < rw; x++) {
                    int dx = rx + x;
                    if (dx < 0 || dx >= d->native_w) continue;
                    int idx = raw[y * rw + x];
                    if (idx >= num_colors) idx = 0;
                    uint8_t *dst = d->framebuffer + ((size_t)dy * d->native_w + dx) * 4;
                    dst[0] = palette[idx][2];
                    dst[1] = palette[idx][1];
                    dst[2] = palette[idx][0];
                    dst[3] = 255;
                }
            }
        }
    } else {
        /* CopyFilter or GradientFilter — raw RGB tpixels */
        for (int y = 0; y < rh; y++) {
            int dy = ry + y;
            if (dy < 0 || dy >= d->native_h) { continue; }
            for (int x = 0; x < rw; x++) {
                int dx = rx + x;
                if (dx < 0 || dx >= d->native_w) continue;
                int si = (y * rw + x) * 3;
                uint8_t *dst = d->framebuffer + ((size_t)dy * d->native_w + dx) * 4;
                dst[0] = raw[si + 2]; /* B */
                dst[1] = raw[si + 1]; /* G */
                dst[2] = raw[si + 0]; /* R */
                dst[3] = 255;
            }
        }
    }

    free(raw);
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────── */

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

/* Write BGRA framebuffer to JPEG file (B and R are in VNC's BGRA order) */
static int write_jpeg(const char *path, const uint8_t *bgra,
                      int width, int height, int quality) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = (JDIMENSION)width;
    cinfo.image_height = (JDIMENSION)height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    uint8_t *row = malloc((size_t)width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *src = bgra + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 2]; /* R from BGRA offset 2 */
            row[x * 3 + 1] = src[x * 4 + 1]; /* G from BGRA offset 1 */
            row[x * 3 + 2] = src[x * 4 + 0]; /* B from BGRA offset 0 */
        }
        JSAMPROW rowp = row;
        jpeg_write_scanlines(&cinfo, &rowp, 1);
    }

    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 0;
}

/* ── Capture ────────────────────────────────────────────────── */

static char *display_capture_vnc(display_t *d) {
    if (d->sock_fd < 0) return NULL;

    /* Send FramebufferUpdateRequest (non-incremental for full refresh)
     * msg(1) + incremental(1) + x(2) + y(2) + w(2) + h(2) = 10 bytes */
    uint8_t req[10];
    req[0] = RFB_FB_UPDATE_REQUEST;
    req[1] = 0;  /* non-incremental = full refresh */
    wr16(req + 2, 0);
    wr16(req + 4, 0);
    wr16(req + 6, (uint16_t)d->native_w);
    wr16(req + 8, (uint16_t)d->native_h);
    if (send_exact(d->sock_fd, req, 10) < 0) return NULL;

    /* Read server response — loop until we get a FramebufferUpdate */
    for (;;) {
        uint8_t msg_type;
        if (recv_exact(d->sock_fd, &msg_type, 1) < 0) return NULL;

        if (msg_type == RFB_FB_UPDATE) {
            /* Padding(1) + num_rects(2) */
            uint8_t hdr[3];
            if (recv_exact(d->sock_fd, hdr, 3) < 0) return NULL;
            uint16_t num_rects = rd16(hdr + 1);

            for (int i = 0; i < num_rects; i++) {
                /* x(2) + y(2) + w(2) + h(2) + encoding(4) = 12 bytes */
                uint8_t rect_hdr[12];
                if (recv_exact(d->sock_fd, rect_hdr, 12) < 0) return NULL;

                int rx = rd16(rect_hdr);
                int ry = rd16(rect_hdr + 2);
                int rw = rd16(rect_hdr + 4);
                int rh = rd16(rect_hdr + 6);
                int32_t enc = rd32s(rect_hdr + 8);

                if (enc == RFB_ENC_DESKTOP_SIZE) {
                    /* Resize — update dimensions and reallocate framebuffer */
                    d->native_w = rw;
                    d->native_h = rh;
                    free(d->framebuffer);
                    d->framebuffer = calloc((size_t)rw * rh * 4, 1);
                    if (!d->framebuffer) return NULL;

                } else if (enc == RFB_ENC_TIGHT) {
                    if (parse_tight_rect(d, rx, ry, rw, rh) < 0) return NULL;

                } else if (enc == RFB_ENC_RAW) {
                    /* Raw: read w*h*4 bytes (BGRA), copy into framebuffer */
                    size_t raw_size = (size_t)rw * rh * 4;
                    uint8_t *raw = malloc(raw_size);
                    if (!raw) return NULL;
                    if (recv_exact(d->sock_fd, raw, raw_size) < 0) { free(raw); return NULL; }

                    for (int y = 0; y < rh; y++) {
                        int dy = ry + y;
                        if (dy < 0 || dy >= d->native_h) continue;
                        size_t src_off = (size_t)y * rw * 4;
                        size_t dst_off = ((size_t)dy * d->native_w + rx) * 4;
                        int copy_w = rw;
                        if (rx + copy_w > d->native_w) copy_w = d->native_w - rx;
                        if (rx >= 0 && copy_w > 0)
                            memcpy(d->framebuffer + dst_off, raw + src_off, (size_t)copy_w * 4);
                    }
                    free(raw);

                } else if (enc == RFB_ENC_COPYRECT) {
                    /* CopyRect: read source x(2) + y(2) */
                    uint8_t cr[4];
                    if (recv_exact(d->sock_fd, cr, 4) < 0) return NULL;
                    int sx = rd16(cr);
                    int sy = rd16(cr + 2);

                    /* Copy pixels within framebuffer */
                    for (int y = 0; y < rh; y++) {
                        int src_y = sy + y;
                        int dst_y = ry + y;
                        if (src_y < 0 || src_y >= d->native_h) continue;
                        if (dst_y < 0 || dst_y >= d->native_h) continue;
                        size_t src_off = ((size_t)src_y * d->native_w + sx) * 4;
                        size_t dst_off = ((size_t)dst_y * d->native_w + rx) * 4;
                        int copy_w = rw;
                        if (rx + copy_w > d->native_w) copy_w = d->native_w - rx;
                        if (sx + copy_w > d->native_w) copy_w = d->native_w - sx;
                        if (copy_w > 0)
                            memmove(d->framebuffer + dst_off,
                                    d->framebuffer + src_off,
                                    (size_t)copy_w * 4);
                    }

                } else {
                    nash_log("[display_vnc] unsupported encoding %d, skipping rect", enc);
                    /* Cannot skip unknown encoding — we don't know its size */
                    return NULL;
                }
            }
            break;  /* Done processing FramebufferUpdate */

        } else if (msg_type == RFB_SERVER_CUT_TEXT) {
            /* ServerCutText: pad(3) + len(4) + text(len) */
            uint8_t sct[7];
            if (recv_exact(d->sock_fd, sct, 7) < 0) return NULL;
            uint32_t tlen = rd32(sct + 3);
            if (tlen > 0 && tlen < 10 * 1024 * 1024) {
                uint8_t *discard = malloc(tlen);
                if (discard) {
                    recv_exact(d->sock_fd, discard, tlen);
                    free(discard);
                }
            }

        } else if (msg_type == RFB_BELL) {
            /* Bell: no payload */

        } else if (msg_type == RFB_SET_COLOUR_MAP) {
            /* SetColourMapEntries: pad(1) + firstColour(2) + numColours(2) + colours(6*n) */
            uint8_t cm[5];
            if (recv_exact(d->sock_fd, cm, 5) < 0) return NULL;
            uint16_t ncols = rd16(cm + 3);
            uint8_t *discard = malloc((size_t)ncols * 6);
            if (discard) {
                recv_exact(d->sock_fd, discard, (size_t)ncols * 6);
                free(discard);
            }

        } else {
            nash_log("[display_vnc] unknown server message type %d", msg_type);
            return NULL;
        }
    }

    /* Write framebuffer to JPEG */
    const char *dir = d->cfg.screenshot_dir;
    if (!dir || !dir[0]) dir = "/tmp/device_screenshots";
    char *path = make_screenshot_path(dir);
    if (!path) return NULL;

    if (write_jpeg(path, d->framebuffer, d->native_w, d->native_h, 85) != 0) {
        nash_log("[display_vnc] failed to write JPEG: %s", path);
        free(path);
        return NULL;
    }

    return path;
}

/* ── Close ──────────────────────────────────────────────────── */

static void display_close_vnc(display_t *d) {
    if (d->sock_fd >= 0) {
        close(d->sock_fd);
        d->sock_fd = -1;
    }
    for (int i = 0; i < 4; i++) {
        if (d->zstream_inited[i]) {
            inflateEnd(&d->zstreams[i]);
            d->zstream_inited[i] = 0;
        }
    }
    free(d->framebuffer);
    free(d);
}

/* ── Open ───────────────────────────────────────────────────── */

display_t *display_open_vnc(const display_config_t *cfg) {
    display_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->type = DISPLAY_VNC;
    d->cfg = *cfg;
    d->sock_fd = -1;
    d->capture_fn = display_capture_vnc;
    d->close_fn = display_close_vnc;
    memset(d->zstream_inited, 0, sizeof(d->zstream_inited));

    if (rfb_connect(d) < 0) {
        free(d);
        return NULL;
    }

    return d;
}

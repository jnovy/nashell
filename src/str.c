#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

str_t str_new(size_t initial_cap) {
    str_t s;
    s.cap  = initial_cap > 0 ? initial_cap : 64;
    s.data = malloc(s.cap);
    if (!s.data) { s.len = 0; s.cap = 0; return s; }
    s.len  = 0;
    s.data[0] = '\0';
    return s;
}

void str_free(str_t *s) {
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

void str_clear(str_t *s) {
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

static void str_grow(str_t *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return;
    size_t new_cap = s->cap ? s->cap * 2 : 64;
    while (new_cap < s->len + need + 1) new_cap *= 2;
    char *p = realloc(s->data, new_cap);
    if (!p) return;  /* keep old data on OOM */
    s->data = p;
    s->cap  = new_cap;
}

void str_append(str_t *s, const char *data, size_t len) {
    str_grow(s, len);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
}

void str_append_cstr(str_t *s, const char *cstr) {
    str_append(s, cstr, strlen(cstr));
}

void str_appendf(str_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        str_grow(s, (size_t)n);
        vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap2);
        s->len += (size_t)n;
    }
    va_end(ap2);
}

char *str_steal(str_t *s) {
    char *p = s->data;
    s->data = NULL;
    s->len = s->cap = 0;
    return p;
}

const char *str_cstr(const str_t *s) {
    return s->data ? s->data : "";
}

/* Format seconds into human-readable duration */
/* ── UTF-8 safe truncation ──────────────────────────────────────── */

int utf8_truncate(char *dst, const char *src, int max_bytes) {
    if (!dst || !src || max_bytes <= 0) {
        if (dst) dst[0] = '\0';
        return 0;
    }

    /* Find actual length to copy (min of strlen and max_bytes) */
    int len = 0;
    while (len < max_bytes && src[len]) len++;

    /* If we didn't hit max_bytes, the string fits entirely */
    if (!src[len]) {
        memcpy(dst, src, len);
        dst[len] = '\0';
        return len;
    }

    /* We hit max_bytes — back up past any incomplete UTF-8 sequence.
     * UTF-8 continuation bytes have the form 10xxxxxx (0x80..0xBF).
     * Walk backwards past continuation bytes, then check if the
     * leading byte expects more bytes than we have. */
    int cut = len;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
        cut--;

    /* If we backed up to a multi-byte leader, check if the full
     * sequence fits. If not, drop the incomplete leader too. */
    if (cut > 0) {
        unsigned char lead = (unsigned char)src[cut - 1];
        int expected = 1;
        if ((lead & 0xE0) == 0xC0) expected = 2;       /* 110xxxxx */
        else if ((lead & 0xF0) == 0xE0) expected = 3;  /* 1110xxxx */
        else if ((lead & 0xF8) == 0xF0) expected = 4;  /* 11110xxx */
        int have = len - (cut - 1);
        if (have < expected)
            cut--;  /* drop the incomplete leader */
    }

    memcpy(dst, src, cut);
    dst[cut] = '\0';
    return cut;
}

const char *fmt_duration(double seconds, char *buf, size_t sz) {
    int s = (int)seconds;
    if (s < 60) {
        snprintf(buf, sz, "%ds", s);
    } else {
        int m = s / 60; s %= 60;
        if (m < 60) {
            snprintf(buf, sz, "%dm%02ds", m, s);
        } else {
            int h = m / 60; m %= 60;
            if (h < 24) {
                snprintf(buf, sz, "%dh%02dm%02ds", h, m, s);
            } else {
                int d = h / 24; h %= 24;
                snprintf(buf, sz, "%dd%02dh%02dm%02ds", d, h, m, s);
            }
        }
    }
    return buf;
}

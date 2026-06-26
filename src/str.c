#include "str.h"
#include "nash_limits.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <curl/curl.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/file.h>
#include <fcntl.h>

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
/* ── UTF-8 helpers ──────────────────────────────────────────────── */

int utf8_char_len(const char *p) {
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  /* invalid: treat as single byte */
}

const char *utf8_next(const char *p) {
    return p + utf8_char_len(p);
}

const char *utf8_prev(const char *begin, const char *p) {
    if (p <= begin) return begin;
    p--;
    while (p > begin && ((unsigned char)*p & 0xC0) == 0x80) p--;
    return p;
}

/* ── UTF-8 display width ───────────────────────────────────────── */

/* Decode a UTF-8 character at *p into a Unicode codepoint.
 * Returns the codepoint, or (uint32_t)-1 on invalid sequence. */
static uint32_t utf8_decode(const char *p, int *out_len) {
    unsigned char c = (unsigned char)p[0];
    uint32_t cp;
    int len;
    if (c < 0x80)        { cp = c;              len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { *out_len = 1; return (uint32_t)-1; }
    for (int i = 1; i < len; i++) {
        if (((unsigned char)p[i] & 0xC0) != 0x80) { *out_len = 1; return (uint32_t)-1; }
        cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
    }
    *out_len = len;
    return cp;
}

/* Is a codepoint fullwidth or wide (2 display columns)?
 * Covers CJK Unified Ideographs, Katakana, Hangul, fullwidth forms, etc. */
static int is_wide_codepoint(uint32_t cp) {
    return (cp >= 0x1100 &&
            (cp <= 0x115F ||                    /* Hangul Jamo */
             cp == 0x2329 || cp == 0x232A ||    /* angle brackets */
             (cp >= 0x2E80 && cp <= 0x303E) ||  /* CJK radicals, symbols */
             (cp >= 0x3040 && cp <= 0x33BF) ||  /* Hiragana, Katakana, CJK compat */
             (cp >= 0x3400 && cp <= 0x4DBF) ||  /* CJK Unified Ext A */
             (cp >= 0x4E00 && cp <= 0xA4CF) ||  /* CJK Unified + Yi */
             (cp >= 0xA960 && cp <= 0xA97C) ||  /* Hangul Jamo Extended-A */
             (cp >= 0xAC00 && cp <= 0xD7A3) ||  /* Hangul Syllables */
             (cp >= 0xF900 && cp <= 0xFAFF) ||  /* CJK Compat Ideographs */
             (cp >= 0xFE10 && cp <= 0xFE6F) ||  /* CJK compat forms, small forms */
             (cp >= 0xFF01 && cp <= 0xFF60) ||  /* Fullwidth ASCII */
             (cp >= 0xFFE0 && cp <= 0xFFE6) ||  /* Fullwidth signs */
             (cp >= 0x1F300 && cp <= 0x1F9FF) || /* Emoji (misc symbols, emoticons) */
             (cp >= 0x20000 && cp <= 0x2FFFF) || /* CJK Unified Ext B-F */
             (cp >= 0x30000 && cp <= 0x3FFFF))); /* CJK Unified Ext G+ */
}

int utf8_char_width(const char *p) {
    int len;
    uint32_t cp = utf8_decode(p, &len);
    if (cp == (uint32_t)-1) return 1;  /* invalid byte: treat as 1 column */
    if (cp < 32 || cp == 127) return 0; /* control chars: 0 width */
    if (is_wide_codepoint(cp)) return 2;
    return 1;
}

int utf8_display_width(const char *s, int nbytes) {
    int w = 0;
    const char *end = s + nbytes;
    while (s < end && *s) {
        w += utf8_char_width(s);
        s += utf8_char_len(s);
    }
    return w;
}

int utf8_bytes_for_width(const char *s, int nbytes, int max_cols) {
    int cols = 0;
    const char *start = s;
    const char *end = s + nbytes;
    while (s < end && *s) {
        int cw = utf8_char_width(s);
        if (cols + cw > max_cols) break;
        cols += cw;
        s += utf8_char_len(s);
    }
    return (int)(s - start);
}

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

    /* We hit max_bytes — need to find the last complete UTF-8 character
     * entirely within [0, len). Walk backwards from len, skipping
     * continuation bytes (10xxxxxx). After the loop, cut points to
     * the position just after the leader/ASCII byte that starts the
     * last character sequence in [0, len). */
    int cut = len;
    while (cut > 0 && ((unsigned char)src[cut - 1] & 0xC0) == 0x80)
        cut--;

    /* Now src[cut-1] is the leader (or ASCII) of the last character
     * included in [0, len). Check if it's a multi-byte leader whose
     * continuations extend beyond len. */
    if (cut > 0) {
        unsigned char lead = (unsigned char)src[cut - 1];
        int expected = 1;
        if ((lead & 0xE0) == 0xC0) expected = 2;       /* 110xxxxx */
        else if ((lead & 0xF0) == 0xE0) expected = 3;  /* 1110xxxx */
        else if ((lead & 0xF8) == 0xF0) expected = 4;  /* 11110xxx */
        /* The character spans [cut-1, cut-1+expected).
         * If complete within [0, len), include it by advancing cut.
         * If incomplete, drop the partial bytes we included. */
        if (cut - 1 + expected <= len) {
            /* Complete character — advance cut to include it */
            cut = cut - 1 + expected;
        } else {
            /* Incomplete — drop the partial character */
            cut--;
        }
        /* If cut went negative, clamp to 0 */
        if (cut < 0) cut = 0;
    }

    memcpy(dst, src, cut);
    dst[cut] = '\0';
    return cut;
}

size_t utf8_clamp(const char *s, size_t max_bytes) {
    if (!s || max_bytes == 0) return 0;
    /* Find actual length up to max_bytes */
    size_t len = 0;
    while (len < max_bytes && s[len]) len++;
    /* String fits entirely — no clamping needed */
    if (len < max_bytes || !s[len]) return len;
    /* Walk backwards past any continuation bytes (10xxxxxx) */
    size_t cut = len;
    while (cut > 0 && ((unsigned char)s[cut - 1] & 0xC0) == 0x80)
        cut--;
    /* Now s[cut-1] is a leader byte — check if the character is complete */
    if (cut > 0) {
        unsigned char lead = (unsigned char)s[cut - 1];
        int expected = 1;
        if ((lead & 0xE0) == 0xC0) expected = 2;
        else if ((lead & 0xF0) == 0xE0) expected = 3;
        else if ((lead & 0xF8) == 0xF0) expected = 4;
        if (cut - 1 + (size_t)expected <= len)
            cut = cut - 1 + (size_t)expected;
        else
            cut--;
    }
    return cut;
}

/* ── file I/O ───────────────────────────────────────────────────── */

int mkdir_p(const char *path, mode_t mode) {
    char tmp[NASH_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, len + 1);
    /* Strip trailing slash */
    if (tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Create session directory: <nash_dir>/sessions/<epoch.NNNNN>/ */
char *create_session_dir(const char *nash_dir) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    char sessions_base[1024];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
    mkdir(sessions_base, 0755);

    char path[1088];  /* sessions_base (1024) + "/" + epoch.nanos (~30) */
    snprintf(path, sizeof(path), "%s/%ld.%05ld",
             sessions_base, (long)tp.tv_sec, tp.tv_nsec / 10000);
    mkdir(path, 0755);
    return strdup(path);
}

/* ── Session locking ─────────────────────────────────────────────── */

int session_lock_acquire(const char *session_dir) {
    if (!session_dir) return -1;

    char lock_path[1120];
    snprintf(lock_path, sizeof(lock_path), "%s/.lock", session_dir);

    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "[session] warning: cannot create lock file %s: %s\n",
                lock_path, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                    "[session] ERROR: session %s is already in use by another process\n",
                    session_dir);
        } else {
            fprintf(stderr, "[session] warning: flock(%s) failed: %s\n",
                    lock_path, strerror(errno));
        }
        close(fd);
        return -1;
    }

    return fd;
}

void session_lock_release(int fd) {
    if (fd >= 0) close(fd);  /* flock is automatically released on close */
}

char *slurp_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

void *slurp_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* ── Directory iteration ─────────────────────────────────────────── */

void for_each_dir_entry(const char *dirpath, const char *suffix,
                        dir_entry_cb cb, void *user_data) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    size_t sfx_len = suffix ? strlen(suffix) : 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (suffix) {
            size_t len = strlen(de->d_name);
            if (len <= sfx_len) continue;
            if (strcmp(de->d_name + len - sfx_len, suffix) != 0) continue;
        }

        char path[NASH_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);

        int rc = cb(dirpath, de->d_name, path, user_data);
        if (rc) break;
    }
    closedir(dir);
}

/* ── HTTP helpers (libcurl) ──────────────────────────────────── */

/* Curl write callback that appends to a str_t */
size_t str_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *s = userdata;
    str_append(s, ptr, size * nmemb);
    return size * nmemb;
}

int http_get(const char *url, long timeout_sec, str_t *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int http_get_web(const char *url, long timeout_sec, str_t *out, long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (http_code)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int http_post(const char *url, const char *body,
              struct curl_slist *headers, long timeout_sec, str_t *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int write_file(const char *path, const char *data, size_t len) {
    /* FIX 3b: Atomic write via temp file + rename.  Prevents data corruption
     * (empty/partial file) if the process crashes between open and close.
     * Previously used fopen("w") which truncates immediately. */
    char tmp[NASH_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    if (fflush(f) != 0 || n != len) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

cJSON *slurp_json(const char *path) {
    char *buf = slurp_file(path, NULL);
    if (!buf) return NULL;
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

/* ── counting ───────────────────────────────────────────────────── */

int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

/* ── duration formatting ────────────────────────────────────────── */

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

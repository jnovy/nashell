#include "str.h"
#include "nash_limits.h"
#include "nash_log.h"
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
#include <utf8proc.h>
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

/* Returns 0 on success, -1 on OOM (buffer unchanged). */
static int str_grow(str_t *s, size_t need) {
    if (need > SIZE_MAX - s->len - 1) return -1;  /* overflow guard */
    if (s->len + need + 1 <= s->cap) return 0;
    size_t new_cap = s->cap ? s->cap * 2 : 64;
    while (new_cap < s->len + need + 1) new_cap *= 2;
    char *p = realloc(s->data, new_cap);
    if (!p) return -1;  /* keep old data on OOM */
    s->data = p;
    s->cap  = new_cap;
    return 0;
}

void str_append(str_t *s, const char *data, size_t len) {
    if (str_grow(s, len) != 0) return;  /* OOM: skip append */
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
        if (str_grow(s, (size_t)n) != 0) { va_end(ap2); return; }
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

int utf8_char_width(const char *p) {
    int len;
    uint32_t cp = utf8_decode(p, &len);
    if (cp == (uint32_t)-1) return 1;  /* invalid byte: treat as 1 column */
    /* Delegate to utf8proc which has complete Unicode character width tables
     * (East_Asian_Width, Emoji_Presentation, combining marks, etc.). */
    int w = utf8proc_charwidth((utf8proc_int32_t)cp);
    return w > 0 ? w : (cp >= 0x20 && cp != 0x7F) ? 1 : 0;
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

/* Return sessions base directory, workspace-aware. */
char *sessions_base_dir(const char *nash_dir, const char *workspace) {
    char buf[1024];
    if (workspace && workspace[0]) {
        snprintf(buf, sizeof(buf), "%s/workspaces/%s", nash_dir, workspace);
        mkdir(buf, 0755);  /* ensure workspace dir exists */
        snprintf(buf, sizeof(buf), "%s/workspaces/%s/sessions", nash_dir, workspace);
    } else {
        snprintf(buf, sizeof(buf), "%s/sessions", nash_dir);
    }
    mkdir(buf, 0755);
    return strdup(buf);
}

char *create_session_dir(const char *nash_dir, const char *workspace) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    char *base = sessions_base_dir(nash_dir, workspace);

    char epoch[64];
    snprintf(epoch, sizeof(epoch), "%ld.%05ld",
             (long)tp.tv_sec, tp.tv_nsec / 10000);

    char path[1088];
    snprintf(path, sizeof(path), "%s/%s", base, epoch);
    free(base);
    mkdir(path, 0755);

    /* Pre-create session-scoped temporary directory under /tmp/.nash/ */
    char tmpdir[1088];
    if (workspace && workspace[0])
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/.nash/%s/%s", workspace, epoch);
    else
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/.nash/%s", epoch);
    mkdir_p(tmpdir, 0755);

    return strdup(path);
}

/* ── Session locking ─────────────────────────────────────────────── */

int session_lock_acquire(const char *session_dir) {
    if (!session_dir) return -1;

    char lock_path[1120];
    snprintf(lock_path, sizeof(lock_path), "%s/.lock", session_dir);

    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        nash_log("[session] warning: cannot create lock file %s: %s",
                lock_path, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            nash_log(
                    "[session] ERROR: session %s is already in use by another process",
                    session_dir);
        } else {
            nash_log("[session] warning: flock(%s) failed: %s",
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
    size_t total = size * nmemb;
    size_t prev_len = s->len;
    str_append(s, ptr, total);
    if (total > 0 && s->len == prev_len)
        return 0;  /* signal OOM to libcurl */
    return total;
}

int http_get(const char *url, long timeout_sec, str_t *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int http_get_h(const char *url, struct curl_slist *headers,
               long timeout_sec, str_t *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

/* Perform an HTTP PUT with a JSON body.
 * headers is a curl_slist (caller frees after call).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_put(const char *url, const char *body,
             struct curl_slist *headers, long timeout_sec, str_t *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int write_file(const char *path, const char *data, size_t len) {
    /* Atomic write via temp file + rename.  Prevents data corruption
     * (empty/partial file) if the process crashes between open and close.
     * Uses mkstemp for thread-safe unique temp filenames. */
    char tmp[NASH_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
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

/* Like fmt_duration but shows only the two most significant units.
 * Examples: "1d17h", "8h10m", "37m44s", "42s".  Good for columns. */
const char *fmt_duration_short(double seconds, char *buf, size_t sz) {
    int s = (int)seconds;
    if (s < 60) {
        snprintf(buf, sz, "%ds", s);
    } else {
        int m = s / 60; s %= 60;
        if (m < 60) {
            snprintf(buf, sz, "%dm%ds", m, s);
        } else {
            int h = m / 60; m %= 60;
            if (h < 24) {
                snprintf(buf, sz, "%dh%dm", h, m);
            } else {
                int d = h / 24; h %= 24;
                snprintf(buf, sz, "%dd%dh", d, h);
            }
        }
    }
    return buf;
}

/* ── Workspace name sanitization ──────────────────────────────────── */

char *sanitize_workspace_name(const char *display_name) {
    if (!display_name || !display_name[0]) return NULL;

    /* Allocate worst-case: same length + NUL */
    size_t len = strlen(display_name);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len && j < 64; i++) {
        unsigned char c = (unsigned char)display_name[i];
        if (c >= 'A' && c <= 'Z') {
            buf[j++] = (char)(c + 32);  /* lowercase */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.') {
            buf[j++] = (char)c;
        } else if (c == ' ' || c == '_' || c == '/' || c == '\\') {
            /* Replace separators with hyphen (collapse later) */
            buf[j++] = '-';
        } else if (c >= 0x80) {
            /* Skip multi-byte UTF-8 characters (emoji, accented chars) */
            while (i + 1 < len && ((unsigned char)display_name[i + 1] & 0xC0) == 0x80)
                i++;
        }
        /* All other ASCII chars (punctuation etc.) are dropped */
    }
    buf[j] = '\0';

    /* Collapse multiple hyphens */
    size_t w = 0;
    for (size_t r = 0; r < j; r++) {
        if (buf[r] == '-' && w > 0 && buf[w - 1] == '-')
            continue;
        buf[w++] = buf[r];
    }
    buf[w] = '\0';

    /* Strip leading and trailing hyphens/dots */
    size_t start = 0;
    while (buf[start] == '-' || buf[start] == '.') start++;
    size_t end = w;
    while (end > start && (buf[end - 1] == '-' || buf[end - 1] == '.')) end--;

    if (end <= start) {
        free(buf);
        return NULL;
    }

    /* Shift to start if needed */
    if (start > 0)
        memmove(buf, buf + start, end - start);
    buf[end - start] = '\0';

    return buf;
}

/* ── jsonl_iterate ───────────────────────────────────────────────────── */
int jsonl_iterate(const char *path,
                  int (*cb)(cJSON *entry, void *user_data),
                  void *user_data) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[65536];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Skip blank lines */
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
            continue;
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;
        int rc = cb(entry, user_data);
        cJSON_Delete(entry);
        count++;
        if (rc != 0) break;
    }
    fclose(fp);
    return count;
}

/* ── list_dir ────────────────────────────────────────────────────────── */
typedef struct {
    char **entries;
    int    count;
    int    cap;
} list_dir_ctx_t;

static int list_dir_cb(const char *dirpath, const char *filename,
                       const char *fullpath, void *user_data) {
    (void)dirpath; (void)fullpath;
    list_dir_ctx_t *ld = user_data;
    VEC_PUSH(ld->entries, ld->count, ld->cap, strdup(filename));
    return 0;
}

char **list_dir(const char *dirpath, const char *suffix, int *out_count) {
    list_dir_ctx_t ld = {0};
    for_each_dir_entry(dirpath, suffix, list_dir_cb, &ld);
    if (out_count) *out_count = ld.count;
    return ld.entries;
}

/* ── cjson_msg ───────────────────────────────────────────────────────── */
cJSON *cjson_msg(const char *role, const char *content) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", content);
    return msg;
}

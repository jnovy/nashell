#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <curl/curl.h>

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} str_t;

str_t  str_new(size_t initial_cap);
void   str_free(str_t *s);
void   str_clear(str_t *s);
void   str_append(str_t *s, const char *data, size_t len);
void   str_append_cstr(str_t *s, const char *cstr);
void   str_appendf(str_t *s, const char *fmt, ...);
char  *str_steal(str_t *s);   /* take ownership, reset str_t */
const char *str_cstr(const str_t *s);

/* Format seconds into human-readable duration: 5s, 1m30s, 2h05m30s, 1d02h05m30s
 * Writes into buf and returns buf for convenience. */
const char *fmt_duration(double seconds, char *buf, size_t sz);

/* Like fmt_duration but only the two most significant units.
 * Good for columnar display: "1d17h", "8h10m", "37m44s", "42s". */
const char *fmt_duration_short(double seconds, char *buf, size_t sz);

/* Read entire file into NUL-terminated buffer.
 * Returns NULL on failure. Caller must free.
 * If out_len is non-NULL, stores the number of bytes read. */
char *slurp_file(const char *path, size_t *out_len);

/* Read entire file in binary mode. Returns malloc'd buffer (not NUL-terminated).
 * If out_len is non-NULL, stores the number of bytes read.
 * Returns NULL on failure. Caller must free. */
void *slurp_file_binary(const char *path, size_t *out_len);

/* Write data to a file. Returns 0 on success, -1 on failure. */
int write_file(const char *path, const char *data, size_t len);

/* Read a JSON file and return parsed cJSON object.
 * Combines slurp_file + cJSON_Parse + free(buf).
 * Returns NULL on failure. Caller must cJSON_Delete() the result. */
struct cJSON;
struct cJSON *slurp_json(const char *path);

/* Create directory path recursively (like mkdir -p).
 * Returns 0 on success, -1 on failure. */
int mkdir_p(const char *path, mode_t mode);

/* Create session directory under the appropriate sessions base:
 *   workspace set: <nash_dir>/workspaces/<workspace>/sessions/<epoch.NNNNN>/
 *   workspace NULL: <nash_dir>/sessions/<epoch.NNNNN>/
 * Returns strdup'd path. Caller must free. */
char *create_session_dir(const char *nash_dir, const char *workspace);

/* Return the sessions base directory for a workspace (or global).
 * Creates the directory if it doesn't exist. Returns strdup'd path. */
char *sessions_base_dir(const char *nash_dir, const char *workspace);

/* Acquire an exclusive flock on <session_dir>/.lock.
 * Returns the lock fd (>= 0) on success, -1 on failure.
 * Non-blocking: if another process holds the lock, logs a warning
 * and returns -1 (caller decides whether to abort or continue). */
int session_lock_acquire(const char *session_dir);

/* Release and close a session lock fd. Safe to call with fd == -1. */
void session_lock_release(int fd);

/* ── HTTP helpers (libcurl) ─────────────────────────────────────────
 * Perform a simple HTTP GET.  Stores response body into *out (str_t).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_get(const char *url, long timeout_sec, str_t *out);

/* Like http_get but with optional custom headers (e.g. Authorization).
 * headers is a curl_slist (caller frees after call).  May be NULL.
 * Returns 0 on success, -1 on failure. */
int http_get_h(const char *url, struct curl_slist *headers,
               long timeout_sec, str_t *out);

/* Like http_get but with web-browsing defaults: follow redirects, user-agent,
 * protocol restrictions. Stores HTTP status code in *http_code if non-NULL.
 * Returns 0 on success, -1 on curl failure. */
int http_get_web(const char *url, long timeout_sec, str_t *out, long *http_code);

/* Perform an HTTP POST with a JSON body.
 * headers is a curl_slist (caller frees after call).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_post(const char *url, const char *body,
              struct curl_slist *headers, long timeout_sec, str_t *out);

/* Perform an HTTP PUT with a JSON body.
 * headers is a curl_slist (caller frees after call).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_put(const char *url, const char *body,
             struct curl_slist *headers, long timeout_sec, str_t *out);

/* Curl write callback that appends to a str_t.
 * Exported so callers can use str_t with custom curl setups. */
size_t str_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata);

/* Check if a string is NULL, empty, or whitespace-only (spaces, tabs, newlines) */
static inline int is_whitespace_only(const char *s) {
    if (!s) return 1;
    while (*s) {
        if (*s != ' ' && *s != '\n' && *s != '\r' && *s != '\t')
            return 0;
        s++;
    }
    return 1;
}

/* Skip leading whitespace (spaces, tabs, newlines).
 * Returns pointer to first non-whitespace char, or to the NUL terminator. */
static inline const char *skip_whitespace(const char *s) {
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
        s++;
    return s;
}

/* Trim trailing whitespace in-place by writing a NUL terminator.
 * Safe on empty strings. */
static inline void rtrim_whitespace(char *s) {
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\t'))
        end--;
    *end = '\0';
}

/* Count newline characters in a string. */
int count_lines(const char *s);

/* UTF-8 safe truncation: copy at most max_bytes from src into dst,
 * ensuring the result never cuts in the middle of a multi-byte UTF-8
 * sequence. dst must have room for max_bytes+1 (NUL terminator).
 * Returns the number of bytes written (excluding NUL). */
int utf8_truncate(char *dst, const char *src, int max_bytes);

/* Clamp a byte length so it does not split a multi-byte UTF-8 character.
 * Returns the largest value <= max_bytes such that s[0..return) ends on
 * a complete UTF-8 character boundary. */
size_t utf8_clamp(const char *s, size_t max_bytes);

/* Return the byte length of the UTF-8 character starting at *p.
 * Returns 1 for ASCII/invalid bytes, 2-4 for valid multi-byte sequences. */
int utf8_char_len(const char *p);

/* Advance p by one UTF-8 character. Returns the new pointer.
 * Equivalent to p += utf8_char_len(p). */
const char *utf8_next(const char *p);

/* Return the start of the UTF-8 character that contains p.
 * Walks backwards from p past any continuation bytes. */
const char *utf8_prev(const char *begin, const char *p);

/* Return the display width (columns) of a single UTF-8 character at *p.
 * ASCII printable = 1, control = 0, most Unicode = 1,
 * CJK fullwidth/wide = 2. */
int utf8_char_width(const char *p);

/* Return the display width (columns) of the first `nbytes` bytes of a
 * UTF-8 string. Stops at NUL or after nbytes bytes. */
int utf8_display_width(const char *s, int nbytes);

/* Return the number of bytes from `s` (up to `nbytes`) that fit in
 * `max_cols` display columns.  Never splits a multi-byte character. */
int utf8_bytes_for_width(const char *s, int nbytes, int max_cols);

/* ── Directory iteration helpers ──────────────────────────────────────
 * Callback-based directory traversal.  Iterates over files matching
 * *suffix* in *dirpath*, calling cb(dirpath, filename, fullpath, user_data)
 * for each match.  Stops when cb returns non-zero.
 *
 * Callback return values:
 *   0  = continue iterating
 *  !=0 = stop iterating
 *
 * Entries starting with '.' are skipped. */
typedef int (*dir_entry_cb)(const char *dirpath, const char *filename,
                            const char *fullpath, void *user_data);

void for_each_dir_entry(const char *dirpath, const char *suffix,
                        dir_entry_cb cb, void *user_data);

/* ── Workspace name sanitization ─────────────────────────────────────
 * Converts an arbitrary display name (topic name, room name) into a
 * valid workspace name: lowercase, hyphens instead of spaces/underscores,
 * only [a-z0-9.-] kept, collapsed hyphens, max 64 chars.
 * Returns a newly allocated string, or NULL if the name is empty
 * after sanitization.  Caller must free(). */
char *sanitize_workspace_name(const char *display_name);

/* ── Safe realloc wrapper ────────────────────────────────────────────
 * Attempts realloc.  On success, updates *ptr and returns 0.
 * On failure, *ptr is left unchanged (no leak) and returns -1.
 * Usage:  if (safe_realloc((void **)&buf, new_size)) { handle error } */
static inline int safe_realloc(void **ptr, size_t new_size) {
    void *tmp = realloc(*ptr, new_size);
    if (!tmp) return -1;
    *ptr = tmp;
    return 0;
}

/* ── Abort-on-failure allocators ─────────────────────────────────────
 * Like malloc/calloc/strdup but abort on failure instead of returning
 * NULL.  Suitable for allocations where OOM is unrecoverable. */
static inline void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p && size) { fprintf(stderr, "xmalloc(%zu): out of memory\n", size); abort(); }
    return p;
}
static inline void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p && n && size) { fprintf(stderr, "xcalloc(%zu,%zu): out of memory\n", n, size); abort(); }
    return p;
}
static inline char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "xstrdup: out of memory\n"); abort(); }
    return p;
}
/* Like xstrdup but NULL input yields strdup("") instead of crash. */
static inline char *xstrdupz(const char *s) {
    return xstrdup(s ? s : "");
}

/* ── String replacement helper ───────────────────────────────────────
 * Frees *dst, then sets *dst = strdup(src) (or NULL if src is NULL).
 * Eliminates the common free(x); x = strdup(y); two-liner. */
static inline void str_replace(char **dst, const char *src) {
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

/* ── Dynamic array push macro ────────────────────────────────────────
 * Appends an item to a dynamically-growing array with count/capacity
 * tracking.  Doubles capacity on overflow; uses safe_realloc.
 *
 * Usage:
 *   char **lines = NULL; int n = 0, cap = 0;
 *   VEC_PUSH(lines, n, cap, strdup("hello"));
 *
 * On realloc failure, the item is NOT added and control falls through
 * (caller should check n after if critical). */
#define VEC_PUSH(arr, count, cap, item) do {          \
    if ((count) >= (cap)) {                           \
        int _new_cap = (cap) ? (cap) * 2 : 8;        \
        if (safe_realloc((void **)&(arr),             \
            (size_t)_new_cap * sizeof(*(arr))) == 0)  \
            (cap) = _new_cap;                         \
        else break;                                   \
    }                                                 \
    (arr)[(count)++] = (item);                        \
} while (0)

/* ── Free a string array ─────────────────────────────────────────────
 * Frees each element then the array itself.  Safe with NULL arr. */
static inline void free_string_array(char **arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++)
        free(arr[i]);
    free(arr);
}

/* ── JSONL iteration helper ──────────────────────────────────────────
 * Opens a .jsonl file and calls cb(entry, user_data) for each parsed
 * JSON line.  Callback returns 0 to continue, non-zero to stop.
 * Returns number of entries processed, or -1 on file open failure. */
int jsonl_iterate(const char *path,
                  int (*cb)(struct cJSON *entry, void *user_data),
                  void *user_data);

/* ── Directory listing helper ────────────────────────────────────────
 * Returns a malloc'd array of filenames (strdup'd) in dirpath that
 * end with suffix (NULL suffix = all files).  Dot-files are skipped.
 * *out_count receives the number of entries.  Caller must
 * free_string_array() the result. */
char **list_dir(const char *dirpath, const char *suffix, int *out_count);

/* ── cJSON message helper ────────────────────────────────────────────
 * Create a {"role":"...","content":"..."} cJSON object.
 * Caller must cJSON_Delete() or add to an array (which takes ownership). */
struct cJSON *cjson_msg(const char *role, const char *content);

/* ── cJSON extraction helpers ────────────────────────────────────────
 * Convenience accessors for pulling typed values out of cJSON objects.
 * Eliminates the common GetObjectItem + type-check + valuestring pattern. */

/* Return string value for key, or NULL if missing/wrong type. */
const char *json_str(struct cJSON *obj, const char *key);

/* Return string value for key, or dflt if missing/wrong type. */
const char *json_str_or(struct cJSON *obj, const char *key, const char *dflt);

/* Return int value for key, or dflt if missing/wrong type. */
int json_int(struct cJSON *obj, const char *key, int dflt);

/* Return double value for key, or dflt if missing/wrong type. */
double json_num(struct cJSON *obj, const char *key, double dflt);

/* Return boolean value for key, or dflt if missing/wrong type. */
int json_bool(struct cJSON *obj, const char *key, int dflt);

/* ── dump_json (mirror of slurp_json) ────────────────────────────────
 * Pretty-print a cJSON object to a file.  Combines cJSON_Print +
 * write_file + free.  Returns 0 on success, -1 on failure. */
int dump_json(const char *path, struct cJSON *obj);

/* ── Timestamp formatting helpers ────────────────────────────────────
 * Format a time_t into buf.  Returns buf for convenience.
 * format_iso_date:     "2026-08-01"
 * format_iso_datetime: "2026-08-01 17:50:00" */
#include <time.h>
static inline char *format_iso_date(time_t t, char *buf, size_t sz) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%d", &tm);
    return buf;
}
static inline char *format_iso_datetime(time_t t, char *buf, size_t sz) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

/* ── Convenience micro-helpers ───────────────────────────────────────
 * Small inline utilities that eliminate common boilerplate patterns
 * (prefix/suffix checks, path building, file existence tests). */

#include <sys/stat.h>
#include <errno.h>

/* Check if string s starts with prefix pfx. */
static inline int starts_with(const char *s, const char *pfx) {
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

/* Check if string s ends with suffix sfx. */
static inline int ends_with(const char *s, const char *sfx) {
    size_t slen = strlen(s), xlen = strlen(sfx);
    return slen >= xlen && memcmp(s + slen - xlen, sfx, xlen) == 0;
}

/* Return 1 if path exists (any type), 0 otherwise. */
static inline int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Return 1 if path exists and is a directory, 0 otherwise. */
static inline int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Create directory if it does not already exist. Returns 0 on success
 * (including already-exists), -1 on failure. */
static inline int ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) return 0;
    return (errno == EEXIST) ? 0 : -1;
}

/* Build "a/b" into buf. Returns buf for convenience. */
static inline char *path_join(char *buf, size_t sz,
                              const char *a, const char *b) {
    snprintf(buf, sz, "%s/%s", a, b);
    return buf;
}

/* Return pointer to the basename portion of path (after last '/').
 * Returns path itself if no '/' is found. Never allocates. */
static inline const char *path_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

#endif

#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
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

#endif

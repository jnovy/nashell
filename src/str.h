#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdarg.h>
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

/* Read entire file into NUL-terminated buffer.
 * Returns NULL on failure. Caller must free.
 * If out_len is non-NULL, stores the number of bytes read. */
char *slurp_file(const char *path, size_t *out_len);

/* Write data to a file. Returns 0 on success, -1 on failure. */
int write_file(const char *path, const char *data, size_t len);

/* Create directory path recursively (like mkdir -p).
 * Returns 0 on success, -1 on failure. */
int mkdir_p(const char *path, mode_t mode);

/* ── HTTP helpers (libcurl) ─────────────────────────────────────────
 * Perform a simple HTTP GET.  Stores response body into *out (str_t).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_get(const char *url, long timeout_sec, str_t *out);

/* Perform an HTTP POST with a JSON body.
 * headers is a curl_slist (caller frees after call).
 * Caller must str_free(*out) on success.
 * Returns 0 on success, -1 on failure. */
int http_post(const char *url, const char *body,
              struct curl_slist *headers, long timeout_sec, str_t *out);

/* Curl write callback that appends to a str_t.
 * Exported so callers can use str_t with custom curl setups. */
size_t str_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata);

/* Count newline characters in a string. */
int count_lines(const char *s);

/* UTF-8 safe truncation: copy at most max_bytes from src into dst,
 * ensuring the result never cuts in the middle of a multi-byte UTF-8
 * sequence. dst must have room for max_bytes+1 (NUL terminator).
 * Returns the number of bytes written (excluding NUL). */
int utf8_truncate(char *dst, const char *src, int max_bytes);

/* Return the byte length of the UTF-8 character starting at *p.
 * Returns 1 for ASCII/invalid bytes, 2-4 for valid multi-byte sequences. */
int utf8_char_len(const char *p);

/* Advance p by one UTF-8 character. Returns the new pointer.
 * Equivalent to p += utf8_char_len(p). */
const char *utf8_next(const char *p);

/* Return the start of the UTF-8 character that contains p.
 * Walks backwards from p past any continuation bytes. */
const char *utf8_prev(const char *begin, const char *p);

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

#endif

#ifndef STR_H
#define STR_H

#include <stddef.h>
#include <stdarg.h>

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

#endif

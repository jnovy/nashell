#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

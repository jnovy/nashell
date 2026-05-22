#ifndef STORE_H
#define STORE_H

#include "str.h"

/* Content-addressed store: saves raw content to store/<sha256>.txt */

typedef struct {
    char *dir;   /* e.g. "/tmp/nash_session/store" */
} store_t;

store_t *store_new(const char *session_dir);
void     store_free(store_t *s);

/* Save content, return ref path (caller must free) */
char *store_save(store_t *s, const char *content, const char *ext);

/* Compute SHA256 hex string (caller must free, 64 hex chars + NUL) */
char *sha256_hex(const char *data, size_t len);

#endif

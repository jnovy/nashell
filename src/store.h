#ifndef STORE_H
#define STORE_H

#include "str.h"
#include <stddef.h>

/* Content-addressed store: shared across all sessions.
 * Files stored as .store/<sha256>.<ext> at the project root.
 * Dedup: same content = same hash = one file on disk. */

typedef struct {
    char *dir;   /* e.g. "/path/to/project/.store" */
} store_t;

/* Create shared store at project root (creates .store/ directory) */
store_t *store_new(const char *project_root);
void     store_free(store_t *s);

/* Save content to shared store. Returns the SHA256 hash (caller must free).
 * The actual file is at .store/<hash>.<ext>.
 * If content already exists (same hash), no write — just returns the hash. */
char *store_save(store_t *s, const char *content, const char *ext);

/* Resolve a hash to the full filesystem path (caller must free).
 * Returns .store/<hash>.<ext> */
char *store_resolve(store_t *s, const char *hash, const char *ext);

/* Compute SHA256 hex string (caller must free, 64 hex chars + NUL) */
char *sha256_hex(const char *data, size_t len);

#endif

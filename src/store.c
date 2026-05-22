#define _GNU_SOURCE
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>

store_t *store_new(const char *session_dir) {
    store_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    char path[4096];
    snprintf(path, sizeof(path), "%s/store", session_dir);
    mkdir(path, 0755);  /* ignore EEXIST */
    s->dir = strdup(path);
    return s;
}

void store_free(store_t *s) {
    if (!s) return;
    free(s->dir);
    free(s);
}

char *sha256_hex(const char *data, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, len, hash);
    char *hex = malloc(65);
    if (!hex) return NULL;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = '\0';
    return hex;
}

char *store_save(store_t *s, const char *content, const char *ext) {
    if (!s || !content) return NULL;
    size_t clen = strlen(content);
    char *hex = sha256_hex(content, clen);
    if (!hex) return NULL;

    const char *e = ext ? ext : "txt";

    /* Build full path */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.%s", s->dir, hex, e);

    /* Content-addressed dedup: atomic create with O_CREAT|O_EXCL
     * Avoids TOCTOU race between stat() and fopen() */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        /* New file — write content */
        write(fd, content, clen);
        close(fd);
    }
    /* fd < 0 && errno == EEXIST means file already exists (dedup hit) — OK */

    /* Return relative ref: store/<hash>.<ext> */
    char *ref = NULL;
    if (asprintf(&ref, "store/%s.%s", hex, e) < 0) ref = NULL;
    free(hex);
    return ref;
}

#define _GNU_SOURCE
#include "store.h"
#include "nash_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>

store_t *store_new(const char *project_root) {
    store_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/store", project_root);
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

char *store_save(store_t *s, const char *content) {
    if (!s || !content) return NULL;
    size_t clen = strlen(content);
    char *hex = sha256_hex(content, clen);
    if (!hex) return NULL;

    /* Build full path: .store/<hash> (no extension) */
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s->dir, hex);

    /* Content-addressed dedup: atomic create with O_CREAT|O_EXCL */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        ssize_t written = write(fd, content, clen);
        if (written < 0 || (size_t)written != clen) {
            close(fd);
            unlink(path);   /* remove partial file */
            free(hex);
            return NULL;
        }
        close(fd);
    }

    return hex;  /* caller gets the hash */
}

char *store_resolve(store_t *s, const char *hash) {
    if (!s || !hash) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/%s", s->dir, hash) < 0) return NULL;
    return path;
}

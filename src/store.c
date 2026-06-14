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
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
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

/* ── Garbage Collection ──────────────────────────────── */

#include <dirent.h>

/* Simple hash set for referenced store hashes. */
typedef struct { char **keys; int count, cap; } hashset_t;

static void hashset_add(hashset_t *hs, const char *key) {
    for (int i = 0; i < hs->count; i++)
        if (strcmp(hs->keys[i], key) == 0) return;
    if (hs->count >= hs->cap) {
        hs->cap = hs->cap ? hs->cap * 2 : 256;
        hs->keys = realloc(hs->keys, sizeof(char *) * (size_t)hs->cap);
    }
    hs->keys[hs->count++] = strdup(key);
}

static int hashset_contains(hashset_t *hs, const char *key) {
    for (int i = 0; i < hs->count; i++)
        if (strcmp(hs->keys[i], key) == 0) return 1;
    return 0;
}

static void hashset_free(hashset_t *hs) {
    for (int i = 0; i < hs->count; i++) free(hs->keys[i]);
    free(hs->keys);
}

/* Scan a session directory for symlinks → store. Extract the hash. */
static void scan_session_refs(const char *sess_dir, const char *store_dir,
                              hashset_t *refs) {
    DIR *d = opendir(sess_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[NASH_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", sess_dir, de->d_name);
        char target[NASH_PATH_MAX];
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len <= 0) continue;
        target[len] = '\0';
        /* Check if symlink points into the store */
        if (strstr(target, "store/") || strstr(target, store_dir)) {
            const char *slash = strrchr(target, '/');
            if (slash) hashset_add(refs, slash + 1);
        }
    }
    closedir(d);
}

int store_gc(store_t *s, const char *nash_dir) {
    if (!s || !nash_dir) return -1;

    /* Phase 1: Collect all referenced hashes from session symlinks */
    hashset_t refs = {0};
    char sessions_path[NASH_PATH_MAX];
    snprintf(sessions_path, sizeof(sessions_path), "%s/sessions", nash_dir);
    DIR *sd = opendir(sessions_path);
    if (sd) {
        struct dirent *de;
        while ((de = readdir(sd))) {
            if (de->d_name[0] == '.') continue;
            char sess_dir[NASH_PATH_MAX];
            snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                     sessions_path, de->d_name);
            scan_session_refs(sess_dir, s->dir, &refs);
        }
        closedir(sd);
    }

    /* Phase 2: Delete unreferenced store entries */
    int removed = 0;
    DIR *store_d = opendir(s->dir);
    if (!store_d) { hashset_free(&refs); return -1; }
    struct dirent *de;
    while ((de = readdir(store_d))) {
        if (de->d_name[0] == '.') continue;
        if (!hashset_contains(&refs, de->d_name)) {
            char path[NASH_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", s->dir, de->d_name);
            if (unlink(path) == 0) removed++;
        }
    }
    closedir(store_d);
    hashset_free(&refs);
    return removed;
}

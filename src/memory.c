#define _GNU_SOURCE
#include "memory.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <strings.h>  /* strcasestr */

/* ── helpers ─────────────────────────────────────────── */

/* Sanitize key for filename: replace : with _ */
static void key_to_filename(const char *key, char *out, size_t out_sz) {
    size_t i = 0;
    for (; key[i] && i < out_sz - 6; i++)
        out[i] = (key[i] == ':' || key[i] == '/') ? '_' : key[i];
    out[i] = '\0';
    strcat(out, ".json");
}

static double epoch_now(void) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return (double)tp.tv_sec + (double)tp.tv_nsec / 1e9;
}

/* ── create/free ─────────────────────────────────────── */

memory_t *memory_new(const char *project_root) {
    memory_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    char path[4096];
    snprintf(path, sizeof(path), "%s/memory", project_root);
    mkdir(path, 0755);
    m->dir = strdup(path);
    return m;
}

void memory_free(memory_t *m) {
    if (!m) return;
    free(m->dir);
    free(m);
}

/* ── store ───────────────────────────────────────────── */

int memory_store(memory_t *m, const char *key, const char *value,
                 const char **tags, int n_tags, int pinned) {
    if (!m || !key || !value) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "key", key);
    cJSON_AddStringToObject(entry, "value", value);

    cJSON *tags_arr = cJSON_CreateArray();
    for (int i = 0; i < n_tags; i++)
        cJSON_AddItemToArray(tags_arr, cJSON_CreateString(tags[i]));
    cJSON_AddItemToObject(entry, "tags", tags_arr);

    cJSON_AddBoolToObject(entry, "pinned", pinned);

    /* Check if entry already exists (update access_count) */
    FILE *existing = fopen(path, "r");
    int access_count = 0;
    double created_at = epoch_now();
    if (existing) {
        fseek(existing, 0, SEEK_END);
        long sz = ftell(existing);
        fseek(existing, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (buf) {
            fread(buf, 1, (size_t)sz, existing);
            buf[sz] = '\0';
            cJSON *old = cJSON_Parse(buf);
            if (old) {
                cJSON *ac = cJSON_GetObjectItem(old, "access_count");
                if (ac) access_count = (int)cJSON_GetNumberValue(ac);
                cJSON *ca = cJSON_GetObjectItem(old, "created_at");
                if (ca) created_at = cJSON_GetNumberValue(ca);
                cJSON_Delete(old);
            }
            free(buf);
        }
        fclose(existing);
    }

    char ts[32];
    snprintf(ts, sizeof(ts), "%.5f", created_at);
    cJSON_AddStringToObject(entry, "created_at", ts);
    snprintf(ts, sizeof(ts), "%.5f", epoch_now());
    cJSON_AddStringToObject(entry, "last_accessed", ts);
    cJSON_AddNumberToObject(entry, "access_count", access_count);

    char *json = cJSON_Print(entry);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);
    return 0;
}

/* ── recall (search) ─────────────────────────────────── */

/* Score a memory entry against a query (higher = more relevant) */
static int score_entry(const char *key, const char *value,
                       cJSON *tags, const char *query) {
    int score = 0;
    /* Key match (most valuable) */
    if (strcasestr(key, query)) score += 3;
    /* Tag match */
    if (tags) {
        int n = cJSON_GetArraySize(tags);
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(tags, i);
            if (t && t->valuestring && strcasestr(t->valuestring, query))
                score += 2;
        }
    }
    /* Value match */
    if (strcasestr(value, query)) score += 1;
    return score;
}

memory_results_t memory_recall(memory_t *m, const char *query, int max_results) {
    memory_results_t results = {0};
    if (!m || !query) return results;

    DIR *dir = opendir(m->dir);
    if (!dir) return results;

    /* Temporary storage for scored results */
    typedef struct { char path[4096]; int score; } scored_t;
    scored_t *scored = calloc(1024, sizeof(scored_t));
    int n_scored = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n_scored < 1024) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        const char *key = "";
        const char *value = "";
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");
        if (k && k->valuestring) key = k->valuestring;
        if (v && v->valuestring) value = v->valuestring;

        int s = score_entry(key, value, tags, query);
        if (s > 0) {
            snprintf(scored[n_scored].path, sizeof(scored[n_scored].path), "%s", path);
            scored[n_scored].score = s;
            n_scored++;
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    /* Sort by score descending (simple bubble sort, n is small) */
    for (int i = 0; i < n_scored - 1; i++)
        for (int j = i + 1; j < n_scored; j++)
            if (scored[j].score > scored[i].score) {
                scored_t tmp = scored[i];
                scored[i] = scored[j];
                scored[j] = tmp;
            }

    /* Load top results */
    int n = n_scored < max_results ? n_scored : max_results;
    results.entries = calloc((size_t)n, sizeof(memory_entry_t));
    results.count = 0;

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(scored[i].path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        memory_entry_t *e = &results.entries[results.count];
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *v = cJSON_GetObjectItem(entry, "value");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");
        cJSON *p = cJSON_GetObjectItem(entry, "pinned");

        e->key = k && k->valuestring ? strdup(k->valuestring) : strdup("");
        e->value = v && v->valuestring ? strdup(v->valuestring) : strdup("");
        e->pinned = p ? cJSON_IsTrue(p) : 0;

        /* Copy tags */
        if (tags) {
            e->n_tags = cJSON_GetArraySize(tags);
            e->tags = calloc((size_t)e->n_tags, sizeof(char *));
            for (int t = 0; t < e->n_tags; t++) {
                cJSON *tag = cJSON_GetArrayItem(tags, t);
                e->tags[t] = tag && tag->valuestring ? strdup(tag->valuestring) : strdup("");
            }
        }

        /* Update access_count and last_accessed */
        cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
        if (ac) {
            cJSON_SetNumberValue(ac, cJSON_GetNumberValue(ac) + 1);
        }
        char ts[32];
        snprintf(ts, sizeof(ts), "%.5f", epoch_now());
        cJSON_ReplaceItemInObject(entry, "last_accessed", cJSON_CreateString(ts));

        /* Write back updated entry */
        char *json = cJSON_Print(entry);
        FILE *wf = fopen(scored[i].path, "w");
        if (wf) { fputs(json, wf); fclose(wf); }
        free(json);

        cJSON_Delete(entry);
        results.count++;
    }

    free(scored);
    return results;
}

/* ── build_index ─────────────────────────────────────── */

char *memory_build_index(memory_t *m) {
    if (!m) return NULL;

    DIR *dir = opendir(m->dir);
    if (!dir) return NULL;

    str_t out = str_new(1024);
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        cJSON *k = cJSON_GetObjectItem(entry, "key");
        cJSON *tags = cJSON_GetObjectItem(entry, "tags");

        if (k && k->valuestring) {
            str_appendf(&out, "  %s", k->valuestring);
            if (tags && cJSON_GetArraySize(tags) > 0) {
                str_append_cstr(&out, " [");
                int n = cJSON_GetArraySize(tags);
                for (int i = 0; i < n; i++) {
                    cJSON *t = cJSON_GetArrayItem(tags, i);
                    if (i > 0) str_append_cstr(&out, ", ");
                    if (t && t->valuestring) str_append_cstr(&out, t->valuestring);
                }
                str_append_cstr(&out, "]");
            }
            str_append_cstr(&out, "\n");
            count++;
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    if (count == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

/* ── load_pinned ─────────────────────────────────────── */

char *memory_load_pinned(memory_t *m) {
    if (!m) return NULL;

    DIR *dir = opendir(m->dir);
    if (!dir) return NULL;

    str_t out = str_new(1024);
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".json") != 0) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, (size_t)sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *entry = cJSON_Parse(buf);
        free(buf);
        if (!entry) continue;

        cJSON *p = cJSON_GetObjectItem(entry, "pinned");
        if (p && cJSON_IsTrue(p)) {
            cJSON *k = cJSON_GetObjectItem(entry, "key");
            cJSON *v = cJSON_GetObjectItem(entry, "value");
            if (k && k->valuestring && v && v->valuestring) {
                if (count > 0) str_append_cstr(&out, "\n");
                str_appendf(&out, "[PINNED: %s]\n%s", k->valuestring, v->valuestring);
                count++;
            }
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    if (count == 0) {
        str_free(&out);
        return NULL;
    }
    return str_steal(&out);
}

/* ── free results ────────────────────────────────────── */

void memory_results_free(memory_results_t *r) {
    if (!r || !r->entries) return;
    for (int i = 0; i < r->count; i++) {
        free(r->entries[i].key);
        free(r->entries[i].value);
        for (int t = 0; t < r->entries[i].n_tags; t++)
            free(r->entries[i].tags[t]);
        free(r->entries[i].tags);
    }
    free(r->entries);
    r->entries = NULL;
    r->count = 0;
}

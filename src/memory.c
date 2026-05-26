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
#include <unistd.h>   /* unlink */
#include <math.h>     /* exp, log */

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

    /* Check if entry already exists (preserve counters) */
    FILE *existing = fopen(path, "r");
    int access_count = 1;  /* store itself counts as one access */
    int recall_hits = 0;
    int recall_misses = 0;
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
                if (ac) access_count = (int)cJSON_GetNumberValue(ac) + 1;  /* increment */
                cJSON *ca = cJSON_GetObjectItem(old, "created_at");
                if (ca) created_at = cJSON_GetNumberValue(ca);
                cJSON *rh = cJSON_GetObjectItem(old, "recall_hits");
                if (rh) recall_hits = (int)cJSON_GetNumberValue(rh);
                cJSON *rm = cJSON_GetObjectItem(old, "recall_misses");
                if (rm) recall_misses = (int)cJSON_GetNumberValue(rm);
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
    cJSON_AddNumberToObject(entry, "recall_hits", recall_hits);
    cJSON_AddNumberToObject(entry, "recall_misses", recall_misses);

    char *json = cJSON_Print(entry);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);

    /* Auto-update MEMORY.md index file */
    memory_write_index_file(m);

    return 0;
}


/* ── pin/unpin ───────────────────────────────────────────── */

/* Helper: load entry JSON from file, modify pinned flag, write back */
static int memory_set_pinned(memory_t *m, const char *key, int pinned) {
    if (!m || !key) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* entry not found */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *entry = cJSON_Parse(buf);
    free(buf);
    if (!entry) return -1;

    /* Replace or add the pinned field */
    cJSON *p = cJSON_GetObjectItem(entry, "pinned");
    if (p) {
        cJSON_ReplaceItemInObject(entry, "pinned",
                                  pinned ? cJSON_CreateTrue() : cJSON_CreateFalse());
    } else {
        cJSON_AddBoolToObject(entry, "pinned", pinned);
    }

    /* Write back */
    char *json = cJSON_Print(entry);
    f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);

    /* Update MEMORY.md index */
    memory_write_index_file(m);

    return 0;
}

int memory_pin(memory_t *m, const char *key) {
    return memory_set_pinned(m, key, 1);
}

int memory_unpin(memory_t *m, const char *key) {
    return memory_set_pinned(m, key, 0);
}

/* ── recall (search) ─────────────────────────────────── */

/* Composite scoring: relevance × recency × importance (research-backed) */
static double score_entry_composite(const char *key, const char *value,
                                     cJSON *tags, const char *query,
                                     double last_accessed, int access_count) {
    /* Relevance: substring match on key, tags, value */
    double relevance = 0;
    if (strcasestr(key, query)) relevance += 3.0;
    if (tags) {
        int n = cJSON_GetArraySize(tags);
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(tags, i);
            if (t && t->valuestring && strcasestr(t->valuestring, query))
                relevance += 2.0;
        }
    }
    if (strcasestr(value, query)) relevance += 1.0;
    if (relevance == 0) return 0;  /* no match at all */

    /* Recency: exponential decay — recent memories score higher */
    double age_days = (epoch_now() - last_accessed) / 86400.0;
    if (age_days < 0) age_days = 0;
    double recency = exp(-age_days / 30.0);  /* half-life ~30 days */

    /* Importance: logarithmic access frequency */
    double importance = 1.0 + log(1.0 + (double)access_count);

    /* Composite: weighted combination */
    return relevance * 0.6 + recency * 0.2 + importance * 0.2;
}

memory_results_t memory_recall(memory_t *m, const char *query, int max_results) {
    memory_results_t results = {0};
    if (!m || !query) return results;

    DIR *dir = opendir(m->dir);
    if (!dir) return results;

    /* Temporary storage for scored results */
    typedef struct { char path[4096]; double score; } scored_t;
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

        /* Get recency/importance data for composite scoring */
        double last_acc = 0;
        int acc_count = 0;
        cJSON *la = cJSON_GetObjectItem(entry, "last_accessed");
        cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
        if (la && la->valuestring) last_acc = atof(la->valuestring);
        if (ac) acc_count = (int)cJSON_GetNumberValue(ac);

        double s = score_entry_composite(key, value, tags, query, last_acc, acc_count);
        if (s > 0.01) {
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

        /* Copy validation counters */
        cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
        cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
        e->recall_hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
        e->recall_misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;

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

char *memory_build_index(memory_t *m, int max_entries) {
    if (!m) return NULL;

    DIR *dir = opendir(m->dir);
    if (!dir) return NULL;

    /* Count entries by type for progressive disclosure */
    int n_lessons = 0, n_strategies = 0, n_facts = 0, n_tasks = 0, n_skills = 0, n_other = 0;
    int total = 0;

    str_t out = str_new(2048);

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
            /* Count by type */
            if (strncmp(k->valuestring, "lesson:", 7) == 0) n_lessons++;
            else if (strncmp(k->valuestring, "strategy:", 9) == 0) n_strategies++;
            else if (strncmp(k->valuestring, "fact:", 5) == 0) n_facts++;
            else if (strncmp(k->valuestring, "task:", 5) == 0) n_tasks++;
            else if (strncmp(k->valuestring, "skill:", 6) == 0) n_skills++;
            else n_other++;

            /* Progressive disclosure: only show first N entries inline */
            if (total < 50) {
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
            }
            total++;
        }
        cJSON_Delete(entry);
    }
    closedir(dir);

    if (total == 0) {
        str_free(&out);
        return NULL;
    }

    /* Build header with topic summary */
    str_t result = str_new(2048);
    str_appendf(&result, "Memory: %d entries", total);
    if (n_lessons > 0) str_appendf(&result, ", %d lessons", n_lessons);
    if (n_strategies > 0) str_appendf(&result, ", %d strategies", n_strategies);
    if (n_skills > 0) str_appendf(&result, ", %d skills", n_skills);
    if (n_facts > 0) str_appendf(&result, ", %d facts", n_facts);
    if (n_tasks > 0) str_appendf(&result, ", %d tasks", n_tasks);
    if (n_other > 0) str_appendf(&result, ", %d other", n_other);
    str_append_cstr(&result, "\n");

    if (total > max_entries && max_entries > 0) {
        str_appendf(&result, "  (showing first %d of %d — use memory_recall to search)\n", max_entries, total);
    }
    str_append_cstr(&result, str_cstr(&out));
    str_free(&out);

    return str_steal(&result);
}

/* — write MEMORY.md index file ——————————————————————— */

int memory_write_index_file(memory_t *m) {
    if (!m) return -1;

    char path[4096];
    snprintf(path, sizeof(path), "%s/../MEMORY.md", m->dir);

    char *index = memory_build_index(m, 0);  /* 0 = show all for MEMORY.md */

    FILE *f = fopen(path, "w");
    if (!f) { free(index); return -1; }

    fprintf(f, "# Nash Memory Index\n\n");
    fprintf(f, "Auto-generated — do not edit manually.\n");
    fprintf(f, "Use `memory_store` and `memory_recall` to manage.\n\n");
    if (index) {
        fprintf(f, "%s", index);
        free(index);
    } else {
        fprintf(f, "(empty)\n");
    }
    fclose(f);
    return 0;
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

/* ── prune (forgetting/decay) ─────────────────────────────── */

int memory_prune(memory_t *m, int max_age_days, int min_access_count) {
    if (!m) return 0;

    DIR *dir = opendir(m->dir);
    if (!dir) return 0;

    double now = epoch_now();
    double max_age_sec = (double)max_age_days * 86400.0;
    int pruned = 0;

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

        /* Never prune pinned memories */
        cJSON *pin = cJSON_GetObjectItem(entry, "pinned");
        if (pin && cJSON_IsTrue(pin)) { cJSON_Delete(entry); continue; }

        /* Lessons/strategies: validation-score-based pruning.
         * Only prune if stale AND low validation score AND enough evidence. */
        cJSON *k = cJSON_GetObjectItem(entry, "key");
        if (k && k->valuestring) {
            if (strncmp(k->valuestring, "strategy:", 9) == 0 ||
                strncmp(k->valuestring, "lesson:", 7) == 0 ||
                strncmp(k->valuestring, "skill:", 6) == 0) {
                cJSON *la = cJSON_GetObjectItem(entry, "last_accessed");
                double last_acc = la && la->valuestring ? atof(la->valuestring) : now;
                double age = now - last_acc;
                cJSON *rh = cJSON_GetObjectItem(entry, "recall_hits");
                cJSON *rm = cJSON_GetObjectItem(entry, "recall_misses");
                int hits = rh ? (int)cJSON_GetNumberValue(rh) : 0;
                int misses = rm ? (int)cJSON_GetNumberValue(rm) : 0;
                int evidence = hits + misses;
                double vscore = (hits + 1.0) / (hits + misses + 2.0);
                /* Prune only if: stale + low score + enough evidence */
                if (age > max_age_sec && vscore < 0.35 && evidence >= 3) {
                    unlink(path);
                    pruned++;
                }
                cJSON_Delete(entry);
                continue;
            }
        }

        /* Generic entries: check age and access count */
        cJSON *la = cJSON_GetObjectItem(entry, "last_accessed");
        cJSON *ac = cJSON_GetObjectItem(entry, "access_count");
        double last_acc = la && la->valuestring ? atof(la->valuestring) : now;
        int acc_count = ac ? (int)cJSON_GetNumberValue(ac) : 0;

        double age = now - last_acc;
        if (age > max_age_sec && acc_count < min_access_count) {
            unlink(path);
            pruned++;
        }

        cJSON_Delete(entry);
    }
    closedir(dir);
    return pruned;
}

/* ── validation scoring ─────────────────────────────────────── */

/* Internal: increment a numeric field in a memory entry's JSON file */
static int memory_increment_field(memory_t *m, const char *key,
                                   const char *field) {
    if (!m || !key || !field) return -1;

    char fname[512];
    key_to_filename(key, fname, sizeof(fname));

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", m->dir, fname);

    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* entry not found */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *entry = cJSON_Parse(buf);
    free(buf);
    if (!entry) return -1;

    /* Increment the field (create if missing) */
    cJSON *fld = cJSON_GetObjectItem(entry, field);
    if (fld) {
        cJSON_SetNumberValue(fld, cJSON_GetNumberValue(fld) + 1);
    } else {
        cJSON_AddNumberToObject(entry, field, 1);
    }

    /* Write back */
    char *json = cJSON_Print(entry);
    f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
    cJSON_Delete(entry);
    return 0;
}

int memory_increment_hits(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_hits");
}

int memory_increment_misses(memory_t *m, const char *key) {
    return memory_increment_field(m, key, "recall_misses");
}

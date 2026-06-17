/* session_index.c — In-memory index of past session summaries with embeddings.
 * Part of Nash unified memory architecture v4 (L3 tier). */

#include "session_index.h"
#include "journal.h"
#include "str.h"
#include "nash_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

/* ── Internal helpers ──────────────────────────────────── */

static int si_grow(session_index_t *idx) {
    if (idx->count < idx->cap) return 0;
    int new_cap = idx->cap ? idx->cap * 2 : 64;
    session_index_entry_t *new_e = realloc(idx->entries,
        (size_t)new_cap * sizeof(session_index_entry_t));
    if (!new_e) return -1;
    memset(new_e + idx->cap, 0,
        (size_t)(new_cap - idx->cap) * sizeof(session_index_entry_t));
    idx->entries = new_e;
    idx->cap = new_cap;
    return 0;
}

/* Extract timestamp from session directory name (e.g. "1750000123.45678") */
static double parse_session_timestamp(const char *dirname) {
    /* Session dirs are named with unix timestamps */
    const char *base = strrchr(dirname, '/');
    if (base) base++;
    else base = dirname;
    return atof(base);
}

/* Compare entries by timestamp (newest first) for sorting */
static int entry_cmp_ts(const void *a, const void *b) {
    const session_index_entry_t *ea = a;
    const session_index_entry_t *eb = b;
    if (ea->timestamp > eb->timestamp) return -1;
    if (ea->timestamp < eb->timestamp) return 1;
    return 0;
}

/* ── Lifecycle ──────────────────────────────────────── */

session_index_t *session_index_load(const char *sessions_dir) {
    if (!sessions_dir) return NULL;

    DIR *d = opendir(sessions_dir);
    if (!d) return NULL;

    session_index_t *idx = calloc(1, sizeof(session_index_t));
    if (!idx) { closedir(d); return NULL; }
    pthread_mutex_init(&idx->mtx, NULL);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Check for summary.emb file */
        char emb_path[PATH_MAX];
        snprintf(emb_path, sizeof(emb_path), "%s/%s/summary.emb",
                 sessions_dir, ent->d_name);

        struct stat st;
        if (stat(emb_path, &st) != 0) continue;  /* no embedding = skip */

        /* Load embedding */
        embed_vec_t emb = embed_vec_load(emb_path);
        if (!emb.data) continue;

        /* Load manifest text */
        char txt_path[PATH_MAX];
        snprintf(txt_path, sizeof(txt_path), "%s/%s/summary.txt",
                 sessions_dir, ent->d_name);
        char *manifest = slurp_file(txt_path, NULL);

        /* Build full session dir path */
        char sess_dir[PATH_MAX];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        /* Add to index */
        if (si_grow(idx) < 0) {
            embed_vec_free(&emb);
            free(manifest);
            continue;
        }

        session_index_entry_t *e = &idx->entries[idx->count];
        e->session_dir = strdup(sess_dir);
        e->manifest = manifest;  /* takes ownership */
        e->timestamp = parse_session_timestamp(ent->d_name);
        e->emb = emb;            /* takes ownership */
        e->has_emb = 1;
        idx->count++;
    }
    closedir(d);

    /* Sort by timestamp (newest first) */
    if (idx->count > 1) {
        qsort(idx->entries, (size_t)idx->count,
              sizeof(session_index_entry_t), entry_cmp_ts);
    }

    nash_log("session_index: loaded %d sessions from %s",
             idx->count, sessions_dir);
    return idx;
}

int session_index_add(session_index_t *idx, const char *session_dir,
                      const char *manifest, const embed_vec_t *emb) {
    if (!idx || !session_dir) return -1;

    pthread_mutex_lock(&idx->mtx);
    if (si_grow(idx) < 0) {
        pthread_mutex_unlock(&idx->mtx);
        return -1;
    }

    session_index_entry_t *e = &idx->entries[idx->count];
    e->session_dir = strdup(session_dir);
    e->manifest = manifest ? strdup(manifest) : NULL;
    e->timestamp = parse_session_timestamp(session_dir);
    if (emb && emb->data) {
        e->emb.dim = emb->dim;
        e->emb.data = malloc((size_t)emb->dim * sizeof(float));
        if (e->emb.data) {
            memcpy(e->emb.data, emb->data, (size_t)emb->dim * sizeof(float));
            e->has_emb = 1;
        }
    }
    idx->count++;
    pthread_mutex_unlock(&idx->mtx);
    return 0;
}

void session_index_free(session_index_t *idx) {
    if (!idx) return;
    for (int i = 0; i < idx->count; i++) {
        free(idx->entries[i].session_dir);
        free(idx->entries[i].manifest);
        embed_vec_free(&idx->entries[i].emb);
    }
    free(idx->entries);
    pthread_mutex_destroy(&idx->mtx);
    free(idx);
}

/* ── Search ─────────────────────────────────────────── */

session_index_results_t session_index_search(
    session_index_t *idx,
    const embed_vec_t *query_emb,
    int max_results)
{
    session_index_results_t out = {0};
    if (!idx || !query_emb || !query_emb->data || max_results <= 0)
        return out;

    pthread_mutex_lock(&idx->mtx);

    /* Score all sessions */
    typedef struct { int idx; double score; } scored_t;
    scored_t *scored = calloc((size_t)idx->count, sizeof(scored_t));
    if (!scored) { pthread_mutex_unlock(&idx->mtx); return out; }

    double now_ts = (double)time(NULL);
    int n_scored = 0;

    for (int i = 0; i < idx->count; i++) {
        if (!idx->entries[i].has_emb) continue;

        float semantic = embed_cosine_sim(query_emb, &idx->entries[i].emb);
        if (semantic < 0.15f) continue;  /* skip very low matches */

        /* Recency boost: log curve, newer sessions mildly preferred.
         * At 0 days: recency = 1.0
         * At 30 days: recency ≈ 0.59
         * At 365 days: recency ≈ 0.28 */
        double age_days = (now_ts - idx->entries[i].timestamp) / 86400.0;
        if (age_days < 0) age_days = 0;
        double recency = 1.0 / (1.0 + log1p(age_days / 30.0));

        scored[n_scored].idx = i;
        scored[n_scored].score = (double)semantic * recency;
        n_scored++;
    }

    /* Sort by score descending */
    for (int i = 0; i < n_scored - 1; i++) {
        for (int j = i + 1; j < n_scored; j++) {
            if (scored[j].score > scored[i].score) {
                scored_t tmp = scored[i];
                scored[i] = scored[j];
                scored[j] = tmp;
            }
        }
    }

    /* Take top-K */
    int n_results = n_scored < max_results ? n_scored : max_results;
    if (n_results > 0) {
        out.results = calloc((size_t)n_results, sizeof(session_index_result_t));
        if (out.results) {
            for (int i = 0; i < n_results; i++) {
                session_index_entry_t *e = &idx->entries[scored[i].idx];
                out.results[i].session_dir = strdup(e->session_dir);
                out.results[i].manifest = e->manifest ? strdup(e->manifest) : NULL;
                out.results[i].timestamp = e->timestamp;
                out.results[i].score = scored[i].score;
            }
            out.count = n_results;
        }
    }

    free(scored);
    pthread_mutex_unlock(&idx->mtx);
    return out;
}

void session_index_results_free(session_index_results_t *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->results[i].session_dir);
        free(r->results[i].manifest);
    }
    free(r->results);
    r->results = NULL;
    r->count = 0;
}

/* ── Backfill ───────────────────────────────────────── */

int session_index_backfill(const char *sessions_dir, embed_ctx_t *embed) {
    if (!sessions_dir || !embed) return 0;

    DIR *d = opendir(sessions_dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Skip sessions that already have summary.txt */
        char txt_path[PATH_MAX];
        snprintf(txt_path, sizeof(txt_path), "%s/%s/summary.txt",
                 sessions_dir, ent->d_name);

        struct stat st;
        if (stat(txt_path, &st) == 0) continue;  /* already has summary */

        /* Check if journal.jsonl exists */
        char journal_path[PATH_MAX];
        snprintf(journal_path, sizeof(journal_path), "%s/%s/journal.jsonl",
                 sessions_dir, ent->d_name);
        if (stat(journal_path, &st) != 0) continue;  /* no journal */

        /* Generate manifest via journal_manifest() */
        char sess_dir[PATH_MAX];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        journal_t *j = journal_new(sess_dir);
        if (!j) continue;

        char *manifest = journal_manifest(j, 0);
        journal_free(j);

        if (!manifest || strlen(manifest) < 30) {
            free(manifest);
            continue;
        }

        /* Write summary.txt */
        FILE *f = fopen(txt_path, "w");
        if (f) {
            fputs(manifest, f);
            fclose(f);
        }

        /* Generate and write summary.emb */
        embed_vec_t emb_vec = embed_text(embed, manifest);
        if (emb_vec.data) {
            char emb_path[PATH_MAX];
            snprintf(emb_path, sizeof(emb_path), "%s/%s/summary.emb",
                     sessions_dir, ent->d_name);
            embed_vec_save(&emb_vec, emb_path);
            embed_vec_free(&emb_vec);
        }

        free(manifest);
        count++;
    }
    closedir(d);

    if (count > 0) {
        nash_log("session_index: backfilled %d sessions", count);
    }
    return count;
}

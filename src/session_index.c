/* session_index.c — In-memory index of past session summaries with embeddings.
 * Part of Nash unified memory architecture v4 (L3 tier).
 *
 * v4.1: Per-chunk journal RAG — sessions are indexed with N chunk embeddings
 * (agent thoughts + tool outputs) for granular MaxSim retrieval.
 * Falls back to single summary.emb for sessions without chunks. */

#include "session_index.h"
#include "journal.h"
#include "str.h"
#include "nash_log.h"
#include "nash_limits.h"
#include "cJSON.h"
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

/* ── Chunk preview I/O ─────────────────────────────────── */

/* Load chunk previews from chunks.idx (JSONL format).
 * Each line: {"id":0,"preview":"text..."}
 * Returns array of strings (caller frees each + array).
 * Sets *out_count to number of previews loaded. */
static char **load_chunk_previews(const char *idx_path, int *out_count) {
    *out_count = 0;
    FILE *f = fopen(idx_path, "r");
    if (!f) return NULL;

    int cap = 16;
    char **previews = calloc((size_t)cap, sizeof(char *));
    if (!previews) { fclose(f); return NULL; }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;
        cJSON *preview_j = cJSON_GetObjectItem(entry, "preview");
        if (preview_j && preview_j->valuestring) {
            if (*out_count >= cap) {
                cap *= 2;
                char **new_p = realloc(previews, (size_t)cap * sizeof(char *));
                if (!new_p) { cJSON_Delete(entry); break; }
                previews = new_p;
            }
            previews[*out_count] = strdup(preview_j->valuestring);
            (*out_count)++;
        }
        cJSON_Delete(entry);
    }
    fclose(f);
    return previews;
}

/* Save chunk previews to chunks.idx (JSONL format). */
static int save_chunk_previews(const char *idx_path, char **previews,
                                int n_previews) {
    FILE *f = fopen(idx_path, "w");
    if (!f) return -1;
    for (int i = 0; i < n_previews; i++) {
        /* Build JSON manually for speed */
        fprintf(f, "{\"id\":%d,\"preview\":", i);
        /* JSON-escape the preview string */
        cJSON *s = cJSON_CreateString(previews[i] ? previews[i] : "");
        char *escaped = cJSON_PrintUnformatted(s);
        fputs(escaped, f);
        free(escaped);
        cJSON_Delete(s);
        fputs("}\n", f);
    }
    fclose(f);
    return 0;
}

/* Free chunk previews array */
static void free_chunk_previews(char **previews, int count) {
    if (!previews) return;
    for (int i = 0; i < count; i++) free(previews[i]);
    free(previews);
}

/* Build preview text for a chunk (first ~200 chars, stripping prefix) */
static char *make_chunk_preview(const char *chunk_text) {
    if (!chunk_text) return strdup("");
    /* Skip "Session YYYY-MM-DD | Query: ..." prefix line */
    const char *body = strchr(chunk_text, '\n');
    if (body) body++; else body = chunk_text;
    char buf[201];
    size_t len = strlen(body);
    if (len > 200) len = 200;
    /* UTF-8 safe truncation */
    while (len > 0 && ((unsigned char)body[len] & 0xC0) == 0x80) len--;
    memcpy(buf, body, len);
    buf[len] = '\0';
    return strdup(buf);
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

        char sess_dir[PATH_MAX];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        /* v4.1: Try chunks.emb first (preferred) */
        char chunks_emb_path[PATH_MAX];
        snprintf(chunks_emb_path, sizeof(chunks_emb_path), "%s/chunks.emb",
                 sess_dir);

        char emb_path[PATH_MAX];
        snprintf(emb_path, sizeof(emb_path), "%s/summary.emb", sess_dir);

        struct stat st;
        int has_chunks_file = (stat(chunks_emb_path, &st) == 0);
        int has_emb_file = (stat(emb_path, &st) == 0);

        if (!has_chunks_file && !has_emb_file) continue;  /* nothing to index */

        if (si_grow(idx) < 0) continue;

        session_index_entry_t *e = &idx->entries[idx->count];
        memset(e, 0, sizeof(*e));
        e->session_dir = strdup(sess_dir);
        e->timestamp = parse_session_timestamp(ent->d_name);

        /* Load manifest text (summary.txt) */
        char txt_path[PATH_MAX];
        snprintf(txt_path, sizeof(txt_path), "%s/summary.txt", sess_dir);
        e->manifest = slurp_file(txt_path, NULL);

        /* Load chunk embeddings if available */
        if (has_chunks_file) {
            e->chunks_emb = embed_multi_vec_load(chunks_emb_path);
            if (e->chunks_emb.data && e->chunks_emb.n_chunks > 0) {
                e->has_chunks = 1;
                /* Load chunk previews */
                char cidx_path[PATH_MAX];
                snprintf(cidx_path, sizeof(cidx_path), "%s/chunks.idx",
                         sess_dir);
                e->chunk_previews = load_chunk_previews(cidx_path,
                    &e->n_chunk_previews);
            }
        }

        /* Also load legacy summary.emb as fallback */
        if (has_emb_file) {
            e->emb = embed_vec_load(emb_path);
            if (e->emb.data) e->has_emb = 1;
        }

        /* Must have at least one valid embedding */
        if (!e->has_chunks && !e->has_emb) {
            free(e->session_dir);
            free(e->manifest);
            embed_multi_vec_free(&e->chunks_emb);
            free_chunk_previews(e->chunk_previews, e->n_chunk_previews);
            memset(e, 0, sizeof(*e));
            continue;
        }

        idx->count++;
    }
    closedir(d);

    /* Sort by timestamp (newest first) */
    if (idx->count > 1) {
        qsort(idx->entries, (size_t)idx->count,
              sizeof(session_index_entry_t), entry_cmp_ts);
    }

    int n_chunks_total = 0;
    int n_with_chunks = 0;
    for (int i = 0; i < idx->count; i++) {
        if (idx->entries[i].has_chunks) {
            n_with_chunks++;
            n_chunks_total += idx->entries[i].chunks_emb.n_chunks;
        }
    }
    nash_log("session_index: loaded %d sessions (%d with chunks, %d total chunks)",
             idx->count, n_with_chunks, n_chunks_total);
    return idx;
}

int session_index_add(session_index_t *idx, const char *session_dir,
                      const char *manifest, const embed_vec_t *emb,
                      const embed_multi_vec_t *chunks,
                      char **chunk_previews, int n_previews) {
    if (!idx || !session_dir) return -1;

    pthread_mutex_lock(&idx->mtx);

    /* Check for existing entry (continued session) — update in-place */
    session_index_entry_t *e = NULL;
    int is_update = 0;
    for (int i = 0; i < idx->count; i++) {
        if (idx->entries[i].session_dir &&
            strcmp(idx->entries[i].session_dir, session_dir) == 0) {
            e = &idx->entries[i];
            is_update = 1;
            /* Free old data before overwriting */
            free(e->manifest);
            embed_vec_free(&e->emb);
            embed_multi_vec_free(&e->chunks_emb);
            free_chunk_previews(e->chunk_previews, e->n_chunk_previews);
            /* Keep session_dir (same string), clear the rest */
            char *kept_dir = e->session_dir;
            memset(e, 0, sizeof(*e));
            e->session_dir = kept_dir;
            nash_log("session_index: updating existing entry for %s", session_dir);
            break;
        }
    }

    if (!e) {
        /* New entry */
        if (si_grow(idx) < 0) {
            pthread_mutex_unlock(&idx->mtx);
            return -1;
        }
        e = &idx->entries[idx->count];
        memset(e, 0, sizeof(*e));
        e->session_dir = strdup(session_dir);
    }

    e->manifest = manifest ? strdup(manifest) : NULL;
    e->timestamp = parse_session_timestamp(session_dir);

    /* Legacy summary embedding */
    if (emb && emb->data) {
        e->emb.dim = emb->dim;
        e->emb.data = malloc((size_t)emb->dim * sizeof(float));
        if (e->emb.data) {
            memcpy(e->emb.data, emb->data, (size_t)emb->dim * sizeof(float));
            e->has_emb = 1;
        }
    }

    /* v4.1: Chunk embeddings */
    if (chunks && chunks->data && chunks->n_chunks > 0) {
        size_t data_sz = (size_t)chunks->dim * (size_t)chunks->n_chunks * sizeof(float);
        e->chunks_emb.data = malloc(data_sz);
        if (e->chunks_emb.data) {
            memcpy(e->chunks_emb.data, chunks->data, data_sz);
            e->chunks_emb.dim = chunks->dim;
            e->chunks_emb.n_chunks = chunks->n_chunks;
            e->has_chunks = 1;
        }
        /* Copy chunk previews */
        if (chunk_previews && n_previews > 0) {
            e->chunk_previews = calloc((size_t)n_previews, sizeof(char *));
            if (e->chunk_previews) {
                for (int i = 0; i < n_previews; i++)
                    e->chunk_previews[i] = chunk_previews[i]
                        ? strdup(chunk_previews[i]) : NULL;
                e->n_chunk_previews = n_previews;
            }
        }
    }

    if (!is_update)
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
        embed_multi_vec_free(&idx->entries[i].chunks_emb);
        free_chunk_previews(idx->entries[i].chunk_previews,
                           idx->entries[i].n_chunk_previews);
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
    typedef struct { int idx; double score; int best_chunk; } scored_t;
    scored_t *scored = calloc((size_t)idx->count, sizeof(scored_t));
    if (!scored) { pthread_mutex_unlock(&idx->mtx); return out; }

    double now_ts = (double)time(NULL);
    int n_scored = 0;

    for (int i = 0; i < idx->count; i++) {
        float semantic = 0.0f;
        int best_chunk = -1;

        if (idx->entries[i].has_chunks) {
            /* v4.1: MaxSim across chunks — find best-matching chunk */
            for (int c = 0; c < idx->entries[i].chunks_emb.n_chunks; c++) {
                embed_vec_t chunk_vec = {
                    .data = idx->entries[i].chunks_emb.data
                          + c * idx->entries[i].chunks_emb.dim,
                    .dim = idx->entries[i].chunks_emb.dim
                };
                float sim = embed_cosine_sim(query_emb, &chunk_vec);
                if (sim > semantic) {
                    semantic = sim;
                    best_chunk = c;
                }
            }
        } else if (idx->entries[i].has_emb) {
            /* Legacy: single summary embedding */
            semantic = embed_cosine_sim(query_emb, &idx->entries[i].emb);
        } else {
            continue;
        }

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
        scored[n_scored].best_chunk = best_chunk;
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
                out.results[i].best_chunk = scored[i].best_chunk;

                /* v4.1: Include best-matching chunk preview */
                if (scored[i].best_chunk >= 0 && e->chunk_previews &&
                    scored[i].best_chunk < e->n_chunk_previews) {
                    out.results[i].chunk_preview = e->chunk_previews[scored[i].best_chunk]
                        ? strdup(e->chunk_previews[scored[i].best_chunk]) : NULL;
                }
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
        free(r->results[i].chunk_preview);
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

/* ── v4.1: Chunk backfill ──────────────────────────── */

int session_index_chunk_backfill(const char *sessions_dir, embed_ctx_t *embed,
                                  session_index_t *idx) {
    if (!sessions_dir || !embed) return 0;

    DIR *d = opendir(sessions_dir);
    if (!d) return 0;

    int count = 0;
    int total_chunks = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sess_dir[PATH_MAX];
        snprintf(sess_dir, sizeof(sess_dir), "%s/%s",
                 sessions_dir, ent->d_name);

        /* Skip sessions that already have chunks.emb */
        char chunks_emb_path[PATH_MAX];
        snprintf(chunks_emb_path, sizeof(chunks_emb_path), "%s/chunks.emb",
                 sess_dir);
        struct stat st;
        if (stat(chunks_emb_path, &st) == 0) continue;

        /* Check if journal.jsonl exists */
        char journal_path[PATH_MAX];
        snprintf(journal_path, sizeof(journal_path), "%s/journal.jsonl",
                 sess_dir);
        if (stat(journal_path, &st) != 0) continue;

        /* Extract semantic chunks from journal */
        journal_chunks_t jc = journal_extract_chunks(sess_dir, 900, 50);
        if (jc.n_chunks == 0) continue;

        /* Embed all chunks via batch API */
        int out_count = 0;
        embed_vec_t *vecs = embed_text_batch(embed,
            (const char **)jc.texts, jc.n_chunks, &out_count);

        if (!vecs || out_count == 0) {
            journal_chunks_free(&jc);
            continue;
        }

        /* Build multi-vector from individual embeddings */
        int dim = 0;
        int valid_count = 0;
        for (int i = 0; i < out_count; i++) {
            if (vecs[i].data) {
                if (dim == 0) dim = vecs[i].dim;
                valid_count++;
            }
        }

        if (valid_count > 0 && dim > 0) {
            embed_multi_vec_t mv = {0};
            mv.dim = dim;
            mv.n_chunks = valid_count;
            mv.data = malloc((size_t)dim * (size_t)valid_count * sizeof(float));

            /* Build previews */
            char **previews = calloc((size_t)valid_count, sizeof(char *));
            int vi = 0;

            if (mv.data && previews) {
                for (int i = 0; i < out_count && vi < valid_count; i++) {
                    if (!vecs[i].data) continue;
                    memcpy(mv.data + vi * dim, vecs[i].data,
                           (size_t)dim * sizeof(float));
                    previews[vi] = make_chunk_preview(jc.texts[i]);
                    vi++;
                }

                /* Save chunks.emb */
                embed_multi_vec_save(&mv, chunks_emb_path);

                /* Save chunks.idx */
                char cidx_path[PATH_MAX];
                snprintf(cidx_path, sizeof(cidx_path), "%s/chunks.idx",
                         sess_dir);
                save_chunk_previews(cidx_path, previews, valid_count);

                /* Also generate summary.txt + summary.emb if missing */
                char txt_path[PATH_MAX];
                snprintf(txt_path, sizeof(txt_path), "%s/summary.txt",
                         sess_dir);
                if (stat(txt_path, &st) != 0) {
                    journal_t *j = journal_new(sess_dir);
                    if (j) {
                        char *manifest = journal_manifest(j, 0);
                        journal_free(j);
                        if (manifest && strlen(manifest) > 30) {
                            FILE *sf = fopen(txt_path, "w");
                            if (sf) { fputs(manifest, sf); fclose(sf); }

                            embed_vec_t semb = embed_text(embed, manifest);
                            if (semb.data) {
                                char emb_path[PATH_MAX];
                                snprintf(emb_path, sizeof(emb_path),
                                         "%s/summary.emb", sess_dir);
                                embed_vec_save(&semb, emb_path);
                                embed_vec_free(&semb);
                            }
                        }
                        free(manifest);
                    }
                }

                /* Add to in-memory index if provided */
                if (idx) {
                    char *manifest = NULL;
                    char mpath[PATH_MAX];
                    snprintf(mpath, sizeof(mpath), "%s/summary.txt", sess_dir);
                    manifest = slurp_file(mpath, NULL);

                    session_index_add(idx, sess_dir, manifest, NULL,
                                     &mv, previews, valid_count);
                    free(manifest);
                }

                total_chunks += valid_count;
                count++;
            }

            free_chunk_previews(previews, valid_count);
            embed_multi_vec_free(&mv);
        }

        /* Cleanup */
        for (int i = 0; i < out_count; i++)
            embed_vec_free(&vecs[i]);
        free(vecs);
        journal_chunks_free(&jc);
    }
    closedir(d);

    if (count > 0) {
        nash_log("session_index: chunk-backfilled %d sessions (%d total chunks)",
                 count, total_chunks);
    }
    return count;
}

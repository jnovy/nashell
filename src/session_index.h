#ifndef SESSION_INDEX_H
#define SESSION_INDEX_H

/* Session index: in-memory index of past session summaries with embeddings.
 * Enables semantic search across session history via /? query and
 * unified memory_recall (L3 tier in unified memory architecture v4). */

#include "embedding.h"
#include <pthread.h>

typedef struct {
    char              *session_dir;   /* full path to sessions/<ts>/ */
    char              *manifest;      /* journal_manifest() output (summary.txt) */
    double             timestamp;     /* session timestamp from dir name */
    embed_vec_t        emb;           /* embedding of manifest text */
    int                has_emb;       /* 1 if emb is valid */
} session_index_entry_t;

typedef struct session_index_t {
    session_index_entry_t *entries;
    int count;
    int cap;
    pthread_mutex_t mtx;              /* thread safety: /? vs react loop */
} session_index_t;

/* Search result from session index */
typedef struct {
    char   *session_dir;
    char   *manifest;      /* journal_manifest() output */
    double  timestamp;
    double  score;         /* composite: semantic × recency */
} session_index_result_t;

typedef struct {
    session_index_result_t *results;
    int count;
} session_index_results_t;

/* ── Lifecycle ──────────────────────────────────────── */

/* Load session index from disk: scan sessions/<ts>/summary.emb files.
 * Returns NULL on error or if sessions_dir does not exist.
 * embed_ctx may be NULL (sessions without embeddings are skipped). */
session_index_t *session_index_load(const char *sessions_dir);

/* Add a just-completed session to the in-memory index.
 * manifest: journal_manifest() output (owned by caller).
 * embed: embedding context for generating summary.emb (may be NULL). */
int session_index_add(session_index_t *idx, const char *session_dir,
                      const char *manifest, const embed_vec_t *emb);

/* Free session index and all entries. */
void session_index_free(session_index_t *idx);

/* ── Search ─────────────────────────────────────────── */

/* Search session history by semantic similarity.
 * query_emb: pre-computed embedding of the query.
 * max_results: maximum number of results to return.
 * Returns results sorted by score (descending).
 * Caller must free with session_index_results_free(). */
session_index_results_t session_index_search(
    session_index_t *idx,
    const embed_vec_t *query_emb,
    int max_results);

/* Free search results. */
void session_index_results_free(session_index_results_t *r);

/* ── Backfill ───────────────────────────────────────── */

/* Backfill old sessions: generate summary.txt + summary.emb for sessions
 * that don't have them yet. Returns number of sessions backfilled. */
int session_index_backfill(const char *sessions_dir, embed_ctx_t *embed);

#endif /* SESSION_INDEX_H */

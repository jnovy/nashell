#ifndef SESSION_INDEX_H
#define SESSION_INDEX_H

/* Session index: in-memory index of past session summaries with embeddings.
 * Enables semantic search across session history via /? query and
 * unified memory_query (L3 tier in unified memory architecture v4).
 *
 * v4.1: Per-chunk journal RAG — each session gets N chunk embeddings
 * (agent thoughts + tool outputs) instead of just 1 summary embedding.
 * Uses MaxSim (max cosine over chunks) for granular retrieval. */

#include "embedding.h"
#include <pthread.h>

typedef struct {
    char              *session_dir;   /* full path to sessions/<ts>/ */
    char              *manifest;      /* journal_manifest() output (summary.txt) */
    double             timestamp;     /* session timestamp from dir name */
    /* Legacy single-vector embedding (summary.emb) */
    embed_vec_t        emb;           /* embedding of manifest text */
    int                has_emb;       /* 1 if emb is valid */
    /* v4.1: Per-chunk multi-vector embedding (chunks.emb) */
    embed_multi_vec_t  chunks_emb;    /* multi-vector: one per chunk */
    char             **chunk_previews;/* preview text per chunk (from chunks.idx) */
    int                n_chunk_previews;
    int                has_chunks;    /* 1 if chunk data loaded */
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
    char   *chunk_preview; /* v4.1: best matching chunk text (NULL if legacy) */
    double  timestamp;
    double  score;         /* composite: semantic × recency */
    int     best_chunk;    /* v4.1: index of best-matching chunk (-1 if legacy) */
} session_index_result_t;

typedef struct {
    session_index_result_t *results;
    int count;
} session_index_results_t;

/* ── Lifecycle ──────────────────────────────────────── */

/* Load session index from disk: scan sessions/<ts>/ for chunks.emb
 * (preferred) or summary.emb (fallback).
 * Returns NULL on error or if sessions_dir does not exist. */
session_index_t *session_index_load(const char *sessions_dir);

/* Load additional sessions from another directory into an existing index.
 * Used to merge workspace sessions into the global index.
 * Returns number of sessions added, -1 on error. */
int session_index_load_dir(session_index_t *idx, const char *sessions_dir);

/* Add a just-completed session to the in-memory index.
 * manifest: journal_manifest() output (owned by caller).
 * emb: single summary embedding (legacy, may be NULL).
 * chunks: chunk multi-vector embedding (may be NULL).
 * chunk_previews/n_previews: preview texts for chunks (may be NULL/0). */
int session_index_add(session_index_t *idx, const char *session_dir,
                      const char *manifest, const embed_vec_t *emb,
                      const embed_multi_vec_t *chunks,
                      char **chunk_previews, int n_previews);

/* Free session index and all entries. */
void session_index_free(session_index_t *idx);

/* ── Search ─────────────────────────────────────────── */

/* Search session history by semantic similarity.
 * query_emb: pre-computed embedding of the query.
 * max_results: maximum number of results to return.
 * Uses MaxSim across chunks when available (v4.1), falls back to
 * single summary embedding for legacy sessions.
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

/* v4.1: Backfill chunk embeddings for all sessions that have journal.jsonl
 * but no chunks.emb. Generates semantic chunks from journal, embeds them
 * via ONNX, and saves chunks.emb + chunks.idx.
 * Also generates summary.txt + summary.emb if missing.
 * Returns number of sessions backfilled. */
int session_index_chunk_backfill(const char *sessions_dir, embed_ctx_t *embed,
                                 session_index_t *idx);

#endif /* SESSION_INDEX_H */

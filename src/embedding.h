#ifndef EMBEDDING_H
#define EMBEDDING_H

/* Semantic embedding support for memory recall.
 *
 * Generates vector embeddings via:
 *   - Local ONNX Runtime inference (preferred — no external service needed)
 *   - External API (ollama, OpenAI-compatible)
 *
 * Design inspired by GDN-2's "short convolution on gates" principle:
 * instead of independent scoring per memory (substring match), we use
 * dense vector representations that capture semantic relationships —
 * a continuous, context-aware scoring mechanism.
 *
 * Graceful degradation: if no embedding backend is available, memory_query
 * falls back to the existing substring-based scoring. */

#include <stddef.h>

/* Forward declaration for ONNX backend (opaque) */
typedef struct onnx_embed_ctx onnx_embed_ctx_t;

/* ── Embedding backend types ─────────────────────────── */

typedef enum {
  EMBED_NONE = 0,   /* disabled — use substring matching */
  EMBED_OLLAMA = 1, /* Ollama API (http://host:11434/api/embed) */
  EMBED_OPENAI = 2, /* OpenAI-compatible (/v1/embeddings) */
  EMBED_ONNX = 3,   /* Local ONNX Runtime (all-MiniLM-L6-v2) */
} embed_type_t;

/* ── Embedding configuration ─────────────────────────── */

typedef struct {
  embed_type_t type;
  char *model;         /* e.g. "nomic-embed-text", "text-embedding-3-small" */
  char *api_base;      /* e.g. "http://localhost:11434" */
  char *model_path;    /* ONNX: directory containing onnx/model.onnx + vocab.txt */
  int dimension;       /* expected embedding dimension (0 = auto-detect) */
  int max_input_chars; /* max input chars for text preparation (0 = auto from model) */
} embed_config_t;

/* ── Embedding context ───────────────────────────────── */

typedef struct {
  embed_config_t cfg;
  int available;          /* 1 if embedding backend is ready */
  int detected_dim;       /* auto-detected dimension from first successful call */
  onnx_embed_ctx_t *onnx; /* ONNX backend context (NULL if not using ONNX) */
  int owns_onnx;          /* 1 if this ctx owns (and should free) the onnx ctx */
} embed_ctx_t;

/* ── Embedding vector ────────────────────────────────── */

typedef struct {
  float *data; /* float32 array */
  int dim;     /* dimension count */
} embed_vec_t;

/* ── Lifecycle ───────────────────────────────────────── */

/* Create embedding context. Does NOT probe the service yet.
 * Call embed_probe() to check availability. */
embed_ctx_t *embed_new(const embed_config_t *cfg);

/* Probe embedding service availability. Returns 1 if available, 0 if not.
 * Sends a test embedding request. Sets ctx->available and ctx->detected_dim. */
int embed_probe(embed_ctx_t *ctx);

/* Free embedding context */
void embed_free(embed_ctx_t *ctx);

/* Create a shared embedding context that reuses the ONNX session from src.
 * The shared context does NOT own the ONNX session (won't free it).
 * This avoids loading the model twice for global + workspace memory. */
embed_ctx_t *embed_share(embed_ctx_t *src);

/* Return the effective max input chars for text preparation.
 * If cfg.max_input_chars is set (>0), uses that.
 * Otherwise, auto-detects from model name using a built-in table of
 * known embedding models and their context windows.
 * Fallback: 2000 chars (~500 tokens) for unknown models. */
int embed_max_input_chars(const embed_ctx_t *ctx);

/* Count tokens for text using the active backend's tokenizer.
 * Returns token count, or -1 if backend has no tokenizer.
 * For API backends, returns estimated count = strlen(text) / 4. */
int embed_count_tokens(const embed_ctx_t *ctx, const char *text);

/* ── Embedding generation ────────────────────────────── */

/* Generate embedding for a text string.
 * Returns embed_vec_t with data=NULL on failure.
 * Caller must free result with embed_vec_free(). */
embed_vec_t embed_text(embed_ctx_t *ctx, const char *text);

/* Free an embedding vector */
void embed_vec_free(embed_vec_t *v);

/* ── Vector operations ───────────────────────────────── */

/* Cosine similarity between two vectors. Returns [-1.0, 1.0].
 * Returns 0.0 if dimensions don't match or either is NULL. */
float embed_cosine_sim(const embed_vec_t *a, const embed_vec_t *b);

/* ── Persistence ─────────────────────────────────────── */

/* Save embedding vector to a binary file.
 * Format: [int32 dimension][float32 × dimension]
 * Returns 0 on success, -1 on failure. */
int embed_vec_save(const embed_vec_t *v, const char *path);

/* Load embedding vector from a binary file.
 * Returns embed_vec_t with data=NULL on failure.
 * Caller must free with embed_vec_free(). */
embed_vec_t embed_vec_load(const char *path);

/* ── Batch embedding ─────────────────────────────────── */

/* Generate embeddings for multiple texts at once.
 * Returns array of embed_vec_t (caller must free each with embed_vec_free,
 * then free the array itself).
 * FIX #15: Currently calls embed_text in a loop; backends can optimize
 * with batched API calls in the future.
 * Returns NULL on failure. Sets *out_count to number of results. */
embed_vec_t *embed_text_batch(embed_ctx_t *ctx, const char **texts,
                              int n_texts, int *out_count);

/* ── Text preparation ────────────────────────────────── */

/* Prepare memory content for embedding: concatenates key + value,
 * truncated to max_chars. Caller must free returned string. */
char *embed_prepare_text(const char *key, const char *value, int max_chars);

/* ── Chunked (multi-vector) embeddings ───────────────── */

/* Multi-vector embedding: one memory entry → N chunk vectors.
 * Solves the truncation asymmetry: long values (skills, strategies) are
 * split into overlapping chunks, each prefixed with key for context.
 * At recall time, similarity = max over all chunks (MaxSim). */

typedef struct {
  float *data;  /* float32 array: dim × n_chunks contiguous */
  int dim;      /* dimension of each chunk vector */
  int n_chunks; /* number of chunk vectors (≥1) */
} embed_multi_vec_t;

/* Prepare memory content as overlapping chunks for embedding.
 * Each chunk = key + value_slice (with overlap between slices).
 * If content fits in one chunk, returns array of 1.
 * chunk_max_chars: max chars per chunk (default 2000 if ≤0).
 * overlap_chars: overlap between consecutive value slices (default 200 if ≤0).
 * Sets *out_n_chunks to number of chunks returned.
 * Caller must free each string and the array itself. */
char **embed_prepare_text_chunked(const char *key, const char *value,
                                  int chunk_max_chars, int overlap_chars,
                                  int *out_n_chunks);

/* Save multi-vector embedding to a binary file.
 * Format: [int32 -n_chunks][int32 dim][float32 × dim × n_chunks]
 * Negative first int32 distinguishes from single-vec format (positive dim).
 * Returns 0 on success, -1 on failure. */
int embed_multi_vec_save(const embed_multi_vec_t *mv, const char *path);

/* Load multi-vector embedding from a binary file.
 * Auto-detects single-vec (old) vs multi-vec (new) format:
 *   - first int32 > 0 → old format, returns n_chunks=1
 *   - first int32 < 0 → new format, returns n_chunks=abs(first)
 * Returns embed_multi_vec_t with data=NULL on failure.
 * Caller must free with embed_multi_vec_free(). */
embed_multi_vec_t embed_multi_vec_load(const char *path);

/* Free a multi-vector embedding */
void embed_multi_vec_free(embed_multi_vec_t *mv);

/* MaxSim: max cosine similarity of a single query vector against
 * all chunks of a stored multi-vector.
 * sim = max_i cosine(query, stored_chunk_i)
 * Returns 0.0 if either is NULL or dimensions mismatch. */
float embed_cosine_sim_multi(const embed_vec_t *query,
                             const embed_multi_vec_t *stored);

/* MaxSim between two multi-vectors (for consolidation):
 * sim = max_{i,j} cosine(a_chunk_i, b_chunk_j)
 * Returns 0.0 if either is NULL or dimensions mismatch. */
float embed_cosine_sim_multi_multi(const embed_multi_vec_t *a,
                                   const embed_multi_vec_t *b);

#endif

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
 * Graceful degradation: if no embedding backend is available, memory_recall
 * falls back to the existing substring-based scoring. */

#include <stddef.h>

/* Forward declaration for ONNX backend (opaque) */
typedef struct onnx_embed_ctx onnx_embed_ctx_t;

/* ── Embedding backend types ─────────────────────────── */

typedef enum {
    EMBED_NONE   = 0,  /* disabled — use substring matching */
    EMBED_OLLAMA = 1,  /* Ollama API (http://host:11434/api/embed) */
    EMBED_OPENAI = 2,  /* OpenAI-compatible (/v1/embeddings) */
    EMBED_ONNX   = 3,  /* Local ONNX Runtime (all-MiniLM-L6-v2) */
} embed_type_t;

/* ── Embedding configuration ─────────────────────────── */

typedef struct {
    embed_type_t type;
    char *model;       /* e.g. "nomic-embed-text", "text-embedding-3-small" */
    char *api_base;    /* e.g. "http://localhost:11434" */
    char *model_path;  /* ONNX: directory containing onnx/model.onnx + vocab.txt */
    int   dimension;   /* expected embedding dimension (0 = auto-detect) */
} embed_config_t;

/* ── Embedding context ───────────────────────────────── */

typedef struct {
    embed_config_t cfg;
    int  available;     /* 1 if embedding backend is ready */
    int  detected_dim;  /* auto-detected dimension from first successful call */
    onnx_embed_ctx_t *onnx;  /* ONNX backend context (NULL if not using ONNX) */
} embed_ctx_t;

/* ── Embedding vector ────────────────────────────────── */

typedef struct {
    float *data;    /* float32 array */
    int    dim;     /* dimension count */
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

/* Prepare memory content for embedding: concatenates key + tags + value,
 * truncated to max_chars. Caller must free returned string. */
char *embed_prepare_text(const char *key, const char *value,
                         const char **tags, int n_tags, int max_chars);

#endif

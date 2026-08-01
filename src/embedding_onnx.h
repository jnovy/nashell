#ifndef EMBEDDING_ONNX_H
#define EMBEDDING_ONNX_H

/* ONNX Runtime local embedding backend for nash.
 *
 * Runs all-MiniLM-L6-v2 (or compatible BERT-family model) locally
 * via ONNX Runtime C API. No external service needed.
 *
 * Components:
 *   1. WordPiece tokenizer (parses vocab.txt)
 *   2. ONNX Runtime session (loads model.onnx)
 *   3. Mean pooling (converts per-token hidden states → single embedding)
 *
 * Output: 384-dimensional float32 embedding vector.
 */

#include <stddef.h>

/* Opaque handle for ONNX embedding context */
typedef struct onnx_embed_ctx onnx_embed_ctx_t;

/* Initialize ONNX embedding.
 * model_dir: directory containing onnx/model.onnx and vocab.txt
 * Returns context on success, NULL on failure. */
onnx_embed_ctx_t *onnx_embed_init(const char *model_dir);

/* Generate embedding for text.
 * out_dim: receives the dimension count (384 for MiniLM).
 * Returns malloc'd float array on success, NULL on failure.
 * Caller must free() the returned array. */
float *onnx_embed_text(onnx_embed_ctx_t *ctx, const char *text, int *out_dim);

/* Batch embed N texts in a single ONNX Run() call.
 * Returns array of N float* pointers (each malloc'd, dim floats).
 * Individual entries may be NULL if tokenization failed.
 * Sets *out_dim to the hidden dimension.
 * Caller must free each non-NULL entry and the array.
 * Processes up to 32 texts; caller should chunk for larger sets.
 * Returns NULL on total failure. */
float **onnx_embed_text_batch(onnx_embed_ctx_t *ctx, const char **texts,
                              int n_texts, int *out_dim);

/* Free ONNX embedding context */
void onnx_embed_free(onnx_embed_ctx_t *ctx);

/* Get embedding dimension (0 if not initialized) */
int onnx_embed_dim(const onnx_embed_ctx_t *ctx);

/* Count tokens for text without running inference.
 * Returns token count (including [CLS] and [SEP]), or 0 on error. */
int onnx_count_tokens(onnx_embed_ctx_t *ctx, const char *text);

#endif

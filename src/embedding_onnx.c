#define _GNU_SOURCE
#include "embedding_onnx.h"
#include "nash_limits.h"
#include "str.h"
#include <onnxruntime/onnxruntime_c_api.h>
#include "tui.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h> /* sysconf */

/* ── WordPiece tokenizer ─────────────────────────────── */

#define WP_MAX_TOKENS 512   /* BERT max sequence length */
#define WP_MAX_WORD_LEN 200 /* max chars per word before giving up */
#define WP_UNK_ID 100       /* [UNK] token ID */
#define WP_CLS_ID 101       /* [CLS] token ID */
#define WP_SEP_ID 102       /* [SEP] token ID */
#define WP_PAD_ID 0         /* [PAD] token ID */

#define ONNX_MODEL_MAX_TOKENS 256 /* all-MiniLM-L6-v2 trained context */

/* Hash table entry for vocab lookup */
typedef struct wp_entry {
  char *token;
  int id;
  struct wp_entry *next;
} wp_entry_t;

#define WP_HASH_SIZE 65536

typedef struct {
  wp_entry_t *buckets[WP_HASH_SIZE];
  int vocab_size;
} wp_vocab_t;

/* FNV-1a hash */
static unsigned wp_hash(const char *s, int len) {
  unsigned h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 16777619u;
  }
  return h & (WP_HASH_SIZE - 1);
}

static void wp_vocab_free(wp_vocab_t *v) {
  if (!v) return;
  for (int i = 0; i < WP_HASH_SIZE; i++) {
    wp_entry_t *e = v->buckets[i];
    while (e) {
      wp_entry_t *next = e->next;
      free(e->token);
      free(e);
      e = next;
    }
  }
  free(v);
}

static void wp_vocab_insert(wp_vocab_t *v, const char *token, int id) {
  int len = (int)strlen(token);
  unsigned h = wp_hash(token, len);
  wp_entry_t *e = xmalloc(sizeof(*e));
  e->token = xstrdup(token);
  e->id = id;
  e->next = v->buckets[h];
  v->buckets[h] = e;
  v->vocab_size++;
}

static int wp_vocab_lookup(const wp_vocab_t *v, const char *token, int len) {
  unsigned h = wp_hash(token, len);
  for (wp_entry_t *e = v->buckets[h]; e; e = e->next) {
    if ((int)strlen(e->token) == len && memcmp(e->token, token, (size_t)len) == 0)
      return e->id;
  }
  return -1;
}

/* Load vocab.txt: one token per line, line number = ID */
static wp_vocab_t *wp_vocab_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    nash_log("[onnx-embed] cannot open vocab: %s", path);
    return NULL;
  }

  wp_vocab_t *v = xcalloc(1, sizeof(*v));

  char line[1024];
  int id = 0;
  while (fgets(line, sizeof(line), f)) {
    /* Strip trailing newline/whitespace */
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
      line[--len] = '\0';
    if (len > 0)
      wp_vocab_insert(v, line, id);
    id++;
  }

  fclose(f);
  nash_log("[onnx-embed] loaded vocab: %d tokens from %s",
           v->vocab_size, path);
  return v;
}

/* Tokenize a single word using WordPiece.
 * Returns number of token IDs written to out_ids.
 * word must be lowercase, ASCII (basic tokenization already done). */
static int wp_tokenize_word(const wp_vocab_t *vocab, const char *word, int word_len,
                            int64_t *out_ids, int max_out) {
  if (word_len <= 0 || max_out <= 0) return 0;

  int count = 0;
  int start = 0;

  while (start < word_len) {
    int end = word_len;
    int found = 0;

    while (end > start) {
      /* Build candidate: "##" prefix for continuation tokens */
      char candidate[WP_MAX_WORD_LEN + 4];
      int clen;
      if (start > 0) {
        candidate[0] = '#';
        candidate[1] = '#';
        memcpy(candidate + 2, word + start, (size_t)(end - start));
        clen = 2 + (end - start);
      } else {
        memcpy(candidate, word + start, (size_t)(end - start));
        clen = end - start;
      }

      int id = wp_vocab_lookup(vocab, candidate, clen);
      if (id >= 0) {
        if (count >= max_out) return count;
        out_ids[count++] = (int64_t)id;
        start = end;
        found = 1;
        break;
      }
      end--;
    }

    if (!found) {
      /* Character not in vocab — emit [UNK] for entire word */
      if (count >= max_out) return count;
      out_ids[count++] = WP_UNK_ID;
      break;
    }
  }

  return count;
}

/* Full BERT tokenization: lowercase + basic tokenize + WordPiece.
 * Adds [CLS] at start and [SEP] at end.
 * Returns total number of tokens.
 *
 * TODO(flaw-I): isspace/ispunct/tolower are ASCII-only. Multi-byte UTF-8
 * characters are split into individual bytes and mapped to [UNK], degrading
 * embedding quality for non-English text. Need ICU or a UTF-8-aware
 * ctype replacement for proper Unicode tokenization. */
static int wp_tokenize(const wp_vocab_t *vocab, const char *text,
                       int64_t *input_ids, int64_t *attention_mask,
                       int64_t *token_type_ids, int max_len) {
  if (!text || max_len < 3) return 0;

  int pos = 0;
  input_ids[pos] = WP_CLS_ID;
  attention_mask[pos] = 1;
  token_type_ids[pos] = 0;
  pos++;

  const char *p = text;
  while (*p && pos < max_len - 1) { /* -1 for final [SEP] */
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p) break;

    /* Extract word: split on whitespace and punctuation */
    char word[WP_MAX_WORD_LEN];
    int wlen = 0;

    if (ispunct((unsigned char)*p)) {
      /* Punctuation is its own token */
      word[0] = (char)tolower((unsigned char)*p);
      wlen = 1;
      p++;
    } else {
      /* Collect word characters (letters, digits) */
      while (*p && !isspace((unsigned char)*p) &&
             !ispunct((unsigned char)*p) &&
             wlen < WP_MAX_WORD_LEN - 1) {
        word[wlen++] = (char)tolower((unsigned char)*p);
        p++;
      }
    }
    word[wlen] = '\0';

    if (wlen > 0) {
      int n = wp_tokenize_word(vocab, word, wlen,
                               input_ids + pos, max_len - 1 - pos);
      for (int i = 0; i < n; i++) {
        attention_mask[pos + i] = 1;
        token_type_ids[pos + i] = 0;
      }
      pos += n;
    }
  }

  /* Add [SEP] */
  if (pos < max_len) {
    input_ids[pos] = WP_SEP_ID;
    attention_mask[pos] = 1;
    token_type_ids[pos] = 0;
    pos++;
  }

  return pos;
}

/* ── ONNX Runtime context ────────────────────────────── */

struct onnx_embed_ctx {
  const OrtApi *api;
  OrtEnv *env;
  OrtSessionOptions *opts;
  OrtSession *session;
  OrtMemoryInfo *mem_info;
  wp_vocab_t *vocab;
  int dim; /* embedding dimension (384 for MiniLM) */
};

/* Count tokens for text without running inference.
 * Returns token count (including [CLS] and [SEP]), or 0 on error. */
int onnx_count_tokens(onnx_embed_ctx_t *ctx, const char *text) {
  if (!ctx || !text || !ctx->vocab) return 0;

  int64_t input_ids[WP_MAX_TOKENS];
  int64_t attention_mask[WP_MAX_TOKENS];
  int64_t token_type_ids[WP_MAX_TOKENS];

  return wp_tokenize(ctx->vocab, text,
                     input_ids, attention_mask, token_type_ids,
                     WP_MAX_TOKENS);
}

onnx_embed_ctx_t *onnx_embed_init(const char *model_dir) {
  if (!model_dir) return NULL;

  /* Build paths */
  char model_path[NASH_PATH_MAX], vocab_path[NASH_PATH_MAX];
  snprintf(model_path, sizeof(model_path), "%s/onnx/model.onnx", model_dir);
  snprintf(vocab_path, sizeof(vocab_path), "%s/vocab.txt", model_dir);

  /* Check files exist */
  FILE *f = fopen(model_path, "rb");
  if (!f) {
    nash_log("[onnx-embed] model not found: %s", model_path);
    return NULL;
  }
  fclose(f);

  /* Load vocab */
  wp_vocab_t *vocab = wp_vocab_load(vocab_path);
  if (!vocab) return NULL;

  /* Get ONNX Runtime API - try compile-time version first, then fall back
   * to older versions so a binary built against newer headers still works
   * with an older onnxruntime shared library at runtime. */
  const OrtApiBase *base = OrtGetApiBase();
  const OrtApi *api = NULL;
  for (int v = ORT_API_VERSION; v >= 1; v--) {
    api = base->GetApi(v);
    if (api) break;
  }
  if (!api) {
    nash_log("[onnx-embed] failed to get ONNX Runtime API (tried v%d..1)",
             ORT_API_VERSION);
    wp_vocab_free(vocab);
    return NULL;
  }

  onnx_embed_ctx_t *ctx = xcalloc(1, sizeof(*ctx));
  ctx->api = api;
  ctx->vocab = vocab;

  OrtStatus *status = NULL;

  /* Create environment */
  status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "nash-embed", &ctx->env);
  if (status) {
    nash_log("[onnx-embed] CreateEnv failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto fail;
  }

  /* Create session options */
  status = api->CreateSessionOptions(&ctx->opts);
  if (status) {
    nash_log("[onnx-embed] CreateSessionOptions failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto fail;
  }

  /* Auto-detect thread count for ONNX inference.
     * Clamp to [2, 8] - more threads help batch inference but diminishing
     * returns beyond 8 for small embedding models. */
  {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    int n_threads = (nproc > 0) ? (int)nproc : 2;
    if (n_threads < 2) n_threads = 2;
    if (n_threads > 8) n_threads = 8;
    status = api->SetIntraOpNumThreads(ctx->opts, n_threads);
    if (status) api->ReleaseStatus(status);
  }
  status = api->SetSessionGraphOptimizationLevel(ctx->opts, ORT_ENABLE_ALL);
  if (status) api->ReleaseStatus(status);

  /* Create session (loads model) */
  status = api->CreateSession(ctx->env, model_path, ctx->opts, &ctx->session);
  if (status) {
    nash_log("[onnx-embed] CreateSession failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto fail;
  }

  /* Create CPU memory info */
  status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                    &ctx->mem_info);
  if (status) {
    nash_log("[onnx-embed] CreateCpuMemoryInfo failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto fail;
  }

  /* Detect output dimension by running a test inference */
  int test_dim = 0;
  float *test = onnx_embed_text(ctx, "test", &test_dim);
  if (test) {
    ctx->dim = test_dim;
    free(test);
    nash_log("[onnx-embed] initialized: %s (dim=%d)",
             model_path, ctx->dim);
  } else {
    nash_log("[onnx-embed] test inference failed");
    goto fail;
  }

  return ctx;

fail:
  onnx_embed_free(ctx);
  return NULL;
}

float *onnx_embed_text(onnx_embed_ctx_t *ctx, const char *text, int *out_dim) {
  if (!ctx || !text || !ctx->session) return NULL;
  if (out_dim) *out_dim = 0;

  const OrtApi *api = ctx->api;
  OrtStatus *status = NULL;

  /* Tokenize */
  int64_t input_ids[WP_MAX_TOKENS];
  int64_t attention_mask[WP_MAX_TOKENS];
  int64_t token_type_ids[WP_MAX_TOKENS];
  memset(input_ids, 0, sizeof(input_ids));
  memset(attention_mask, 0, sizeof(attention_mask));
  memset(token_type_ids, 0, sizeof(token_type_ids));

  int n_tokens = wp_tokenize(ctx->vocab, text,
                             input_ids, attention_mask, token_type_ids,
                             WP_MAX_TOKENS);
  if (n_tokens <= 0) return NULL;

  if (n_tokens > ONNX_MODEL_MAX_TOKENS) {
    nash_log("[onnx-embed] clamping %d tokens to model max %d",
             n_tokens, ONNX_MODEL_MAX_TOKENS);
    /* Zero out attention beyond the trained context window so mean
         * pooling ignores positions with untrained positional embeddings. */
    for (int i = ONNX_MODEL_MAX_TOKENS; i < n_tokens; i++)
      attention_mask[i] = 0;
    n_tokens = ONNX_MODEL_MAX_TOKENS;
  }

  /* Create input tensors */
  int64_t shape[2] = {1, (int64_t)n_tokens};
  size_t data_len = sizeof(int64_t) * (size_t)n_tokens;

  OrtValue *input_tensors[3] = {NULL, NULL, NULL};

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, input_ids, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[0]);
  if (status) {
    api->ReleaseStatus(status);
    goto cleanup;
  }

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, attention_mask, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[1]);
  if (status) {
    api->ReleaseStatus(status);
    goto cleanup;
  }

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, token_type_ids, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[2]);
  if (status) {
    api->ReleaseStatus(status);
    goto cleanup;
  }

  /* Run inference */
  const char *input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
  const char *output_names[] = {"last_hidden_state"};
  OrtValue *output_tensor = NULL;

  status = api->Run(ctx->session, NULL,
                    input_names, (const OrtValue *const *)input_tensors, 3,
                    output_names, 1, &output_tensor);
  if (status) {
    nash_log("[onnx-embed] Run failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto cleanup;
  }

  /* Extract output: [1, n_tokens, hidden_dim] */
  float *output_data = NULL;
  status = api->GetTensorMutableData(output_tensor, (void **)&output_data);
  if (status || !output_data) {
    if (status) api->ReleaseStatus(status);
    goto cleanup;
  }

  /* Get output shape to determine hidden dimension */
  OrtTensorTypeAndShapeInfo *type_info = NULL;
  status = api->GetTensorTypeAndShape(output_tensor, &type_info);
  if (status) {
    api->ReleaseStatus(status);
    goto cleanup;
  }

  size_t dim_count = 0;
  status = api->GetDimensionsCount(type_info, &dim_count);
  if (status) {
    api->ReleaseStatus(status);
    api->ReleaseTensorTypeAndShapeInfo(type_info);
    goto cleanup;
  }

  int64_t dims[4] = {0};
  if (dim_count > 0 && dim_count <= 4) {
    status = api->GetDimensions(type_info, dims, dim_count);
    if (status) {
      api->ReleaseStatus(status);
      api->ReleaseTensorTypeAndShapeInfo(type_info);
      goto cleanup;
    }
  }
  api->ReleaseTensorTypeAndShapeInfo(type_info);

  if (dim_count != 3) {
    nash_log("[onnx-embed] unexpected output rank: %zu", dim_count);
    goto cleanup;
  }

  int hidden_dim = (int)dims[2];
  if (hidden_dim <= 0 || hidden_dim > 4096) {
    nash_log("[onnx-embed] unexpected hidden dim: %d", hidden_dim);
    goto cleanup;
  }

  /* Mean pooling: average over token positions, weighted by attention_mask */
  float *embedding = xcalloc((size_t)hidden_dim, sizeof(float));

  float mask_sum = 0.0f;
  for (int t = 0; t < n_tokens; t++) {
    if (attention_mask[t]) {
      mask_sum += 1.0f;
      for (int d = 0; d < hidden_dim; d++) {
        embedding[d] += output_data[t * hidden_dim + d];
      }
    }
  }

  if (mask_sum > 0.0f) {
    for (int d = 0; d < hidden_dim; d++) {
      embedding[d] /= mask_sum;
    }
  }

  /* L2 normalize (sentence-transformers convention) */
  float norm = 0.0f;
  for (int d = 0; d < hidden_dim; d++) {
    norm += embedding[d] * embedding[d];
  }
  norm = sqrtf(norm);
  if (norm > 1e-12f) {
    for (int d = 0; d < hidden_dim; d++) {
      embedding[d] /= norm;
    }
  }

  if (out_dim) *out_dim = hidden_dim;

  /* Cleanup tensors */
  for (int i = 0; i < 3; i++) {
    if (input_tensors[i]) api->ReleaseValue(input_tensors[i]);
  }
  if (output_tensor) api->ReleaseValue(output_tensor);

  return embedding;

cleanup:
  for (int i = 0; i < 3; i++) {
    if (input_tensors[i]) api->ReleaseValue(input_tensors[i]);
  }
  if (output_tensor) api->ReleaseValue(output_tensor);
  return NULL;
}

/* ---- ONNX batch embedding ------------------------------------------------
 * Embed N texts in a single ONNX Runtime Run() call.
 * Tokenizes all texts, pads to the longest sequence, creates batched
 * tensors with shape {N, max_seq_len}, runs one inference, then
 * mean-pools + L2-normalizes each row independently.
 *
 * Processes up to ONNX_BATCH_MAX texts at a time.  If n_texts exceeds
 * that, falls back to sequential onnx_embed_text() for the overflow
 * (caller can chunk externally for larger sets).
 *
 * Returns heap-allocated array of n_texts float* pointers (each a
 * malloc'd dim-float vector).  Sets *out_dim to the hidden dimension.
 * Individual entries may be NULL if tokenization failed for that text.
 * Caller must free each non-NULL entry and the array itself.
 * Returns NULL on total failure. */

#define ONNX_BATCH_MAX 32

float **onnx_embed_text_batch(onnx_embed_ctx_t *ctx, const char **texts,
                              int n_texts, int *out_dim) {
  if (!ctx || !texts || n_texts <= 0 || !ctx->session) return NULL;
  if (out_dim) *out_dim = 0;

  /* Clamp to batch max - caller handles overflow */
  if (n_texts > ONNX_BATCH_MAX) n_texts = ONNX_BATCH_MAX;

  const OrtApi *api = ctx->api;
  OrtStatus *status = NULL;

  /* Allocate tokenization buffers: n_texts * WP_MAX_TOKENS each */
  size_t buf_elems = (size_t)n_texts * WP_MAX_TOKENS;
  int64_t *all_input_ids = xcalloc(buf_elems, sizeof(int64_t));
  int64_t *all_attention_mask = xcalloc(buf_elems, sizeof(int64_t));
  int64_t *all_token_type_ids = xcalloc(buf_elems, sizeof(int64_t));
  int *token_counts = xcalloc((size_t)n_texts, sizeof(int));

  if (!all_input_ids || !all_attention_mask || !all_token_type_ids || !token_counts) {
    free(all_input_ids);
    free(all_attention_mask);
    free(all_token_type_ids);
    free(token_counts);
    return NULL;
  }

  /* Tokenize all texts and track max sequence length */
  int max_tokens = 0;
  for (int i = 0; i < n_texts; i++) {
    int offset = i * WP_MAX_TOKENS;
    if (!texts[i]) {
      token_counts[i] = 0;
      continue;
    }

    int n = wp_tokenize(ctx->vocab, texts[i],
                        all_input_ids + offset,
                        all_attention_mask + offset,
                        all_token_type_ids + offset,
                        WP_MAX_TOKENS);
    if (n <= 0) {
      token_counts[i] = 0;
      continue;
    }

    /* Clamp to model max like single-text path */
    if (n > ONNX_MODEL_MAX_TOKENS) {
      for (int t = ONNX_MODEL_MAX_TOKENS; t < n; t++)
        all_attention_mask[offset + t] = 0;
      n = ONNX_MODEL_MAX_TOKENS;
    }
    token_counts[i] = n;
    if (n > max_tokens) max_tokens = n;
  }

  if (max_tokens <= 0) {
    free(all_input_ids);
    free(all_attention_mask);
    free(all_token_type_ids);
    free(token_counts);
    return NULL;
  }

  /* Build compact padded buffers: shape {n_texts, max_tokens} */
  size_t compact_elems = (size_t)n_texts * (size_t)max_tokens;
  int64_t *compact_ids = xcalloc(compact_elems, sizeof(int64_t));
  int64_t *compact_mask = xcalloc(compact_elems, sizeof(int64_t));
  int64_t *compact_type = xcalloc(compact_elems, sizeof(int64_t));

  if (!compact_ids || !compact_mask || !compact_type) {
    free(all_input_ids);
    free(all_attention_mask);
    free(all_token_type_ids);
    free(token_counts);
    free(compact_ids);
    free(compact_mask);
    free(compact_type);
    return NULL;
  }

  for (int i = 0; i < n_texts; i++) {
    int src_off = i * WP_MAX_TOKENS;
    int dst_off = i * max_tokens;
    int n = token_counts[i];
    if (n > max_tokens) n = max_tokens;
    /* Copy actual tokens; rest stays zero (PAD) from calloc */
    memcpy(compact_ids + dst_off, all_input_ids + src_off, (size_t)n * sizeof(int64_t));
    memcpy(compact_mask + dst_off, all_attention_mask + src_off, (size_t)n * sizeof(int64_t));
    memcpy(compact_type + dst_off, all_token_type_ids + src_off, (size_t)n * sizeof(int64_t));
  }

  /* Done with wide buffers */
  free(all_input_ids);
  free(all_attention_mask);
  free(all_token_type_ids);

  /* Create batched input tensors */
  int64_t shape[2] = {(int64_t)n_texts, (int64_t)max_tokens};
  size_t data_len = compact_elems * sizeof(int64_t);

  OrtValue *input_tensors[3] = {NULL, NULL, NULL};
  OrtValue *output_tensor = NULL;
  float **results = NULL;

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, compact_ids, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[0]);
  if (status) {
    api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, compact_mask, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[1]);
  if (status) {
    api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  status = api->CreateTensorWithDataAsOrtValue(
    ctx->mem_info, compact_type, data_len, shape, 2,
    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[2]);
  if (status) {
    api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  /* Run batched inference */
  const char *input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
  const char *output_names[] = {"last_hidden_state"};

  status = api->Run(ctx->session, NULL,
                    input_names, (const OrtValue *const *)input_tensors, 3,
                    output_names, 1, &output_tensor);
  if (status) {
    nash_log("[onnx-embed] batch Run failed: %s",
             api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  /* Extract output shape: expect [n_texts, max_tokens, hidden_dim] */
  float *output_data = NULL;
  status = api->GetTensorMutableData(output_tensor, (void **)&output_data);
  if (status || !output_data) {
    if (status) api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  OrtTensorTypeAndShapeInfo *type_info = NULL;
  status = api->GetTensorTypeAndShape(output_tensor, &type_info);
  if (status) {
    api->ReleaseStatus(status);
    goto batch_cleanup;
  }

  size_t dim_count = 0;
  status = api->GetDimensionsCount(type_info, &dim_count);
  if (status) {
    api->ReleaseStatus(status);
    api->ReleaseTensorTypeAndShapeInfo(type_info);
    goto batch_cleanup;
  }
  int64_t dims[4] = {0};
  if (dim_count >= 3 && dim_count <= 4) {
    status = api->GetDimensions(type_info, dims, dim_count);
    if (status) {
      api->ReleaseStatus(status);
      api->ReleaseTensorTypeAndShapeInfo(type_info);
      goto batch_cleanup;
    }
  }
  api->ReleaseTensorTypeAndShapeInfo(type_info);

  if (dim_count != 3 || dims[0] != n_texts) {
    nash_log("[onnx-embed] batch: unexpected output shape: "
             "rank=%zu dims=[%lld,%lld,%lld]",
             dim_count, (long long)dims[0],
             (long long)dims[1], (long long)dims[2]);
    goto batch_cleanup;
  }

  int hidden_dim = (int)dims[2];
  if (hidden_dim <= 0 || hidden_dim > 4096) {
    nash_log("[onnx-embed] batch: unexpected hidden dim: %d", hidden_dim);
    goto batch_cleanup;
  }

  if (out_dim) *out_dim = hidden_dim;

  /* Mean pool + L2 normalize each row independently */
  results = xcalloc((size_t)n_texts, sizeof(float *));

  for (int i = 0; i < n_texts; i++) {
    if (token_counts[i] <= 0) continue; /* no tokens - leave NULL */

    float *emb = xcalloc((size_t)hidden_dim, sizeof(float));
    if (!emb) continue;

    /* Mean pooling over attended tokens for row i */
    int row_offset = i * max_tokens;
    float *row_data = output_data + (size_t)i * (size_t)max_tokens * (size_t)hidden_dim;
    float mask_sum = 0.0f;

    for (int t = 0; t < token_counts[i]; t++) {
      if (compact_mask[row_offset + t]) {
        mask_sum += 1.0f;
        for (int d = 0; d < hidden_dim; d++)
          emb[d] += row_data[t * hidden_dim + d];
      }
    }

    if (mask_sum > 0.0f) {
      for (int d = 0; d < hidden_dim; d++)
        emb[d] /= mask_sum;
    }

    /* L2 normalize */
    float norm = 0.0f;
    for (int d = 0; d < hidden_dim; d++)
      norm += emb[d] * emb[d];
    norm = sqrtf(norm);
    if (norm > 1e-12f) {
      for (int d = 0; d < hidden_dim; d++)
        emb[d] /= norm;
    }

    results[i] = emb;
  }

  /* Cleanup tensors (success path) */
  for (int i = 0; i < 3; i++)
    if (input_tensors[i]) api->ReleaseValue(input_tensors[i]);
  if (output_tensor) api->ReleaseValue(output_tensor);
  free(compact_ids);
  free(compact_mask);
  free(compact_type);
  free(token_counts);
  return results;

batch_cleanup:
  for (int i = 0; i < 3; i++)
    if (input_tensors[i]) api->ReleaseValue(input_tensors[i]);
  if (output_tensor) api->ReleaseValue(output_tensor);
  free(compact_ids);
  free(compact_mask);
  free(compact_type);
  free(token_counts);
  if (results) {
    for (int i = 0; i < n_texts; i++)
      free(results[i]);
    free(results);
  }
  return NULL;
}

void onnx_embed_free(onnx_embed_ctx_t *ctx) {
  if (!ctx) return;
  if (ctx->api) {
    if (ctx->mem_info) ctx->api->ReleaseMemoryInfo(ctx->mem_info);
    if (ctx->session) ctx->api->ReleaseSession(ctx->session);
    if (ctx->opts) ctx->api->ReleaseSessionOptions(ctx->opts);
    if (ctx->env) ctx->api->ReleaseEnv(ctx->env);
  }
  wp_vocab_free(ctx->vocab);
  free(ctx);
}

int onnx_embed_dim(const onnx_embed_ctx_t *ctx) {
  return ctx ? ctx->dim : 0;
}

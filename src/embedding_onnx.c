#define _GNU_SOURCE
#include "embedding_onnx.h"
#include "onnxruntime_c_api.h"
#include "tui.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ── WordPiece tokenizer ─────────────────────────────── */

#define WP_MAX_TOKENS   512   /* BERT max sequence length */
#define WP_MAX_WORD_LEN 200   /* max chars per word before giving up */
#define WP_UNK_ID       100   /* [UNK] token ID */
#define WP_CLS_ID       101   /* [CLS] token ID */
#define WP_SEP_ID       102   /* [SEP] token ID */
#define WP_PAD_ID       0     /* [PAD] token ID */

/* Hash table entry for vocab lookup */
typedef struct wp_entry {
    char *token;
    int   id;
    struct wp_entry *next;
} wp_entry_t;

#define WP_HASH_SIZE 65536

typedef struct {
    wp_entry_t *buckets[WP_HASH_SIZE];
    int         vocab_size;
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
    wp_entry_t *e = malloc(sizeof(*e));
    if (!e) return;
    e->token = strdup(token);
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

    wp_vocab_t *v = calloc(1, sizeof(*v));
    if (!v) { fclose(f); return NULL; }

    char line[1024];
    int id = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline/whitespace */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t'))
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
 * Returns total number of tokens. */
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
    while (*p && pos < max_len - 1) {  /* -1 for final [SEP] */
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
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
    const OrtApi     *api;
    OrtEnv           *env;
    OrtSessionOptions *opts;
    OrtSession       *session;
    OrtMemoryInfo    *mem_info;
    wp_vocab_t       *vocab;
    int               dim;        /* embedding dimension (384 for MiniLM) */
};

onnx_embed_ctx_t *onnx_embed_init(const char *model_dir) {
    if (!model_dir) return NULL;

    /* Build paths */
    char model_path[4096], vocab_path[4096];
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

    /* Get ONNX Runtime API */
    const OrtApiBase *base = OrtGetApiBase();
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    if (!api) {
        nash_log("[onnx-embed] failed to get ONNX Runtime API v%d",
                ORT_API_VERSION);
        wp_vocab_free(vocab);
        return NULL;
    }

    onnx_embed_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { wp_vocab_free(vocab); return NULL; }
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

    /* Use 2 threads for inference (embeddings are small) */
    status = api->SetIntraOpNumThreads(ctx->opts, 2); if (status) api->ReleaseStatus(status);
    status = api->SetSessionGraphOptimizationLevel(ctx->opts, ORT_ENABLE_ALL); if (status) api->ReleaseStatus(status);

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

    /* Create input tensors */
    int64_t shape[2] = {1, (int64_t)n_tokens};
    size_t data_len = sizeof(int64_t) * (size_t)n_tokens;

    OrtValue *input_tensors[3] = {NULL, NULL, NULL};

    status = api->CreateTensorWithDataAsOrtValue(
        ctx->mem_info, input_ids, data_len, shape, 2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[0]);
    if (status) { api->ReleaseStatus(status); goto cleanup; }

    status = api->CreateTensorWithDataAsOrtValue(
        ctx->mem_info, attention_mask, data_len, shape, 2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[1]);
    if (status) { api->ReleaseStatus(status); goto cleanup; }

    status = api->CreateTensorWithDataAsOrtValue(
        ctx->mem_info, token_type_ids, data_len, shape, 2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[2]);
    if (status) { api->ReleaseStatus(status); goto cleanup; }

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
    if (status) { api->ReleaseStatus(status); goto cleanup; }

    size_t dim_count = 0;
    status = api->GetDimensionsCount(type_info, &dim_count); if (status) { api->ReleaseStatus(status); goto cleanup; }

    int64_t dims[4] = {0};
    if (dim_count > 0 && dim_count <= 4) {
        status = api->GetDimensions(type_info, dims, dim_count); if (status) { api->ReleaseStatus(status); goto cleanup; }
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
    float *embedding = calloc((size_t)hidden_dim, sizeof(float));
    if (!embedding) goto cleanup;

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

void onnx_embed_free(onnx_embed_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->api) {
        if (ctx->mem_info) ctx->api->ReleaseMemoryInfo(ctx->mem_info);
        if (ctx->session)  ctx->api->ReleaseSession(ctx->session);
        if (ctx->opts)     ctx->api->ReleaseSessionOptions(ctx->opts);
        if (ctx->env)      ctx->api->ReleaseEnv(ctx->env);
    }
    wp_vocab_free(ctx->vocab);
    free(ctx);
}

int onnx_embed_dim(const onnx_embed_ctx_t *ctx) {
    return ctx ? ctx->dim : 0;
}

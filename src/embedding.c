#define _GNU_SOURCE
#include "embedding.h"
#include "embedding_onnx.h"
#include "cJSON.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>

/* ── curl write callback ─────────────────────────────── */
/* Now uses str_t from str.h — see str_write_cb in str.c */

/* ── lifecycle ───────────────────────────────────────── */

embed_ctx_t *embed_new(const embed_config_t *cfg) {
    if (!cfg) return NULL;
    embed_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->cfg.type = cfg->type;
    ctx->cfg.model = cfg->model ? strdup(cfg->model) : NULL;
    ctx->cfg.api_base = cfg->api_base ? strdup(cfg->api_base) : NULL;
    ctx->cfg.model_path = cfg->model_path ? strdup(cfg->model_path) : NULL;
    ctx->cfg.dimension = cfg->dimension;
    ctx->cfg.max_input_chars = cfg->max_input_chars;
    ctx->available = 0;
    ctx->detected_dim = 0;
    ctx->onnx = NULL;
    ctx->owns_onnx = 1;  /* new contexts own their ONNX session */

    return ctx;
}

void embed_free(embed_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->onnx && ctx->owns_onnx) onnx_embed_free(ctx->onnx);
    free(ctx->cfg.model);
    free(ctx->cfg.api_base);
    free(ctx->cfg.model_path);
    free(ctx);
}

embed_ctx_t *embed_share(embed_ctx_t *src) {
    if (!src) return NULL;
    embed_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->cfg.type = src->cfg.type;
    ctx->cfg.model = src->cfg.model ? strdup(src->cfg.model) : NULL;
    ctx->cfg.api_base = src->cfg.api_base ? strdup(src->cfg.api_base) : NULL;
    ctx->cfg.model_path = src->cfg.model_path ? strdup(src->cfg.model_path) : NULL;
    ctx->cfg.dimension = src->cfg.dimension;
    ctx->cfg.max_input_chars = src->cfg.max_input_chars;
    ctx->available = src->available;
    ctx->detected_dim = src->detected_dim;
    ctx->onnx = src->onnx;    /* share the ONNX session */
    ctx->owns_onnx = 0;       /* do NOT free it on embed_free */

    return ctx;
}

/* ── Model context window lookup ─────────────────────── */

/* Known embedding models and their max input tokens.
 * Tokens are converted to chars using ~4 chars/token.
 * Table is searched by substring match on model name so that
 * versioned names (e.g. "nomic-embed-text:v1.5") still match. */
static const struct {
    const char *pattern;   /* substring to match in model name */
    int         max_tokens;
} embed_model_limits[] = {
    /* Large context models (8K tokens) */
    { "nomic-embed",             8192 },
    { "text-embedding-3",        8191 },
    { "text-embedding-ada",      8191 },
    { "jina-embeddings-v3",      8192 },
    { "jina-embeddings-v2",      8192 },
    /* Medium context models (2K tokens) */
    { "snowflake-arctic-embed",  2048 },
    { "mxbai-embed",              512 },
    { "bge-large",                512 },
    { "bge-base",                 512 },
    { "bge-small",                512 },
    { "bge-m3",                  8192 },
    { "e5-large",                 512 },
    { "e5-base",                  512 },
    { "e5-small",                 512 },
    { "e5-mistral",              4096 },
    { "gte-large",                512 },
    { "gte-base",                 512 },
    { "gte-small",                512 },
    { "gte-Qwen",                8192 },
    /* Small context models (256 tokens) */
    { "all-MiniLM",               256 },
    { "all-minilm",               256 },
    { "all-mpnet",                384 },
    { "paraphrase-",              128 },
    { NULL, 0 }
};

#define EMBED_DEFAULT_CHARS_PER_TOKEN 4
#define EMBED_DEFAULT_MAX_CHARS       2000  /* ~500 tokens, safe for most models */

int embed_max_input_chars(const embed_ctx_t *ctx) {
    if (!ctx) return EMBED_DEFAULT_MAX_CHARS;

    /* Explicit override from config takes priority */
    if (ctx->cfg.max_input_chars > 0)
        return ctx->cfg.max_input_chars;

    /* ONNX backend: typically all-MiniLM-L6-v2 with 256 tokens */
    if (ctx->cfg.type == EMBED_ONNX)
        return 256 * EMBED_DEFAULT_CHARS_PER_TOKEN;  /* 1024 chars */

    /* Look up model name in known models table */
    const char *model = ctx->cfg.model;
    if (model) {
        for (int i = 0; embed_model_limits[i].pattern; i++) {
            if (strstr(model, embed_model_limits[i].pattern)) {
                return embed_model_limits[i].max_tokens
                     * EMBED_DEFAULT_CHARS_PER_TOKEN;
            }
        }
    }

    return EMBED_DEFAULT_MAX_CHARS;
}

int embed_count_tokens(const embed_ctx_t *ctx, const char *text) {
    if (!ctx || !text) return -1;

    switch (ctx->cfg.type) {
    case EMBED_ONNX:
        if (ctx->onnx)
            return onnx_count_tokens(ctx->onnx, text);
        return -1;
    case EMBED_OLLAMA:
    case EMBED_OPENAI: {
        /* No real tokenizer for API backends - estimate */
        int len = (int)strlen(text);
        return len / EMBED_DEFAULT_CHARS_PER_TOKEN;
    }
    case EMBED_NONE:
    default:
        return -1;
    }
}

/* ── API calls ───────────────────────────────────────── */

/* Build URL for embedding API endpoint */
static char *build_url(embed_ctx_t *ctx) {
    if (!ctx || !ctx->cfg.api_base) return NULL;

    char url[2048];
    switch (ctx->cfg.type) {
    case EMBED_OLLAMA:
        /* Ollama: POST /api/embed */
        snprintf(url, sizeof(url), "%s/api/embed", ctx->cfg.api_base);
        break;
    case EMBED_OPENAI:
        /* OpenAI-compatible: POST /v1/embeddings */
        snprintf(url, sizeof(url), "%s/v1/embeddings", ctx->cfg.api_base);
        break;
    default:
        return NULL;
    }
    return strdup(url);
}

/* Build request JSON body */
static char *build_request_json(embed_ctx_t *ctx, const char *text) {
    if (!ctx || !text) return NULL;

    cJSON *req = cJSON_CreateObject();
    if (!req) return NULL;

    switch (ctx->cfg.type) {
    case EMBED_OLLAMA:
        /* Ollama format: {"model": "...", "input": "..."} */
        cJSON_AddStringToObject(req, "model",
                                ctx->cfg.model ? ctx->cfg.model : "nomic-embed-text");
        cJSON_AddStringToObject(req, "input", text);
        break;
    case EMBED_OPENAI:
        /* OpenAI format: {"model": "...", "input": "..."} */
        cJSON_AddStringToObject(req, "model",
                                ctx->cfg.model ? ctx->cfg.model : "text-embedding-3-small");
        cJSON_AddStringToObject(req, "input", text);
        break;
    default:
        cJSON_Delete(req);
        return NULL;
    }

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* Parse embedding vector from API response JSON */
static embed_vec_t parse_response(embed_ctx_t *ctx, const char *response) {
    embed_vec_t result = {0};
    if (!ctx || !response) return result;

    cJSON *root = cJSON_Parse(response);
    if (!root) return result;

    cJSON *embeddings = NULL;

    switch (ctx->cfg.type) {
    case EMBED_OLLAMA:
        /* Ollama response: {"embeddings": [[0.1, 0.2, ...]]} */
        embeddings = cJSON_GetObjectItem(root, "embeddings");
        if (embeddings && cJSON_IsArray(embeddings)) {
            cJSON *first = cJSON_GetArrayItem(embeddings, 0);
            if (first && cJSON_IsArray(first)) {
                int dim = cJSON_GetArraySize(first);
                if (dim > 0) {
                    result.data = malloc(sizeof(float) * (size_t)dim);
                    if (result.data) {
                        result.dim = dim;
                        for (int i = 0; i < dim; i++) {
                            cJSON *v = cJSON_GetArrayItem(first, i);
                            result.data[i] = v ? (float)cJSON_GetNumberValue(v) : 0.0f;
                        }
                    }
                }
            }
        }
        break;

    case EMBED_OPENAI:
        /* OpenAI response: {"data": [{"embedding": [0.1, 0.2, ...]}]} */
        {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            if (data && cJSON_IsArray(data)) {
                cJSON *first = cJSON_GetArrayItem(data, 0);
                if (first) {
                    embeddings = cJSON_GetObjectItem(first, "embedding");
                    if (embeddings && cJSON_IsArray(embeddings)) {
                        int dim = cJSON_GetArraySize(embeddings);
                        if (dim > 0) {
                            result.data = malloc(sizeof(float) * (size_t)dim);
                            if (result.data) {
                                result.dim = dim;
                                for (int i = 0; i < dim; i++) {
                                    cJSON *v = cJSON_GetArrayItem(embeddings, i);
                                    result.data[i] = v ? (float)cJSON_GetNumberValue(v) : 0.0f;
                                }
                            }
                        }
                    }
                }
            }
        }
        break;

    default:
        break;
    }

    cJSON_Delete(root);
    return result;
}

/* Make HTTP POST request to embedding API */
static embed_vec_t call_api(embed_ctx_t *ctx, const char *text) {
    embed_vec_t result = {0};
    if (!ctx || !text || ctx->cfg.type == EMBED_NONE) return result;

    char *url = build_url(ctx);
    char *body = build_request_json(ctx, text);
    if (!url || !body) {
        free(url);
        free(body);
        return result;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t response = str_new(8192);
    int rc = http_post(url, body, headers, 30, &response);

    if (rc == 0 && response.len > 0)
        result = parse_response(ctx, response.data);

    str_free(&response);
    curl_slist_free_all(headers);
    free(url);
    free(body);

    return result;
}

/* ── Batch API support (FIX D7) ──────────────────────── */
/* Both Ollama and OpenAI support array inputs in a single API call,
 * reducing N HTTP round-trips to 1 for memory_embed_all(). */

/* Build batch request JSON body with array input */
static char *build_request_json_batch(embed_ctx_t *ctx, const char **texts,
                                       int n_texts) {
    if (!ctx || !texts || n_texts <= 0) return NULL;

    cJSON *req = cJSON_CreateObject();
    if (!req) return NULL;

    const char *model_name = NULL;
    switch (ctx->cfg.type) {
    case EMBED_OLLAMA:
        model_name = ctx->cfg.model ? ctx->cfg.model : "nomic-embed-text";
        break;
    case EMBED_OPENAI:
        model_name = ctx->cfg.model ? ctx->cfg.model : "text-embedding-3-small";
        break;
    default:
        cJSON_Delete(req);
        return NULL;
    }
    cJSON_AddStringToObject(req, "model", model_name);

    /* Both APIs accept "input" as an array of strings */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n_texts; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(texts[i] ? texts[i] : ""));
    }
    cJSON_AddItemToObject(req, "input", arr);

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* Parse batch response — returns array of embed_vec_t (caller frees each + array) */
static embed_vec_t *parse_response_batch(embed_ctx_t *ctx, const char *response,
                                          int n_expected, int *out_count) {
    *out_count = 0;
    if (!ctx || !response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) return NULL;

    embed_vec_t *results = calloc((size_t)n_expected, sizeof(embed_vec_t));
    if (!results) { cJSON_Delete(root); return NULL; }

    switch (ctx->cfg.type) {
    case EMBED_OLLAMA: {
        /* Ollama batch response: {"embeddings": [[...], [...], ...]} */
        cJSON *embeddings = cJSON_GetObjectItem(root, "embeddings");
        if (embeddings && cJSON_IsArray(embeddings)) {
            int n = cJSON_GetArraySize(embeddings);
            if (n > n_expected) n = n_expected;
            for (int i = 0; i < n; i++) {
                cJSON *vec = cJSON_GetArrayItem(embeddings, i);
                if (vec && cJSON_IsArray(vec)) {
                    int dim = cJSON_GetArraySize(vec);
                    if (dim > 0) {
                        results[i].data = malloc(sizeof(float) * (size_t)dim);
                        results[i].dim = dim;
                        if (results[i].data) {
                            for (int j = 0; j < dim; j++) {
                                cJSON *v = cJSON_GetArrayItem(vec, j);
                                results[i].data[j] = v ? (float)cJSON_GetNumberValue(v) : 0.0f;
                            }
                        }
                    }
                }
            }
            *out_count = n;
        }
        break;
    }
    case EMBED_OPENAI: {
        /* OpenAI batch response: {"data": [{"embedding": [...], "index": 0}, ...]}
         * Note: OpenAI may return results out of order — use "index" field. */
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (data && cJSON_IsArray(data)) {
            int n = cJSON_GetArraySize(data);
            if (n > n_expected) n = n_expected;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(data, i);
                if (!item) continue;
                /* Use "index" field for correct ordering */
                int idx = i;
                cJSON *idx_j = cJSON_GetObjectItem(item, "index");
                if (idx_j) idx = (int)cJSON_GetNumberValue(idx_j);
                if (idx < 0 || idx >= n_expected) continue;

                cJSON *emb = cJSON_GetObjectItem(item, "embedding");
                if (emb && cJSON_IsArray(emb)) {
                    int dim = cJSON_GetArraySize(emb);
                    if (dim > 0) {
                        results[idx].data = malloc(sizeof(float) * (size_t)dim);
                        results[idx].dim = dim;
                        if (results[idx].data) {
                            for (int j = 0; j < dim; j++) {
                                cJSON *v = cJSON_GetArrayItem(emb, j);
                                results[idx].data[j] = v ? (float)cJSON_GetNumberValue(v) : 0.0f;
                            }
                        }
                    }
                }
            }
            *out_count = n;
        }
        break;
    }
    default:
        break;
    }

    cJSON_Delete(root);
    return results;
}

/* Make batch HTTP POST request to embedding API.
 * Sends all texts in a single API call. Returns array of embed_vec_t.
 * Caller must free each vec with embed_vec_free() and the array with free(). */
static embed_vec_t *call_api_batch(embed_ctx_t *ctx, const char **texts,
                                    int n_texts, int *out_count) {
    *out_count = 0;
    if (!ctx || !texts || n_texts <= 0 || ctx->cfg.type == EMBED_NONE) return NULL;

    char *url = build_url(ctx);
    char *body = build_request_json_batch(ctx, texts, n_texts);
    if (!url || !body) {
        free(url);
        free(body);
        return NULL;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t response = str_new(16384);
    /* Longer timeout for batch requests — may have many texts */
    int rc = http_post(url, body, headers, 120, &response);

    embed_vec_t *results = NULL;
    if (rc == 0 && response.len > 0)
        results = parse_response_batch(ctx, response.data, n_texts, out_count);

    str_free(&response);
    curl_slist_free_all(headers);
    free(url);
    free(body);

    return results;
}

/* ── public API ──────────────────────────────────────── */

int embed_probe(embed_ctx_t *ctx) {
    if (!ctx || ctx->cfg.type == EMBED_NONE) {
        if (ctx) ctx->available = 0;
        return 0;
    }

    /* ONNX backend: initialize locally, no network probe needed */
    if (ctx->cfg.type == EMBED_ONNX) {
        if (!ctx->cfg.model_path) {
            nash_log("[embed] ONNX: no model_path configured");
            ctx->available = 0;
            return 0;
        }
        ctx->onnx = onnx_embed_init(ctx->cfg.model_path);
        if (ctx->onnx) {
            ctx->available = 1;
            ctx->detected_dim = onnx_embed_dim(ctx->onnx);
            if (ctx->cfg.dimension == 0)
                ctx->cfg.dimension = ctx->detected_dim;
            return 1;
        }
        ctx->available = 0;
        return 0;
    }

    /* HTTP backends: send a minimal test embedding */
    embed_vec_t test = call_api(ctx, "test");
    if (test.data && test.dim > 0) {
        ctx->available = 1;
        ctx->detected_dim = test.dim;
        if (ctx->cfg.dimension == 0)
            ctx->cfg.dimension = test.dim;
        embed_vec_free(&test);
        return 1;
    }

    ctx->available = 0;
    embed_vec_free(&test);
    return 0;
}

embed_vec_t embed_text(embed_ctx_t *ctx, const char *text) {
    embed_vec_t result = {0};
    if (!ctx || !text || !ctx->available) return result;

    /* ONNX backend: local inference */
    if (ctx->cfg.type == EMBED_ONNX && ctx->onnx) {
        int dim = 0;
        float *data = onnx_embed_text(ctx->onnx, text, &dim);
        if (data && dim > 0) {
            result.data = data;
            result.dim = dim;
        }
        return result;
    }

    /* HTTP backends */
    return call_api(ctx, text);
}

void embed_vec_free(embed_vec_t *v) {
    if (!v) return;
    free(v->data);
    v->data = NULL;
    v->dim = 0;
}

/* ── vector operations ───────────────────────────────── */

/* Core cosine similarity between two raw float vectors of same dimension */
static float cosine_sim_raw(const float *a, const float *b, int dim) {
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (int i = 0; i < dim; i++) {
        dot    += (double)a[i] * (double)b[i];
        norm_a += (double)a[i] * (double)a[i];
        norm_b += (double)b[i] * (double)b[i];
    }
    double denom = sqrt(norm_a) * sqrt(norm_b);
    if (denom < 1e-12) return 0.0f;
    return (float)(dot / denom);
}

float embed_cosine_sim(const embed_vec_t *a, const embed_vec_t *b) {
    if (!a || !b || !a->data || !b->data) return 0.0f;
    if (a->dim != b->dim || a->dim == 0) return 0.0f;
    return cosine_sim_raw(a->data, b->data, a->dim);
}

/* ── persistence ─────────────────────────────────────── */

int embed_vec_save(const embed_vec_t *v, const char *path) {
    if (!v || !v->data || v->dim <= 0 || !path) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* Header: int32 dimension */
    int32_t dim = (int32_t)v->dim;
    if (fwrite(&dim, sizeof(dim), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    /* Data: float32 × dimension */
    if (fwrite(v->data, sizeof(float), (size_t)v->dim, f) != (size_t)v->dim) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

embed_vec_t embed_vec_load(const char *path) {
    embed_vec_t result = {0};
    if (!path) return result;

    FILE *f = fopen(path, "rb");
    if (!f) return result;

    /* Read header: int32 dimension */
    int32_t dim = 0;
    if (fread(&dim, sizeof(dim), 1, f) != 1 || dim <= 0 || dim > 65536) {
        fclose(f);
        return result;
    }

    /* Read data: float32 × dimension */
    result.data = malloc(sizeof(float) * (size_t)dim);
    if (!result.data) {
        fclose(f);
        return result;
    }

    if (fread(result.data, sizeof(float), (size_t)dim, f) != (size_t)dim) {
        free(result.data);
        result.data = NULL;
        fclose(f);
        return result;
    }

    result.dim = (int)dim;
    fclose(f);
    return result;
}

/* ── batch embedding ─────────────────────────────────── */

embed_vec_t *embed_text_batch(embed_ctx_t *ctx, const char **texts,
                              int n_texts, int *out_count) {
    if (!ctx || !texts || n_texts <= 0 || !out_count) return NULL;
    *out_count = 0;

    /* FIX D7: Use native batch API for Ollama/OpenAI backends.
     * Both support array inputs in a single HTTP call, reducing
     * N round-trips to 1 (or ceil(N/BATCH_SIZE) for large sets).
     * ONNX backend falls through to sequential (no batch API). */
    if ((ctx->cfg.type == EMBED_OLLAMA || ctx->cfg.type == EMBED_OPENAI)
        && ctx->available) {
        /* Process in chunks of up to 64 texts to avoid oversized requests */
        const int BATCH_SIZE = 64;
        embed_vec_t *results = calloc((size_t)n_texts, sizeof(embed_vec_t));
        if (!results) return NULL;

        for (int offset = 0; offset < n_texts; offset += BATCH_SIZE) {
            int chunk = n_texts - offset;
            if (chunk > BATCH_SIZE) chunk = BATCH_SIZE;

            int chunk_count = 0;
            embed_vec_t *chunk_results = call_api_batch(ctx, texts + offset,
                                                        chunk, &chunk_count);
            if (chunk_results) {
                for (int i = 0; i < chunk && i < chunk_count; i++)
                    results[offset + i] = chunk_results[i];
                free(chunk_results);  /* array only - vecs moved to results */
            } else {
                /* Batch call failed - fall back to sequential for this chunk */
                nash_log("[embed] batch API failed for chunk %d-%d, "
                        "falling back to sequential", offset, offset + chunk);
                for (int i = 0; i < chunk; i++)
                    results[offset + i] = embed_text(ctx, texts[offset + i]);
            }
        }
        /* Return full array preserving positional correspondence with
         * input texts[].  Failed embeddings have .data == NULL (from
         * calloc).  Callers must check .data before use. */
        *out_count = n_texts;
        return results;
    }

    /* ONNX backend: use native batch inference for 2-3x speedup.
     * Process in chunks of up to 32 texts (ONNX_BATCH_MAX). */
    if (ctx->cfg.type == EMBED_ONNX && ctx->onnx) {
        embed_vec_t *results = calloc((size_t)n_texts, sizeof(embed_vec_t));
        if (!results) return NULL;

        for (int offset = 0; offset < n_texts; offset += 32) {
            int chunk = n_texts - offset;
            if (chunk > 32) chunk = 32;

            int batch_dim = 0;
            float **batch = onnx_embed_text_batch(ctx->onnx,
                                texts + offset, chunk, &batch_dim);
            if (batch && batch_dim > 0) {
                for (int i = 0; i < chunk; i++) {
                    if (batch[i]) {
                        results[offset + i].data = batch[i];
                        results[offset + i].dim = batch_dim;
                    }
                }
                free(batch);  /* array only - float* moved to results */
            } else {
                /* Batch failed - fall back to sequential for this chunk */
                for (int i = 0; i < chunk; i++)
                    results[offset + i] = embed_text(ctx, texts[offset + i]);
            }
        }

        *out_count = n_texts;
        return results;
    }

    /* Sequential fallback for other backends */
    embed_vec_t *results = calloc((size_t)n_texts, sizeof(embed_vec_t));
    if (!results) return NULL;

    for (int i = 0; i < n_texts; i++) {
        results[i] = embed_text(ctx, texts[i]);
    }

    /* Return full array preserving positional correspondence with
     * input texts[].  Failed embeddings have .data == NULL (from
     * calloc).  Callers must check .data before use. */
    *out_count = n_texts;
    return results;
}

/* ── text preparation ────────────────────────────────── */

char *embed_prepare_text(const char *key, const char *value, int max_chars) {
    if (!key && !value) return NULL;
    if (max_chars <= 0) max_chars = 2000;  /* default: ~500 tokens */

    str_t out = str_new((size_t)max_chars + 256);

    /* Key (most important for matching) */
    if (key) {
        str_append_cstr(&out, key);
        str_append_cstr(&out, "\n");
    }

    /* Value (truncated to fit max_chars) */
    if (value) {
        size_t remaining = (size_t)max_chars > out.len
                         ? (size_t)max_chars - out.len : 0;
        if (remaining > 0) {
            size_t vlen = strlen(value);
            if (vlen <= remaining) {
                str_append_cstr(&out, value);
            } else {
                /* Truncate at word boundary if possible.
                 * FIX B9: Guard against remaining < 100 — the subtraction
                 * remaining - 100 wraps around (size_t is unsigned),
                 * making the condition always false and disabling the
                 * word-boundary search.
                 * FIX UTF8: Use UTF-8 safe truncation so we never split
                 * a multi-byte character. First find the byte-level
                 * word-boundary cut, then clamp it to a valid UTF-8
                 * boundary. */
                size_t cut = remaining;
                if (remaining > 100) {
                    while (cut > remaining - 100 && cut > 0 && value[cut] != ' ')
                        cut--;
                }
                if (cut == 0) cut = remaining;
                /* Clamp cut to a valid UTF-8 character boundary */
                while (cut > 0 && ((unsigned char)value[cut] & 0xC0) == 0x80)
                    cut--;
                str_appendf(&out, "%.*s...", (int)cut, value);
            }
        }
    }

    return str_steal(&out);
}

/* ── chunked (multi-vector) embeddings ───────────────── */

/* Build the key prefix that starts every chunk.
 * full=1: "key\n", full=0: abbreviated "[key] " for subsequent chunks. */
static char *build_chunk_prefix(const char *key, int full) {
    str_t pfx = str_new(256);
    if (key) {
        if (full) {
            str_append_cstr(&pfx, key);
            str_append_cstr(&pfx, "\n");
        } else {
            str_append_cstr(&pfx, "[");
            str_append_cstr(&pfx, key);
            str_append_cstr(&pfx, "] ");
        }
    }
    return str_steal(&pfx);
}

/* ── Segment splitting for paragraph-aware chunking ──── */

/* A text segment: offset + length into the original value string. */
typedef struct {
    size_t off;
    size_t len;
} chunk_seg_t;

/* Split text into segments at paragraph boundaries (\n\n), then
 * sub-split oversized segments at \n, then ". ", then word boundary.
 * Returns segment count. Caller must free *out_segs. */
static int split_into_segments(const char *text, size_t text_len,
                               size_t max_seg_len, chunk_seg_t **out_segs) {
    if (!text || text_len == 0 || !out_segs) return 0;

    int cap = 32, count = 0;
    chunk_seg_t *segs = malloc(sizeof(chunk_seg_t) * (size_t)cap);
    if (!segs) return 0;

    /* Helper macro to add a segment */
    #define ADD_SEG(o, l) do { \
        if (count >= cap) { \
            cap *= 2; \
            if (safe_realloc((void **)&segs, sizeof(chunk_seg_t) * (size_t)cap)) \
                goto done; \
        } \
        segs[count].off = (o); \
        segs[count].len = (l); \
        count++; \
    } while (0)

    /* First pass: split on \n\n (paragraph boundaries) */
    size_t pos = 0;
    while (pos < text_len) {
        /* Find next \n\n */
        const char *pp = NULL;
        for (size_t i = pos; i + 1 < text_len; i++) {
            if (text[i] == '\n' && text[i + 1] == '\n') {
                pp = text + i;
                break;
            }
        }

        size_t seg_end;
        if (pp) {
            seg_end = (size_t)(pp - text);
            /* Skip past the \n\n separator */
        } else {
            seg_end = text_len;
        }

        /* Trim leading whitespace from segment */
        size_t seg_start = pos;
        while (seg_start < seg_end &&
               (text[seg_start] == '\n' || text[seg_start] == ' ' ||
                text[seg_start] == '\t'))
            seg_start++;

        size_t seg_len = seg_end > seg_start ? seg_end - seg_start : 0;

        if (seg_len > 0) {
            if (seg_len <= max_seg_len) {
                ADD_SEG(seg_start, seg_len);
            } else {
                /* Sub-split oversized paragraph at single \n */
                size_t sub_pos = seg_start;
                while (sub_pos < seg_start + seg_len) {
                    /* Find next \n */
                    size_t nl = sub_pos;
                    while (nl < seg_start + seg_len && text[nl] != '\n')
                        nl++;

                    size_t sub_len = nl - sub_pos;
                    if (sub_len == 0) { sub_pos = nl + 1; continue; }

                    if (sub_len <= max_seg_len) {
                        ADD_SEG(sub_pos, sub_len);
                    } else {
                        /* Sub-split at ". " (sentence boundary) */
                        size_t sent_pos = sub_pos;
                        while (sent_pos < sub_pos + sub_len) {
                            /* Find next ". " */
                            size_t dot = sent_pos;
                            int found_dot = 0;
                            while (dot + 1 < sub_pos + sub_len) {
                                if (text[dot] == '.' && text[dot + 1] == ' ') {
                                    found_dot = 1;
                                    break;
                                }
                                dot++;
                            }

                            size_t sent_end;
                            if (found_dot) {
                                sent_end = dot + 1; /* include the period */
                            } else {
                                sent_end = sub_pos + sub_len;
                            }

                            size_t sent_len = sent_end - sent_pos;
                            if (sent_len == 0) { sent_pos = sent_end + 1; continue; }

                            if (sent_len <= max_seg_len) {
                                ADD_SEG(sent_pos, sent_len);
                            } else {
                                /* Last resort: hard split at word boundary */
                                size_t hard_pos = sent_pos;
                                while (hard_pos < sent_pos + sent_len) {
                                    size_t chunk_end = hard_pos + max_seg_len;
                                    if (chunk_end >= sent_pos + sent_len) {
                                        ADD_SEG(hard_pos, sent_pos + sent_len - hard_pos);
                                        hard_pos = sent_pos + sent_len;
                                    } else {
                                        /* Look back for space */
                                        size_t cut = chunk_end;
                                        if (cut > hard_pos + 50) {
                                            while (cut > chunk_end - 100 && cut > hard_pos &&
                                                   text[cut] != ' ')
                                                cut--;
                                        }
                                        if (cut <= hard_pos) cut = chunk_end;
                                        /* UTF-8 boundary clamp */
                                        while (cut > hard_pos &&
                                               ((unsigned char)text[cut] & 0xC0) == 0x80)
                                            cut--;
                                        ADD_SEG(hard_pos, cut - hard_pos);
                                        hard_pos = cut;
                                        /* Skip whitespace after split */
                                        while (hard_pos < sent_pos + sent_len &&
                                               text[hard_pos] == ' ')
                                            hard_pos++;
                                    }
                                }
                            }

                            sent_pos = sent_end;
                            /* Skip space after ". " */
                            while (sent_pos < sub_pos + sub_len &&
                                   text[sent_pos] == ' ')
                                sent_pos++;
                        }
                    }

                    sub_pos = nl + 1;
                }
            }
        }

        if (pp) {
            pos = (size_t)(pp - text) + 2; /* skip past \n\n */
        } else {
            break;
        }
    }

    #undef ADD_SEG

done:
    *out_segs = segs;
    return count;
}

char **embed_prepare_text_chunked(const char *key, const char *value,
                                  int chunk_max_chars, int overlap_chars,
                                  int *out_n_chunks) {
    if (!out_n_chunks) return NULL;
    *out_n_chunks = 0;
    if (!key && !value) return NULL;
    if (chunk_max_chars <= 0) chunk_max_chars = 2000;
    (void)overlap_chars; /* no longer used - semantic overlap replaces fixed char overlap */

    /* Build the full prefix for chunk 0 */
    char *prefix_full = build_chunk_prefix(key, 1);
    if (!prefix_full) return NULL;
    size_t prefix_full_len = strlen(prefix_full);

    /* Abbreviated prefix for chunks 1+ */
    char *prefix_short = build_chunk_prefix(key, 0);
    if (!prefix_short) { free(prefix_full); return NULL; }
    size_t prefix_short_len = strlen(prefix_short);

    /* Value budget: how much content fits per chunk.
     * Use the larger prefix (full) for conservative budgeting. */
    size_t value_budget = (size_t)chunk_max_chars > prefix_full_len + 20
                        ? (size_t)chunk_max_chars - prefix_full_len - 10
                        : 100;

    size_t vlen = value ? strlen(value) : 0;

    /* If everything fits in one chunk, use the single-text path */
    if (prefix_full_len + vlen <= (size_t)chunk_max_chars) {
        char **result = malloc(sizeof(char *));
        if (!result) { free(prefix_full); free(prefix_short); return NULL; }
        result[0] = embed_prepare_text(key, value, chunk_max_chars);
        if (!result[0]) { free(result); free(prefix_full); free(prefix_short); return NULL; }
        *out_n_chunks = 1;
        free(prefix_full);
        free(prefix_short);
        return result;
    }

    /* Split value into semantic segments */
    chunk_seg_t *segs = NULL;
    int n_segs = split_into_segments(value, vlen, value_budget, &segs);
    if (n_segs <= 0 || !segs) {
        free(prefix_full);
        free(prefix_short);
        free(segs);
        return NULL;
    }

    /* Bin-pack segments into chunks.
     * Semantic overlap: the last segment of chunk N is repeated as the
     * first segment of chunk N+1, providing context continuity. */
    const int MAX_CHUNKS = 16;
    char **result = malloc(sizeof(char *) * (size_t)MAX_CHUNKS);
    if (!result) { free(prefix_full); free(prefix_short); free(segs); return NULL; }

    int actual = 0;
    int seg_i = 0;
    int prev_last_seg = -1;  /* index of last segment in previous chunk */

    while (seg_i < n_segs && actual < MAX_CHUNKS) {
        int is_first_chunk = (actual == 0);
        const char *pfx = is_first_chunk ? prefix_full : prefix_short;
        size_t pfx_len = is_first_chunk ? prefix_full_len : prefix_short_len;

        str_t chunk = str_new((size_t)chunk_max_chars + 64);
        str_append_cstr(&chunk, pfx);

        /* Reserve space for position indicator "[N/M] " - estimate max 10 chars */
        size_t budget = (size_t)chunk_max_chars > pfx_len + 10
                      ? (size_t)chunk_max_chars - pfx_len - 10
                      : 100;
        size_t used = 0;
        int last_seg_in_chunk = -1;

        /* Semantic overlap: repeat last segment from previous chunk */
        if (prev_last_seg >= 0 && prev_last_seg < n_segs) {
            size_t slen = segs[prev_last_seg].len;
            if (slen <= budget) {
                str_append(&chunk, value + segs[prev_last_seg].off, slen);
                str_append_cstr(&chunk, "\n");
                used += slen + 1;
            }
        }

        /* Pack segments into this chunk */
        int packed_any = 0;
        while (seg_i < n_segs) {
            size_t slen = segs[seg_i].len;
            size_t need = used > 0 ? slen + 1 : slen; /* +1 for \n separator */

            if (used + need > budget) {
                if (!packed_any) {
                    /* Single segment exceeds budget - take it truncated */
                    size_t take = budget > used ? budget - used : 0;
                    if (take > 0) {
                        /* UTF-8 safe truncation */
                        while (take > 0 &&
                               ((unsigned char)value[segs[seg_i].off + take] & 0xC0) == 0x80)
                            take--;
                        if (used > 0) str_append_cstr(&chunk, "\n");
                        str_append(&chunk, value + segs[seg_i].off, take);
                        str_append_cstr(&chunk, "...");
                    }
                    last_seg_in_chunk = seg_i;
                    seg_i++;
                    packed_any = 1;
                }
                break;
            }

            if (used > 0) {
                str_append_cstr(&chunk, "\n");
                used += 1;
            }
            str_append(&chunk, value + segs[seg_i].off, slen);
            used += slen;
            last_seg_in_chunk = seg_i;
            packed_any = 1;
            seg_i++;
        }

        if (packed_any) {
            result[actual] = str_steal(&chunk);
            prev_last_seg = last_seg_in_chunk;
            actual++;
        } else {
            str_free(&chunk);
            break;
        }
    }

    /* Now go back and insert position indicators into each chunk.
     * We deferred this because we didn't know n_chunks during packing. */
    if (actual > 1) {
        for (int i = 0; i < actual; i++) {
            size_t pfx_len_i = (i == 0) ? prefix_full_len : prefix_short_len;

            /* Find where the prefix ends in the chunk string */
            char *old = result[i];
            size_t old_len = strlen(old);

            str_t tagged = str_new(old_len + 12);
            str_append(&tagged, old, pfx_len_i);
            str_appendf(&tagged, "[%d/%d] ", i + 1, actual);
            if (old_len > pfx_len_i)
                str_append(&tagged, old + pfx_len_i, old_len - pfx_len_i);

            free(old);
            result[i] = str_steal(&tagged);
        }
    }

    free(prefix_full);
    free(prefix_short);
    free(segs);
    *out_n_chunks = actual;
    return result;
}

/* ── multi-vector persistence ────────────────────────── */

int embed_multi_vec_save(const embed_multi_vec_t *mv, const char *path) {
    if (!mv || !mv->data || mv->dim <= 0 || mv->n_chunks <= 0 || !path)
        return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    if (mv->n_chunks == 1) {
        /* Single chunk: write in old format for backward compatibility.
         * [int32 dim][float32 × dim] */
        int32_t dim = (int32_t)mv->dim;
        if (fwrite(&dim, sizeof(dim), 1, f) != 1) { fclose(f); return -1; }
        if (fwrite(mv->data, sizeof(float), (size_t)mv->dim, f)
            != (size_t)mv->dim) { fclose(f); return -1; }
    } else {
        /* Multi-chunk: new format.
         * [int32 -n_chunks][int32 dim][float32 × dim × n_chunks] */
        int32_t neg_chunks = -(int32_t)mv->n_chunks;
        int32_t dim = (int32_t)mv->dim;
        if (fwrite(&neg_chunks, sizeof(neg_chunks), 1, f) != 1) { fclose(f); return -1; }
        if (fwrite(&dim, sizeof(dim), 1, f) != 1) { fclose(f); return -1; }
        size_t total_floats = (size_t)mv->dim * (size_t)mv->n_chunks;
        if (fwrite(mv->data, sizeof(float), total_floats, f) != total_floats) {
            fclose(f); return -1;
        }
    }

    fclose(f);
    return 0;
}

embed_multi_vec_t embed_multi_vec_load(const char *path) {
    embed_multi_vec_t result = {0};
    if (!path) return result;

    FILE *f = fopen(path, "rb");
    if (!f) return result;

    /* Read first int32 to determine format */
    int32_t first = 0;
    if (fread(&first, sizeof(first), 1, f) != 1) { fclose(f); return result; }

    int32_t dim;
    int n_chunks;

    if (first > 0 && first <= 65536) {
        /* Old format: first int32 is the dimension, single chunk */
        dim = first;
        n_chunks = 1;
    } else if (first < 0 && first >= -256) {
        /* New format: first int32 is -n_chunks */
        n_chunks = -first;
        if (fread(&dim, sizeof(dim), 1, f) != 1 || dim <= 0 || dim > 65536) {
            fclose(f);
            return result;
        }
    } else {
        /* Invalid or corrupt file */
        fclose(f);
        return result;
    }

    /* Read all float data */
    size_t total_floats = (size_t)dim * (size_t)n_chunks;
    result.data = malloc(sizeof(float) * total_floats);
    if (!result.data) { fclose(f); return result; }

    if (fread(result.data, sizeof(float), total_floats, f) != total_floats) {
        free(result.data);
        result.data = NULL;
        fclose(f);
        return result;
    }

    result.dim = (int)dim;
    result.n_chunks = n_chunks;
    fclose(f);
    return result;
}

void embed_multi_vec_free(embed_multi_vec_t *mv) {
    if (!mv) return;
    free(mv->data);
    mv->data = NULL;
    mv->dim = 0;
    mv->n_chunks = 0;
}

/* ── multi-vector similarity ─────────────────────────── */

float embed_cosine_sim_multi(const embed_vec_t *query,
                             const embed_multi_vec_t *stored) {
    if (!query || !stored || !query->data || !stored->data) return 0.0f;
    if (query->dim != stored->dim || query->dim == 0) return 0.0f;

    float max_sim = -2.0f;
    for (int c = 0; c < stored->n_chunks; c++) {
        float sim = cosine_sim_raw(query->data,
                                   stored->data + c * stored->dim,
                                   stored->dim);
        if (sim > max_sim) max_sim = sim;
    }
    return max_sim;
}

float embed_cosine_sim_multi_multi(const embed_multi_vec_t *a,
                                   const embed_multi_vec_t *b) {
    if (!a || !b || !a->data || !b->data) return 0.0f;
    if (a->dim != b->dim || a->dim == 0) return 0.0f;

    float max_sim = -2.0f;
    for (int i = 0; i < a->n_chunks; i++) {
        for (int j = 0; j < b->n_chunks; j++) {
            float sim = cosine_sim_raw(a->data + i * a->dim,
                                       b->data + j * b->dim,
                                       a->dim);
            if (sim > max_sim) max_sim = sim;
        }
    }
    return max_sim;
}

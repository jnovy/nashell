#define _GNU_SOURCE
#include "embedding.h"
#include "embedding_onnx.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>

/* ── curl write callback ─────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_buf_t *buf = (curl_buf_t *)userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

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
    ctx->available = 0;
    ctx->detected_dim = 0;
    ctx->onnx = NULL;

    return ctx;
}

void embed_free(embed_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->onnx) onnx_embed_free(ctx->onnx);
    free(ctx->cfg.model);
    free(ctx->cfg.api_base);
    free(ctx->cfg.model_path);
    free(ctx);
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
                    result.dim = dim;
                    if (result.data) {
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
                            result.dim = dim;
                            if (result.data) {
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

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(url);
        free(body);
        return result;
    }

    curl_buf_t response = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK && response.data) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) {
            result = parse_response(ctx, response.data);
        }
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);
    free(body);

    return result;
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
            fprintf(stderr, "[embed] ONNX: no model_path configured\n");
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

float embed_cosine_sim(const embed_vec_t *a, const embed_vec_t *b) {
    if (!a || !b || !a->data || !b->data) return 0.0f;
    if (a->dim != b->dim || a->dim == 0) return 0.0f;

    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (int i = 0; i < a->dim; i++) {
        dot    += (double)a->data[i] * (double)b->data[i];
        norm_a += (double)a->data[i] * (double)a->data[i];
        norm_b += (double)b->data[i] * (double)b->data[i];
    }

    double denom = sqrt(norm_a) * sqrt(norm_b);
    if (denom < 1e-12) return 0.0f;

    return (float)(dot / denom);
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

    embed_vec_t *results = calloc((size_t)n_texts, sizeof(embed_vec_t));
    if (!results) return NULL;

    /* FIX #15: Sequential fallback — each backend can optimize with batch
     * API calls in the future (e.g., Ollama supports batch input arrays,
     * OpenAI supports array inputs in /v1/embeddings). */
    int count = 0;
    for (int i = 0; i < n_texts; i++) {
        results[i] = embed_text(ctx, texts[i]);
        if (results[i].data && results[i].dim > 0) {
            count++;
        }
    }
    *out_count = n_texts;  /* all slots populated (some may have data=NULL) */
    return results;
}

/* ── text preparation ────────────────────────────────── */

char *embed_prepare_text(const char *key, const char *value,
                         const char **tags, int n_tags, int max_chars) {
    if (!key && !value) return NULL;
    if (max_chars <= 0) max_chars = 2000;  /* default: ~500 tokens */

    str_t out = str_new((size_t)max_chars + 256);

    /* Key (most important for matching) */
    if (key) {
        str_append_cstr(&out, key);
        str_append_cstr(&out, "\n");
    }

    /* Tags (secondary importance) */
    if (tags && n_tags > 0) {
        str_append_cstr(&out, "Tags: ");
        for (int i = 0; i < n_tags; i++) {
            if (i > 0) str_append_cstr(&out, ", ");
            if (tags[i]) str_append_cstr(&out, tags[i]);
        }
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
                /* Truncate at word boundary if possible */
                size_t cut = remaining;
                while (cut > remaining - 100 && cut > 0 && value[cut] != ' ')
                    cut--;
                if (cut == 0) cut = remaining;
                str_appendf(&out, "%.*s...", (int)cut, value);
            }
        }
    }

    return str_steal(&out);
}

/* tool_image.c — image_analyze tool: multimodal image analysis via LLM
 *
 * Reads an image file, base64-encodes it, and sends it to the configured
 * LLM provider as a multimodal (vision) request.  Returns the model's
 * textual analysis of the image.
 *
 * Supports all provider types:
 *   - OpenAI / Local:  content array with image_url (data URI)
 *   - Anthropic / Vertex: content array with image source block
 */

#include "tools_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <curl/curl.h>

/* ── Base64 encoder ─────────────────────────────────────────────── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode binary data to base64.  Returns malloc'd string (caller frees).
 * Returns NULL on allocation failure. */
static char *base64_encode(const unsigned char *data, size_t input_len,
                           size_t *output_len) {
    size_t olen = 4 * ((input_len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i + 2 < input_len; i += 3, j += 4) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i+1] << 8) |
                      (uint32_t)data[i+2];
        out[j]   = b64_table[(v >> 18) & 0x3F];
        out[j+1] = b64_table[(v >> 12) & 0x3F];
        out[j+2] = b64_table[(v >>  6) & 0x3F];
        out[j+3] = b64_table[ v        & 0x3F];
    }

    if (i < input_len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < input_len) v |= (uint32_t)data[i+1] << 8;
        out[j]   = b64_table[(v >> 18) & 0x3F];
        out[j+1] = b64_table[(v >> 12) & 0x3F];
        out[j+2] = (i + 1 < input_len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j+3] = '=';
        j += 4;
    }

    out[j] = '\0';
    if (output_len) *output_len = j;
    return out;
}

/* ── MIME type detection from file extension ─────────────────────── */

static const char *mime_from_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    dot++;  /* skip the dot */

    if (strcasecmp(dot, "png") == 0)  return "image/png";
    if (strcasecmp(dot, "jpg") == 0)  return "image/jpeg";
    if (strcasecmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, "gif") == 0)  return "image/gif";
    if (strcasecmp(dot, "webp") == 0) return "image/webp";
    if (strcasecmp(dot, "bmp") == 0)  return "image/bmp";
    if (strcasecmp(dot, "svg") == 0)  return "image/svg+xml";
    if (strcasecmp(dot, "tiff") == 0) return "image/tiff";
    if (strcasecmp(dot, "tif") == 0)  return "image/tiff";
    return NULL;
}

/* ── Build OpenAI-format multimodal request ──────────────────────── */

static char *build_openai_image_request(provider_t *p, const char *question,
                                         const char *mime, const char *b64) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model",
                            p->cfg.model_id ? p->cfg.model_id : "gpt-4o");
    cJSON_AddNumberToObject(req, "max_tokens", 4096);
    cJSON_AddNumberToObject(req, "temperature", 0.2);

    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    /* Content array: text + image_url */
    cJSON *content = cJSON_CreateArray();

    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", question);
    cJSON_AddItemToArray(content, text_part);

    cJSON *img_part = cJSON_CreateObject();
    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON *img_url = cJSON_CreateObject();
    /* Build data URI: data:<mime>;base64,<data> */
    size_t uri_len = strlen(mime) + strlen(b64) + 32;
    char *data_uri = malloc(uri_len);
    if (data_uri) {
        snprintf(data_uri, uri_len, "data:%s;base64,%s", mime, b64);
        cJSON_AddStringToObject(img_url, "url", data_uri);
        free(data_uri);
    }
    cJSON_AddStringToObject(img_url, "detail", "auto");
    cJSON_AddItemToObject(img_part, "image_url", img_url);
    cJSON_AddItemToArray(content, img_part);

    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(req, "messages", msgs);

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── Build Anthropic-format multimodal request ───────────────────── */

static char *build_anthropic_image_request(provider_t *p, const char *question,
                                            const char *mime, const char *b64) {
    cJSON *req = cJSON_CreateObject();

    /* Vertex AI: anthropic_version in body, model in URL (not body).
     * Direct Anthropic API: model in body, anthropic_version as header
     * (but we also add it to body for direct API calls). */
    if (p->type == PROVIDER_VERTEX) {
        cJSON_AddStringToObject(req, "anthropic_version", "vertex-2023-10-16");
    } else {
        cJSON_AddStringToObject(req, "model",
                                p->cfg.model_id ? p->cfg.model_id : "claude-sonnet-4-20250514");
    }
    cJSON_AddNumberToObject(req, "max_tokens", 4096);

    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    /* Content array: image + text */
    cJSON *content = cJSON_CreateArray();

    /* Image block first (Anthropic recommends image before text) */
    cJSON *img_block = cJSON_CreateObject();
    cJSON_AddStringToObject(img_block, "type", "image");
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "type", "base64");
    cJSON_AddStringToObject(source, "media_type", mime);
    cJSON_AddStringToObject(source, "data", b64);
    cJSON_AddItemToObject(img_block, "source", source);
    cJSON_AddItemToArray(content, img_block);

    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", question);
    cJSON_AddItemToArray(content, text_block);

    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(req, "messages", msgs);

    /* anthropic_version already added at top of function for both
     * PROVIDER_VERTEX and PROVIDER_ANTHROPIC (via the if/else block). */

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── Extract response text from provider response ────────────────── */

static char *extract_response_text(provider_t *p, const char *resp_json) {
    cJSON *resp = cJSON_Parse(resp_json);
    if (!resp) return NULL;

    char *result = NULL;

    if (p->type == PROVIDER_ANTHROPIC || p->type == PROVIDER_VERTEX) {
        /* Anthropic format: {"content":[{"type":"text","text":"..."}]} */
        cJSON *content = cJSON_GetObjectItem(resp, "content");
        if (content && cJSON_IsArray(content)) {
            int n = cJSON_GetArraySize(content);
            for (int i = 0; i < n; i++) {
                cJSON *block = cJSON_GetArrayItem(content, i);
                cJSON *type = cJSON_GetObjectItem(block, "type");
                if (type && cJSON_IsString(type) &&
                    strcmp(type->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        result = strdup(text->valuestring);
                        break;
                    }
                }
            }
        }
    } else {
        /* OpenAI format: {"choices":[{"message":{"content":"..."}}]} */
        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *choice = cJSON_GetArrayItem(choices, 0);
            cJSON *message = cJSON_GetObjectItem(choice, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content)) {
                    result = strdup(content->valuestring);
                }
            }
        }
    }

    /* Check for error responses */
    if (!result) {
        cJSON *error = cJSON_GetObjectItem(resp, "error");
        if (error) {
            const char *msg = "";
            if (cJSON_IsString(error)) msg = error->valuestring;
            else {
                cJSON *emsg = cJSON_GetObjectItem(error, "message");
                if (emsg && cJSON_IsString(emsg)) msg = emsg->valuestring;
            }
            size_t len = strlen(msg) + 64;
            result = malloc(len);
            if (result) snprintf(result, len, "API error: %s", msg);
        }
    }

    cJSON_Delete(resp);
    return result;
}

/* ── image_analyze tool handler ──────────────────────────────────── */

#define IMAGE_MAX_SIZE (20 * 1024 * 1024)  /* 20 MB max */

tool_result_t tool_image_analyze(tool_ctx_t *ctx, cJSON *params) {
    cJSON *path_j = cJSON_GetObjectItem(params, "path");
    if (!path_j || !path_j->valuestring || !path_j->valuestring[0])
        return tools_make_error("image_analyze requires a non-empty 'path' string.");

    const char *path = path_j->valuestring;

    /* Optional question/prompt about the image */
    cJSON *question_j = cJSON_GetObjectItem(params, "question");
    const char *question = (question_j && cJSON_IsString(question_j) &&
                            question_j->valuestring[0])
        ? question_j->valuestring
        : "Describe this image in detail. Include all visible text, "
          "layout, colors, and any notable features.";

    /* Resolve aliases and store/ paths */
    char *resolved = NULL;
    char resolved_buf[NASH_PATH_MAX];
    path = tools_resolve_path(ctx, path, resolved_buf, &resolved);

    /* Validate file */
    struct stat st;
    if (stat(path, &st) != 0) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot stat '%.4095s': %s",
                 path, strerror(errno));
        free(resolved);
        return tools_make_error(msg);
    }
    if (!S_ISREG(st.st_mode)) {
        free(resolved);
        return tools_make_error("not a regular file");
    }
    if (st.st_size > IMAGE_MAX_SIZE) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "image too large (%ld bytes, limit %d MB)",
                 (long)st.st_size, IMAGE_MAX_SIZE / (1024 * 1024));
        free(resolved);
        return tools_make_error(msg);
    }

    /* Detect MIME type */
    const char *mime = mime_from_ext(path);
    if (!mime) {
        free(resolved);
        return tools_make_error(
            "unsupported image format. Supported: "
            "png, jpg/jpeg, gif, webp, bmp, svg, tiff");
    }

    /* Read image as binary */
    size_t img_len = 0;
    unsigned char *img_data = slurp_file_binary(path, &img_len);
    if (!img_data) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "cannot read '%.4095s': %s",
                 path, strerror(errno));
        free(resolved);
        return tools_make_error(msg);
    }

    /* Base64 encode */
    size_t b64_len = 0;
    char *b64 = base64_encode(img_data, img_len, &b64_len);
    free(img_data);  /* done with raw data */
    if (!b64) {
        free(resolved);
        return tools_make_error("base64 encoding failed (out of memory)");
    }

    /* Check provider availability */
    provider_t *p = ctx->provider;
    if (!p) {
        free(b64);
        free(resolved);
        return tools_make_error("no provider configured");
    }

    /* Build provider-specific request */
    char *req_body = NULL;
    if (p->type == PROVIDER_ANTHROPIC || p->type == PROVIDER_VERTEX) {
        req_body = build_anthropic_image_request(p, question, mime, b64);
    } else {
        req_body = build_openai_image_request(p, question, mime, b64);
    }
    free(b64);  /* done with base64 data */

    if (!req_body) {
        free(resolved);
        return tools_make_error("failed to build request JSON");
    }

    /* Get endpoint and headers */
    const char *endpoint = p->get_endpoint ? p->get_endpoint(p) : NULL;
    if (!endpoint) {
        free(req_body);
        free(resolved);
        return tools_make_error("cannot determine API endpoint");
    }

    struct curl_slist *headers = p->build_headers ? p->build_headers(p) : NULL;

    /* Make the API call */
    str_t response = str_new(4096);
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(req_body);
        free(resolved);
        curl_slist_free_all(headers);
        str_free(&response);
        return tools_make_error("curl_easy_init failed");
    }

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    /* Vision requests can be slow — use a generous timeout */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    nash_log("[image_analyze] sending %.1f KB image (%s) to %s",
             (double)img_len / 1024.0, mime, endpoint);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "API request failed: %s",
                 curl_easy_strerror(res));
        free(req_body);
        free(resolved);
        str_free(&response);
        return tools_make_error(msg);
    }

    if (http_code != 200) {
        char msg[4224];
        snprintf(msg, sizeof(msg), "API returned HTTP %ld: %.4000s",
                 http_code,
                 response.len > 0 ? str_cstr(&response) : "(empty)");
        nash_log("[image_analyze] HTTP %ld error: %.2000s",
                 http_code,
                 response.len > 0 ? str_cstr(&response) : "(empty)");
        free(req_body);
        free(resolved);
        str_free(&response);
        return tools_make_error(msg);
    }

    /* Parse the response */
    char *analysis = extract_response_text(p, str_cstr(&response));
    str_free(&response);
    free(req_body);

    if (!analysis) {
        free(resolved);
        return tools_make_error("failed to parse API response");
    }

    /* Store result and build metadata */
    char *hash = store_save(ctx->store, analysis);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "path", path_j->valuestring);
    cJSON_AddStringToObject(meta, "mime_type", mime);
    cJSON_AddNumberToObject(meta, "image_size", (double)img_len);
    cJSON_AddStringToObject(meta, "question", question);
    cJSON_AddNumberToObject(meta, "chars", (double)strlen(analysis));
    cJSON_AddStringToObject(meta, "ref", alias);

    /* Include analysis text inline if small enough */
    if (strlen(analysis) <= 50000) {
        cJSON_AddStringToObject(meta, "content", analysis);
    }

    tool_result_t result = tools_make_result(1, meta, hash);
    result.importance = 2;  /* high — user explicitly requested analysis */

    free(analysis);
    free(alias);
    free(resolved);

    return result;
}

#include "tools_internal.h"
#include "tool_plugin.h"
#include "html_extract.h"
#include "searxng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* ── web_fetch ──────────────────────────────────────────── */

/* HTML text extraction moved to html_extract.c/h */

/* Sanitize a buffer to valid UTF-8 in-place.
 * Invalid byte sequences (e.g. Windows-1250, Latin-1) are replaced with '?'.
 * Without this, websites using non-UTF-8 charsets produce invalid JSON
 * when serialized for the LLM API, causing HTTP 400 errors.
 * Returns the (unchanged) string length. */
static size_t utf8_sanitize(char *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    unsigned char *end = p + len;
    while (p < end) {
        if (p[0] < 0x80) {
            p++;  /* ASCII */
        } else if ((p[0] & 0xE0) == 0xC0 && p + 1 < end &&
                   (p[1] & 0xC0) == 0x80) {
            p += 2;  /* 2-byte UTF-8 */
        } else if ((p[0] & 0xF0) == 0xE0 && p + 2 < end &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            p += 3;  /* 3-byte UTF-8 */
        } else if ((p[0] & 0xF8) == 0xF0 && p + 3 < end &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            p += 4;  /* 4-byte UTF-8 */
        } else {
            *p = '?';  /* Invalid byte → replace */
            p++;
        }
    }
    return len;
}

static size_t web_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *buf = userdata;
    size_t total = size * nmemb;
    /* Cap at 500KB to prevent memory explosion */
    if (buf->len + total > 512000) {
        size_t remaining = 512000 - buf->len;
        if (remaining > 0) str_append(buf, ptr, remaining);
        return 0;  /* abort transfer — returning != total tells curl to stop */
    }
    str_append(buf, ptr, total);
    return total;
}

tool_result_t tool_web_fetch(tool_ctx_t *ctx, cJSON *params) {
    cJSON *url_j = cJSON_GetObjectItem(params, "url");
    if (!url_j || !url_j->valuestring || !url_j->valuestring[0])
        return tools_make_error("web_fetch requires a non-empty 'url' string. "
                          "Provide the full URL (https://...) to fetch.");

    const char *url = url_j->valuestring;

    CURL *curl = curl_easy_init();
    if (!curl) return tools_make_error("curl_easy_init failed");

    str_t body = str_new(8192);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    long web_timeout = (ctx->cfg && ctx->cfg->web_timeout > 0)
                       ? (long)ctx->cfg->web_timeout : 30L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, web_timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char *content_type = NULL;
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) content_type = strdup(ct);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK && !(res == CURLE_WRITE_ERROR && body.len > 0)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "fetch failed: %s", curl_easy_strerror(res));
        str_free(&body);
        free(content_type);
        return tools_make_error(msg);
    }

    /* For HTML content, extract text to save context tokens */
    char *store_data = body.data;
    size_t store_len = body.len;
    char *extracted = NULL;
    if (content_type && strstr(content_type, "text/html")) {
        extracted = html_extract_text(body.data, body.len);
        if (extracted) {
            store_data = extracted;
            store_len = strlen(extracted);
        }
    }

    /* Sanitize to valid UTF-8 — websites using Windows-1250, Latin-1, etc.
     * produce bytes that break JSON serialization to the LLM API */
    utf8_sanitize(store_data, store_len);

    /* Store to shared store */
    char *hash = store_save(ctx->store, store_data);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    /* Build metadata */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "url", url);
    cJSON_AddNumberToObject(meta, "http_code", http_code);
    cJSON_AddNumberToObject(meta, "chars", (double)store_len);
    cJSON_AddNumberToObject(meta, "lines", count_lines(store_data));
    if (content_type) cJSON_AddStringToObject(meta, "content_type", content_type);
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);
    if (extracted)
        cJSON_AddNumberToObject(meta, "original_chars", (double)body.len);
    /* Notify model when response was truncated by web_write_cb's 512KB cap */
    if (body.len >= 512000)
        cJSON_AddStringToObject(meta, "truncated",
            "Response exceeded 512KB and was truncated. "
            "Content may be incomplete.");

    /* Content stored to .store/ — model reads via file_read(ref) */

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "web_fetch",
                   params, alias, store_len, count_lines(store_data), NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(extracted);
    free(content_type);
    str_free(&body);
    return tools_make_result(http_code >= 200 && http_code < 400, meta, ref_copy);
}

/* ── web_search ────────────────────────────────────────── */

tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params) {
    cJSON *query_j = cJSON_GetObjectItem(params, "query");
    if (!query_j || !query_j->valuestring || !query_j->valuestring[0])
        return tools_make_error("web_search requires a non-empty 'query' string. "
                          "Provide specific search terms.");

    const char *query = query_j->valuestring;
    const char *searxng_url = (ctx->cfg) ? ctx->cfg->searxng_url : NULL;
    char *results_text = NULL;
    int result_count = 0;
    long search_timeout = (ctx->cfg && ctx->cfg->web_timeout > 0)
                          ? (long)ctx->cfg->web_timeout : 30L;

    /* SearXNG is the sole search backend. Auto-start if not running. */
    if (ensure_searxng(searxng_url) != 0) {
        return tools_make_error("SearXNG not available and could not be auto-started. "
                          "Install podman/docker or configure a running SearXNG instance "
                          "in ~/.nash/config.toml [search] section.");
    }
    results_text = searxng_search(searxng_url, query, &result_count, search_timeout);

    if (!results_text || result_count == 0) {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg),
                 "no results found for query: %s", query);
        /* Store error to .store/ so it gets a ref for reactRX.md hyperlink */
        char *err_hash = store_save(ctx->store, errmsg);
        char *err_alias = tool_register_alias(ctx, err_hash ? err_hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "web_search",
                       params, err_alias, strlen(errmsg), 0, errmsg, NULL);
        free(err_hash);
        free(err_alias);
        free(results_text);
        return tools_make_error(errmsg);
    }

    /* Store results */
    char *hash = store_save(ctx->store, results_text);
    char *alias = tool_register_alias(ctx, hash ? hash : "");

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "query", query);
    cJSON_AddNumberToObject(meta, "results", result_count);
    cJSON_AddNumberToObject(meta, "chars", (double)strlen(results_text));
    if (alias) cJSON_AddStringToObject(meta, "ref", alias);

    tools_inject_thought(ctx, params);
    tool_journal(ctx, "web_search",
                   params, alias, strlen(results_text), result_count, NULL, NULL);

    char *ref_copy = alias ? strdup(alias) : NULL;
    free(alias);
    free(hash);
    free(results_text);
    return tools_make_result(1, meta, ref_copy);
}

/* ── Plugin registration ──────────────────────────────── */

static const tool_param_t web_fetch_params[] = {
    TOOL_PARAM("url", "string", "URL to fetch", 1),
    TOOL_PARAM_END
};

static const tool_param_t web_search_params[] = {
    TOOL_PARAM("query", "string", "Search query", 1),
    TOOL_PARAM_END
};

static const tool_plugin_t web_plugins[] = {
    TOOL_DEF("web_fetch",  "Fetch content from a URL.",
             web_fetch_params, tool_web_fetch),
    TOOL_DEF("web_search", "Search the web for information.",
             web_search_params, tool_web_search),
};
TOOL_PLUGIN_REGISTER_ARRAY(web_plugins, 2)

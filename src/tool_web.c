#include "tools_internal.h"
#include "tool_plugin.h"
#include "html_extract.h"
#include "searxng.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <curl/curl.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ── SSRF protection ───────────────────────────────────── */

/* Check if a resolved IP address is internal/private.
 * Returns 1 if the address is loopback, link-local, or RFC1918. */
static int is_internal_ip(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        uint32_t addr = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
        /* 127.0.0.0/8 (loopback) */
        if ((addr >> 24) == 127) return 1;
        /* 10.0.0.0/8 */
        if ((addr >> 24) == 10) return 1;
        /* 172.16.0.0/12 */
        if ((addr >> 20) == (172 << 4 | 1)) return 1;  /* 0xAC1 */
        /* 192.168.0.0/16 */
        if ((addr >> 16) == ((192 << 8) | 168)) return 1;
        /* 169.254.0.0/16 (link-local) */
        if ((addr >> 16) == ((169 << 8) | 254)) return 1;
        /* 0.0.0.0/8 */
        if ((addr >> 24) == 0) return 1;
        return 0;
    } else if (sa->sa_family == AF_INET6) {
        const uint8_t *b = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
        /* ::1 loopback */
        static const uint8_t lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(b, lo, 16) == 0) return 1;
        /* fe80::/10 link-local */
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 1;
        /* ::ffff:0:0/96 IPv4-mapped — check the embedded IPv4 */
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 &&
            b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
            b[8] == 0 && b[9] == 0 && b[10] == 0xff && b[11] == 0xff) {
            uint32_t v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                          ((uint32_t)b[14] << 8)  | (uint32_t)b[15];
            if ((v4 >> 24) == 127) return 1;
            if ((v4 >> 24) == 10) return 1;
            if ((v4 >> 20) == (172 << 4 | 1)) return 1;
            if ((v4 >> 16) == ((192 << 8) | 168)) return 1;
            if ((v4 >> 16) == ((169 << 8) | 254)) return 1;
            if ((v4 >> 24) == 0) return 1;
        }
        /* fc00::/7 unique local */
        if ((b[0] & 0xfe) == 0xfc) return 1;
        return 0;
    }
    return 0;
}

/* Check if a URL targets an internal network.  On success (returns 0,
 * meaning the URL is safe), *resolve_out receives a curl_slist of
 * "+host:port:ip" entries that pin the resolved address so curl reuses
 * the same IP we checked (prevents DNS rebinding TOCTOU).
 * Returns 1 if internal/blocked; caller must curl_slist_free_all(*resolve_out). */
static int url_check_ssrf(const char *url, struct curl_slist **resolve_out) {
    *resolve_out = NULL;
    CURLU *cu = curl_url();
    if (!cu) return 1;  /* fail closed */
    if (curl_url_set(cu, CURLUPART_URL, url, 0) != CURLUE_OK) {
        curl_url_cleanup(cu);
        return 1;  /* fail closed */
    }
    char *host = NULL, *port = NULL, *scheme = NULL;
    curl_url_get(cu, CURLUPART_HOST, &host, 0);
    curl_url_get(cu, CURLUPART_PORT, &port, 0);
    curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
    curl_url_cleanup(cu);
    if (!host) { curl_free(port); curl_free(scheme); return 1; }

    /* Determine effective port for CURLOPT_RESOLVE entry */
    const char *eff_port = port;
    if (!eff_port) {
        if (scheme && strcmp(scheme, "https") == 0) eff_port = "443";
        else eff_port = "80";
    }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        if (res) freeaddrinfo(res);
        curl_free(host); curl_free(port); curl_free(scheme);
        return 1;  /* can't resolve - fail closed */
    }

    int internal = 0;
    struct curl_slist *resolve_list = NULL;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (is_internal_ip(ai->ai_addr)) { internal = 1; break; }
        /* Build resolve entry to pin this IP */
        char ipbuf[INET6_ADDRSTRLEN];
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, ipbuf, sizeof(ipbuf),
                        NULL, 0, NI_NUMERICHOST) == 0) {
            char entry[512];
            snprintf(entry, sizeof(entry), "+%s:%s:%s", host, eff_port, ipbuf);
            resolve_list = curl_slist_append(resolve_list, entry);
        }
    }
    freeaddrinfo(res);

    if (internal || !resolve_list) {
        curl_slist_free_all(resolve_list);
        curl_free(host); curl_free(port); curl_free(scheme);
        return 1;
    }

    *resolve_out = resolve_list;
    curl_free(host); curl_free(port); curl_free(scheme);
    return 0;
}

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
    if (size > 0 && nmemb > SIZE_MAX / size) return 0;  /* overflow guard */
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
    TOOL_REQ_STR(params, "url", url);

    /* SSRF protection: reject requests to internal/private networks.
     * Pin the resolved IP via CURLOPT_RESOLVE so curl reuses the same
     * address we checked (prevents DNS rebinding TOCTOU). */
    struct curl_slist *resolve_list = NULL;
    if (url_check_ssrf(url, &resolve_list))
        return tools_make_error("Blocked: URL resolves to an internal/private network address");

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(resolve_list);
        return tools_make_error("curl_easy_init failed");
    }

    str_t body = str_new(8192);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
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
    curl_slist_free_all(resolve_list);

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
    TOOL_REQ_STR(params, "query", query);
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

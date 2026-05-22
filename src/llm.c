#include "llm.h"
#include "str.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Chat management ─────────────────────────────────────────── */

llm_chat_t *llm_chat_new(void) {
    llm_chat_t *c = calloc(1, sizeof(*c));
    c->cap_msgs = 32;
    c->msgs = calloc(c->cap_msgs, sizeof(llm_msg_t));
    return c;
}

void llm_chat_free(llm_chat_t *chat) {
    if (!chat) return;
    for (int i = 0; i < chat->n_msgs; i++) {
        free(chat->msgs[i].role);
        free(chat->msgs[i].content);
    }
    free(chat->msgs);
    free(chat);
}

void llm_chat_add(llm_chat_t *chat, const char *role, const char *content) {
    if (chat->n_msgs >= chat->cap_msgs) {
        chat->cap_msgs *= 2;
        chat->msgs = realloc(chat->msgs, chat->cap_msgs * sizeof(llm_msg_t));
    }
    chat->msgs[chat->n_msgs].role = strdup(role);
    chat->msgs[chat->n_msgs].content = strdup(content);
    chat->n_msgs++;
}

/* ── CURL callback ───────────────────────────────────────────── */

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *buf = userdata;
    size_t total = size * nmemb;
    str_append(buf, ptr, total);
    return total;
}

/* ── Build request JSON ──────────────────────────────────────── */

static char *build_request(const llm_config_t *cfg, llm_chat_t *chat) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg->model);
    cJSON_AddNumberToObject(req, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(req, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(req, "stream", 0);

    /* Response format: force JSON output */
    cJSON *resp_fmt = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_fmt, "type", "json_object");
    cJSON_AddItemToObject(req, "response_format", resp_fmt);

    /* Disable thinking mode for Qwen models (uses reasoning_content otherwise) */
    cJSON *tmpl_kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", 0);
    cJSON_AddItemToObject(req, "chat_template_kwargs", tmpl_kwargs);

    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < chat->n_msgs; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", chat->msgs[i].role);
        cJSON_AddStringToObject(m, "content", chat->msgs[i].content);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(req, "messages", msgs);

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── Main completion call ────────────────────────────────────── */

char *llm_complete(const llm_config_t *cfg, llm_chat_t *chat, llm_stats_t *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", cfg->api_base);

    char *req_body = build_request(cfg, chat);
    if (!req_body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(req_body); return NULL; }

    str_t response = str_new(4096);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  /* 5 min timeout */

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(req_body);

    if (res != CURLE_OK) {
        fprintf(stderr, "[llm] curl error: %s\n", curl_easy_strerror(res));
        str_free(&response);
        return NULL;
    }

    /* Parse response JSON */
    cJSON *resp = cJSON_Parse(response.data);
    str_free(&response);
    if (!resp) {
        fprintf(stderr, "[llm] failed to parse response JSON\n");
        return NULL;
    }

    /* Extract choices[0].message.content */
    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        /* Check for error */
        cJSON *err = cJSON_GetObjectItem(resp, "error");
        if (err) {
            cJSON *msg = cJSON_GetObjectItem(err, "message");
            if (msg) fprintf(stderr, "[llm] API error: %s\n", msg->valuestring);
        }
        cJSON_Delete(resp);
        return NULL;
    }

    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");

    char *result = NULL;
    if (content && content->valuestring && content->valuestring[0]) {
        result = strdup(content->valuestring);
    }

    /* If content is empty, check reasoning_content (thinking mode fallback) */
    if (!result) {
        cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
        if (reasoning && reasoning->valuestring && reasoning->valuestring[0]) {
            fprintf(stderr, "[debug] LLM returned reasoning_content instead of content (thinking mode still active?)\n");
            fprintf(stderr, "[debug] reasoning: %.200s...\n", reasoning->valuestring);
        }
        /* Dump raw response for debugging */
        char *raw = cJSON_PrintUnformatted(resp);
        if (raw) {
            fprintf(stderr, "[debug] raw API response: %.500s\n", raw);
            free(raw);
        }
    }

    /* Parse usage + timings into stats output param */
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        cJSON *usage = cJSON_GetObjectItem(resp, "usage");
        if (usage) {
            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
            if (pt) stats->prompt_tokens = (int)cJSON_GetNumberValue(pt);
            if (ct) stats->completion_tokens = (int)cJSON_GetNumberValue(ct);
        }
        cJSON *timings = cJSON_GetObjectItem(resp, "timings");
        if (timings) {
            cJSON *pps = cJSON_GetObjectItem(timings, "prompt_per_second");
            cJSON *gps = cJSON_GetObjectItem(timings, "predicted_per_second");
            cJSON *dn  = cJSON_GetObjectItem(timings, "draft_n");
            cJSON *da  = cJSON_GetObjectItem(timings, "draft_n_accepted");
            if (pps) stats->prompt_per_second = cJSON_GetNumberValue(pps);
            if (gps) stats->predicted_per_second = cJSON_GetNumberValue(gps);
            if (dn)  stats->draft_n = (int)cJSON_GetNumberValue(dn);
            if (da)  stats->draft_accepted = (int)cJSON_GetNumberValue(da);
        }
    }

    cJSON_Delete(resp);
    return result;
}

/* ── Parse action from assistant response ────────────────────── */

cJSON *llm_parse_action(const char *response) {
    if (!response) return NULL;

    /* The response should be JSON: {"thought":"...","action":"...","param":"..."} */
    /* Try to find JSON in the response (model might wrap in markdown) */
    const char *start = response;

    /* Skip leading whitespace and markdown fences */
    while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t')
        start++;
    if (strncmp(start, "```json", 7) == 0) {
        start += 7;
        while (*start == '\n' || *start == '\r') start++;
    } else if (strncmp(start, "```", 3) == 0) {
        start += 3;
        while (*start == '\n' || *start == '\r') start++;
    }

    /* Find the opening brace */
    const char *brace = strchr(start, '{');
    if (!brace) return NULL;

    cJSON *action = cJSON_Parse(brace);
    return action;
}

/* ── Fetch context size from /props ──────────────────────── */

int llm_fetch_context_size(const char *api_base) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/props", api_base);

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    str_t response = str_new(4096);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        str_free(&response);
        return 0;
    }

    cJSON *resp = cJSON_Parse(response.data);
    str_free(&response);
    if (!resp) return 0;

    int n_ctx = 0;
    cJSON *dgs = cJSON_GetObjectItem(resp, "default_generation_settings");
    if (dgs) {
        cJSON *ctx = cJSON_GetObjectItem(dgs, "n_ctx");
        if (ctx && cJSON_IsNumber(ctx)) n_ctx = (int)cJSON_GetNumberValue(ctx);
    }

    cJSON_Delete(resp);
    return n_ctx;
}

/* ── Streaming SSE state ─────────────────────────────────── */

typedef struct {
    str_t          line_buf;      /* accumulates partial SSE lines */
    str_t          full_content;  /* assembled full response */
    llm_token_fn   on_token;
    void          *userdata;
    llm_stats_t   *stats;
} sse_state_t;

static void sse_process_line(sse_state_t *st, const char *line) {
    /* Skip empty lines and non-data lines */
    if (line[0] == '\0' || line[0] == '\n') return;
    if (strncmp(line, "data: ", 6) != 0) return;

    const char *json_str = line + 6;

    /* Check for stream end */
    if (strncmp(json_str, "[DONE]", 6) == 0) return;

    /* Parse the SSE JSON chunk */
    cJSON *chunk = cJSON_Parse(json_str);
    if (!chunk) return;

    /* Extract delta.content from choices[0].delta.content */
    cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
    if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice0, "delta");
        if (delta) {
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (content && content->valuestring && content->valuestring[0]) {
                /* Got a token! */
                str_append_cstr(&st->full_content, content->valuestring);
                if (st->on_token)
                    st->on_token(content->valuestring, st->userdata);
            }
        }

        /* Check for finish_reason — last chunk has usage/timings */
        cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
        if (finish && cJSON_IsString(finish) && finish->valuestring) {
            /* Extract stats from the final chunk */
            if (st->stats) {
                cJSON *usage = cJSON_GetObjectItem(chunk, "usage");
                if (usage) {
                    cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
                    cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
                    if (pt) st->stats->prompt_tokens = (int)cJSON_GetNumberValue(pt);
                    if (ct) st->stats->completion_tokens = (int)cJSON_GetNumberValue(ct);
                }
                cJSON *timings = cJSON_GetObjectItem(chunk, "timings");
                if (timings) {
                    cJSON *pps = cJSON_GetObjectItem(timings, "prompt_per_second");
                    cJSON *gps = cJSON_GetObjectItem(timings, "predicted_per_second");
                    cJSON *dn  = cJSON_GetObjectItem(timings, "draft_n");
                    cJSON *da  = cJSON_GetObjectItem(timings, "draft_n_accepted");
                    if (pps) st->stats->prompt_per_second = cJSON_GetNumberValue(pps);
                    if (gps) st->stats->predicted_per_second = cJSON_GetNumberValue(gps);
                    if (dn)  st->stats->draft_n = (int)cJSON_GetNumberValue(dn);
                    if (da)  st->stats->draft_accepted = (int)cJSON_GetNumberValue(da);
                }
            }
        }
    }

    cJSON_Delete(chunk);
}

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    sse_state_t *st = userdata;
    size_t total = size * nmemb;
    const char *data = ptr;

    for (size_t i = 0; i < total; i++) {
        if (data[i] == '\n') {
            /* Process complete line */
            sse_process_line(st, str_cstr(&st->line_buf));
            str_clear(&st->line_buf);
        } else {
            str_append(&st->line_buf, &data[i], 1);
        }
    }

    return total;
}

char *llm_complete_stream(const llm_config_t *cfg, llm_chat_t *chat,
                          llm_stats_t *stats, llm_token_fn on_token,
                          void *userdata) {
    if (stats) memset(stats, 0, sizeof(*stats));

    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", cfg->api_base);

    /* Build request with stream=true */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg->model);
    cJSON_AddNumberToObject(req, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(req, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(req, "stream", 1);

    cJSON *resp_fmt = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_fmt, "type", "json_object");
    cJSON_AddItemToObject(req, "response_format", resp_fmt);

    cJSON *tmpl_kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", 0);
    cJSON_AddItemToObject(req, "chat_template_kwargs", tmpl_kwargs);

    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < chat->n_msgs; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", chat->msgs[i].role);
        cJSON_AddStringToObject(m, "content", chat->msgs[i].content);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(req, "messages", msgs);

    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_body) return NULL;

    /* Set up SSE state */
    sse_state_t st = {
        .line_buf     = str_new(256),
        .full_content = str_new(4096),
        .on_token     = on_token,
        .userdata     = userdata,
        .stats        = stats,
    };

    CURL *curl = curl_easy_init();
    if (!curl) { free(req_body); str_free(&st.line_buf); str_free(&st.full_content); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(req_body);

    /* Process any remaining data in line buffer */
    if (st.line_buf.len > 0)
        sse_process_line(&st, str_cstr(&st.line_buf));
    str_free(&st.line_buf);

    if (res != CURLE_OK) {
        str_free(&st.full_content);
        return NULL;
    }

    /* Return assembled content */
    if (st.full_content.len == 0) {
        str_free(&st.full_content);
        return NULL;
    }

    return str_steal(&st.full_content);
}

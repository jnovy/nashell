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

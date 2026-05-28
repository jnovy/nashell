/* provider_local.c — Local llama.cpp / OpenAI-compatible server provider
 *
 * This is the original nash backend, now wrapped in the provider vtable.
 * Talks to any OpenAI-compatible /v1/chat/completions endpoint.
 * Supports llama.cpp-specific extensions: chat_template_kwargs, reasoning_budget.
 */

#include "provider.h"
#include "str.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ── Build request ──────────────────────────────────────────────── */

static char *local_build_request(provider_t *p, llm_chat_t *chat, int stream) {
    cJSON *req = cJSON_CreateObject();
    if (p->cfg.model_id) cJSON_AddStringToObject(req, "model", p->cfg.model_id);
    cJSON_AddNumberToObject(req, "max_tokens", p->cfg.max_tokens);
    cJSON_AddNumberToObject(req, "temperature", p->cfg.temperature);
    cJSON_AddBoolToObject(req, "stream", stream);

    /* Tools */
    cJSON *tools = build_tools_from_registry(PROVIDER_LOCAL);
    cJSON_AddItemToObject(req, "tools", tools);

    /* llama.cpp-specific: chat_template_kwargs for thinking mode */
    cJSON *tmpl_kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", p->cfg.enable_thinking);
    cJSON_AddItemToObject(req, "chat_template_kwargs", tmpl_kwargs);

    /* llama.cpp-specific: reasoning budget */
    if (p->cfg.thinking_budget >= 0) {
        cJSON_AddNumberToObject(req, "reasoning_budget", p->cfg.thinking_budget);
    }

    /* Build messages array */
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < chat->n_msgs; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", chat->msgs[i].role);

        if (strcmp(chat->msgs[i].role, "tool") == 0 && chat->msgs[i].tool_call_id) {
            cJSON_AddStringToObject(m, "tool_call_id", chat->msgs[i].tool_call_id);
        }

        if (strcmp(chat->msgs[i].role, "assistant") == 0 && chat->msgs[i].tool_calls_json) {
            cJSON *tc = cJSON_Parse(chat->msgs[i].tool_calls_json);
            if (tc) cJSON_AddItemToObject(m, "tool_calls", tc);
            if (chat->msgs[i].content && chat->msgs[i].content[0])
                cJSON_AddStringToObject(m, "content", chat->msgs[i].content);
        } else {
            cJSON_AddStringToObject(m, "content", chat->msgs[i].content);
        }

        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(req, "messages", msgs);

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── Build headers ──────────────────────────────────────────────── */

static struct curl_slist *local_build_headers(provider_t *p) {
    (void)p;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    return headers;
}

/* ── Get endpoint ───────────────────────────────────────────────── */

static const char *local_get_endpoint(provider_t *p) {
    if (!p->_cached_endpoint) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/v1/chat/completions",
                 p->cfg.api_base ? p->cfg.api_base : "http://localhost:8080");
        p->_cached_endpoint = strdup(url);
    }
    return p->_cached_endpoint;
}

/* ── Parse response ─────────────────────────────────────────────── */

static char *local_parse_response(provider_t *p, const char *response_json,
                                  llm_chat_t *chat, llm_stats_t *stats) {
    (void)p;
    cJSON *resp = cJSON_Parse(response_json);
    if (!resp) return NULL;

    /* Extract stats */
    if (stats) {
        cJSON *usage = cJSON_GetObjectItem(resp, "usage");
        if (usage) {
            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
            if (pt) stats->prompt_tokens = pt->valueint;
            if (ct) stats->completion_tokens = ct->valueint;
        }
    }

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(resp);
        return NULL;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) { cJSON_Delete(resp); return NULL; }

    /* Check for tool_calls */
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (fn) {
            cJSON *name = cJSON_GetObjectItem(fn, "name");
            cJSON *args_str = cJSON_GetObjectItem(fn, "arguments");
            cJSON *id = cJSON_GetObjectItem(tc, "id");

            /* Build unified response */
            cJSON *unified = cJSON_CreateObject();
            cJSON *content = cJSON_GetObjectItem(message, "content");
            cJSON_AddStringToObject(unified, "thought",
                                    (content && cJSON_IsString(content)) ?
                                    content->valuestring : "");
            cJSON_AddStringToObject(unified, "action",
                                    name ? name->valuestring : "");

            if (args_str && cJSON_IsString(args_str)) {
                cJSON *args = cJSON_Parse(args_str->valuestring);
                if (args) {
                    cJSON *child = args->child;
                    while (child) {
                        cJSON *next = child->next;
                        cJSON_DetachItemViaPointer(args, child);
                        cJSON_AddItemToObject(unified, child->string, child);
                        child = next;
                    }
                    cJSON_Delete(args);
                }
            }

            char *result = cJSON_PrintUnformatted(unified);
            cJSON_Delete(unified);

            /* Store tool call info */
            if (chat) {
                free(chat->last_tool_call_id);
                chat->last_tool_call_id = (id && cJSON_IsString(id)) ?
                                          strdup(id->valuestring) : NULL;
                free(chat->last_tool_calls_json);
                chat->last_tool_calls_json = cJSON_PrintUnformatted(tool_calls);
            }

            cJSON_Delete(resp);
            return result;
        }
    }

    /* Plain text response */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    char *result = NULL;
    if (content && cJSON_IsString(content)) {
        result = strdup(content->valuestring);
    }

    if (chat) {
        free(chat->last_tool_call_id);
        chat->last_tool_call_id = NULL;
        free(chat->last_tool_calls_json);
        chat->last_tool_calls_json = NULL;
    }

    cJSON_Delete(resp);
    return result;
}

/* ── Build tools ────────────────────────────────────────────────── */

static cJSON *local_build_tools(provider_t *p) {
    (void)p;
    return build_tools_from_registry(PROVIDER_LOCAL);
}

/* ── Fetch model info (local server only) ───────────────────────── */

static size_t local_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *s = userdata;
    str_append(s, ptr, size * nmemb);
    return size * nmemb;
}

static int local_fetch_model_info(provider_t *p, int *context_size,
                                  char **model_name, char **props_json) {
    const char *base = p->cfg.api_base ? p->cfg.api_base : "http://localhost:8080";

    /* Fetch /props for context size */
    if (context_size || props_json) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/props", base);

        str_t response = str_new(4096);
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, local_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && response.len > 0) {
                if (props_json) *props_json = strdup(response.data);

                if (context_size) {
                    cJSON *props = cJSON_Parse(response.data);
                    if (props) {
                        cJSON *dgs = cJSON_GetObjectItem(props, "default_generation_settings");
                        if (dgs) {
                            cJSON *nctx = cJSON_GetObjectItem(dgs, "n_ctx");
                            if (nctx) *context_size = nctx->valueint;
                        }
                        cJSON_Delete(props);
                    }
                }
            }
            str_free(&response);
        }
    }

    /* Fetch /v1/models for model name */
    if (model_name) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/v1/models", base);

        str_t response = str_new(1024);
        CURL *curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, local_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && response.len > 0) {
                cJSON *models = cJSON_Parse(response.data);
                if (models) {
                    cJSON *data = cJSON_GetObjectItem(models, "data");
                    if (data && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
                        cJSON *first = cJSON_GetArrayItem(data, 0);
                        cJSON *id = cJSON_GetObjectItem(first, "id");
                        if (id && cJSON_IsString(id))
                            *model_name = strdup(id->valuestring);
                    }
                    cJSON_Delete(models);
                }
            }
            str_free(&response);
        }
    }

    return 0;
}

/* ── Init ───────────────────────────────────────────────────────── */

void provider_local_init(provider_t *p) {
    p->build_headers    = local_build_headers;
    p->build_request    = local_build_request;
    p->parse_response   = local_parse_response;
    p->parse_sse_event  = NULL;  /* uses shared OpenAI SSE parser */
    p->get_endpoint     = local_get_endpoint;
    p->build_tools      = local_build_tools;
    p->fetch_model_info = local_fetch_model_info;
    p->destroy          = NULL;
}

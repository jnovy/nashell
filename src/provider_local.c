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
    /* Use shared OpenAI-compatible base request builder */
    cJSON *req = build_openai_base_request(p, chat, stream, p->cfg.model_id,
                                           "max_tokens", PROVIDER_LOCAL);

    /* llama.cpp-specific: chat_template_kwargs for thinking mode */
    cJSON *tmpl_kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", p->cfg.enable_thinking);
    cJSON_AddItemToObject(req, "chat_template_kwargs", tmpl_kwargs);

    /* llama.cpp-specific: reasoning budget */
    if (p->cfg.thinking_budget >= 0) {
        cJSON_AddNumberToObject(req, "reasoning_budget", p->cfg.thinking_budget);
    }

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
    return provider_cache_endpoint(p, "%s/v1/chat/completions",
                                   p->cfg.api_base ? p->cfg.api_base : "http://localhost:8080");
}

/* ── Build tools ────────────────────────────────────────────────── */

static cJSON *local_build_tools(provider_t *p) {
    return build_tools_from_registry_filtered(PROVIDER_LOCAL, p->tool_filter);
}

/* ── Fetch model info (local server only) ───────────────────────── */

/* Local servers have /props and /v1/models endpoints — cannot use
 * the shared API provider fetch_model_info. Uses http_get(). */
static int local_fetch_model_info(provider_t *p, int *context_size,
                                  char **model_name, char **props_json) {
    const char *base = p->cfg.api_base ? p->cfg.api_base : "http://localhost:8080";

    /* Fetch /props for context size */
    if (context_size || props_json) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/props", base);

        str_t response = str_new(4096);
        if (http_get(url, 5, &response) == 0 && response.len > 0) {
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

    /* Fetch /v1/models for model name */
    if (model_name) {
        char url[1024];
        snprintf(url, sizeof(url), "%s/v1/models", base);

        str_t response = str_new(1024);
        if (http_get(url, 5, &response) == 0 && response.len > 0) {
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

    return 0;
}

/* ── Init ───────────────────────────────────────────────────────── */

void provider_local_init(provider_t *p) {
    p->build_headers    = local_build_headers;
    p->build_request    = local_build_request;
    p->parse_response   = parse_openai_response;  /* shared OpenAI-format parser */
    p->parse_sse_event  = NULL;  /* uses shared OpenAI SSE parser */
    p->get_endpoint     = local_get_endpoint;
    p->build_tools      = local_build_tools;
    p->fetch_model_info = local_fetch_model_info;
    p->destroy          = NULL;
}

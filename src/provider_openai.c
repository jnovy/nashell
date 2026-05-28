/* provider_openai.c — OpenAI API provider (GPT-4o, GPT-5, etc.)
 *
 * Same wire format as local (OpenAI-compatible), but:
 * - Adds Authorization: Bearer header
 * - No llama.cpp-specific fields (chat_template_kwargs, reasoning_budget)
 * - Uses OpenAI strict mode for tool schemas
 * - Endpoint: https://api.openai.com/v1/chat/completions
 */

#include "provider.h"
#include "str.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Resolve API key from env var ───────────────────────────────── */

static const char *resolve_api_key(const provider_config_t *cfg) {
    const char *env_name = cfg->api_key_env;
    if (!env_name || !env_name[0]) env_name = "OPENAI_API_KEY";
    return getenv(env_name);
}


/* ── Build request ──────────────────────────────────────────────── */

static char *openai_build_request(provider_t *p, llm_chat_t *chat, int stream) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model",
                            p->cfg.model_id ? p->cfg.model_id : "gpt-4o");
    cJSON_AddNumberToObject(req, "max_completion_tokens", p->cfg.max_tokens);
    cJSON_AddNumberToObject(req, "temperature", p->cfg.temperature);
    cJSON_AddBoolToObject(req, "stream", stream);

    if (stream) {
        /* Request usage stats in streaming mode */
        cJSON *stream_opts = cJSON_CreateObject();
        cJSON_AddBoolToObject(stream_opts, "include_usage", 1);
        cJSON_AddItemToObject(req, "stream_options", stream_opts);
    }

    /* Tools with strict mode */
    cJSON *tools = build_tools_from_registry(PROVIDER_OPENAI);
    cJSON_AddItemToObject(req, "tools", tools);

    /* Build messages array — same format as OpenAI expects */
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

static struct curl_slist *openai_build_headers(provider_t *p) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    const char *api_key = resolve_api_key(&p->cfg);
    if (api_key && api_key[0]) {
        char auth[2048];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth);
    } else {
        fprintf(stderr, "[provider/openai] WARNING: no API key found in $%s\n",
                p->cfg.api_key_env ? p->cfg.api_key_env : "OPENAI_API_KEY");
    }

    return headers;
}

/* ── Get endpoint ───────────────────────────────────────────────── */

static const char *openai_get_endpoint(provider_t *p) {
    if (!p->_cached_endpoint) {
        const char *base = p->cfg.api_base;
        if (!base || !base[0]) base = "https://api.openai.com/v1";
        char url[1024];
        snprintf(url, sizeof(url), "%s/chat/completions", base);
        p->_cached_endpoint = strdup(url);
    }
    return p->_cached_endpoint;
}

/* ── Parse response (same as local — OpenAI format) ─────────────── */

/* Reuse local provider's parse_response — same wire format */
extern char *local_parse_response(provider_t *p, const char *response_json,
                                  llm_chat_t *chat, llm_stats_t *stats);

/* Actually, we need our own since local_parse_response is static.
 * The logic is identical, so we duplicate it here. */
static char *openai_parse_response(provider_t *p, const char *response_json,
                                   llm_chat_t *chat, llm_stats_t *stats) {
    (void)p;
    cJSON *resp = cJSON_Parse(response_json);
    if (!resp) return NULL;

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

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (fn) {
            cJSON *name = cJSON_GetObjectItem(fn, "name");
            cJSON *args_str = cJSON_GetObjectItem(fn, "arguments");
            cJSON *id = cJSON_GetObjectItem(tc, "id");

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

static cJSON *openai_build_tools_vtable(provider_t *p) {
    (void)p;
    return build_tools_from_registry(PROVIDER_OPENAI);
}

/* ── Model info (context size lookup) ──────────────────────────── */

/* Known context window sizes for OpenAI models (in tokens).
 * Used when context_size is not explicitly set in config. */
static int openai_lookup_context_size(const char *model_id) {
    if (!model_id) return 0;

    /* o-series reasoning models */
    if (strstr(model_id, "o4-mini"))  return 200000;
    if (strstr(model_id, "o3-mini"))  return 200000;
    if (strstr(model_id, "o3"))       return 200000;
    if (strstr(model_id, "o1-pro"))   return 200000;
    if (strstr(model_id, "o1-mini"))  return 128000;
    if (strstr(model_id, "o1"))       return 200000;

    /* GPT-4.1 family */
    if (strstr(model_id, "gpt-4.1"))  return 1047576;

    /* GPT-4o family */
    if (strstr(model_id, "gpt-4o"))   return 128000;

    /* GPT-4 turbo */
    if (strstr(model_id, "gpt-4-turbo")) return 128000;

    /* GPT-4 (original) */
    if (strstr(model_id, "gpt-4-32k"))   return 32768;
    if (strstr(model_id, "gpt-4"))       return 8192;

    /* GPT-3.5 */
    if (strstr(model_id, "gpt-3.5-turbo-16k")) return 16384;
    if (strstr(model_id, "gpt-3.5"))            return 16384;

    return 0;
}

static int openai_fetch_model_info(provider_t *p, int *context_size,
                                   char **model_name, char **props_json) {
    if (props_json) *props_json = NULL;  /* no /props for API providers */

    if (model_name && p->cfg.model_id)
        *model_name = strdup(p->cfg.model_id);

    if (context_size) {
        /* Use config value if explicitly set, otherwise look up by model */
        if (p->cfg.context_size > 0)
            *context_size = p->cfg.context_size;
        else
            *context_size = openai_lookup_context_size(p->cfg.model_id);
    }

    return 0;
}

/* ── Init ───────────────────────────────────────────────────────── */

void provider_openai_init(provider_t *p) {
    p->build_headers    = openai_build_headers;
    p->build_request    = openai_build_request;
    p->parse_response   = openai_parse_response;
    p->parse_sse_event  = NULL;  /* uses shared OpenAI SSE parser */
    p->get_endpoint     = openai_get_endpoint;
    p->build_tools      = openai_build_tools_vtable;
    p->fetch_model_info = openai_fetch_model_info;
    p->destroy          = NULL;
}

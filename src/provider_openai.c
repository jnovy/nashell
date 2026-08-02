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
#include "tui.h"
#include "nash_log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default fallback model when none configured */
#define OPENAI_DEFAULT_MODEL "gpt-4o"

/* ── Resolve API key from env var ───────────────────────────────── */

static const char *resolve_api_key(const provider_config_t *cfg) {
    const char *env_name = cfg->api_key_env;
    if (!env_name || !env_name[0]) env_name = "OPENAI_API_KEY";
    return getenv(env_name);
}


/* ── Build request ──────────────────────────────────────────────── */

static char *openai_build_request(provider_t *p, llm_chat_t *chat, int stream) {
    /* Use shared OpenAI-compatible base request builder.
     * OpenAI uses "max_completion_tokens" instead of "max_tokens". */
    cJSON *req = build_openai_base_request(p, chat, stream,
                                           p->cfg.model_id ? p->cfg.model_id : OPENAI_DEFAULT_MODEL,
                                           "max_completion_tokens", PROVIDER_OPENAI);

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
        nash_log("[provider/openai] WARNING: no API key found in $%s",
                 p->cfg.api_key_env ? p->cfg.api_key_env : "OPENAI_API_KEY");
        curl_slist_free_all(headers);
        return NULL;
    }

    return headers;
}

/* ── Get endpoint ───────────────────────────────────────────────── */

static const char *openai_get_endpoint(provider_t *p) {
    const char *base = p->cfg.api_base;
    if (!base || !base[0]) base = "https://api.openai.com/v1";
    return provider_cache_endpoint(p, "%s/chat/completions", base);
}

/* ── Build tools ────────────────────────────────────────────────── */

static cJSON *openai_build_tools_vtable(provider_t *p) {
    return build_tools_from_registry_filtered(PROVIDER_OPENAI, p->tool_filter);
}

/* ── Model info ─────────────────────────────────────────────────── */

static int openai_fetch_model_info(provider_t *p, int *context_size,
                                   char **model_name, char **props_json) {
    return provider_api_fetch_model_info(p, context_size, model_name, props_json);
}

/* ── Init ───────────────────────────────────────────────────────── */

void provider_openai_init(provider_t *p) {
    p->build_headers    = openai_build_headers;
    p->build_request    = openai_build_request;
    p->parse_response   = parse_openai_response;  /* shared OpenAI-format parser */
    p->parse_sse_event  = NULL;  /* uses shared OpenAI SSE parser */
    p->get_endpoint     = openai_get_endpoint;
    p->build_tools      = openai_build_tools_vtable;
    p->fetch_model_info = openai_fetch_model_info;
    p->destroy          = NULL;
}

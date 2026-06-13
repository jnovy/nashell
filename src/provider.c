#include "provider.h"
#include "tools.h"
#include "tools_registry.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>

#define PROVIDER_MAX_RETRIES    10
#define PROVIDER_RETRY_BASE_SEC 10

/* ── Shared model context size table ────────────────────────────── */

/* Unified context size lookup for all API providers (OpenAI + Anthropic).
 * Replaces separate openai_lookup_context_size() and anthropic_lookup_context_size()
 * functions. Linear search over prefix → size mappings. */
static const struct {
    const char *prefix;
    int        size;
} MODEL_CONTEXT_SIZES[] = {
    /* OpenAI o-series */
    { "o4-mini",     200000 },
    { "o3-mini",     200000 },
    { "o3",          200000 },
    { "o1-pro",      200000 },
    { "o1-mini",     128000 },
    { "o1",          200000 },
    /* OpenAI GPT-4.1 */
    { "gpt-4.1",    1047576 },
    /* OpenAI GPT-4o */
    { "gpt-4o",      128000 },
    /* OpenAI GPT-4 turbo */
    { "gpt-4-turbo", 128000 },
    /* OpenAI GPT-4 */
    { "gpt-4-32k",    32768 },
    { "gpt-4",         8192 },
    /* OpenAI GPT-3.5 */
    { "gpt-3.5-turbo-16k", 16384 },
    { "gpt-3.5",        16384 },
    /* Anthropic Claude 4.6+ (1M context) */
    { "claude-opus-4-6", 1000000 },
    { "claude-sonnet-4-6", 1000000 },
    { "claude-4-6",      1000000 },
    /* Anthropic Claude 4 */
    { "claude-opus-4", 200000 },
    { "claude-sonnet-4", 200000 },
    { "claude-4",      200000 },
    /* Anthropic Claude 3.7 */
    { "claude-3-7",    200000 },
    { "claude-3.7",    200000 },
    /* Anthropic Claude 3.5 */
    { "claude-3-5",    200000 },
    { "claude-3.5",    200000 },
    /* Anthropic Claude 3 */
    { "claude-3",      200000 },
    /* Anthropic Claude 2.x */
    { "claude-2",     100000 },
    { NULL, 0 }  /* sentinel */
};

/* Look up context window size by model ID prefix.
 * Returns 0 if no match found. */
int provider_lookup_context_size(const char *model_id) {
    if (!model_id) return 0;
    for (int i = 0; MODEL_CONTEXT_SIZES[i].prefix; i++) {
        if (strstr(model_id, MODEL_CONTEXT_SIZES[i].prefix))
            return MODEL_CONTEXT_SIZES[i].size;
    }
    return 0;
}

/* Shared fetch_model_info for API providers (OpenAI, Anthropic, Vertex).
 * These don't have /props endpoints — they use static lookup tables.
 * Sets model_name = strdup(cfg.model_id), context_size from config or lookup,
 * props_json = NULL. */
int provider_api_fetch_model_info(provider_t *p, int *context_size,
                                  char **model_name, char **props_json) {
    if (props_json) *props_json = NULL;

    if (model_name && p->cfg.model_id)
        *model_name = strdup(p->cfg.model_id);

    if (context_size) {
        if (p->cfg.context_size > 0)
            *context_size = p->cfg.context_size;
        else
            *context_size = provider_lookup_context_size(p->cfg.model_id);
    }

    return 0;
}

/* ── Shared endpoint caching helper ─────────────────────────────── */

/* Cache an endpoint URL in provider->_cached_endpoint.
 * Returns the cached string (do NOT free). */
const char *provider_cache_endpoint(provider_t *p, const char *fmt, ...) {
    if (p->_cached_endpoint) return p->_cached_endpoint;
    char url[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(url, sizeof(url), fmt, ap);
    va_end(ap);
    p->_cached_endpoint = strdup(url);
    return p->_cached_endpoint;
}

/* ── Forward declarations for provider constructors ─────────────── */

extern void provider_local_init(provider_t *p);
extern void provider_openai_init(provider_t *p);
extern void provider_anthropic_init(provider_t *p);

/* ── Provider type string conversion ────────────────────────────── */

provider_type_t provider_type_from_str(const char *s) {
    if (!s) return PROVIDER_LOCAL;
    if (strcmp(s, "local") == 0 || strcmp(s, "llama") == 0)
        return PROVIDER_LOCAL;
    if (strcmp(s, "openai") == 0 || strcmp(s, "gpt") == 0)
        return PROVIDER_OPENAI;
    if (strcmp(s, "anthropic") == 0 || strcmp(s, "claude") == 0)
        return PROVIDER_ANTHROPIC;
    if (strcmp(s, "vertex") == 0)
        return PROVIDER_VERTEX;
    nash_log("[provider] unknown type '%s', defaulting to local", s);
    return PROVIDER_LOCAL;
}

const char *provider_type_to_str(provider_type_t t) {
    switch (t) {
        case PROVIDER_LOCAL:     return "local";
        case PROVIDER_OPENAI:    return "openai";
        case PROVIDER_ANTHROPIC: return "anthropic";
        case PROVIDER_VERTEX:    return "vertex";
    }
    return "local";
}

/* ── Provider lifecycle ─────────────────────────────────────────── */

provider_t *provider_create(const provider_config_t *cfg) {
    provider_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->type = cfg->type;
    p->cfg = *cfg;  /* shallow copy of scalars */
    /* Deep-copy all string fields so provider owns its own strings.
     * This avoids aliasing/dangling-pointer hazards when callers free the original. */
    p->cfg.model_id    = cfg->model_id    ? strdup(cfg->model_id)    : NULL;
    p->cfg.api_base    = cfg->api_base    ? strdup(cfg->api_base)    : NULL;
    p->cfg.api_key_env = cfg->api_key_env ? strdup(cfg->api_key_env) : NULL;
    p->cfg.project_id  = cfg->project_id  ? strdup(cfg->project_id)  : NULL;
    p->cfg.region      = cfg->region      ? strdup(cfg->region)      : NULL;

    /* Set defaults */
    if (p->cfg.chars_per_token <= 0) p->cfg.chars_per_token = 3.5f;
    if (p->cfg.max_tokens <= 0) p->cfg.max_tokens = 16384;
    if (p->cfg.temperature < 0) p->cfg.temperature = 0.7f;

    /* Initialize provider-specific vtable */
    switch (cfg->type) {
        case PROVIDER_LOCAL:
            provider_local_init(p);
            break;
        case PROVIDER_OPENAI:
            provider_openai_init(p);
            break;
        case PROVIDER_ANTHROPIC:
        case PROVIDER_VERTEX:
            provider_anthropic_init(p);
            break;
    }

    return p;
}

void provider_free(provider_t *p) {
    if (!p) return;
    if (p->destroy) p->destroy(p);
    free(p->_cached_endpoint);
    free(p->_cached_auth_token);
    /* Free error diagnostic strings (heap-allocated on provider errors) */
    free(p->last_error);
    free(p->last_error_request);
    free(p->last_error_response);
    /* Free all deep-copied string fields from provider_create */
    free((char *)p->cfg.model_id);
    free((char *)p->cfg.api_base);
    free((char *)p->cfg.api_key_env);
    free((char *)p->cfg.project_id);
    free((char *)p->cfg.region);
    free(p);
}

/* ── Shared tool registry → provider-specific JSON ──────────────── */

#include "tools_registry.h"

/* Recursively add "additionalProperties": false to all objects (OpenAI strict mode) */
static void strict_object(cJSON *schema) {
    if (!schema || !cJSON_IsObject(schema)) return;
    cJSON *type = cJSON_GetObjectItem(schema, "type");
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "object") == 0) {
        cJSON *props = cJSON_GetObjectItem(schema, "properties");
        if (props) {
            cJSON *child = props->child;
            while (child) {
                strict_object(child);
                child = child->next;
            }
            if (!cJSON_GetObjectItem(schema, "additionalProperties"))
                cJSON_AddBoolToObject(schema, "additionalProperties", 0);
        }
    }
}

cJSON *build_tools_from_registry(provider_type_t type) {
    return build_tools_from_registry_filtered(type, NULL);
}

cJSON *build_tools_from_registry_filtered(provider_type_t type,
                                           const struct tool_filter_t *filter) {
    cJSON *tools = cJSON_CreateArray();

    for (int i = 0; TOOL_REGISTRY[i].name; i++) {
        const tool_def_t *td = &TOOL_REGISTRY[i];

        /* Apply tool filter if provided */
        if (filter) {
            if (filter->allowed) {
                int found = 0;
                for (int j = 0; j < filter->n_allowed; j++)
                    if (strcmp(td->name, filter->allowed[j]) == 0) { found = 1; break; }
                if (!found) continue;
            }
            if (filter->blocked) {
                int skip = 0;
                for (int j = 0; j < filter->n_blocked; j++)
                    if (strcmp(td->name, filter->blocked[j]) == 0) { skip = 1; break; }
                if (skip) continue;
            }
        }

        /* Check for per-tool description override from model profile */
        const char *desc = td->description;
        if (filter && filter->n_descs > 0) {
            for (int j = 0; j < filter->n_descs; j++) {
                if (strcmp(td->name, filter->desc_names[j]) == 0) {
                    desc = filter->desc_values[j];
                    break;
                }
            }
        }

        cJSON *params = cJSON_Parse(td->params_json);

        if (type == PROVIDER_ANTHROPIC || type == PROVIDER_VERTEX) {
            /* Anthropic format: {"name":"X","description":"Y","input_schema":{...}} */
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", td->name);
            cJSON_AddStringToObject(t, "description", desc);
            if (params) cJSON_AddItemToObject(t, "input_schema", params);
            cJSON_AddItemToArray(tools, t);
        } else {
            /* OpenAI / Local format: {"type":"function","function":{"name":"X",...}} */
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", td->name);
            cJSON_AddStringToObject(fn, "description", desc);
            if (params) {
                if (type == PROVIDER_OPENAI) strict_object(params);
                cJSON_AddItemToObject(fn, "parameters", params);
            }
            if (type == PROVIDER_OPENAI)
                cJSON_AddBoolToObject(fn, "strict", 1);
            cJSON_AddItemToObject(t, "function", fn);
            cJSON_AddItemToArray(tools, t);
        }
    }
    return tools;
}

/* ── Shared: build OpenAI-compatible base request ───────────────── */

/* Build the common part of an OpenAI-compatible request body.
 * Handles: model, max_tokens (or max_completion_tokens), temperature,
 * stream, stream_options (include_usage), tools, messages.
 *
 * Parameters:
 *   p          — provider config
 *   chat       — chat history
 *   stream     — whether to stream
 *   model_id   — model identifier (passed through)
 *   max_token_field — "max_tokens" or "max_completion_tokens"
 *   provider_type — for build_tools_from_registry()
 *
 * Returns a cJSON object (caller owns, caller adds extra fields if needed).
 * Shared by local and openai providers. */
cJSON *build_openai_base_request(provider_t *p, llm_chat_t *chat,
                                 int stream, const char *model_id,
                                 const char *max_token_field,
                                 provider_type_t provider_type) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model_id ? model_id : "gpt-4o");
    cJSON_AddNumberToObject(req, max_token_field, p->cfg.max_tokens);
    cJSON_AddNumberToObject(req, "temperature", p->cfg.temperature);
    cJSON_AddBoolToObject(req, "stream", stream);

    if (stream) {
        cJSON *so = cJSON_CreateObject();
        cJSON_AddBoolToObject(so, "include_usage", 1);
        cJSON_AddItemToObject(req, "stream_options", so);
    }

    /* Tools — use filter if set on provider (allows model profile tool restrictions) */
    cJSON *tools = build_tools_from_registry_filtered(provider_type, p->tool_filter);
    cJSON_AddItemToObject(req, "tools", tools);

    /* Messages — use shared helper */
    cJSON *msgs = build_messages_json(chat);
    cJSON_AddItemToObject(req, "messages", msgs);

    return req;
}

/* ── Shared: extract stats from OpenAI-format response ──────────── */

/* Extract prompt_tokens/completion_tokens from OpenAI-compatible usage JSON.
 * Shared by local and openai providers (identical wire format). */
void extract_openai_stats(cJSON *resp, llm_stats_t *stats) {
    if (!resp || !stats) return;
    cJSON *usage = cJSON_GetObjectItem(resp, "usage");
    if (!usage) return;
    cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
    cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
    if (pt) stats->prompt_tokens = pt->valueint;
    if (ct) stats->completion_tokens = ct->valueint;
}

/* ── Shared: build messages array from llm_chat_t ───────────────── */

/* Build the "messages" JSON array for an OpenAI-compatible request.
 * Handles tool_calls, tool_call_id, and content fields correctly.
 * Caller must cJSON_AddItemToObject(req, "messages", result). */
cJSON *build_messages_json(llm_chat_t *chat) {
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
    return msgs;
}

/* ── Shared: parse OpenAI-format response ───────────────────────── */

/* Parse a non-streaming OpenAI-compatible response JSON.
 * Handles tool_calls and plain text responses.
 * Returns unified JSON: {"thought":"...", "action":"name", ...params} for tool calls,
 * or plain content string for text responses.
 * Sets chat->last_tool_call_id and chat->last_tool_calls_json for tool calls.
 * Returns NULL on parse failure.
 * Shared by local and openai providers (identical wire format). */
char *parse_openai_response(provider_t *p, const char *response_json,
                            llm_chat_t *chat, llm_stats_t *stats) {
    (void)p;
    cJSON *resp = cJSON_Parse(response_json);
    if (!resp) return NULL;

    /* Extract stats */
    if (stats) {
        extract_openai_stats(resp, stats);
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

/* ── SSE streaming state (shared across providers) ──────────────── */

typedef struct {
    str_t          line_buf;
    str_t          full_content;
    provider_token_fn on_token;
    void          *userdata;
    llm_stats_t   *stats;
    size_t         max_response;
    int            repeat_threshold;
    int            repeat_count;
    int            stopped;
    str_t          tool_call_name;
    str_t          tool_call_args;
    char          *tool_call_id;
    int            has_tool_call;
    int            last_token_idx;
    /* Anthropic-specific SSE state */
    int            in_tool_use;       /* currently inside a tool_use block */
    str_t          thinking_content;  /* accumulated thinking text */
    provider_t    *provider;          /* back-pointer for vtable dispatch */
    /* Wall-clock streaming timing (fallback when server doesn't report t/s) */
    struct timespec first_token_time;   /* timestamp of first content token */
    struct timespec request_start_time; /* timestamp when HTTP request started */
    int            first_token_seen;    /* 1 = first_token_time is valid */
    int            streaming_token_count; /* number of content tokens received */
} provider_sse_state_t;

/* ── SSE line processing (OpenAI-compatible format) ─────────────── */

static void sse_process_line_openai(provider_sse_state_t *st, const char *line) {
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *json_str = line + 6;
    if (strcmp(json_str, "[DONE]") == 0) return;

    cJSON *data = cJSON_Parse(json_str);
    if (!data) return;

    cJSON *choices = cJSON_GetObjectItem(data, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        /* Check for usage in final chunk */
        cJSON *usage = cJSON_GetObjectItem(data, "usage");
        if (usage && st->stats) {
            cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
            if (pt) st->stats->prompt_tokens = pt->valueint;
            if (ct) st->stats->completion_tokens = ct->valueint;
        }
        cJSON_Delete(data);
        return;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = cJSON_GetObjectItem(choice, "delta");
    if (!delta) { cJSON_Delete(data); return; }

    /* Tool call chunks */
    cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        st->has_tool_call = 1;
        cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);

        /* Tool call ID (first chunk only) */
        cJSON *id = cJSON_GetObjectItem(tc, "id");
        if (id && cJSON_IsString(id) && !st->tool_call_id) {
            st->tool_call_id = strdup(id->valuestring);
        }

        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (fn) {
            cJSON *name = cJSON_GetObjectItem(fn, "name");
            if (name && cJSON_IsString(name))
                str_append_cstr(&st->tool_call_name, name->valuestring);
            cJSON *args = cJSON_GetObjectItem(fn, "arguments");
            if (args && cJSON_IsString(args))
                str_append_cstr(&st->tool_call_args, args->valuestring);
        }
    }

    /* Content delta */
    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (content && cJSON_IsString(content)) {
        const char *text = content->valuestring;
        size_t tlen = strlen(text);

        if (st->max_response > 0 &&
            st->full_content.len + tlen > st->max_response) {
            st->stopped = 1;
            cJSON_Delete(data);
            return;
        }

        str_append_cstr(&st->full_content, text);

        /* Wall-clock streaming timing for t/s computation */
        st->streaming_token_count++;
        if (!st->first_token_seen) {
            clock_gettime(CLOCK_MONOTONIC, &st->first_token_time);
            st->first_token_seen = 1;
        }

        /* Repeat detection */
        if (st->repeat_threshold > 0 && tlen > 0) {
            /* Simple: count consecutive identical single-char tokens */
            if (tlen == 1 && st->full_content.len >= 2 &&
                st->full_content.data[st->full_content.len - 1] ==
                st->full_content.data[st->full_content.len - 2]) {
                st->repeat_count++;
                if (st->repeat_count >= st->repeat_threshold) {
                    st->stopped = 1;
                    cJSON_Delete(data);
                    return;
                }
            } else {
                st->repeat_count = 0;
            }
        }

        /* Token callback */
        if (st->on_token && !st->has_tool_call) {
            st->last_token_idx++;
            st->on_token(text, st->userdata);
        }
    }

    /* Usage stats from streaming response */
    cJSON *usage = cJSON_GetObjectItem(data, "usage");
    if (usage && st->stats) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *pps = cJSON_GetObjectItem(usage, "prompt_per_second");
        cJSON *tps = cJSON_GetObjectItem(usage, "predicted_per_second");
        if (pt) st->stats->prompt_tokens = pt->valueint;
        if (ct) st->stats->completion_tokens = ct->valueint;
        if (pps) st->stats->prompt_per_second = pps->valuedouble;
        if (tps) st->stats->predicted_per_second = tps->valuedouble;
    }

    cJSON_Delete(data);
}

/* ── Anthropic SSE line processing ──────────────────────────────── */

static void sse_process_line_anthropic(provider_sse_state_t *st, const char *line) {
    /* Anthropic SSE format:
     * event: message_start / content_block_start / content_block_delta /
     *        content_block_stop / message_delta / message_stop
     * data: {...}
     *
     * We track state: are we in a text block or tool_use block?
     */
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *json_str = line + 6;

    cJSON *data = cJSON_Parse(json_str);
    if (!data) return;

    cJSON *type = cJSON_GetObjectItem(data, "type");
    if (!type || !cJSON_IsString(type)) { cJSON_Delete(data); return; }

    const char *event_type = type->valuestring;

    if (strcmp(event_type, "message_start") == 0) {
        /* Extract usage from message_start */
        cJSON *message = cJSON_GetObjectItem(data, "message");
        if (message && st->stats) {
            cJSON *usage = cJSON_GetObjectItem(message, "usage");
            if (usage) {
                cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
                if (it) {
                    st->stats->prompt_tokens = it->valueint;
                    nash_log("[provider/sse] message_start: input_tokens=%d (valueint=%d, valuedouble=%.0f)",
                             st->stats->prompt_tokens, it->valueint, it->valuedouble);
                } else {
                    nash_log("[provider/sse] message_start: no input_tokens in usage! usage=%s",
                             cJSON_PrintUnformatted(usage));
                }
            } else {
                nash_log("[provider/sse] message_start: no usage in message!");
            }
        }
    }
    else if (strcmp(event_type, "content_block_start") == 0) {
        cJSON *cb = cJSON_GetObjectItem(data, "content_block");
        if (cb) {
            cJSON *cb_type = cJSON_GetObjectItem(cb, "type");
            if (cb_type && cJSON_IsString(cb_type)) {
                if (strcmp(cb_type->valuestring, "tool_use") == 0) {
                    st->in_tool_use = 1;
                    st->has_tool_call = 1;
                    cJSON *id = cJSON_GetObjectItem(cb, "id");
                    if (id && cJSON_IsString(id)) {
                        free(st->tool_call_id);
                        st->tool_call_id = strdup(id->valuestring);
                    }
                    cJSON *name = cJSON_GetObjectItem(cb, "name");
                    if (name && cJSON_IsString(name)) {
                        str_clear(&st->tool_call_name);
                        str_append_cstr(&st->tool_call_name, name->valuestring);
                    }
                } else {
                    st->in_tool_use = 0;
                }
            }
        }
    }
    else if (strcmp(event_type, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(data, "delta");
        if (delta) {
            cJSON *delta_type = cJSON_GetObjectItem(delta, "type");
            if (delta_type && cJSON_IsString(delta_type)) {
                if (strcmp(delta_type->valuestring, "text_delta") == 0) {
                    cJSON *text = cJSON_GetObjectItem(delta, "text");
                    if (text && cJSON_IsString(text)) {
                        const char *t = text->valuestring;
                        str_append_cstr(&st->full_content, t);
                        /* Wall-clock streaming timing for t/s computation */
                        st->streaming_token_count++;
                        if (!st->first_token_seen) {
                            clock_gettime(CLOCK_MONOTONIC, &st->first_token_time);
                            st->first_token_seen = 1;
                        }
                        if (st->on_token && !st->in_tool_use) {
                            st->last_token_idx++;
                            st->on_token(t, st->userdata);
                        }
                    }
                }
                else if (strcmp(delta_type->valuestring, "input_json_delta") == 0) {
                    cJSON *partial = cJSON_GetObjectItem(delta, "partial_json");
                    if (partial && cJSON_IsString(partial)) {
                        str_append_cstr(&st->tool_call_args, partial->valuestring);
                    }
                    /* Count tool input deltas for wall-clock t/s timing */
                    st->streaming_token_count++;
                    if (!st->first_token_seen) {
                        clock_gettime(CLOCK_MONOTONIC, &st->first_token_time);
                        st->first_token_seen = 1;
                    }
                }
                else if (strcmp(delta_type->valuestring, "thinking") == 0) {
                    cJSON *thinking = cJSON_GetObjectItem(delta, "thinking");
                    if (thinking && cJSON_IsString(thinking)) {
                        str_append_cstr(&st->thinking_content, thinking->valuestring);
                    }
                    /* Count thinking deltas for wall-clock t/s timing */
                    st->streaming_token_count++;
                    if (!st->first_token_seen) {
                        clock_gettime(CLOCK_MONOTONIC, &st->first_token_time);
                        st->first_token_seen = 1;
                    }
                }
            }
        }
    }
    else if (strcmp(event_type, "content_block_stop") == 0) {
        st->in_tool_use = 0;
    }
    else if (strcmp(event_type, "message_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(data, "delta");
        if (delta && st->stats) {
            /* stop_reason is in message_delta */
        }
        cJSON *usage = cJSON_GetObjectItem(data, "usage");
        if (usage && st->stats) {
            cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
            if (ot) st->stats->completion_tokens = ot->valueint;
        }
    }

    cJSON_Delete(data);
}

/* ── Shared SSE write callback ──────────────────────────────────── */

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    provider_sse_state_t *st = userdata;
    size_t total = size * nmemb;
    const char *data = ptr;

    if (st->stopped) return 0;  /* abort transfer */

    for (size_t i = 0; i < total; i++) {
        if (data[i] == '\n') {
            const char *line = str_cstr(&st->line_buf);
            if (st->provider->type == PROVIDER_ANTHROPIC ||
                st->provider->type == PROVIDER_VERTEX) {
                sse_process_line_anthropic(st, line);
            } else {
                sse_process_line_openai(st, line);
            }
            str_clear(&st->line_buf);
        } else {
            str_append(&st->line_buf, &data[i], 1);
        }
    }

    return total;
}

/* ── Build unified result from SSE state ────────────────────────── */

static char *build_sse_result(provider_sse_state_t *st, llm_chat_t *chat) {
    char *result = NULL;

    if (st->has_tool_call && st->tool_call_name.len > 0) {
        /* Build unified JSON: {"thought":"...", "action":"tool_name", ...params} */
        cJSON *unified = cJSON_CreateObject();
        /* Skip whitespace-only text content (e.g. "\n\n" before tool_use) —
         * treating it as thought would cause misaligned display in reactRX.md */
        const char *thought_text = "";
        if (st->full_content.len > 0 &&
            !is_whitespace_only(str_cstr(&st->full_content)))
            thought_text = str_cstr(&st->full_content);
        cJSON_AddStringToObject(unified, "thought", thought_text);
        cJSON_AddStringToObject(unified, "action",
                                str_cstr(&st->tool_call_name));

        /* Parse and merge tool arguments */
        if (st->tool_call_args.len > 0) {
            cJSON *args = cJSON_Parse(str_cstr(&st->tool_call_args));
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

        result = cJSON_PrintUnformatted(unified);
        cJSON_Delete(unified);

        /* Store tool call info for conversation threading */
        if (chat) {
            free(chat->last_tool_call_id);
            chat->last_tool_call_id = st->tool_call_id ?
                                      strdup(st->tool_call_id) : NULL;

            /* Build tool_calls JSON array for history */
            cJSON *tc_arr = cJSON_CreateArray();
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id",
                                    st->tool_call_id ? st->tool_call_id : "call_0");
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", str_cstr(&st->tool_call_name));
            cJSON_AddStringToObject(fn, "arguments",
                                    st->tool_call_args.len > 0 ?
                                    str_cstr(&st->tool_call_args) : "{}");
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tc_arr, tc);

            free(chat->last_tool_calls_json);
            chat->last_tool_calls_json = cJSON_PrintUnformatted(tc_arr);
            cJSON_Delete(tc_arr);
        }
    } else if (st->full_content.len > 0) {
        result = strdup(str_cstr(&st->full_content));
        if (chat) {
            free(chat->last_tool_call_id);
            chat->last_tool_call_id = NULL;
            free(chat->last_tool_calls_json);
            chat->last_tool_calls_json = NULL;
        }
    }

    return result;
}

/* ── High-level: non-streaming completion ───────────────────────── */

char *provider_complete(provider_t *p, llm_chat_t *chat, llm_stats_t *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));

    char *endpoint = NULL;
    if (p->get_endpoint) {
        endpoint = strdup(p->get_endpoint(p));
    }
    if (!endpoint) return NULL;

    char *req_body = p->build_request(p, chat, 0);
    if (!req_body) { free(endpoint); return NULL; }

    str_t response = str_new(4096);
    cJSON *resp = NULL;

    for (int attempt = 1; attempt <= PROVIDER_MAX_RETRIES; attempt++) {
        str_clear(&response);

        CURL *curl = curl_easy_init();
        if (!curl) { free(req_body); free(endpoint); str_free(&response); return NULL; }

        struct curl_slist *headers = p->build_headers(p);

        curl_easy_setopt(curl, CURLOPT_URL, endpoint);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        if (p->cfg.llm_timeout > 0)
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)p->cfg.llm_timeout);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            nash_log("[provider] curl error: %s (attempt %d/%d, retry in %ds)",
                     curl_easy_strerror(res), attempt, PROVIDER_MAX_RETRIES, delay);
            if (attempt < PROVIDER_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body); free(endpoint); str_free(&response);
            return NULL;
        }

        /* HTTP 401/403: auth failure — invalidate cached token and
         * retry once with a fresh token (Vertex AI OAuth2). */
        if ((http_code == 401 || http_code == 403) &&
            p->type == PROVIDER_VERTEX && attempt == 1) {
            p->_auth_token_expiry = 0;  /* force token refresh */
            str_clear(&response);
            continue;
        }

        resp = cJSON_Parse(response.data);
        str_free(&response);
        if (!resp) {
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            nash_log("[provider] JSON parse failed (attempt %d/%d, retry in %ds)",
                     attempt, PROVIDER_MAX_RETRIES, delay);
            if (attempt < PROVIDER_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body); free(endpoint);
            return NULL;
        }

        /* Check for API errors (all providers return {"error": ...}) */
        cJSON *error = cJSON_GetObjectItem(resp, "error");
        if (error) {
            const char *msg = "";
            if (cJSON_IsString(error)) msg = error->valuestring;
            else {
                cJSON *emsg = cJSON_GetObjectItem(error, "message");
                if (emsg && cJSON_IsString(emsg)) msg = emsg->valuestring;
            }
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            nash_log("[provider] API error: %s (attempt %d/%d, retry in %ds)",
                     msg, attempt, PROVIDER_MAX_RETRIES, delay);
            cJSON_Delete(resp);
            if (attempt < PROVIDER_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body); free(endpoint);
            return NULL;
        }

        break;  /* success */
    }

    free(req_body);
    free(endpoint);

    /* Dispatch to provider-specific response parser */
    char *result = NULL;
    if (p->parse_response) {
        char *resp_str = cJSON_PrintUnformatted(resp);
        result = p->parse_response(p, resp_str, chat, stats);
        free(resp_str);
    }

    cJSON_Delete(resp);
    return result;
}

/* ── High-level: streaming completion ───────────────────────────── */

char *provider_complete_stream(provider_t *p, llm_chat_t *chat,
                               llm_stats_t *stats, provider_token_fn on_token,
                               void *userdata, int max_response_bytes,
                               int repeat_threshold) {
    if (stats) memset(stats, 0, sizeof(*stats));

    const char *endpoint = p->get_endpoint ? p->get_endpoint(p) : NULL;
    if (!endpoint) return NULL;

    char *req_body = p->build_request(p, chat, 1);
    if (!req_body) return NULL;

    /* Set up SSE state */
    provider_sse_state_t st = {
        .line_buf         = str_new(256),
        .full_content     = str_new(4096),
        .on_token         = on_token,
        .userdata         = userdata,
        .stats            = stats,
        .max_response     = (size_t)max_response_bytes,
        .repeat_threshold = repeat_threshold,
        .repeat_count     = 0,
        .stopped          = 0,
        .tool_call_name   = str_new(64),
        .tool_call_args   = str_new(1024),
        .tool_call_id     = NULL,
        .has_tool_call    = 0,
        .last_token_idx   = 0,
        .in_tool_use      = 0,
        .thinking_content = str_new(256),
        .provider         = p,
        .first_token_time    = {0, 0},
        .request_start_time  = {0, 0},
        .first_token_seen    = 0,
        .streaming_token_count = 0,
    };

    char *result = NULL;
    for (int attempt = 1; attempt <= PROVIDER_MAX_RETRIES; attempt++) {
        str_clear(&st.line_buf);
        str_clear(&st.full_content);
        str_clear(&st.tool_call_name);
        str_clear(&st.tool_call_args);
        str_clear(&st.thinking_content);
        free(st.tool_call_id); st.tool_call_id = NULL;
        st.has_tool_call = 0;
        st.in_tool_use = 0;
        st.stopped = 0;
        st.repeat_count = 0;
        st.last_token_idx = 0;
        st.first_token_seen = 0;
        st.streaming_token_count = 0;
        if (stats) memset(stats, 0, sizeof(*stats));

        CURL *curl = curl_easy_init();
        if (!curl) {
            free(req_body);
            goto cleanup;
        }

        struct curl_slist *headers = p->build_headers(p);

        curl_easy_setopt(curl, CURLOPT_URL, endpoint);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
        /* Wall-clock timeout for streaming LLM calls (configurable via
         * [limits] llm_timeout in config.toml, default 600s = 10 min).
         * 0 = no limit. */
        if (p->cfg.llm_timeout > 0)
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)p->cfg.llm_timeout);

        clock_gettime(CLOCK_MONOTONIC, &st.request_start_time);
        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        /* Process any remaining data in line buffer */
        if (st.line_buf.len > 0) {
            if (p->type == PROVIDER_ANTHROPIC || p->type == PROVIDER_VERTEX) {
                sse_process_line_anthropic(&st, str_cstr(&st.line_buf));
            } else {
                sse_process_line_openai(&st, str_cstr(&st.line_buf));
            }
        }

        /* Handle HTTP errors: 4xx = fatal, 5xx = return NULL immediately.
         * Don't retry 500s at the provider level — the same request body
         * produces the same malformed output (near-deterministic with
         * grammar-constrained generation). Let react.c handle recovery
         * by stripping context and retrying with a modified prompt. */
        if (http_code >= 400) {
            nash_log("[provider] HTTP %ld error: %.2000s",
                     http_code,
                     st.full_content.len > 0 ? str_cstr(&st.full_content) : "(empty)");
            if (http_code > 500 && attempt < PROVIDER_MAX_RETRIES) {
                /* Only retry on transient gateway/overload errors (502/503/504).
                 * HTTP 500 from llama.cpp is almost always deterministic —
                 * the same request body produces the same malformed output.
                 * Retrying wastes up to 550 seconds (10 retries × linear backoff)
                 * while the user sees a "stuck" process. Let react.c handle
                 * recovery by reformulating the scratchpad/context. */
                int delay = attempt * PROVIDER_RETRY_BASE_SEC;
                nash_log("[provider] transient %ld error (attempt %d/%d, "
                         "retry in %ds)",
                         http_code, attempt, PROVIDER_MAX_RETRIES, delay);
                sleep(delay);
                continue;
            }
            /* HTTP 401/403: auth failure — invalidate cached token and
             * retry once with a fresh token.  Only applies to Vertex AI
             * (OAuth2 tokens expire and can be refreshed via gcloud). */
            if ((http_code == 401 || http_code == 403) &&
                p->type == PROVIDER_VERTEX && attempt == 1) {
                p->_auth_token_expiry = 0;  /* force token refresh */
                continue;
            }
            /* 4xx, deterministic 500, or final attempt: return NULL */
            /* Populate error diagnostics for react.c journal entry */
            free(p->last_error);
            {
                char ebuf[512];
                const char *body = st.full_content.len > 0 ?
                                   str_cstr(&st.full_content) : "(empty)";
                snprintf(ebuf, sizeof(ebuf), "HTTP %ld: %.400s", http_code, body);
                p->last_error = strdup(ebuf);
            }
            free(p->last_error_request);
            p->last_error_request = req_body;  /* transfer ownership */
            req_body = NULL;
            free(p->last_error_response);
            p->last_error_response = (st.full_content.len > 0)
                ? strdup(str_cstr(&st.full_content)) : NULL;
            goto cleanup;
        }

        if (res != CURLE_OK && !st.stopped) {
            /* Don't retry on timeout — it means the LLM response is too long
             * (e.g., runaway thinking), not a transient network error.
             * Retrying would just burn another 300s+ per attempt. */
            if (res == CURLE_OPERATION_TIMEDOUT) {
                nash_log("[provider] LLM call timed out after %lds "
                         "(streaming_tokens=%d) — not retrying",
                         (long)300, st.streaming_token_count);
                free(p->last_error);
                p->last_error = strdup("LLM call timed out (response too long)");
                free(p->last_error_request);
                p->last_error_request = req_body;
                req_body = NULL;
                free(p->last_error_response);
                p->last_error_response = (st.full_content.len > 0)
                    ? strdup(str_cstr(&st.full_content)) : NULL;
                goto cleanup;
            }
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            nash_log("[provider] curl error: %s (attempt %d/%d, retry in %ds)",
                     curl_easy_strerror(res), attempt, PROVIDER_MAX_RETRIES, delay);
            if (attempt < PROVIDER_MAX_RETRIES) { sleep(delay); continue; }
            /* Populate error diagnostics for react.c journal entry */
            free(p->last_error);
            {
                char ebuf[512];
                snprintf(ebuf, sizeof(ebuf), "curl error: %s", curl_easy_strerror(res));
                p->last_error = strdup(ebuf);
            }
            free(p->last_error_request);
            p->last_error_request = req_body;  /* transfer ownership */
            req_body = NULL;
            free(p->last_error_response);
            p->last_error_response = NULL;
            goto cleanup;
        }

        break;  /* success */
    }

    /* Compute wall-clock streaming t/s as fallback when server doesn't report it.
     * This makes gen/pp t/s available for all providers (Anthropic, Vertex, OpenAI)
     * even when the server doesn't include per_second fields in the response.
     *
     * For API providers (Anthropic/Vertex), streaming_token_count counts SSE
     * content_block_delta events (text_delta + input_json_delta + thinking),
     * which are chunks, not actual tokens.  When completion_tokens is available
     * from the usage stats, use that instead for accurate t/s. */
    if (stats && st.first_token_seen && st.streaming_token_count > 1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* Generation speed: completion_tokens / time_since_first_token.
         * Prefer completion_tokens from API usage stats (accurate token count).
         * Fall back to streaming_token_count (SSE chunk count) if unavailable. */
        if (stats->predicted_per_second <= 0) {
            double gen_elapsed = (now.tv_sec - st.first_token_time.tv_sec) +
                                 (now.tv_nsec - st.first_token_time.tv_nsec) / 1e9;
            if (gen_elapsed > 0.1) {
                int gen_count = (stats->completion_tokens > 0)
                    ? stats->completion_tokens
                    : st.streaming_token_count - 1;
                if (gen_count > 0)
                    stats->predicted_per_second = (double)gen_count / gen_elapsed;
            }
        }

        /* Prompt processing speed: prompt_tokens / time_to_first_token.
         * Time-to-first-token approximates prompt processing time. */
        if (stats->prompt_per_second <= 0 && stats->prompt_tokens > 0) {
            double pp_elapsed = (st.first_token_time.tv_sec - st.request_start_time.tv_sec) +
                                (st.first_token_time.tv_nsec - st.request_start_time.tv_nsec) / 1e9;
            if (pp_elapsed > 0.1) {
                stats->prompt_per_second = stats->prompt_tokens / pp_elapsed;
            }
        }
    }

    if (stats)
        nash_log("[provider/complete] final stats: prompt_tokens=%d completion_tokens=%d pp=%.1f gen=%.1f",
                 stats->prompt_tokens, stats->completion_tokens,
                 stats->prompt_per_second, stats->predicted_per_second);

    free(req_body);
    result = build_sse_result(&st, chat);

cleanup:
    str_free(&st.line_buf);
    str_free(&st.full_content);
    str_free(&st.tool_call_name);
    str_free(&st.tool_call_args);
    str_free(&st.thinking_content);
    free(st.tool_call_id);
    return result;
}

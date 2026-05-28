#include "provider.h"
#include "str.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define PROVIDER_MAX_RETRIES    10
#define PROVIDER_RETRY_BASE_SEC 10

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
    fprintf(stderr, "[provider] unknown type '%s', defaulting to local\n", s);
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
    p->cfg = *cfg;  /* shallow copy — caller must keep strings alive */

    /* Set defaults */
    if (p->cfg.chars_per_token <= 0) p->cfg.chars_per_token = 3.5f;
    if (p->cfg.max_tokens <= 0) p->cfg.max_tokens = 16384;
    if (p->cfg.temperature <= 0) p->cfg.temperature = 0.7f;

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
    free(p);
}

/* ── Shared curl write callback ─────────────────────────────────── */

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *s = userdata;
    str_append(s, ptr, size * nmemb);
    return size * nmemb;
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
                if (it) st->stats->prompt_tokens = it->valueint;
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
                }
                else if (strcmp(delta_type->valuestring, "thinking") == 0) {
                    cJSON *thinking = cJSON_GetObjectItem(delta, "thinking");
                    if (thinking && cJSON_IsString(thinking)) {
                        str_append_cstr(&st->thinking_content, thinking->valuestring);
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
        cJSON_AddStringToObject(unified, "thought",
                                st->full_content.len > 0 ?
                                str_cstr(&st->full_content) : "");
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
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            fprintf(stderr, "[provider] curl error: %s (attempt %d/%d, retry in %ds)\n",
                    curl_easy_strerror(res), attempt, PROVIDER_MAX_RETRIES, delay);
            if (attempt < PROVIDER_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body); free(endpoint); str_free(&response);
            return NULL;
        }

        resp = cJSON_Parse(response.data);
        str_free(&response);
        if (!resp) {
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            fprintf(stderr, "[provider] JSON parse failed (attempt %d/%d, retry in %ds)\n",
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
            fprintf(stderr, "[provider] API error: %s (attempt %d/%d, retry in %ds)\n",
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

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
            fprintf(stderr, "[provider] HTTP %ld error: %.2000s\n",
                    http_code,
                    st.full_content.len > 0 ? str_cstr(&st.full_content) : "(empty)");
            if (http_code >= 500 && attempt < PROVIDER_MAX_RETRIES) {
                /* Only retry on transient server errors (502/503/504).
                 * 500 with "parse" in the body = deterministic model output
                 * error — retrying is pointless. */
                const char *body = st.full_content.len > 0 ?
                                   str_cstr(&st.full_content) : "";
                if (http_code == 500 && strstr(body, "parse")) {
                    fprintf(stderr, "[provider] deterministic parse error — "
                            "not retrying (let react handle recovery)\n");
                } else {
                    int delay = attempt * PROVIDER_RETRY_BASE_SEC;
                    fprintf(stderr, "[provider] server error (attempt %d/%d, "
                            "retry in %ds)\n",
                            attempt, PROVIDER_MAX_RETRIES, delay);
                    sleep(delay);
                    continue;
                }
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
            int delay = attempt * PROVIDER_RETRY_BASE_SEC;
            fprintf(stderr, "[provider] curl error: %s (attempt %d/%d, retry in %ds)\n",
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

/* provider_anthropic.c — Anthropic API + Vertex AI provider
 *
 * Handles both direct Anthropic API and Vertex AI endpoints.
 * Key differences from OpenAI format:
 * - Messages use content blocks (text, tool_use, tool_result)
 * - System prompt is a separate parameter, not a message
 * - Tool schemas use input_schema (not parameters)
 * - SSE events are typed (content_block_start/delta/stop)
 * - Tool results go in user messages as tool_result blocks
 *
 * Auth:
 * - Direct Anthropic: x-api-key header from ANTHROPIC_API_KEY env
 * - Vertex AI: Authorization Bearer from `gcloud auth print-access-token`
 */

#include "provider.h"
#include "str.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Auth helpers ───────────────────────────────────────────────── */

static const char *get_anthropic_api_key(const provider_config_t *cfg) {
    const char *env_name = cfg->api_key_env;
    if (!env_name || !env_name[0]) env_name = "ANTHROPIC_API_KEY";
    return getenv(env_name);
}

/* Get OAuth2 access token for Vertex AI via gcloud CLI.
 * Caches token and refreshes when expired. */
static const char *get_vertex_token(provider_t *p) {
    time_t now = time(NULL);

    /* Return cached token if still valid (refresh 5 min before expiry) */
    if (p->_cached_auth_token && p->_auth_token_expiry > now + 300) {
        return p->_cached_auth_token;
    }

    /* Get fresh token via gcloud */
    FILE *fp = popen("gcloud auth print-access-token 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "[provider/vertex] failed to run gcloud auth\n");
        return NULL;
    }

    char token[4096];
    if (!fgets(token, sizeof(token), fp)) {
        pclose(fp);
        fprintf(stderr, "[provider/vertex] gcloud auth returned empty token\n");
        return NULL;
    }
    pclose(fp);

    /* Strip trailing newline */
    size_t len = strlen(token);
    while (len > 0 && (token[len-1] == '\n' || token[len-1] == '\r'))
        token[--len] = '\0';

    if (len == 0) {
        fprintf(stderr, "[provider/vertex] gcloud auth returned empty token\n");
        return NULL;
    }

    free(p->_cached_auth_token);
    p->_cached_auth_token = strdup(token);
    p->_auth_token_expiry = now + 3600;  /* gcloud tokens last ~1 hour */

    return p->_cached_auth_token;
}


/* ── Message format conversion: internal → Anthropic ────────────── */

/* Convert chat messages to Anthropic format.
 * Returns a cJSON object with "system" and "messages" keys.
 * Mirrors nashell's AnthropicProvider._to_anthropic(). */
static cJSON *convert_to_anthropic(provider_t *p, llm_chat_t *chat) {
    cJSON *result = cJSON_CreateObject();
    cJSON *api_messages = cJSON_CreateArray();
    char *system_text = NULL;
    int call_idx = 0;
    char *pending_tool_id = NULL;

    for (int i = 0; i < chat->n_msgs; i++) {
        const char *role = chat->msgs[i].role;
        const char *content = chat->msgs[i].content;

        if (strcmp(role, "system") == 0) {
            free(system_text);
            system_text = strdup(content ? content : "");
            continue;
        }

        if (strcmp(role, "assistant") == 0) {
            /* If there's an unmatched tool_use, inject synthetic result */
            if (pending_tool_id) {
                /* Find or create last user message to append tool_result */
                cJSON *last = cJSON_GetArrayItem(api_messages,
                              cJSON_GetArraySize(api_messages) - 1);
                if (last && strcmp(cJSON_GetObjectItem(last, "role")->valuestring,
                                  "user") == 0) {
                    cJSON *lc = cJSON_GetObjectItem(last, "content");
                    cJSON *tr = cJSON_CreateObject();
                    cJSON_AddStringToObject(tr, "type", "tool_result");
                    cJSON_AddStringToObject(tr, "tool_use_id", pending_tool_id);
                    cJSON_AddStringToObject(tr, "content", "(interrupted)");
                    cJSON_AddItemToArray(lc, tr);
                } else {
                    cJSON *user_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(user_msg, "role", "user");
                    cJSON *uc = cJSON_CreateArray();
                    cJSON *tr = cJSON_CreateObject();
                    cJSON_AddStringToObject(tr, "type", "tool_result");
                    cJSON_AddStringToObject(tr, "tool_use_id", pending_tool_id);
                    cJSON_AddStringToObject(tr, "content", "(interrupted)");
                    cJSON_AddItemToArray(uc, tr);
                    cJSON_AddItemToObject(user_msg, "content", uc);
                    cJSON_AddItemToArray(api_messages, user_msg);
                }
                free(pending_tool_id);
                pending_tool_id = NULL;
            }

            /* Convert assistant message to content blocks */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON *blocks = cJSON_CreateArray();

            /* Check if this is a tool call (has tool_calls_json) */
            if (chat->msgs[i].tool_calls_json) {
                /* Add thought as text block */
                if (content && content[0]) {
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddStringToObject(text_block, "type", "text");
                    cJSON_AddStringToObject(text_block, "text", content);
                    cJSON_AddItemToArray(blocks, text_block);
                }

                /* Convert tool_calls to tool_use blocks */
                cJSON *tc_arr = cJSON_Parse(chat->msgs[i].tool_calls_json);
                if (tc_arr && cJSON_IsArray(tc_arr)) {
                    int n = cJSON_GetArraySize(tc_arr);
                    for (int j = 0; j < n; j++) {
                        cJSON *tc = cJSON_GetArrayItem(tc_arr, j);
                        cJSON *fn = cJSON_GetObjectItem(tc, "function");
                        if (!fn) continue;

                        /* Use the actual tool_call ID from the API response,
                         * NOT a synthetic one. Anthropic requires tool_use.id
                         * to match the tool_result.tool_use_id exactly. */
                        cJSON *tc_id_obj = cJSON_GetObjectItem(tc, "id");
                        const char *real_id = (tc_id_obj && cJSON_IsString(tc_id_obj))
                            ? tc_id_obj->valuestring : NULL;
                        char synth_id[32];
                        if (!real_id) {
                            snprintf(synth_id, sizeof(synth_id), "call_%d", call_idx);
                            real_id = synth_id;
                        }
                        call_idx++;

                        cJSON *tu = cJSON_CreateObject();
                        cJSON_AddStringToObject(tu, "type", "tool_use");
                        cJSON_AddStringToObject(tu, "id", real_id);

                        cJSON *name = cJSON_GetObjectItem(fn, "name");
                        cJSON_AddStringToObject(tu, "name",
                                                name ? name->valuestring : "");

                        cJSON *args_str = cJSON_GetObjectItem(fn, "arguments");
                        if (args_str && cJSON_IsString(args_str)) {
                            cJSON *input = cJSON_Parse(args_str->valuestring);
                            if (input) cJSON_AddItemToObject(tu, "input", input);
                            else cJSON_AddItemToObject(tu, "input",
                                                       cJSON_CreateObject());
                        } else {
                            cJSON_AddItemToObject(tu, "input", cJSON_CreateObject());
                        }

                        cJSON_AddItemToArray(blocks, tu);
                        free(pending_tool_id);
                        pending_tool_id = strdup(real_id);
                    }
                }
                cJSON_Delete(tc_arr);
            } else {
                /* Plain text assistant message */
                if (content && content[0]) {
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddStringToObject(text_block, "type", "text");
                    cJSON_AddStringToObject(text_block, "text", content);
                    cJSON_AddItemToArray(blocks, text_block);
                } else {
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddStringToObject(text_block, "type", "text");
                    cJSON_AddStringToObject(text_block, "text", "(empty)");
                    cJSON_AddItemToArray(blocks, text_block);
                }
            }

            cJSON_AddItemToObject(asst_msg, "content", blocks);
            cJSON_AddItemToArray(api_messages, asst_msg);
            continue;
        }

        if (strcmp(role, "tool") == 0) {
            /* Tool result → user message with tool_result block */
            const char *tc_id = chat->msgs[i].tool_call_id;
            if (!tc_id) tc_id = pending_tool_id ? pending_tool_id : "call_0";

            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "type", "tool_result");
            cJSON_AddStringToObject(tr, "tool_use_id", tc_id);
            cJSON_AddStringToObject(tr, "content",
                                    (content && content[0]) ? content : "(empty)");

            /* Check if error */
            if (content && strncmp(content, "ERROR", 5) == 0) {
                cJSON_AddBoolToObject(tr, "is_error", 1);
            }

            /* Merge into last user message or create new one */
            cJSON *last = NULL;
            int n = cJSON_GetArraySize(api_messages);
            if (n > 0) last = cJSON_GetArrayItem(api_messages, n - 1);

            if (last && cJSON_GetObjectItem(last, "role") &&
                strcmp(cJSON_GetObjectItem(last, "role")->valuestring, "user") == 0) {
                cJSON *lc = cJSON_GetObjectItem(last, "content");
                if (lc && cJSON_IsArray(lc)) {
                    cJSON_AddItemToArray(lc, tr);
                } else {
                    /* Convert string content to array */
                    cJSON *new_content = cJSON_CreateArray();
                    if (lc && cJSON_IsString(lc)) {
                        cJSON *tb = cJSON_CreateObject();
                        cJSON_AddStringToObject(tb, "type", "text");
                        cJSON_AddStringToObject(tb, "text", lc->valuestring);
                        cJSON_AddItemToArray(new_content, tb);
                    }
                    cJSON_AddItemToArray(new_content, tr);
                    cJSON_ReplaceItemInObject(last, "content", new_content);
                }
            } else {
                cJSON *user_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(user_msg, "role", "user");
                cJSON *uc = cJSON_CreateArray();
                cJSON_AddItemToArray(uc, tr);
                cJSON_AddItemToObject(user_msg, "content", uc);
                cJSON_AddItemToArray(api_messages, user_msg);
            }

            free(pending_tool_id);
            pending_tool_id = NULL;
            continue;
        }

        if (strcmp(role, "user") == 0) {
            /* If there's a pending tool_id and content has tool result */
            if (pending_tool_id && content && strstr(content, "Tool result:")) {
                const char *tr_start = strstr(content, "Tool result:");
                const char *result_text = tr_start + strlen("Tool result:");
                while (*result_text == ' ') result_text++;

                cJSON *user_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(user_msg, "role", "user");
                cJSON *uc = cJSON_CreateArray();

                /* Prefix text before "Tool result:" */
                if (tr_start > content) {
                    size_t prefix_len = (size_t)(tr_start - content);
                    char *prefix = malloc(prefix_len + 1);
                    memcpy(prefix, content, prefix_len);
                    prefix[prefix_len] = '\0';
                    /* Trim trailing whitespace */
                    while (prefix_len > 0 &&
                           (prefix[prefix_len-1] == ' ' ||
                            prefix[prefix_len-1] == '\n'))
                        prefix[--prefix_len] = '\0';
                    if (prefix_len > 0) {
                        cJSON *tb = cJSON_CreateObject();
                        cJSON_AddStringToObject(tb, "type", "text");
                        cJSON_AddStringToObject(tb, "text", prefix);
                        cJSON_AddItemToArray(uc, tb);
                    }
                    free(prefix);
                }

                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", pending_tool_id);
                cJSON_AddStringToObject(tr, "content",
                                        result_text[0] ? result_text : "(empty)");
                if (strncmp(result_text, "ERROR", 5) == 0)
                    cJSON_AddBoolToObject(tr, "is_error", 1);
                cJSON_AddItemToArray(uc, tr);

                cJSON_AddItemToObject(user_msg, "content", uc);
                cJSON_AddItemToArray(api_messages, user_msg);

                free(pending_tool_id);
                pending_tool_id = NULL;
            } else if (pending_tool_id) {
                /* User message after tool_use without "Tool result:" — wrap as tool_result */
                cJSON *user_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(user_msg, "role", "user");
                cJSON *uc = cJSON_CreateArray();
                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", pending_tool_id);
                cJSON_AddStringToObject(tr, "content",
                                        (content && content[0]) ? content : "(empty)");
                cJSON_AddItemToArray(uc, tr);
                cJSON_AddItemToObject(user_msg, "content", uc);
                cJSON_AddItemToArray(api_messages, user_msg);

                free(pending_tool_id);
                pending_tool_id = NULL;
            } else {
                /* Regular user message */
                cJSON *user_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(user_msg, "role", "user");
                cJSON *uc = cJSON_CreateArray();
                cJSON *tb = cJSON_CreateObject();
                cJSON_AddStringToObject(tb, "type", "text");
                cJSON_AddStringToObject(tb, "text",
                                        (content && content[0]) ? content : "(empty)");
                cJSON_AddItemToArray(uc, tb);
                cJSON_AddItemToObject(user_msg, "content", uc);
                cJSON_AddItemToArray(api_messages, user_msg);
            }
            continue;
        }
    }

    /* Handle trailing pending tool_id */
    if (pending_tool_id) {
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON *uc = cJSON_CreateArray();
        cJSON *tr = cJSON_CreateObject();
        cJSON_AddStringToObject(tr, "type", "tool_result");
        cJSON_AddStringToObject(tr, "tool_use_id", pending_tool_id);
        cJSON_AddStringToObject(tr, "content", "(interrupted)");
        cJSON_AddItemToArray(uc, tr);
        cJSON_AddItemToObject(user_msg, "content", uc);
        cJSON_AddItemToArray(api_messages, user_msg);
        free(pending_tool_id);
    }

    /* Add system with caching support */
    if (system_text) {
        if (p->cfg.caching) {
            cJSON *sys_arr = cJSON_CreateArray();
            cJSON *sys_block = cJSON_CreateObject();
            cJSON_AddStringToObject(sys_block, "type", "text");
            cJSON_AddStringToObject(sys_block, "text", system_text);
            cJSON *cc = cJSON_CreateObject();
            cJSON_AddStringToObject(cc, "type", "ephemeral");
            cJSON_AddItemToObject(sys_block, "cache_control", cc);
            cJSON_AddItemToArray(sys_arr, sys_block);
            cJSON_AddItemToObject(result, "system", sys_arr);
        } else {
            cJSON_AddStringToObject(result, "system", system_text);
        }
        free(system_text);
    }

    /* Cache breakpoint on second-to-last user message */
    if (p->cfg.caching) {
        int n = cJSON_GetArraySize(api_messages);
        for (int i = n - 2; i >= 0; i--) {
            cJSON *msg = cJSON_GetArrayItem(api_messages, i);
            cJSON *r = cJSON_GetObjectItem(msg, "role");
            if (r && strcmp(r->valuestring, "user") == 0) {
                cJSON *c = cJSON_GetObjectItem(msg, "content");
                if (c && cJSON_IsArray(c) && cJSON_GetArraySize(c) > 0) {
                    cJSON *last_block = cJSON_GetArrayItem(c,
                                        cJSON_GetArraySize(c) - 1);
                    cJSON *cc = cJSON_CreateObject();
                    cJSON_AddStringToObject(cc, "type", "ephemeral");
                    cJSON_AddItemToObject(last_block, "cache_control", cc);
                }
                break;
            }
        }
    }

    cJSON_AddItemToObject(result, "messages", api_messages);
    return result;
}

/* ── Build request ──────────────────────────────────────────────── */

static char *anthropic_build_request(provider_t *p, llm_chat_t *chat, int stream) {
    cJSON *converted = convert_to_anthropic(p, chat);
    if (!converted) return NULL;

    cJSON *req = cJSON_CreateObject();

    /* Vertex AI streamRawPredict requires anthropic_version in the body
     * (not as a header like the direct Anthropic API).
     * Also: model is specified in the URL path, NOT in the body —
     * Vertex rejects "model" as an extra input. */
    if (p->type == PROVIDER_VERTEX) {
        cJSON_AddStringToObject(req, "anthropic_version", "vertex-2023-10-16");
    } else {
        cJSON_AddStringToObject(req, "model",
                                p->cfg.model_id ? p->cfg.model_id : "claude-sonnet-4-20250514");
    }
    cJSON_AddNumberToObject(req, "max_tokens", p->cfg.max_tokens);
    cJSON_AddNumberToObject(req, "temperature", p->cfg.temperature);

    if (stream) {
        cJSON_AddBoolToObject(req, "stream", 1);
    }

    /* Move system and messages from converted */
    cJSON *sys = cJSON_DetachItemFromObject(converted, "system");
    if (sys) cJSON_AddItemToObject(req, "system", sys);

    cJSON *msgs = cJSON_DetachItemFromObject(converted, "messages");
    if (msgs) cJSON_AddItemToObject(req, "messages", msgs);

    cJSON_Delete(converted);

    /* Tools */
    cJSON *tools = build_tools_from_registry(PROVIDER_ANTHROPIC);
    cJSON_AddItemToObject(req, "tools", tools);

    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── Build headers ──────────────────────────────────────────────── */

static struct curl_slist *anthropic_build_headers(provider_t *p) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (p->type == PROVIDER_VERTEX) {
        /* Vertex AI: OAuth2 Bearer token */
        const char *token = get_vertex_token(p);
        if (token) {
            char auth[8192];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
            headers = curl_slist_append(headers, auth);
        } else {
            fprintf(stderr, "[provider/vertex] WARNING: no OAuth2 token available\n");
        }
    } else {
        /* Direct Anthropic: x-api-key header */
        const char *api_key = get_anthropic_api_key(&p->cfg);
        if (api_key && api_key[0]) {
            char auth[2048];
            snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
            headers = curl_slist_append(headers, auth);
        } else {
            fprintf(stderr, "[provider/anthropic] WARNING: no API key in $%s\n",
                    p->cfg.api_key_env ? p->cfg.api_key_env : "ANTHROPIC_API_KEY");
        }
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    }

    /* Enable prompt caching beta (direct Anthropic API only — Vertex AI
     * does not support the anthropic-beta header and rejects it with 400) */
    if (p->cfg.caching && p->type == PROVIDER_ANTHROPIC) {
        headers = curl_slist_append(headers,
                                    "anthropic-beta: prompt-caching-2024-07-31");
    }

    return headers;
}

/* ── Get endpoint ───────────────────────────────────────────────── */

static const char *anthropic_get_endpoint(provider_t *p) {
    if (p->type == PROVIDER_VERTEX) {
        const char *region = p->cfg.region ? p->cfg.region : "us-east5";
        int is_global = (strcmp(region, "global") == 0);
        if (is_global) {
            return provider_cache_endpoint(p,
                "https://aiplatform.googleapis.com/v1/projects/%s/"
                "locations/global/publishers/anthropic/models/%s:streamRawPredict",
                p->cfg.project_id ? p->cfg.project_id : "",
                p->cfg.model_id ? p->cfg.model_id : "claude-sonnet-4-20250514");
        } else {
            return provider_cache_endpoint(p,
                "https://%s-aiplatform.googleapis.com/v1/projects/%s/"
                "locations/%s/publishers/anthropic/models/%s:streamRawPredict",
                region,
                p->cfg.project_id ? p->cfg.project_id : "",
                region,
                p->cfg.model_id ? p->cfg.model_id : "claude-sonnet-4-20250514");
        }
    } else {
        const char *base = p->cfg.api_base;
        if (!base || !base[0]) base = "https://api.anthropic.com/v1";
        return provider_cache_endpoint(p, "%s/messages", base);
    }
}

/* ── Parse response ─────────────────────────────────────────────── */

static char *anthropic_parse_response(provider_t *p, const char *response_json,
                                      llm_chat_t *chat, llm_stats_t *stats) {
    (void)p;
    cJSON *resp = cJSON_Parse(response_json);
    if (!resp) return NULL;

    /* Extract usage stats */
    if (stats) {
        cJSON *usage = cJSON_GetObjectItem(resp, "usage");
        if (usage) {
            cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
            cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
            if (it) stats->prompt_tokens = it->valueint;
            if (ot) stats->completion_tokens = ot->valueint;
        }
    }

    /* Parse content blocks */
    cJSON *content = cJSON_GetObjectItem(resp, "content");
    if (!content || !cJSON_IsArray(content)) {
        cJSON_Delete(resp);
        return NULL;
    }

    str_t thought = str_new(256);
    cJSON *tool_block = NULL;

    int n = cJSON_GetArraySize(content);
    for (int i = 0; i < n; i++) {
        cJSON *block = cJSON_GetArrayItem(content, i);
        cJSON *btype = cJSON_GetObjectItem(block, "type");
        if (!btype || !cJSON_IsString(btype)) continue;

        if (strcmp(btype->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text)) {
                if (thought.len > 0) str_append_cstr(&thought, " ");
                str_append_cstr(&thought, text->valuestring);
            }
        } else if (strcmp(btype->valuestring, "tool_use") == 0) {
            tool_block = block;
        }
    }

    /* Check stop reason */
    cJSON *stop_reason = cJSON_GetObjectItem(resp, "stop_reason");
    const char *stop = stop_reason && cJSON_IsString(stop_reason) ?
                       stop_reason->valuestring : "";

    if (strcmp(stop, "end_turn") == 0 && !tool_block) {
        /* No tool call — model wants to finish */
        char *result = thought.len > 0 ? strdup(thought.data) : strdup("");
        str_free(&thought);
        if (chat) {
            free(chat->last_tool_call_id);
            chat->last_tool_call_id = NULL;
            free(chat->last_tool_calls_json);
            chat->last_tool_calls_json = NULL;
        }
        cJSON_Delete(resp);
        return result;
    }

    if (tool_block) {
        cJSON *name = cJSON_GetObjectItem(tool_block, "name");
        cJSON *id = cJSON_GetObjectItem(tool_block, "id");
        cJSON *input = cJSON_GetObjectItem(tool_block, "input");

        /* Build unified response */
        cJSON *unified = cJSON_CreateObject();
        cJSON_AddStringToObject(unified, "thought",
                                thought.len > 0 ? thought.data : "");
        cJSON_AddStringToObject(unified, "action",
                                name ? name->valuestring : "");

        /* Merge input params into unified */
        if (input && cJSON_IsObject(input)) {
            cJSON *child = input->child;
            while (child) {
                cJSON_AddItemToObject(unified, child->string,
                                      cJSON_Duplicate(child, 1));
                child = child->next;
            }
        }

        char *result = cJSON_PrintUnformatted(unified);
        cJSON_Delete(unified);

        /* Store tool call info */
        if (chat) {
            free(chat->last_tool_call_id);
            chat->last_tool_call_id = (id && cJSON_IsString(id)) ?
                                      strdup(id->valuestring) : NULL;

            /* Build tool_calls JSON in OpenAI format for history */
            cJSON *tc_arr = cJSON_CreateArray();
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id",
                                    id && cJSON_IsString(id) ?
                                    id->valuestring : "call_0");
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name",
                                    name ? name->valuestring : "");
            char *args_str = input ? cJSON_PrintUnformatted(input) : strdup("{}");
            cJSON_AddStringToObject(fn, "arguments", args_str);
            free(args_str);
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tc_arr, tc);

            free(chat->last_tool_calls_json);
            chat->last_tool_calls_json = cJSON_PrintUnformatted(tc_arr);
            cJSON_Delete(tc_arr);
        }

        str_free(&thought);
        cJSON_Delete(resp);
        return result;
    }

    /* Fallback: return thought text */
    char *result = thought.len > 0 ? strdup(thought.data) : NULL;
    str_free(&thought);
    cJSON_Delete(resp);
    return result;
}

/* ── Build tools ────────────────────────────────────────────────── */

static cJSON *anthropic_build_tools_vtable(provider_t *p) {
    (void)p;
    return build_tools_from_registry(PROVIDER_ANTHROPIC);
}

/* ── Model info ─────────────────────────────────────────────────── */

static int anthropic_fetch_model_info(provider_t *p, int *context_size,
                                      char **model_name, char **props_json) {
    return provider_api_fetch_model_info(p, context_size, model_name, props_json);
}

/* ── Init ───────────────────────────────────────────────────────── */

void provider_anthropic_init(provider_t *p) {
    p->build_headers    = anthropic_build_headers;
    p->build_request    = anthropic_build_request;
    p->parse_response   = anthropic_parse_response;
    p->parse_sse_event  = NULL;  /* uses shared Anthropic SSE parser in provider.c */
    p->get_endpoint     = anthropic_get_endpoint;
    p->build_tools      = anthropic_build_tools_vtable;
    p->fetch_model_info = anthropic_fetch_model_info;
    p->destroy          = NULL;
}

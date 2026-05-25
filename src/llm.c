#include "llm.h"
#include "str.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>    /* sqrtf, expf — for EDRM entropy probe */
#include <unistd.h>  /* sleep */

#define LLM_MAX_RETRIES    10
#define LLM_RETRY_BASE_SEC 10  /* linear backoff: 10s, 20s, 30s, ... */

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
        free(chat->msgs[i].tool_call_id);
        free(chat->msgs[i].tool_calls_json);
    }
    free(chat->msgs);
    free(chat);
}

void llm_chat_add(llm_chat_t *chat, const char *role, const char *content) {
    if (chat->n_msgs >= chat->cap_msgs) {
        chat->cap_msgs *= 2;
        chat->msgs = realloc(chat->msgs, chat->cap_msgs * sizeof(llm_msg_t));
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
    chat->n_msgs++;
}

/* Add a tool result message (role: "tool" with tool_call_id) */
void llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                               const char *content) {
    if (chat->n_msgs >= chat->cap_msgs) {
        chat->cap_msgs *= 2;
        chat->msgs = realloc(chat->msgs, chat->cap_msgs * sizeof(llm_msg_t));
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup("tool");
    m->content = strdup(content);
    m->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
    chat->n_msgs++;
}

/* Add an assistant message with tool_calls (for conversation history) */
void llm_chat_add_assistant_tool_call(llm_chat_t *chat, const char *content,
                                       const char *tool_calls_json) {
    if (chat->n_msgs >= chat->cap_msgs) {
        chat->cap_msgs *= 2;
        chat->msgs = realloc(chat->msgs, chat->cap_msgs * sizeof(llm_msg_t));
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup("assistant");
    m->content = content ? strdup(content) : strdup("");
    m->tool_calls_json = tool_calls_json ? strdup(tool_calls_json) : NULL;
    chat->n_msgs++;
}

/* ── CURL callback ─────────────────────────────────────────── */

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    str_t *buf = userdata;
    size_t total = size * nmemb;
    str_append(buf, ptr, total);
    return total;
}

/* ── Build tools array (shared between streaming and non-streaming) ── */

static cJSON *build_tools_array(void) {
    cJSON *tools = cJSON_CreateArray();

    /* Helper macro to add a tool */
    #define ADD_TOOL(name, desc, params_json) do { \
        cJSON *t = cJSON_CreateObject(); \
        cJSON_AddStringToObject(t, "type", "function"); \
        cJSON *fn = cJSON_CreateObject(); \
        cJSON_AddStringToObject(fn, "name", name); \
        cJSON_AddStringToObject(fn, "description", desc); \
        cJSON *p = cJSON_Parse(params_json); \
        if (p) cJSON_AddItemToObject(fn, "parameters", p); \
        cJSON_AddItemToObject(t, "function", fn); \
        cJSON_AddItemToArray(tools, t); \
    } while(0)

    ADD_TOOL("shell_exec",
        "Execute a shell command. Output is stored; you see metadata.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"}},\"required\":[\"command\"]}");

    ADD_TOOL("file_read",
        "Read a file. Use step aliases (R0S1, R1S2...) to read stored tool outputs.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path or step alias\"}},\"required\":[\"path\"]}");

    ADD_TOOL("file_write",
        "Write content to a file.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}");

    ADD_TOOL("file_edit",
        "Replace exact text in a file. Always file_read first to get exact text.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}");

    ADD_TOOL("grep_search",
        "Search files with regex. Results stored.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search (default: .)\"}},\"required\":[\"pattern\"]}");

    ADD_TOOL("web_fetch",
        "Fetch a URL. Content stored; you see metadata.",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"}},\"required\":[\"url\"]}");

    ADD_TOOL("web_search",
        "Search the web. Results stored.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}");

    ADD_TOOL("memory_store",
        "Save knowledge for future sessions. Key format: lesson:name, strategy:name, fact:name, skill:name, task:name.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key (e.g. lesson:redis-v7)\"},\"value\":{\"type\":\"string\",\"description\":\"The knowledge to store\"},\"tags\":{\"type\":\"string\",\"description\":\"Comma-separated tags\"}},\"required\":[\"key\",\"value\"]}");

    ADD_TOOL("memory_recall",
        "Search saved knowledge by keyword.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}");

    ADD_TOOL("memory_pin",
        "Pin an existing memory so it is always injected into the system prompt.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to pin\"}},\"required\":[\"key\"]}");

    ADD_TOOL("memory_unpin",
        "Unpin a memory so it is no longer always injected into the system prompt.",
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to unpin\"}},\"required\":[\"key\"]}");

    ADD_TOOL("notes",
        "Save persistent scratchpad. Survives context resets.",
        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"Scratchpad content\"}},\"required\":[\"content\"]}");

    ADD_TOOL("done",
        "Signal task completion with final answer.",
        "{\"type\":\"object\",\"properties\":{\"result\":{\"type\":\"string\",\"description\":\"Final answer\"}},\"required\":[\"result\"]}");

    #undef ADD_TOOL
    return tools;
}

/* ── Build request JSON ──────────────────────────────────────── */

static char *build_request(const llm_config_t *cfg, llm_chat_t *chat, int stream) {
    cJSON *req = cJSON_CreateObject();
    if (cfg->model) cJSON_AddStringToObject(req, "model", cfg->model);
    cJSON_AddNumberToObject(req, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(req, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(req, "stream", stream);

    /* Native tool calling — each tool has its own parameter schema */
    cJSON *tools = build_tools_array();
    cJSON_AddItemToObject(req, "tools", tools);

    /* Set thinking mode — controlled by EDRM routing or config */
    cJSON *tmpl_kwargs = cJSON_CreateObject();
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", cfg->enable_thinking);
    cJSON_AddItemToObject(req, "chat_template_kwargs", tmpl_kwargs);

    /* Build messages array — handle tool_calls and tool results */
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < chat->n_msgs; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", chat->msgs[i].role);

        /* For tool results, include tool_call_id */
        if (strcmp(chat->msgs[i].role, "tool") == 0 && chat->msgs[i].tool_call_id) {
            cJSON_AddStringToObject(m, "tool_call_id", chat->msgs[i].tool_call_id);
        }

        /* For assistant messages with tool_calls, include them */
        if (strcmp(chat->msgs[i].role, "assistant") == 0 && chat->msgs[i].tool_calls_json) {
            cJSON *tc = cJSON_Parse(chat->msgs[i].tool_calls_json);
            if (tc) cJSON_AddItemToObject(m, "tool_calls", tc);
            /* Content may be empty for tool-call-only messages */
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

/* ── Extract tool call from response message ──────────────────── */

/* Converts tool_calls[0] from the API response into a JSON string that
 * looks like the old format: {"thought":"...", "action":"tool_name", "param":"value"}
 * Also extracts tool_call_id and raw tool_calls JSON for history threading. */
static char *extract_tool_call(cJSON *message, char **out_tool_call_id,
                                char **out_tool_calls_json) {
    if (out_tool_call_id) *out_tool_call_id = NULL;
    if (out_tool_calls_json) *out_tool_calls_json = NULL;

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (!tool_calls || !cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0)
        return NULL;

    cJSON *tc0 = cJSON_GetArrayItem(tool_calls, 0);
    if (!tc0) return NULL;

    /* Extract tool_call_id */
    cJSON *id = cJSON_GetObjectItem(tc0, "id");
    if (id && id->valuestring && out_tool_call_id)
        *out_tool_call_id = strdup(id->valuestring);

    /* Save raw tool_calls JSON for history threading */
    if (out_tool_calls_json) {
        char *raw = cJSON_PrintUnformatted(tool_calls);
        if (raw) *out_tool_calls_json = raw;
    }

    /* Extract function name and arguments */
    cJSON *function = cJSON_GetObjectItem(tc0, "function");
    if (!function) return NULL;

    cJSON *name = cJSON_GetObjectItem(function, "name");
    cJSON *args = cJSON_GetObjectItem(function, "arguments");
    if (!name || !name->valuestring) return NULL;

    /* Build a unified JSON object: {"thought":"...", "action":"name", ...params} */
    cJSON *result = cJSON_CreateObject();

    /* Get thought from content (if present) */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && content->valuestring && content->valuestring[0])
        cJSON_AddStringToObject(result, "thought", content->valuestring);
    else
        cJSON_AddStringToObject(result, "thought", "");

    cJSON_AddStringToObject(result, "action", name->valuestring);

    /* Parse arguments and merge into result */
    if (args && args->valuestring) {
        cJSON *parsed_args = cJSON_Parse(args->valuestring);
        if (parsed_args) {
            cJSON *child = parsed_args->child;
            while (child) {
                cJSON *next = child->next;
                /* Detach and add to result */
                cJSON_DetachItemViaPointer(parsed_args, child);
                cJSON_AddItemToObject(result, child->string, child);
                child = next;
            }
            cJSON_Delete(parsed_args);
        }
    }

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* ── Main completion call ──────────────────────────────────── */

char *llm_complete(const llm_config_t *cfg, llm_chat_t *chat, llm_stats_t *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", cfg->api_base);

    char *req_body = build_request(cfg, chat, 0);
    if (!req_body) return NULL;

    cJSON *resp = NULL;
    str_t response = str_new(4096);

    for (int attempt = 1; attempt <= LLM_MAX_RETRIES; attempt++) {
        str_clear(&response);

        CURL *curl = curl_easy_init();
        if (!curl) { free(req_body); str_free(&response); return NULL; }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            int delay = attempt * LLM_RETRY_BASE_SEC;
            fprintf(stderr, "[llm] curl error: %s (attempt %d/%d, retry in %ds)\n",
                    curl_easy_strerror(res), attempt, LLM_MAX_RETRIES, delay);
            if (attempt < LLM_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body); str_free(&response);
            return NULL;
        }

        resp = cJSON_Parse(response.data);
        str_free(&response);
        if (!resp) {
            int delay = attempt * LLM_RETRY_BASE_SEC;
            fprintf(stderr, "[llm] failed to parse response JSON (attempt %d/%d, retry in %ds)\n",
                    attempt, LLM_MAX_RETRIES, delay);
            if (attempt < LLM_MAX_RETRIES) { sleep(delay); continue; }
            free(req_body);
            return NULL;
        }

        cJSON *choices = cJSON_GetObjectItem(resp, "choices");
        if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            cJSON *err = cJSON_GetObjectItem(resp, "error");
            if (err) {
                cJSON *msg = cJSON_GetObjectItem(err, "message");
                int delay = attempt * LLM_RETRY_BASE_SEC;
                fprintf(stderr, "[llm] API error: %s (attempt %d/%d, retry in %ds)\n",
                        msg ? msg->valuestring : "unknown", attempt, LLM_MAX_RETRIES, delay);
                cJSON_Delete(resp); resp = NULL;
                if (attempt < LLM_MAX_RETRIES) { sleep(delay); continue; }
                free(req_body);
                return NULL;
            }
            cJSON_Delete(resp);
            free(req_body);
            return NULL;
        }

        break;
    }
    free(req_body);

    if (!resp) return NULL;

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice0, "message");

    char *result = NULL;

    /* Try tool_calls first (native tool calling) */
    char *tc_id = NULL, *tc_json = NULL;
    char *tool_call_result = extract_tool_call(message, &tc_id, &tc_json);
    if (tool_call_result) {
        result = tool_call_result;
        /* Store tool_call_id and tool_calls_json for react.c message threading */
        if (chat->last_tool_call_id) free(chat->last_tool_call_id);
        if (chat->last_tool_calls_json) free(chat->last_tool_calls_json);
        chat->last_tool_call_id = tc_id;      /* ownership transferred */
        chat->last_tool_calls_json = tc_json;  /* ownership transferred */
    }

    /* Fallback: try content (JSON-in-content mode) */
    if (!result) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && content->valuestring && content->valuestring[0]) {
            result = strdup(content->valuestring);
        }
    }

    /* If still nothing, check reasoning_content */
    if (!result) {
        cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
        if (reasoning && reasoning->valuestring && reasoning->valuestring[0]) {
            fprintf(stderr, "[debug] LLM returned reasoning_content (thinking mode active?)\n");
        }
        char *raw = cJSON_PrintUnformatted(resp);
        if (raw) {
            fprintf(stderr, "[debug] raw API response: %.500s\n", raw);
            free(raw);
        }
    }

    /* Parse stats */
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

/* Attempt to repair common JSON errors produced by models:
 * - "key="value"  → "key":"value"  (missing colon)
 * - "key=value"   → "key":"value"  (missing colon and quotes)
 * - XML fragments after JSON start → truncated at first < */
static char *repair_json(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    /* Allocate extra space for inserted colons */
    char *buf = malloc(len * 2 + 1);
    if (!buf) return NULL;

    size_t j = 0;
    int in_string = 0;
    int escape = 0;

    for (size_t i = 0; src[i] && j < len * 2 - 1; i++) {
        if (escape) { buf[j++] = src[i]; escape = 0; continue; }
        if (src[i] == '\\') { buf[j++] = src[i]; escape = 1; continue; }
        if (src[i] == '"') in_string = !in_string;

        /* Detect XML fragment: truncate at < outside strings */
        if (src[i] == '<' && !in_string) {
            /* Close any open JSON structure */
            buf[j++] = '}';
            break;
        }

        /* Fix: "key="value" → "key":"value" (missing colon after key) */
        if (src[i] == '=' && !in_string) {
            /* Check if this looks like "key"=... or "key=... */
            /* Replace = with : */
            buf[j++] = ':';
            /* If next char is not a quote, add one for the value */
            if (src[i+1] && src[i+1] != '"' && src[i+1] != '{' &&
                src[i+1] != '[' && src[i+1] != ' ') {
                buf[j++] = '"';
            }
            continue;
        }

        buf[j++] = src[i];
    }
    buf[j] = '\0';
    return buf;
}

cJSON *llm_parse_action(const char *response) {
    if (!response) return NULL;

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

    const char *brace = strchr(start, '{');
    if (!brace) return NULL;

    /* Try strict parse first */
    cJSON *action = cJSON_Parse(brace);
    if (action) return action;

    /* Strict parse failed — try repairing common JSON errors */
    char *repaired = repair_json(brace);
    if (repaired) {
        action = cJSON_Parse(repaired);
        if (action) {
            fprintf(stderr, "[llm] repaired malformed JSON response\n");
        }
        free(repaired);
    }
    return action;
}

/* ── Fetch context size from /props ──────────────────────────── */

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

/* ── Streaming SSE state ─────────────────────────────────────── */

typedef struct {
    str_t          line_buf;
    str_t          full_content;
    llm_token_fn   on_token;
    void          *userdata;
    llm_stats_t   *stats;
    /* Safety limits */
    size_t         max_response;
    int            repeat_threshold;
    char           last_tokens[16][64];
    int            last_token_idx;
    int            repeat_count;
    int            stopped;
    /* Tool call accumulation for streaming */
    str_t          tool_call_name;
    str_t          tool_call_args;
    char          *tool_call_id;
    int            has_tool_call;
} sse_state_t;

static void sse_process_line(sse_state_t *st, const char *line) {
    if (line[0] == '\0' || line[0] == '\n') return;
    if (strncmp(line, "data: ", 6) != 0) return;

    const char *json_str = line + 6;
    if (strncmp(json_str, "[DONE]", 6) == 0) return;

    cJSON *chunk = cJSON_Parse(json_str);
    if (!chunk) return;

    cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
    if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice0, "delta");
        if (delta) {
            /* Check for tool_calls in delta (streaming tool calling) */
            cJSON *tc = cJSON_GetObjectItem(delta, "tool_calls");
            if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
                cJSON *tc0 = cJSON_GetArrayItem(tc, 0);
                st->has_tool_call = 1;

                /* Extract tool_call_id (only in first chunk) */
                cJSON *id = cJSON_GetObjectItem(tc0, "id");
                if (id && id->valuestring && !st->tool_call_id)
                    st->tool_call_id = strdup(id->valuestring);

                cJSON *function = cJSON_GetObjectItem(tc0, "function");
                if (function) {
                    cJSON *name = cJSON_GetObjectItem(function, "name");
                    if (name && name->valuestring)
                        str_append_cstr(&st->tool_call_name, name->valuestring);

                    cJSON *args = cJSON_GetObjectItem(function, "arguments");
                    if (args && args->valuestring) {
                        str_append_cstr(&st->tool_call_args, args->valuestring);
                        /* Stream the arguments as tokens for display */
                        if (st->on_token && !st->stopped)
                            st->on_token(args->valuestring, st->userdata);
                    }
                }
            }

            /* Regular content streaming */
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (content && content->valuestring && content->valuestring[0] && !st->stopped) {
                if (st->max_response > 0 && st->full_content.len > st->max_response) {
                    fprintf(stderr, "\n[llm] response exceeded %zu bytes — stopping\n",
                            st->max_response);
                    st->stopped = 1;
                    cJSON_Delete(chunk);
                    return;
                }

                if (st->repeat_threshold > 0) {
                    const char *tok = content->valuestring;
                    int idx = st->last_token_idx % 16;
                    if (st->last_token_idx > 0) {
                        int prev = (st->last_token_idx - 1) % 16;
                        if (strcmp(st->last_tokens[prev], tok) == 0)
                            st->repeat_count++;
                        else
                            st->repeat_count = 0;
                    }
                    snprintf(st->last_tokens[idx], 64, "%.63s", tok);
                    st->last_token_idx++;

                    if (st->repeat_count >= st->repeat_threshold) {
                        fprintf(stderr, "\n[llm] degenerate output: token '%s' repeated %d times — stopping\n",
                                tok, st->repeat_count);
                        st->stopped = 1;
                        cJSON_Delete(chunk);
                        return;
                    }
                }

                str_append_cstr(&st->full_content, content->valuestring);
                if (st->on_token)
                    st->on_token(content->valuestring, st->userdata);
            }
        }

        /* Check for finish_reason — last chunk has usage/timings */
        cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
        if (finish && cJSON_IsString(finish) && finish->valuestring) {
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
                    if (st->stats->prompt_tokens == 0) {
                        cJSON *pn = cJSON_GetObjectItem(timings, "prompt_n");
                        if (pn) st->stats->prompt_tokens = (int)cJSON_GetNumberValue(pn);
                    }
                    if (st->stats->completion_tokens == 0) {
                        cJSON *cn = cJSON_GetObjectItem(timings, "predicted_n");
                        if (cn) st->stats->completion_tokens = (int)cJSON_GetNumberValue(cn);
                    }
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
                          void *userdata,
                          int max_response_bytes, int repeat_threshold) {
    if (stats) memset(stats, 0, sizeof(*stats));

    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/chat/completions", cfg->api_base);

    char *req_body = build_request(cfg, chat, 1);
    if (!req_body) return NULL;

    /* Set up SSE state */
    sse_state_t st = {
        .line_buf     = str_new(256),
        .full_content = str_new(4096),
        .on_token     = on_token,
        .userdata     = userdata,
        .stats        = stats,
        .max_response       = (size_t)max_response_bytes,
        .repeat_threshold   = repeat_threshold,
        .repeat_count       = 0,
        .stopped            = 0,
        .tool_call_name     = str_new(64),
        .tool_call_args     = str_new(1024),
        .tool_call_id       = NULL,
        .has_tool_call      = 0,
    };

    /* Retry loop with linear backoff */
    char *result = NULL;
    for (int attempt = 1; attempt <= LLM_MAX_RETRIES; attempt++) {
        str_clear(&st.line_buf);
        str_clear(&st.full_content);
        str_clear(&st.tool_call_name);
        str_clear(&st.tool_call_args);
        free(st.tool_call_id); st.tool_call_id = NULL;
        st.has_tool_call = 0;
        st.stopped = 0;
        st.repeat_count = 0;
        st.last_token_idx = 0;
        if (stats) memset(stats, 0, sizeof(*stats));

        CURL *curl = curl_easy_init();
        if (!curl) { free(req_body); str_free(&st.line_buf); str_free(&st.full_content);
                      str_free(&st.tool_call_name); str_free(&st.tool_call_args);
                      free(st.tool_call_id); return NULL; }

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

        if (st.line_buf.len > 0)
            sse_process_line(&st, str_cstr(&st.line_buf));
        str_free(&st.line_buf);

        if (res != CURLE_OK) {
            int delay = attempt * LLM_RETRY_BASE_SEC;
            fprintf(stderr, "[llm] curl error: %s (attempt %d/%d, retry in %ds)\n",
                    curl_easy_strerror(res), attempt, LLM_MAX_RETRIES, delay);
            if (attempt < LLM_MAX_RETRIES) { sleep(delay); continue; }
            str_free(&st.full_content);
            str_free(&st.tool_call_name);
            str_free(&st.tool_call_args);
            free(st.tool_call_id);
            free(req_body);
            return NULL;
        }

        /* Check if we got a tool call (streaming) */
        if (st.has_tool_call && st.tool_call_name.len > 0) {
            /* Build unified JSON: {"thought":"", "action":"name", ...parsed_args} */
            cJSON *unified = cJSON_CreateObject();
            cJSON_AddStringToObject(unified, "thought",
                st.full_content.len > 0 ? str_cstr(&st.full_content) : "");
            cJSON_AddStringToObject(unified, "action", str_cstr(&st.tool_call_name));

            /* Parse and merge arguments */
            if (st.tool_call_args.len > 0) {
                cJSON *args = cJSON_Parse(str_cstr(&st.tool_call_args));
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
        } else if (st.full_content.len > 0) {
            result = str_steal(&st.full_content);
        }

        if (result) break;

        int delay = attempt * LLM_RETRY_BASE_SEC;
        fprintf(stderr, "[llm] empty response (attempt %d/%d, retry in %ds)\n",
                attempt, LLM_MAX_RETRIES, delay);
        if (attempt < LLM_MAX_RETRIES) { sleep(delay); continue; }
    }

    str_free(&st.full_content);
    str_free(&st.tool_call_name);
    str_free(&st.tool_call_args);
    free(st.tool_call_id);
    free(req_body);
    return result;
}

/* ── Fetch model name from /v1/models ──────────────────────── */

char *llm_fetch_model_name(const char *api_base) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/models", api_base);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    str_t response = str_new(4096);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        str_free(&response);
        return NULL;
    }

    cJSON *resp = cJSON_Parse(response.data);
    str_free(&response);
    if (!resp) return NULL;

    char *model_name = NULL;

    /* Try OpenAI format: data[0].id */
    cJSON *data = cJSON_GetObjectItem(resp, "data");
    if (data && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        cJSON *m0 = cJSON_GetArrayItem(data, 0);
        cJSON *id = cJSON_GetObjectItem(m0, "id");
        if (id && id->valuestring)
            model_name = strdup(id->valuestring);
    }

    /* Fallback: try models[0].model (Ollama format) */
    if (!model_name) {
        cJSON *models = cJSON_GetObjectItem(resp, "models");
        if (models && cJSON_IsArray(models) && cJSON_GetArraySize(models) > 0) {
            cJSON *m0 = cJSON_GetArrayItem(models, 0);
            cJSON *mn = cJSON_GetObjectItem(m0, "model");
            if (mn && mn->valuestring)
                model_name = strdup(mn->valuestring);
        }
    }

    cJSON_Delete(resp);
    return model_name;
}

/* ── Fetch raw /props JSON ──────────────────────────────────── */

char *llm_fetch_props_json(const char *api_base) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/props", api_base);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    str_t response = str_new(8192);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        str_free(&response);
        return NULL;
    }

    return str_steal(&response);
}

/* ── EDRM entropy probe ──────────────────────────────────────────────
 * Implements the entropy dynamics routing from [arXiv:2605.22873]:
 *   "When Do LLMs Reason? A Dynamical Systems View via Entropy Phase
 *    Transitions"
 *
 * Sends a short /completion probe, extracts per-token entropy from
 * logprobs, computes three descriptors (H̄, ρ_s, VNR), and returns
 * a routing decision: 0=direct (thinking off), 1=cot (thinking on).
 * ─────────────────────────────────────────────────────────────────── */

/* Spearman rank correlation between two arrays of length n */
static float spearman_corr(const float *x, const float *y, int n) {
    if (n < 3) return 0.0f;
    /* Compute ranks for x and y (simple: sort indices) */
    float *rx = calloc(n, sizeof(float));
    float *ry = calloc(n, sizeof(float));
    if (!rx || !ry) { free(rx); free(ry); return 0.0f; }

    /* Rank by sorting indices */
    int *idx = calloc(n, sizeof(int));
    for (int i = 0; i < n; i++) idx[i] = i;

    /* Rank x */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (x[idx[i]] > x[idx[j]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < n; i++) rx[idx[i]] = (float)(i + 1);

    /* Rank y */
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (y[idx[i]] > y[idx[j]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < n; i++) ry[idx[i]] = (float)(i + 1);

    /* Pearson correlation on ranks */
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += rx[i]; my += ry[i]; }
    mx /= n; my /= n;

    float num = 0, dx = 0, dy = 0;
    for (int i = 0; i < n; i++) {
        float a = rx[i] - mx, b = ry[i] - my;
        num += a * b;
        dx += a * a;
        dy += b * b;
    }
    free(rx); free(ry); free(idx);
    if (dx == 0 || dy == 0) return 0.0f;
    return num / sqrtf(dx * dy);
}

edrm_result_t llm_edrm_probe(const char *api_base, const char *prompt,
                               int n_predict, int n_probs, float temperature,
                               float tau_rho, float tau_vnr, float tau_h) {
    edrm_result_t result = {0, 0, 0, 0};

    /* Build /completion request with logprobs */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt", prompt);
    cJSON_AddNumberToObject(req, "n_predict", n_predict);
    cJSON_AddNumberToObject(req, "n_probs", n_probs);
    cJSON_AddNumberToObject(req, "temperature", temperature);
    cJSON_AddBoolToObject(req, "cache_prompt", 1);
    cJSON_AddBoolToObject(req, "stream", 0);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return result;

    char url[1024];
    snprintf(url, sizeof(url), "%s/completion", api_base);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return result; }

    str_t response = str_new(16384);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);

    if (res != CURLE_OK) {
        str_free(&response);
        return result;
    }

    /* Parse response and extract entropy from completion_probabilities */
    cJSON *resp = cJSON_Parse(response.data);
    str_free(&response);
    if (!resp) return result;

    cJSON *probs_arr = cJSON_GetObjectItem(resp, "completion_probabilities");
    if (!probs_arr || !cJSON_IsArray(probs_arr)) {
        cJSON_Delete(resp);
        return result;
    }

    int n_tokens = cJSON_GetArraySize(probs_arr);
    if (n_tokens < 3) {
        cJSON_Delete(resp);
        return result;
    }

    /* Compute per-token entropy from top logprobs */
    float *entropies = calloc(n_tokens, sizeof(float));
    float *steps = calloc(n_tokens, sizeof(float));
    if (!entropies || !steps) {
        free(entropies); free(steps);
        cJSON_Delete(resp);
        return result;
    }

    for (int i = 0; i < n_tokens; i++) {
        cJSON *tok = cJSON_GetArrayItem(probs_arr, i);
        cJSON *top = cJSON_GetObjectItem(tok, "top_logprobs");
        if (!top || !cJSON_IsArray(top)) continue;

        float h = 0;
        int k = cJSON_GetArraySize(top);
        for (int j = 0; j < k; j++) {
            cJSON *entry = cJSON_GetArrayItem(top, j);
            cJSON *lp = cJSON_GetObjectItem(entry, "logprob");
            if (lp && cJSON_IsNumber(lp)) {
                float logp = (float)lp->valuedouble;
                float p = expf(logp);
                if (p > 0) h -= p * logp;  /* H = -Σ p·log(p) */
            }
        }
        entropies[i] = h;
        steps[i] = (float)i;
    }

    /* Descriptor 1: mean entropy (H̄) */
    float h_sum = 0;
    for (int i = 0; i < n_tokens; i++) h_sum += entropies[i];
    result.h_mean = h_sum / n_tokens;

    /* Descriptor 2: Spearman correlation (ρ_s) — entropy vs step index */
    result.rho_s = spearman_corr(entropies, steps, n_tokens);

    /* Descriptor 3: von Neumann ratio (VNR) — smoothness measure */
    float diff_sq_sum = 0, var_sum = 0;
    for (int i = 1; i < n_tokens; i++) {
        float d = entropies[i] - entropies[i - 1];
        diff_sq_sum += d * d;
    }
    for (int i = 0; i < n_tokens; i++) {
        float d = entropies[i] - result.h_mean;
        var_sum += d * d;
    }
    result.vnr = (var_sum > 0) ? diff_sq_sum / var_sum : 999.0f;

    /* Routing decision: convergent regime → CoT, otherwise → Direct */
    result.route = (result.rho_s < tau_rho &&
                    result.vnr < tau_vnr &&
                    result.h_mean < tau_h) ? 1 : 0;

    free(entropies);
    free(steps);
    cJSON_Delete(resp);
    return result;
}

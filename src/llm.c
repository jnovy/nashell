#include "llm.h"
#include "provider.h"
#include "str.h"
#include "tui.h"
#include "nash_log.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>    /* sqrtf, expf — for EDRM entropy probe */

/* ── Chat management ─────────────────────────────────────────── */

llm_chat_t *llm_chat_new(void) {
    llm_chat_t *c = calloc(1, sizeof(*c));
    c->cap_msgs = 32;
    c->msgs = calloc(c->cap_msgs, sizeof(llm_msg_t));
    return c;
}

/* Free all heap fields of a single message (but not the struct itself). */
void llm_msg_free_fields(llm_msg_t *m) {
    free(m->role);
    free(m->content);
    free(m->tool_call_id);
    free(m->tool_calls_json);
    free(m->tool_call_id_outbound);
    free(m->store_alias);
}

/* Ensure the msgs array has room for at least one more entry.
 * Returns 0 on success, -1 on allocation failure. */
static int llm_chat_ensure_capacity(llm_chat_t *chat) {
    if (chat->n_msgs < chat->cap_msgs) return 0;
    int new_cap = chat->cap_msgs * 2;
    llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
    if (!tmp) return -1;
    chat->msgs = tmp;
    chat->cap_msgs = new_cap;
    return 0;
}

void llm_chat_free(llm_chat_t *chat) {
    if (!chat) return;
    for (int i = 0; i < chat->n_msgs; i++)
        llm_msg_free_fields(&chat->msgs[i]);
    free(chat->msgs);
    free(chat->last_tool_call_id);
    free(chat->last_tool_calls_json);
    free(chat);
}

void llm_chat_add(llm_chat_t *chat, const char *role, const char *content) {
    if (llm_chat_ensure_capacity(chat) != 0) return;
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
    if (!m->role || !m->content) {
        nash_log("[llm] CRITICAL: strdup failed for message role=%s — context will be incomplete", role);
        free(m->role);
        free(m->content);
        return;
    }
    m->content_len = strlen(m->content);
    chat->total_chars += (long)m->content_len;
    chat->n_msgs++;
}


/* Auto-assign importance based on message type (Harness-1 §3.2).
 * Called by all message-adding functions to ensure consistent tagging. */
static llm_msg_importance_t llm_importance_for_type(llm_msg_type_t type) {
    switch (type) {
        case LLM_MSG_SYSTEM:
        case LLM_MSG_USER_QUERY:
        case LLM_MSG_SCRATCHPAD:
            return LLM_MSG_IMPORTANCE_CRITICAL;
        /* D2 FIX: EVICTION_SUMMARY demoted from CRITICAL to HIGH.
         * Breadcrumb indices are useful but not irreplaceable — they
         * should not consume the same budget tier as the system prompt
         * and user query. They're removed at the start of each eviction
         * cycle anyway, so CRITICAL protection was only relevant between
         * evictions where it wasted context budget. */
        case LLM_MSG_EVICTION_SUMMARY:
        case LLM_MSG_MEMORY_INDEX:
        case LLM_MSG_PINNED:
        case LLM_MSG_SKILLS:
        case LLM_MSG_LESSONS:
        case LLM_MSG_STRATEGIES:
        case LLM_MSG_ANTIPATTERNS:
        case LLM_MSG_PREV_RESULT:
            return LLM_MSG_IMPORTANCE_HIGH;
        case LLM_MSG_ERROR:
        case LLM_MSG_MEMORY_HINT:
            return LLM_MSG_IMPORTANCE_LOW;
        case LLM_MSG_TOOL_RESULT:
        case LLM_MSG_GENERIC:
        case LLM_MSG_THINKING:
        default:
            return LLM_MSG_IMPORTANCE_NORMAL;
    }
}

/* Add a typed message — sets msg_type for structured routing. */
void llm_chat_add_typed(llm_chat_t *chat, const char *role,
                         const char *content, llm_msg_type_t type) {
    if (llm_chat_ensure_capacity(chat) != 0) return;
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
    if (!m->role || !m->content) {
        nash_log("[llm] CRITICAL: strdup failed for typed message role=%s", role);
        free(m->role);
        free(m->content);
        return;
    }
    m->content_len = strlen(m->content);
    chat->total_chars += (long)m->content_len;
    m->msg_type = type;
    m->importance = llm_importance_for_type(type);
    chat->n_msgs++;
}

/* Remove all messages of a given type. Returns count removed. */
int llm_chat_remove_by_type(llm_chat_t *chat, llm_msg_type_t type) {
    if (!chat) return 0;
    int removed = 0;
    int dst = 0;
    for (int src = 0; src < chat->n_msgs; src++) {
        if (chat->msgs[src].msg_type == type) {
            chat->total_chars -= (long)chat->msgs[src].content_len;
            llm_msg_free_fields(&chat->msgs[src]);
            removed++;
        } else {
            if (dst != src)
                chat->msgs[dst] = chat->msgs[src];
            dst++;
        }
    }
    chat->n_msgs = dst;
    return removed;
}

/* D1 FIX: Single-pass removal of multiple message types.
 * Avoids 3× O(n) scanning when removing SCRATCHPAD + EVICTION_SUMMARY + MEMORY_HINT. */
int llm_chat_remove_by_types(llm_chat_t *chat,
                              const llm_msg_type_t *types, int n_types) {
    if (!chat || !types || n_types <= 0) return 0;
    int removed = 0;
    int dst = 0;
    for (int src = 0; src < chat->n_msgs; src++) {
        int match = 0;
        for (int t = 0; t < n_types; t++) {
            if (chat->msgs[src].msg_type == types[t]) {
                match = 1;
                break;
            }
        }
        if (match) {
            chat->total_chars -= (long)chat->msgs[src].content_len;
            llm_msg_free_fields(&chat->msgs[src]);
            removed++;
        } else {
            if (dst != src)
                chat->msgs[dst] = chat->msgs[src];
            dst++;
        }
    }
    chat->n_msgs = dst;
    return removed;
}

/* Find the first message of a given type. Returns index or -1. */
int llm_chat_find_by_type(llm_chat_t *chat, llm_msg_type_t type) {
    if (!chat) return -1;
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].msg_type == type)
            return i;
    }
    return -1;
}

/* Remove a range of messages [start, end). Frees all fields. */
void llm_chat_remove_range(llm_chat_t *chat, int start, int end) {
    if (!chat || start < 0 || end > chat->n_msgs || start >= end) return;
    for (int i = start; i < end; i++) {
        chat->total_chars -= (long)chat->msgs[i].content_len;
        llm_msg_free_fields(&chat->msgs[i]);
    }
    int tail = chat->n_msgs - end;
    if (tail > 0)
        memmove(&chat->msgs[start], &chat->msgs[end],
                tail * sizeof(llm_msg_t));
    chat->n_msgs -= (end - start);
}

/* FIX D5: Insert a typed message at a specific position.
 * Grows array if needed, shifts messages from pos..n_msgs-1 forward.
 * BUG 4 FIX: strdup BEFORE memmove — if strdup fails, chat array is untouched. */
void llm_chat_insert_typed(llm_chat_t *chat, int pos,
                            const char *role, const char *content,
                            llm_msg_type_t type) {
    if (!chat || pos < 0 || pos > chat->n_msgs) return;
    if (llm_chat_ensure_capacity(chat) != 0) return;
    /* BUG 4 FIX: Allocate strings BEFORE shifting array.
     * Previously, memmove ran first, then strdup failure left a
     * corrupted hole at pos with shifted messages at pos+1..n_msgs. */
    char *r = strdup(role);
    char *c = strdup(content);
    if (!r || !c) {
        nash_log("[llm] CRITICAL: strdup failed for inserted typed message role=%s", role);
        free(r);
        free(c);
        return;
    }
    /* Shift existing messages to make room */
    int tail = chat->n_msgs - pos;
    if (tail > 0)
        memmove(&chat->msgs[pos + 1], &chat->msgs[pos],
                tail * sizeof(llm_msg_t));
    llm_msg_t *m = &chat->msgs[pos];
    memset(m, 0, sizeof(*m));
    m->role = r;
    m->content = c;
    m->content_len = strlen(c);
    chat->total_chars += (long)m->content_len;
    m->msg_type = type;
    m->importance = llm_importance_for_type(type);
    chat->n_msgs++;
}

/* Add a tool result message (role: "tool" with tool_call_id) */
void llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                               const char *content) {
    if (llm_chat_ensure_capacity(chat) != 0) return;
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup("tool");
    m->content = strdup(content);
    m->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
    if (!m->role || !m->content) {
        nash_log("[llm] CRITICAL: strdup failed for tool result");
        free(m->role);
        free(m->content);
        free(m->tool_call_id);
        return;
    }
    m->content_len = strlen(m->content);
    chat->total_chars += (long)m->content_len;
    m->msg_type = LLM_MSG_TOOL_RESULT;
    m->importance = llm_importance_for_type(m->msg_type);
    chat->n_msgs++;
}

/* Add an assistant message with tool_calls (for conversation history).
 * Proposal C: Caches the outbound tool_call_id from tool_calls_json at creation
 * time for O(1) partner matching during eviction (eliminates repeated JSON parsing). */
void llm_chat_add_assistant_tool_call(llm_chat_t *chat, const char *content,
                                       const char *tool_calls_json) {
    if (llm_chat_ensure_capacity(chat) != 0) return;
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup("assistant");
    m->content = content ? strdup(content) : strdup("");
    m->tool_calls_json = tool_calls_json ? strdup(tool_calls_json) : NULL;
    if (!m->role || !m->content) {
        nash_log("[llm] CRITICAL: strdup failed for assistant tool call");
        free(m->role);
        free(m->content);
        free(m->tool_calls_json);
        return;
    }
    /* Proposal C: Extract and cache outbound tool_call_id from JSON */
    if (m->tool_calls_json) {
        cJSON *tc_arr = cJSON_Parse(m->tool_calls_json);
        if (tc_arr && cJSON_IsArray(tc_arr)) {
            cJSON *first = cJSON_GetArrayItem(tc_arr, 0);
            if (first) {
                cJSON *id_item = cJSON_GetObjectItem(first, "id");
                if (id_item && cJSON_IsString(id_item))
                    m->tool_call_id_outbound = strdup(id_item->valuestring);
            }
        }
        cJSON_Delete(tc_arr);
    }
    m->content_len = strlen(m->content);
    chat->total_chars += (long)m->content_len;
    m->msg_type = LLM_MSG_GENERIC; // Assistant tool calls are generally generic context
    m->importance = llm_importance_for_type(m->msg_type);
    chat->n_msgs++;
}

/* Replace the content of message at index `idx` with `new_content` (takes ownership).
 * Updates content_len and total_chars incrementally. */
void llm_chat_replace_content(llm_chat_t *chat, int idx, char *new_content) {
    if (!chat || idx < 0 || idx >= chat->n_msgs) { free(new_content); return; }
    llm_msg_t *m = &chat->msgs[idx];
    chat->total_chars -= (long)m->content_len;
    free(m->content);
    m->content = new_content;
    m->content_len = new_content ? strlen(new_content) : 0;
    chat->total_chars += (long)m->content_len;
}

/* ── Chat serialization ──────────────────────────────────────── */

/* Serialize entire chat into a human-readable markdown document.
 * Format per message: ### role\n\ncontent\n\n
 * Returns malloc'd string. Caller must free. */
char *llm_chat_serialize(llm_chat_t *chat) {
    if (!chat || chat->n_msgs == 0) return strdup("");
    str_t s = str_new(4096);
    for (int i = 0; i < chat->n_msgs; i++) {
        llm_msg_t *m = &chat->msgs[i];
        str_appendf(&s, "### %s\n\n%s\n\n",
                    m->role ? m->role : "unknown",
                    m->content ? m->content : "");
    }
    return str_steal(&s);
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
    int bare_value = 0;  /* FIX BUG#10: track inserted opening quote for bare values */

    for (size_t i = 0; src[i] && j < len * 2 - 1; i++) {
        if (escape) { buf[j++] = src[i]; escape = 0; continue; }
        if (src[i] == '\\') { buf[j++] = src[i]; escape = 1; continue; }
        if (src[i] == '"') in_string = !in_string;

        /* FIX BUG#10: Close bare value quote before delimiters */
        if (bare_value && !in_string &&
            (src[i] == ',' || src[i] == '}' || src[i] == ']')) {
            buf[j++] = '"';
            bare_value = 0;
        }

        /* Detect XML fragment: truncate at < outside strings */
        if (src[i] == '<' && !in_string) {
            /* Close any open bare value and JSON structure */
            if (bare_value) buf[j++] = '"';
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
                bare_value = 1;  /* FIX BUG#10: remember to close it */
            }
            continue;
        }

        buf[j++] = src[i];
    }
    if (bare_value) buf[j++] = '"';  /* FIX BUG#10: close trailing bare value */
    buf[j] = '\0';
    return buf;
}

cJSON *llm_parse_action(const char *response, int *multi_count) {
    if (multi_count) *multi_count = 0;
    if (!response) return NULL;

    const char *start = response;

    /* Skip leading whitespace and markdown fences */
    start = skip_whitespace(start);
    if (strncmp(start, "```json", 7) == 0) {
        start += 7;
        while (*start == '\n' || *start == '\r') start++;
    } else if (strncmp(start, "```", 3) == 0) {
        start += 3;
        while (*start == '\n' || *start == '\r') start++;
    }

    const char *brace = strchr(start, '{');
    if (!brace) return NULL;

    /* Try strict parse first, tracking where parsing ended */
    const char *parse_end = NULL;
    cJSON *action = cJSON_ParseWithOpts(brace, &parse_end, 0);
    if (!action) {
        /* Strict parse failed — try repairing common JSON errors */
        char *repaired = repair_json(brace);
        if (repaired) {
            action = cJSON_Parse(repaired);
            if (action) {
                nash_log("[llm] repaired malformed JSON response");
            }
            free(repaired);
        }
        if (multi_count && action) *multi_count = 1;
        return action;
    }

    /* Detect concatenated JSON objects (e.g., gemma4/qwen3.6 emitting
     * multiple tool calls as content: {...}{...}).
     * Count how many additional JSON objects follow the first one.
     * Only the first is returned — callers use multi_count to inject
     * a corrective hint telling the model to issue one call at a time. */
    int count = 1;
    if (parse_end && multi_count) {
        const char *rest = parse_end;
        while (rest && *rest) {
            /* Skip whitespace between concatenated objects */
            while (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')
                rest++;
            if (*rest != '{') break;
            /* Try parsing the next JSON object */
            const char *next_end = NULL;
            cJSON *next = cJSON_ParseWithOpts(rest, &next_end, 0);
            if (!next) break;
            /* Verify it looks like a tool call (has "action" field) */
            if (cJSON_GetObjectItem(next, "action")) {
                count++;
            }
            cJSON_Delete(next);
            rest = next_end;
        }
        *multi_count = count;
    }

    if (count > 1) {
        nash_log("[llm] detected %d concatenated tool calls in content "
                 "(only first executed)", count);
    }

    return action;
}

/* ── Fetch context size from /props ──────────────────────────── */

int llm_fetch_context_size(const char *api_base) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/props", api_base);

    str_t response = str_new(4096);
    if (http_get(url, 5L, &response) != 0) {
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

/* ── Fetch model name from /v1/models ──────────────────────── */

char *llm_fetch_model_name(const char *api_base) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/v1/models", api_base);

    str_t response = str_new(4096);
    if (http_get(url, 5L, &response) != 0) {
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

    str_t response = str_new(8192);
    if (http_get(url, 5L, &response) != 0) {
        str_free(&response);
        return NULL;
    }

    return str_steal(&response);
}


/* Apply chat template via /apply-template endpoint (#7).
 * Sends a minimal [{"role":"user","content":query}] and gets back
 * the formatted prompt string. Returns malloc'd string or NULL. */
char *llm_apply_template(const char *api_base, const char *user_query) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/apply-template", api_base);

    cJSON *req = cJSON_CreateObject();
    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", user_query);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(req, "messages", msgs);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t response = str_new(4096);
    if (http_post(url, body, headers, 5L, &response) != 0) {
        curl_slist_free_all(headers);
        free(body);
        str_free(&response);
        return NULL;
    }
    curl_slist_free_all(headers);
    free(body);

    /* Parse response: {"prompt": "..."} */
    cJSON *resp = cJSON_Parse(response.data);
    str_free(&response);
    if (!resp) return NULL;

    cJSON *prompt = cJSON_GetObjectItem(resp, "prompt");
    char *result = NULL;
    if (prompt && prompt->valuestring)
        result = strdup(prompt->valuestring);
    cJSON_Delete(resp);
    return result;
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

    typedef struct { int idx; float val; } pair_t;
    pair_t *px = malloc(n * sizeof(pair_t));
    pair_t *py = malloc(n * sizeof(pair_t));
    if (!px || !py) { free(rx); free(ry); free(px); free(py); return 0.0f; }

    for (int i = 0; i < n; i++) { px[i] = (pair_t){i, x[i]}; py[i] = (pair_t){i, y[i]}; }

    int cmp_float(const void *a, const void *b) {
        float diff = ((pair_t *)a)->val - ((pair_t *)b)->val;
        return (diff > 0) - (diff < 0);
    }

    qsort(px, n, sizeof(pair_t), cmp_float);
    for (int i = 0; i < n; i++) rx[px[i].idx] = (float)(i + 1);

    qsort(py, n, sizeof(pair_t), cmp_float);
    for (int i = 0; i < n; i++) ry[py[i].idx] = (float)(i + 1);

    free(px); free(py);

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
    free(rx); free(ry);
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

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t response = str_new(16384);
    if (http_post(url, body, headers, 30L, &response) != 0) {
        curl_slist_free_all(headers);
        free(body);
        str_free(&response);
        return result;
    }
    curl_slist_free_all(headers);
    free(body);

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

/* ── Belief Entropy probe — MMPO [arXiv:2605.30159] ────────────── */

belief_entropy_result_t llm_belief_entropy_probe(const char *api_base,
                                                    const char *memory_context,
                                                    const char *anchor_question,
                                                    int n_predict, int n_probs,
                                                    float temperature) {
    belief_entropy_result_t result = {0, 0, 0, 0};

    /* Build prompt: memory context + anchor question */
    str_t prompt = str_new(8192);
    if (memory_context && strlen(memory_context) > 0) {
        str_appendf(&prompt,
            "Current memory state:\n%s\n\n", memory_context);
    }
    str_appendf(&prompt,
        "Question: %s\n\nAnswer:", anchor_question);

    /* Build /completion request with logprobs */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt", prompt.data);
    cJSON_AddNumberToObject(req, "n_predict", n_predict);
    cJSON_AddNumberToObject(req, "n_probs", n_probs);
    cJSON_AddNumberToObject(req, "temperature", temperature);
    cJSON_AddBoolToObject(req, "cache_prompt", 1);
    cJSON_AddBoolToObject(req, "stream", 0);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    str_free(&prompt);
    if (!body) return result;

    char url[1024];
    snprintf(url, sizeof(url), "%s/completion", api_base);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t response = str_new(16384);
    if (http_post(url, body, headers, 30L, &response) != 0) {
        curl_slist_free_all(headers);
        free(body);
        str_free(&response);
        return result;
    }
    curl_slist_free_all(headers);
    free(body);

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
    if (n_tokens < 1) {
        cJSON_Delete(resp);
        return result;
    }

    /* Compute per-token entropy from top logprobs */
    float h_sum = 0;
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
        h_sum += h;
    }

    result.h_mean = h_sum / n_tokens;
    result.h_total = h_sum;
    result.n_tokens = n_tokens;
    result.ok = 1;

    cJSON_Delete(resp);
    return result;
}

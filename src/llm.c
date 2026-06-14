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

void llm_chat_free(llm_chat_t *chat) {
    if (!chat) return;
    for (int i = 0; i < chat->n_msgs; i++) {
        free(chat->msgs[i].role);
        free(chat->msgs[i].content);
        free(chat->msgs[i].tool_call_id);
        free(chat->msgs[i].tool_calls_json);
    }
    free(chat->msgs);
    free(chat->last_tool_call_id);
    free(chat->last_tool_calls_json);
    free(chat);
}

void llm_chat_add(llm_chat_t *chat, const char *role, const char *content) {
    if (chat->n_msgs >= chat->cap_msgs) {
        int new_cap = chat->cap_msgs * 2;
        llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
        if (!tmp) return;  /* FIX BUG#2: don't lose old pointer on realloc failure */
        chat->msgs = tmp;
        chat->cap_msgs = new_cap;
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
    chat->n_msgs++;
}

/* Remove all messages whose content starts with prefix.
 * Returns the number of messages removed. */
int llm_chat_remove_by_prefix(llm_chat_t *chat, const char *prefix) {
    if (!chat || !prefix) return 0;
    size_t plen = strlen(prefix);
    int removed = 0;
    int dst = 0;
    for (int src = 0; src < chat->n_msgs; src++) {
        if (chat->msgs[src].content &&
            strncmp(chat->msgs[src].content, prefix, plen) == 0) {
            /* Free this message */
            free(chat->msgs[src].role);
            free(chat->msgs[src].content);
            free(chat->msgs[src].tool_call_id);
            free(chat->msgs[src].tool_calls_json);
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

/* Auto-assign importance based on message type (Harness-1 §3.2).
 * Called by all message-adding functions to ensure consistent tagging. */
static llm_msg_importance_t llm_importance_for_type(llm_msg_type_t type) {
    switch (type) {
        case LLM_MSG_SYSTEM:
        case LLM_MSG_USER_QUERY:
        case LLM_MSG_SCRATCHPAD:
        case LLM_MSG_EVICTION_SUMMARY:
            return LLM_MSG_IMPORTANCE_CRITICAL;
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
    if (chat->n_msgs >= chat->cap_msgs) {
        int new_cap = chat->cap_msgs * 2;
        llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
        if (!tmp) return;  /* FIX BUG#2 */
        chat->msgs = tmp;
        chat->cap_msgs = new_cap;
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
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
            free(chat->msgs[src].role);
            free(chat->msgs[src].content);
            free(chat->msgs[src].tool_call_id);
            free(chat->msgs[src].tool_calls_json);
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
        free(chat->msgs[i].role);
        free(chat->msgs[i].content);
        free(chat->msgs[i].tool_call_id);
        free(chat->msgs[i].tool_calls_json);
    }
    int tail = chat->n_msgs - end;
    if (tail > 0)
        memmove(&chat->msgs[start], &chat->msgs[end],
                tail * sizeof(llm_msg_t));
    chat->n_msgs -= (end - start);
}

/* FIX D5: Insert a typed message at a specific position.
 * Grows array if needed, shifts messages from pos..n_msgs-1 forward. */
void llm_chat_insert_typed(llm_chat_t *chat, int pos,
                            const char *role, const char *content,
                            llm_msg_type_t type) {
    if (!chat || pos < 0 || pos > chat->n_msgs) return;
    if (chat->n_msgs >= chat->cap_msgs) {
        int new_cap = chat->cap_msgs * 2;
        llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
        if (!tmp) return;  /* FIX BUG#2 */
        chat->msgs = tmp;
        chat->cap_msgs = new_cap;
    }
    /* Shift existing messages to make room */
    int tail = chat->n_msgs - pos;
    if (tail > 0)
        memmove(&chat->msgs[pos + 1], &chat->msgs[pos],
                tail * sizeof(llm_msg_t));
    llm_msg_t *m = &chat->msgs[pos];
    memset(m, 0, sizeof(*m));
    m->role = strdup(role);
    m->content = strdup(content);
    m->msg_type = type;
    m->importance = llm_importance_for_type(type);
    chat->n_msgs++;
}

/* Add a tool result message (role: "tool" with tool_call_id) */
void llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                               const char *content) {
    if (chat->n_msgs >= chat->cap_msgs) {
        int new_cap = chat->cap_msgs * 2;
        llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
        if (!tmp) return;  /* FIX BUG#2 */
        chat->msgs = tmp;
        chat->cap_msgs = new_cap;
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
        int new_cap = chat->cap_msgs * 2;
        llm_msg_t *tmp = realloc(chat->msgs, (size_t)new_cap * sizeof(llm_msg_t));
        if (!tmp) return;  /* FIX BUG#2 */
        chat->msgs = tmp;
        chat->cap_msgs = new_cap;
    }
    llm_msg_t *m = &chat->msgs[chat->n_msgs];
    memset(m, 0, sizeof(*m));
    m->role = strdup("assistant");
    m->content = content ? strdup(content) : strdup("");
    m->tool_calls_json = tool_calls_json ? strdup(tool_calls_json) : NULL;
    chat->n_msgs++;
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

cJSON *llm_parse_action(const char *response) {
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

    /* Try strict parse first */
    cJSON *action = cJSON_Parse(brace);
    if (action) return action;

    /* Strict parse failed — try repairing common JSON errors */
    char *repaired = repair_json(brace);
    if (repaired) {
        action = cJSON_Parse(repaired);
        if (action) {
            nash_log("[llm] repaired malformed JSON response");
        }
        free(repaired);
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

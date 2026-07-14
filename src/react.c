#include "react_internal.h"
#include "compress.h"
#include "tui.h"  /* g_tui_active — for condvar timeout escape hatch */
#include "tools_internal.h"  /* tool_device_cleanup — stream save on done */

/* Constant moved from react_internal.h (used only here). */
#define REACT_SP_BM25_BUDGET        500
/* Cap total augmentation chars (thoughts + scratchpad) to prevent
 * query dilution.  With 100+ tokenized words, each term match contributes only
 * ~0.01 to the score, reducing discriminative power.  Cap at ~400 chars
 * produces ~30 augmented words — enough for context but not overwhelming. */
#define REACT_BM25_MAX_AUGMENT_CHARS 400

/* Log a parse error to journal + store.  Shared between the two parse-error
 * branches (no JSON / missing "action" field) to eliminate duplication. */
static void log_parse_error(react_ctx_t *ctx, int step, const char *type,
                            const char *response, const char *desc) {
    char *err_hash = store_save(ctx->tools->store, response);
    char *err_alias = tool_register_alias(ctx->tools,
                                err_hash ? err_hash : "");
    cJSON *err_p = cJSON_CreateObject();
    cJSON_AddStringToObject(err_p, "type", type);
    cJSON_AddStringToObject(err_p, "raw_preview",
        strlen(response) > 200 ? "(truncated)" : response);
    journal_append(ctx->tools->journal, ctx->tools->react_loop,
                   step, "parse_error", err_p, err_alias,
                   strlen(response), 0, desc, NULL, 0);
    cJSON_Delete(err_p);
    free(err_alias);
    free(err_hash);
}

/* Wait for user pause/redirect — shared between top-of-loop and bottom-of-loop
 * pause handlers. Waits on condvar, injects redirect query into chat, and
 * cleans up checkpoint. Caller is responsible for outer condition checks. */
static void react_wait_for_redirect(react_ctx_t *ctx, llm_chat_t *chat,
                                     int step,
                                     react_event_fn on_event, void *userdata) {
    {
        react_event_t ev = {0};
        ev.react_loop = ctx->tools->react_loop;
        ev.type = REACT_EVENT_WARNING;
        ev.step = step;
        ev.message = "Paused (Space to resume, type query to redirect)";
        react_emit(on_event, userdata, &ev);
    }
    /* Wait for user to provide a redirect query (or resume).
     * Uses pthread_cond_timedwait with 2s timeout as an escape hatch:
     * if the TUI thread crashes/exits without signaling, the inference
     * thread won't block forever — it checks g_tui_active each cycle
     * and breaks out with a synthetic "quit" redirect. */
    ctx->pause_waiting = 1;
    pthread_mutex_lock(&ctx->pause_mutex);
    while (!ctx->pause_query) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        pthread_cond_timedwait(&ctx->pause_cond, &ctx->pause_mutex, &ts);
        if (!ctx->pause_query && !atomic_load(&g_tui_active)) {
            ctx->pause_query = strdup("quit");
            break;
        }
    }
    char *redirect = ctx->pause_query;
    ctx->pause_query = NULL;
    ctx->pause_waiting = 0;
    ctx->pause_requested = 0;
    pthread_mutex_unlock(&ctx->pause_mutex);

    /* Reset abort flag so next LLM call proceeds normally */
    ctx->provider->abort_retry = 0;

    /* Inject the redirect query into the chat context */
    char *inject_msg = malloc(strlen(redirect) + 64);
    if (inject_msg) {
        snprintf(inject_msg, strlen(redirect) + 64,
                 "[User redirect]\n%s", redirect);
        llm_chat_add(chat, "user", inject_msg);
        if (chat->n_msgs > 0)
            chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_NORMAL;
        free(inject_msg);
    }
    free(redirect);
    react_checkpoint_remove(ctx);
}

/* ── helpers ─────────────────────────────────────────── */

/* Get chars-per-token from runtime state (mutable) instead of
 * provider config (INIT-ONLY). Falls back to provider config, then 3.5. */
float react_get_chars_per_token(const react_ctx_t *ctx) {
    if (ctx->rt.chars_per_token > 0)
        return ctx->rt.chars_per_token;
    if (ctx->provider && ctx->provider->cfg.chars_per_token > 0)
        return ctx->provider->cfg.chars_per_token;
    return 3.5f;
}

/* Compute dynamic keep_head: count consecutive CRITICAL-importance messages
 * from the start of the chat. Adapts to actual injection config rather than
 * assuming a fixed [system, memory_index, pinned] header structure.
 * Only CRITICAL messages (system prompt, user query, scratchpad) extend
 * the head — HIGH messages (EVICTION_SUMMARY, MEMORY_INDEX) remain evictable
 * to avoid double-protection. */
int react_compute_keep_head(const llm_chat_t *chat) {
    int head = 0;
    for (int i = 0; i < chat->n_msgs; i++) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_CRITICAL) {
            head = i + 1;
        } else {
            break;
        }
    }
    /* Always protect at least the system prompt */
    return head > 0 ? head : 1;
}

/* Compute dynamic keep_tail: walk backward from end to find the last
 * 2 complete tool-call exchange boundaries (assistant+tool_result pairs).
 * Adapts to actual tail structure instead of assuming fixed 4 messages. */
int react_compute_keep_tail(const llm_chat_t *chat) {
    int pairs_found = 0;
    int tail_start = chat->n_msgs;
    for (int i = chat->n_msgs - 1; i >= 0 && pairs_found < 2; i--) {
        /* A tool_result followed by its tool_call = one pair */
        if (chat->msgs[i].tool_call_id && i > 0 &&
            chat->msgs[i - 1].tool_calls_json) {
            tail_start = i - 1;
            pairs_found++;
            i--;  /* skip the assistant tool_call too */
        } else if (chat->msgs[i].role &&
                   strcmp(chat->msgs[i].role, "user") == 0 &&
                   !chat->msgs[i].tool_call_id &&
                   /* Exclude injected hints/summaries with role="user".
                    * Without this, MEMORY_HINT nudges (injected with role="user")
                    * inflate keep_tail, shrinking the evictable range. */
                   chat->msgs[i].msg_type != LLM_MSG_MEMORY_HINT &&
                   chat->msgs[i].msg_type != LLM_MSG_EVICTION_SUMMARY &&
                   chat->msgs[i].msg_type != LLM_MSG_SCRATCHPAD) {
            /* user message (e.g., user_ask response, hint) — include */
            tail_start = i;
        } else {
            /* Skip injected hint/summary messages — don't let them
             * silently expand the protected tail zone. Only genuine conversation
             * messages should anchor tail boundaries. */
            llm_msg_type_t mt = chat->msgs[i].msg_type;
            if (mt == LLM_MSG_EVICTION_SUMMARY || mt == LLM_MSG_MEMORY_HINT ||
                mt == LLM_MSG_SCRATCHPAD) {
                continue;
            }
            /* Stop if we hit something that isn't part of recent exchanges */
            if (pairs_found > 0) break;
            tail_start = i;
        }
    }
    int keep = chat->n_msgs - tail_start;
    /* Cap keep_tail to prevent unbounded growth from interleaved
     * user_ask responses. Without this, the tail can grow to 6-8+ messages,
     * shrinking the evictable range and forcing emergency eviction. */
    if (keep > REACT_KEEP_TAIL_MAX) keep = REACT_KEEP_TAIL_MAX;
    return keep >= 2 ? keep : 2;
}

const char *react_json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

/* Build enriched BM25 query from user_query + recent thoughts + scratchpad.
 * Shared between eviction (react_maybe_evict) and context construction
 * (react_build_context) to avoid duplicated logic. */
char *react_build_bm25_query(const llm_chat_t *chat, const char *user_query,
                             scratchpad_t *scratch) {
    str_t buf = str_new(1024);
    if (user_query && user_query[0])
        str_append_cstr(&buf, user_query);
    /* Fallback: extract query from chat if user_query is short/empty.
     * Scan backward — after eviction the query may only survive
     * near the head, but backward scan is O(1) amortized — there is
     * typically just one USER_QUERY message, found quickly. */
    if (buf.len < 10) {
        for (int i = chat->n_msgs - 1; i >= 0; i--) {
            if (chat->msgs[i].msg_type == LLM_MSG_USER_QUERY &&
                chat->msgs[i].content && chat->msgs[i].content_len >= 10) {
                str_append_cstr(&buf, chat->msgs[i].content);
                break;
            }
        }
    }
    /* Track augmentation budget to prevent query dilution.
     * User query terms are primary; augmented content is secondary context. */
    size_t augment_remaining = REACT_BM25_MAX_AUGMENT_CHARS;

    /* Augment with recent assistant thoughts (last 3 exchanges) */
    int thought_count = 0;
    for (int i = chat->n_msgs - 1; i >= 0 && thought_count < 3; i--) {
        if (augment_remaining == 0) break;
        if (chat->msgs[i].role && strcmp(chat->msgs[i].role, "assistant") == 0 &&
            chat->msgs[i].content && chat->msgs[i].content_len > 20) {
            str_append_cstr(&buf, " ");
            size_t tlen = chat->msgs[i].content_len;
            if (tlen > REACT_THOUGHT_TRUNC_LEN) tlen = REACT_THOUGHT_TRUNC_LEN;
            if (tlen > augment_remaining) tlen = augment_remaining;
            /* Clamp to UTF-8 boundary to avoid splitting multi-byte chars */
            tlen = utf8_clamp(chat->msgs[i].content, tlen);
            str_append(&buf, chat->msgs[i].content, tlen);
            augment_remaining -= tlen;
            thought_count++;
        }
    }
    /* Augment with scratchpad content if available */
    if (augment_remaining > 0 && scratch && scratch->count > 0) {
        size_t sp_budget = augment_remaining < REACT_SP_BM25_BUDGET
            ? augment_remaining : REACT_SP_BM25_BUDGET;
        char *sp = scratchpad_serialize_budget(scratch, sp_budget);
        if (sp && sp[0]) {
            str_append_cstr(&buf, " ");
            str_append_cstr(&buf, sp);
        }
        free(sp);
    }
    return str_steal(&buf);
}

/* Find the partner of a tool_call or tool_result message by scanning.
 * Matches by scanning (not adjacency) and validates importance.
 * Scans past interleaved non-tool messages (hints, error recovery).
 * Uses cached tool_call_id_outbound for O(1) ID lookup instead
 * of parsing tool_calls_json on every call. Falls back to JSON parse if
 * the cached field is not populated (e.g., checkpoint-restored messages). */
int react_find_tool_partner(const llm_chat_t *chat, int msg_idx,
                            int range_start, int range_end) {
    if (msg_idx < 0 || msg_idx >= chat->n_msgs) return -1;
    const llm_msg_t *msg = &chat->msgs[msg_idx];

    if (msg->tool_calls_json) {
        /* Use cached outbound ID if available, else parse JSON */
        const char *expected_id = msg->tool_call_id_outbound;
        cJSON *tc_arr = NULL;
        if (!expected_id) {
            /* Fallback for messages not created via llm_chat_add_assistant_tool_call
             * (e.g., restored from checkpoint without the cached field). */
            tc_arr = cJSON_Parse(msg->tool_calls_json);
            if (tc_arr && cJSON_IsArray(tc_arr)) {
                cJSON *first = cJSON_GetArrayItem(tc_arr, 0);
                if (first) {
                    cJSON *id_item = cJSON_GetObjectItem(first, "id");
                    if (id_item && cJSON_IsString(id_item))
                        expected_id = id_item->valuestring;
                }
            }
        }
        /* without an ID we can't safely match — bail out rather
         * than accepting the first arbitrary tool_result message. */
        if (!expected_id) {
            cJSON_Delete(tc_arr);
            return -1;
        }
        int result = -1;
        for (int pi = msg_idx + 1; pi < range_end && pi < chat->n_msgs; pi++) {
            if (!chat->msgs[pi].tool_call_id) continue;
            if (strcmp(chat->msgs[pi].tool_call_id, expected_id) != 0)
                continue;
            if (chat->msgs[pi].importance >= LLM_MSG_IMPORTANCE_HIGH) {
                result = -1;
                break;
            }
            result = pi;
            break;
        }
        cJSON_Delete(tc_arr);
        return result;
    }
    if (msg->tool_call_id) {
        /* tool_result: scan backward for tool_call. use cached
         * tool_call_id_outbound for O(1) match instead of strstr on JSON. */
        for (int pi = msg_idx - 1; pi >= range_start; pi--) {
            if (!chat->msgs[pi].tool_calls_json) continue;
            /* Use cached ID if available */
            if (chat->msgs[pi].tool_call_id_outbound) {
                if (strcmp(chat->msgs[pi].tool_call_id_outbound, msg->tool_call_id) == 0) {
                    if (chat->msgs[pi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                        return -1;
                    return pi;
                }
                continue;
            }
            /* Fallback: strstr check on raw JSON.
             * Match the quoted form "<tool_call_id>" to prevent
             * substring false positives (e.g. "call_12" matching "call_123"). */
            {
                size_t id_len = strlen(msg->tool_call_id);
                char *quoted_id = malloc(id_len + 3);  /* '"' + id + '"' + NUL */
                if (quoted_id) {
                    quoted_id[0] = '"';
                    memcpy(quoted_id + 1, msg->tool_call_id, id_len);
                    quoted_id[id_len + 1] = '"';
                    quoted_id[id_len + 2] = '\0';
                    int found = strstr(chat->msgs[pi].tool_calls_json, quoted_id) != NULL;
                    free(quoted_id);
                    if (found) {
                        if (chat->msgs[pi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                            return -1;
                        return pi;
                    }
                }
            }
        }
    }
    return -1;
}

/* Recover tool_call threading from surviving messages after eviction.
 * Scans backward for the last tool_calls_json, then uses react_find_tool_partner
 * to locate the matching result (not adjacency — handles interleaved messages).
 * Used by both progressive eviction (pass3) and emergency eviction. */
void react_recover_tool_threading(llm_chat_t *chat) {
    free(chat->last_tool_call_id);
    chat->last_tool_call_id = NULL;
    free(chat->last_tool_calls_json);
    chat->last_tool_calls_json = NULL;
    for (int i = chat->n_msgs - 1; i >= 0; i--) {
        if (chat->msgs[i].tool_calls_json) {
            chat->last_tool_calls_json = strdup(chat->msgs[i].tool_calls_json);
            /* Use scanning partner match instead of assuming i+1 adjacency.
             * Interleaved hints/errors can separate tool_call from result. */
            int partner = react_find_tool_partner(chat, i, 0, chat->n_msgs);
            if (partner >= 0 && chat->msgs[partner].tool_call_id)
                chat->last_tool_call_id = strdup(chat->msgs[partner].tool_call_id);
            break;
        }
    }
}

/* Build the full system prompt string (base + model-specific rules).
 * Returns malloc'd string — caller must free.
 * Used for both chat injection and journal logging. */
char *react_build_system_prompt(const config_t *cfg, const char *session_dir) {
    const char *workspace = cfg ? cfg->workspace : NULL;
    char *base = tools_system_prompt(session_dir, workspace);
    const char *extra = cfg ? cfg->system_prompt_extra : NULL;
    if (extra && extra[0]) {
        size_t len = strlen(base) + strlen(extra) + 64;
        char *full = malloc(len);
        if (full) {
            snprintf(full, len, "%s\n\n[MODEL-SPECIFIC RULES]\n%s", base, extra);
            free(base);
            return full;
        }
    }
    return base;
}

/* Add system prompt to chat, appending model-specific rules if configured.
 * Uses react_build_system_prompt() to avoid duplication. */
void react_add_system_prompt(llm_chat_t *chat, const config_t *cfg,
                             const char *session_dir) {
    char *prompt = react_build_system_prompt(cfg, session_dir);
    llm_chat_add_typed(chat, "system", prompt, LLM_MSG_SYSTEM);
    free(prompt);
}


/* Log memory context injection to the journal for debugging.
 * Captures: index summary (total + type counts), pinned keys, skills recalled.
 * Called at both the checkpoint-restore path and the main react_run path. */
void react_log_memory_context(tool_ctx_t *tools, int react_loop, int step,
                               const char *mem_index,
                               const char *pinned,
                               memory_results_t *all_memories,
                               const char *query)
{
    if ((!tools->memory && !tools->ws) || !tools->journal) return;

    cJSON *params = cJSON_CreateObject();

    /* Index summary — extract from the mem_index header line
     * Format: "Memory: N entries, X lessons, Y strategies, Z skills, ..." */
    if (mem_index && strlen(mem_index) > 0) {
        cJSON_AddStringToObject(params, "index_summary", mem_index);
    } else {
        cJSON_AddStringToObject(params, "index_summary", "no entries");
    }

    /* Pinned memory keys — extract from pinned output
     * Format: "[PINNED: key1]\nvalue1\n\n[PINNED: key2]\nvalue2" */
    if (pinned && strlen(pinned) > 0) {
        cJSON *pinned_keys = cJSON_CreateArray();
        const char *p = pinned;
        while (*p) {
            /* Find "[PINNED: key]" pattern */
            if (strncmp(p, "[PINNED: ", 9) == 0) {
                const char *end = strchr(p + 9, ']');
                if (end) {
                    char key[256];
                    int klen = (int)(end - (p + 9));
                    if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
                    memcpy(key, p + 9, (size_t)klen);
                    key[klen] = '\0';
                    cJSON_AddItemToArray(pinned_keys, cJSON_CreateString(key));
                    p = end + 1;
                    /* Skip to the start of the next pinned tag or end of string */
                    const char *next_tag = strstr(p, "[PINNED: ");
                    if (next_tag) p = next_tag;
                    else p = *p ? p + 1 : p;
                } else {
                    break;
                }
            } else {
                /* Skip to next line */
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }
        }
        cJSON_AddItemToObject(params, "pinned_keys", pinned_keys);
    } else {
        cJSON_AddItemToObject(params, "pinned_keys", cJSON_CreateArray());
    }

    /* Per-type memory recall logging — skills, lessons, strategies, anti-patterns */
    static const char *type_prefixes[] = {
        "skill:", "lesson:", "strategy:", "anti-pattern:", NULL
    };
    static const char *json_keys[] = {
        "skills_matched", "lessons_matched", "strategies_matched", "antipatterns_matched"
    };
    for (int t = 0; type_prefixes[t] != NULL; t++) {
        cJSON *arr = cJSON_CreateArray();
        size_t plen = strlen(type_prefixes[t]);
        if (all_memories) {
            for (int i = 0; i < all_memories->count; i++) {
                if (all_memories->entries[i].key &&
                    strncmp(all_memories->entries[i].key, type_prefixes[t], plen) == 0) {
                    cJSON *e = cJSON_CreateObject();
                    cJSON_AddStringToObject(e, "key", all_memories->entries[i].key);
                    cJSON_AddNumberToObject(e, "score", all_memories->entries[i].relevance);
                    cJSON_AddNumberToObject(e, "raw_rel", all_memories->entries[i].raw_relevance);
                    cJSON_AddNumberToObject(e, "hits", all_memories->entries[i].recall_hits);
                    cJSON_AddNumberToObject(e, "misses", all_memories->entries[i].recall_misses);
                    cJSON_AddItemToArray(arr, e);
                }
            }
        }
        cJSON_AddItemToObject(params, json_keys[t], arr);
    }

    /* Query that triggered the recall */
    if (query) {
        /* Truncate query to 200 chars for compactness (UTF-8 safe) */
        char qtrunc[201];
        strncpy(qtrunc, query, 200);
        qtrunc[utf8_clamp(qtrunc, 200)] = '\0';
        /* Strip newlines for single-line log */
        for (int i = 0; qtrunc[i]; i++) {
            if (qtrunc[i] == '\n' || qtrunc[i] == '\r') qtrunc[i] = ' ';
        }
        cJSON_AddStringToObject(params, "query", qtrunc);
    }

    /* Store full memory context in store/ for audit trail */
    char *mc_alias = NULL;
    {
        char *content = cJSON_Print(params);
        if (content && tools->store && tools->aliases) {
            char *hash = store_save(tools->store, content);
            if (hash) {
                mc_alias = tool_register_alias(tools, hash);
                free(hash);
            }
        }
        free(content);
    }
    journal_append(tools->journal, react_loop, step, "memory_context",
                   params, mc_alias, mc_alias ? strlen(mc_alias) : 0, 0, NULL, NULL, 0);
    free(mc_alias);
    cJSON_Delete(params);
}

/* Recursively unwrap nested JSON in the "thought" field.
 * Delegates to the shared unwrap_thought() in journal.c. */
void react_sanitize_thought(cJSON *action) {
    cJSON *th = cJSON_GetObjectItemCaseSensitive(action, "thought");
    if (!th || !cJSON_IsString(th) || !th->valuestring || th->valuestring[0] != '{')
        return;
    char *clean = unwrap_thought(th->valuestring);
    if (clean) {
        free(th->valuestring);
        th->valuestring = clean;
    } else {
        /* unwrap_thought returned NULL — the thought was a JSON object
         * with no extractable thought text (e.g. the model echoed the full
         * action JSON with thought="").  Clear it to empty. */
        free(th->valuestring);
        th->valuestring = strdup("");
    }
}

/* Checkpoint restore, save, remove are in react_checkpoint.c */


void react_stream_token_cb(const char *token, void *userdata) {
    react_stream_ctx_t *sctx = userdata;
    if (!sctx->on_event) return;
    react_event_t ev = {0};
    ev.type  = REACT_EVENT_LLM_TOKEN;
    ev.step  = sctx->step;
    ev.react_loop = sctx->react_loop;
    ev.token = token;
    sctx->on_event(&ev, sctx->userdata);
}

void react_progress_cb(int processed, int total, void *userdata) {
    react_stream_ctx_t *sctx = userdata;
    if (!sctx->on_event) return;
    react_event_t ev = {0};
    ev.type = REACT_EVENT_PROMPT_PROGRESS;
    ev.step = sctx->step;
    ev.react_loop = sctx->react_loop;
    ev.prompt_progress_processed = processed;
    ev.prompt_progress_total = total;
    sctx->on_event(&ev, sctx->userdata);
}

void react_emit(react_event_fn fn, void *ud, react_event_t *ev) {
    if (fn) fn(ev, ud);
}

/* Extract the key display parameter for a tool action.
 * Derives the display param from TOOL_REGISTRY[].params_json "required"[0]
 * instead of hardcoding tool→param mappings. */
const char *react_get_action_desc(cJSON *action, const char *action_name,
                                   const char *thought) {
    /* Special cases that don't map to a required param */
    if (strcmp(action_name, "notes") == 0)
        return "[saving notes]";
    if (strcmp(action_name, "done") == 0)
        return thought;
    /* Generic: look up first required param from the registry schema */
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        if (strcmp(action_name, TOOL_REGISTRY[i].name) != 0)
            continue;
        if (!TOOL_REGISTRY[i].params_json)
            break;
        cJSON *schema = cJSON_Parse(TOOL_REGISTRY[i].params_json);
        if (!schema) break;
        cJSON *req = cJSON_GetObjectItem(schema, "required");
        if (req && cJSON_IsArray(req) && cJSON_GetArraySize(req) > 0) {
            cJSON *first = cJSON_GetArrayItem(req, 0);
            if (first && cJSON_IsString(first)) {
                const char *val = react_json_get_str(action,
                                                      first->valuestring);
                cJSON_Delete(schema);
                return val ? val : thought;
            }
        }
        cJSON_Delete(schema);
        break;
    }
    return thought;
}

/* Read the original user_query from a checkpoint without restoring full state.
 * Used by /continue to resume with the original query instead of "continue".
 * Returns heap-allocated string or NULL if no checkpoint. Caller frees. */
char *checkpoint_read_query(const char *session_dir) {
    if (!session_dir) return NULL;
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", session_dir);
    cJSON *cp = slurp_json(path);
    if (!cp) return NULL;
    cJSON *q = cJSON_GetObjectItem(cp, "user_query");
    char *result = NULL;
    if (q && q->valuestring && q->valuestring[0])
        result = strdup(q->valuestring);
    cJSON_Delete(cp);
    return result;
}

/* Extract usable text from LLM output that may be plain markdown OR a tool-call
 * JSON object (e.g. {"thought":"...","action":"notes","content":"..."}).
 * Works WITH the model's training instead of fighting it:
 *   - If output is JSON with a "content" field → extract and return that
 *   - If output is wrapped in ```markdown code fences → strip them
 *   - Otherwise → return as-is
 * Returns a new allocation (caller must free). Returns NULL on empty/NULL input. */
char *react_extract_llm_text_output(const char *raw) {
    if (!raw || !raw[0]) return NULL;

    /* Skip leading whitespace */
    raw = skip_whitespace(raw);
    if (!*raw) return NULL;

    /* Case 1: JSON tool call — extract "content" field */
    if (raw[0] == '{') {
        cJSON *j = cJSON_Parse(raw);
        if (j) {
            cJSON *c = cJSON_GetObjectItem(j, "content");
            if (c && cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
                char *result = strdup(c->valuestring);
                cJSON_Delete(j);
                return result;
            }
            /* If JSON has an "action" field, it's a tool call without content
             * (e.g. {"action":"notes","op":"list"} — model trying to call a tool
             * instead of outputting text). Reject it — return NULL so the caller
             * knows the LLM didn't produce usable text. */
            cJSON *action = cJSON_GetObjectItem(j, "action");
            if (action && cJSON_IsString(action)) {
                cJSON_Delete(j);
                return NULL;  /* tool call without content — reject */
            }
            cJSON_Delete(j);
        }
        /* JSON but not a tool call — fall through to return as-is */
    }

    /* Case 2: Code-fenced output — strip ``` wrapper */
    if (strncmp(raw, "```", 3) == 0) {
        const char *start = raw + 3;
        /* Skip optional language tag (e.g. ```markdown) */
        while (*start && *start != '\n') start++;
        if (*start == '\n') start++;
        /* Find closing ``` */
        const char *end = strstr(start, "```");
        if (end) {
            return strndup(start, end - start);
        }
        /* No closing fence — return everything after opening */
        return strdup(start);
    }

    /* Case 3: Plain text — return as-is */
    return strdup(raw);
}

/* ── Harness-1 helpers (arXiv 2606.02373) ─────────────── */

/* Auto-assign importance to a tool result based on tool name and success.
 * Harness-1 §3.2: different tools produce outputs of different value. */
static int react_tool_importance(const char *tool_name, int success) {
    if (!success) return LLM_MSG_IMPORTANCE_LOW;
    if (!tool_name) return LLM_MSG_IMPORTANCE_NORMAL;
    /* Search/analysis tools produce higher-value results */
    if (strcmp(tool_name, "grep_search") == 0 ||
        strcmp(tool_name, "web_search") == 0 ||
        strcmp(tool_name, "web_fetch") == 0 ||
        strcmp(tool_name, "memory_search") == 0)
        return LLM_MSG_IMPORTANCE_HIGH;
    /* Done is critical — never evict the final result */
    if (strcmp(tool_name, "done") == 0)
        return LLM_MSG_IMPORTANCE_CRITICAL;
    /* Notes/plan results are important (scratchpad state) */
    if (strcmp(tool_name, "notes") == 0 || strcmp(tool_name, "plan") == 0)
        return LLM_MSG_IMPORTANCE_HIGH;
    return LLM_MSG_IMPORTANCE_NORMAL;
}

/* Find tool index in TOOL_REGISTRY by name (for diversity tracking).
 * Returns -1 if not found. */
static int react_tool_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < TOOL_REGISTRY_COUNT && i < 32; i++) {
        if (TOOL_REGISTRY[i].name && strcmp(TOOL_REGISTRY[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ── main react loop ─────────────────────────────────── */

char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata) {
    llm_chat_t *chat = llm_chat_new();

    /* Reset per-loop state */
    ctx->user_ask_used = 0;

    /* Initialize mutable runtime state from provider config.
     * These values may be modified during the loop without violating
     * the provider's INIT-ONLY contract. */
    ctx->rt.chars_per_token = (ctx->provider && ctx->provider->cfg.chars_per_token > 0)
        ? ctx->provider->cfg.chars_per_token : 0;
    ctx->rt.enable_thinking = ctx->provider ? ctx->provider->cfg.enable_thinking : 0;
    ctx->rt.thinking_budget = ctx->provider ? ctx->provider->cfg.thinking_budget : -1;

    /* Defer git commits during the react loop to batch them.
     * Every memory_store/pin/unpin/delete during the loop skips individual
     * git commits; a single batch commit happens after react_post_loop. */
    if (ctx->tools->ws)
        workspace_git_defer(ctx->tools->ws);
    else if (ctx->tools->memory)
        memory_git_defer(ctx->tools->memory);

    /* Set alias sequence counter past any existing symlinks in the session
     * directory so new aliases don't collide with refs from earlier queries.
     * Without this, each react_run() would start at R<N>S0 and silently
     * shadow symlinks created by previous queries in the same session. */
    ctx->tools->aliases->next_seq =
        alias_scan_max_seq(ctx->tools->session_dir, ctx->tools->react_loop) + 1;

    /* Check for checkpoint — resume interrupted task */
    int resume_step = 0;
    resume_step = react_checkpoint_restore(ctx, chat, user_query, on_event, userdata);
    int restored = (resume_step >= 0);


    if (!restored) {
        react_build_context(ctx, chat, user_query, on_event, userdata);
    } /* end if (!restored) — context building */
    /* Layer 2: full spec snapshot on change (content-addressed, deduplicated).
     * Emitted once at session start and again whenever the spec hash changes
     * (e.g., model switch, hot-reload, --load-spec).  The full resolved spec
     * TOML is saved in the content-addressed store — identical specs across
     * sessions or loops cost zero extra storage.
     * Runs unconditionally (outside !restored) so even checkpoint-resumed
     * sessions record the spec for the current process invocation. */
    if (ctx->tools->cfg) {
        char *spec_str = config_dump_spec_to_string(ctx->tools->cfg,
                                                    ctx->tools->cfg->matched_profile_file);
        if (spec_str) {
            char *spec_hash = store_save(ctx->tools->store, spec_str);
            int changed = 0;
            if (spec_hash) {
                if (!ctx->tools->last_spec_hash ||
                    strcmp(spec_hash, ctx->tools->last_spec_hash) != 0) {
                    changed = 1;
                    free(ctx->tools->last_spec_hash);
                    ctx->tools->last_spec_hash = strdup(spec_hash);
                }
            }
            if (changed && spec_hash) {
                char *spec_alias = tool_register_alias(ctx->tools, spec_hash);
                cJSON *sp = cJSON_CreateObject();
                if (ctx->provider && ctx->provider->cfg.model_id)
                    cJSON_AddStringToObject(sp, "model", ctx->provider->cfg.model_id);
                if (ctx->tools->cfg->provider.type)
                    cJSON_AddStringToObject(sp, "provider", ctx->tools->cfg->provider.type);
                if (ctx->tools->cfg->matched_profile_file)
                    cJSON_AddStringToObject(sp, "profile", ctx->tools->cfg->matched_profile_file);
                cJSON_AddStringToObject(sp, "spec_hash", spec_hash);
                journal_append(ctx->tools->journal, ctx->tools->react_loop, 0,
                               "spec", sp, spec_alias,
                               strlen(spec_str), count_lines(spec_str), NULL, NULL, 0);
                cJSON_Delete(sp);
                free(spec_alias);
            }
            free(spec_hash);
            free(spec_str);
        }
    }

    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Cycling detection: track last action signature and its result.
     * If the model repeats the exact same action, return the cached result
     * instead of re-executing — no window, no threshold, just last-vs-current.
     * Two-stage: 1st repeat → cached result, 2nd+ repeat → refuse. */
    char *last_sig = NULL;
    char *last_result_json = NULL;  /* cached meta_str from previous action */
    char *last_ref = NULL;          /* cached store alias (e.g. "R0S24") */
    int repeat_count = 0;           /* consecutive repeats of last_sig */
    int consecutive_null_responses = 0;  /* Track LLM failures (HTTP 500 etc.) */

    int total_400_errors = 0;            /* Track HTTP 400 errors (never reset) */
    int tools_executed = 0;              /* Hallucination guard: real tools executed */
    int total_errors = 0;                /* Error budget: total tool errors across session */

    for (int step = resume_step; ctx->max_steps < 0 || step < ctx->max_steps; step++) {
        /* Check for pause request at the TOP of the loop — this catches
         * pause_requested set during error recovery paths that `continue`
         * back to the loop header (parse_error, unknown_tool, server_error,
         * user_ask).  Without this, those `continue` paths bypass the
         * pause_requested check at the bottom of the loop,
         * making the TUI appear stuck since the user's pause/redirect
         * is ignored until a normal step completion.
         *
         * Instead of breaking out (which destroys the chat context),
         * we wait on a condvar for the user to provide a redirect query.
         * This preserves the full conversation history in the llm_chat_t. */
        if (ctx->pause_requested) {
            react_checkpoint_save(ctx, step, user_query,
                                  chat->last_tool_call_id);
            react_wait_for_redirect(ctx, chat, step, on_event, userdata);
        }

        ctx->tools->step = step + 1;
        nash_log_set_context(ctx->tools->react_loop, step + 1);

        /* Emit step start */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_STEP_START;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);
        }

        /* Call LLM */
        struct timespec step_start;
        clock_gettime(CLOCK_MONOTONIC, &step_start);

        /* Thinking mode: decide before LLM call on the first step.
         * Subsequent steps inherit the decision. */
        if (ctx->tools->cfg && step == resume_step) {
            int mode = ctx->tools->cfg->thinking.mode;

            if (mode == THINKING_ON) {
                ctx->rt.enable_thinking = 1;
            } else {
                ctx->rt.enable_thinking = 0;
            }
            /* Propagate thinking budget from config */
            ctx->rt.thinking_budget = ctx->tools->cfg->thinking.budget;
        }

        llm_stats_t stats = {0};
        react_stream_ctx_t sctx = { on_event, userdata, step + 1,
                                     ctx->tools->react_loop };
        int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
        int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;

        /* Role-based provider routing: use planner_provider for step 0,
         * otherwise use the default (worker) provider.
         * The active_provider is a local — ctx->provider remains the primary
         * for all sizing/config reads (context_size, model_id, etc.). */
        provider_t *active_provider = ctx->provider;
        if (step == 0 && ctx->planner_provider)
            active_provider = ctx->planner_provider;

        /* Propagate tool filter so provider builds schema with only allowed tools */
        active_provider->tool_filter = &ctx->tools->tool_filter;
        /* Copy runtime thinking state to provider->cfg just before the
         * provider call.  This is the ONLY place cfg.enable_thinking and
         * cfg.thinking_budget are written during the loop — safe because
         * no other thread reads them between here and provider_complete_stream(). */
        active_provider->cfg.enable_thinking = ctx->rt.enable_thinking;
        active_provider->cfg.thinking_budget = ctx->rt.thinking_budget;
        char *response = provider_complete_stream(active_provider, chat, &stats,
                on_event ? react_stream_token_cb : NULL, &sctx,
                max_resp, rep_thresh,
                on_event ? react_progress_cb : NULL, &sctx);

        if (!response) {
            /* If the HTTP call was aborted because of a pause request
             * (Space pressed), skip error handling — continue to the
             * top-of-loop where the pause_requested condvar handles it. */
            if (ctx->pause_requested) {
                continue;
            }
            consecutive_null_responses++;
            int rc = react_handle_null_response(ctx, chat,
                &consecutive_null_responses, &total_400_errors,
                &stats, step, on_event, userdata);
            if (rc) break;   /* give up */
            continue;         /* retry */
        }
        consecutive_null_responses = 0;  /* Reset on successful LLM response */
        /* P4: Decay HTTP 400 error counter on success. Without this, 5
         * unrelated 400s across a long session would exhaust the budget
         * and the 6th would give up unconditionally. Decay instead of
         * hard reset retains some caution about recurring issues. */
        if (total_400_errors > 0) total_400_errors--;

        struct timespec step_end;
        clock_gettime(CLOCK_MONOTONIC, &step_end);
        double step_elapsed = (step_end.tv_sec - step_start.tv_sec) +
                              (step_end.tv_nsec - step_start.tv_nsec) / 1e9;

        /* Output truncation recovery: detect when the model hit max_tokens
         * and the response was truncated. Common when the model tries to
         * write a very large file in one call. Inject a recovery instruction
         * telling the model to split its work into smaller chunks.
         * Borrowed from nashell's __MAX_TOKENS__ handler. */
        if (stats.completion_tokens > 0 && ctx->provider &&
            ctx->provider->cfg.max_tokens > 0 &&
            stats.completion_tokens >= ctx->provider->cfg.max_tokens) {
            /* The response was truncated — don't try to parse it as JSON */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user",
                "Your response was truncated because it exceeded the maximum "
                "output length. You MUST split your work into smaller steps:\n"
                "- For file_write: write the first ~100 lines, then use "
                "file_edit to append subsequent sections.\n"
                "- For done: summarize key findings concisely rather than "
                "including full file contents.\n"
                "- For shell_exec: run the command once; output is stored at a "
                "ref you can re-analyze with grep/head/tail.\n"
                "Retry your last action with a smaller scope.");
            /* Recovery instructions are important guidance — NORMAL */
            if (chat->n_msgs >= 2) {
                chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_NORMAL;
                chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_NORMAL;
            }

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;
            ev.message = "Output truncated at max_tokens — injected split instruction";
            react_emit(on_event, userdata, &ev);

            free(response);
            continue;
        }

        /* Parse JSON response.
         * multi_tool_count > 1 means the model emitted multiple concatenated
         * tool calls (common with gemma4/qwen3.6). Only the first is parsed;
         * we inject a corrective hint after execution. */
        int multi_tool_count = 0;
        cJSON *action = llm_parse_action(response, &multi_tool_count);

        if (!action) {
            /* No implicit done — always ask the model to confirm.
             *
             * llm_parse_action() already tried 4 strategies (strict JSON,
             * repair_json, hybrid XML, pure XML).  A plain-text response
             * that survives all 4 is definitively prose, not broken JSON.
             *
             * Rather than silently accepting it as a final answer (which
             * risks premature task termination when the model is just
             * thinking aloud), we inject it back and ask the model to
             * explicitly call done() or continue with the next tool.
             * One extra round-trip is cheap; silent data loss is not. */
            size_t resp_len = strlen(response);

            if (tools_executed > 0 && resp_len > 20) {
                nash_log("[react] plain-text response after %d tool(s) "
                         "(len=%zu) — asking model to confirm completion",
                         tools_executed, resp_len);
            }

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;
            ev.message = "Failed to parse LLM response as JSON";
            react_emit(on_event, userdata, &ev);

            log_parse_error(ctx, step + 1, "parse_error", response,
                            "LLM response was not valid JSON");

            /* Retry — tell model to use tool_calls.
             * Truncate the malformed response before injecting into context:
             * the model only needs a short excerpt to understand what went
             * wrong — injecting 10K+ tokens of non-JSON prose is pure waste
             * (observed: 55s / 10518 tokens burned in session 1782390827). */
            #define PARSE_ERR_PREVIEW_LEN 500
            if (resp_len > PARSE_ERR_PREVIEW_LEN + 40) {
                char *trunc = malloc(PARSE_ERR_PREVIEW_LEN + 64);
                if (trunc) {
                    snprintf(trunc, PARSE_ERR_PREVIEW_LEN + 64,
                             "%.*s\n[...truncated %zu chars...]",
                             (int)utf8_clamp(response, PARSE_ERR_PREVIEW_LEN),
                             response, resp_len - PARSE_ERR_PREVIEW_LEN);
                    llm_chat_add(chat, "assistant", trunc);
                    free(trunc);
                } else {
                    llm_chat_add(chat, "assistant", response);
                }
            } else {
                llm_chat_add(chat, "assistant", response);
            }
            llm_chat_add(chat, "user",
                "Your response was plain text, not a JSON tool call. "
                "If you are finished, call the `done` tool with your result. "
                "If you have more work to do, call the appropriate tool.");
            /* Parse error corrections are ephemeral — explicitly LOW */
            if (chat->n_msgs >= 2) {
                chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_LOW;
                chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_LOW;
            }
            free(response);
            continue;
        }

        /* Sanitize thought field: unwrap nested JSON if the LLM echoed
         * back a full action object as its content/thought. */
        react_sanitize_thought(action);

        const char *thought = react_json_get_str(action, "thought");
        const char *action_name = react_json_get_str(action, "action");

        if (!action_name) {
            if (thought && thought[0]) {
                /* Thought-only response — the model is reasoning without acting.
                 * This is valid (e.g., deep code analysis, planning).
                 * Log as "thinking" step (not an error), preserve in context. */
                char *think_hash = store_save(ctx->tools->store, response);
                char *think_alias = tool_register_alias(ctx->tools,
                                            think_hash ? think_hash : "");
                cJSON *think_p = cJSON_CreateObject();
                cJSON_AddStringToObject(think_p, "type", "thinking");
                cJSON_AddStringToObject(think_p, "thought", thought);
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "thinking", think_p, think_alias,
                               strlen(thought), 0, NULL, NULL, 0);
                cJSON_Delete(think_p);
                free(think_alias);
                free(think_hash);

                /* Emit as a step complete (not error) so TUI shows it */
                {
                    react_event_t ev = {0};
                    ev.react_loop = ctx->tools->react_loop;
                    ev.type = REACT_EVENT_STEP_COMPLETE;
                    ev.step = step + 1;
                    ev.max_steps = ctx->max_steps;
                    ev.step_elapsed = step_elapsed;
                    ev.action = "thinking";
                    ev.description = thought;
                    ev.stats = stats;
                    ev.context_size = ctx->provider->cfg.context_size;
                    react_emit(on_event, userdata, &ev);
                }

                /* Add thought to conversation as assistant message,
                 * followed by a brief user nudge to prevent consecutive
                 * assistant messages (which llama.cpp rejects with 400). */
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user",
                    "Good thinking. Now call a tool to act on it.");
            } else {
                /* No thought AND no action — genuine parse error */
                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_ERROR;
                ev.step = step + 1;
                ev.message = "No 'action' field in response";
                react_emit(on_event, userdata, &ev);

                log_parse_error(ctx, step + 1, "missing_action", response,
                                "LLM response missing 'action' field");

                /* Retry — tell model to use tool_calls */
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user",
                    "Your response was plain text, not a JSON tool call. "
                    "If you are finished, call the `done` tool with your result. "
                    "If you have more work to do, call the appropriate tool.");
            }
            cJSON_Delete(action);
            free(response);
            continue;
        }

        const char *desc = react_get_action_desc(action, action_name, thought);

        /* Check for user_ask — pause react loop and wait for user input */
        if (strcmp(action_name, "user_ask") == 0) {
            const char *question = react_json_get_str(action, "question");
            if (!question || !question[0]) {
                /* Model called user_ask without a question — nudge it to retry */
                const char *errmsg =
                    "ERROR: user_ask requires a 'question' parameter with a "
                    "non-empty string. Re-call user_ask with a specific question, "
                    "or use a different tool if no question is needed.";
                if (chat->last_tool_call_id) {
                    llm_chat_add_assistant_tool_call(chat, response,
                        chat->last_tool_calls_json);
                    llm_chat_add_tool_result(chat, chat->last_tool_call_id,
                                              errmsg);
                } else {
                    llm_chat_add(chat, "assistant", response);
                    llm_chat_add(chat, "user", errmsg);
                }
                cJSON_Delete(action);
                free(response);
                continue;
            }

            /* Store question in shared state for TUI to read */
            free(ctx->user_ask_question);
            ctx->user_ask_question = strdup(question);
            free(ctx->user_ask_answer);
            ctx->user_ask_answer = NULL;
            ctx->user_ask_pending = 1;
            ctx->user_ask_used = 1;

            /* Emit event so TUI shows the question */
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_USER_ASK;
            ev.step = step + 1;
            ev.message = question;
            react_emit(on_event, userdata, &ev);

            /* P7: Wait on condition variable instead of polling.
             * The TUI thread signals user_ask_cond after setting the answer.
             * Uses pthread_cond_timedwait with 2s timeout as escape hatch:
             * if the TUI exits without answering, we unblock with "(quit)". */
            pthread_mutex_lock(&ctx->user_ask_mutex);
            while (ctx->user_ask_pending) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 2;
                pthread_cond_timedwait(&ctx->user_ask_cond, &ctx->user_ask_mutex, &ts);
                /* Escape hatch: TUI gone → unblock with synthetic answer */
                if (ctx->user_ask_pending && !atomic_load(&g_tui_active)) {
                    free(ctx->user_ask_answer);
                    ctx->user_ask_answer = strdup("(quit)");
                    ctx->user_ask_pending = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&ctx->user_ask_mutex);

            /* Build tool result from user's answer */
            const char *answer = ctx->user_ask_answer;
            if (!answer) answer = "(no answer)";

            /* Log to journal — store as markdown so TUI renders it nicely */
            cJSON *ua_params = cJSON_CreateObject();
            cJSON_AddStringToObject(ua_params, "question", question);
            cJSON_AddStringToObject(ua_params, "answer", answer);
            size_t ua_md_len = strlen(question) + strlen(answer) + 64;
            char *ua_md = malloc(ua_md_len);
            snprintf(ua_md, ua_md_len,
                     "## Question\n\n%s\n\n## Answer\n\n%s\n",
                     question, answer);
            char *ua_hash = store_save(ctx->tools->store, ua_md);
            free(ua_md);
            char *ua_alias = ua_hash ? tool_register_alias(ctx->tools, ua_hash) : NULL;
            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step + 1, "user_ask", ua_params, ua_alias,
                           strlen(answer), 0, NULL, NULL, 0);
            free(ua_hash);

            /* Build result message for the model (JSON-escape the answer) */
            cJSON *ans_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(ans_obj, "answer", answer);
            char *ans_json = cJSON_PrintUnformatted(ans_obj);
            cJSON_Delete(ans_obj);
            size_t ans_len = (ans_json ? strlen(ans_json) : 2) + 32;
            char *result_msg = malloc(ans_len);
            snprintf(result_msg, ans_len, "%s\n[step %d | user_ask]",
                     ans_json ? ans_json : "{}", step + 1);
            free(ans_json);

            /* Add to chat as tool result */
            if (chat->last_tool_call_id) {
                llm_chat_add_assistant_tool_call(chat, response,
                    chat->last_tool_calls_json);
                llm_chat_add_tool_result(chat, chat->last_tool_call_id, result_msg);
            } else {
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user", result_msg);
            }
            /* user_ask answers carry user content — NORMAL importance */
            if (chat->n_msgs >= 2) {
                chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_NORMAL;
                chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_NORMAL;
            }

            free(result_msg);
            free(ua_alias);
            cJSON_Delete(ua_params);
            cJSON_Delete(action);
            free(response);
            continue;
        }

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            /* Hallucination guard: reject 'done' if no real tool has been executed.
             * Catches models that produce a plan then immediately call done with
             * the plan text. The user_ask tool doesn't count as a real tool.
             *
             * Exception: allow done on the very first step (step == resume_step).
             * This handles simple knowledge questions (e.g. "What is 4+4?")
             * where the model correctly answers without needing any tools. */
            if (tools_executed == 0 && step > resume_step) {
                const char *guard_msg =
                    "ERROR: You called 'done' without executing any real tools. "
                    "You must actually perform the task (use file_read, shell_exec, "
                    "grep_search, etc.) before calling done. Do NOT just plan — "
                    "execute the plan step by step.";
                if (chat->last_tool_call_id) {
                    llm_chat_add_assistant_tool_call(chat, response,
                        chat->last_tool_calls_json);
                    llm_chat_add_tool_result(chat, chat->last_tool_call_id,
                                              guard_msg);
                } else {
                    llm_chat_add(chat, "assistant", response);
                    llm_chat_add(chat, "user", guard_msg);
                }
                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_WARNING;
                ev.step = step + 1;
                ev.message = "Hallucination guard: rejected premature done";
                react_emit(on_event, userdata, &ev);
                cJSON_Delete(action);
                free(response);
                continue;
            }

            const char *result = react_json_get_str(action, "result");
            /* Fallback: if result is empty but thought has content, use thought.
             * Local models sometimes put the summary in "thought" and leave
             * "result" empty — the thought IS the answer for done calls. */
            if ((!result || !result[0]) && thought && thought[0]) {
                result = thought;
            }
            final_result = result ? strdup(result) : strdup("(no result)");
            react_checkpoint_remove(ctx);

            /* Auto-save done result for cross-loop inheritance.
             * Two mechanisms:
             * 1. Scratchpad section (R<N>_result) — may be pruned by post-reflection
             * 2. session_dir/result.md — survives pruning, loaded as [PREVIOUS RESULT]
             *    in the next react loop. Provides reliable fallback for user follow-ups. */
            {
                char sec_name[32];
                snprintf(sec_name, sizeof(sec_name), "R%d_result",
                         ctx->tools->react_loop);
                scratchpad_write(&ctx->tools->scratch, sec_name,
                                 final_result, 1);  /* priority 1 = high */
                scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
            }
            if (ctx->tools->session_dir) {
                char rpath[NASH_PATH_MAX];
                snprintf(rpath, sizeof(rpath), "%s/result.md",
                         ctx->tools->session_dir);
                write_file(rpath, final_result, strlen(final_result));
            }

            /* Store result for full audit trail (journal + store/) */
            ctx->tools->thought = thought;
            ctx->tools->on_event = on_event;
            ctx->tools->on_event_data = userdata;
            tool_result_t tr = tool_execute(ctx->tools, action_name, action);
            ctx->tools->on_event = NULL;
            ctx->tools->on_event_data = NULL;
            ctx->tools->thought = NULL;
            tool_result_free(&tr);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double total = (now.tv_sec - task_start.tv_sec) +
                           (now.tv_nsec - task_start.tv_nsec) / 1e9;

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_DONE;
            ev.step = step + 1;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total;
            ev.action = action_name;
            ev.description = desc;
            ev.result = final_result;
            ev.stats = stats;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);

            cJSON_Delete(action);
            free(response);
            break;
        }

        /* Cycling detection — enabled by default, disable via config:
         *   [limits]
         *   cycling_detection = false
         * When enabled, if the model repeats the exact same action as the
         * previous step, the cached result is returned without re-execution.
         * Disable for tasks that legitimately require repeated operations. */
        int cycling_enabled = ctx->tools->cfg ? ctx->tools->cfg->cycling_detection : 1;
        const char *cmd = react_json_get_str(action, "command");
        const char *path = react_json_get_str(action, "path");
        const char *pattern = react_json_get_str(action, "pattern");
        const char *content = react_json_get_str(action, "content");
        const char *old_text = react_json_get_str(action, "old_text");
        const char *new_text = react_json_get_str(action, "new_text");
        const char *query = react_json_get_str(action, "query");
        const char *question = react_json_get_str(action, "question");
        const char *url = react_json_get_str(action, "url");
        /* Include key and value in signature so memory_store
         * calls with different keys aren't falsely detected as cycling. */
        const char *key = react_json_get_str(action, "key");
        const char *value = react_json_get_str(action, "value");
        /* Include op and section in signature so notes(op="write", section="A")
         * and notes(op="write", section="B") aren't falsely detected as cycling. */
        const char *op = react_json_get_str(action, "op");
        const char *section = react_json_get_str(action, "section");
        /* Include start_line/end_line in signature so that reading different
         * line ranges of the same file is NOT detected as cycling.
         * file_read("react.c", 1, 50) and file_read("react.c", 50, 100)
         * are different actions, not repetitions. */
        cJSON *sl = cJSON_GetObjectItem(action, "start_line");
        cJSON *el = cJSON_GetObjectItem(action, "end_line");
        int start_line = sl ? (int)cJSON_GetNumberValue(sl) : 0;
        int end_line = el ? (int)cJSON_GetNumberValue(el) : 0;
        /* Include priority, regex — without these,
         * notes(priority=1) vs notes(priority=5) and grep_search(regex=true)
         * vs grep_search(regex=false) would falsely trigger cycling. */
        cJSON *pr = cJSON_GetObjectItem(action, "priority");
        cJSON *rx = cJSON_GetObjectItem(action, "regex");
        int priority = pr ? (int)cJSON_GetNumberValue(pr) : 0;
        int regex = rx ? (cJSON_IsTrue(rx) ? 1 : 0) : -1;
        /* Build action signature dynamically — no fixed buffer, no truncation.
         * Short fields (action_name, cmd, path, pattern) go verbatim for
         * debuggability.  Long fields get FNV-1a hashed to 8 hex chars each
         * (10 fields × 9 bytes = 90 bytes fixed overhead). */
        const char *cmd_s = cmd ? cmd : "";
        const char *path_s = path ? path : "";
        const char *pattern_s = pattern ? pattern : "";
        /* 90 bytes for 10 hashed fields + 9 colons + 40 for ints + 1 null */
        size_t sig_cap = strlen(action_name) + strlen(cmd_s) + strlen(path_s)
                       + strlen(pattern_s) + 90 + 48 + 1;
        char *sig = malloc(sig_cap);
        #define SIG_HASH_FIELD(s) do { \
            unsigned _h = 2166136261u; \
            if (s) { for (const char *_p = (s); *_p; _p++) \
                _h = (_h ^ (unsigned char)*_p) * 16777619u; } \
            sig_pos += snprintf(sig + sig_pos, sig_cap - (size_t)sig_pos, \
                     "%08x:", _h); \
        } while (0)
        int sig_pos = 0;
        /* Short fields go verbatim for debuggability */
        sig_pos += snprintf(sig, sig_cap, "%s:%s:%s:%s:%d:%d:%d:%d:",
                 action_name, cmd_s, path_s, pattern_s,
                 start_line, end_line, priority, regex);
        /* Long fields get hashed — no truncation, no overflow */
        SIG_HASH_FIELD(content);
        SIG_HASH_FIELD(old_text);
        SIG_HASH_FIELD(new_text);
        SIG_HASH_FIELD(query);
        SIG_HASH_FIELD(question);
        SIG_HASH_FIELD(url);
        SIG_HASH_FIELD(key);
        SIG_HASH_FIELD(value);
        SIG_HASH_FIELD(op);
        SIG_HASH_FIELD(section);
        #undef SIG_HASH_FIELD

        int is_repeat = (last_sig && strcmp(last_sig, sig) == 0);

        /* Exempt device_control from cycling detection entirely.
         * The signature doesn't capture GUI-specific params (x, y,
         * key_name, direction, text, amount, etc.), so different
         * actions look identical — left_click(100,200) vs
         * left_click(500,600), key("tab") vs key("enter"),
         * type("hello") vs type("world") all produce the same sig.
         * Even truly identical calls (repeated scroll, screenshot
         * to verify) are standard GUI workflow, not cycling. */
        if (is_repeat && strcmp(action_name, "device_control") == 0)
            is_repeat = 0;

        /* Cycling: two-stage response to repeated identical actions.
         * Stage 1 (repeat_count==0): return cached result — model gets real data.
         * Stage 2 (repeat_count>=1): refuse — tell model to stop, result is above. */
        tool_result_t tr;
        if (cycling_enabled && is_repeat && last_result_json) {
            repeat_count++;

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;

            if (repeat_count == 1) {
                /* Stage 1: return cached result */
                ev.message = "Cycling — returning cached result from previous identical action";
                react_emit(on_event, userdata, &ev);

                cJSON *cached = cJSON_Parse(last_result_json);
                if (!cached) cached = cJSON_CreateObject();
                cJSON_AddStringToObject(cached, "note",
                    "cached — identical action already executed, result reused");
                tr = (tool_result_t){ .meta = cached, .store_ref = NULL, .success = 1 };

                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "cycling_cached", cached, last_ref,
                               strlen(last_result_json), 0,
                               NULL, NULL, 0);
            } else {
                /* Stage 2+: refuse — the result is already in context */
                ev.message = "Cycling — refusing repeated action, result already in context";
                react_emit(on_event, userdata, &ev);

                cJSON *refused = cJSON_CreateObject();
                if (repeat_count >= 4) {
                    /* Escalation: 3+ consecutive refusals -- forceful message */
                    char esc_msg[512];
                    snprintf(esc_msg, sizeof(esc_msg),
                        "STOP. This action has been refused %d times. "
                        "You are stuck in an infinite loop. "
                        "You MUST take a DIFFERENT action immediately. Options: "
                        "(1) Use a different tool or different parameters, "
                        "(2) Analyze the results you already have, "
                        "(3) Call done() with your current findings. "
                        "DO NOT repeat this action again.",
                        repeat_count - 1);
                    cJSON_AddStringToObject(refused, "error", esc_msg);
                } else {
                    cJSON_AddStringToObject(refused, "error",
                        "Refused: you already executed this identical action and received "
                        "the result above. Do NOT repeat it. Read the previous output "
                        "and continue.");
                }
                tr = (tool_result_t){ .meta = refused, .store_ref = NULL, .success = 0 };

                const char *cycling_event = repeat_count >= 4
                    ? "cycling_escalated" : "cycling_refused";
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, cycling_event, refused, last_ref,
                               0, 0, "refused repeated action", NULL, 0);

                /* ── Change 6: Cycling-triggered retrieval ─────────────
                 * The agent is stuck repeating the same action. Query
                 * memory for alternative approaches — the repeated action
                 * IS the retrieval query because it reveals what the
                 * agent is stuck on. */
                if (ctx->flags.inject_memory
                    && (ctx->tools->memory || ctx->tools->ws)) {
                    int cy_candidates = ctx->tools->cfg
                        ? ctx->tools->cfg->cycling_recall_candidates : 2;
                    double cy_min_rel = ctx->tools->cfg
                        ? ctx->tools->cfg->cycling_recall_min_relevance : 0.30;
                    char cycle_query[512];
                    snprintf(cycle_query, sizeof(cycle_query),
                        "stuck cycling: %.*s %.*s",
                        (int)utf8_clamp(action_name, 200), action_name,
                        (int)utf8_clamp(path_s, 200), path_s);
                    memory_results_t cy_mem = ctx->tools->ws
                        ? workspace_recall(ctx->tools->ws, cycle_query, cy_candidates)
                        : memory_query(ctx->tools->memory, cycle_query, cy_candidates);
                    int cy_injected = 0;
                    for (int cj = 0; cj < cy_mem.count && cy_injected < 1; cj++) {
                        int cdup = 0;
                        for (int ck = 0; ck < ctx->tools->n_recalled_keys; ck++) {
                            if (strcmp(ctx->tools->recalled_keys[ck],
                                       cy_mem.entries[cj].key) == 0) {
                                cdup = 1; break;
                            }
                        }
                        if (!cdup && cy_mem.entries[cj].relevance > cy_min_rel) {
                            char cy_hint[2048];
                            snprintf(cy_hint, sizeof(cy_hint),
                                "[MEMORY HINT — you may be stuck, consider this approach]\n"
                                "--- %s ---\n%s",
                                cy_mem.entries[cj].key,
                                cy_mem.entries[cj].value);
                            llm_chat_add_typed(chat, "user", cy_hint,
                                               LLM_MSG_MEMORY_HINT);
                            tool_track_recalled_key(ctx->tools,
                                                     cy_mem.entries[cj].key);
                            cy_injected++;
                        }
                    }
                    memory_results_free(&cy_mem);
                }
            }
        } else {
            /* Emit tool-start event so frontends can show what's about to run */
            {
                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_TOOL_START;
                ev.step = step + 1;
                ev.max_steps = ctx->max_steps;
                ev.action = action_name;
                ev.description = desc ? desc : "";
                react_emit(on_event, userdata, &ev);
            }

            /* Normal execution — inject thought into tool_ctx for journal recording */
            ctx->tools->thought = thought;
            ctx->tools->on_event = on_event;
            ctx->tools->on_event_data = userdata;
            /* Capture tool start time so journal timestamp reflects when
             * the tool began executing, not when it completed. */
            {
                struct timespec _start_tp;
                clock_gettime(CLOCK_REALTIME, &_start_tp);
                ctx->tools->start_ts = (double)_start_tp.tv_sec
                                     + (double)_start_tp.tv_nsec / 1e9;
            }
            tr = tool_execute(ctx->tools, action_name, action);
            ctx->tools->start_ts = 0;
            ctx->tools->on_event = NULL;
            ctx->tools->on_event_data = NULL;
            ctx->tools->thought = NULL;

            /* Hallucination guard: count real tool executions.
             * "done", "plan", "notes", "user_ask" are meta-tools — don't count.
             * Only tools that interact with the outside world count. */
            if (strcmp(action_name, "plan") != 0 &&
                strcmp(action_name, "notes") != 0) {
                tools_executed++;
            }

            /* Plan-then-shed: after plan() executes, preamble injections
             * (memory index, temporal, episodic, skills, etc.) have served
             * their purpose — degrade to LOW so they shed first on eviction. */
            if (strcmp(action_name, "plan") == 0) {
                ctx->rt.preamble_consumed = 1;
                react_degrade_preamble(chat);
            }

            /* Error budget: track total errors across the session */
            if (!tr.success) total_errors++;

            /* Unknown tool recovery: if the model generated a garbled tool name
             * (e.g., "shell_execshell_exec"), don't send the raw error back —
             * instead inject a corrective message and let the model retry.
             * This prevents the "garbled name → error → model confusion → 500" cascade. */
            if (!tr.success && tr.meta) {
                cJSON *err_j = cJSON_GetObjectItem(tr.meta, "error");
                if (err_j && err_j->valuestring &&
                    strstr(err_j->valuestring, "unknown tool")) {
                    /* Store garbled tool details in the store for debugging.
                     * This lets us investigate patterns: why did the model produce
                     * a garbled name? Which model? What context triggered it? */
                    cJSON *ut_data = cJSON_CreateObject();
                    cJSON_AddStringToObject(ut_data, "garbled_tool", action_name);
                    char *action_str = cJSON_PrintUnformatted(action);
                    cJSON_AddStringToObject(ut_data, "params", action_str ? action_str : "");
                    char *ut_json = cJSON_PrintUnformatted(ut_data);
                    char *ut_ref = (ut_json && ctx->tools->store)
                        ? store_save(ctx->tools->store, ut_json) : NULL;
                    char *ut_alias = (ut_ref && ctx->tools->aliases)
                        ? tool_register_alias(ctx->tools, ut_ref) : NULL;

                    /* Log the error for audit trail */
                    journal_append(ctx->tools->journal, ctx->tools->react_loop,
                                   step + 1, "unknown_tool", tr.meta, ut_alias,
                                   ut_json ? strlen(ut_json) : 0, 0,
                                   err_j->valuestring, NULL, 0);

                    /* Inject corrective message into chat (with store ref for debugging) */
                    char *tool_names = tool_registry_names_csv();
                    char correction[1024];
                    if (ut_alias) {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Stored garbled call for debugging: file_read(\"%s\"). "
                            "Available tools: %s. "
                            "Please retry with the correct tool name.",
                            action_name, ut_alias,
                            tool_names ? tool_names : "(unknown)");
                    } else {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Available tools: %s. "
                            "Please retry with the correct tool name.",
                            action_name,
                            tool_names ? tool_names : "(unknown)");
                    }
                    free(tool_names);

                    if (chat->last_tool_call_id) {
                        /* Tool calls API: send error as tool result */
                        llm_chat_add_tool_result(chat, chat->last_tool_call_id,
                                                  correction);
                    } else {
                        llm_chat_add(chat, "user", correction);
                    }

                    tool_result_free(&tr);
                    cJSON_Delete(action);
                    free(response);
                    free(action_str);
                    free(ut_json);
                    free(ut_ref);
                    free(ut_alias);
                    cJSON_Delete(ut_data);
                    continue;  /* retry — model gets another chance */
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double total_elapsed = (now.tv_sec - task_start.tv_sec) +
                               (now.tv_nsec - task_start.tv_nsec) / 1e9;

        /* Emit step complete */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_STEP_COMPLETE;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total_elapsed;
            ev.action = action_name;
            ev.description = desc ? desc : "";
            ev.stats = stats;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);
        }

        /* Emit tool output */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_TOOL_OUTPUT;
            ev.step = step + 1;
            ev.tool_meta = tr.meta;
            ev.store_ref = tr.store_ref;
            react_emit(on_event, userdata, &ev);
        }

        /* Build tool result string for context */
        char *meta_str = cJSON_PrintUnformatted(tr.meta);

        /* Cache signature + result for cycling detection on next step.
         * Only update on fresh executions, not cached hits. */
        if (!is_repeat) {
            free(last_sig);
            last_sig = sig;
            sig = NULL;  /* ownership transferred — don't free below */
            free(last_result_json);
            last_result_json = strdup(meta_str);
            /* Cache the store alias for journal hyperlinks on cycling hits */
            free(last_ref);
            last_ref = NULL;
            if (tr.store_ref && ctx->tools->aliases) {
                const char *alias = alias_map_reverse_lookup(
                    ctx->tools->aliases, tr.store_ref);
                if (alias) last_ref = strdup(alias);
            }
            repeat_count = 0;
        }
        free(sig);  /* no-op if ownership was transferred above */
        size_t result_len = strlen(meta_str) + 128;
        char *result_msg = malloc(result_len);
        { char _dur[32]; fmt_duration(total_elapsed, _dur, sizeof(_dur));
        snprintf(result_msg, result_len, "%s\n[step %d | %s]",
                 meta_str, step + 1, _dur); }

        /* Harness-1 §3.2: Assign importance to tool result messages */
        int tool_imp = react_tool_importance(action_name, tr.success);

        /* Harness-1 §3.3: Context-level deduplication — detect and skip
         * near-duplicate tool results to avoid wasting context budget.
         * Check both CRC32 hash AND content length to reduce
         * false positives from hash collisions on structured JSON data. */
        int is_dedup = 0;
        if (result_msg && result_msg[0] && tr.success) {
            size_t meta_len = strlen(meta_str);
            uint32_t content_hash = compress_crc32(meta_str, meta_len);
            uint32_t content_len = (uint32_t)meta_len;
            /* Check for duplicate: require BOTH hash AND length match */
            int dedup_step = -1;
            for (int di = 0; di < ctx->tools->dedup_count; di++) {
                if (ctx->tools->dedup_hashes[di] == content_hash &&
                    ctx->tools->dedup_lens[di] == content_len) {
                    dedup_step = ctx->tools->dedup_steps[di];
                    is_dedup = 1;
                    break;
                }
            }
            if (is_dedup) {
                /* Replace with a short reference */
                free(result_msg);
                result_len = 128;
                result_msg = malloc(result_len);
                if (dedup_step >= 0)
                    snprintf(result_msg, result_len,
                        "{\"note\":\"Same content as step %d — see earlier result\"}\n[step %d | dedup]",
                        dedup_step, step + 1);
                else
                    snprintf(result_msg, result_len,
                        "{\"note\":\"Duplicate content — see earlier result\"}\n[step %d | dedup]",
                        step + 1);
                tool_imp = LLM_MSG_IMPORTANCE_LOW;  /* deduped results are low priority */
            } else {
                /* Record hash + length for future dedup checks.
                 * Separate fill vs. full cases: fill (count<64) appends,
                 * full (count==64) shifts then writes to slot 63. */
                int idx;
                if (ctx->tools->dedup_count < 64) {
                    idx = ctx->tools->dedup_count++;
                } else {
                    /* Rolling buffer full — evict oldest entry */
                    memmove(ctx->tools->dedup_hashes, ctx->tools->dedup_hashes + 1,
                            63 * sizeof(uint32_t));
                    memmove(ctx->tools->dedup_lens, ctx->tools->dedup_lens + 1,
                            63 * sizeof(uint32_t));
                    memmove(ctx->tools->dedup_steps, ctx->tools->dedup_steps + 1,
                            63 * sizeof(int));
                    idx = 63;
                }
                ctx->tools->dedup_hashes[idx] = content_hash;
                ctx->tools->dedup_lens[idx] = content_len;
                ctx->tools->dedup_steps[idx] = step + 1;
            }
        }

        /* Add assistant + tool result to chat (with tool_calls threading if available) */
        if (chat->last_tool_call_id) {
            /* Tool calls API: add assistant with tool_calls, then tool result */
            llm_chat_add_assistant_tool_call(chat, response,
                                              chat->last_tool_calls_json);
            llm_chat_add_tool_result(chat, chat->last_tool_call_id, result_msg);
        } else {
            /* Fallback: legacy JSON-in-content format */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user", result_msg);
        }

        /* Harness-1 §3.2: Tag the newly added messages with importance.
         * The assistant message gets NORMAL, the tool result gets tool-specific importance. */
        if (chat->n_msgs >= 2) {
            chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_NORMAL;
            chat->msgs[chat->n_msgs - 1].importance = (llm_msg_importance_t)tool_imp;
            chat->msgs[chat->n_msgs - 1].msg_type = tr.success ? LLM_MSG_TOOL_RESULT : LLM_MSG_ERROR;

            /* CWL §3 [arXiv:2606.11213]: Set recoverability based on tool type.
             * Messages whose content is persisted elsewhere can be evicted more
             * aggressively because the agent can recover them via file_read.
             * Pichay [arXiv:2603.09023]: GC vs Paging distinction —
             *   file_read → RECOVER_FILE (addressable, can always re-read)
             *   file_write/edit → RECOVER_FILE (effects persisted)
             *   shell_exec/grep/glob → RECOVER_STORE (ephemeral, only in store)
             * LCM-Lite [arXiv:2605.04050]: Copy store ref alias to enable
             * breadcrumb generation during eviction. */
            llm_recoverability_t recover = LLM_RECOVER_NONE;
            if (action_name) {
                if (strcmp(action_name, "file_write") == 0 ||
                    strcmp(action_name, "file_edit") == 0)
                    recover = LLM_RECOVER_FILE;
                else if (strcmp(action_name, "file_read") == 0)
                    recover = LLM_RECOVER_FILE;  /* Pichay: addressable, re-requestable */
                else if (strcmp(action_name, "memory_store") == 0 ||
                         strcmp(action_name, "memory_pin") == 0)
                    recover = LLM_RECOVER_MEMORY;
                else if (strcmp(action_name, "notes") == 0 ||
                         strcmp(action_name, "plan") == 0)
                    recover = LLM_RECOVER_SCRATCHPAD;
                else if (tr.store_ref)
                    recover = LLM_RECOVER_STORE;
            }
            chat->msgs[chat->n_msgs - 1].recoverability = recover;

            /* Lifecycle [arXiv:2603.09023]: Record tool name and path for
             * stale-read detection and type-aware compression in eviction.
             * tool_name enables type-specific compress strategies.
             * tool_path enables tracking which file_reads are stale
             * (file was subsequently edited) or superseded (re-read). */
            if (action_name) {
                chat->msgs[chat->n_msgs - 1].tool_name = strdup(action_name);
                const char *tp = react_json_get_str(action, "path");
                if (tp)
                    chat->msgs[chat->n_msgs - 1].tool_path = strdup(tp);
            }

            /* Look up existing alias — tool_execute() already registered it.
             * Using reverse lookup avoids creating a duplicate alias. */
            if (tr.store_ref && ctx->tools->aliases) {
                const char *existing = alias_map_reverse_lookup(
                    ctx->tools->aliases, tr.store_ref);
                if (existing)
                    chat->msgs[chat->n_msgs - 1].store_alias = strdup(existing);
            }
        }

        /* Cycling escalation: after 3+ consecutive refusals, inject a forceful
         * message AFTER the tool result and importance tagging so the model sees
         * it as the last thing before generating its next response. */
        if (is_repeat && repeat_count >= 4) {
            const char *escalation =
                "[CYCLING BREAKER] You have been caught in an infinite loop "
                "repeating the same action. The harness has refused this action "
                "multiple times. You MUST change your approach NOW. "
                "Do NOT call the same tool with the same parameters. "
                "If you have enough information, call done(). "
                "If you need more information, try a DIFFERENT tool or DIFFERENT parameters.";
            llm_chat_add_typed(chat, "user", escalation, LLM_MSG_MEMORY_HINT);
            if (chat->n_msgs >= 1)
                chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_HIGH;
        }

        /* Slow-command nudge: when shell_exec takes >5s, inject a hint
         * teaching the capture-then-analyze pattern (file_read/grep_search
         * on the stored ref instead of re-running the command). */
        if (action_name && strcmp(action_name, "shell_exec") == 0 && tr.meta) {
            cJSON *ems = cJSON_GetObjectItem(tr.meta, "elapsed_ms");
            if (ems && ems->valuedouble > 5000.0) {
                cJSON *ref_j = cJSON_GetObjectItem(tr.meta, "ref");
                cJSON *chars_j = cJSON_GetObjectItem(tr.meta, "chars");
                cJSON *lines_j = cJSON_GetObjectItem(tr.meta, "lines");
                char nudge[256];
                snprintf(nudge, sizeof(nudge),
                    "[SLOW COMMAND] shell_exec took %ds. "
                    "Output stored at %s (%d chars, %d lines). "
                    "Do NOT re-run this command -- "
                    "analyze the stored output instead.",
                    (int)(ems->valuedouble / 1000.0),
                    ref_j ? ref_j->valuestring : "?",
                    chars_j ? (int)chars_j->valuedouble : 0,
                    lines_j ? (int)lines_j->valuedouble : 0);
                llm_chat_add_typed(chat, "user", nudge,
                                   LLM_MSG_EVICTION_SUMMARY);
            }
        }

        /* Harness-1 §4.2: Track tool usage for diversity nudging */
        {
            int tidx = react_tool_index(action_name);
            if (tidx >= 0 && tidx < 32)
                ctx->tools->tool_use_counts[tidx]++;
            ctx->tools->n_tool_uses++;
        }

        /* Track incremental note-taking: detect deferred synthesis anti-pattern.
         * When the model reads many files without saving findings to notes(),
         * compaction evicts the raw content and the model confabulates. */
        if (action_name) {
            if (strcmp(action_name, "file_read") == 0 && tr.success)
                ctx->tools->file_reads_since_notes++;
            else if (strcmp(action_name, "notes") == 0 && tr.success) {
                ctx->tools->file_reads_since_notes = 0;
                ctx->tools->last_notes_step = step;
                ctx->tools->pre_compact_warned = 0; /* allow re-warning after save */
            }
        }

        /* Multi-tool detection: inject corrective hint when the model emitted
         * multiple tool calls (concatenated JSON or native tool_calls array > 1).
         * Common with gemma4/qwen3.6 models. Only the first tool call was executed;
         * tell the model to issue one call at a time so no work is silently lost.
         * The hint is injected as a LOW-importance user message to avoid wasting
         * context on models that learn quickly. */
        {
            /* Prefer native API count (more reliable) over content-parse count */
            int detected = chat->multi_tool_count > 1 ? chat->multi_tool_count
                         : multi_tool_count > 1       ? multi_tool_count
                         : 0;
            if (detected > 1) {
                char hint[256];
                snprintf(hint, sizeof(hint),
                    "You attempted %d tool calls at once. Only the first was "
                    "executed. Issue exactly ONE tool call per response.",
                    detected);
                llm_chat_add(chat, "user", hint);
                if (chat->n_msgs > 0)
                    chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_LOW;

                /* Log for postmortem analysis */
                nash_log("[react] multi-tool corrective hint injected (%d calls)",
                         detected);

                /* Reset for next step */
                chat->multi_tool_count = 0;
            }
        }

        /* Harness-1 §3.4: Auto-seed scratchpad from first successful tool result.
         * Ensures scratchpad is never empty when eviction kicks in.
         * Like Harness-1's auto-seeding of the curated set at "fair" importance. */
        if (step == 0 && tr.success && !is_dedup && meta_str) {
            int sp_exists = scratchpad_find(&ctx->tools->scratch, "auto_seed");
            if (sp_exists < 0) {
                /* Build a metadata-only summary — strip bulk "content" field
                 * which dominates output and is not useful as a seed. */
                cJSON *seed_meta = cJSON_Duplicate(tr.meta, 1);
                if (seed_meta) {
                    cJSON *c = cJSON_DetachItemFromObject(seed_meta, "content");
                    if (c) cJSON_Delete(c);
                    char *seed_json = cJSON_PrintUnformatted(seed_meta);
                    cJSON_Delete(seed_meta);
                    if (seed_json) {
                        int sj_len = (int)strlen(seed_json);
                        char *seed = malloc((size_t)(sj_len + 64));
                        if (seed) {
                            snprintf(seed, (size_t)(sj_len + 64),
                                "First result (%s): %s",
                                action_name, seed_json);
                            scratchpad_write(&ctx->tools->scratch,
                                             "auto_seed", seed, 3);
                            free(seed);
                        }
                        free(seed_json);
                    }
                }
            }
        }

        /* ── Change 8: Working Memory Auto-Promotion ─────────────────
         * CogniFold's "always-on" principle: memory operates without explicit
         * agent action. After successful tool execution with substantial output,
         * auto-append a snippet to the scratchpad "auto_findings" section.
         * This replaces the diversity nudge hack — the harness offloads
         * bookkeeping from the LLM (Harness-1 §3.2). */
        {
            int do_promote = ctx->tools->cfg
                ? ctx->tools->cfg->auto_promote : 1;
            int promote_min = ctx->tools->cfg
                ? ctx->tools->cfg->auto_promote_min_length : 500;
            int promote_max = ctx->tools->cfg
                ? ctx->tools->cfg->auto_promote_max_chars : 2000;
            if (do_promote && tr.success && meta_str
                && (int)strlen(meta_str) > promote_min) {
                /* Check current auto_findings size before appending */
                int af_idx = scratchpad_find(&ctx->tools->scratch, "auto_findings");
                int cur_len = 0;
                if (af_idx >= 0 && ctx->tools->scratch.sections[af_idx].content)
                    cur_len = (int)strlen(ctx->tools->scratch.sections[af_idx].content);
                if (cur_len < promote_max) {
                    /* Build metadata-only snippet — strip bulk "content" field
                     * to keep auto_findings concise and useful. No leading \n:
                     * scratchpad_append() already inserts \n between entries. */
                    cJSON *promo_meta = cJSON_Duplicate(tr.meta, 1);
                    if (promo_meta) {
                        cJSON *c = cJSON_DetachItemFromObject(promo_meta, "content");
                        if (c) cJSON_Delete(c);
                        char *promo_json = cJSON_PrintUnformatted(promo_meta);
                        cJSON_Delete(promo_meta);
                        if (promo_json) {
                            char snippet[320];
                            snprintf(snippet, sizeof(snippet),
                                     "[step %d] %s: %.*s",
                                     step + 1, action_name,
                                     (int)utf8_clamp(promo_json, 280),
                                     promo_json);
                            scratchpad_append(&ctx->tools->scratch,
                                              "auto_findings", snippet, 4);
                            free(promo_json);
                        }
                    }
                }
            }
        }

        /* P3: Error-triggered reactive retrieval — when a tool fails, query
         * memory with the error message to surface relevant lessons/skills.
         * Based on: arXiv 2605.30621 "Harness Updating Is Not Harness Benefit"
         * — activation failure occurs when stored knowledge exists but isn't
         * retrieved. Error scenarios are the highest-value retrieval opportunity
         * because the agent may have a lesson about exactly this error.
         *
         * All thresholds are configurable via [limits] in config.toml:
         *   error_recall_min_length    — min error text chars to trigger (default 10)
         *   error_recall_candidates    — candidates to retrieve (default 3)
         *   error_recall_max_inject    — max entries to inject (default 1)
         *   error_recall_min_relevance — relevance floor for injection (default 0.25) */
        if (!tr.success && (ctx->tools->memory || ctx->tools->ws) && ctx->flags.inject_memory) {
            cJSON *err_j = cJSON_GetObjectItem(tr.meta, "error");
            const char *err_text = err_j ? err_j->valuestring : NULL;
            int err_min_len = ctx->tools->cfg
                ? ctx->tools->cfg->error_recall_min_length : 10;
            if (err_text && (int)strlen(err_text) > err_min_len) {
                /* Build recall query from error text + action name */
                char err_query[512];
                snprintf(err_query, sizeof(err_query), "error: %.400s %s",
                         err_text, action_name);
                int err_candidates = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_candidates : 3;
                memory_results_t err_mem = ctx->tools->ws
                    ? workspace_recall(ctx->tools->ws, err_query, err_candidates)
                    : memory_query(ctx->tools->memory, err_query, err_candidates);
                int err_max_inject = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_max_inject : 1;
                double err_min_rel = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_min_relevance : 0.25;
                /* Inject top relevant matches not already recalled */
                int injected = 0;
                for (int j = 0; j < err_mem.count && injected < err_max_inject; j++) {
                    int dup = 0;
                    for (int k = 0; k < ctx->tools->n_recalled_keys; k++) {
                        if (strcmp(ctx->tools->recalled_keys[k],
                                   err_mem.entries[j].key) == 0) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup && err_mem.entries[j].relevance > err_min_rel) {
                        char hint[2048];
                        snprintf(hint, sizeof(hint),
                            "[MEMORY HINT — relevant to this error]\n"
                            "--- %s ---\n%s",
                            err_mem.entries[j].key,
                            err_mem.entries[j].value);
                        llm_chat_add_typed(chat, "user", hint, LLM_MSG_MEMORY_HINT);
                        tool_track_recalled_key(ctx->tools,
                                                 err_mem.entries[j].key);
                        injected++;
                    }
                }
                memory_results_free(&err_mem);
            }
        }

        /* Error budget: when total errors exceed threshold, force wrap-up.
         * Base threshold of 15, scaled up by step count (1 extra per 5 steps).
         * Prevents endless error-retry-error spirals that waste compute. */
        {
            int error_threshold = 15 + (step / 5);
            if (total_errors >= error_threshold) {
                char budget_msg[256];
                snprintf(budget_msg, sizeof(budget_msg),
                    "ERROR BUDGET EXCEEDED: %d errors in %d steps (threshold: %d). "
                    "You must wrap up NOW. Save your findings with notes() "
                    "and call done() with whatever partial results you have.",
                    total_errors, step + 1, error_threshold);
                llm_chat_add(chat, "user", budget_msg);
                /* Error budget warnings are critical guardrails — NORMAL */
                if (chat->n_msgs > 0)
                    chat->msgs[chat->n_msgs - 1].importance = LLM_MSG_IMPORTANCE_NORMAL;

                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_WARNING;
                ev.step = step + 1;
                ev.message = "Error budget exceeded — forcing wrap-up";
                react_emit(on_event, userdata, &ev);

                /* Reset to avoid spamming the message every step */
                total_errors = 0;
            }
        }

        /* Self-calibrate chars_per_token from actual API response.
         * When the API reports prompt_tokens, compute the actual ratio from
         * total_chars / prompt_tokens and use exponential moving average to
         * smooth out noise. This corrects for content-type-dependent variation
         * (JSON-heavy prompts tokenize differently than prose).
         * Use alpha=0.8 on the first calibration (when still
         * using the default/config value) for fast convergence, then switch
         * to alpha=0.3 for subsequent updates. The first measurement from
         * the actual API is far more informative than any default. */
        if (stats.prompt_tokens > 100) {  /* need enough tokens for reliable ratio */
            /* Use shared inline helper */
            long actual_chars = react_calc_total_chars(chat);
            float actual_cpt = (float)actual_chars / (float)stats.prompt_tokens;
            /* Clamp to reasonable range [1.5, 8.0] to avoid outliers */
            if (actual_cpt > 1.5f && actual_cpt < 8.0f) {
                float old_cpt = react_get_chars_per_token(ctx);
                /* First calibration uses high alpha for fast convergence.
                 * Detect "uncalibrated" state by checking if rt.chars_per_token
                 * hasn't been set from actual measurement yet (still 0 or matches
                 * the config default exactly). */
                float alpha;
                if (ctx->rt.chars_per_token <= 0 ||
                    (ctx->provider && ctx->provider->cfg.chars_per_token > 0 &&
                     ctx->rt.chars_per_token == ctx->provider->cfg.chars_per_token)) {
                    alpha = 0.8f;  /* first measurement: trust it heavily */
                } else {
                    alpha = 0.3f;  /* subsequent: smooth EMA */
                }
                float calibrated = old_cpt * (1.0f - alpha) + actual_cpt * alpha;
                ctx->rt.chars_per_token = calibrated;
            }
        }

        /* Pre-compaction warning: warn the model BEFORE eviction fires so it
         * can save unsaved analysis while file contents are still in context.
         * The hint fires when context usage enters the "warning zone" (within
         * warn_gap of trigger_pct, both derived from config) AND the model
         * has 3+ unsaved file_reads.
         * When eviction actually fires, react_maybe_evict removes MEMORY_HINT
         * messages as part of cleanup (step 1), so the warning is self-cleaning.
         * Reset on notes() to allow re-warning after the next batch of reads. */
        if (ctx->flags.enable_compaction && ctx->provider &&
            ctx->provider->cfg.context_size > 0 &&
            !ctx->tools->pre_compact_warned &&
            ctx->tools->file_reads_since_notes >= 3)
        {
            eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
            long budget = react_context_budget(ctx);
            int usage_pct = react_chat_usage_pct(chat, budget);
            int warn_zone = pol.trigger_pct - pol.warn_gap;
            if (usage_pct >= warn_zone && usage_pct <= pol.trigger_pct) {
                llm_chat_add_typed(chat, "user", EVICT_PRE_COMPACT_WARN,
                    LLM_MSG_MEMORY_HINT);
                ctx->tools->pre_compact_warned = 1;
            }
        }

        /* Harness-1 §3.5: Multi-pass progressive context eviction */
        int pre_evict_msgs = chat->n_msgs;
        react_maybe_evict(ctx, chat, step, user_query, on_event, userdata);

        /* Reset cycling detection state after compaction evicts messages.
         * Without this, the model cannot legitimately re-read content that was
         * evicted from context — the stale last_sig matches the new action and
         * cycling_cached fires as a false positive.  The cached result IS
         * returned (stage 1), but the "note: cached" annotation confuses the
         * model, and a third attempt triggers cycling_refused → data loss. */
        if (chat->n_msgs < pre_evict_msgs) {
            free(last_sig);
            last_sig = NULL;
            free(last_result_json);
            last_result_json = NULL;
            free(last_ref);
            last_ref = NULL;
            repeat_count = 0;
            /* Reset pre-compaction warning so it can fire again for the
             * next batch of file reads before the next compaction. */
            ctx->tools->pre_compact_warned = 0;
        }

        /* Incremental notes nudge: detect deferred synthesis anti-pattern.
         * The model tends to read many files and defer note-taking until the
         * end, by which time compaction has evicted file contents and the
         * model confabulates findings.  Two triggers:
         *   (a) 5+ file_reads since last notes() — volume-based
         *   (b) 10+ steps since last notes() with 3+ file_reads — time-based
         * Either trigger fires the nudge, then resets the counter to allow
         * re-nudging.  The nudge text emphasizes saving with exact file:line
         * references to maximize utility of what's preserved. */
        {
            int fr = ctx->tools->file_reads_since_notes;
            int steps_since = (ctx->tools->last_notes_step >= 0)
                ? step - ctx->tools->last_notes_step : step;
            int nudge = 0;
            if (fr >= 5)
                nudge = 1;  /* volume trigger: 5+ files without notes */
            else if (steps_since >= 10 && fr >= 3)
                nudge = 1;  /* time trigger: 10+ steps with 3+ files */
            if (nudge) {
                llm_chat_add_typed(chat, "user",
                    "[HINT] You have read multiple files without saving findings "
                    "to notes(). Save your analysis NOW with exact file:line "
                    "references before context compaction evicts the file contents. "
                    "Use notes(op=\"append\", section=\"findings\", content=\"...\").",
                    LLM_MSG_MEMORY_HINT);
                /* Don't reset the counter here — it creates a
                 * gap where the model can accumulate 5 more file_reads without
                 * re-nudging even if it ignored this nudge. The counter is
                 * properly reset at line 1703 when notes() is actually called. */
            }
        }

        /* Cleanup */
        free(meta_str);
        free(result_msg);
        tool_result_free(&tr);

        /* Save checkpoint after each tool execution (atomic write) */
        if (!final_result) react_checkpoint_save(ctx, step + 1, user_query,
                        chat->last_tool_call_id);

        /* Check for pause request (Space pressed in TUI — toggle pause/resume).
         * Instead of breaking out (which destroys chat context), wait on a
         * condvar for the user to provide a redirect query or resume.
         * Checkpoint was already saved above. */
        if (!final_result && ctx->pause_requested) {
            react_wait_for_redirect(ctx, chat, step + 1, on_event, userdata);
        }

        cJSON_Delete(action);
        free(response);
    }

    /* Stop device stream / VNC session and save capture to session dir.
     * Placed after the for-loop so it fires on ALL exit paths: explicit done,
     * max-steps exhaustion, and error bail-outs.
     * No-op if no device session was started in this react loop. */
    tool_device_cleanup(ctx->tools->session_dir);

    llm_chat_free(chat);

    /* Emit REACT_EVENT_DONE on fatal error so the TUI regenerates
     * reactRX.md showing the error instead of leaving "prompt processing..."
     * frozen on screen.  Without this, the md is never regenerated because
     * the error event fires while STATUS_RUNNING, then the loop moves on. */
    if (!final_result && on_event) {
        struct timespec task_end;
        clock_gettime(CLOCK_MONOTONIC, &task_end);
        double total = (task_end.tv_sec - task_start.tv_sec) +
                       (task_end.tv_nsec - task_start.tv_nsec) / 1e9;
        react_event_t ev = {0};
        ev.react_loop = ctx->tools->react_loop;
        ev.type = REACT_EVENT_DONE;
        ev.total_elapsed = total;
        ev.result = NULL;
        react_emit(on_event, userdata, &ev);
    }

    /* Remove checkpoint — task completed (successfully or not).
     * Only needed for non-done exits (max steps, errors);
     * the done handler already removes it on success. */
    if (!final_result)
        react_checkpoint_remove(ctx);

    /* Post-loop: validation scoring, reflection, promotion, pruning */
    {
        int task_succeeded = (final_result != NULL);
        react_post_loop(ctx, user_query, final_result, task_succeeded,
                        on_event, userdata);
    }

    /* Flush all deferred git commits as a single batch. */
    if (ctx->tools->ws)
        workspace_git_flush(ctx->tools->ws, "memory: batch update (react loop)");
    else if (ctx->tools->memory)
        memory_git_flush(ctx->tools->memory, "memory: batch update (react loop)");

    /* Don't free last_query/last_result here — the caller (main.c) manages them.
     * They are set after each react_run() call and used to inject previous context. */

    /* Reset recalled keys for next query (each task is independent) */
    for (int i = 0; i < ctx->tools->n_recalled_keys; i++)
        free(ctx->tools->recalled_keys[i]);
    free(ctx->tools->recalled_keys);
    ctx->tools->recalled_keys = NULL;
    ctx->tools->n_recalled_keys = 0;
    ctx->tools->recalled_keys_cap = 0;

    free(last_sig);
    free(last_result_json);
    free(last_ref);
    return final_result;
}

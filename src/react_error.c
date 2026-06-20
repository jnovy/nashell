/* react_error.c — Error recovery for NULL LLM responses.
 * Extracted from react.c to reduce file size.
 * Includes emergency eviction and the 5-tier retry strategy.
 *
 * Bug/Design fixes applied:
 *   BUG 4:    Floor calculation now uses REACT_EVICT_FLOOR_PCT (not hardcoded /5)
 *   BUG 8:    Considers recoverability — evicts RECOVER_STORE/FILE/MEMORY
 *             messages before RECOVER_NONE to preserve irreplaceable content
 *   DESIGN 4: Compaction floor prevents over-eviction
 *   DESIGN 9: Protection levels aligned with progressive eviction (>= HIGH)
 *   UNIFY:    Uses shared react_find_tool_partner() for pair-safety */

#include "react_internal.h"

/* ── Emergency Eviction ────────────────────────────────── */

/* Emergency eviction — removes enough evictable messages to reach ~80% of
 * context budget, prioritizing recoverable content over irreplaceable.
 * Uses the same floor calculation as progressive eviction (REACT_EVICT_FLOOR_PCT).
 * Returns the number of messages evicted (0 if not enough to evict). */
int react_emergency_evict(llm_chat_t *chat, long context_budget) {
    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;
    if (evict_end <= evict_start + 2) return 0;

    long total_chars = react_calc_total_chars(chat);

    long target_chars;
    if (context_budget > 0)
        target_chars = context_budget * REACT_EMERGENCY_TARGET_PCT / 100;
    else
        target_chars = total_chars * REACT_EMERGENCY_TARGET_PCT / 100;
    long need_to_remove = total_chars - target_chars;
    if (need_to_remove <= 0) return 0;

    /* FIX FLAW 4: Use shared floor calculation helper for consistency
     * with progressive eviction. */
    long floor_chars = react_calc_floor_chars(chat, evict_start, context_budget);

    /* head_chars still needed for remaining_nonhead calculations below */
    long head_chars = 0;
    for (int i = 0; i < evict_start && i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            head_chars += (long)strlen(chat->msgs[i].content);

    long removed_chars = 0;
    int removed = 0;

    /* Two-pass eviction — recoverable content first.
     * Pass 0: Only evict messages with recoverability > RECOVER_NONE
     * Pass 1: Evict remaining (non-recoverable) messages if still needed. */
    for (int pass = 0; pass < 2 && removed_chars < need_to_remove; pass++) {
        int i = evict_start;
        while (i < chat->n_msgs - keep_tail && removed_chars < need_to_remove) {
            if (chat->msgs[i].importance >= LLM_MSG_IMPORTANCE_HIGH) {
                i++;
                continue;
            }
            if (pass == 0 && chat->msgs[i].recoverability == LLM_RECOVER_NONE) {
                i++;
                continue;
            }

            /* Compaction floor check */
            long remaining_nonhead = total_chars - head_chars - removed_chars;
            long msg_chars = chat->msgs[i].content
                ? (long)strlen(chat->msgs[i].content) : 0;
            if (remaining_nonhead - msg_chars < floor_chars)
                break;

            /* Pair-safe using shared helper */
            int partner = react_find_tool_partner(chat, i, evict_start,
                                                   chat->n_msgs - keep_tail);

            if (partner >= 0) {
                if (pass == 0 && chat->msgs[partner].recoverability == LLM_RECOVER_NONE) {
                    i++;
                    continue;
                }
                long pair_chars = chat->msgs[partner].content
                    ? (long)strlen(chat->msgs[partner].content) : 0;
                if (remaining_nonhead - msg_chars - pair_chars < floor_chars)
                    break;

                /* Remove higher index first to preserve lower index */
                int hi = partner > i ? partner : i;
                int lo = partner > i ? i : partner;
                long hi_chars = chat->msgs[hi].content
                    ? (long)strlen(chat->msgs[hi].content) : 0;
                long lo_chars = chat->msgs[lo].content
                    ? (long)strlen(chat->msgs[lo].content) : 0;
                removed_chars += hi_chars;
                llm_chat_remove_range(chat, hi, hi + 1);
                removed++;
                removed_chars += lo_chars;
                llm_chat_remove_range(chat, lo, lo + 1);
                removed++;
                /* Don't increment i — next msg is now at position lo */
                continue;
            }

            /* Standalone message */
            removed_chars += msg_chars;
            llm_chat_remove_range(chat, i, i + 1);
            removed++;
        }
    }

    /* Fallback — evict at least one message if possible */
    if (removed == 0) {
        long cur_total = react_calc_total_chars(chat);
        long cur_nonhead = cur_total - head_chars;
        if (cur_nonhead > floor_chars) {
            int best = -1;
            for (int j = evict_start; j < chat->n_msgs - keep_tail; j++) {
                if (chat->msgs[j].importance < LLM_MSG_IMPORTANCE_HIGH &&
                    chat->msgs[j].recoverability > LLM_RECOVER_NONE) {
                    best = j; break;
                }
            }
            if (best < 0) {
                for (int j = evict_start; j < chat->n_msgs - keep_tail; j++) {
                    if (chat->msgs[j].importance < LLM_MSG_IMPORTANCE_HIGH) {
                        best = j; break;
                    }
                }
            }
            if (best >= 0) {
                int partner = react_find_tool_partner(chat, best, evict_start,
                                                      chat->n_msgs - keep_tail);
                if (partner >= 0) {
                    int hi = partner > best ? partner : best;
                    int lo = partner > best ? best : partner;
                    llm_chat_remove_range(chat, hi, hi + 1);
                    llm_chat_remove_range(chat, lo, lo + 1);
                    removed = 2;
                } else {
                    llm_chat_remove_range(chat, best, best + 1);
                    removed = 1;
                }
            }
        }
    }
    return removed;
}

/* ── NULL Response Handling ────────────────────────────── */

/* Handle NULL response from LLM (HTTP 400/500/auth errors).
 * Returns: 0 = continue (retry), 1 = break (give up).
 * Modifies chat in-place for recovery. */
int react_handle_null_response(react_ctx_t *ctx, llm_chat_t *chat,
                               int *consecutive_null, int *total_400,
                               llm_stats_t *stats, int step,
                               react_event_fn on_event, void *userdata) {

    /* Write server error to journal */
    {
        cJSON *err_params = cJSON_CreateObject();
        {
            const char *srv_err = NULL;
            if (ctx->provider && ctx->provider->last_error)
                srv_err = ctx->provider->last_error;

            if (*consecutive_null >= 3) {
                if (srv_err) {
                    char emsg[512];
                    snprintf(emsg, sizeof(emsg),
                        "LLM server error after recovery attempt — %s", srv_err);
                    cJSON_AddStringToObject(err_params, "error", emsg);
                } else {
                    cJSON_AddStringToObject(err_params, "error",
                        "LLM server error after recovery attempt — giving up");
                }
            } else {
                if (srv_err) {
                    char emsg[512];
                    snprintf(emsg, sizeof(emsg),
                        "LLM server returned NULL response (%s)", srv_err);
                    cJSON_AddStringToObject(err_params, "error", emsg);
                } else {
                    cJSON_AddStringToObject(err_params, "error",
                        "LLM server returned NULL response (unknown error)");
                }
            }
        }
        cJSON_AddNumberToObject(err_params, "attempt", *consecutive_null);
        cJSON_AddNumberToObject(err_params, "completion_tokens", stats->completion_tokens);

        long total_chars = react_calc_total_chars(chat);
        cJSON_AddNumberToObject(err_params, "context_chars", (double)total_chars);
        cJSON_AddNumberToObject(err_params, "context_msgs", chat->n_msgs);

        const char *err_msg = ctx->provider ? ctx->provider->last_error : NULL;
        const char *err_req = ctx->provider ? ctx->provider->last_error_request : NULL;
        const char *err_resp = ctx->provider ? ctx->provider->last_error_response : NULL;

        if (err_msg)
            cJSON_AddStringToObject(err_params, "server_message", err_msg);

        if (err_req && ctx->tools->store) {
            char *req_ref = store_save(ctx->tools->store, err_req);
            if (req_ref) {
                cJSON_AddStringToObject(err_params, "request_ref", req_ref);
                free(req_ref);
            }
        }
        if (err_resp && ctx->tools->store) {
            char *resp_ref = store_save(ctx->tools->store, err_resp);
            if (resp_ref) {
                cJSON_AddStringToObject(err_params, "response_ref", resp_ref);
                free(resp_ref);
            }
        }

        char *se_alias = NULL;
        {
            char *content = cJSON_Print(err_params);
            if (content && ctx->tools->store && ctx->tools->aliases) {
                char *hash = store_save(ctx->tools->store, content);
                if (hash) {
                    se_alias = tool_register_alias(ctx->tools, hash);
                    free(hash);
                }
            }
            free(content);
        }

        journal_append(ctx->tools->journal,
            ctx->tools->react_loop, step + 1, "server_error",
            err_params, se_alias, se_alias ? strlen(se_alias) : 0, 0,
            "LLM server error", NULL);
        free(se_alias);
        cJSON_Delete(err_params);
    }

    react_event_t ev = {0};
    ev.react_loop = ctx->tools->react_loop;
    ev.type = REACT_EVENT_ERROR;
    ev.step = step + 1;

    /* Authentication error (HTTP 401/403) — can't be fixed by eviction */
    {
        const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
        if (perr && (strstr(perr, "HTTP 401") || strstr(perr, "HTTP 403"))) {
            ev.message = "Authentication failed — token expired or invalid, "
                         "please re-authenticate (e.g. gcloud auth login)";
            react_emit(on_event, userdata, &ev);
            return 1;
        }
    }

    /* HTTP 400 (client error = bad request) — aggressive context eviction */
    {
        const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
        if (perr && strstr(perr, "HTTP 400")) {
            (*total_400)++;
            if (*total_400 >= 6) {
                ev.message = "HTTP 400 — context still too large after "
                             "repeated eviction, giving up";
                react_emit(on_event, userdata, &ev);
                return 1;
            }
            long cb = react_context_budget(ctx);
            int n_evict = react_emergency_evict(chat, cb);
            if (n_evict > 0) {
                react_reinject_scratchpad(ctx, chat, react_compute_keep_head(chat));
                char emsg[128];
                snprintf(emsg, sizeof(emsg),
                    "HTTP 400 — evicted %d messages to reduce context "
                    "(attempt %d/6)", n_evict, *total_400);
                ev.message = emsg;
                react_emit(on_event, userdata, &ev);
            } else {
                ev.message = "HTTP 400 — no evictable messages remain, giving up";
                react_emit(on_event, userdata, &ev);
                return 1;
            }
            *consecutive_null = 0;
            return 0;
        }
    }

    /* Max-token exhaustion — deterministic, not transient */
    if (stats->completion_tokens > 0 && ctx->provider &&
        stats->completion_tokens >= ctx->provider->cfg.max_tokens) {
        char mtmsg[256];
        snprintf(mtmsg, sizeof(mtmsg),
            "Max-token exhaustion (%d/%d tokens) — "
            "evicting context to recover",
            stats->completion_tokens, ctx->provider->cfg.max_tokens);
        ev.message = mtmsg;
        react_emit(on_event, userdata, &ev);

        {
            long cb = react_context_budget(ctx);
            int n_evict = react_emergency_evict(chat, cb);
            if (n_evict > 0)
                react_reinject_scratchpad(ctx, chat, react_compute_keep_head(chat));
        }
        (*consecutive_null)++;
        return 0;
    }

    /* 5-tier retry strategy for HTTP 500 / NULL responses */
    if (*consecutive_null >= 6) {
        ev.message = "LLM server error — all recovery tiers exhausted, giving up";
        react_emit(on_event, userdata, &ev);
        return 1;
    }

    if (*consecutive_null <= 2) {
        /* Tier 0: Plain retry with backoff */
        if (ctx->pause_requested) return 1;
        int backoff_ms = *consecutive_null * 2000;
        char rmsg[128];
        snprintf(rmsg, sizeof(rmsg),
            "LLM server error — plain retry %d/2 (backoff %dms)",
            *consecutive_null, backoff_ms);
        ev.message = rmsg;
        react_emit(on_event, userdata, &ev);
        for (int ms = 0; ms < backoff_ms && !ctx->pause_requested; ms += 100)
            usleep(100000);
    } else if (*consecutive_null == 3) {
        /* Tier 1: Remove the last assistant+tool_result pair */
        ev.message = "LLM server error — removing last exchange and retrying (tier 1)";
        react_emit(on_event, userdata, &ev);
        if (chat->n_msgs >= 2) {
            int remove_from = chat->n_msgs - 2;
            if (remove_from < 3) remove_from = 3;
            if (remove_from < chat->n_msgs)
                llm_chat_remove_range(chat, remove_from, chat->n_msgs);
        }
    } else if (*consecutive_null == 4) {
        /* Tier 2: Reformulate scratchpad (strip code blocks) */
        int sp_idx = llm_chat_find_by_type(chat, LLM_MSG_SCRATCHPAD);
        if (sp_idx >= 0) {
            ev.message = "LLM server error — stripping code blocks from scratchpad (tier 2)";
            react_emit(on_event, userdata, &ev);
            const char *src = chat->msgs[sp_idx].content;
            {
                const char *nl = strchr(src, '\n');
                if (nl) src = nl + 1;
            }
            size_t src_len = strlen(src);
            char *cleaned = malloc(src_len + 1);
            if (cleaned) {
                size_t di = 0;
                for (size_t si = 0; si < src_len; ) {
                    if (si + 3 <= src_len && strncmp(src + si, "```", 3) == 0) {
                        const char *end = strstr(src + si + 3, "```");
                        if (end) {
                            si = (size_t)(end - src) + 3;
                            if (si < src_len && src[si] == '\n') si++;
                        } else {
                            si += 3;
                        }
                        continue;
                    }
                    if (src[si] == '`') {
                        const char *end = strchr(src + si + 1, '`');
                        if (end && end - (src + si) < REACT_THOUGHT_TRUNC_LEN) {
                            si++;
                            while (src + si < end)
                                cleaned[di++] = src[si++];
                            si++;
                            continue;
                        }
                    }
                    cleaned[di++] = src[si++];
                }
                cleaned[di] = '\0';
                size_t clen = strlen(cleaned);
                char *new_sp = malloc(clen + 32);
                if (new_sp) {
                    snprintf(new_sp, clen + 32, "[SCRATCHPAD]\n%s", cleaned);
                    free(chat->msgs[sp_idx].content);
                    chat->msgs[sp_idx].content = new_sp;
                }
                free(cleaned);
            }
        } else {
            ev.message = "LLM server error — no scratchpad, skipping tier 2";
            react_emit(on_event, userdata, &ev);
        }
    } else if (*consecutive_null == 5) {
        /* Tier 3: Strip scratchpad entirely (nuclear option) */
        ev.message = "LLM server error — stripping scratchpad entirely (tier 3)";
        react_emit(on_event, userdata, &ev);
        llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
        react_reinject_scratchpad(ctx, chat, react_compute_keep_head(chat));
    }
    return 0;
}

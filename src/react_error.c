/* react_error.c — Error recovery for NULL LLM responses.
 * Extracted from react.c to reduce file size.
 * Includes emergency eviction and the 5-tier retry strategy. */

#include "react_internal.h"

/* ── Emergency Eviction ────────────────────────────────── */

/* Emergency eviction — proportionally removes enough of the oldest
 * evictable messages to reach ~80% of context budget.
 * FIX #3: Takes context_budget parameter. Previously targeted 80% of
 * CURRENT usage, which when context was 150% of budget would produce
 * 120% — still over budget. Now targets 80% of the actual budget.
 * FIX #7: Pair-safe — removes tool_call/tool_result pairs together
 * to avoid orphaning messages that violate the LLM API contract.
 * Returns the number of messages evicted (0 if not enough to evict). */
int react_emergency_evict(llm_chat_t *chat, int context_budget) {
    int keep_head = REACT_EVICT_KEEP_HEAD;
    int keep_tail = REACT_EVICT_KEEP_TAIL;
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;
    if (evict_end <= evict_start + 2) return 0;

    /* Calculate total context chars */
    int total_chars = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            total_chars += (int)strlen(chat->msgs[i].content);

    /* FIX #3: Target 80% of context BUDGET, not 80% of current usage.
     * If context_budget is 0 (unknown), fall back to 80% of current. */
    int target_chars;
    if (context_budget > 0)
        target_chars = context_budget * 80 / 100;
    else
        target_chars = total_chars * 80 / 100;
    int need_to_remove = total_chars - target_chars;
    if (need_to_remove <= 0) return 0;

    /* Remove oldest evictable messages until we've freed enough.
     * Skip CRITICAL and HIGH importance messages.
     * FIX #7: Handle tool_call/tool_result pairs atomically. */
    int removed_chars = 0;
    int removed = 0;
    int i = evict_start;
    while (i < chat->n_msgs - keep_tail && removed_chars < need_to_remove) {
        /* Never evict CRITICAL or HIGH messages */
        if (chat->msgs[i].importance >= LLM_MSG_IMPORTANCE_HIGH) {
            i++;
            continue;
        }

        /* FIX #7: If this is an assistant with tool_calls, check if the
         * next message is its tool_result — remove both together. */
        if (chat->msgs[i].tool_calls_json &&
            i + 1 < chat->n_msgs - keep_tail &&
            chat->msgs[i + 1].tool_call_id &&
            chat->msgs[i + 1].importance < LLM_MSG_IMPORTANCE_HIGH) {
            if (chat->msgs[i].content)
                removed_chars += (int)strlen(chat->msgs[i].content);
            if (chat->msgs[i + 1].content)
                removed_chars += (int)strlen(chat->msgs[i + 1].content);
            llm_chat_remove_range(chat, i, i + 2);
            removed += 2;
            continue;  /* don't increment — removal shifts array */
        }

        /* FIX #7: If this is a tool_result, also remove the preceding
         * assistant (with tool_calls) if it's in range and evictable. */
        if (chat->msgs[i].tool_call_id &&
            i - 1 >= evict_start &&
            chat->msgs[i - 1].tool_calls_json &&
            chat->msgs[i - 1].importance < LLM_MSG_IMPORTANCE_HIGH) {
            if (chat->msgs[i - 1].content)
                removed_chars += (int)strlen(chat->msgs[i - 1].content);
            if (chat->msgs[i].content)
                removed_chars += (int)strlen(chat->msgs[i].content);
            llm_chat_remove_range(chat, i - 1, i + 1);
            removed += 2;
            continue;
        }

        /* Standalone message (no pair) or pair partner is protected */
        if (chat->msgs[i].content)
            removed_chars += (int)strlen(chat->msgs[i].content);
        llm_chat_remove_range(chat, i, i + 1);
        removed++;
        /* Don't increment i — removal shifts array down */
    }
    /* Ensure we evict at least something (skip CRITICAL+HIGH even here) */
    if (removed == 0) {
        for (int j = evict_start; j < chat->n_msgs - keep_tail; j++) {
            if (chat->msgs[j].importance < LLM_MSG_IMPORTANCE_HIGH) {
                /* Pair-safe: if assistant with tool_calls, take both */
                if (chat->msgs[j].tool_calls_json &&
                    j + 1 < chat->n_msgs - keep_tail &&
                    chat->msgs[j + 1].tool_call_id) {
                    llm_chat_remove_range(chat, j, j + 2);
                    removed = 2;
                } else {
                    llm_chat_remove_range(chat, j, j + 1);
                    removed = 1;
                }
                break;
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

    /* Write server error to journal so it's visible in TUI and
     * preserved for post-mortem analysis. The journal entry uses
     * tool="server_error" with failed=true, which the TUI renders
     * with an "x" marker instead of "+". */
    {
        cJSON *err_params = cJSON_CreateObject();
        /* Build error message from actual server error if available */
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

        /* Capture context size for diagnostics */
        int total_chars = 0;
        for (int ci = 0; ci < chat->n_msgs; ci++)
            if (chat->msgs[ci].content)
                total_chars += (int)strlen(chat->msgs[ci].content);
        cJSON_AddNumberToObject(err_params, "context_chars", total_chars);
        cJSON_AddNumberToObject(err_params, "context_msgs", chat->n_msgs);

        /* Include the actual server error message if available. */
        const char *err_msg = ctx->provider ? ctx->provider->last_error : NULL;
        const char *err_req = ctx->provider ? ctx->provider->last_error_request : NULL;
        const char *err_resp = ctx->provider ? ctx->provider->last_error_response : NULL;

        if (err_msg) {
            cJSON_AddStringToObject(err_params, "server_message", err_msg);
        }

        /* Save raw request and response bodies to store/ for
         * post-mortem analysis. These contain the exact JSON that
         * caused the server error, including the offset information
         * the server reports in its error message. */
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

        /* Store full error params in store/ for audit trail */
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

    /* Check if this is an authentication error (HTTP 401/403).
     * Auth errors can't be fixed by scratchpad manipulation —
     * the provider already retried once with a fresh token.
     * If it still fails, the credentials are truly invalid. */
    {
        const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
        if (perr && (strstr(perr, "HTTP 401") || strstr(perr, "HTTP 403"))) {
            ev.message = "Authentication failed — token expired or invalid, "
                         "please re-authenticate (e.g. gcloud auth login)";
            react_emit(on_event, userdata, &ev);
            return 1;  /* break */
        }
    }

    /* BUG FIX: Detect HTTP 400 (client error = bad request).
     * Unlike HTTP 500 (transient server error), 400 means the request
     * itself is malformed — typically context too large. The old tier 1
     * retry (remove 2 messages + continue) would let the model respond
     * successfully, resetting consecutive_null_responses to 0, then the
     * next LLM call would fail again with 400 → infinite loop.
     * For 400 errors: do aggressive context eviction immediately. */
    {
        const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
        if (perr && strstr(perr, "HTTP 400")) {
            (*total_400)++;
            if (*total_400 >= 6) {
                ev.message = "HTTP 400 — context still too large after "
                             "repeated eviction, giving up";
                react_emit(on_event, userdata, &ev);
                return 1;  /* break */
            }
            /* FIX #3: Compute context_budget and pass to emergency eviction */
            float cpt = react_get_chars_per_token(ctx);
            int cb = (int)(ctx->provider->cfg.context_size * cpt);
            int n_evict = react_emergency_evict(chat, cb);
            if (n_evict > 0) {
                char emsg[128];
                snprintf(emsg, sizeof(emsg),
                    "HTTP 400 — evicted %d messages to reduce context "
                    "(attempt %d/6)", n_evict, *total_400);
                ev.message = emsg;
                react_emit(on_event, userdata, &ev);
            } else {
                /* FIX HIGH#6: No messages were evictable (all CRITICAL or
                 * too few remaining). Break instead of retrying — without
                 * eviction, subsequent attempts will produce the same 400. */
                ev.message = "HTTP 400 — no evictable messages remain, giving up";
                react_emit(on_event, userdata, &ev);
                return 1;  /* break */
            }
            /* HTTP 400 is a client error (context too large), not a
             * transient server error. Don't let it poison the HTTP 500
             * retry tier counter — otherwise N recoverable 400s would
             * exhaust the 500-recovery budget. */
            *consecutive_null = 0;
            return 0;  /* continue */
        }
    }

    /* FIX: Detect max-token exhaustion as a distinct error class.
     * When completion_tokens == max_tokens, the model hit the output
     * ceiling — this is deterministic, not transient. Typically happens
     * after aggressive compaction leaves too little context, causing
     * the model to generate a massive response trying to reconstruct
     * everything. Recovery: aggressive context eviction (same as 400). */
    if (stats->completion_tokens > 0 && ctx->provider &&
        stats->completion_tokens >= ctx->provider->cfg.max_tokens) {
        char mtmsg[256];
        snprintf(mtmsg, sizeof(mtmsg),
            "Max-token exhaustion (%d/%d tokens) — "
            "evicting context to recover",
            stats->completion_tokens, ctx->provider->cfg.max_tokens);
        ev.message = mtmsg;
        react_emit(on_event, userdata, &ev);

        /* FIX #3: Compute context_budget and pass to emergency eviction */
        {
            float cpt = react_get_chars_per_token(ctx);
            int cb = (int)(ctx->provider->cfg.context_size * cpt);
            react_emergency_evict(chat, cb);
        }
        /* Don't count as consecutive (recovery may work) */
        *consecutive_null = 0;
        return 0;  /* continue */
    }

    /* 5-tier retry strategy for HTTP 500 / NULL responses.
     * Tiers 0a/0b: Plain retry with backoff (transient server errors)
     * Tier 1: Remove last assistant+tool_result pair (model confusion)
     * Tier 2: Reformulate scratchpad (context pollution)
     * Tier 3: Strip scratchpad entirely (nuclear option)
     * Tier 4+: Give up
     *
     * FIX: Previously, the first failure immediately removed the last
     * exchange (destructive). For intermittent server errors (e.g.,
     * llama.cpp returning sporadic HTTP 500s), this wastes the previous
     * tool result and forces the agent to re-execute the same tool.
     * Now we do 2 plain retries with backoff first. */
    if (*consecutive_null >= 6) {
        ev.message = "LLM server error — all recovery tiers exhausted, giving up";
        react_emit(on_event, userdata, &ev);
        return 1;  /* break */
    }

    if (*consecutive_null <= 2) {
        /* Tier 0: Plain retry with backoff — no context modification.
         * Most HTTP 500s from local servers (llama.cpp) are transient.
         * Retrying without destroying context avoids wasting tool results
         * and forcing the agent to re-execute the same operations. */
        if (ctx->pause_requested) return 1;  /* honor TUI pause immediately */
        int backoff_ms = *consecutive_null * 2000; /* 2s, 4s */
        char rmsg[128];
        snprintf(rmsg, sizeof(rmsg),
            "LLM server error — plain retry %d/2 (backoff %dms)",
            *consecutive_null, backoff_ms);
        ev.message = rmsg;
        react_emit(on_event, userdata, &ev);
        /* Interruptible sleep: check pause_requested every 100ms
         * instead of blocking for the full backoff duration. */
        for (int ms = 0; ms < backoff_ms && !ctx->pause_requested; ms += 100)
            usleep(100000);
    } else if (*consecutive_null == 3) {
        /* Tier 1: Remove the last assistant+tool_result pair.
         * The model's previous output was likely malformed (e.g.,
         * "shell_execshell_exec"). Removing it gives the model a
         * clean slate to regenerate from the previous context. */
        ev.message = "LLM server error — removing last exchange and retrying (tier 1)";
        react_emit(on_event, userdata, &ev);

        /* Remove last 2 messages (assistant + tool_result) if they exist.
         * Fix #9: use llm_chat_remove_range instead of manual free. */
        if (chat->n_msgs >= 2) {
            int remove_from = chat->n_msgs - 2;
            if (remove_from < 3) remove_from = 3;
            if (remove_from < chat->n_msgs)
                llm_chat_remove_range(chat, remove_from, chat->n_msgs);
        }
    } else if (*consecutive_null == 4) {
        /* Tier 2: Reformulate scratchpad into plain prose.
         * Code blocks and JSON in the scratchpad can confuse
         * the model's JSON generation.
         * Fix #3: Use msg_type instead of content-prefix scanning. */
        int sp_idx = llm_chat_find_by_type(chat, LLM_MSG_SCRATCHPAD);

        if (sp_idx >= 0) {
            ev.message = "LLM server error — stripping code blocks from scratchpad (tier 2)";
            react_emit(on_event, userdata, &ev);

            /* P6: Local code-block stripping instead of LLM call.
             * The LLM server may be overloaded (common cause of 500s),
             * so making another LLM call during recovery adds load.
             * Strip ```...``` code blocks and inline `code` locally. */
            /* FIX #8: Use strstr instead of hardcoded sizeof offset.
             * The previous code assumed content starts with exactly
             * "[SCRATCHPAD]\n" (14 bytes). Now finds the first newline
             * dynamically, so format changes won't corrupt the pointer. */
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
                    /* Strip fenced code blocks: ```...``` */
                    if (si + 3 <= src_len && strncmp(src + si, "```", 3) == 0) {
                        /* Skip to closing ``` */
                        const char *end = strstr(src + si + 3, "```");
                        if (end) {
                            si = (size_t)(end - src) + 3;
                            /* Skip trailing newline */
                            if (si < src_len && src[si] == '\n') si++;
                        } else {
                            si += 3; /* no closing fence — skip opening */
                        }
                        continue;
                    }
                    /* Strip inline backtick code: `...` */
                    if (src[si] == '`') {
                        const char *end = strchr(src + si + 1, '`');
                        if (end && end - (src + si) < 200) {
                            /* Copy content without backticks */
                            si++;
                            while (src + si < end) {
                                cleaned[di++] = src[si++];
                            }
                            si++; /* skip closing backtick */
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
        /* Tier 3: Strip scratchpad entirely (nuclear option).
         * If reformulation didn't help, the scratchpad itself
         * may be the problem. Remove it completely.
         * Fix #3: Use msg_type instead of content-prefix scanning. */
        ev.message = "LLM server error — stripping scratchpad entirely (tier 3)";
        react_emit(on_event, userdata, &ev);
        llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    }
    return 0;  /* continue */
}

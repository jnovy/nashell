/* react_eviction.c — Progressive context eviction (Harness-1 §3.5).
 * Extracted from react.c to reduce file size.
 * Implements multi-pass importance-aware context rendering:
 *   Pass 1: Strip LOW importance messages
 *   Pass 2: Compress NORMAL messages to top-4 sentences (BM25)
 *   Pass 3: Recoverability-aware eviction with breadcrumb generation */

#include "react_internal.h"
#include "compress.h"

/* ── Progressive Context Eviction ──────────────────────── */

/* Check context usage and evict old messages if over threshold.
 * Includes importance-aware multi-pass eviction, pair-safe boundaries,
 * scratchpad re-injection, and breadcrumb generation (LCM-Lite). */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata) {

    if (!ctx->flags.enable_compaction || ctx->provider->cfg.context_size <= 0)
        return;

    float cpt_ev = react_get_chars_per_token(ctx);
    int context_budget = (int)(ctx->provider->cfg.context_size * cpt_ev);
    int eviction_pct = ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70;
    int keep_head = REACT_EVICT_KEEP_HEAD;
    int keep_tail = REACT_EVICT_KEEP_TAIL;

    /* Recalculate total chars */
    int total_chars = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            total_chars += (int)strlen(chat->msgs[i].content);
    int usage_pct = (int)(100.0 * total_chars / context_budget);

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* Capture pre-compaction state for journal logging */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;

    int did_evict = 0;

    /* ── Pass 1: Strip LOW importance messages (errors, stale hints, deduped)
     * These have the least value and may actively degrade performance.
     * FIX #10: Remove in reverse order to avoid O(n²) memmove cascade. */
    for (int i = chat->n_msgs - keep_tail - 1; i >= keep_head; i--) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_LOW) {
            llm_chat_remove_range(chat, i, i + 1);
            did_evict = 1;
        }
    }
    if (did_evict) {
        total_chars = 0;
        for (int i = 0; i < chat->n_msgs; i++)
            if (chat->msgs[i].content)
                total_chars += (int)strlen(chat->msgs[i].content);
        usage_pct = (int)(100.0 * total_chars / context_budget);
    }

    /* ── Pass 2: Compress NORMAL messages to top-4 sentences.
     * Uses sentence-BM25 relevance scoring (Harness-1 §3.1).
     * Only compresses messages longer than 500 chars. */
    if (usage_pct > eviction_pct) {
        for (int i = keep_head; i < chat->n_msgs - keep_tail; i++) {
            if (chat->msgs[i].importance <= LLM_MSG_IMPORTANCE_NORMAL &&
                chat->msgs[i].content &&
                (int)strlen(chat->msgs[i].content) > 500) {
                char *compressed = compress_to_relevant(
                    chat->msgs[i].content, user_query, 4, 400);
                if (compressed) {
                    free(chat->msgs[i].content);
                    chat->msgs[i].content = compressed;
                    did_evict = 1;
                }
            }
        }
        if (did_evict) {
            total_chars = 0;
            for (int i = 0; i < chat->n_msgs; i++)
                if (chat->msgs[i].content)
                    total_chars += (int)strlen(chat->msgs[i].content);
            usage_pct = (int)(100.0 * total_chars / context_budget);
        }
    }

    /* ── Pass 3: Recoverability-aware eviction of NORMAL middle messages.
     * CWL [arXiv:2606.11213]: Sort evictable messages so those with
     * higher recoverability (content persisted elsewhere) are evicted
     * FIRST — they can be recovered via file_read.
     * LCM-Lite [arXiv:2605.04050]: Generate a breadcrumb index of
     * evicted store refs so the agent knows how to recover content. */
    if (usage_pct > eviction_pct) {
        int evict_start = keep_head;
        int evict_end = chat->n_msgs - keep_tail;

        /* FIX B5: Adjust eviction boundary to not split tool_call/tool_result pairs */
        for (int adj_iter = 0; evict_end > evict_start && evict_end < chat->n_msgs && adj_iter < 20; adj_iter++) {
            if (chat->msgs[evict_end].tool_call_id) {
                evict_end++;
                continue;
            }
            if (evict_end - 1 >= evict_start &&
                chat->msgs[evict_end - 1].tool_calls_json) {
                evict_end--;
                continue;
            }
            break;
        }

        /* FIX #1 + FIX #5: Score-based eviction WITHOUT physical reordering.
         * Previously sorted messages in-place, breaking tool_call/tool_result
         * pairing required by LLM APIs. Now: score → mark → breadcrumb → remove.
         * Also fixes compaction floor double-counting head chars in kept_chars. */
        {
            int n_evictable = evict_end - evict_start;
            if (n_evictable < 0) n_evictable = 0;
            /* Allocate eviction mark array (1 = evict, 0 = keep) */
            int *evict_mark = n_evictable > 0
                ? calloc((size_t)n_evictable, sizeof(int)) : NULL;
            int n_to_evict = 0;

            if (evict_mark && n_evictable > 0) {
                typedef struct { int idx; int score; } evict_scored_t;
                evict_scored_t *scored = malloc((size_t)n_evictable * sizeof(evict_scored_t));
                if (scored) {
                    for (int i = 0; i < n_evictable; i++) {
                        int mi = evict_start + i;
                        int imp = (int)chat->msgs[mi].importance;
                        int rec = (int)chat->msgs[mi].recoverability;
                        scored[i].idx = i;  /* index within evictable range */
                        scored[i].score = imp * 100 - rec * 10 + i;
                    }
                    /* Sort by score ascending (lowest = evict first) */
                    for (int i = 1; i < n_evictable; i++) {
                        evict_scored_t key = scored[i];
                        int j = i - 1;
                        while (j >= 0 && scored[j].score > key.score) {
                            scored[j + 1] = scored[j];
                            j--;
                        }
                        scored[j + 1] = key;
                    }

                    /* Compaction floor: keep at least 20% of non-head context.
                     * FIX #5: Only count tail chars in kept_chars (not head). */
                    int head_chars = 0;
                    for (int ki = 0; ki < evict_start; ki++)
                        if (chat->msgs[ki].content)
                            head_chars += (int)strlen(chat->msgs[ki].content);
                    int floor_chars = (context_budget - head_chars) / 5;
                    if (floor_chars < 4000) floor_chars = 4000;

                    int tail_chars = 0;
                    for (int ki = evict_end; ki < chat->n_msgs; ki++)
                        if (chat->msgs[ki].content)
                            tail_chars += (int)strlen(chat->msgs[ki].content);

                    /* Total chars in evictable range */
                    int evictable_chars = 0;
                    for (int ki = evict_start; ki < evict_end; ki++)
                        if (chat->msgs[ki].content)
                            evictable_chars += (int)strlen(chat->msgs[ki].content);

                    /* Mark messages for eviction in score order, stopping
                     * when removing more would drop below compaction floor. */
                    int remaining_chars = tail_chars + evictable_chars;
                    for (int si = 0; si < n_evictable; si++) {
                        int ri = scored[si].idx;  /* index in evictable range */
                        int mi = evict_start + ri;
                        int msg_chars = chat->msgs[mi].content
                            ? (int)strlen(chat->msgs[mi].content) : 0;

                        /* Don't evict CRITICAL messages */
                        if (chat->msgs[mi].importance == LLM_MSG_IMPORTANCE_CRITICAL)
                            continue;

                        /* Compaction floor check */
                        if (remaining_chars - msg_chars < floor_chars)
                            break;

                        /* Don't split tool_call/tool_result pairs:
                         * if this is an assistant with tool_calls, also mark next;
                         * if this is a tool result, also mark the preceding assistant. */
                        evict_mark[ri] = 1;
                        remaining_chars -= msg_chars;
                        n_to_evict++;
                    }
                    free(scored);
                }
            }

            if (n_to_evict > 0 && evict_mark) {
                /* LCM-Lite [arXiv:2605.04050]: Build breadcrumb index of
                 * evicted messages that have store refs — these can be
                 * recovered via file_read. Only process MARKED messages. */
                str_t breadcrumb = str_new(512);
                {
                    size_t sp_budget = (size_t)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 100);
                    str_t summary = str_new(sp_budget > 4096 ? 4096 : sp_budget);
                    int max_per_msg = n_to_evict > 0
                        ? (int)(sp_budget / (unsigned)n_to_evict) : 200;
                    if (max_per_msg < 200) max_per_msg = 200;
                    if (max_per_msg > 2000) max_per_msg = 2000;

                    str_append_cstr(&breadcrumb,
                        "[EVICTED CONTEXT — recoverable via file_read]\n");
                    int n_breadcrumbs = 0;

                    for (int ei = 0; ei < n_evictable; ei++) {
                        if (!evict_mark[ei]) continue;
                        int mi = evict_start + ei;
                        const char *content = chat->msgs[mi].content;
                        const char *role = chat->msgs[mi].role;
                        if (!content || !content[0] || !role) continue;
                        if (strcmp(role, "system") == 0) continue;

                        if (chat->msgs[mi].store_alias) {
                            char brief[81];
                            int blen = (int)strlen(content);
                            if (blen > 80) blen = 80;
                            memcpy(brief, content, (size_t)blen);
                            brief[blen] = '\0';
                            for (int b = 0; brief[b]; b++)
                                if (brief[b] == '\n' || brief[b] == '\r')
                                    brief[b] = ' ';
                            str_appendf(&breadcrumb, "- %s: %s (%s, %d chars)\n",
                                chat->msgs[mi].store_alias, brief, role,
                                (int)strlen(content));
                            n_breadcrumbs++;
                            continue;
                        }

                        if (strcmp(role, "tool") == 0 && strlen(content) < 50) continue;
                        int clen = (int)strlen(content);
                        if (clen > max_per_msg) clen = max_per_msg;
                        str_appendf(&summary, "[%s]: ", role);
                        str_append(&summary, content, (size_t)clen);
                        if ((int)strlen(content) > max_per_msg)
                            str_append_cstr(&summary, "...[truncated]");
                        str_append_cstr(&summary, "\n");
                        if (summary.len >= sp_budget) break;
                    }

                    if (n_breadcrumbs == 0) {
                        str_free(&breadcrumb);
                        breadcrumb = str_new(0);
                    }

                    if (summary.len > 0) {
                        char *summ_str = str_steal(&summary);
                        scratchpad_write(&ctx->tools->scratch, "evicted_context",
                                         summ_str, 2);
                        scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
                        free(summ_str);
                    } else {
                        str_free(&summary);
                    }
                }

                /* FIX #1: Remove marked messages in REVERSE order to preserve
                 * indices.  This avoids the O(n²) of individual removes AND
                 * preserves original message ordering (no sort-reorder). */
                for (int ri = n_evictable - 1; ri >= 0; ri--) {
                    if (evict_mark[ri])
                        llm_chat_remove_range(chat, evict_start + ri, evict_start + ri + 1);
                }

                /* Recover tool_call threading from surviving messages */
                free(chat->last_tool_call_id);
                chat->last_tool_call_id = NULL;
                free(chat->last_tool_calls_json);
                chat->last_tool_calls_json = NULL;
                for (int ri = chat->n_msgs - 1; ri >= 0; ri--) {
                    if (chat->msgs[ri].tool_calls_json) {
                        chat->last_tool_calls_json = strdup(chat->msgs[ri].tool_calls_json);
                        if (ri + 1 < chat->n_msgs && chat->msgs[ri + 1].tool_call_id)
                            chat->last_tool_call_id = strdup(chat->msgs[ri + 1].tool_call_id);
                        break;
                    }
                }

                /* Re-inject scratchpad at earliest eviction point */
                {
                    size_t sp_max = (ctx->provider->cfg.context_size > 0)
                        ? (size_t)(ctx->provider->cfg.context_size * react_get_chars_per_token(ctx) * 15 / 100) : 8192;
                    char *fresh_sp = scratchpad_serialize_budget(
                        &ctx->tools->scratch, sp_max);
                    if (fresh_sp && fresh_sp[0]) {
                        size_t slen = strlen(fresh_sp);
                        char *sp_msg = malloc(slen + 32);
                        if (sp_msg) {
                            snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", fresh_sp);
                            llm_chat_insert_typed(chat, evict_start,
                                "user", sp_msg, LLM_MSG_SCRATCHPAD);
                            free(sp_msg);
                        }
                    }
                    free(fresh_sp);
                }

                if (breadcrumb.len > 0) {
                    char *bc_str = str_steal(&breadcrumb);
                    llm_chat_insert_typed(chat, evict_start + 1,
                        "user", bc_str, LLM_MSG_EVICTION_SUMMARY);
                    free(bc_str);
                } else {
                    str_free(&breadcrumb);
                }

                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_WARNING;
                ev.step = step + 1;
                ev.message = "Context compacted (CWL+LCM) — recoverable refs indexed, non-recoverable summarized";
                react_emit(on_event, userdata, &ev);
            }
            free(evict_mark);
        } /* end evict_mark block */
    }

    /* Log compaction event to journal if anything changed */
    if (did_evict && ctx->tools->journal) {
        int after_msgs = chat->n_msgs;
        int after_chars = 0;
        for (int i = 0; i < chat->n_msgs; i++)
            if (chat->msgs[i].content)
                after_chars += (int)strlen(chat->msgs[i].content);
        int after_pct = (int)(100.0 * after_chars / context_budget);

        cJSON *params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "before_msgs", before_msgs);
        cJSON_AddNumberToObject(params, "after_msgs", after_msgs);
        cJSON_AddNumberToObject(params, "before_pct", before_pct);
        cJSON_AddNumberToObject(params, "after_pct", after_pct);
        journal_append(ctx->tools->journal, ctx->tools->react_loop,
                       step, "compaction", params, NULL, 0, 0, NULL, NULL);
        cJSON_Delete(params);
    }
}

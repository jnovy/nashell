/* react_eviction.c — Progressive context eviction (Harness-1 §3.5).
 * Extracted from react.c to reduce file size.
 * Implements multi-pass importance-aware context rendering:
 *   Pass 1: Strip LOW importance messages
 *   Pass 2: Compress NORMAL messages to top-4 sentences (BM25)
 *   Pass 3: Recoverability-aware eviction with breadcrumb generation
 *
 * Bug/Design fixes applied:
 *   BUG 1:  Pair-eviction now checks floor for both primary and partner
 *   BUG 2:  Pass 3 skips HIGH+ (not just CRITICAL) — aligns with emergency
 *   BUG 3:  Stale EVICTION_SUMMARY/SCRATCHPAD/MEMORY_HINT cleanup runs
 *           unconditionally when eviction triggers, not only inside Pass 3
 *   BUG 7:  context_budget uses long instead of int
 *   BUG 9:  Explicit guard against n_evictable <= 0 (no calloc(0))
 *   DESIGN 1: Post-eviction budget verification loop
 *   DESIGN 2: Eviction targets a low-watermark (eviction_pct - 10)
 *   DESIGN 3: Pass 2 compresses largest messages first
 *   DESIGN 5: Pass 1 already uses reverse removal (was previously fixed)
 *   DESIGN 6: Score formula normalizes position to not overwhelm size_bonus
 *   DESIGN 7: Breadcrumb string capped at 4096 chars
 *   DESIGN 10: Low-watermark provides hysteresis (trigger@pct, target@pct-10)
 */

#include "react_internal.h"
#include "compress.h"

/* ── Progressive Context Eviction ──────────────────────── */

/* Helper: recalculate total chars in chat. */
static long react_calc_total_chars(const llm_chat_t *chat) {
    long total = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            total += (long)strlen(chat->msgs[i].content);
    return total;
}

/* Helper: re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive eviction (BUG A FIX) and emergency eviction
 * (BUG C FIX) to ensure scratchpad is always restored after any eviction. */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos) {
    size_t sp_max = (ctx->provider->cfg.context_size > 0)
        ? (size_t)(ctx->provider->cfg.context_size
                   * react_get_chars_per_token(ctx) * 15 / 100)
        : 8192;
    char *fresh_sp = scratchpad_serialize_budget(&ctx->tools->scratch, sp_max);
    long injected_chars = 0;
    if (fresh_sp && fresh_sp[0]) {
        size_t slen = strlen(fresh_sp);
        char *sp_msg = malloc(slen + 32);
        if (sp_msg) {
            snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", fresh_sp);
            llm_chat_insert_typed(chat, insert_pos,
                "user", sp_msg, LLM_MSG_SCRATCHPAD);
            injected_chars = (long)strlen(sp_msg);
            free(sp_msg);
        }
    }
    free(fresh_sp);
    return injected_chars;
}

/* Check context usage and evict old messages if over threshold.
 * Includes importance-aware multi-pass eviction, pair-safe boundaries,
 * scratchpad re-injection, and breadcrumb generation (LCM-Lite). */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata) {

    if (!ctx->flags.enable_compaction || ctx->provider->cfg.context_size <= 0)
        return;

    float cpt_ev = react_get_chars_per_token(ctx);
    /* BUG 7 FIX: Use long for context_budget to avoid overflow for large contexts. */
    long context_budget = (long)(ctx->provider->cfg.context_size * cpt_ev);
    int eviction_pct = ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70;
    /* DESIGN 2+10 FIX: Target a lower watermark to provide hysteresis.
     * Trigger at eviction_pct, but evict down to target_pct.
     * This prevents re-injection of scratchpad/breadcrumb from immediately
     * pushing back over the trigger threshold, causing thrashing. */
    int target_pct = eviction_pct - 10;
    if (target_pct < 30) target_pct = 30;
    int keep_head = REACT_EVICT_KEEP_HEAD;
    int keep_tail = REACT_EVICT_KEEP_TAIL;

    /* Recalculate total chars */
    long total_chars = react_calc_total_chars(chat);
    int usage_pct = (context_budget > 0)
        ? (int)(100L * total_chars / context_budget) : 0;

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* Capture pre-compaction state for journal logging */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;

    int did_evict = 0;

    /* BUG 3 FIX: Always clean stale injected messages when eviction triggers,
     * not only when Pass 3 fires. Previously, if Pass 1+2 were sufficient,
     * CRITICAL-importance eviction summaries from prior cycles persisted. */
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    llm_chat_remove_by_type(chat, LLM_MSG_EVICTION_SUMMARY);
    llm_chat_remove_by_type(chat, LLM_MSG_MEMORY_HINT);
    /* Recalculate after cleanup */
    total_chars = react_calc_total_chars(chat);
    usage_pct = (context_budget > 0)
        ? (int)(100L * total_chars / context_budget) : 0;

    /* BUG A FIX: If cleanup alone brought us under target, re-inject the
     * scratchpad and return early. Previously, the scratchpad was removed
     * unconditionally above but only re-injected inside Pass 3's conditional
     * block — if Pass 1+2 (or even cleanup alone) were sufficient, the
     * scratchpad was silently lost from the LLM context. */
    if (usage_pct <= target_pct) {
        react_reinject_scratchpad(ctx, chat, keep_head);
        did_evict = 1; /* cleanup removed messages */
        goto post_eviction;
    }

    /* ── Pass 1: Strip LOW importance messages (errors, stale hints, deduped)
     * These have the least value and may actively degrade performance.
     * Remove in reverse order to avoid index shifting issues. */
    for (int i = chat->n_msgs - keep_tail - 1; i >= keep_head; i--) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_LOW) {
            llm_chat_remove_range(chat, i, i + 1);
            did_evict = 1;
        }
    }
    if (did_evict) {
        total_chars = react_calc_total_chars(chat);
        usage_pct = (context_budget > 0)
            ? (int)(100L * total_chars / context_budget) : 0;
        /* BUG A FIX (cont.): If Pass 1 brought us under target, re-inject
         * scratchpad and skip remaining passes. */
        if (usage_pct <= target_pct) {
            react_reinject_scratchpad(ctx, chat, keep_head);
            goto post_eviction;
        }
    }

    /* ── Pass 2: Compress NORMAL messages to top-4 sentences.
     * Uses sentence-BM25 relevance scoring (Harness-1 §3.1).
     * Only compresses messages longer than 500 chars.
     * DESIGN 3 FIX: Sort candidates by length (largest first) for biggest
     * space savings. Previously compressed oldest first (least relevant).
     * Uses target_pct (low watermark) for hysteresis. */
    if (usage_pct > target_pct) {
        /* Collect compressible message indices and sort by length descending */
        int n_candidates = 0;
        typedef struct { int idx; int len; } compress_cand_t;
        compress_cand_t *candidates = NULL;
        int cand_cap = 0;

        for (int i = keep_head; i < chat->n_msgs - keep_tail; i++) {
            if (chat->msgs[i].importance <= LLM_MSG_IMPORTANCE_NORMAL &&
                chat->msgs[i].content &&
                (int)strlen(chat->msgs[i].content) > 500) {
                if (n_candidates >= cand_cap) {
                    cand_cap = cand_cap ? cand_cap * 2 : 16;
                    compress_cand_t *tmp = realloc(candidates,
                        (size_t)cand_cap * sizeof(compress_cand_t));
                    if (!tmp) break;
                    candidates = tmp;
                }
                candidates[n_candidates].idx = i;
                candidates[n_candidates].len = (int)strlen(chat->msgs[i].content);
                n_candidates++;
            }
        }
        /* Sort by length descending (largest first for most space savings) */
        for (int i = 1; i < n_candidates; i++) {
            compress_cand_t key = candidates[i];
            int j = i - 1;
            while (j >= 0 && candidates[j].len < key.len) {
                candidates[j + 1] = candidates[j];
                j--;
            }
            candidates[j + 1] = key;
        }

        for (int ci = 0; ci < n_candidates; ci++) {
            int i = candidates[ci].idx;
            char *compressed = compress_to_relevant(
                chat->msgs[i].content, user_query, 4, 400);
            if (compressed) {
                int old_len = (int)strlen(chat->msgs[i].content);
                int new_len = (int)strlen(compressed);
                free(chat->msgs[i].content);
                chat->msgs[i].content = compressed;
                total_chars -= (old_len - new_len);
                did_evict = 1;
                /* Stop compressing once we're under target watermark */
                usage_pct = (context_budget > 0)
                    ? (int)(100L * total_chars / context_budget) : 0;
                if (usage_pct <= target_pct)
                    break;
            }
        }
        free(candidates);
    }

    /* ── Pass 3: Recoverability-aware eviction of NORMAL middle messages.
     * CWL [arXiv:2606.11213]: Sort evictable messages so those with
     * higher recoverability (content persisted elsewhere) are evicted
     * FIRST — they can be recovered via file_read.
     * LCM-Lite [arXiv:2605.04050]: Generate a breadcrumb index of
     * evicted store refs so the agent knows how to recover content.
     * Uses target_pct (low watermark) for hysteresis. */
    if (usage_pct > target_pct) {
        int evict_start = keep_head;
        int evict_end = chat->n_msgs - keep_tail;

        /* FLAW 3 FIX: Adjust eviction boundary to not split tool_call/result
         * pairs. Now tries to ADVANCE past a complete pair first (including
         * it in the eviction pool) before retreating. Previously only
         * decremented evict_end, which could shrink the pool significantly
         * when the tail boundary fell in a chain of tool_call/result pairs. */
        for (int adj_iter = 0; evict_end > evict_start && adj_iter < 20; adj_iter++) {
            if (evict_end < chat->n_msgs && chat->msgs[evict_end].tool_call_id) {
                /* We're at a tool_result — its paired tool_call is at evict_end-1.
                 * Try advancing past this result to include the complete pair. */
                if (evict_end + 1 <= chat->n_msgs - keep_tail) {
                    evict_end++; /* include the tool_result in eviction pool */
                } else {
                    evict_end--; /* can't advance — retreat past the tool_call too */
                }
                continue;
            }
            if (evict_end - 1 >= evict_start &&
                chat->msgs[evict_end - 1].tool_calls_json) {
                /* The last evictable msg is a tool_call with no result in pool.
                 * Try advancing to include the result. */
                if (evict_end + 1 <= chat->n_msgs - keep_tail) {
                    evict_end++; /* include the tool_result */
                } else {
                    evict_end--; /* can't advance — exclude the orphan tool_call */
                }
                continue;
            }
            break;
        }

        int n_evictable = evict_end - evict_start;
        /* BUG 9 FIX: Explicit guard — skip scoring when range is empty.
         * Previously calloc(0, ...) had implementation-defined behavior. */
        if (n_evictable > 0) {
            int *evict_mark = calloc((size_t)n_evictable, sizeof(int));
            int n_to_evict = 0;

            if (evict_mark) {
                typedef struct { int idx; int score; } evict_scored_t;
                evict_scored_t *scored = malloc((size_t)n_evictable * sizeof(evict_scored_t));
                if (scored) {
                    for (int i = 0; i < n_evictable; i++) {
                        int mi = evict_start + i;
                        int imp = (int)chat->msgs[mi].importance;
                        int rec = (int)chat->msgs[mi].recoverability;
                        int msg_len = chat->msgs[mi].content
                            ? (int)strlen(chat->msgs[mi].content) : 0;
                        scored[i].idx = i;
                        /* FLAW 1 FIX: Normalize position to 0-19 range so
                         * recoverability (rec*10, range 0-40) is the dominant
                         * eviction factor below importance tier, and position
                         * serves only as a tiebreaker. Previously 0-99 range
                         * caused position to overwhelm recoverability, evicting
                         * non-recoverable messages before recoverable ones. */
                        int pos_norm = (n_evictable > 1)
                            ? (i * 19 / (n_evictable - 1)) : 0;
                        int size_bonus = (rec > 0 && msg_len > 200)
                            ? (msg_len / 500) * rec : 0;
                        scored[i].score = imp * 100 - rec * 10 - size_bonus + pos_norm;
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

                    /* Compaction floor: keep at least 20% of non-head context. */
                    int head_chars = 0;
                    for (int ki = 0; ki < evict_start; ki++)
                        if (chat->msgs[ki].content)
                            head_chars += (int)strlen(chat->msgs[ki].content);
                    int floor_chars = (int)((context_budget - head_chars) / 5);
                    if (floor_chars < 4000) floor_chars = 4000;

                    /* FLAW 2 FIX: Account for re-injection budget in floor.
                     * After eviction, scratchpad (~15% of context), breadcrumb
                     * (up to 4096 chars), and memory hint (~200 chars) are
                     * injected. Without accounting for these, post-eviction
                     * usage can exceed the trigger threshold, causing the
                     * DESIGN 1 WARNING to fire. Add estimated re-injection
                     * size to the floor so eviction leaves room. */
                    {
                        long reinject_est = (long)(context_budget
                            * REACT_SCRATCHPAD_BUDGET_PCT / 100)
                            + 4096   /* breadcrumb cap */
                            + 200;   /* memory hint */
                        floor_chars += (int)reinject_est;
                    }

                    int tail_chars = 0;
                    for (int ki = evict_end; ki < chat->n_msgs; ki++)
                        if (chat->msgs[ki].content)
                            tail_chars += (int)strlen(chat->msgs[ki].content);

                    int evictable_chars = 0;
                    for (int ki = evict_start; ki < evict_end; ki++)
                        if (chat->msgs[ki].content)
                            evictable_chars += (int)strlen(chat->msgs[ki].content);

                    int remaining_chars = tail_chars + evictable_chars;
                    for (int si = 0; si < n_evictable; si++) {
                        int ri = scored[si].idx;
                        int mi = evict_start + ri;
                        int msg_chars = chat->msgs[mi].content
                            ? (int)strlen(chat->msgs[mi].content) : 0;

                        if (evict_mark[ri]) continue;

                        /* BUG 2 FIX: Skip HIGH and CRITICAL messages.
                         * Previously only skipped CRITICAL, making progressive
                         * eviction MORE aggressive than emergency eviction for
                         * HIGH-importance messages (skills, lessons, etc.). */
                        if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                            continue;

                        /* Compaction floor check */
                        if (remaining_chars - msg_chars < floor_chars)
                            break;

                        /* BUG 1 FIX: Pair-safety with floor check for BOTH halves.
                         * Previously, the pair partner was marked without checking
                         * if its chars would breach the floor. Now we calculate
                         * the total pair cost and check floor for the whole pair. */
                        int pair_ri = -1;
                        int pair_chars = 0;

                        /* If assistant with tool_calls, pair with next tool_result */
                        if (chat->msgs[mi].tool_calls_json &&
                            ri + 1 < n_evictable && !evict_mark[ri + 1]) {
                            int pair_mi = evict_start + ri + 1;
                            if (pair_mi < evict_end &&
                                chat->msgs[pair_mi].tool_call_id) {
                                /* Don't evict pair if partner is HIGH+ */
                                if (chat->msgs[pair_mi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                                    continue;
                                pair_ri = ri + 1;
                                pair_chars = chat->msgs[pair_mi].content
                                    ? (int)strlen(chat->msgs[pair_mi].content) : 0;
                            }
                        }
                        /* If tool_result, pair with preceding assistant */
                        if (chat->msgs[mi].tool_call_id &&
                            ri - 1 >= 0 && !evict_mark[ri - 1]) {
                            int pair_mi = evict_start + ri - 1;
                            if (pair_mi >= evict_start &&
                                chat->msgs[pair_mi].tool_calls_json) {
                                if (chat->msgs[pair_mi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                                    continue;
                                pair_ri = ri - 1;
                                pair_chars = chat->msgs[pair_mi].content
                                    ? (int)strlen(chat->msgs[pair_mi].content) : 0;
                            }
                        }

                        /* BUG 1 FIX: Check floor for total pair cost */
                        int total_pair_chars = msg_chars + pair_chars;
                        if (remaining_chars - total_pair_chars < floor_chars)
                            break;

                        /* Mark primary for eviction */
                        evict_mark[ri] = 1;
                        remaining_chars -= msg_chars;
                        n_to_evict++;

                        /* Mark pair partner if found */
                        if (pair_ri >= 0) {
                            evict_mark[pair_ri] = 1;
                            remaining_chars -= pair_chars;
                            n_to_evict++;
                        }
                    }
                    if (n_to_evict > 0) did_evict = 1;
                    free(scored);
                }
            }

            if (n_to_evict > 0 && evict_mark) {
                /* LCM-Lite: Build breadcrumb index of evicted store refs. */
                str_t breadcrumb = str_new(512);
                {
                    size_t sp_budget = (size_t)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 100);
                    size_t summary_budget = sp_budget / 3;
                    if (summary_budget > 4096) summary_budget = 4096;
                    str_t summary = str_new(summary_budget > 4096 ? 4096 : summary_budget);
                    int max_per_msg = n_to_evict > 0
                        ? (int)(summary_budget / (unsigned)n_to_evict) : 200;
                    if (max_per_msg < 200) max_per_msg = 200;
                    if (max_per_msg > 1000) max_per_msg = 1000;

                    str_append_cstr(&breadcrumb,
                        "[EVICTED CONTEXT — recoverable via file_read]\n");
                    int n_breadcrumbs = 0;
                    /* DESIGN 7 FIX: Cap breadcrumb at 4096 chars to prevent
                     * unbounded growth in long sessions with many store refs. */
                    int breadcrumb_cap = 4096;

                    for (int ei = 0; ei < n_evictable; ei++) {
                        if (!evict_mark[ei]) continue;
                        int mi = evict_start + ei;
                        const char *content = chat->msgs[mi].content;
                        const char *role = chat->msgs[mi].role;
                        if (!content || !content[0] || !role) continue;
                        if (strcmp(role, "system") == 0) continue;

                        if (chat->msgs[mi].store_alias) {
                            /* DESIGN 7 FIX: Stop adding breadcrumbs once capped */
                            if ((int)breadcrumb.len >= breadcrumb_cap) continue;

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
                        if (summary.len >= summary_budget) break;
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

                /* Remove marked messages in REVERSE order to preserve indices. */
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

                /* Recalculate evict_start since removals shifted indices */
                evict_start = keep_head;

                /* Re-inject scratchpad (uses shared helper — BUG A FIX) */
                react_reinject_scratchpad(ctx, chat, evict_start);

                int insert_pos = evict_start + 1;  /* after scratchpad */

                if (breadcrumb.len > 0) {
                    char *bc_str = str_steal(&breadcrumb);
                    llm_chat_insert_typed(chat, insert_pos,
                        "user", bc_str, LLM_MSG_EVICTION_SUMMARY);
                    free(bc_str);
                    insert_pos++;
                } else {
                    str_free(&breadcrumb);
                }

                /* v4 unified memory: post-compaction hint */
                llm_chat_insert_typed(chat, insert_pos,
                    "user",
                    "[Context compacted. Use memory_recall to recover lost "
                    "context — it searches both stored knowledge and past "
                    "session history.]",
                    LLM_MSG_MEMORY_HINT);

                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_WARNING;
                ev.step = step + 1;
                ev.message = "Context compacted (CWL+LCM) — recoverable refs indexed, non-recoverable summarized";
                react_emit(on_event, userdata, &ev);
            }
            free(evict_mark);
        } /* end n_evictable > 0 */
    }

post_eviction:
    /* DESIGN 1 FIX: Post-eviction budget verification.
     * After eviction + re-injection of scratchpad/breadcrumb/hint,
     * verify usage is actually under budget. If not, log a warning.
     * This prevents silent over-budget states. */
    if (did_evict) {
        total_chars = react_calc_total_chars(chat);
        usage_pct = (context_budget > 0)
            ? (int)(100L * total_chars / context_budget) : 0;
        if (usage_pct > eviction_pct) {
            nash_log("[eviction] WARNING: post-eviction usage %d%% still above "
                     "trigger %d%% after re-injection", usage_pct, eviction_pct);
        }
    }

    /* Log compaction event to journal if anything changed */
    if (did_evict && ctx->tools->journal) {
        int after_msgs = chat->n_msgs;
        long after_chars = react_calc_total_chars(chat);
        int after_pct = (context_budget > 0)
            ? (int)(100L * after_chars / context_budget) : 0;

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
